# Independent native engine rewrite

The rewrite replaces the recompiled engine with ordinary PC data structures and Windows APIs. Its executable does not link the PowerPC context, Xbox memory map, Xbox imports, original XEX loader or AOT game library. It is developed alongside the working port; `Launch.cmd` continues to run that port.

**Current status: character animation, static course/environment rendering, basic course movement and a partial native trigger/checkpoint session. The replacement game is incomplete.**

## Implemented and checked

- A separate CMake build route returns before configuring the AOT toolchain or its dependencies.
- Native little-endian asset bundle with bounded counts, native indices, transforms and strings.
- Native skeleton hierarchy, inverse-bind transforms, quaternion interpolation, looping, one-shot sampling, local pose blending and CPU skinning. Authored root-motion curves retain their nonuniform key times and accumulate correctly across animation loops.
- Direct3D 11 device/resources, project-authored shaders and WIC image loading.
- Direct Windows keyboard/controller input and a fixed-step native movement/jump diagnostic arena.
- Original 15-course catalog and chunk references converted from the player's package.
- Native static mesh and material resources, texture decoding, alpha blending and authored resource transforms. Course chunks join through their start/end links; all 15 layouts are converted, with 9,647 static instances across the course set. Serialized angles are yaw(Y), pitch(X), roll(Z), composed in that order; axis and noncommuting-transform tests cover the conversion.
- Three authored environment stages, including 11 additional static mesh resources, background colors, base camera settings and diffuse ambient/directional lights. The camera uses the stage FOV's original 4:3-to-vertical conversion. The base orbit follows the recovered axis/order; smoothing, dynamic framing and runtime camera overrides remain pending. Decals, water, animated stage actors, effects, full materials and shadows remain unfinished, so these views do not establish visual parity.
- Native triangle queries with an engine-built spatial tree, using 25,390 converted static platform triangles across the 15 courses. Downward rays locate all 15 authored starting platforms. Continuous ellipsoid sweeps handle triangle faces, edges and vertices, initial overlaps and sliding. The native course player uses these responses and a separate base-point grounding query. Dynamic obstacle collision remains pending.
- Locally converted movement settings, collider offset/radii and eleven movement/landing/startup clips, including slow/fast authored motion. Native course movement includes inertia, no-input drag, playback speed, slow/fast motion blending, airborne steering, held jumps, gravity, adaptive movement substeps and landing. Startup selects the country's idle and waits for directional input; Space alone does not leave it. A simultaneous first direction/jump press queues the jump for the next input tick. Synthetic tests cover these transitions, settlement, held-jump height, release/repress behavior and swept walls. Intro/countdown, turns/braking, other action states and obstacle interactions remain unfinished.
- Structural readers parse all 183 scene files, 56 resource templates, 106 animation files, 153 static mesh files and 36 collision mesh files in the supported original package.
- A native Lua 5.1 library loads all 1,886 compiled scene-script assets after a bounded conversion of their serialized string lengths for Windows x64. Instructions, constants and strings retain their authored values. Native host objects, task-specific actor contexts, `Wait(seconds)` and `WaitFrames(n)` retain context across coroutine yields and enforce memory/instruction limits. Four source-text scripts are reported separately and are not converted.
- A bounded scene bundle retains 273 STARTPOS/TRIGGER records and 260 authored trigger boxes across all 15 courses, with exact script references. The partial single-player session dispatches enter/leave commands, cooldowns and one-shot counts; runs the owned checkpoint helper; stores checkpoint/HUD/resource state; and supports manual respawn at the exact checkpoint position without resetting the session timer. Finish-group contact freezes the timer separately from the unimplemented finisher cinematic. Sensors currently sample post-step overlap; swept contacts, original subframe finish correction and intro/timer-start parity remain pending. Actor-group activation/deactivation, many script methods, automatic deaths, medals and results remain pending. Unsupported commands produce explicit issues in the log/HUD; they are not treated as implemented.

The first isolated live run rendered textured CJ in idle, jump and post-run poses, with 1,321 native simulation ticks, 661 GPU frames, movement and a measured jump arc. The rest-pose skinning error was approximately `1.23e-7` model units. This validates the animation arena and asset catalog; it does not establish original course gameplay, matching original physics or a performance comparison.

A separate isolated static-course sweep produced 15 GPU captures and verified visible course geometry, distinct layouts and successful native process shutdown. These views do not establish full-course visual parity or obstacle simulation.

Offline comparisons replay original USA 1 input masks and raw simulation intervals through the native controller. Acceleration/release, a jump from rest, a simultaneous direction/jump press and a preaccelerated jump closely match the captured positions; the 132-step preaccelerated sequence has maximum horizontal/vertical errors below `3e-6` units after applying the recorded input on the following frame. This is a scoped movement check from the ready state. Separate original-state telemetry establishes the startup directional gate now implemented in the preview. Opposite-direction motion still has a small transient difference. These results do not establish full movement or course parity.

## Build the preview

