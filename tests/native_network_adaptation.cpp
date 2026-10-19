#include <winsock2.h>
#include "runtime/native/runtime.h"
#include "runtime/native/network_adaptation.h"
#include "runtime/native/network_services.h"
#include <cstdio>
#include <array>
#include <cstring>
#include <stdexcept>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
PPC_FUNC(sub_82851BA0);
PPC_FUNC(sub_82849EB0);
using namespace Doritos::Native;
namespace {
constexpr uint32_t owner = 0x82b34704, pool = owner + 32;
unsigned created = 0, destroyed = 0, timerCalls = 0;
uint32_t poolTable = 0, tick = 0;
bool poolFailure = false, timerFailure = false;
uint32_t titleOwner = 0, soundOwner = 0, audioOwner = 0, createdEngine = 0;
unsigned engineCalls = 0, factoryFallbackCalls = 0;
bool engineFailure = false;
PPCContext factoryArguments{};
void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
struct StopNetwork { ~StopNetwork() { shutdownNetworkServices(); } };
}

// These isolate the platform adaptation's verified original CPU call boundary.
// Native Winsock is real; actual original pool allocation, locking, CPU-engine
// creation and session-state execution are validated separately in the title.
PPC_FUNC(sub_82860D28) {
    require(ctx.r3.u32 == pool && ctx.r4.u32 == 1 && ctx.r5.u32 == 0,
        "original shared pool initialization ABI");
    require(memory->read32(pool) == poolTable && !memory->read32(pool + 4) && !memory->read32(pool + 8),
        "original pool is initialized before its native platform bootstrap");
    if (poolFailure) { ctx.r3.s64 = int32_t(0x8007000e); return; }
    const uint32_t list = memory->allocate(4096), item = memory->allocate(4096);
    memory->write32(list, item);
    memory->write32(pool + 4, list); memory->write32(pool + 8, 1);
    ++created;
    ctx.r3.u64 = 0; ctx.r31.u64 = 0xdeadbeef;
}
PPC_FUNC(sub_82860E88) {
    require(ctx.r3.u32 == pool, "original pool release ABI");
    const uint32_t list = memory->read32(pool + 4);
    require(memory->read32(pool + 8) == 1 && memory->release(memory->read32(list)) && memory->release(list),
        "actual test allocations release completely");
    memory->write32(pool + 4, 0); memory->write32(pool + 8, 0);
    ++destroyed;
    ctx.r3.u64 = 0;
}
PPC_FUNC(sub_82851A50) {
    require(ctx.r3.u32 != 0, "original network error mapper receives genuine Winsock failure");
    ctx.r3.s64 = int32_t(0x807a100a);
}
PPC_FUNC(sub_825FE810) {
    ++timerCalls;
    if (timerFailure) { ctx.r6.u64 = 0xbadc0de; throw NativeFailure("injected original timer failure"); }
    tick = GetTickCount(); ctx.r3.u64 = tick;
}

