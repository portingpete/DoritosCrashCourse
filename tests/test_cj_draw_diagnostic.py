"""Independent original-code proof for the opt-in CJ draw diagnostic ABI."""
import hashlib
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CJDrawDiagnosticTests(unittest.TestCase):
    @unittest.skipUnless((ROOT / "build_native/game_image.bin").exists(), "private original image unavailable")
    def test_original_material_frame_and_opacity_oracles(self):
        raw = (ROOT / "build_native/game_image.bin").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), "e2be3b416bffca18e59c34cbc6d20bc6ff14490a0ec3b1c30e4a0594ce79c8d7")
        words = {
            # Type1 preserves the selected actual ShaderMaster in r30, and
            # the actual geometry interface in r16. Its three VT12 calls are
            # the only supported saved shader provenance routes.
            0x822D060C: 0x7C7E1B78, 0x822D0614: 0x7C902378,
            0x822D1500: 0x4E800421, 0x822D169C: 0x4E800421, 0x822D16E4: 0x4E800421,
            # Bone interface VT12, actual128B frame, saved64-bit r30 at-24,
            # then renderer112B frame and original indexed call return.
            0x82161F2C: 0x8239B648, 0x8239B650: 0x9421FF80,
            0x825D5AC0: 0xFBC1FFE8, 0x8239B700: 0x4E800421,
            0x8234D4B0: 0x9421FF90,
            # Ordinary BoneRender reads actual world opacity+200. Shader
            # VT616 multiplies it by the renderer's actual global opacity
            # and writes material diffuse-color alpha, not model scale.
            0x8239B1A4: 0xC3F800C8,
            0x82160F54: 0x822CFAE8, 0x82157DE4: 0x822CFAE8,
            0x822CFB60: 0x7C1F4C2E, 0x822CFB64: 0xEFA00072,
            0x822CFDB0: 0xD028000C,
            # BoneLoad records material loadnow+668; Type1's diffuse
            # name is typed-data+80 and actual TextureMaster stored+340.
            0x8239E9BC: 0x9B3F029C, 0x822CBAD4: 0x816A0050,
            0x822CBB74: 0x90780000, 0x822AF248: 0x816300A8,
            # The Type1 loader's actual texture-manager global, its shipped
            # DX group factory/vtable, pure type-index getter, and original
            # flags predicate after a genuine named-resource lookup misses.
            0x822CBB60: 0x809AAB78,
            0x8238B6D0: 0x38600048, 0x8238B6F0: 0x394B0C44,
            0x82160C78: 0x822AC9C0,
            0x822AC9C0: 0x3D6082A7, 0x822AC9C4: 0x806B4B08,
            0x8224E3F8: 0x814B0034, 0x8224E40C: 0x3BCBA938,
            0x8224E414: 0x550706B4, 0x8224E444: 0x7D0740F8,
            0x8224E448: 0x54EBF7FE,
        }
        for address, expected in words.items():
            self.assertEqual(struct.unpack_from(">I", raw, address - 0x82000000)[0], expected, f"original word {address:08X}")
        offset = 0x820F61C8 - 0x82000000 + 35 * 30
        self.assertEqual(raw[offset:offset + len(b"g_diffuseColor\0")], b"g_diffuseColor\0")

if __name__ == "__main__":
    unittest.main()
