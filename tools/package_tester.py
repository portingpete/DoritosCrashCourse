"""Stage a portable private tester build using an explicit file allowlist."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "0.1.0-test.20261016.2"


def digest(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def files_without_links(folder: Path):
    pending = [folder]
    while pending:
        current = pending.pop()
        for entry in os.scandir(current):
            metadata = entry.stat(follow_symlinks=False)
            if metadata.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                raise RuntimeError(f"Package input contains a reparse point: {entry.path}")
            if entry.is_dir(follow_symlinks=False):
                if entry.name not in (".git", "__pycache__", ".vs"):
                    pending.append(Path(entry.path))
            elif entry.is_file(follow_symlinks=False):
                yield Path(entry.path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--d3dx-package", type=Path, required=True)
    parser.add_argument("--include-game", action="store_true")
    args = parser.parse_args()
    if not args.include_game:
        parser.error("This private portable package includes game files; explicit --include-game is required")
    output = args.output.absolute()
    if output.exists():
        raise RuntimeError("Refusing to replace an existing package folder")
    output.mkdir(parents=True)

    def copy(source: Path, relative: str):
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)

    def write(relative: str, content: str):
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(content, encoding="utf-8", newline="\r\n")

    copy(ROOT / "build_native/native/Release/DoritosNative.exe", "bin/DoritosNative.exe")
    for name in ("default.xex", "data.pak"):
        copy(ROOT / "game" / name, "game/" + name)
    shader_root = ROOT / "build_native/renderer_inputs/data/art/dxshaders"
    for name in ("precache_win32_3.bsh", "precache_xenon.bsh"):
        copy(shader_root / name, "assets/renderer/" + name)
    header = (ROOT / "build_native/native/native_inputs/shader_inputs.h").read_text()
    import re
    for label in ("path", "sourcePath"):
        match = re.search(rf'{label}\[\] = L("[^\n]*");', header)
        if not match:
            raise RuntimeError("Cannot read pinned shader input path")
        configured = json.loads(match.group(1))
        if configured:
            source = Path(configured)
            copy(source, "assets/renderer/" + source.name)
    override = ROOT / "build_native/offline_menu_override/data"
    for source in files_without_links(override):
        copy(source, "assets/override/data/" + source.relative_to(override).as_posix())
    with zipfile.ZipFile(args.d3dx_package) as package:
        for name in ("D3DX9_43.dll", "D3DCompiler_43.dll"):
            destination = output / "bin" / name
            destination.write_bytes(package.read("build/native/release/bin/x64/" + name))
        (output / "licenses").mkdir()
        (output / "licenses/Microsoft-DXSDK-D3DX-LICENSE.txt").write_bytes(package.read("LICENSE.txt"))
        (output / "licenses/Microsoft-DXSDK-D3DX-NOTICE.md").write_bytes(package.read("NOTICE.md"))
    copy(ROOT / "COPYING", "licenses/Port-GPL-3.0.txt")
    copy(ROOT / "assets/input-prompts/LICENSE.txt", "licenses/Kenney-CC0.txt")
    for source in files_without_links(ROOT / "tools/tester_diagnostics"):
        copy(source, "diagnostics/" + source.name)

    # Include owned port sources, build configuration and generator patches;
    # generated title code, private assets, credentials and development logs
    # are deliberately outside this source archive's allowlist.
    with zipfile.ZipFile(output / "Source.zip", "w", zipfile.ZIP_DEFLATED, compresslevel=6) as source_zip:
        for name in ("app", "runtime", "renderer", "config", "tools", "tests", "assets"):
            for source in files_without_links(ROOT / name):
                if source.suffix.lower() not in (".pyc", ".log") and source.name != ".env":
                    source_zip.write(source, source.relative_to(ROOT).as_posix())
        for name in ("CMakeLists.txt", "COPYING", "README.md", ".gitattributes", ".gitignore"):
            source_zip.write(ROOT / name, name)

    settings = (ROOT / "saves/video-settings.ini").read_text()
    for old, new in (("width=2560", "width=1280"), ("height=1440", "height=720"),
                     ("frame_limit=120", "frame_limit=60")):
        if old not in settings:
            raise RuntimeError("Video defaults changed; review tester defaults before packaging")
        settings = settings.replace(old, new)
    write("saves/video-settings.ini", settings)
    command = ('"%~dp0bin\\DoritosNative.exe" --game-dir "%~dp0game" '
               '--shader-dir "%~dp0assets\\renderer" --asset-override-dir "%~dp0assets\\override" %*')
    write("Play.cmd", "@echo off\nsetlocal\ncd /d \"%~dp0\"\n"
          "if not exist \"logs\" mkdir \"logs\"\n" + command +
          " > \"logs\\last-run.log\" 2>&1\nset \"gameExit=%errorlevel%\"\n"
          "if not \"%gameExit%\"==\"0\" (\n"
          "    echo The game stopped with an error. Please send logs\\last-run.log with your report.\n"
          "    pause\n)\nexit /b %gameExit%\n")
    for title, script in (("Play with Diagnostics", "Run-Diagnostics.ps1"),
                          ("Collect Diagnostic Report", "Collect-Diagnostics.ps1")):
        write(title + ".cmd", '@echo off\nsetlocal\ncd /d "%~dp0"\n'
              'echo Doritos Crash Course - ' + title + '\n'
              'echo Opening diagnostic tools. Please keep this window open.\n'
              'if not exist "%~dp0diagnostics\\' + script + '" goto missingFiles\n'
              '"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" -NoLogo -NoProfile '
              '-ExecutionPolicy Bypass -File "%~dp0diagnostics\\' + script + '"\n'
              'set "toolExit=%errorlevel%"\n'
              'if not "%toolExit%"=="0" echo Diagnostic tool stopped with an error. Include the message above in your report.\n'
              'pause\nexit /b %toolExit%\n'
              ':missingFiles\necho Required diagnostic files are missing.\n'
              'echo Extract the entire ZIP, open that folder in File Explorer, then double-click this file.\n'
              'pause\nexit /b 1\n')
    write("README.txt", f"""Doritos Crash Course - private tester build {VERSION}

