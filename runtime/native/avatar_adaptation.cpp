#include "avatar_adaptation.h"
#include "runtime.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

PPC_FUNC(sub_8223B988);
PPC_FUNC(sub_8223BB90);
PPC_FUNC(sub_822A5980);
PPC_FUNC(sub_822A56E0);
extern "C" PPC_FUNC(__imp__sub_8223B8F8);

namespace Doritos::Native {
namespace {
constexpr uint32_t assetManagerGlobal = 0x82ba9874;
constexpr uint32_t avatarMaster = 0x82161544;
constexpr uint32_t titleAvatarMaster = 0x8213d2a4;
constexpr uint32_t titleBoneMaster = 0x82161af4;
constexpr uint32_t titleBoneInstance = 0x82161c24;
constexpr uint32_t guiActivationCaller = 0x822b571c;
constexpr uint32_t fighterActivationCaller = 0x822889e0;
constexpr uint32_t titleFighter = 0x8214582c;
constexpr char boneResource[] = "BNMD:_avatar/cj_native.cfg";
std::atomic<uint64_t> redirectedInstances{0};
std::atomic<uint64_t> redirectedFighterInstances{0};
std::atomic<uint32_t> fighterActivationDiagnostics{0};
std::atomic<uint32_t> otherActivationDiagnostics{0};
std::atomic<uint32_t> boneResolveDiagnostics{0};

void span(uint32_t address, uint32_t bytes) {
    if (!memory || !memory->valid(address, bytes))
        throw NativeFailure("Avatar instance adaptation has an invalid guest buffer");
}

// Calls actual AOT resource operations with a temporary guest call frame.
// Restore the caller's register state after a completed helper; on failure its
// real helper context remains available for the runtime diagnostic.
uint64_t resourceCall(PPCContext& ctx, uint8_t* base, PPCFunc* entry,
    uint64_t r3, uint64_t r4, uint64_t r5 = 0) {
    const PPCContext saved = ctx;
    if (ctx.r1.u32 < 0x400) throw NativeFailure("Avatar instance adaptation has an invalid guest stack");
    const uint32_t callStack = ctx.r1.u32 - 0x200;
    span(callStack - 0x200, 0x400);
    ctx.r1.u64 = callStack;
    ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5;
    entry(ctx, base);
    const uint64_t result = ctx.r3.u64;
    ctx = saved;
    // FPSCR caches the host rounding/flush mode. Restoring that cache alone
    // could make a later generated operation skip its required host switch.
    ctx.fpscr.setcsr(ctx.fpscr.csr);
    return result;
}

std::string diagnosticString(uint32_t address) {
    if (!address) return "<null>";
    std::string result;
    for (uint32_t offset = 0; offset < 128; ++offset) {
        if (uint64_t(address) + offset > UINT32_MAX || !memory->valid(address + offset, 1))
            return "<invalid>";
        const unsigned char value = memory->base()[address + offset];
        if (!value) return result;
        result.push_back(value >= 32 && value < 127 ? char(value) : '?');
    }
    return "<unbounded>";
}

void diagnoseBoneResolve(PPCContext& ctx, uint8_t* base, uint64_t handle,
    uint32_t output, uint32_t name, uint32_t manager) {
    if (boneResolveDiagnostics.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    constexpr uint32_t typeCountAddress = 0x82ba9878, typeTableAddress = 0x82b5ad90;
    constexpr uint32_t typeFlagsAddress = 0x82baa938, boneManagerVtable = 0x82161f64;
    const bool countValid = memory && memory->valid(typeCountAddress, 4);
    const uint32_t count = countValid ? memory->read32(typeCountAddress) : UINT32_MAX;
    const auto input = diagnosticString(name);
    std::fprintf(stderr,
        "[NATIVE BONE RESOLVE STOP] handle=%016llX output=%08X input=%s manager=%08X typeCount=%u\n",
        static_cast<unsigned long long>(handle), output, input.c_str(), manager, count);
    // Original8223B9E0 searches this registered type table. Bound diagnostics
    // independently; unknown types keep their original behavior and are not called.
    // Original82398984 bounds registered resource groups to fewer than 128.
    if (countValid && count < 128 && memory->valid(typeTableAddress, count * 4)) {
        for (uint32_t slot = 0; slot < count; ++slot) {
            const uint32_t group = memory->read32(typeTableAddress + slot * 4);
            const uint32_t vtable = group && memory->valid(group, 4) ? memory->read32(group) : 0;
            const bool validVtable = vtable && memory->valid(vtable, 56);
            const uint32_t prefixGetter = validVtable ? memory->read32(vtable + 32) : 0;
            const uint32_t indexGetter = validVtable ? memory->read32(vtable + 52) : 0;
            std::fprintf(stderr,
                "[NATIVE BONE RESOLVE TYPE] slot=%u group=%08X vtable=%08X prefixGetter=%08X indexGetter=%08X\n",
                slot, group, vtable, prefixGetter, indexGetter);
            if (vtable != boneManagerVtable || prefixGetter != 0x822a5980 || indexGetter != 0x822a56e0)
                continue;
            // These exact shipped getters only return BNMD's literal and its
            // actual type index. They perform no loading, allocation or callbacks.
            const uint32_t prefix = uint32_t(resourceCall(ctx, base, sub_822A5980, group, 0));
            const uint64_t index = resourceCall(ctx, base, sub_822A56E0, group, 0);
            const bool flagsValid = index < 256 && memory->valid(typeFlagsAddress + uint32_t(index) * 4, 4);
            const uint32_t flags = flagsValid ? memory->read32(typeFlagsAddress + uint32_t(index) * 4) : 0;
            const auto typeName = diagnosticString(prefix);
            std::fprintf(stderr,
                "[NATIVE BONE RESOLVE BNMD] group=%08X prefix=%08X name=%s index=%llu flagsValid=%u flags=%08X creationAllowed=%u\n",
                group, prefix, typeName.c_str(), static_cast<unsigned long long>(index),
                flagsValid ? 1u : 0u, flags,
                flagsValid && ((flags & 0x20) || !(flags & 4)) ? 1u : 0u);
        }
    }
    std::fflush(stderr);
}

void diagnoseFighterActivation(PPCContext& ctx, uint8_t* base) {
    if (ctx.lr != fighterActivationCaller || !memory) return;
    static const bool enabled = [] {
        const char* value = std::getenv("DORITOS_AVATAR_TRACE");
        return value && value[0] == '1';
    }();
    if (!enabled) return;
    const uint32_t owner = ctx.r30.u32;
    const bool ownerValid = owner && memory->valid(owner, 4);
    const uint32_t ownerType = ownerValid ? memory->read32(owner) : 0;
    auto& counter = ownerType == titleFighter ? fighterActivationDiagnostics : otherActivationDiagnostics;
    const uint32_t limit = ownerType == titleFighter ? 64 : 16;
    if (counter.fetch_add(1, std::memory_order_relaxed) >= limit) return;
    const uint64_t handle = ctx.r4.u64;
    const uint64_t slot = ctx.r29.u64;
    const bool slotValid = ownerValid && slot < 24 && memory->valid(owner, 888);
    const uint64_t ownerHandle = slotValid ? memory->read64(owner + 504 + uint32_t(slot) * 8) : 0;
    const uint64_t childHandle = slotValid ? memory->read64(owner + 696 + uint32_t(slot) * 8) : 0;
    const bool managerValid = memory->valid(assetManagerGlobal, 4) &&
        ctx.r3.u32 == memory->read32(assetManagerGlobal) && ctx.r3.u32 != 0;
    const uint32_t master = managerValid && uint16_t(handle) == 4 ?
        uint32_t(resourceCall(ctx, base, sub_8223B988, handle, 0)) : 0;
    const bool masterValid = master && memory->valid(master, 396);
    const uint32_t masterType = masterValid ? memory->read32(master) : 0;
    const bool avatar = masterType == avatarMaster || masterType == titleAvatarMaster;
    const auto name = masterValid ? diagnosticString(memory->read32(master + 8)) : "<unavailable>";
    const auto filename = avatar ? diagnosticString(memory->read32(master + 292)) : "<not-avatar>";
    const uint64_t ownerIdentity = ownerValid && memory->valid(owner, 24) ? memory->read64(owner + 16) : 0;
    const uint32_t game = memory->valid(0x82b5caa0, 4) ? memory->read32(0x82b5caa0) : 0;
    const bool gameValid = game && memory->valid(game, 592) && memory->read32(game) == 0x8213c9cc;
    uint32_t playerMask = 0;
    if (gameValid && ownerIdentity && memory->read32(game + 364) <= 4) {
        for (uint32_t player = 0; player < memory->read32(game + 364); ++player)
            if (memory->read64(game + 368 + player * 56) == ownerIdentity) playerMask |= 1u << player;
    }
    std::fprintf(stderr,
        "[NATIVE FIGHTER MODEL ACTIVATION] owner=%08X vtable=%08X slot=%llu handle=%016llX ownerHandle=%016llX childHandle=%016llX manager=%08X validManager=%u master=%08X masterVtable=%08X name=%s file=%s avatarLoading=%u avatarType=%u avatarIndex=%u ownerIdentity=%016llX game=%08X playerMask=%X\n",
        owner, ownerType, static_cast<unsigned long long>(slot),
        static_cast<unsigned long long>(handle), static_cast<unsigned long long>(ownerHandle),
        static_cast<unsigned long long>(childHandle), ctx.r3.u32, managerValid ? 1u : 0u,
        master, masterType, name.c_str(), filename.c_str(), avatar ? memory->read32(master + 388) : 0,
        avatar ? memory->read32(master + 328) : 0, avatar ? memory->read32(master + 332) : 0,
        static_cast<unsigned long long>(ownerIdentity), game, playerMask);
    std::fflush(stderr);
}

bool authoredDummyModel(uint32_t object) {
    span(object, 320);
    const uint32_t address = memory->read32(object + 292);
    if (!address) return false;
    std::string name;
    for (uint32_t offset = 0; offset < 128; ++offset) {
        if (uint64_t(address) + offset > UINT32_MAX)
            throw NativeFailure("Avatar FileName wraps guest memory");
        span(address + offset, 1);
        char value = char(memory->base()[address + offset]);
        if (!value) return name == "_avatar/dummymodel.cfg";
        if (value == '\\') value = '/';
        if (value >= 'A' && value <= 'Z') value = char(value + ('a' - 'A'));
        name.push_back(value);
    }
    throw NativeFailure("Avatar FileName is not bounded");
}

bool authoredFighterAvatar(uint32_t object) {
    // Original 82276B80 stores the unprefixed registered master name at +8.
    span(object, 12);
    const uint32_t name = memory->read32(object + 8);
    if (name && memory->valid(name, 9) &&
        !memcmp(memory->base() + name, "gamepad", 7) &&
        memory->base()[name + 7] >= '0' && memory->base()[name + 7] <= '3' &&
        memory->base()[name + 8] == 0) return true;
    // GAMEXA 821B5418..542C selects player/random on actual SDK load failure
    // (+388 == 1); profile 82232048..206C resolves the same named master.
    // Keep readiness unchanged and accept only that exact failed-load identity.
    return name && memory->valid(name, 7) &&
        !memcmp(memory->base() + name, "random", 7) &&
        memory->valid(object, 392) && memory->read32(object + 388) == 1;
}

struct SelectedMaster { uint64_t handle; uint32_t authored = 0, model = 0, fighterOwner = 0; };
SelectedMaster instanceMaster(PPCContext& ctx, uint8_t* base) {
    const uint64_t authoredHandle = ctx.r4.u64;
    // Only the verified GUI activation and Fighter visual-slot-0 activation
    // can select a different model. Type 4 is a master handle; later lookups
    // retain the genuine type 3 instance handle and original ownership.
    const bool fighter = ctx.lr == fighterActivationCaller;
    if ((ctx.lr != guiActivationCaller && !fighter) || uint16_t(authoredHandle) != 4)
        return {authoredHandle};
    uint32_t fighterOwner = 0;
    if (fighter) {
        fighterOwner = ctx.r30.u32;
        if (ctx.r29.u64 != 0 || !fighterOwner || !memory || !memory->valid(fighterOwner, 512) ||
            memory->read32(fighterOwner) != titleFighter ||
            memory->read64(fighterOwner + 504) != authoredHandle)
            return {authoredHandle};
        // Shipped AVATARPOSE billboard children belong to ACTOR. Requiring
        // the exact Fighter class excludes their direct Xbox SDK fields.
    }
    span(assetManagerGlobal, 4);
    const uint32_t manager = memory->read32(assetManagerGlobal);
    if (!manager || ctx.r3.u32 != manager) return {authoredHandle};
    const uint32_t authored = uint32_t(resourceCall(ctx, base, sub_8223B988, authoredHandle, 0));
    if (!authored) return {authoredHandle};
    span(authored, 4);
    const uint32_t type = memory->read32(authored);
    if (type != avatarMaster && type != titleAvatarMaster) return {authoredHandle};
    if (fighter && !authoredFighterAvatar(authored)) return {authoredHandle};
    if (!authoredDummyModel(authored)) return {authoredHandle};

    // Original 8223BB90 resolves type:name through the already registered
    // title manager and creates/loads the real resource if it is absent.
    // Its BNMD factory 82398C10 installs 82161AF4: CFG parser 8239FD28 loads
    // actual Cal3D geometry, animations, masks and authored script callbacks.
    span(manager, 4);
    const uint32_t callStack = ctx.r1.u32 - 0x200;
    const uint32_t output = callStack + 0x80, name = callStack + 0x90;
    span(output, 8); span(name, sizeof(boneResource));
    memory->write64(output, 0);
    memcpy(base + name, boneResource, sizeof(boneResource));
    resourceCall(ctx, base, sub_8223BB90, output, manager, name);
    const uint64_t handle = memory->read64(output);
    if (uint16_t(handle) != 4) {
        diagnoseBoneResolve(ctx, base, handle, output, name, manager);
        char detail[256];
        std::snprintf(detail, sizeof(detail),
            "Original BoneModel resolver did not return a master handle; handle=%016llX output=%08X name=%s manager=%08X",
            static_cast<unsigned long long>(handle), output, boneResource, manager);
        throw NativeFailure(detail);
    }
    const uint32_t model = uint32_t(resourceCall(ctx, base, sub_8223B988, handle, 0));
    span(model, 4);
    if (memory->read32(model) != titleBoneMaster)
        throw NativeFailure("Native avatar selection did not resolve the actual title BoneModel class");
    return {handle, authored, model, fighterOwner};
}
}
uint64_t avatarRenderAdaptationCount() { return redirectedInstances.load(std::memory_order_relaxed); }
uint64_t avatarFighterAdaptationCount() { return redirectedFighterInstances.load(std::memory_order_relaxed); }
}

// GUIRESOURCE activation 822B5680 resolves its GRES master's Resource string,
// then requests a model instance here. The original dispatch calls the actual
// selected master's VT+140 pool and VT+116 factory. GUI and Fighter configuration,
// animation and cleanup subsequently use the returned instance's real type-3 handle and
// virtual methods. The common GUICONTROL constructor 82274A80 is unchanged.
PPC_FUNC(sub_8223B8F8) {
    Doritos::Native::diagnoseFighterActivation(ctx, base);
    const auto selected = Doritos::Native::instanceMaster(ctx, base);
    const uint32_t manager = ctx.r3.u32;
    ctx.r4.u64 = selected.handle;
    __imp__sub_8223B8F8(ctx, base);
    if (selected.authored && ctx.r3.u32) {
        using namespace Doritos::Native;
        const uint32_t instance = ctx.r3.u32;
        span(instance, 712);
        const uint64_t instanceHandle = memory->read64(instance + 16);
        if (memory->read32(instance) != titleBoneInstance || memory->read32(instance + 64) != selected.model ||
            uint16_t(instanceHandle) != 3 ||
            uint32_t(resourceCall(ctx, base, __imp__sub_8223B8F8, manager, instanceHandle)) != instance)
            throw NativeFailure("Original model factory did not return its owned actual BoneModel instance");
        const uint64_t count = redirectedInstances.fetch_add(1, std::memory_order_relaxed) + 1;
        if (selected.fighterOwner) {
            const uint64_t fighterCount = redirectedFighterInstances.fetch_add(1, std::memory_order_relaxed) + 1;
            printf("[native-avatar] Actual Cal3D BoneModel selected for Fighter instance #%llu; owner %08X; authored avatar master %08X; total %llu\n",
                static_cast<unsigned long long>(fighterCount), selected.fighterOwner, selected.authored,
                static_cast<unsigned long long>(count));
        } else {
            printf("[native-avatar] Actual Cal3D BoneModel selected for GUI instance #%llu; authored avatar master %08X\n",
                static_cast<unsigned long long>(count), selected.authored);
        }
        fflush(stdout);
    }
}
