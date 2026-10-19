# Native title renderer contracts

This renderer consumes the original compiled Tongas engine's CPU draw, shader,
resource and frame contracts. Windows D3D9Ex owns real GPU resources. It does
not execute Xenos packets or supply a guest GPU object layout. Guest pointers
returned by these adapters are explicitly tagged opaque resource tokens.

The original encrypted XEX and package remain inputs. Decompiled/generated
source, extracted shader caches, HLSL and other game assets belong under the
ignored `build_native/` directory. The tracked code contains format contracts,
bounded adapters and synthetic verification fixtures.

## Integration

Call `prepareTitleGraphics()` with the two verified original cache payloads
and an optional verified native cache extension before guest entry. Extension
keys append only; an original key or stage cannot be replaced. Link
`title_bridge.cpp` directly into the native executable
so its strong `sub_ADDRESS` symbols replace the generated weak wrappers.
Call `shutdownTitleGraphics()` before destroying guest `Memory`. The dedicated
window thread owns a 2560×1440 client window. Successful device initialization
reveals it with a startup caption so foreground keyboard input can reach the
title. An actual title draw followed by successful presentation changes the
caption to the game's name.

`titleGraphicsCounters()` distinguishes real draw submissions, triangles and
frame presents from initialization and clear-only presents. The initialization
present is permitted only at the original call site `0x8234737C`; it does not
count as a rendered game frame. A backbuffer target or a
successful hardware contract test alone does not verify gameplay or 60 FPS.

## Established boundaries

| Original function | Contract consumed |
| --- | --- |
| `823D25D8` | Device creation, presentation pointer in r7 and output device pointer in r8. Creates an actual HAL D3D9Ex device with SM3. |
| `823C0440`, `823C0628` | Pixel/vertex creation from original Xenon shader blob in r3. Match all original cache bytecode occurrences, then resolve exact name and three feature words to native PC code. |
| `823BEA90`, `823BEC50` | Bind pixel/vertex shader by opaque token in r4, including null. |
| `823BE6E8`, `823BE7C0` | Original float4 register uploads. Decode big-endian floats; remap by original parameter IDs into each native variant's reflected registers. |
| `823BEFA8` | Original shader GPR allocation policies flags0, 32/96 or default0/0. D3D9 owns compiled shader register allocation; no guest packets are fabricated. |
| `823CE338`, `823CE410` | VB byte allocation and triangle-list INDEX16 allocation. The original IB format is r5=1. |
| `824CF7D0`, `824CFBD8`, `823E3498` | Exact AvatarPose constructor's supplied32-byte VB storage and borrowed3456-byte slices of real6912-byte physical backing. Address association stages CPU bytes; the genuine physical free detaches pending/attached aliases before forwarding the original free. |
| `823CF300`, `823CF350` | Verified whole-backing pointer locks, flags0. Original wrappers replace incoming offset/size registers. |
| `823CE400`, `823CE4C0` | Mark CPU buffer bytes dirty for native reupload. |
| `823BEF28`, `823BEE20` | Decode original 12-byte declarations and bind actual native declarations. Supported encoded types are FLOAT2/FLOAT3/FLOAT4/D3DCOLOR. |
| `823C6040`, `823C61E8` | Stream0, offset0 vertex binding and INDEX16 binding, including null. Endian conversion occurs from original CPU data. |
| `823C7E50` | Original indexed type4 triangle lists or type6 strips, base vertex r5, start index r6 and index count r7. Submit actual native primitives with checked index/vertex extents. |
| `823C79C0` | Original indexed-UP type4/type6 batches, checked BE16 indices and actual declaration/stride conversion. The original GUI batch uses4vertices/6indices/stride32. |
| `823D6BB8` | Create 2D/cube textures from width, height, depth1/6, mip count, usage0/1 and verified Xbox format. |
| `823D6968`, `823D6A48` | Return actual native texture level or cube face/level surface view. |
| `823D6CD8` | Create actual color/depth surface, native multisample none/2/4. Guest EDRAM-placement descriptors are unsupported. |
| `823C6FC0`, `823C6CE8` | Bind color render target and depth surface. |
| `823D6F38` | Bind native 2D/cube texture to original sampler slot. Native resource binding replaces the guest fetch dirty-mask packet operation. |
| `82648F60` | Original whole linear surface upload: no palettes, destination rectangle, resize or color key; observed filter1. Convert fetch endian to native rows, then stage and upload actual GPU pixels. |
| `823C6868` | Original 24-byte viewport: four big-endian integers and two floats. |
| `823C63D0`, `823C6300` | Native gamma-ramp get/set, converting 768 halfwords between host and guest endian. Getter output is r4; setter ramp is r5 and original incoming r4 flags are ignored. |
| `823CDF60`, `823D6958`, `823D6A40`, `823D6E10` | Actual resource type, mip count and texture/surface descriptions; CPU outputs retain original format/type values without fabricated Xbox GPU headers. |
| `8264A4B8` | Actual native BOX mip generation from source level0, filter5 and no palette. |
| `8264B0B8`, `8264B128` | Actual 2D/cube DDS creation and upload for the observed default arguments. Missing lower levels are generated from the real authored base images. |
| `823CFE40` | Return actual native D3D9 capabilities through the original 304-byte big-endian output ABI. |
| `823CDF10`, `823D75C0` | Actual main-target resolve and exact ordinary2xMSAA color RT resolve; verified no-EDRAM/no-predication mask0 policy. Depth-source and nonzero predication masks remain unsupported. |
| `823CF280` | Release supported native resources. Device teardown is explicit. |
| `823D2188`, `823D2220` | Native BeginScene/EndScene. |
| `823D2EC0`, `823D2EC8` | Actual frame present and next presentation texture selection. |
| `82371F60` | Original signed alpha-reference policy: negative disables alpha test; nonnegative clamps to255 and tests GREATER. Preserves CPU cache at renderer+56B80. |
| `8237A5E8` | Original sampler U/V addressing modes wrap/mirror/clamp/border; border RGB reduces to transparent black or white exactly as the original function. |
| `823C4770`, `823C4728`, `823C64B8` | Native six-bit clip-plane enable query/set and four-float plane upload. Original CPU plane transformation remains compiled. |

