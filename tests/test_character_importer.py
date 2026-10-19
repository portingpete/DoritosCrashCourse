"""Pack, selection, overlay and UI contracts; no game files needed for fixtures."""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tests")]
import character_packs as packs
import character_importer as importer
from cal3d_character import CharacterError, read_cmf, write_cmf
from test_prepare_native_character import cj_assets_fixture


def fixture(folder: Path, count=1, bone=71):
    manifest, metadata, files, cfg, csf = cj_assets_fixture(folder / "runtime")
    mesh = read_cmf(files["data/art/_avatar/cj_native.cmf"])
    mesh.submeshes = mesh.submeshes[:count] if count <= 4 else mesh.submeshes + [copy.deepcopy(mesh.submeshes[0])]
    if bone != 71:
        for submesh in mesh.submeshes:
            for vertex in submesh.vertices:
                vertex.influences = ((bone, 1.0),)
    binary = write_cmf(mesh)
    folder.mkdir(exist_ok=True)
    png = packs.solid_png((.2, .4, .6, 1))
    (folder / "model.cmf").write_bytes(binary)
    (folder / "diffuse-0.png").write_bytes(png)
    metadata = {"schema_version": 1, "id": "test-runner", "name": "Test runner",
                "skeleton_sha256": packs.SKELETON_SHA256,
                "mesh": {"file": "model.cmf", "sha256": packs.digest(binary)},
                "textures": [{"file": "diffuse-0.png", "sha256": packs.digest(png)}]}
    (folder / "character.json").write_text(json.dumps(metadata), encoding="utf-8")
    return folder, metadata, cfg, csf


