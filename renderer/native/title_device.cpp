#include "runtime/native/runtime.h"
#include "title_bridge.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace Doritos::Graphics {
namespace {
using Doritos::Native::memory;
using Doritos::Native::fail;
constexpr uint32_t deviceGlobal = 0x82bac23c; // lis -32069; lwz/stw -15812 (signed low half).
constexpr uint32_t colorFormat = 0x28280186;
constexpr uint32_t depthFormat = 0x2d200196;
constexpr uint32_t resolveFormat = 0x28280106;
constexpr uint32_t width = 2560, height = 1440;
bool rendererType(uint32_t renderer) {
    const uint32_t table = memory->read32(renderer);
    // The actual title constructs derived renderer 0x82225940, whose table
    // 0x8214C3AC retains these native-renderer methods from base 0x821601C4.
    return (table == 0x821601c4 || table == 0x8214c3ac) &&
        memory->read32(table+244) == 0x823472d0 && memory->read32(table+280) == 0x82348a40 &&
        memory->read32(table+592) == 0x8237e0f8 && memory->read32(table+604) == 0x8234af50;
}

uint32_t invoke(PPCContext& ctx, uint8_t* base, uint32_t address, std::initializer_list<uint32_t> arguments) {
    if (address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE || (address & 3))
        fail("Title device CPU callback lies outside compiled code");
    auto function = PPC_LOOKUP_FUNC(base,address);
    if (!function || arguments.size() > 8) fail("Title device callback has no native AOT implementation");
    PPCRegister* registers[]{&ctx.r3,&ctx.r4,&ctx.r5,&ctx.r6,&ctx.r7,&ctx.r8,&ctx.r9,&ctx.r10};
    for (auto* value : registers) value->u64 = 0;
    size_t index = 0;
    for (uint32_t value : arguments) registers[index++]->u64 = value;
    function(ctx,base);
    return ctx.r3.u32;
}
uint32_t halfword(uint32_t address) {
    if (!memory->valid(address,2)) fail("Inaccessible title device viewport dimensions");
    return (uint32_t(memory->base()[address]) << 8) | memory->base()[address+1];
}
struct GuestCallFrame {
    PPCContext& ctx;
    uint64_t stack, link;
    explicit GuestCallFrame(PPCContext& context) : ctx(context), stack(context.r1.u64), link(context.lr) {
        if (ctx.r1.u32 < 512 || !memory->valid(ctx.r1.u32-512,512))
            fail("Title native device initialization has no guest call frame");
        ctx.r1.u64 = ctx.r1.u32-512;
    }
    ~GuestCallFrame() { ctx.r1.u64 = stack; ctx.lr = link; }
};
void initializeDevice(PPCContext& ctx, uint8_t* base) {
    const uint32_t renderer = ctx.r3.u32;
    if (!memory || base != memory->base() || !memory->valid(renderer,0x56bcc))
        fail("Invalid original title renderer storage");
    if (!rendererType(renderer)) fail("Unverified title native renderer CPU type");
    if (memory->read32(deviceGlobal)) fail("Repeated title native device initialization");
    GuestCallFrame frame(ctx);
    // The original routine selects a fixed Xbox mode from a table and then
    // computes EDRAM tile placement by dereferencing GPU resource words. Native
    // D3D9 owns these allocations. Use its actual 1440p, no-MSAA policy and the
    // original CPU resolution setter to retain all UI/aspect/projection fields.
    if (!invoke(ctx,base,0x8224d528,{width,height})) fail("Original CPU resolution update failed");
    memory->base()[renderer+0x56b7f] = 0; // Native D24S8, rather than floating Xbox depth.
    memory->base()[renderer+0x56ba9] = 0; // Native resources do not use the EDRAM tiled frame path.
    memory->write32(renderer+0x56564,colorFormat);
    memory->write32(renderer+0x56568,depthFormat);
    memory->write32(renderer+0x56bac,5); // Original engine tile-mode preference, inactive above.
    memory->write32(renderer+0x56af8,0);
    memory->write32(renderer+0x56b88,1); // Original requested preference; actual MSAA below is none.
    memory->write32(renderer+0x56b84,0);
    // Original CPU presentation descriptor +0x56B00 retains the title's
    // windowed mode and default interval preference. The native backend owns
    // the actual presentation interval selected from the display refresh rate.
    const std::array<uint32_t,16> presentation{width,height,colorFormat,1,0,0,1,0,1,0,depthFormat,0,0,0,1,1};
    for (uint32_t index = 0; index < presentation.size(); ++index)
        memory->write32(renderer+0x56b00+index*4,presentation[index]);
    const uint32_t status = invoke(ctx,base,0x823d25d8,{0,1,0,1,renderer+0x56b00,deviceGlobal});
    if (int32_t(status) < 0 || !memory->read32(deviceGlobal)) fail("Native title D3D9 device creation failed");
    memory->write32(renderer+0x11954,halfword(0x82a74220));
    memory->write32(renderer+0x11958,halfword(0x82a74224));
    invoke(ctx,base,0x823befa8,{memory->read32(deviceGlobal),0,32,96});
    // These are the actual frame resolve/presentation textures. D3D9 must
    // create them with render-target usage; they are never tiled GPU headers.
    const uint32_t first = invoke(ctx,base,0x823d6bb8,{width,height,1,1,1,resolveFormat,0,3});
    if (!first) fail("Native title first resolve texture creation failed");
    memory->write32(renderer+0x56bb8,first);
    const uint32_t second = invoke(ctx,base,0x823d6bb8,{width,height,1,1,1,resolveFormat,0,3});
    if (!second) fail("Native title second resolve texture creation failed");
    memory->write32(renderer+0x56bbc,second);
    // Preserve the original CPU format-string assignment and owner-thread
    // callback. Neither operation consumes Xbox GPU object fields.
    invoke(ctx,base,0x825d8808,{renderer+0x564d4,0x82146460,48});
    const uint32_t owner = invoke(ctx,base,0x823e1fc0,{});
    const uint32_t table = memory->read32(renderer);
    invoke(ctx,base,memory->read32(table+592),{renderer,owner});
    std::fprintf(stderr,"[native renderer] original engine CPU mode 2560x1440, native D24S8/no MSAA, two real presentation textures\n");
    ctx.r3.u64 = 0;
}
void startFrame(PPCContext& ctx, uint8_t* base) {
    const uint32_t renderer = ctx.r3.u32, view = ctx.r4.u32;
    if (!memory || base != memory->base() || !memory->valid(renderer,0x56bcc) ||
        !rendererType(renderer))
        fail("Invalid native title frame renderer");
    if (memory->base()[renderer+0x56ba8])
        fail("Original title postprocess frame path requires native shader/resource integration");
    if (memory->base()[renderer+0x56ba9])
        fail("Native title frame cannot consume Xbox EDRAM tile configuration");
    prepareTitleSceneResolution(renderer);
    prepareTitleFrame();
    GuestCallFrame frame(ctx);
    // Original +0x56B8C skips target/letterbox setup only. Its branch lands at
    // 0x82348CAC, before per-view viewport selection and camera clears.
    if (!memory->base()[renderer+0x56b8c]) {
        const uint32_t device = memory->read32(deviceGlobal);
        invoke(ctx,base,0x823c6fc0,{device,0,memory->read32(renderer+0x5655c)});
        invoke(ctx,base,0x823c6ce8,{device,memory->read32(renderer+0x56560)});
        invoke(ctx,base,0x82348460,{renderer}); // Original letterbox viewport and clears.
    }
    bool selected = false;
    if (view) {
        const uint32_t rectangle = invoke(ctx,base,0x82242660,{view});
        if (invoke(ctx,base,0x822e5df0,{rectangle,0x82bac294})) {
            if (!memory->valid(rectangle,16)) fail("Inaccessible original normalized viewport");
            float values[4];
            for (unsigned index = 0; index < 4; ++index)
                values[index] = std::bit_cast<float>(memory->read32(rectangle+index*4));
            if (!std::isfinite(values[0]) || !std::isfinite(values[1]) || !std::isfinite(values[2]) ||
                !std::isfinite(values[3]) || values[0] < 0 || values[1] < 0 || values[2] > 1 ||
                values[3] > 1 || values[0] >= values[2] || values[1] >= values[3])
                fail("Unverified original normalized viewport bounds");
            const uint32_t fullWidth = invoke(ctx,base,0x82248548,{}), fullHeight = invoke(ctx,base,0x82248558,{});
            const uint32_t activeWidth = invoke(ctx,base,0x821a98b0,{}), activeHeight = invoke(ctx,base,0x821a98a0,{});
            if (activeWidth > fullWidth || activeHeight > fullHeight) fail("Invalid original active viewport dimensions");
            const float xProduct = float(activeWidth)*values[0], yProduct = float(activeHeight)*values[1];
            const uint32_t x = uint32_t(float((fullWidth-activeWidth)/2)+xProduct);
            const uint32_t y = uint32_t(float((fullHeight-activeHeight)/2)+yProduct);
            const uint32_t viewportWidth = uint32_t(float(activeWidth)*(values[2]-values[0]));
            const uint32_t viewportHeight = uint32_t(float(activeHeight)*(values[3]-values[1]));
            ctx.f1.f64 = 0; ctx.f2.f64 = 1;
            invoke(ctx,base,memory->read32(memory->read32(renderer)+604),{renderer,viewportWidth,viewportHeight,x,y});
            selected = true;
        }
    }
    if (!selected) invoke(ctx,base,0x8234a708,{renderer}); // Original default viewport CPU calculation.
    invoke(ctx,base,0x823486e0,{renderer,view}); // Original camera-dependent clear rectangles, color and depth.
    memory->base()[renderer+0x56b8c] = 1;
    ctx.r3.u64 = 1;
}
void createRenderTarget(PPCContext& ctx, uint8_t* base) {
    // Caller 822AE7A0 supplies resource flags in r7; the original constructor
    // overwrites r7 from owner+24 at 8238FA64 without retaining that argument.
    const uint32_t target = ctx.r3.u32, requestedWidth = ctx.r4.u32,
        requestedHeight = ctx.r5.u32, type = ctx.r6.u32, disableSamples = ctx.r8.u32;
    if (!memory || base != memory->base() || !memory->valid(target,100) ||
        memory->read32(target) != 0x82161380 ||
        memory->read32(0x82161380+52) != 0x8238efe8)
        fail("Unverified original title render-target CPU owner");
    const uint32_t renderer = memory->read32(0x82baa92c);
    if (!memory->valid(renderer,0x56bcc) || !rendererType(renderer) ||
        memory->base()[renderer+0x56ba9] || !memory->read32(deviceGlobal))
        fail("Title render target requires the initialized native non-EDRAM renderer");
    // Original type 1 unconditionally disables MSAA before selecting D24S8 at
    // 8238FB14..8238FB24, regardless of the ordinary renderer preference.
    const bool depthTarget = type == 1;
    const uint32_t samples = depthTarget || disableSamples ? 0 : memory->read32(renderer+0x56b88);
    std::fprintf(stderr,"[native render target] owner=%08X type=%u requested=%ux%u samples=%u disable_samples=%u\n",
        target,type,requestedWidth,requestedHeight,samples,disableSamples);
    // Types 0/3 retain their existing ordinary color allocation path. Type 1
    // supports only the original non-floating D24S8 policy: native depth is a
    // real one-level texture plus independently owned level 0 depth view.
    if ((type != 0 && type != 1 && type != 3) || disableSamples > 1 || samples > 2 ||
        !requestedWidth || !requestedHeight || requestedWidth > 8192 || requestedHeight > 8192 ||
        memory->read32(renderer+0x56564) != colorFormat ||
        memory->read32(renderer+0x56568) != depthFormat ||
        (depthTarget && (memory->base()[renderer+0x56b7f] || memory->read32(target+4) == 13)))
        fail("Unverified original title render-target type, format, dimensions or sample policy");
    GuestCallFrame frame(ctx);
    auto cleanup = [&] { invoke(ctx,base,0x8238efe8,{target,1}); };
    cleanup(); // Original virtual +52 releases every previously owned resource.
    memory->write32(target+4,type);
    memory->write32(target+8,requestedWidth);
    memory->write32(target+12,requestedHeight);
    memory->write32(target+24,memory->read32(target+24)&~5u);
    memory->write32(target+84,samples);
    // 999 is the original full-surface branch in all bind/resolve methods.
    // +92 denotes Xbox EDRAM placement bytes only. Native allocation has none;
    // neither a GPU header nor a pretend native-memory bytecount is produced.
    memory->write32(target+88,999);
    memory->write32(target+92,0);
    memory->write32(target+96,depthTarget ? depthFormat : colorFormat);
    bool complete = false;
    try {
        if (depthTarget) {
            // The helper publishes its independently owned tokens atomically
            // and leaves this original owner untouched on allocation failure.
            // Record both results before the matching ordinary color request,
            // so original cleanup releases view at +72 before texture at +28 on any
            // subsequent nullable or exceptional failure. No Xbox header or
            // depth resolve copy is invented; original Begin continues in AOT.
            const auto pair = createTitleDepthTargetResources(target);
            memory->write32(target+28,pair.texture);
            memory->write32(target+72,pair.depthSurface);
            if (pair.texture && pair.depthSurface) {
                const uint32_t color = invoke(ctx,base,0x823d6cd8,
                    {requestedWidth,requestedHeight,colorFormat,0,0});
                memory->write32(target+44,color);
                complete = color != 0;
            }
        } else {
            const uint32_t texture = invoke(ctx,base,0x823d6bb8,
                {requestedWidth,requestedHeight,1,1,1,colorFormat,0,3});
            memory->write32(target+28,texture);
            if (texture) {
                const uint32_t color = invoke(ctx,base,0x823d6cd8,
                    {requestedWidth,requestedHeight,colorFormat,samples,0});
                memory->write32(target+44,color);
                if (color) {
                    const uint32_t depth = invoke(ctx,base,0x823d6cd8,
                        {requestedWidth,requestedHeight,depthFormat,samples,0});
                    memory->write32(target+72,depth);
                    complete = depth != 0;
                }
            }
        }
    } catch (...) {
        cleanup();
        throw;
    }
    if (!complete) cleanup(); // Preserve the original nullable allocation result.
    ctx.r3.u64 = complete ? 1 : 0;
}
}