Original renderer vtable is at `821601C4`, singleton `82BAA92C`; device singleton
is `82BAC23C`. The low halfword is signed and borrows from the `lis 82BB` high
halfword. SimpleMesh submission `82344030` uses original mesh VB/IB handles
at +192/+200 and logical counts at +216/+220. Renderer indexed submission
`8234D4A8` expands triangle start/count by three before the native draw boundary.
GenericMesh material flag +224 selects `8234D5D0`, which consumes N+2 indices
for N strip triangles. Native D3D9 uses strip enum5; Xbox enum6 is translated
explicitly. The authored indices retain their order, including degenerates,
so native strip winding and culling remain intact. Actual referenced indices
must fit the selected vertex range after applying the original base vertex.
Immediate GUI batches through `823C79C0` submit original CPU geometry through
native indexed user-memory draws. A prepared frame may bracket that real draw
with native BeginScene/EndScene while retaining an existing explicit scene.
The observed ordinary buffered list draw uses the same prepared-frame policy,
with all shader/declaration/VB/IB identities and referenced indices validated
before opening a scene. Buffered bindings remain bound afterward. A bounded
diagnostic distinguishes missing bindings from a missing original frame.
Offscreen camera rendering uses its own original RT Begin82390020 rather than
onscreen startFrame. Its exact color-bind call8239009C prepares the native frame
after successful binding, guarded by the CPU owner and whole real2x color/depth/
resolve-texture dimensions and formats. Original viewport/depth binding and CPU
flow remain compiled. A color clear alone grants no frame boundary; unprepared
color clears report their original caller and actual native target separately.

## Shader and vertex evidence

Cache loader `82387AE0` establishes a three-word header `[3,0,shaderCount]`,
length-prefixed terminal-NUL name, variant count, three feature words, parameter
records `[id,registerIndex,registerCount,0]`, and bounded shader code bytes. PC
fields are little-endian and Xenon fields big-endian. The original caches have
210 PC variants and 352 Xenon variants; 170 exact keys overlap. Ambiguous
bytecode matches remain ambiguous unless their native code and parameter maps
agree. Missing core native variants fail with their exact name/key. Known
unavailable original optional Avatar/AvatarPose keys return genuine null
creation without a GPU resource, preserving the original loader's failure
callback. Unknown, corrupt and unresolved ambiguous bytecode still fails.

Original HLSL macro builder `823878C8` uses fixed 30-byte names: lighting31 bits
at `820F5570`, pixel29 at `820F5930`, vertex30 at `820F5CB8`. Stage/platform
names begin at `820F6060`. Native compilation uses the original PC branch,
`SHVER_SH3`, the matching stage define and the original `_MAXBONES` string48.
`82387140` reflects original constant names against the 155-name table at
`820F61C8`; sampler records are skipped. No Xbox bytecode translation is used.

