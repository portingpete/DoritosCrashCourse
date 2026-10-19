from pathlib import Path
from copy import deepcopy
import hashlib
import struct
import sys
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cal3d_character import (Bone, CharacterError, Mesh, ROOT, Skeleton, Submesh, Vertex,
                            make_overlay, preflight_hardware, private_output, read_cmf, read_csf, write_cmf, write_csf)


def skeleton_bytes():
    # A tiny independent binary fixture, with signed zero in the rest pose.
    transform = struct.pack("<14f", -0.0, 0, 1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1)
    return b"CSF\0" + struct.pack("<iii", 1010, 1, 5) + b"root\0" + transform + struct.pack("<ii", -1, 0)


def mesh_bytes():
    result = b"CMF\0" + struct.pack("<9i", 1010, 0, 1, -1, 3, 1, 0, 0, 1)
    for position, uv in [((0, 0, 0), (0, 0)), ((1, 0, 0), (1, 0)), ((0, 1, 0), (0, 1))]:
        result += struct.pack("<6fii2fiif", *position, 0, 0, 1, -1, 0, *uv, 1, 0, 1)
    return result + struct.pack("<3i", 0, 1, 2)


def vertex(bone=0):
    return Vertex((0, 0, 0), (0, 0, 1), -1, 0, ((0, 0),), ((bone, 1.0),))


def referenced_submesh(count):
    # Every source index is referenced; geometry values do not affect mapping.
    faces = [tuple(range(index, index + 3)) for index in range(0, count - 2, 3)]
    if count % 3:
        faces.append((count - 2, count - 1, 0))
    return Submesh(-1, 1, [vertex()] * count, faces)


def conservative_partition_submesh():
    # Fill 46 bone IDs. The next triangle introduces the same new bone three
    # times: the original counts 49, despite the actual union containing 47.
    vertices = [vertex(index) for index in range(46)] + [vertex(46)] * 3
    faces = [tuple(range(index, index + 3)) for index in range(0, 45, 3)]
    faces += [(45, 0, 1), (46, 47, 48), (45, 46, 47)]
    return Submesh(-1, 1, vertices, faces)


