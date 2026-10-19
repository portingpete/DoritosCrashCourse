#include "runtime/native/runtime.h"
#include "runtime/native/local_profile_adaptation.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
PPC_FUNC(sub_821BAC60);
using namespace Doritos::Native;
namespace {
void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
constexpr uint64_t actualId = 0xe123456789abcdefULL;
uint64_t selected = 0, infoId = actualId, xuid = actualId;
uint32_t sdkState = 1, infoState = 1, infoFlags = 0, infoError = 0, xuidError = 0;
uint32_t arguments = 0, output = 0;
unsigned parses = 0, trueValues = 0, falseValues = 0, sdkQueries = 0;
bool parserFailure = false;
}

// Isolated original-CPU and platform-service boundaries. The chooser and its
// actual Windows identity/persistence have separate native service tests; this
// contract proves their identity is required, never manufactures a selection.
namespace Doritos::Native {
uint64_t nativeSelectedLocalProfile(uint32_t user) { return user == 0 ? selected : 0; }
bool userServicesImport(PPCContext& ctx, uint8_t*, const char* name) {
    require(ctx.r3.u32 == 0, "unselected additional users never reach an identity query");
    ++sdkQueries;
    if (!strcmp(name, "XamUserGetSigninState")) { ctx.r3.u64 = sdkState; return true; }
    if (!strcmp(name, "XamUserGetSigninInfo")) {
        require(ctx.r4.u32 == 0 && memory->valid(ctx.r5.u32, 40), "actual default sign-in-info ABI");
        if (!infoError) {
            memory->write64(ctx.r5.u32, infoId); memory->write32(ctx.r5.u32 + 8, infoFlags);
            memory->write32(ctx.r5.u32 + 12, infoState);
        }
        ctx.r3.u64 = infoError; return true;
    }
    if (!strcmp(name, "XamUserGetXUID")) {
        require(ctx.r4.u32 == 1 && memory->valid(ctx.r5.u32, 8), "actual offline-only identity mask");
        if (!xuidError) memory->write64(ctx.r5.u32, xuid);
        ctx.r3.u64 = xuidError; return true;
    }
    return false;
}
}
PPC_FUNC(sub_821AA7F8) {
    require(ctx.r3.u32 == arguments && ctx.r4.u32 == ctx.r1.u32 + 84 && ctx.r5.u32 == 1,
        "original explicit user argument parser ABI is retained");
    ++parses;
    if (parserFailure) throw NativeFailure("injected original argument parser failure");
    memory->write32(ctx.r4.u32, memory->read32(arguments + 24));
    ctx.r31.u64 = 0xbadc0de; ctx.r6.u64 = 0xdecafbad;
}
PPC_FUNC(sub_821AB120) {
    require(ctx.r3.u32 == output && ctx.r4.u32 == ctx.r1.u32 + 80 && base[ctx.r4.u32] == 1,
        "original true Lua value constructor receives its actual boolean");
    base[output] = 1; memory->write32(output + 4, 6); ++trueValues;
    ctx.r31.u64 = 0xf00d;
}
PPC_FUNC(sub_821AAFA0) {
    require(ctx.r3.u32 == output && ctx.r4.u32 == ctx.r1.u32 + 84 && ctx.r5.u32 == output &&
        ctx.r6.u32 == 0 && memory->read32(ctx.r4.u32) == 6 && !memory->read32(output + 4),
        "original false Lua value cleanup and constructor ABI are retained");
    base[output] = 0; memory->write32(output + 4, 6); ++falseValues;
    ctx.r31.u64 = 0xbeef;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        Memory guest(loadOriginalXex(argv[1])); memory = &guest;
        require(guest.read32(0x8213d050) == 0x821bac60 &&
            guest.read32(0x8214d6e8 + 4) == 0x823a7600 &&
            guest.read32(0x8214d714 + 4) == 0x82255090 &&
            guest.read32(0x823a7600) == 0x816309d0 &&
            guest.read32(0x82231fb0) == 0x9083044c,
            "original local-query binding, separate Live predicate and title profile ABI");
        PPCContext ctx{}; guest.initializeThread(ctx, 0x100000); currentContext = &ctx;
        arguments = guest.allocate(4096); output = guest.allocate(4096);
        const uint32_t profiles = guest.allocate(4096), profile = guest.allocate(4096);
        const uint32_t owner = guest.allocate(20480);
        guest.write32(0x82b5caa4, profiles); guest.write32(profiles, profile);
        guest.write32(0x82b5ca98, owner); guest.write32(owner + 16420, 0);
        guest.write32(profile, 0x8214d6e8); guest.write32(profile + 4, 0);
        guest.write32(profile + 1096, 0x8214d714); guest.write32(profile + 1100, 0);
        guest.write32(profile + 2176, 1); guest.write32(profile + 2512, 0);
        guest.write32(arguments + 12, 2); guest.write32(arguments + 24, 0);
        std::array<uint8_t, 4096> before{}; memcpy(before.data(), guest.base() + profile, before.size());
        auto query = [&] {
            ctx.r3.u64 = output; ctx.r4.u64 = 0x11112222; ctx.r5.u64 = arguments;
            ctx.r6.u64 = 0x33334444; ctx.r31.u64 = 0x55556666; ctx.lr = 0x8230ff14;
            const PPCContext caller = ctx;
            sub_821BAC60(ctx, guest.base());
            require(ctx.r1.u64 == caller.r1.u64 && ctx.lr == caller.lr && ctx.r3.u32 == output &&
                ctx.r4.u64 == caller.r4.u64 && ctx.r5.u64 == caller.r5.u64 && ctx.r6.u64 == caller.r6.u64 &&
                ctx.r31.u64 == caller.r31.u64, "original caller stack, return object and nonvolatile state are retained");
            require(guest.read32(output + 4) == 6 && !memcmp(before.data(), guest.base() + profile, before.size()),
                "only actual result changes; no title profile, Live state or readiness is fabricated");
            return guest.base()[output] != 0;
        };
        require(!query() && sdkQueries == 0 && nativeLocalProfileQueryCount() == 0,
            "an available Windows account does not count as an explicit user choice");
        selected = actualId;
        require(query() && trueValues == 1 && nativeLocalProfileQueryCount() == 1,
            "selected offline non-guest identity is accepted without Live state 2");
        selected = actualId + 1; require(!query(), "selection must equal actual sign-in identity"); selected = actualId;
        infoId ^= 1; require(!query(), "changed sign-in identity is not accepted"); infoId = actualId;
        xuid ^= 1; require(!query(), "offline XUID must independently match selected identity"); xuid = actualId;
        infoFlags = 2; require(!query(), "a guest identity cannot satisfy the non-guest splash"); infoFlags = 0;
        sdkState = 2; require(!query(), "native local adaptation does not claim a Live identity"); sdkState = 1;
        infoState = 2; require(!query(), "sign-in info must also describe actual offline state 1"); infoState = 1;
        infoError = 0x80070525; require(!query(), "actual sign-in query failure remains false"); infoError = 0;
        xuidError = 0x80070525; require(!query(), "actual offline identity failure remains false"); xuidError = 0;
        guest.write32(profile + 2176, 0); memcpy(before.data(), guest.base() + profile, before.size());
        require(!query(), "original embedded local profile must already track its actual local account");
        guest.write32(profile + 2176, 1); memcpy(before.data(), guest.base() + profile, before.size());
        guest.write32(arguments + 24, 1); const unsigned previousQueries = sdkQueries;
        require(!query() && sdkQueries == previousQueries, "additional player identities are not invented");
        guest.write32(arguments + 24, UINT32_MAX); require(!query(), "invalid explicit user remains logged out");
        guest.write32(arguments + 24, 0); guest.write32(arguments + 12, 1);
        const unsigned previousParses = parses;
        require(query() && parses == previousParses, "default query retains the actual original current input owner");
        guest.write32(owner + 16420, UINT32_MAX); require(!query(), "no current owner remains logged out");
        guest.write32(owner + 16420, 0); guest.write32(arguments + 12, 2);
        auto rejects = [&] { try { query(); } catch (const NativeFailure&) { return true; } return false; };
        guest.write32(profile, 0x82163924); require(rejects(), "generic platform objects cannot replace the actual title profile");
        guest.write32(profile, 0x8214d6e8);
        guest.write32(profile + 4, 1); require(rejects(), "original registered profile index must match"); guest.write32(profile + 4, 0);
        guest.write32(profile + 1096, 0x82163954); require(rejects(), "unknown embedded profile interfaces are rejected");
        guest.write32(profile + 1096, 0x8214d714);
        guest.write32(profile + 1100, 1); require(rejects(), "embedded account ownership must match"); guest.write32(profile + 1100, 0);
        guest.write32(arguments + 12, 65); require(rejects(), "unbounded original argument lists stop explicitly");
        guest.write32(arguments + 12, 2); parserFailure = true;
        const uint64_t stack = ctx.r1.u64; require(rejects() && ctx.r1.u64 == stack,
            "actual CPU helper failure restores stack without a successful result");
        parserFailure = false;
        require(!memcmp(before.data(), guest.base() + profile, before.size()) && guest.read32(profile + 2512) == 0,
            "all failed or accepted queries preserve the shared Xbox Live profile state");
        std::printf("Native local-profile adaptation contract passed: %u original parses, %u true / %u false values\n",
            parses, trueValues, falseValues);
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