The main-family native extension compiles 122 missing composite and
multilayer keys. All 160 main-family keys shared with the original PC cache
reproduce its exact parameter-ID/register/count maps before the missing keys
are trusted. Original Xenon parameter IDs/counts match the new native keys;
register positions are remapped by ID at runtime. Sources, macros, image,
package and compiler hashes are recorded in the ignored compiler manifest.

Original declarations use big-endian stream/offset halfwords, encoded type
word, method/usage/usage-index bytes. FLOAT2=`002C23A5`, FLOAT3=`002A23B9`,
FLOAT4=`001A23A6`, D3DCOLOR=`00182886`. All 23 original engine declaration
constructors are retained; unsupported forms fail. Logical IB counts describe
triangles, with six bytes per triangle.

## Texture evidence and remaining boundaries

Stock package inventory contains 82 PNGs, 386 JPEGs and two standard DXT5 DDS
files. The original ordinary decoder emits linear A,R,G,B bytes. Fetch endian
8-in-32 requires four-byte reversal into native B,G,R,A; opaque images force
alpha255. Original DXT uploads use 8-in-16 byte-pair reversal. Tiled destination
allocation is replaced by native resources, while tiled source uploads fail.
The two original DDS payloads are already standard PC little-endian data.
Their seven actual authored faces have passed native upload and generation
of 57 real mip levels, including the 1×1 DXT5 levels. This isolated asset check
submits no geometry or game presentation. Native mip generation uses matching
SYSTEMMEM staging textures, installed Windows `d3dx9_43.dll` BOX filtering,
and actual GPU uploads of the resulting mip chain.

The original engine embeds Xbox SDK macros inside several high-level methods.
They must be adapted before they can dereference opaque native tokens. Native
device/frame adapters preserve the verified CPU path and create real native
objects. Original postprocess, EDRAM placement, unverified shader families,
partial uploads, format conversion/compression, arbitrary lock flags and other
unmapped operations remain explicit boundaries; they must not silently become
success returns. Derived strong AOT clones may replace verified exact inline
instruction ranges with established native semantic calls while retaining the
rest of the original CPU function.

## Verification

`NativeGraphicsTests` tests bounded cache/declaration/buffer parsing, truncation,
duplicates, ambiguous cache matches and endian conversion with synthetic data.
`NativeShaderCacheCheck --hardware` creates all 210 original PC shader objects
on the actual hardware at the requested backbuffer size. It submits no draws
and reports `gameplay_verified:false`. These checks do not establish that the
game has reached menus, an actual level, or sustained 60 FPS.

Passing `--extension PATH` additionally validates strict append-only merging
and creates every merged native shader. The 128-key extension adds the six
original AvatarPose pixel variants to the 122 main-family variants and passed
actual HAL creation for all 338 merged shaders at 2560×1440; it covers
298 of 352 original Xenon keys. Draw and present counters remain zero for this
cache validation.

Identical Xenon shader bytes can belong to several distinct feature keys whose
native code differs. The low creation adapter preserves the exact CPU identity
from the original source loader (`82387628`/`82387638`, master r16) and cached
loader (`82387D40`/`82387D4C`, master r23): master+8 canonical name, master+120
stage, and variant r30+8/+12/+16 key. Source variants also require the original
+2536 owner; cached variants require their +44 size/+48 bytecode allocation.
Selection still requires exact original Xenon bytecode equality. Unnamed
divergent aliases, corrupt identities and unknown keys remain explicit errors.
The hardware cache check additionally creates/binds two real duplicate-code
aliases by their individual identities and rejects both unnamed requests.

The original source loader also compiles feature keys absent from its shipped
precache. The first measured BoneModel request is `multilayer/vs [8388614,0,75]`:
two directional lights, no specular, normal/tangent/UV0 inputs and ordinary skin
matrices, with no texture-sampling feature. It reaches the real compiler at
`82648DA8` and the vertex creation return address `82387638`. A bounded diagnostic
capture records its actual original bytes, source-variant owner and reflected
bindings in ignored storage. `compile_title_variants.py --source-capture PATH`
accepts this exact measured request, compiles its native counterpart from the
verified original portable source and requires matching parameter IDs/counts.
Its report pins both the native extension and `original_source_variants.bsh`,
which preserves the captured Xenon code and bindings. Configuration validates
the pair again; native creation still requires exact original key and bytecode
equality. No nearby key, feature mask or generated Xenon substitute is used.

