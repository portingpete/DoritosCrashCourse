# Build the native port

## Requirements and original inputs

Use Windows x64 with Git, Python 3.11+ (including Tkinter), CMake 3.24+, Visual Studio 2022 Build Tools with ClangCL and the Windows SDK. Install the DirectX June 2010 runtime for `d3dx9_43.dll`. Blender is optional for the supplied CJ pack and required for custom model conversion.

Clone the repository and put your own extracted `default.xex` and `data.pak` in `game/`. Only this original revision is supported:

```text
default.xex  SHA-256  6c4fa5a12ef2ad51db4809d5d654b487300a1ace06c2cdf6cb14258fd273dfc1
data.pak     SHA-256  0819b4e9f90695b70e083690e98ac6f76f0df0cf2bbb817f196316c930a54fc3
```

The executable must retain its normal Xbox encryption. Unencrypted, modified or unsupported inputs are rejected. The build decodes the image locally; do not replace your original with a decrypted executable.

## First build

```powershell
powershell -ExecutionPolicy Bypass -File tools/build.ps1
```

The script verifies inputs, prepares CJ from the bundled replacement pack and your original character declarations, fetches the pinned XenonRecomp generator, applies the checked patches, generates C++, prepares native shaders and builds the executable. Proprietary intermediates stay beneath ignored `build_native/` storage. The first compilation can take considerable time and disk space.

### Capture the missing character shader once

The skin shader variant used by CJ is absent from the game's stock shader cache. A fresh checkout needs a genuine source-loader capture from your own game before the final shader build. The project does not redistribute this original shader data.

After the initial executable is built, run it with source capture enabled:

```powershell
$env:DORITOS_SHADER_SOURCE_CAPTURE = '1'
.\Launch.cmd
```

Proceed to Single Player so the original character loader requests the variant. The first run may stop at the missing native shader after writing the capture. Look for `capture.json` beneath `build_native/source_shader_capture/<run>/`. It must describe `multilayer/vs` and feature key `[8388614, 0, 75]`. Keep its sibling shader files in place.

Clear the capture flag, then rebuild with the actual capture path:

```powershell
Remove-Item Env:DORITOS_SHADER_SOURCE_CAPTURE
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -SourceShaderCapture 'build_native/source_shader_capture/<run>/capture.json'
```

Replace `<run>` with the directory produced on your machine. The compiler verifies the source variant and its original/native pairing, then prepares the append-only cache overlay. All stock shader variants remain intact. Later builds reuse the prepared private capture when it is present. This first-checkout bootstrap has not been reproduced on every supported Windows setup; build failures should be reported with the relevant tool error and without attaching original game data.

## Launch and incremental builds

```powershell
.\Launch.cmd
cmake --build build_native/native --config Release --parallel 6
ctest --test-dir build_native/native -C Release --output-on-failure
```

Use `tools/build.ps1` after changing generator or shader inputs. The shorter CMake command is suitable for native code edits after configuration. Output is `build_native/native/Release/DoritosNative.exe`. `Launch.cmd` supplies the prepared assets and selected character.

`Launch.cmd --check` validates mapping/initialization. `Launch.cmd --no-package-cache` uses the disk package-read path. The default retains the verified original package in an immutable 53.1 MiB memory buffer; its assets still pass through the original loaders.

For loader development only, use `tools/build.ps1 -LoaderOnly`. `-AllowIncomplete` permits diagnostic translation traps and does not certify a playable build. `-CharacterAssets` remains available for advanced locally prepared character manifests. Custom players should normally use the [character importer](CHARACTERS.md).

## Checks without original game files

The portable tool contracts can run without building the game:

```powershell
py -3 -m unittest discover -s tests -p test_character_importer.py
py -3 -m unittest discover -s tests -p test_cal3d_character.py
py -3 -m unittest discover -s tests -p test_prepare_native_character.py
```

Some additional contracts use private original inputs and skip when those inputs are absent. Native CTest checks require a configured Windows build and the verified original files. GitHub Actions runs the portable contracts and the public-source audit.
