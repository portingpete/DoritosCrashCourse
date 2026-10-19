#include "local_profile_adaptation.h"
#include "runtime.h"
#include "user_services.h"
#include <atomic>
#include <cstdio>

PPC_FUNC(sub_821AA7F8);
PPC_FUNC(sub_821AB120);
PPC_FUNC(sub_821AAFA0);

namespace Doritos::Native {
namespace {
constexpr uint32_t profileArrayGlobal = 0x82b5caa4;
constexpr uint32_t guiGlobal = 0x82b5ca98;
constexpr uint32_t titleProfileTable = 0x8214d6e8;
constexpr uint32_t localProfileTable = 0x8214d714;
std::atomic<uint64_t> acceptedQueries{0};

void span(uint32_t address, uint32_t bytes) {
    if (!memory || !address || !memory->valid(address, bytes))
        fail("Native local-profile query has an invalid original guest buffer");
}

struct CallFrame {
    PPCContext& ctx;
    uint64_t stack, link;
    explicit CallFrame(PPCContext& context) : ctx(context), stack(context.r1.u64), link(context.lr) {
        if (ctx.r1.u32 < 1024) fail("Native local-profile query has an invalid guest stack");
        span(ctx.r1.u32 - 1024, 1024);
        ctx.r1.u64 = ctx.r1.u32 - 512;
        memory->write32(ctx.r1.u32, uint32_t(stack));
    }
    ~CallFrame() { ctx.r1.u64 = stack; ctx.lr = link; }
};

uint32_t sdk(PPCContext& ctx, uint8_t* base, const char* name, uint32_t user, uint32_t flags, uint32_t output) {
    const PPCContext saved = ctx;
    ctx.r3.u64 = user; ctx.r4.u64 = flags; ctx.r5.u64 = output;
    if (!userServicesImport(ctx, base, name)) fail("Native local-profile query has no matching identity service");
    const uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
}

bool selectedOfflineUser(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t scratch) {
    if (user >= 4) return false;
    const uint64_t selected = nativeSelectedLocalProfile(user);
    if (!selected) return false;
    span(profileArrayGlobal, 4);
    const uint32_t profiles = memory->read32(profileArrayGlobal);
    span(profiles, 16);
    const uint32_t profile = memory->read32(profiles + 4 * user);
    span(profile, 2516);
    // Original title constructor 822315E8 and user setter 82231FB0 establish
    // this real object. Its embedded settings interface tracks local SDK
    // state 1; the shared platform predicate 823A7600 remains unchanged.
    if (memory->read32(profile) != titleProfileTable || memory->read32(profile + 4) != user ||
        memory->read32(profile + 1096) != localProfileTable || memory->read32(profile + 1100) != user)
        fail("Native local-profile query did not resolve the actual original title profile");
    if (memory->read32(profile + 2176) != 1) return false;
    if (sdk(ctx, base, "XamUserGetSigninState", user, 0, 0) != 1) return false;
    if (sdk(ctx, base, "XamUserGetSigninInfo", user, 0, scratch)) return false;
    if (memory->read64(scratch) != selected || memory->read32(scratch + 12) != 1 ||
        (memory->read32(scratch + 8) & 2)) return false;
    if (sdk(ctx, base, "XamUserGetXUID", user, 1, scratch + 48)) return false;
    return memory->read64(scratch + 48) == selected;
}

void query(PPCContext& ctx, uint8_t* base) {
    if (!memory || base != memory->base()) fail("Native local-profile query has no original memory mapping");
    const PPCContext caller = ctx;
    const uint32_t result = ctx.r3.u32, arguments = ctx.r5.u32;
    span(result, 8); span(arguments, 32);
    const uint32_t argumentCount = memory->read32(arguments + 12);
    if (argumentCount > 64) fail("Native local-profile query has an unbounded original argument list");
    {
        CallFrame frame(ctx);
        const uint32_t temporary = ctx.r1.u32 + 84;
        uint32_t user;
        // Retain both original 821BAC60 argument branches. Shipped splash Lua
        // passes gui.GetInputDevice() explicitly; other authored queries can
        // use the actual current input owner at GUI +16420.
        if (argumentCount > 1) {
            ctx.r3.u64 = arguments; ctx.r4.u64 = temporary; ctx.r5.u64 = 1;
            sub_821AA7F8(ctx, base);
            user = memory->read32(temporary);
        } else {
            span(guiGlobal, 4);
            const uint32_t owner = memory->read32(guiGlobal);
            span(owner, 16424);
            user = memory->read32(owner + 16420);
        }
        const bool logged = selectedOfflineUser(ctx, base, user, ctx.r1.u32 + 128);
        ctx.r3.u64 = result;
        if (logged) {
            base[ctx.r1.u32 + 80] = 1;
            ctx.r4.u64 = ctx.r1.u32 + 80;
            sub_821AB120(ctx, base);
            const uint64_t count = acceptedQueries.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1) {
                std::printf("[native-local-profile] Original local-user query accepts selected offline account; user %u\n", user);
                std::fflush(stdout);
            }
        } else {
            // Original false-result construction, including its value cleanup.
            memory->write32(result + 4, 0); memory->write32(temporary, 6);
            ctx.r4.u64 = temporary; ctx.r5.u64 = result; ctx.r6.u64 = 0;
            sub_821AAFA0(ctx, base);
        }
    }
    ctx = caller;
    ctx.r3.u64 = result;
}
}
uint64_t nativeLocalProfileQueryCount() { return acceptedQueries.load(std::memory_order_relaxed); }
}

// This single original Lua query bridges the title's Xbox-only splash predicate
// to the user's real selected Windows offline profile. IsCurrentUserLiveLogged,
// the shared 823A7600 predicate, profile objects and every Live session/posting
// gate continue to run their original code and receive actual SDK state 1.
PPC_FUNC(sub_821BAC60) { Doritos::Native::query(ctx, base); }
