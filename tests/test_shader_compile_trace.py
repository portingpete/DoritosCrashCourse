"""Independent original-byte oracle for shader compilation and lookup ABI."""
import hashlib
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ShaderCompileTraceOracle(unittest.TestCase):
    @unittest.skipUnless((ROOT / "build_native/game_image.bin").exists(), "private original image unavailable")
    def test_original_compile_lookup_and_retry_contract(self):
        raw = (ROOT / "build_native/game_image.bin").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), "e2be3b416bffca18e59c34cbc6d20bc6ff14490a0ec3b1c30e4a0594ce79c8d7")
        words = {
            # Actual compile keeps master inr16 and key inr31, looks up its
            # inline136B map at7158 (return715C), then writesvariantkey+8.
            0x8238714C: 0x7C701B78, 0x82387150: 0x38630088,
            0x82387154: 0x7C9F2378, 0x82387158: 0x480011D1,
            0x82387170: 0x91430008,
            # Shader getter compiles only whenreadybyte+4 iszero.
            0x8238777C: 0x89630004,
            # The original descriptor owner is storedat+2536.
            0x82387184: 0x920309E8,
            # Lookup allocates2568B thenreturnsnode+24.
            0x82388454: 0x38600A08,
            0x823884E4: 0x387C0018,
            # Successful sourcecompile marksready+4=1, failedcreation clears
            # real VS+36 andPS+40 without settingready.
            0x82387724: 0x38600000,
            0x82387728: 0x923E0028,
            0x8238772C: 0x923E0024,
            0x8238773C: 0x39600001,
            0x82387748: 0x997E0004,
            # Genuine compiler thread-context accessor tailcallsKeTlsGetValue.
            0x8268B3F8: 0x4836BD24,
        }
        for address, expected in words.items():
            self.assertEqual(struct.unpack_from(">I", raw, address - 0x82000000)[0], expected, f"original word {address:08X}")


if __name__ == "__main__":
    unittest.main()
