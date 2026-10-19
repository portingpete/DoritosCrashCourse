#include <winsock2.h>
#include "network_adaptation.h"
#include "network_services.h"
#include "runtime.h"
#include <atomic>
#include <cstdio>

PPC_FUNC(sub_82860D28);
PPC_FUNC(sub_82860E88);
PPC_FUNC(sub_82851A50);
PPC_FUNC(sub_825FE810);
PPC_FUNC(sub_828499E8);
extern "C" PPC_FUNC(__imp__sub_82849EB0);

namespace Doritos::Native {
namespace {
constexpr uint32_t originalSharedService = 0x82b34704;
std::atomic<uint64_t> bootstraps{0};
std::atomic<uint64_t> offlineEngines{0};
std::atomic<uint64_t> voiceOwnershipFailures{0};
constexpr uint32_t originalEngineOutput = 0x82bac26c;
constexpr uint32_t originalEngineSingleton = 0x82b346f8;
constexpr uint32_t originalSoundOwner = 0x82bab13c;
constexpr uint32_t originalSoundFactoryGlobal = 0x82a740f4;
constexpr uint32_t originalTitleSoundFactory = 0x82943fa8;

void reportVoiceOwnershipFailure(uint32_t owner, uint32_t factory,
    uint32_t sound, uint64_t incomingAudio) {
    const uint64_t count = voiceOwnershipFailures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 8) return;
    const uint32_t audio = uint32_t(incomingAudio);
    const bool soundValid = sound && memory->valid(sound, 16);
    const bool audioValid = audio && memory->valid(audio, 16);
    std::fprintf(stderr,
        "[NETWORK] Voice-provider ownership failure #%llu: owner=%08X factory=%08X r6=%016llX high=%08X sound=%08X span16=%u VT=%08X +4=%08X +12=%08X audio=%08X span16=%u VT=%08X +12=%08X\n",
        static_cast<unsigned long long>(count), owner, factory,
        static_cast<unsigned long long>(incomingAudio), uint32_t(incomingAudio >> 32),
        sound, unsigned(soundValid), soundValid ? memory->read32(sound) : 0,
        soundValid ? memory->read32(sound + 4) : 0,
        soundValid ? memory->read32(sound + 12) : 0,
        audio, unsigned(audioValid), audioValid ? memory->read32(audio) : 0,
        audioValid ? memory->read32(audio + 12) : 0);
}

void createOfflineEngine(PPCContext& ctx, uint8_t* base) {
    // 823A8818 is the sole title caller of this factory. Its base constructor
    // has already set the real current mode and state to LOCAL/IDLE. The
    // derived NextSessionType field is initialized later and is not read here.
    if (ctx.lr != 0x823a8980) {
        __imp__sub_82849EB0(ctx, base);
        return;
    }
    const uint32_t owner = ctx.r30.u32;
    constexpr uint32_t callbackOffset = 0x26288;
    if (!memory || base != memory->base() || ctx.r3.u64 || ctx.r5.u64 ||
        owner > UINT32_MAX - callbackOffset - 4 ||
        !memory->valid(owner, callbackOffset + 4) ||
        memory->read32(owner) != 0x82164064 ||
        memory->read32(owner + 168) || memory->read32(owner + 176) ||
        ctx.r28.u64 != uint64_t(owner) + callbackOffset ||
        ctx.r4.u64 != uint64_t(owner) + callbackOffset ||
        memory->read32(owner + callbackOffset) != 0x82164260 ||
        ctx.r29.u32 != originalEngineOutput || ctx.r7.u32 != originalEngineOutput ||
        !memory->valid(originalEngineOutput, 4) || memory->read32(originalEngineOutput) ||
        !memory->valid(originalEngineSingleton, 4) || memory->read32(originalEngineSingleton) ||
        !memory->valid(originalSoundOwner, 4) ||
        !memory->valid(originalSoundFactoryGlobal, 4))
        fail("Unverified original LOCAL network factory contract");

    // Original title startup replaces the default SOUND factory at 82232DBC
    // with 82943FA8. Its 16-byte object installs 82163714, retaining the same
    // slot4 getter 82278DE8 and slot8 release 82278E90. The getter returns its
    // stored primary audio engine directly; keep that engine's lifetime intact.
    const uint32_t factory = memory->read32(originalSoundFactoryGlobal);
    const uint32_t sound = memory->read32(originalSoundOwner);
    const uint32_t audio = ctx.r6.u32;
    if (factory != originalTitleSoundFactory || ctx.r6.u64 != audio || !sound || !audio ||
        !memory->valid(sound, 16) || memory->read32(sound) != 0x82163714 ||
        memory->read32(sound + 4) != audio ||
        !memory->valid(audio, 16) || memory->read32(audio) != 0x82083bd8 ||
        !memory->read32(audio + 12)) {
        reportVoiceOwnershipFailure(owner, factory, sound, ctx.r6.u64);
        fail("Unverified original network voice-provider ownership");
    }

    // This native port has no Xbox system-voice capture/provider. Select the
    // engine's actual absent-provider configuration instead of inventing an
    // XamVoiceCreate result or handle. Original 828499E8 -> 82843DE8 -> 82842910
    // calls 8284E200 with this null optional factory. Its pair stays null, and
    // 8284E3E0/82845BA0 take their authored no-provider branches. All original
    // allocations, session mode, CPU callbacks, peers and failure cleanup run.
    ctx.r6.u64 = 0;
    sub_828499E8(ctx, base);
    if (ctx.r3.u32) return;
    const uint32_t engine = memory->read32(originalEngineOutput);
    if (!engine || !memory->valid(engine, 744) ||
        memory->read32(engine) != 0x820ecd68 ||
        memory->read32(engine + 12) != owner + callbackOffset ||
        memory->read32(engine + 28) || memory->read32(engine + 424) ||
        memory->read32(engine + 428) ||
        memory->read32(originalEngineSingleton) != engine)
        fail("Original offline engine did not retain its absent voice-provider contract");
    const uint64_t count = offlineEngines.fetch_add(1, std::memory_order_relaxed) + 1;
    std::fprintf(stderr,
        "[NETWORK] Original LOCAL CPU engine #%llu uses the native absent Xbox voice-provider policy; no voice handle created\n",
        static_cast<unsigned long long>(count));
}

uint32_t originalCall(PPCContext& ctx, uint8_t* base, PPCFunc* entry,
    uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
    const PPCContext saved = ctx;
    ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5;
    entry(ctx, base);
    const uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
}

uint32_t winsockCall(PPCContext& ctx, uint8_t* base, const char* operation,
    uint32_t version = 0, uint32_t output = 0) {
    const PPCContext saved = ctx;
    ctx.r3.u64 = 1; ctx.r4.u64 = version; ctx.r5.u64 = output;
    if (!networkServicesImport(ctx, base, operation))
        fail("Native network bootstrap has no ordinary Winsock service");
    const uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
}

struct CallFrame {
    PPCContext& context;
    uint64_t stack, link;
    explicit CallFrame(PPCContext& ctx) : context(ctx), stack(ctx.r1.u64), link(ctx.lr) {
        if (ctx.r1.u32 < 1024 || !memory->valid(ctx.r1.u32 - 1024, 1024))
            fail("Native network bootstrap has no guest call frame");
        ctx.r1.u64 = ctx.r1.u32 - 1024;
    }
    ~CallFrame() { context.r1.u64 = stack; context.lr = link; }
};

void initialize(PPCContext& ctx, uint8_t* base) {
    const uint32_t owner = ctx.r3.u32;
    if (!memory || base != memory->base() || owner != originalSharedService ||
        !memory->valid(owner, 48) || memory->read32(owner + 28) != 1)
        fail("Unverified original shared network bootstrap ownership");
    // Original 82851C48 holds this service's lock, raises its first reference,
    // and invokes this platform initializer. Retain the original allocator and
    // pool, then initialize the port's actual ordinary Windows socket service.
    // The Xbox secure transport import remains unavailable. This establishes
    // no session, endpoints, XUIDs, authentication, peers or online readiness.
    const uint32_t pool = owner + 32;
    if (!memory->read32(pool) || memory->read32(pool + 4) || memory->read32(pool + 8))
        fail("Native network bootstrap requires an initialized empty original pool");
    CallFrame frame(ctx);
    const uint32_t poolStatus = originalCall(ctx, base, sub_82860D28, pool, 1, 0);
    if (int32_t(poolStatus) < 0) { ctx.r3.s64 = int32_t(poolStatus); return; }
    const uint32_t output = ctx.r1.u32 + 128;
    uint32_t socketStatus;
    try {
        socketStatus = winsockCall(ctx, base, "NetDll_WSAStartup", MAKEWORD(2, 2), output);
    } catch (...) {
        originalCall(ctx, base, sub_82860E88, pool);
        throw;
    }
    if (!socketStatus) {
        const uint16_t negotiated = uint16_t(uint16_t(base[output]) << 8 | base[output + 1]);
        if (negotiated != MAKEWORD(2, 2)) {
            if (winsockCall(ctx, base, "NetDll_WSACleanup"))
                fail("Native network bootstrap could not release rejected Winsock version");
            socketStatus = WSAVERNOTSUPPORTED;
        }
    }
    if (socketStatus) {
        originalCall(ctx, base, sub_82860E88, pool);
        const uint32_t failure = originalCall(ctx, base, sub_82851A50, socketStatus);
        ctx.r3.s64 = int32_t(failure);
        return;
    }
    // Actual original timer helper supplies the service's tick baseline. Its
    // source is the real kernel clock mapped by the native runtime.
    uint32_t tick;
    try {
        tick = originalCall(ctx, base, sub_825FE810, 0);
    } catch (...) {
        const uint32_t cleanupStatus = winsockCall(ctx, base, "NetDll_WSACleanup");
        originalCall(ctx, base, sub_82860E88, pool);
        if (cleanupStatus) fail("Native network timer failure could not release its Winsock reference");
        throw;
    }
    memory->write32(owner + 44, tick);
    const uint64_t count = bootstraps.fetch_add(1, std::memory_order_relaxed) + 1;
    std::fprintf(stderr, "[NETWORK] Native offline socket foundation #%llu; original CPU network engine can configure local state\n",
        static_cast<unsigned long long>(count));
    ctx.r3.u64 = 0;
}
}
uint64_t nativeNetworkBootstrapCount() { return bootstraps.load(std::memory_order_relaxed); }
uint64_t nativeOfflineNetworkEngineCount() { return offlineEngines.load(std::memory_order_relaxed); }
}

PPC_FUNC(sub_82851BA0) { Doritos::Native::initialize(ctx, base); }
PPC_FUNC(sub_82849EB0) { Doritos::Native::createOfflineEngine(ctx, base); }