Use the same Windows x64, VS 2022 + ClangCL, Windows SDK, CMake and Python requirements as the main project. Original game assets are converted locally; they are not redistributed. CMake downloads the hash-pinned official Lua 5.1.5 source and builds it for Windows x64; no original executable translation is needed. See [dependency and license details](../engine/THIRD_PARTY.md).

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_engine.ps1
& '.\Native Engine Preview.cmd'
```

The preview uses the bundled CJ pack by default. To prepare another installed character, pass `-Character <pack-id>` to the build script. Conversion does not change the working port's selected character.

In the preview, A/D moves, Space jumps, Tab cycles the course catalog and Escape closes. Controllers use the left stick and A. The floor is a diagnostic arena; choosing a catalog entry does not load that original course yet. The HUD states this scope explicitly.

To inspect original static course layouts:

```powershell
& '.\Native Engine Preview.cmd' --course-preview --level 0
```

This mode uses A/D to pan the camera, W/S to change camera height and Tab to load the next course. CJ remains at the authored start position. It shows static course and environment RVM geometry with diffuse lighting and a base following-camera placement. Animated/attached/procedurally constructed actors, complete materials, effects and obstacle simulation remain pending. The converter reports omitted categories and unresolved resources.

To try native movement on the static course geometry:

```powershell
& '.\Native Engine Preview.cmd' --course-play --level 0
```

A/D leaves startup and moves, Space jumps with hold-dependent height, R respawns at the last registered checkpoint and Tab starts another course. Controllers use the left stick and A. Until a checkpoint registers, R uses the authored starting STARTPOS. This mode has a following camera, native static collision and the partial trigger session, with no save writes. Rebuild/reconvert for the new environment and scene bundles. Dynamic/procedural actors and full course gameplay are still missing, so gaps and obstacles cannot yet be treated as a faithful playable replacement. Automatic failure handling remains pending.

The independent executable is `build_native/engine/engine/Release/DoritosNativeEngine.exe`; prepared assets are under `build_native/engine_inputs/`.

## Verify independence and behavior

```powershell
ctest --test-dir build_native/engine -C Release --output-on-failure
py -3 tools/audit_engine.py --executable build_native/engine/engine/Release/DoritosNativeEngine.exe
build_native/engine/engine/Release/DoritosNativeEngine.exe --check --assets build_native/engine_inputs
build_native/engine/engine/Release/DoritosNativeEngine.exe --check --course-preview --assets build_native/engine_inputs
build_native/engine/engine/Release/DoritosNativeEngine.exe --check --course-play --assets build_native/engine_inputs
```

The dependency check scans the independent source boundary and the actual native linker map for forbidden runtime dependencies. It also requires the native core and D3D11 link evidence. This is a scoped build check, not a claim that naming alone proves independence.

For a bounded automated run, provide absolute asset and output paths and use `--hidden --verify-run <fresh-output-directory>`. It drives the normal native simulation input path through idle, running and jumping, saves three actual GPU captures and a result file, then exits. It sends no operating-system input. The output directory must not exist already. Run from an empty isolated working directory to verify that only converted assets are required. This mode has no save/progression writes.

`--hidden --verify-courses <fresh-output-directory> --assets <absolute-assets-directory>` inspects a middle chunk in each of the 15 static course layouts, captures one GPU frame per course and exits. These captures establish static rendering only. They do not verify obstacle motion or course completion.

`--hidden --verify-player <fresh-output-directory> --level <0-14> --assets <absolute-assets-directory>` tests settlement, acceleration, a held moving jump and landing on that course's starting platform. It saves four GPU captures, a per-tick input/position CSV and a result file, then exits after five seconds. This verifies the starting-platform sequence only.

The independent CTests include synthetic player, script, scene-session and environment contracts. `NativePlayerTests.exe <assets-directory>` additionally checks settlement and a short movement/jump sequence on all 15 authored starting platforms. Its optional `<input.csv> <new-output.csv>` arguments replay a private USA 1 controller trace from the ready state; append `startup` to replay from startup. The input header is `dt,horizontal,pressed,held`, and the output must not exist. Replays read converted assets and ordinary control values only.

`NativeScriptTests.exe <scripts.dnl>` additionally asks the native Lua loader to validate each converted script prototype. The regular `--check` performs the same validation when a converted script bundle is present. `NativeSessionTests.exe <assets-directory>` also exercises the owned checkpoint helper through the native session on authored checkpoint boxes. These are bounded dispatch/respawn checks; they do not establish obstacle behavior or course completion. Unknown host functions fail explicitly.

## Work still required for the full game

The rewrite is complete only when it reproduces the original game through independent PC systems and passes comparison against the working port:

1. Original static and animated course geometry, materials, lighting, cameras and chunk placement.
2. Original collision shapes, movement, sliding, grabbing, ropes, conveyors, moving platforms and obstacle interactions.
3. Checkpoints, failures, respawns, finishing, timing, medals and progression.
4. Original audio decoding/playback, music, sound events and mixing.
5. Native menus, settings, profiles and separate native save storage, with deliberate migration rather than overwriting existing saves.
6. End-to-end testing on all 15 courses and importer regression checks.

The existing game remains the behavior oracle while these systems are replaced. Analysis data, extracted assets, converted bundles and evidence stay in ignored local storage. No full-game or original-physics success is inferred from the current arena.