void updateTitleRenderResolution(PPCContext& context, uint8_t* base, uint32_t renderer,
    uint32_t newWidth, uint32_t newHeight) {
    if (!memory || base != memory->base() || !memory->valid(renderer,0x56bcc) ||
        !rendererType(renderer) || newWidth < 320 || newHeight < 200 || newWidth > 8192 || newHeight > 8192)
        fail("Unverified original CPU render resolution update");
    // Run the original resolution/aspect notification on the current title
    // thread, with an isolated PPC call frame and complete caller preservation.
    PPCContext callback = context;
    struct RestoreFpscr {
        PPCContext& caller;
        ~RestoreFpscr() { caller.fpscr.setcsr(caller.fpscr.csr); }
    } restore{context};
    GuestCallFrame frame(callback);
    if (!invoke(callback,base,0x8224d528,{newWidth,newHeight}))
        fail("Original CPU render resolution update failed");
    memory->write32(renderer+0x11954,halfword(0x82a74220));
    memory->write32(renderer+0x11958,halfword(0x82a74224));
    memory->write32(renderer+0x56b00,newWidth);
    memory->write32(renderer+0x56b04,newHeight);
    memory->base()[renderer+0x56b8c] = 0; // Next original Begin recomputes viewport/letterbox.
}
}

PPC_FUNC(sub_82347820) { Doritos::Graphics::initializeDevice(ctx,base); }
PPC_FUNC(sub_82348A40) { Doritos::Graphics::startFrame(ctx,base); }
PPC_FUNC(sub_8238FA28) { Doritos::Graphics::createRenderTarget(ctx,base); }