START
Extract the entire ZIP to a local writable folder, then double-click Play.cmd.
Use File Explorer to open the extracted folder. Do not type a bare launcher
filename into Windows Run: it needs the full path in quotes when it contains spaces.
Run from the extracted folder, not from inside the ZIP. No administrator access,
Python, Visual Studio, or legacy DirectX installer is required.
Target: Windows 10/11 x64, Intel/AMD CPU with SSSE3, Direct3D 9-capable GPU.
This is an unfinished native port; gameplay beyond the tested USA 1 section is
not fully verified. Original game license checks remain intact.

CONTROLS
Space/Enter: skip logos, confirm, jump. WASD/arrows: movement.
Escape: pause/resume/back. R: restart area. Backspace: restart level.
Mouse: menu selection, left-click confirm, right-click back, wheel navigation.
Xbox-compatible controllers also work.
Help & Options > Video Settings controls resolution, frame cap and image settings.
The package starts windowed at 1280x720 with a 60 FPS cap; adjust as needed.

TESTING
Please try title/menu navigation, starting USA 1, movement/jumping, pause/resume,
restart, video settings, sound, and closing/reopening the game.
Report the course, exact steps, expected/actual result, Windows version, CPU/GPU,
resolution and frame cap. Attach a screenshot and logs/last-run.log for failures.
Logs stay local. Nothing is uploaded automatically.

LIGHTWEIGHT DIAGNOSTICS
For a bug report, use Play with Diagnostics.cmd instead of Play.cmd.
The console shows startup progress; the game opens in a separate window.
Keep the console open while playing. If the game does not appear, include the
last progress message or error in your report. You can also try Play.cmd directly.
It records hardware/driver details once before launch, game errors, and FPS/frame
time summaries every 300 drawn presentations (about five seconds at 60 FPS).
There is no continuous hardware polling, video/screenshot capture, audio capture,
per-draw tracing or live overlay. The launcher sleeps while the game runs.
Frame timing describes drawn presentation completions, not GPU execution time.
After closing the game, double-click Collect Diagnostic Report.cmd and send the
ZIP created in reports/. It contains hardware/driver details, build identity,
exit status, game logs and video settings. Profile/gameplay save data is excluded.

SAVES AND REMOVAL
This ZIP includes no developer profile or gameplay saves. A new profile is created
in saves/ on first launch. Keep that folder when replacing a test build.
Close the game before moving or removing its extracted folder.

BUILD / CREDITS
This package includes the requested original game files for the private tester.
Credits and dependency terms are in CREDITS.txt and licenses/.
Owned port source/build scripts are included in Source.zip. The pinned offline
generator and dependencies are fetched by tools/setup_generator.py when building.
SHA256SUMS.txt and BUILD.json identify the included files and executable.
""")
    write("CREDITS.txt", """Doritos Crash Course original game: original creators and rights holders.
Native Windows port: this project; AI-assisted development/testing with OpenAI Codex.
XenonRecomp: https://github.com/hedge-dev/XenonRecomp
Method and patches informed by The Darkness Recomp:
https://github.com/portingpete/The-Darkness-Recomp/tree/4f711819bbc6ea12633a3b031454b86b5c0d929e
Port license: GNU GPL v3; see licenses/Port-GPL-3.0.txt and Source.zip.

CJ Carl Johnson model by derry@pennywise (abhishekambily), CC BY 4.0:
https://sketchfab.com/3d-models/cj-carl-johnson-3d-model-59463a7481e346cbb3622b150d3736d6
https://creativecommons.org/licenses/by/4.0/
Modified: fitted to the game rig, weights transferred, converted to Cal3D,
and textures/materials adapted. Original game animations are retained.
Kenney Input Prompts, CC0: https://kenney.nl/assets/input-prompts
See licenses/Kenney-CC0.txt.

Microsoft.DXSDK.D3DX 9.29.952.8, unmodified x64 retail app-local binaries:
https://www.nuget.org/packages/Microsoft.DXSDK.D3DX/9.29.952.8
See the accompanying Microsoft license and third-party notices in licenses/.
These terms apply separately to the Microsoft binaries; they are not relicensed
under the port's GPL. Use of those binaries is subject to those terms.
""")
    build = {"version": VERSION, "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
             "platform": "Windows x64", "executable_sha256": digest(output / "bin/DoritosNative.exe"),
             "includes_original_game_files": True, "includes_personal_profiles": False,
             "diagnostics": {"frame_report_interval": 300, "hardware_collection": "startup only",
                             "continuous_tracing": False, "capture": False, "automatic_uploads": False},
             "d3dx_package_sha256": digest(args.d3dx_package)}
    write("BUILD.json", json.dumps(build, indent=2))
    rows = [f"{digest(path)}  {path.relative_to(output).as_posix()}" for path in sorted(files_without_links(output))]
    write("SHA256SUMS.txt", "\n".join(rows) + "\n")
    assert not (output / "saves/profiles").exists()
    print(json.dumps({"output": str(output), "files": len(rows) + 1, **build}, indent=2))


if __name__ == "__main__":
    main()