`NativeGraphicsHardwareContract` verifies 56 real HAL texture uploads across
ARGB/XRGB/DXT1/DXT5, 2D/cube faces and two mip levels, native color/depth surfaces,
state, clear, resolve and actual BOX mip generation. Its fixtures are synthetic;
the observed1280x3842xMSAA color/depth pair is also allocated, bound and resolved
on the real HAL. Resolve contracts reject a different bound source or sample
count. The original non-tile color RT caller823903DC uses flags0 for automatic
MSAA color averaging; flag4 selects depth and is accepted only by the guarded
original type1 alias End described below. Its supplied
RGBA/f1 are unused because clear gates0x100/0x200 are absent. Native adaptation
checks the original owner identity and actual resources before StretchRect.
The contract submits zero geometry and refuses an empty frame present. Initialization and
clear-only presentations are counted separately from game frames and the
validation window remains hidden. The actual title window becomes visible
after real device initialization to permit foreground input; this does not
claim a rendered game frame.

An absolute local `DORITOS_CAPTURE_DIR` enables one BMP capture after the first
successful presentation containing original title draws. Pixels come from
actual backbuffer GPU readback; clear-only frames are excluded. FPS intervals
likewise require consecutive presentations containing original title draws.
`DORITOS_CAPTURE_FRAME=N` selects a later genuine drawn presentation (positive
1..1000000, default1), saved as `frame-PID-N.bmp`. The capture-path diagnostic
records the selected real texture and its successful resolve history; the
original title selects its next presentation texture after Present, so an
initial image can precede the current draw's resolved image by one frame.
Set `DORITOS_CAPTURE_ON_DEMAND=1` alongside the capture directory to permit
later captures from the same process. Startup logs its PID and creation time;
`python tools/request_native_capture.py --pid PID --creation-time-100ns TIME`
signals one request after verifying the exact executable and process lifetime.
The next genuine drawn presentation produces `frame-PID-N.bmp` using its actual
presentation count. Requests remain pending across clear-only frames and can
be repeated after capture; simultaneous requests coalesce. The helper does not
focus the window or issue keyboard input. GPU readback and file writing can
affect measured intervals, so steady performance should be measured after those
intervals have left the recent window.
Set `DORITOS_FPS_REPORT=1` to flush a live report every 300 successful drawn
presentations, including size, interval count, cumulative average and worst
frame interval. A separate recent report measures the last 300 consecutive
drawn intervals and states whether that window is complete. Clear-only presents
reset the recent window, break timing continuity and produce no FPS report.
This permits measurement during a bounded run without relying on process
teardown, while retaining lifetime loading stalls in the cumulative metrics.
Set `DORITOS_GAME_STATE=1` to sample the original NETMAN mode/state, selected
track/theme and actual Game pointer every 300 genuine drawn presentations.
The probe reads only validated memory spans and makes no guest calls or writes.
Set `DORITOS_PHASE_REPORT=1` to report CPU wall-call timing over the last300
consecutive drawn presentation cycles, once every300 drawn presents. The report
separates validation, constants, constant source decoding, native draw/scene calls, resolve/copy, Present,
pacing, GPU readback/CPU capture writes and buffer uploads. Outermost renderer
bridge hooks are measured once, including nested native operations, so their
inclusive coverage is separate from the selected nonoverlapping phase sum.
Cycle boundaries are the completed original Present hooks; clear-only cycles
reset the window. Time outside those hooks includes original CPU execution,
scheduling, lock waits and other unmeasured work. These are CPU wall-call times,
including possible driver waits, and do not measure GPU execution duration.
Readable pointers and menu timing alone do not establish playable gameplay.

The original vertex/pixel constant-upload boundaries (`823BE6E8`/`823BE7C0`)
validate one complete CPU source span, then decode its big-endian float bits.
They retain the 1024-register bounds and reject MMIO/overflow; inaccessible
committed memory still fails through the existing guest-span validator. This
avoids a repeated `VirtualQuery` for each scalar while preserving original
NaN payloads and all other bits. `constant_decode` measures span validation,
allocation and conversion separately from native constant application.

The exact first SDK Avatar declaration initializer can receive genuine null
creation when its verified packed layout is unavailable. Its original negative
return path leaves zero ownership slots; no declaration token or GPU object is
created. This requires the original call LR, stack pointer, manager vtable and
complete semantic layout. Other unsupported encodings still stop execution;
later binding of missing SDK resources also remains unsupported.

