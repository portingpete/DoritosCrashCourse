#include "runtime/native/runtime.h"
#include "runtime/native/avatar_adaptation.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
PPC_FUNC(sub_8223B8F8);
using namespace Doritos::Native;
namespace {
void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
constexpr uint64_t avatarHandle = 0x4321000000030004ULL;
constexpr uint64_t boneHandle = 0x1234000000020004ULL;
constexpr uint64_t childHandle = 0xabcd000000070003ULL;
std::map<uint64_t, uint32_t> masters, instances;
uint32_t assetManager = 0, bone = 0, child = 0;
uint64_t resolvedHandle = boneHandle, lastFactoryHandle = 0;
unsigned resolves = 0, masterLookups = 0, factories = 0, registryLookups = 0;
unsigned diagnosticPrefixes = 0, diagnosticIndices = 0;
bool nullFactory = false;
bool throwResolver = false;
uint32_t resolverControl = 0;
enum class Corrupt { None, Class, Master, HandleType, Registry } corrupt = Corrupt::None;
}

// Isolated boundary doubles verify real-handle delegation and guard behavior.
// Original-image vtables establish the factory/common cleanup ABI separately;
// these doubles do not prove Cal3D loading, pool cleanup or a rendered model.
PPC_FUNC(sub_8223B988) {
    const auto found = masters.find(ctx.r3.u64);
    ctx.r3.u64 = found == masters.end() ? 0 : found->second;
    ++masterLookups;
    ctx.r4.u64 = 0xdeadbeef; ctx.r5.u64 = 0;
}
PPC_FUNC(sub_8223BB90) {
    require(ctx.r4.u32 == assetManager, "existing original asset manager is used");
    require(!strcmp(reinterpret_cast<const char*>(base + ctx.r5.u32), "BNMD:_avatar/cj_native.cfg"),
        "actual named Cal3D resource is delegated");
    require(memory->valid(ctx.r1.u32 - 256, 512), "resolver has a separate valid guest call frame");
    // A real model load runs generated scalar/vector code that changes native
    // floating-point modes. The boundary must restore both host and cache.
    resolverControl = ctx.fpscr.csr ^ uint32_t(PPCFPSCRRegister::FlushMask) ^
        (uint32_t(1) << PPCFPSCRRegister::RoundShift);
    ctx.fpscr.setcsr(resolverControl);
    ctx.fpscr.loadFromHost();
    require(ctx.fpscr.csr == resolverControl, "resolver double perturbs native flush and rounding modes");
    if (throwResolver) {
        ctx.r3.u64 = 0xc0debabe;
        ctx.r6.u64 = 0xabcdef;
        throw NativeFailure("resolver boundary failure");
    }
    memory->write64(ctx.r3.u32, resolvedHandle);
    ++resolves;
    ctx.r3.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0xabcdef;
}
PPC_FUNC(sub_822A5980) { ++diagnosticPrefixes; ctx.r3.u64 = 0x82154448; }
PPC_FUNC(sub_822A56E0) { ++diagnosticIndices; ctx.r3.u64 = memory->read32(0x82a74b50); }
extern "C" PPC_FUNC(__imp__sub_8223B8F8) {
    if (uint16_t(ctx.r4.u64) == 3) {
        const auto found = instances.find(ctx.r4.u64);
        ctx.r3.u64 = found == instances.end() ? 0 : found->second;
        if (corrupt == Corrupt::Registry && ctx.r3.u32 == child) ctx.r3.u64 = 0;
        ++registryLookups;
        return;
    }
    require(ctx.r5.u32 == 0x12345678 && ctx.r6.u32 == 0x23456789 && ctx.r31.u32 == 0x3456789a,
        "helper calls preserve all original factory arguments and nonvolatile state");
    lastFactoryHandle = ctx.r4.u64; ++factories;
    if (ctx.r4.u64 != boneHandle || nullFactory) { ctx.r3.u64 = 0; return; }
    require(ctx.r3.u32 == assetManager, "genuine BoneModel factory uses the original manager");
    memory->write32(child, corrupt == Corrupt::Class ? 0x82161664 : 0x82161c24);
    memory->write32(child + 64, corrupt == Corrupt::Master ? 0 : bone);
    memory->write64(child + 16, corrupt == Corrupt::HandleType ? boneHandle : childHandle);
    // The real GUI caller, not the adapter, assigns its owner after factory return.
    memory->write32(child + 164, 0x87654321);
    ctx.r3.u64 = child;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        const auto image = loadOriginalXex(argv[1]);
        Memory guest(image); memory = &guest;
        require(guest.read32(0x82155bac + 148) == 0x822b5680 &&
            guest.read32(0x82155bac + 152) == 0x822b57e0 &&
            guest.read32(0x82161544 + 140) == 0x823938d8 &&
            guest.read32(0x8213d2a4 + 116) == 0x821bc5d0 &&
            guest.read32(0x82161af4 + 140) == 0x82276830 &&
            guest.read32(0x82161af4 + 116) == 0x82398bb0 &&
            guest.read32(0x82161c24 + 152) == 0x8239a490 &&
            guest.read32(0x82161c24 + 256) == guest.read32(0x82161664 + 256) &&
            guest.read32(0x8214582c + 348) == 0x822887e0 &&
            guest.read32(0x8215233c + 348) == 0x822887e0 &&
            guest.read32(0x82169acc + 4) == 0 &&
            guest.read32(0x82169acc + 12) == 0x82a71ab8 &&
            guest.read32(0x82276c54) == 0x387d0008,
            "original GUI activation/cleanup and true model factory/common child ABI");
        require(guest.read32(0x82161f64 + 32) == 0x822a5980 &&
            guest.read32(0x82161f64 + 52) == 0x822a56e0 &&
            guest.read32(0x822a5980) == 0x3d608215 &&
            guest.read32(0x822a5984) == 0x386b4448 &&
            guest.read32(0x822a5988) == 0x4e800020 &&
            guest.read32(0x822a56e0) == 0x3d6082a7 &&
            guest.read32(0x822a56e4) == 0x806b4b50 &&
            guest.read32(0x822a56e8) == 0x4e800020 &&
            guest.read32(0x82398984) == 0x2f0b0080 &&
            !strcmp(reinterpret_cast<const char*>(guest.base() + 0x82154448), "BNMD"),
            "independent original-image pure BNMD prefix/type-index getters for bounded failure diagnostics");
        struct OriginalWord { uint32_t address, word; };
        constexpr OriginalWord failedAvatarSelection[] = {
            // Actual selected Fighter Avatar master -> SDK failure -> player/random.
            {0x821b51b8, 0x388ac7d0}, {0x821b51cc, 0x90810058},
            {0x821b53c8, 0x38800000}, {0x821b53cc, 0x48005185},
            {0x821b53d0, 0x80a30000}, {0x821b53e8, 0x808b9414},
            {0x821b53ec, 0x48098f75}, {0x821b5404, 0x817f9874},
            {0x821b540c, 0x480886dd}, {0x821b5418, 0x81630184},
            {0x821b541c, 0x2f0b0001}, {0x821b5420, 0x409a0014},
            {0x821b5424, 0x38610050}, {0x821b5428, 0x80810058},
            {0x821b542c, 0x4bff601d}, {0x821b5430, 0x82c10050},
            // Profile path independently selects registered random on the same failure.
            {0x82232048, 0x817f0184}, {0x8223204c, 0x2f0b0001},
            {0x82232050, 0x409a002c}, {0x82232054, 0x3d608215},
            {0x82232058, 0x809e9414}, {0x8223205c, 0x38610068},
            {0x82232060, 0x38abd670}, {0x82232064, 0x4801c2fd},
            {0x82232068, 0xe8610068}, {0x8223206c, 0x4800991d},
        };
        for (const auto& original : failedAvatarSelection)
            require(guest.read32(original.address) == original.word,
                "independent original-image failed SDK Avatar selection oracle");
        require(!strcmp(reinterpret_cast<const char*>(guest.base() + 0x8213c7d0), "player/random") &&
            !strcmp(reinterpret_cast<const char*>(guest.base() + 0x8214d670), "random"),
            "original random Fighter template and registered Avatar identity literals");
        PPCContext ctx{}; guest.initializeThread(ctx, 0x100000); currentContext = &ctx;
        const uint32_t avatar = guest.allocate(4096), filename = guest.allocate(4096);
        const uint32_t fighter = guest.allocate(4096), masterName = guest.allocate(4096);
        bone = guest.allocate(4096); child = guest.allocate(4096); assetManager = guest.allocate(4096);
        guest.write32(0x82ba9874, assetManager);
        masters.emplace(avatarHandle, avatar); masters.emplace(boneHandle, bone);
        instances.emplace(childHandle, child);
        guest.write32(avatar, 0x82161544); guest.write32(avatar + 292, filename);
        guest.write32(avatar + 388, 1); guest.write32(avatar + 532, 0);
        guest.write32(bone, 0x82161af4);
        strcpy(reinterpret_cast<char*>(guest.base() + filename), "_avatar\\DummyModel.cfg");
        std::array<uint8_t, 700> authored{};
        memcpy(authored.data(), guest.base() + avatar, authored.size());
        auto request = [&](uint64_t handle, uint32_t caller = 0x822b571c, uint32_t manager = 0,
            uint32_t owner = 0, uint64_t index = 0) {
            ctx.r3.u64 = manager ? manager : assetManager; ctx.r4.u64 = handle;
            ctx.r5.u64 = 0x12345678; ctx.r6.u64 = 0x23456789; ctx.r31.u64 = 0x3456789a;
            ctx.r30.u64 = owner; ctx.r29.u64 = index;
            ctx.lr = caller; const auto stack = ctx.r1.u64;
            const uint32_t callerControl = ctx.fpscr.csr;
            require(ctx.fpscr.getcsr() == callerControl, "caller floating-point cache agrees with native mode");
            sub_8223B8F8(ctx, guest.base());
            require(ctx.r1.u64 == stack && ctx.r31.u64 == 0x3456789a && ctx.lr == caller &&
                ctx.r30.u64 == owner && ctx.r29.u64 == index,
                "helper calls preserve the original model request context");
            require(ctx.fpscr.csr == callerControl && ctx.fpscr.getcsr() == callerControl,
                "completed model helpers restore both cached and native flush/rounding modes");
            return ctx.r3.u32;
        };
        require(request(avatarHandle) == child && resolves == 1 && factories == 1 &&
            lastFactoryHandle == boneHandle && registryLookups == 1 && avatarRenderAdaptationCount() == 1,
            "actual master handle reaches original factory and its owned type3 instance is returned");
        require(guest.read32(child + 64) == bone && guest.read64(child + 16) == childHandle &&
            guest.read32(child + 164) == 0x87654321,
            "true instance identity, master ownership and original owner field are preserved");
        require(!memcmp(authored.data(), guest.base() + avatar, authored.size()),
            "authored Avatar metadata and genuine unavailable SDK state remain untouched");

        require(request(childHandle, 0x822b5804) == child && resolves == 1 && factories == 1,
            "normal GUI cleanup uses unchanged type3 lookup rather than an XAVT master handle");
        require(request(avatarHandle, 0x821a7bd0) == 0 && lastFactoryHandle == avatarHandle && resolves == 1,
            "other callers retain original Avatar factory behavior");
        const auto previousLookups = masterLookups;
        require(request(avatarHandle, 0x822b571c, assetManager + 4) == 0 &&
            lastFactoryHandle == avatarHandle && masterLookups == previousLookups && resolves == 1,
            "foreign managers do not trigger the adapter");
        require(request(boneHandle) == child && resolves == 1 && avatarRenderAdaptationCount() == 1,
            "ordinary BoneModel requests use the unchanged original factory");
        strcpy(reinterpret_cast<char*>(guest.base() + filename), "_avatar/creditsmodel.cfg");
        require(request(avatarHandle) == 0 && lastFactoryHandle == avatarHandle && resolves == 1,
            "unproven authored paths keep their original behavior");
        strcpy(reinterpret_cast<char*>(guest.base() + filename), "_avatar/dummymodel.cfg");
        guest.write32(avatar, 0x8213d2a4);
        require(request(avatarHandle) == child && resolves == 2 && avatarRenderAdaptationCount() == 2,
            "actual AvatarCC master selects its genuine common BoneModel child");
        const auto beforeFailures = factories;
        auto rejected = [&](const char* detail) {
            bool failure = false;
            try { request(avatarHandle); } catch (const NativeFailure&) { failure = true; }
            require(failure && avatarRenderAdaptationCount() == 2, detail);
        };
        guest.write32(bone, 0x82154894);
        rejected("wrong resolved master class is rejected before original instance creation");
        require(factories == beforeFailures, "invalid master did not reach original factory");
        guest.write32(bone, 0x82161af4);
        const uint32_t diagnosticGroup = guest.allocate(4096);
        guest.write32(diagnosticGroup, 0x82161f64);
        guest.write32(0x82ba9878, 76);
        for (uint32_t slot = 0; slot < 76; ++slot) guest.write32(0x82b5ad90 + slot * 4, 0);
        guest.write32(0x82b5ad90 + 75 * 4, diagnosticGroup);
        guest.write32(0x82a74b50, 7); guest.write32(0x82baa938 + 7 * 4, 4);
        const auto diagnosticStack = ctx.r1.u64;
        const auto diagnosticControl = ctx.fpscr.csr;
        resolvedHandle = childHandle;
        rejected("resolver result must be a real master handle");
        require(factories == beforeFailures, "wrong handle type did not reach original factory");
        require(ctx.r1.u64 == diagnosticStack && ctx.fpscr.csr == diagnosticControl &&
            ctx.fpscr.getcsr() == diagnosticControl && guest.read32(0x82baa938 + 7 * 4) == 4 &&
            guest.read32(diagnosticGroup) == 0x82161f64 &&
            diagnosticPrefixes == 1 && diagnosticIndices == 1,
            "bounded 76-group diagnostics reach BNMD and preserve its actual gate and caller context");
        guest.write32(0x82ba9878, 0);
        resolvedHandle = boneHandle; masters.erase(boneHandle);
        rejected("absent resolved resource is not fabricated");
        require(factories == beforeFailures, "absent master did not reach original factory");
        masters.emplace(boneHandle, bone);
        for (auto failure : {Corrupt::Class, Corrupt::Master, Corrupt::HandleType, Corrupt::Registry}) {
            corrupt = failure;
            rejected("wrong factory class, master, handle or registry identity is not accepted");
        }
        corrupt = Corrupt::None; nullFactory = true;
        require(request(avatarHandle) == 0 && avatarRenderAdaptationCount() == 2,
            "genuine original null factory result remains null without invented readiness");
        nullFactory = false;
        guest.write32(fighter, 0x8214582c);
        guest.write64(fighter + 504, avatarHandle);
        guest.write64(fighter + 696, 0xffffffffffff0000ULL);
        guest.write32(avatar + 8, masterName);
        std::array<uint8_t, 704> fighterBefore{};
        memcpy(fighterBefore.data(), guest.base() + fighter, fighterBefore.size());
        auto fighterRequest = [&](uint32_t owner = 0, uint64_t index = 0, uint32_t caller = 0x822889e0) {
            return request(avatarHandle, caller, 0, owner ? owner : fighter, index);
        };
        for (unsigned player = 0; player < 4; ++player) {
            guest.write32(avatar, player % 2 ? 0x8213d2a4 : 0x82161544);
            char name[9]; snprintf(name, sizeof(name), "gamepad%u", player);
            strcpy(reinterpret_cast<char*>(guest.base() + masterName), name);
            std::array<uint8_t, 700> masterBefore{};
            memcpy(masterBefore.data(), guest.base() + avatar, masterBefore.size());
            require(fighterRequest() == child && lastFactoryHandle == boneHandle &&
                avatarFighterAdaptationCount() == player + 1 && avatarRenderAdaptationCount() == player + 3,
                "exact authored Fighter gamepad0..3 reaches the actual Cal3D factory");
            require(!memcmp(masterBefore.data(), guest.base() + avatar, masterBefore.size()) &&
                !memcmp(fighterBefore.data(), guest.base() + fighter, fighterBefore.size()),
                "adapter preserves authored Fighter master and child handles for original activation");
        }
        strcpy(reinterpret_cast<char*>(guest.base() + masterName), "random");
        std::array<uint8_t, 700> failedMasterBefore{};
        memcpy(failedMasterBefore.data(), guest.base() + avatar, failedMasterBefore.size());
        require(fighterRequest() == child && lastFactoryHandle == boneHandle &&
            avatarFighterAdaptationCount() == 5 && avatarRenderAdaptationCount() == 7,
            "original failed SDK random Fighter visual selects its actual Cal3D child");
        require(!memcmp(failedMasterBefore.data(), guest.base() + avatar, failedMasterBefore.size()) &&
            !memcmp(fighterBefore.data(), guest.base() + fighter, fighterBefore.size()),
            "failed SDK random selection preserves real readiness, metadata and Fighter ownership");
        const auto fighterResolves = resolves, fighterLookups = masterLookups;
        require(fighterRequest(0, 1) == 0 && resolves == fighterResolves && masterLookups == fighterLookups,
            "other Fighter visual slots retain the original factory");
        require(fighterRequest(0, 0x100000000ULL) == 0 && resolves == fighterResolves,
            "only the exact original zero index is accepted");
        require(fighterRequest(fighter + 4096) == 0 && resolves == fighterResolves,
            "unowned activation owner does not redirect");
        require(request(avatarHandle, 0x822889e0, 0, 0) == 0 && resolves == fighterResolves &&
            request(avatarHandle, 0x822889e0, 0, UINT32_MAX) == 0 && resolves == fighterResolves,
            "missing or inaccessible activation owner does not redirect");
        guest.write32(fighter, 0x8215233c);
        require(fighterRequest() == 0 && resolves == fighterResolves,
            "actual ACTOR owners of authored AvatarPose billboards retain the original factory");
        guest.write32(fighter, 0x82150b54);
        require(fighterRequest() == 0 && resolves == fighterResolves,
            "other non-Fighter classes retain the original factory");
        guest.write32(fighter, 0x8214582c);
        guest.write64(fighter + 504, boneHandle);
        require(fighterRequest() == 0 && resolves == fighterResolves,
            "incoming source handle must equal the actual Fighter visual-slot-0 master");
        guest.write64(fighter + 504, avatarHandle);
        for (uint32_t loading : {0u, 2u}) {
            guest.write32(avatar + 388, loading);
            require(fighterRequest() == 0 && resolves == fighterResolves,
                "random Avatar without the exact original failed SDK load state keeps its factory");
        }
        guest.write32(avatar + 388, 1);
        strcpy(reinterpret_cast<char*>(guest.base() + filename), "_avatar/creditsmodel.cfg");
        require(fighterRequest() == 0 && resolves == fighterResolves,
            "failed SDK random master still requires the actual authored DummyModel CFG");
        strcpy(reinterpret_cast<char*>(guest.base() + filename), "_avatar/dummymodel.cfg");
        for (const char* name : {"Random", "random0", "random/", "XAVT:random", "custom0", "default", "gamepad4", "gamepad00", "Gamepad0", "XAVT:gamepad0"}) {
            strcpy(reinterpret_cast<char*>(guest.base() + masterName), name);
            require(fighterRequest() == 0 && resolves == fighterResolves,
                "other Avatar master identities retain the original factory");
        }
        strcpy(reinterpret_cast<char*>(guest.base() + masterName), "gamepad0");
        nullFactory = true;
        require(fighterRequest() == 0 && avatarFighterAdaptationCount() == 5 && avatarRenderAdaptationCount() == 7,
            "genuine Fighter null result remains null without invented readiness or ownership");
        require(request(childHandle, 0x82288bd0) == child && avatarFighterAdaptationCount() == 5,
            "Fighter cleanup queries its true original type3 child handle unchanged");
        const auto failureStack = ctx.r1.u64;
        const uint32_t failureCallerControl = ctx.fpscr.csr;
        throwResolver = true;
        bool resolverFailed = false;
        try { fighterRequest(); } catch (const NativeFailure&) { resolverFailed = true; }
        require(resolverFailed && ctx.r1.u64 == failureStack - 0x200 && ctx.r3.u64 == 0xc0debabe &&
            ctx.r6.u64 == 0xabcdef && ctx.fpscr.csr == resolverControl && ctx.fpscr.getcsr() == resolverControl &&
            resolverControl != failureCallerControl,
            "failed model helper retains its real stack, clobbers and native mode for diagnostics");
        ctx.fpscr.setcsr(failureCallerControl);
        ctx.fpscr.csr = failureCallerControl;
        ctx.r1.u64 = failureStack;
        printf("Native Avatar/Cal3D GUI and guarded Fighter instance boundary contracts passed\n");
        return 0;
    } catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); return 1; }
}
