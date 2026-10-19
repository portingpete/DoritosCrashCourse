#include "runtime/native/runtime.h"
#include "renderer/native/title_bridge.h"
#include <array>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <vector>
#include <new>
#include <cstring>

PPC_FUNC(sub_8238FA28);
PPC_FUNC(testUpdateResolution);
PPC_FUNC(testReleaseTarget);
PPC_FUNC(testCreateTexture);
PPC_FUNC(testCreateSurface);
PPCFuncMapping PPCFuncMappings[]{
    {0x8224d528,testUpdateResolution},
    {0x8238efe8,testReleaseTarget}, {0x823d6bb8,testCreateTexture},
    {0x823d6cd8,testCreateSurface}, {0,nullptr}};

// Isolate the original CPU ownership boundary. Real D3D9 allocation, clear and
// MSAA resolve are checked by NativeGraphicsHardwareContract and the title.
namespace Doritos::Graphics {
void prepareTitleFrame() { throw std::runtime_error("unexpected frame operation in RT contract"); }
void prepareTitleSceneResolution(uint32_t) { throw std::runtime_error("unexpected scene registration in RT contract"); }
}
using namespace Doritos::Native;
namespace {
constexpr uint32_t colorFormat = 0x28280186, depthFormat = 0x2d200196;
uint32_t owner = 0, renderer = 0, requestedWidth = 0, requestedHeight = 0, samples = 0;
uint32_t targetType = 0;
unsigned creations = 0, releases = 0, cleanups = 0, failAt = 0, throwAt = 0, badAllocAt = 0;
unsigned depthPairCalls = 0;
std::map<uint32_t,uint32_t> resources;
std::map<uint32_t,uint32_t> viewParents;
std::vector<uint32_t> releasedKinds;
uint32_t cpuResolutionWidth = 0, cpuResolutionHeight = 0;
uint32_t expectedResolutionWidth = 0, expectedResolutionHeight = 0;
uint32_t cpuSetterStack = 0;
uint64_t cpuSetterLink = 0;
bool failNextCpuSetter = false;
unsigned cpuSetterCalls = 0;
void require(bool condition, const char* detail) { if (!condition) throw std::runtime_error(detail); }
uint32_t create(uint32_t kind) {
    ++creations;
    if (creations == throwAt) throw NativeFailure("injected genuine allocation failure");
    if (creations == badAllocAt) throw std::bad_alloc();
    if (creations == failAt) return 0;
    const uint32_t token = memory->allocate(4096);
    // Deliberately opaque: no original Xbox GPU size/header is available.
    memory->write32(token+44,0xdeadc0de);
    resources.emplace(token,kind);
    return token;
}
void release(uint32_t token) {
    const auto found = resources.find(token);
    require(found != resources.end(),"each returned resource reference remains owned until its release");
    const uint32_t kind = found->second;
    if (const auto view = viewParents.find(token); view != viewParents.end()) {
        require(resources.contains(view->second),"independent level0 view releases before the owned texture reference");
        viewParents.erase(view);
    } else if (kind == 3) {
        for (const auto& [view,parent] : viewParents)
            require(parent != token,"original texture cleanup must follow every retained depth view");
    }
    require(resources.erase(token) == 1 && memory->release(token),"owned token releases exactly once");
    releasedKinds.push_back(kind);
    ++releases;
}
}
namespace Doritos::Graphics {
// Stand-in for the independently hardware-tested COM allocation boundary.
// This fixture checks high CPU metadata and ownership, not native rendering.
TitleDepthTargetResources createTitleDepthTargetResources(uint32_t suppliedOwner) {
    require(suppliedOwner == owner,"depth pair belongs to the original target owner");
    std::array<uint8_t,100> before{};
    std::memcpy(before.data(),memory->base()+owner,before.size());
    const auto description = titleDepthTargetDescription(before);
    require(description.width == requestedWidth && description.height == requestedHeight &&
        !memory->read32(owner+44),"depth pair receives exact requested dimensions and a cleaned ordinary companion");
    ++depthPairCalls;
    uint32_t texture = 0, view = 0;
    try {
        texture = create(3);
        if (!texture) return {};
        view = create(5);
        if (!view) { release(texture); return {}; }
        viewParents.emplace(view,texture);
        require(!std::memcmp(before.data(),memory->base()+owner,before.size()),
            "transaction publishes neither output into the original CPU owner");
        return {texture,view};
    } catch (const std::bad_alloc&) {
        if (view) release(view);
        if (texture) release(texture);
        return {};
    } catch (...) {
        if (view) release(view);
        if (texture) release(texture);
        throw;
    }
}
}
PPC_FUNC(testReleaseTarget) {
    require(ctx.r3.u32 == owner && ctx.r4.u32 == 1,"original RT release-all ABI");
    ++cleanups;
    for (const uint32_t offset : {72u,44u,32u,48u,52u,56u,60u,64u,68u,28u,36u,80u,76u,40u}) {
        const uint32_t token = memory->read32(owner+offset);
        if (token) {
            release(token);
        }
        memory->write32(owner+offset,0);
    }
    memory->write32(owner+4,14); memory->write32(owner+88,999);
}
PPC_FUNC(testCreateTexture) {
    require(ctx.r3.u32 == requestedWidth && ctx.r4.u32 == requestedHeight &&
        ctx.r5.u32 == 1 && ctx.r6.u32 == 1 && ctx.r7.u32 == 1 &&
        ctx.r8.u32 == colorFormat && !ctx.r9.u32 && ctx.r10.u32 == 3,
        "actual requested full-size 2D render-target texture ABI");
    ctx.r3.u64 = create(3);
}
PPC_FUNC(testCreateSurface) {
    require(ctx.r3.u32 == requestedWidth && ctx.r4.u32 == requestedHeight &&
        ctx.r6.u32 == samples && !ctx.r7.u32,
        "actual full-size native surface and sample policy has no Xbox placement pointer");
    require(ctx.r5.u32 == (targetType == 1 || creations == 1 ? colorFormat : depthFormat),
        "color allocation precedes its matching depth companion");
    ctx.r3.u64 = create(ctx.r5.u32 == depthFormat ? 5 : 4);
}
PPC_FUNC(testUpdateResolution) {
    ++cpuSetterCalls;
    require(ctx.r3.u32 == expectedResolutionWidth && ctx.r4.u32 == expectedResolutionHeight,
        "original CPU resolution setter receives the exact requested width and height");
    cpuSetterStack = ctx.r1.u32;
    cpuSetterLink = ctx.lr;
    if (failNextCpuSetter) {
        failNextCpuSetter = false;
        // Model the original callback updating one global before it rejects a
        // later step. The renderer wrapper must let the backend's old-size
        // callback restore this CPU state after the GPU transaction fails.
        cpuResolutionWidth = ctx.r3.u32;
        ctx.r3.u64 = 0;
    } else {
        cpuResolutionWidth = ctx.r3.u32;
        cpuResolutionHeight = ctx.r4.u32;
        ctx.r3.u64 = 1;
    }
    // Exercise the wrapper's isolated call frame: volatile input/output
    // registers and the callback stack/link can all be clobbered by PPC code.
    ctx.r4.u64 = 0x44444444;
    ctx.r5.u64 = 0x55555555;
    ctx.r10.u64 = 0xaaaaaaaa;
    ctx.r1.u64 = 0xdeadbeef;
    ctx.lr = 0xfeedface;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        const auto image = loadOriginalXex(argv[1]);
        Memory guest(image); memory = &guest;
        require(guest.read32(0x82225904) == 0x3c600005 && guest.read32(0x82225908) == 0x60637960,
            "Original derived renderer allocation owns0x57960 bytes beyond checked cull prefix");
        require(guest.read32(0x822ac6f8) == 0x80630078 && guest.read32(0x822ac6fc) == 0x4e800020,
            "Original cull camera getter only reads+120 and returns without renderer invalidation");
        require(guest.read32(0x82161380) == 0x8238fa28 &&
            guest.read32(0x82161380+4) == 0x82390020 &&
            guest.read32(0x82161380+52) == 0x8238efe8 &&
            guest.read32(0x82161380+8) == 0x82390178,
            "actual original RT creation, cleanup and resolve CPU interfaces");
        const std::array<std::pair<uint32_t,uint32_t>,23> originalWords{{
            {0x822ae79c,0x811e0078}, {0x822ae7a0,0x80ff0060},
            {0x822ae7a8,0x80de0070}, {0x822ae83c,0x4e800421},
            {0x822ae840,0x5469063e}, {0x8238fa64,0x80ff0018},
            {0x8238faf0,0x419a0024}, {0x8238fb14,0x3d602d20},
            {0x8238fb18,0x92ff0054}, {0x8238fb1c,0x38e00002},
            {0x8238fb20,0x616a0196}, {0x8238fb24,0x915f0060},
            {0x8238fb80,0x48047039}, {0x8238fb84,0x907f001c},
            {0x82390080,0x80bf002c}, {0x82390098,0x48036f29},
            {0x8239009c,0x809f0048}, {0x823900b0,0x48036c39},
            {0x823900c4,0x2b0b03e7}, {0x823900c8,0x419a009c},
            {0x8238effc,0x80830048}, {0x8238f00c,0x4bfeedb5},
            {0x8238f088,0x4bfeedd1}
        }};
        for (const auto& [address,word] : originalWords)
            require(guest.read32(address) == word,
                "encrypted original caller/type1 D24S8/no-MSAA, Begin bind and view-before-texture cleanup byte oracle");
        // Independent original-image oracle for RVMBatch's native buffer
        // mode. Both allocations pass r6=1 through their actual virtual+4
        // entries; bind-time flush tests byte50 and clears only dirty byte49.
        const std::array<std::pair<uint32_t,uint32_t>,37> batchBufferWords{{
            // The master allocates248B, then its actual constructor installs
            // this class vtable and the indexed batch's virtual+12 draw slot.
            {0x8231af74,0x386000f8}, {0x8231af78,0x481b7639},
            {0x8231af94,0x480003f5}, {0x8231b3e0,0x3d608216},
            {0x8231b3f0,0x392be29c}, {0x8231b400,0x913f0000},
            {0x8215e2a8,0x8231bb28},
            {0x821605a0,0x823bb640}, {0x821605bc,0x823bb6c8},
            {0x8231b6fc,0x809e00d0}, {0x8231b700,0x4bf8d6a9},
            {0x8231b778,0xa17e00ca}, {0x8231b77c,0xa15f0012},
            {0x8231b780,0x7c8b51d6}, {0x8231b784,0x4bf8d625},
            {0x822a8dec,0x38c00001}, {0x822a8df0,0x7fc5f378},
            {0x822a8df8,0x812a0004}, {0x822a8e04,0x4e800421},
            {0x823bb654,0x98c30032}, {0x823bb668,0x409a0038},
            {0x823bb6a0,0x917f0014}, {0x823bb6ac,0x997f0031},
            {0x823bb6e0,0x98c30032}, {0x823bb6f0,0x409a003c},
            {0x823bb72c,0x90bf0014}, {0x823bb738,0x997f0031},
            {0x82386698,0x89630031}, {0x823866d0,0x897f0032},
            {0x823866d8,0x409a0054}, {0x8238672c,0x39600000},
            {0x82386730,0x997f0031}, {0x82386b00,0x89630031},
            {0x82386b38,0x897f0032}, {0x82386b40,0x409a0058},
            {0x82386b98,0x39600000}, {0x82386b9c,0x997f0031}
        }};
        for (const auto& [address,word] : batchBufferWords)
            require(guest.read32(address) == word,
                "encrypted original RVMBatch allocation and clean direct-native buffer mode byte oracle");
        // Original multilayer material selects the same skin feature for a
        // nonempty renderer palette, binds its immutable VS/PS families, and
        // invokes the incoming RVMBatch's actual virtual+12 draw entry.
        const std::array<std::pair<uint32_t,uint32_t>,24> batchMultilayerWords{{
            {0x82157958,0x6d756c74}, {0x8215795c,0x696c6179},
            {0x82157960,0x65722f76}, {0x82157968,0x6d756c74},
            {0x822d0614,0x7c902378}, {0x822d0678,0x612757e0},
            {0x822d0688,0x7cbf382e}, {0x822d0690,0x2f050000},
            {0x822d0698,0x40990014}, {0x822d06a0,0x814b0008},
            {0x822d06a4,0x61490040}, {0x822d06a8,0x912b0008},
            {0x822d1424,0x3d608215}, {0x822d142c,0x3aeb7958},
            {0x822d1464,0x3d608215}, {0x822d1468,0x3b2b7968},
            {0x822d14cc,0x814b0244}, {0x822d14d4,0x4e800421},
            {0x822d14e4,0x81090248}, {0x822d14ec,0x4e800421},
            {0x822d14f0,0x80f00000}, {0x822d14f4,0x7e038378},
            {0x822d14f8,0x80c7000c}, {0x822d1500,0x4e800421}
        }};
        for (const auto& [address,word] : batchMultilayerWords)
            require(guest.read32(address) == word,
                "encrypted original RVMBatch multilayer skin-shader selection and draw byte oracle");
        PPCContext ctx{}; guest.initializeThread(ctx,0x100000); currentContext = &ctx;
        owner = guest.allocate(4096); renderer = guest.allocate(0x57000);
        guest.write32(owner,0x82161380); guest.write32(owner+100,0x12345678);
        guest.write32(renderer,0x8214c3ac);
        guest.write32(renderer+0x56564,colorFormat); guest.write32(renderer+0x56568,depthFormat);
        guest.write32(0x82baa92c,renderer); guest.write32(0x82bac23c,0xd9000001);
        auto construct = [&](uint32_t type, uint32_t disable = 0, uint32_t resourceFlags = 0xabcd) {
            creations = 0;
            targetType = type;
            ctx.r3.u64 = owner; ctx.r4.u64 = requestedWidth; ctx.r5.u64 = requestedHeight;
            ctx.r6.u64 = type; ctx.r7.u64 = resourceFlags; ctx.r8.u64 = disable; ctx.r9.u64 = 0xbcde;
            ctx.r31.u64 = 0x10203040; ctx.lr = 0x822ae840;
            const uint64_t stack = ctx.r1.u64;
            sub_8238FA28(ctx,guest.base());
            require(ctx.r1.u64 == stack && ctx.lr == 0x822ae840 && ctx.r31.u64 == 0x10203040,
                "RT adapter restores caller stack/link and preserves nonvolatile registers");
            return ctx.r3.u32;
        };
        auto empty = [&] { require(resources.empty() && viewParents.empty() && !guest.read32(owner+28) &&
            !guest.read32(owner+44) && !guest.read32(owner+72),"failed construction leaves no owned resources"); };
        requestedWidth = 2560; requestedHeight = 1440; samples = 1;
        guest.write32(renderer+0x56b88,1); guest.write32(owner+24,0xa5);
        require(construct(0) == 1 && creations == 3 && resources.size() == 3,
            "ordinary color RT has three genuine allocation results");
        require(guest.read32(owner+4) == 0 && guest.read32(owner+8) == requestedWidth &&
            guest.read32(owner+12) == requestedHeight && guest.read32(owner+24) == 0xa0 &&
            guest.read32(owner+84) == 1 && guest.read32(owner+88) == 999 && !guest.read32(owner+92) &&
            guest.read32(owner+96) == colorFormat && guest.read32(owner+100) == 0x12345678,
            "original CPU metadata retained without fabricated EDRAM bytes or out-of-bounds writes");
        requestedWidth = 1280; requestedHeight = 720; samples = 0;
        const unsigned beforeRelease = releases;
        require(construct(3,1) == 1 && releases == beforeRelease+3 && resources.size() == 3 &&
            guest.read32(owner+84) == 0 && guest.read32(owner+4) == 3,
            "recreation releases previous references and original skip-MSAA overrides preference");
        requestedWidth = 768; requestedHeight = 512; samples = 0;
        guest.write32(renderer+0x56b88,2); guest.write32(owner+24,0xa5);
        require(construct(1) == 1 && creations == 3 && resources.size() == 3 && depthPairCalls == 1,
            "type1 owns a separately returned depth texture/view and matching ordinary color companion");
        const uint32_t depthTexture = guest.read32(owner+28), depthView = guest.read32(owner+72);
        require(depthTexture && depthView && depthTexture != depthView &&
            viewParents.at(depthView) == depthTexture && guest.read32(owner+44) &&
            guest.read32(owner+4) == 1 && guest.read32(owner+8) == requestedWidth &&
            guest.read32(owner+12) == requestedHeight && guest.read32(owner+24) == 0xa0 &&
            !guest.read32(owner+84) && guest.read32(owner+88) == 999 && !guest.read32(owner+92) &&
            guest.read32(owner+96) == depthFormat && guest.read32(owner+100) == 0x12345678,
            "type1 retains original full-owner CPU fields, requested extent and forced sample0 without Xbox headers");
        releasedKinds.clear();
        require(construct(1,1) == 1 && releasedKinds == std::vector<uint32_t>{5,4,3} &&
            depthPairCalls == 2 && viewParents.size() == 1,
            "type1 recreation follows original view, color, texture cleanup and preserves independent ownership");
        requestedWidth = requestedHeight = 2048;
        releasedKinds.clear();
        require(construct(1,1,4) == 1 && creations == 3 && depthPairCalls == 3 &&
            releasedKinds == std::vector<uint32_t>{5,4,3} && resources.size() == 3 && viewParents.size() == 1 &&
            guest.read32(owner+8) == 2048 && guest.read32(owner+12) == 2048 && !guest.read32(owner+84) &&
            guest.read32(owner+96) == depthFormat && guest.read32(owner+88) == 999 &&
            !guest.read32(owner+92),
            "observed LR822AE840 type1 2048x2048/disable1/resourceFlags4 owns real depth resources at exact requested extent");
        for (unsigned stage = 1; stage <= 3; ++stage) {
            failAt = stage;
            require(construct(1) == 0 && creations == stage && guest.read32(owner+4) == 14,
                "nullable depth texture, view or color allocation returns original failure after complete cleanup");
            empty();
        }
        failAt = 0;
        for (unsigned stage : {1u,2u}) {
            badAllocAt = stage;
            require(construct(1) == 0,"helper host allocation pressure remains a nullable original target failure");
            empty();
        }
        badAllocAt = 0;
        for (unsigned stage : {1u,2u,3u}) {
            throwAt = stage;
            bool stopped = false;
            const uint64_t savedStack = ctx.r1.u64;
            try { construct(1); } catch (const NativeFailure&) { stopped = true; }
            require(stopped && ctx.r1.u64 == savedStack && ctx.lr == 0x822ae840,
                "unavailable or invalid native operation remains exceptional while all owned references roll back");
            empty();
        }
        throwAt = 0;
        guest.write32(renderer+0x56b88,1);
        for (unsigned stage = 1; stage <= 3; ++stage) {
            failAt = stage;
            require(construct(0,1) == 0 && creations == stage && guest.read32(owner+4) == 14,
                "each genuine NULL allocation returns original failure and release-all metadata");
            empty();
        }
        failAt = 0; throwAt = 3;
        const uint64_t throwingStack = ctx.r1.u64;
        bool rejected = false;
        try { construct(0,1); } catch (const NativeFailure&) { rejected = true; }
        require(rejected && ctx.r1.u64 == throwingStack && ctx.lr == 0x822ae840,
            "exceptional native allocation failure restores caller context");
        empty(); throwAt = 0;
        auto reject = [&](uint32_t type, uint32_t disable = 0) {
            const unsigned before = cleanups;
            const unsigned beforePair = depthPairCalls;
            std::array<uint8_t,100> beforeOwner{};
            std::memcpy(beforeOwner.data(),guest.base()+owner,beforeOwner.size());
            bool stopped = false;
            try { construct(type,disable); } catch (const NativeFailure&) { stopped = true; }
            require(stopped && !creations && cleanups == before && depthPairCalls == beforePair &&
                !std::memcmp(beforeOwner.data(),guest.base()+owner,beforeOwner.size()),
                "unverified policies fail before release or allocation");
        };
        for (uint32_t type : {2u,4u,6u,11u,13u,99u}) reject(type);
        const uint32_t sharedTexture = create(3);
        guest.write32(owner+4,13); guest.write32(owner+28,sharedTexture);
        reject(1);
        require(resources.contains(sharedTexture) && guest.read32(owner+28) == sharedTexture,
            "existing shared-cache type13 retains its external texture without ownership conversion or cleanup");
        guest.write32(owner+28,0); guest.write32(owner+4,14); release(sharedTexture);
        reject(1,2);
        guest.base()[renderer+0x56b7f] = 1; reject(1);
        guest.base()[renderer+0x56b7f] = 0;
        reject(0,2);
        requestedWidth = 0; reject(0);
        reject(1);
        requestedWidth = 8193; reject(0);
        reject(1);
        requestedWidth = 1280;
        requestedHeight = 0; reject(1);
        requestedHeight = 8193; reject(1);
        requestedHeight = 720;
        guest.write32(renderer+0x56b88,3); reject(0);
        guest.write32(renderer+0x56b88,1);
        guest.base()[renderer+0x56ba9] = 1; reject(0);
        reject(1);
        guest.base()[renderer+0x56ba9] = 0;
        guest.write32(renderer+0x56568,0x1a220197); reject(0);
        reject(1);
        guest.write32(renderer+0x56568,depthFormat);
        guest.write32(renderer+0x56564,0x28280106); reject(1);
        guest.write32(renderer+0x56564,colorFormat);
        guest.write32(renderer,0x8214c3b0); reject(1);
        guest.write32(renderer,0x8214c3ac);
        guest.write32(owner,0x82161384); reject(0);
        reject(1);

        // The in-game Apply callback updates the original title's CPU mode on
        // the same presentation frame as the native resources. Exercise the
        // original setter through title_device.cpp with a synthetic AOT entry:
        // success publishes dimensions and invalidates the cached viewport;
        // a partial setter failure leaves the old renderer metadata intact so
        // the backend can call the old-size callback to restore CPU state.
        cpuResolutionWidth = 320; cpuResolutionHeight = 240;
        guest.write32(renderer+0x56b00,320); guest.write32(renderer+0x56b04,240);
        guest.base()[renderer+0x56b8c] = 1;
        ctx.lr = 0x12345678;
        ctx.r3.u64 = 0x11111111; ctx.r4.u64 = 0x22222222;
        ctx.r5.u64 = 0x33333333; ctx.r6.u64 = 0x44444444;
        ctx.r7.u64 = 0x55555555; ctx.r8.u64 = 0x66666666;
        ctx.r9.u64 = 0x77777777; ctx.r10.u64 = 0x88888888;
        const std::array<uint64_t,8> callerRegisters{
            ctx.r3.u64,ctx.r4.u64,ctx.r5.u64,ctx.r6.u64,ctx.r7.u64,ctx.r8.u64,ctx.r9.u64,ctx.r10.u64};
        const uint64_t callerStack = ctx.r1.u64, callerLink = ctx.lr;
        expectedResolutionWidth = 640; expectedResolutionHeight = 360;
        const unsigned settersBefore = cpuSetterCalls;
        Doritos::Graphics::updateTitleRenderResolution(ctx,guest.base(),renderer,640,360);
        require(cpuSetterCalls == settersBefore+1 && cpuResolutionWidth == 640 && cpuResolutionHeight == 360 &&
            guest.read32(renderer+0x56b00) == 640 && guest.read32(renderer+0x56b04) == 360 &&
            !guest.base()[renderer+0x56b8c] &&
            guest.read32(renderer+0x11954) == ((uint32_t(guest.base()[0x82a74220]) << 8) | guest.base()[0x82a74221]) &&
            guest.read32(renderer+0x11958) == ((uint32_t(guest.base()[0x82a74224]) << 8) | guest.base()[0x82a74225]),
            "successful original CPU resolution callback updates renderer dimensions and invalidates viewport cache");
        require(cpuSetterStack == callerStack-512 && cpuSetterLink == callerLink &&
            ctx.r1.u64 == callerStack && ctx.lr == callerLink &&
            std::array<uint64_t,8>{ctx.r3.u64,ctx.r4.u64,ctx.r5.u64,ctx.r6.u64,ctx.r7.u64,ctx.r8.u64,ctx.r9.u64,ctx.r10.u64} == callerRegisters,
            "original CPU resolution callback runs in a bounded PPC frame and preserves its caller registers");

        const uint32_t oldViewportWidth = guest.read32(renderer+0x56b00), oldViewportHeight = guest.read32(renderer+0x56b04);
        const uint32_t oldLetterboxWidth = guest.read32(renderer+0x11954), oldLetterboxHeight = guest.read32(renderer+0x11958);
        expectedResolutionWidth = 800; expectedResolutionHeight = 600; failNextCpuSetter = true;
        bool setterRejected = false;
        try { Doritos::Graphics::updateTitleRenderResolution(ctx,guest.base(),renderer,800,600); }
        catch (const NativeFailure&) { setterRejected = true; }
        require(setterRejected && cpuResolutionWidth == 800 && cpuResolutionHeight == 360 &&
            guest.read32(renderer+0x56b00) == oldViewportWidth && guest.read32(renderer+0x56b04) == oldViewportHeight &&
            guest.read32(renderer+0x11954) == oldLetterboxWidth && guest.read32(renderer+0x11958) == oldLetterboxHeight,
            "partially mutating original CPU setter failure does not publish new renderer-owned dimensions");
        require(ctx.r1.u64 == callerStack && ctx.lr == callerLink &&
            std::array<uint64_t,8>{ctx.r3.u64,ctx.r4.u64,ctx.r5.u64,ctx.r6.u64,ctx.r7.u64,ctx.r8.u64,ctx.r9.u64,ctx.r10.u64} == callerRegisters,
            "failed original CPU resolution callback still preserves its caller context");

        expectedResolutionWidth = 640; expectedResolutionHeight = 360;
        Doritos::Graphics::updateTitleRenderResolution(ctx,guest.base(),renderer,640,360);
        require(cpuResolutionWidth == 640 && cpuResolutionHeight == 360 &&
            guest.read32(renderer+0x56b00) == 640 && guest.read32(renderer+0x56b04) == 360 &&
            cpuSetterCalls == settersBefore+3,
            "backend's old-resolution callback rolls back a partial CPU setter mutation");
        printf("Native RT CPU metadata and allocation ownership contracts passed\n");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr,"Native RT contract failed: %s\n",error.what());
        return 1;
    }
}