class CharacterImporterTests(unittest.TestCase):
    def setUp(self):
        (ROOT / "build_native").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="importer-test-", dir=ROOT / "build_native")
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)

    def test_bundled_default_is_the_verified_cj_and_has_no_original_rig_or_cfg(self):
        pack = packs.resolve_pack("cj")
        self.assertEqual(pack.metadata["license"], "CC-BY-4.0")
        self.assertEqual(pack.mesh_bytes[:4], b"CMF\0")
        self.assertEqual(pack.metadata["mesh"]["sha256"], "167b31fd25fe065d11eab7a11f1ad0d8002161603f82ec7c367e87e1586ad936")
        self.assertEqual(len(pack.mesh.submeshes), 4)
        self.assertEqual(sum(len(s.vertices) for s in pack.mesh.submeshes), 1707)
        self.assertEqual(sum(len(s.faces) for s in pack.mesh.submeshes), 2304)
        self.assertFalse(list(pack.path.parent.glob("*.csf")))
        self.assertFalse(list(pack.path.parent.glob("*.cfg")))

    def test_one_to_four_submeshes_produce_the_fixed_runtime_files_without_copying_originals(self):
        for count in range(1, 5):
            with self.subTest(count=count):
                folder, _, cfg, csf = fixture(self.folder / f"pack{count}", count)
                pack = packs.validate_pack(folder)
                with patch.object(packs, "original_inputs", return_value={"data/art/_avatar/dummymodel.cfg": cfg,
                                  "data/art/_avatar/p_test.csf": csf}):
                    prepared = packs.prepare_pack(pack, Path("unused.pak"), self.folder / f"prepared{count}")
                actual = prepared.parent / "data/art/_avatar"
                self.assertEqual(len(list(actual.iterdir())), 7)
                self.assertEqual((actual / "cj_native.cmf").read_bytes(), pack.mesh_bytes)
                self.assertEqual((actual / "cj_native.cfg").read_bytes(), cfg.replace(b"mesh=stock.cmf", b"mesh=cj_native.cmf"))
                names = "\r\n".join(f"_native/cj_native_{i}" for i in range(count)) + "\r\nendmats\r\n"
                self.assertEqual((actual / "cj_native.mat.txt").read_bytes(), names.encode())
                self.assertFalse(list(prepared.parent.rglob("*.csf")))
                self.assertFalse(list(prepared.parent.rglob("*.caf")))

    def test_rejects_unsupported_geometry_and_unavailable_actual_bones(self):
        for count, bone in ((5, 71), (1, 72)):
            folder, *_ = fixture(self.folder / f"invalid{count}-{bone}", count, bone)
            with self.assertRaises(CharacterError):
                packs.validate_pack(folder)

    def test_manifest_paths_hashes_and_types_are_not_trusted(self):
        folder, data, *_ = fixture(self.folder / "pack")
        changes = [dict(schema_version=True), dict(id="con"), dict(id="../escape"), dict(skeleton_sha256="0" * 64),
                   dict(mesh={"file": "../model.cmf", "sha256": data["mesh"]["sha256"]}),
                   dict(mesh={"file": "model.cmf", "sha256": "0" * 64}),
                   dict(textures=[{"file": "../diffuse-0.png", "sha256": data["textures"][0]["sha256"]}])]
        for change in changes:
            (folder / "character.json").write_text(json.dumps({**data, **change}), encoding="utf-8")
            with self.subTest(change=change), self.assertRaises(ValueError):
                packs.validate_pack(folder)

    def test_png_decoder_rejects_corruption_and_valid_crc_but_oversized_pixel_stream(self):
        png = packs.solid_png((.3, .7, .1, 1))
        self.assertEqual(packs.checked_png(png)["width"], 1)
        with self.assertRaises(ValueError):
            packs.checked_png(png[:-3])
        position = png.index(b"IDAT") - 4
        end = position + 12 + struct.unpack_from(">I", png, position)[0]
        payload = zlib.compress(b"\0" * 100000)
        chunk = struct.pack(">I", len(payload)) + b"IDAT" + payload + struct.pack(">I", zlib.crc32(b"IDAT" + payload))
        with self.assertRaises(CharacterError):
            packs.checked_png(png[:position] + chunk + png[end:])
        for invalid in ((float("nan"), 0, 0, 1), (2, 0, 0, 1)):
            with self.assertRaises(CharacterError):
                packs.solid_png(invalid)

    def test_local_selection_is_atomic_and_changed_packs_require_reselection(self):
        folder, data, *_ = fixture(self.folder / "pack")
        state = self.folder / "settings/character.json"
        pack = packs.validate_pack(folder)
        packs.select_pack(pack, state)
        self.assertEqual(packs.selected_pack(state).fingerprint, pack.fingerprint)
        self.assertFalse(list(state.parent.glob("*.tmp")))
        (folder / "character.json").write_text(json.dumps({**data, "name": "Changed model"}), encoding="utf-8")
        with self.assertRaisesRegex(CharacterError, "changed"):
            packs.selected_pack(state)
        packs.select_pack(packs.resolve_pack("cj"), state)
        self.assertEqual(packs.selected_pack(state).metadata["id"], "cj")
        state.write_text(json.dumps({"schema_version": 1, "pack": "../outside/character.json"}))
        with self.assertRaisesRegex(CharacterError, "outside"):
            packs.selected_pack(state)

    def test_explicit_asset_override_passes_through_without_changing_selection_or_cwd(self):
        executable = self.folder / "fake.exe"; executable.write_bytes(b"fixture")
        before = Path.cwd()
        with patch.object(importer, "selected_pack", side_effect=AssertionError("must not select")), \
             patch.object(importer.subprocess, "run", return_value=SimpleNamespace(returncode=7)) as run:
            result = importer.launch(["--", "--asset-override-dir", "folder with spaces", "--check"], executable=executable)
        self.assertEqual(result, 7)
        self.assertEqual(run.call_args.args[0][-3:], ["--asset-override-dir", "folder with spaces", "--check"])
        self.assertNotIn("cwd", run.call_args.kwargs)
        self.assertEqual(Path.cwd(), before)

    def test_failed_conversion_does_not_activate_a_character(self):
        with patch.object(importer, "import_model", side_effect=CharacterError("conversion rejected")), \
             patch.object(importer, "select_pack") as select:
            with self.assertRaises(CharacterError):
                importer.main(["import-model", "broken.glb", "--activate"])
        select.assert_not_called()

    def test_overlay_copy_preserves_base_and_detects_modified_cache(self):
        folder, *_ = fixture(self.folder / "pack")
        pack = packs.validate_pack(folder)
        prepared = folder / "runtime/cj_native.assets.json"
        base = self.folder / "build_native/base"
        names = ("manifest.json", "character_manifest.json", "material_manifest.json", "texture_manifest.json",
                 "data/templates/guigroup.wbr", "data/templates/bonedmodel.wbr", "data/templates/shader.wbr", "data/templates/texture.wbr",
                 *importer.CJ_ASSET_NAMES)
        for name in names:
            target = base / name; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(b"base-" + name.encode())
        old = {name: (base / name).read_bytes() for name in names}
        def prepare_call(command, log):
            output = Path(command[command.index("--output") + 1])
            for name in importer.CJ_ASSET_NAMES:
                (output / name).write_bytes((prepared.parent / name).read_bytes())
        with patch.object(importer, "ROOT", self.folder), patch.object(importer, "prepare_pack", return_value=prepared), \
             patch.object(importer, "run", side_effect=prepare_call):
            overlay = importer.build_overlay(pack, base=base)
            self.assertEqual({name: (base / name).read_bytes() for name in names}, old)
            self.assertEqual(importer.build_overlay(pack, base=base), overlay)
            (overlay / "data/art/_avatar/cj_native.cmf").write_bytes(b"changed")
            with self.assertRaisesRegex(CharacterError, "cache changed"):
                importer.build_overlay(pack, base=base)


@unittest.skipUnless(os.name == "nt" or os.environ.get("DISPLAY"), "Tk widget check needs a display")
class ImporterUITests(unittest.TestCase):
    def test_controls_construct_with_cj_default_without_driving_os_input(self):
        import tkinter as tk
        from character_importer_ui import Importer
        root = tk.Tk(); root.withdraw()
        try:
            app = Importer(root)
            root.update_idletasks()
            self.assertTrue(app.auto_fit.get())
            self.assertTrue(app.activate.get())
            self.assertEqual(app.packs[0].metadata["id"], "cj")
            self.assertIn("default", app.listbox.get(0))
            self.assertEqual(str(app.import_button["state"]), "normal")
        finally:
            root.destroy()


if __name__ == "__main__":
    unittest.main()
