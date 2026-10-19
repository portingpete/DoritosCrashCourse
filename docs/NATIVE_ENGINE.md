# Independent native engine rewrite

The rewrite replaces the recompiled engine with ordinary PC data structures and Windows APIs. Its executable does not link the PowerPC context, Xbox memory map, Xbox imports, original XEX loader or AOT game library. It is developed alongside the working port; `Launch.cmd` continues to run that port.

**Current status: character animation and static course inspection previews. The replacement game is incomplete.**

## Implemented and checked

- A separate CMake build route returns before configuring the AOT toolchain or its dependencies.
- Native little-endian asset bundle with bounded counts, native indices, transforms and strings.
- Native skeleton hierarchy, inverse-bind transforms, quaternion interpolation, looping and CPU skinning.
- Direct3D 11 device/resources, project-authored shaders and WIC image loading.
- Direct Windows keyboard/controller input and a fixed-step native movement/jump diagnostic arena.
- Original 15-course catalog and chunk references converted from the player's package.
- Native static mesh and material resources, texture decoding, alpha blending and authored resource transforms. Course chunks join through their start/end links; all 15 layouts are converted, with 9,647 static instances across the course set.
- Native triangle queries with an engine-built spatial tree, using 25,390 converted static platform triangles across the 15 courses. Downward rays locate all 15 authored starting platforms. This is collision-query groundwork; original character/obstacle collision responses remain pending.
- Structural readers parse all 183 scene files, 56 resource templates, 106 animation files, 153 static mesh files and 36 collision mesh files in the supported original package. Scene scripts are retained as data, not executed by this preview.

The first isolated live run rendered textured CJ in idle, jump and post-run poses, with 1,321 native simulation ticks, 661 GPU frames, movement and a measured jump arc. The rest-pose skinning error was approximately `1.23e-7` model units. This validates the animation arena and asset catalog; it does not establish original course gameplay, matching original physics or a performance comparison.

A separate isolated static-course sweep produced 15 GPU captures and verified visible course geometry, distinct layouts and successful native process shutdown. These views do not establish full-course visual parity or obstacle simulation.

## Build the preview

Use the same Windows x64, VS 2022 + ClangCL, Windows SDK, CMake and Python requirements as the main project. Original game assets are converted locally; they are not redistributed. No generator download or original executable translation is needed for this target.

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_engine.ps1
.\Native Engine Preview.cmd
```

The preview uses the bundled CJ pack by default. To prepare another installed character, pass `-Character <pack-id>` to the build script. Conversion does not change the working port's selected character.

In the preview, A/D moves, Space jumps, Tab cycles the course catalog and Escape closes. Controllers use the left stick and A. The floor is a diagnostic arena; choosing a catalog entry does not load that original course yet. The HUD states this scope explicitly.

To inspect original static course layouts:

```powershell
.\Native Engine Preview.cmd --course-preview --level 0
```

This mode uses A/D to pan the camera, W/S to change camera height and Tab to load the next course. CJ remains at the authored start position. It shows static RVM geometry and diffuse materials with a simple native light. Animated/attached/procedurally constructed actors, environment rendering, original lighting/shader effects, collision and obstacle simulation are still pending. It is a course inspector, not a playable course replacement. The converter reports omitted attached actors and unresolved resources instead of treating them as implemented.

The independent executable is `build_native/engine/engine/Release/DoritosNativeEngine.exe`; prepared assets are under `build_native/engine_inputs/`.

## Verify independence and behavior

```powershell
ctest --test-dir build_native/engine -C Release --output-on-failure
py -3 tools/audit_engine.py --executable build_native/engine/engine/Release/DoritosNativeEngine.exe
build_native/engine/engine/Release/DoritosNativeEngine.exe --check --assets build_native/engine_inputs
build_native/engine/engine/Release/DoritosNativeEngine.exe --check --course-preview --assets build_native/engine_inputs
```

The dependency check scans the independent source boundary and the actual native linker map for forbidden runtime dependencies. It also requires the native core and D3D11 link evidence. This is a scoped build check, not a claim that naming alone proves independence.

For a bounded automated run, provide absolute asset and output paths and use `--hidden --verify-run <fresh-output-directory>`. It drives the normal native simulation input path through idle, running and jumping, saves three actual GPU captures and a result file, then exits. It sends no operating-system input. The output directory must not exist already. Run from an empty isolated working directory to verify that only converted assets are required. This mode has no save/progression writes.

`--hidden --verify-courses <fresh-output-directory> --assets <absolute-assets-directory>` inspects a middle chunk in each of the 15 static course layouts, captures one GPU frame per course and exits. These captures establish static rendering only. They do not verify obstacle motion or course completion.

## Work still required for the full game

The rewrite is complete only when it reproduces the original game through independent PC systems and passes comparison against the working port:

1. Original static and animated course geometry, materials, lighting, cameras and chunk placement.
2. Original collision shapes, movement, sliding, grabbing, ropes, conveyors, moving platforms and obstacle interactions.
3. Checkpoints, failures, respawns, finishing, timing, medals and progression.
4. Original audio decoding/playback, music, sound events and mixing.
5. Native menus, settings, profiles and separate native save storage, with deliberate migration rather than overwriting existing saves.
6. End-to-end testing on all 15 courses and importer regression checks.

The existing game remains the behavior oracle while these systems are replaced. Analysis data, extracted assets, converted bundles and evidence stay in ignored local storage. No full-game or original-physics success is inferred from the current arena.