class Cal3DCharacterTests(unittest.TestCase):
    def test_independent_binary_roundtrip_preserves_every_byte(self):
        original = skeleton_bytes()
        skeleton = read_csf(original)
        self.assertEqual(write_csf(skeleton), original)
        mesh = mesh_bytes()
        self.assertEqual(write_cmf(read_cmf(mesh, skeleton), skeleton), mesh)
        self.assertEqual(skeleton.bones[0].name, "root")
        self.assertEqual(read_cmf(mesh).submeshes[0].faces, [(0, 1, 2)])

    def test_rejects_truncation_trailing_bytes_and_unknown_layouts(self):
        for original, reader in [(skeleton_bytes(), read_csf), (mesh_bytes(), read_cmf)]:
            for size in range(len(original)):
                with self.assertRaises(CharacterError):
                    reader(original[:size])
            with self.assertRaises(CharacterError):
                reader(original + b"\0")
            unknown = bytearray(original)
            struct.pack_into("<i", unknown, 4, 1000)
            with self.assertRaises(CharacterError):
                reader(unknown)
        flags = bytearray(mesh_bytes())
        struct.pack_into("<i", flags, 8, 2)
        with self.assertRaises(CharacterError):
            read_cmf(flags)

    def test_rejects_unowned_hierarchy_and_invalid_quaternion(self):
        skeleton = read_csf(skeleton_bytes())
        skeleton.bones[0].parent = 0
        with self.assertRaises(CharacterError):
            write_csf(skeleton)
        skeleton = read_csf(skeleton_bytes())
        skeleton.bones[0].children = (0,)
        with self.assertRaises(CharacterError):
            write_csf(skeleton)
        skeleton = read_csf(skeleton_bytes())
        skeleton.bones[0].rotation = (0, 0, 0, 0)
        with self.assertRaises(CharacterError):
            write_csf(skeleton)

    def test_rejects_missing_bone_nonnormalized_weights_and_nonfinite_attributes(self):
        skeleton = read_csf(skeleton_bytes())
        for influences in [((1, 1.0),), ((0, 0.5),), ((0, float("nan")),), ((0, 0.5), (0, 0.5)), ()]:
            mesh = read_cmf(mesh_bytes())
            mesh.submeshes[0].vertices[0].influences = influences
            with self.assertRaises(CharacterError):
                write_cmf(mesh, skeleton)
        mesh = read_cmf(mesh_bytes())
        mesh.submeshes[0].vertices[0].position = (0, float("inf"), 0)
        with self.assertRaises(CharacterError):
            write_cmf(mesh)

    def test_rejects_face_outside_actual_vertex_buffer_and_lod(self):
        mesh = read_cmf(mesh_bytes())
        mesh.submeshes[0].faces = [(0, 1, 3)]
        with self.assertRaises(CharacterError):
            write_cmf(mesh)
        mesh = read_cmf(mesh_bytes())
        mesh.submeshes[0].vertices[0].collapse = 1
        with self.assertRaises(CharacterError):
            write_cmf(mesh)

    def test_hardware_partition_preserves_global_ids_and_repeated_new_bone_count(self):
        skeleton = Skeleton([Bone(f"bone{index}", (0, 0, 0), (0, 0, 0, 1),
                                  (0, 0, 0), (0, 0, 0, 1), -1, ()) for index in range(72)])
        mesh = Mesh([conservative_partition_submesh()])
        # Use the full original global skeleton namespace, including ID71.
        for item in mesh.submeshes[0].vertices[46:]:
            item.influences = ((71, 1.0),)
        before = deepcopy(mesh)
        report = preflight_hardware(mesh, skeleton)
        self.assertEqual(mesh, before)
        self.assertEqual(report["hardware_vertices"], 50)
        self.assertEqual(report["duplicated_vertices"], 1)
        self.assertEqual([(g["vertices"], g["triangles"], g["base_vertex"], g["start_index"])
                          for g in report["groups"]], [(46, 16, 0, 0), (4, 2, 46, 48)])
        self.assertEqual(report["groups"][0]["bone_ids"], list(range(46)))
        self.assertEqual(report["groups"][1]["bone_ids"], [71, 45])
        self.assertEqual(read_cmf(write_cmf(mesh, skeleton), skeleton), before)

    def test_hardware_allows_exact_48_bones_and_conservative_equality(self):
        mesh = Mesh([Submesh(-1, 1, [vertex(index) for index in range(48)],
                              [tuple(range(index, index + 3)) for index in range(0, 48, 3)])])
        report = preflight_hardware(mesh)
        self.assertEqual(len(report["groups"]), 1)
        self.assertEqual(report["groups"][0]["bone_ids"], list(range(48)))
        # 45 current bones + three repeated new influences is exactly 48;
        # using >= instead of > would incorrectly split this face.
        submesh = conservative_partition_submesh()
        submesh.faces = submesh.faces[:15] + [(46, 47, 48)]
        report = preflight_hardware(Mesh([submesh]))
        self.assertEqual(len(report["groups"]), 1)
        self.assertEqual(report["groups"][0]["bone_ids"], list(range(45)) + [46])

    def test_hardware_per_group_vertex_lookup_boundary(self):
        report = preflight_hardware(Mesh([referenced_submesh(50000)]))
        self.assertEqual(report["hardware_vertices"], 50000)
        self.assertEqual(len(report["groups"]), 1)
        with self.assertRaisesRegex(CharacterError, "group vertex lookup exceeds 50000"):
            preflight_hardware(Mesh([referenced_submesh(50001)]))

    def test_hardware_total_vertex_boundary_is_aggregate_and_includes_duplicates(self):
        report = preflight_hardware(Mesh([referenced_submesh(32000), referenced_submesh(32000)]))
        self.assertEqual(report["hardware_vertices"], 64000)
        with self.assertRaisesRegex(CharacterError, "staging exceeds 64000 vertices"):
            preflight_hardware(Mesh([referenced_submesh(32000), referenced_submesh(32001)]))
        # 63,999 distinct source vertices fit only with their one duplicate.
        report = preflight_hardware(Mesh([referenced_submesh(32000), referenced_submesh(31950),
                                          conservative_partition_submesh()]))
        self.assertEqual((report["source_vertices"], report["hardware_vertices"], report["duplicated_vertices"]),
                         (63999, 64000, 1))
        with self.assertRaisesRegex(CharacterError, "after partitioning"):
            preflight_hardware(Mesh([referenced_submesh(32000), referenced_submesh(31951),
                                     conservative_partition_submesh()]))

    def test_hardware_total_triangle_boundary_and_writer_gate(self):
        triangle = Submesh(-1, 1, [vertex()] * 3, [(0, 1, 2)] * 32000)
        report = preflight_hardware(Mesh([triangle, triangle]))
        self.assertEqual((report["hardware_vertices"], report["hardware_triangles"]), (6, 64000))
        excess = Submesh(-1, 1, [vertex()] * 3, [(0, 1, 2)] * 64001)
        with self.assertRaisesRegex(CharacterError, "index staging exceeds 64000 triangles"):
            write_cmf(Mesh([excess]))

    def test_unused_vertices_do_not_consume_hardware_staging(self):
        submesh = Submesh(-1, 1, [vertex()] * 65535, [(0, 1, 2)])
        report = preflight_hardware(Mesh([submesh]))
        self.assertEqual((report["source_vertices"], report["referenced_vertices"], report["hardware_vertices"],
                          report["duplicated_vertices"]), (65535, 3, 3, 0))

    def test_overlay_rejects_oversized_raw_cmf_before_emitting_assets(self):
        cmf = bytearray(mesh_bytes()[:-12])
        struct.pack_into("<i", cmf, 24, 64001)
        cmf += struct.pack("<3i", 0, 1, 2) * 64001
        # Generic format parsing remains available for inspection; activation
        # must independently check title-specific hardware capacity.
        self.assertEqual(len(read_cmf(bytes(cmf)).submeshes[0].faces), 64001)
        inputs = {Path("source.cfg"): b"skeleton=rig.csf\nmesh=source.cmf\nanimation=RUN:run.caf\n",
                  Path("rig.csf"): skeleton_bytes(), Path("source.cmf"): bytes(cmf)}
        with patch("cal3d_character.read", side_effect=lambda path: inputs[path]), \
             patch("cal3d_character.save") as emitted, patch("cal3d_character.private_output") as destination:
            with self.assertRaisesRegex(CharacterError, "index staging"):
                make_overlay(Path("source.cfg"), Path("rig.csf"), Path("source.cmf"),
                             [Path("unused.png")], Path("overlay"), "cj_native")
            emitted.assert_not_called()
            destination.assert_not_called()

    @unittest.skipUnless((ROOT / "build_native/character_recon/roundtrip1010/p_test.csf").is_file() and
                         (ROOT / "build_native/character_recon/roundtrip1010/dummymodel.cmf").is_file(),
                         "Private original character inputs are not present")
    def test_private_original_roundtrip_and_hardware_partition_golden(self):
        root = ROOT / "build_native/character_recon/roundtrip1010"
        csf, cmf = (root / "p_test.csf").read_bytes(), (root / "dummymodel.cmf").read_bytes()
        self.assertEqual(hashlib.sha256(csf).hexdigest(), "599839637025264e824f1d018d4d79c8e3ee104d7cfb70373a7c920e801a5f54")
        self.assertEqual(hashlib.sha256(cmf).hexdigest(), "e0dce81fb11bde7266272422cf114a98582fcb112c14a2277e3e7ec7169157a4")
        skeleton = read_csf(csf)
        mesh = read_cmf(cmf, skeleton)
        self.assertEqual(write_csf(skeleton), csf)
        self.assertEqual(write_cmf(mesh, skeleton), cmf)
        self.assertEqual(len(skeleton.bones), 72)
        report = preflight_hardware(mesh, skeleton)
        self.assertEqual((report["source_vertices"], report["hardware_vertices"], report["hardware_triangles"],
                          report["duplicated_vertices"]), (3491, 3633, 7392, 142))
        self.assertEqual([(g["submesh"], len(g["bone_ids"]), g["vertices"], g["triangles"])
                          for g in report["groups"]], [(0, 1, 78, 116), (1, 1, 127, 208), (2, 1, 127, 208),
                                                       (3, 1, 43, 52), (4, 1, 43, 52), (5, 46, 659, 1184),
                                                       (5, 40, 1735, 3214), (5, 24, 821, 2358)])
        self.assertEqual(max(bone for group in report["groups"] for bone in group["bone_ids"]), 70)

    def test_overlay_preserves_authored_callbacks_and_names_owned_material_and_texture(self):
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(b"\0\xff\x80\0\xff")) + chunk(b"IEND", b""))
        cfg = b"skeleton=rig.CSF\r\nanimation=RUN:run.CAF\r\nmesh=original.CMF\r\ncallback=jump\r\nmask=upper\r\n"
        inputs = {Path("source.cfg"): cfg, Path("rig.csf"): skeleton_bytes(),
                  Path("source.cmf"): mesh_bytes(), Path("diffuse.png"): png}
        outputs = {}
        with patch("cal3d_character.read", side_effect=lambda path: inputs[path]), \
             patch("cal3d_character.private_output", side_effect=lambda path: path), \
             patch("cal3d_character.save", side_effect=lambda path, data: outputs.setdefault(path, data)):
            report = make_overlay(Path("source.cfg"), Path("rig.csf"), Path("source.cmf"),
                                  [Path("diffuse.png")], Path("overlay"), "cj_native")
        root = Path("overlay/data/art/_avatar")
        self.assertEqual(outputs[root / "cj_native.cfg"], cfg.replace(b"mesh=original.CMF", b"mesh=cj_native.cmf"))
        self.assertEqual(outputs[root / "cj_native.cmf"], mesh_bytes())
        self.assertEqual(outputs[root / "cj_native_0.png"], png)
        self.assertEqual(outputs[root / "cj_native.mat.txt"], b"_native/cj_native_0\r\nendmats\r\n")
        self.assertEqual(report["materials"][0]["diffuse_filename"], "_avatar/cj_native_0.png")
        self.assertTrue(report["requires_native_material_binding"])
        self.assertEqual((report["hardware"]["hardware_vertices"], report["hardware"]["hardware_triangles"]), (3, 1))

    def test_derived_outputs_cannot_escape_ignored_build_directory(self):
        for path in [ROOT / "game/test.cmf", ROOT / "build_native/../game/test.cmf"]:
            with self.assertRaises(CharacterError):
                private_output(path)

    def test_invalid_diffuse_does_not_emit_partial_overlay(self):
        inputs = {Path("source.cfg"): b"skeleton=rig.csf\nmesh=source.cmf\nanimation=RUN:run.caf\n",
                  Path("rig.csf"): skeleton_bytes(), Path("source.cmf"): mesh_bytes(), Path("bad.png"): b"invalid"}
        with patch("cal3d_character.read", side_effect=lambda path: inputs[path]), \
             patch("cal3d_character.save") as emitted:
            with self.assertRaises(ValueError):
                make_overlay(Path("source.cfg"), Path("rig.csf"), Path("source.cmf"),
                             [Path("bad.png")], Path("overlay"), "cj_native")
            emitted.assert_not_called()


if __name__ == "__main__":
    unittest.main()