extern "C" PPC_FUNC(__imp__sub_82849EB0) {
    ++factoryFallbackCalls;
    factoryArguments = ctx;
    ctx.r3.s64 = int32_t(0x80070032);
}
PPC_FUNC(sub_828499E8) {
    ++engineCalls;
    factoryArguments = ctx;
    require(ctx.r3.u64 == 0 && ctx.r4.u32 == titleOwner + 0x26288 &&
        ctx.r5.u64 == 0 && ctx.r6.u64 == 0 && ctx.r7.u32 == 0x82bac26c,
        "original LOCAL factory retains mode, CPU callback and output, with only optional voice factory absent");
    if (engineFailure) { ctx.r3.s64 = int32_t(0x8007000e); return; }
    createdEngine = memory->allocate(4096);
    memory->write32(createdEngine, 0x820ecd68);
    memory->write32(createdEngine + 12, ctx.r4.u32);
    memory->write32(createdEngine + 28, ctx.r3.u32);
    // Isolated original constructor boundary: the encrypted image oracle below
    // verifies 8284E200's null factory writes both pair fields to zero.
    memory->write32(createdEngine + 424, ctx.r6.u32);
    memory->write32(createdEngine + 428, 0);
    memory->write32(0x82b346f8, createdEngine);
    memory->write32(ctx.r7.u32, createdEngine);
    ctx.r3.u64 = 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        const auto image = loadOriginalXex(argv[1]);
        Memory guest(image); memory = &guest;
        StopNetwork stop;
        PPCContext ctx{}; guest.initializeThread(ctx, 0x100000); currentContext = &ctx;
        require(guest.read32(0x820ecd68 + 120) == 0x82849158,
            "retained original network CPU object uses its real property interface");
        const std::array<std::pair<uint32_t, uint32_t>, 29> factoryWords{{
            {0x823a897c, 0x484a1535}, {0x823a8964, 0x7c661b78},
            {0x823a8978, 0x38600000}, {0x82849eb0, 0x4bfffb38},
            {0x82849a40, 0x7f47d378}, {0x82842964, 0x4800b89d},
            {0x8284e21c, 0x2b050000}, {0x8284e238, 0x39600000},
            {0x8284e240, 0x7fc3f378}, {0x8284e244, 0x917e0004},
            {0x8284e420, 0x811d0000}, {0x8284e424, 0x2b080000},
            {0x8284e4b8, 0x38600000}, {0x82845c70, 0x2b0a0000},
            {0x82845c74, 0x419a0024}, {0x82604110, 0x394a3bd8},
            {0x82232d88, 0x3d008294}, {0x82232db0, 0x39683fa8},
            {0x82232dbc, 0x916440f4}, {0x82943fcc, 0x392a3714},
            {0x82943fd4, 0x91230000}, {0x82278e14, 0x9123000c},
            {0x82278e28, 0x4838ba61}, {0x82278e74, 0x807e0000},
            {0x82604854, 0x93db0000}, {0x82604120, 0x91430000},
            {0x82604114, 0x93a3000c}, {0x82232d9c, 0x3c8082a7},
            {0x82943fc4, 0x3d408216}
        }};
        for (const auto& [address, word] : factoryWords)
            require(guest.read32(address) == word,
                "encrypted original factory provenance, null-provider constructor and peer no-voice branch oracle");
        require(guest.read32(0x82163714 + 4) == 0x82278de8 &&
            guest.read32(0x82163714 + 8) == 0x82278e90 &&
            guest.read32(0x82164260) == 0x823ab3f0,
            "title-selected SOUND class retains the original getter/release and NETMAN callback identities");
        poolTable = guest.allocate(4096);
        guest.write32(pool, poolTable); guest.write32(pool + 4, 0); guest.write32(pool + 8, 0);
        guest.write32(owner + 28, 1); guest.write32(owner + 44, 0x12345678);
        guest.write32(0x82bac26c, 0);
        auto bootstrap = [&](uint32_t service = owner) {
            ctx.r3.u64 = service; ctx.r4.u64 = 0x12345; ctx.r5.u64 = 0x45678;
            ctx.r31.u64 = 0x87654321; ctx.lr = 0x82851c88;
            const uint64_t stack = ctx.r1.u64;
            sub_82851BA0(ctx, guest.base());
            require(ctx.r1.u64 == stack && ctx.lr == 0x82851c88 && ctx.r31.u64 == 0x87654321 &&
                ctx.r4.u64 == 0x12345 && ctx.r5.u64 == 0x45678,
                "native platform helpers preserve caller stack, link and CPU arguments");
            return ctx.r3.u32;
        };
        auto cleanup = [&] {
            ctx.r3.u64 = 1;
            require(networkServicesImport(ctx, guest.base(), "NetDll_WSACleanup"), "actual native cleanup import");
            return ctx.r3.u32;
        };
        poolFailure = true;
        require(bootstrap() == 0x8007000e && created == 0 && destroyed == 0 && timerCalls == 0 &&
            nativeNetworkBootstrapCount() == 0 && guest.read32(owner + 44) == 0x12345678,
            "original pool failure remains a failure and creates no native socket/timer readiness");
        require(cleanup() == uint32_t(SOCKET_ERROR), "failed pool initialization acquires no Winsock reference");
        poolFailure = false;
        bool rejected = false;
        try { bootstrap(owner + 4); } catch (const NativeFailure&) { rejected = true; }
        require(rejected && !created, "unverified shared-owner addresses are rejected");
        guest.write32(owner + 28, 2); rejected = false;
        try { bootstrap(); } catch (const NativeFailure&) { rejected = true; }
        require(rejected && !created, "initializer requires the original locked first-reference contract");
        guest.write32(owner + 28, 1);
        timerFailure = true; rejected = false;
        const uint64_t failingStack = ctx.r1.u64;
        try { bootstrap(); } catch (const NativeFailure&) { rejected = true; }
        require(rejected && created == 1 && destroyed == 1 && !nativeNetworkBootstrapCount() &&
            guest.read32(owner + 44) == 0x12345678 && ctx.r1.u64 == failingStack && ctx.r6.u64 == 0xbadc0de,
            "failed original timer releases pool and keeps the actual helper diagnostic without readiness");
        require(cleanup() == uint32_t(SOCKET_ERROR), "failed timer releases its real Winsock startup reference");
        timerFailure = false;
        require(bootstrap() == 0 && created == 2 && destroyed == 1 && timerCalls == 2 &&
            nativeNetworkBootstrapCount() == 1 && guest.read32(owner + 44) == tick,
            "native offline foundation initializes real Winsock and preserves actual timer output");
        require(guest.read32(owner + 28) == 1 && guest.read32(0x82bac26c) == 0,
            "platform initialization retains reference ownership and does not fabricate a CPU-engine object");
        rejected = false;
        try { bootstrap(); } catch (const NativeFailure&) { rejected = true; }
        require(rejected && created == 2 && nativeNetworkBootstrapCount() == 1,
            "live original pool cannot be initialized or counted twice");
        ctx.r3.u64 = 1; ctx.r4.u64 = 0;
        require(networkServicesImport(ctx, guest.base(), "NetDll_XNetStartup") && ctx.r3.u32 == WSAEOPNOTSUPP,
            "ordinary native socket foundation does not turn on Xbox secure transport");
        require(cleanup() == 0 && cleanup() == uint32_t(SOCKET_ERROR),
            "successful native foundation owns exactly one real Winsock reference");
        ctx.r3.u64 = 1;
        require(networkServicesImport(ctx, guest.base(), "NetDll_XNetCleanup") && ctx.r3.u32 == WSANOTINITIALISED,
            "original teardown sees the genuine inactive Xbox transport result");
        ctx.r3.u64 = pool; sub_82860E88(ctx, guest.base());
        require(destroyed == 2 && !guest.read32(pool + 4) && !guest.read32(pool + 8),
            "original pool release remains independently responsible for its allocations");
        require(bootstrap() == 0 && created == 3 && nativeNetworkBootstrapCount() == 2 && timerCalls == 3,
            "released original service can acquire a fresh native foundation");
        require(cleanup() == 0, "second native foundation cleanup balances its real reference");
        ctx.r3.u64 = pool; sub_82860E88(ctx, guest.base());
        titleOwner = guest.allocate(0x29000);
        soundOwner = guest.allocate(4096); audioOwner = guest.allocate(4096);
        guest.write32(titleOwner, 0x82164064);
        guest.write32(titleOwner + 168, 0); guest.write32(titleOwner + 176, 0);
        guest.write32(titleOwner + 0x26288, 0x82164260);
        guest.write32(soundOwner, 0x82163714); guest.write32(soundOwner + 4, audioOwner);
        guest.write32(audioOwner, 0x82083bd8); guest.write32(audioOwner + 4, 0x82083bc8);
        guest.write32(audioOwner + 12, 7);
        guest.write32(0x82bab13c, soundOwner);
        guest.write32(0x82a740f4, 0x82943fa8);
        guest.write32(0x82bac26c, 0); guest.write32(0x82b346f8, 0);
        auto factory = [&](uint64_t mode = 0, uint32_t link = 0x823a8980,
            uint64_t provider = UINT64_MAX) {
            if (provider == UINT64_MAX) provider = audioOwner;
            ctx.r3.u64 = mode; ctx.r4.u64 = titleOwner + 0x26288;
            ctx.r5.u64 = 0; ctx.r6.u64 = provider; ctx.r7.s64 = int32_t(0x82bac26c);
            ctx.r28.u64 = titleOwner + 0x26288; ctx.r29.s64 = int32_t(0x82bac26c);
            ctx.r30.u64 = titleOwner; ctx.r31.u64 = 0x12345678; ctx.lr = link;
            const uint64_t stack = ctx.r1.u64;
            sub_82849EB0(ctx, guest.base());
            require(ctx.r1.u64 == stack && ctx.lr == link && ctx.r31.u64 == 0x12345678 &&
                ctx.r28.u64 == titleOwner + 0x26288 && ctx.r29.s64 == int32_t(0x82bac26c) &&
                ctx.r30.u64 == titleOwner,
                "offline configuration preserves original caller stack, link and nonvolatile ownership");
            return ctx.r3.u32;
        };
        auto refusedFactory = [&](uint64_t mode, uint64_t provider = UINT64_MAX) {
            const unsigned before = engineCalls;
            const uint64_t stack = ctx.r1.u64;
            bool refused = false;
            try { factory(mode, 0x823a8980, provider); } catch (const NativeFailure&) { refused = true; }
            require(refused && engineCalls == before && !guest.read32(0x82bac26c) &&
                !guest.read32(0x82b346f8) && !nativeOfflineNetworkEngineCount(),
                "unsupported factory contract changes no CPU allocation, singleton or readiness");
            require(ctx.r1.u64 == stack && ctx.lr == 0x823a8980 && ctx.r3.u64 == mode &&
                ctx.r6.u64 == (provider == UINT64_MAX ? audioOwner : provider) &&
                ctx.r4.u64 == titleOwner + 0x26288 && ctx.r5.u64 == 0 &&
                ctx.r7.s64 == int32_t(0x82bac26c) && ctx.r28.u64 == titleOwner + 0x26288 &&
                ctx.r29.s64 == int32_t(0x82bac26c) && ctx.r30.u64 == titleOwner &&
                ctx.r31.u64 == 0x12345678,
                "guard failures and bounded diagnostics preserve all incoming factory arguments and caller ownership");
        };
        refusedFactory(1); refusedFactory(2); refusedFactory(3); refusedFactory(0x100000000ull);
        guest.write32(titleOwner + 168, 1); refusedFactory(0);
        guest.write32(titleOwner + 168, 2); refusedFactory(0);
        guest.write32(titleOwner + 168, 0);
        guest.write32(titleOwner + 176, 2); refusedFactory(0);
        guest.write32(titleOwner + 176, 0);
        guest.write32(titleOwner + 0x26288, 0x82164264); refusedFactory(0);
        guest.write32(titleOwner + 0x26288, 0x82164260);
        const uint32_t unmapped = guest.allocate(4096);
        require(guest.release(unmapped) && !guest.valid(unmapped, 16),
            "diagnostic failure fixture is a genuinely decommitted native guest span");
        guest.write32(0x82bab13c, unmapped); refusedFactory(0);
        guest.write32(0x82bab13c, soundOwner);
        guest.write32(soundOwner + 4, unmapped); refusedFactory(0, unmapped);
        guest.write32(soundOwner + 4, audioOwner);
        guest.write32(0x82a740f4, 0x82278d88); refusedFactory(0);
        guest.write32(0x82a740f4, 0x82943fa8);
        guest.write32(soundOwner, 0x821515b8); refusedFactory(0);
        guest.write32(soundOwner, 0x82163714);
        guest.write32(soundOwner + 4, audioOwner + 4); refusedFactory(0);
        refusedFactory(0, audioOwner + 4);
        guest.write32(soundOwner + 4, audioOwner);
        guest.write32(audioOwner, 0x82083bdc); refusedFactory(0);
        guest.write32(audioOwner, 0x82083bd8);
        guest.write32(audioOwner + 12, 0); refusedFactory(0);
        guest.write32(audioOwner + 12, 7);
        refusedFactory(0, uint64_t(audioOwner) | (uint64_t(1) << 32));
        require(factory(1, 0x82849f00) == 0x80070032 && factoryFallbackCalls == 1 &&
            factoryArguments.r3.u64 == 1 && factoryArguments.r6.u32 == audioOwner && !engineCalls,
            "other original callers retain their exact mode and provider arguments through unchanged factory");
        engineFailure = true;
        require(factory() == 0x8007000e && engineCalls == 1 && !nativeOfflineNetworkEngineCount() &&
            !guest.read32(0x82bac26c) && !guest.read32(0x82b346f8),
            "real original constructor failure propagates without native success, output or counted engine");
        engineFailure = false;
        std::array<uint8_t, 16> audioBefore{}, soundBefore{};
        std::memcpy(audioBefore.data(), guest.base() + audioOwner, audioBefore.size());
        std::memcpy(soundBefore.data(), guest.base() + soundOwner, soundBefore.size());
        require(factory() == 0 && engineCalls == 2 && nativeOfflineNetworkEngineCount() == 1 &&
            guest.read32(0x82bac26c) == createdEngine && guest.read32(0x82b346f8) == createdEngine &&
            !guest.read32(createdEngine + 424) && !guest.read32(createdEngine + 428) &&
            guest.read32(titleOwner + 168) == 0 && guest.read32(titleOwner + 176) == 0,
            "original LOCAL factory returns its actual CPU engine with no Xbox voice handles or state writes");
        require(!std::memcmp(audioBefore.data(), guest.base() + audioOwner, audioBefore.size()) &&
            !std::memcmp(soundBefore.data(), guest.base() + soundOwner, soundBefore.size()),
            "external real audio ownership, reference and engine bytes remain unchanged");
        require(guest.release(createdEngine), "isolated original engine remains independently owned and releasable");
        guest.write32(0x82bac26c, 0); guest.write32(0x82b346f8, 0);
        require(guest.release(audioOwner) && guest.release(soundOwner) && guest.release(titleOwner),
            "offline configuration acquires no audio references or borrowed CPU allocation ownership");
        printf("Native offline network bootstrap contracts passed (real Winsock, isolated original CPU boundaries)\n");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "Native offline network bootstrap contract failed: %s\n", error.what());
        return 1;
    }
}