The original AvatarPose instance constructor also creates two embedded palette
buffer slots at owner+336/+368, backed by its actual6912-byte physical allocation
at owner+404. Registration and address association require the exact original
caller, vtables, iterator and slice bounds. These resources borrow both CPU
storage and data; native GPU upload stays lazy and graphics teardown never frees
either address. The original physical-free wrapper detaches both pending and
associated aliases before forwarding its unchanged import ABI outside the
graphics mutex. No Xbox GPU header is fabricated. Their later stream1 palette
binding remains an explicit boundary, separate from ordinary Cal3D vertex data.

The verified `multilayer/vs` and `composite/vs` skin variants reflect parameter81
as192 rows, while original parameter code823698A0 uploads only4 rows per active
group bone. Original feature word2 bit6 is `_USE_SKINMATRIX`, and both original
main sources access that array only through the same four-index SkinVertex loop.
The actual cache audit covers110 matching bindings (105 multilayer,5 composite),
with original starts0/5/6/8/9/10/11/12/13 and native start0. The hash-bound source,
feature table and compiler report evidence is recorded in ignored
`build_native/renderer_recon/bone_palette_audit.json`; the non-HAL cache checker
also reports actual accepted binding count. Eligibility requires the immutable
original/native cache identity, vertex stage, exact family, skin feature bit,
ID81/count192 and bounded reflected register spans. Its native draw accepts a
shorter prefix only with the exact Bone
instance/interface/master, selected group and retained format10 VB/IB ownership.
Real logical vertex/triangle counts exclude allocation sentinels. All four
FLOAT4 bone indices of every referenced vertex must address the active group,
including zero-weight lanes. Actual uploaded prefix bits must match the current
renderer palette. Only those real rows are mapped to native registers; the
unreachable reflected tail is untouched. Every other constant remains strict.
Original8239EDF8 retargets the exporter position/stride and index pointers to
the uploaded buffers after its CPU copy. Its secondary attribute pointers keep
freed staging addresses; these are never dereferenced or used to authorize a
native draw. Index validation uses the actual retained native VB/IB bytes.

RVMBatch supplies a separately verified active matrix prefix. Its exact original
owner/parent, full resource handles, clean format16 buffers, replicated indices
and all four per-vertex matrix lanes bound shader reads to the active instances.
The native upload uses only those checked current rows for the same eligible
ID81 skin-shader bindings. The reflected 192-row tail may be unwritten and stays
untouched; missing or stale active rows still fail validation. Other constants
retain complete upload checks.

Device startup reports the actual D3D adapter, display mode and adapter/window
monitors. A known display refresh below 60 Hz uses immediate presentation with
the existing 60 FPS render cap; refresh 0 or at least 60 Hz keeps synchronized
presentation. Desktop display refresh remains separate from measured native
render FPS. The native backend owns this policy and preserves the original CPU
presentation preference in the title descriptor.

Native resolve/presentation uses the documented D3D9 scene ordering; see
[StretchRect](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-stretchrect)
and [BeginScene](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-beginscene).
Native strip ordering follows the documented [triangle strip topology](https://learn.microsoft.com/en-us/windows/win32/direct3d9/triangle-strips).

The original type1 constructor uses a D24S8 helper to create a real one-level DEFAULT
depth texture and an independently owned level0 surface token. It validates
the original type1 owner and actual native capability/container identity,
publishes both tokens atomically, and rolls back native handles and guest
storage on failure. Genuine native/host allocation exhaustion returns an empty
pair; invalid contracts, unsupported capabilities and device errors still fail.
Surface descriptions retain their parent kind without reading a freed parent
token. Depth-only sampler/target identities retain real COM references across
token release, matching D3D9 binding ownership. Conflicting sampling or depth
rebinding is rejected before changing native bindings. No mandatory unbind is
invented at token release.

Original Begin binds the matching real color/depth pair. Exact type1 End
validates its authored full clear, currently bound resources and level0 alias;
the completed depth image stays in place without a copy or an invented unbind.
The subsequent original main-camera StartFrame restores the native depth target.
Actual depth reads require a matched original/native main-family pixel shader,
an eligible non-floating feature key, a declared 2D sampler9 and the retained
completed image. Cube, second-spotlight and floating-depth paths are excluded.
CPU image upload, mip generation, color resolve and presentation cannot accept
these depth textures. Pure allocation rollback, 542 pure checks, hidden HAL
alias/lifetime/hazard checks and all91 actual native pixel declaration audits
pass; these isolated checks report no playable game frames. Live original
shadow-pass and consumer effects remain unverified.
The independent comparison-effect probe and original shadow camera evidence
are saved privately in ignored `build_native/renderer_recon/`.
