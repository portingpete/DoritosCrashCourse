#include "runtime/native/video_menu_entry.h"
#include "runtime/native/input_services.h"
#include "runtime/native/runtime.h"
#include "renderer/native/video_settings_ui.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
PPC_FUNC(sub_82259748);
using namespace Doritos::Native;
namespace {
size_t checks = 0;
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
constexpr uint32_t guiGlobal = 0x82b5ca98, inputGlobal = 0x82bab138, layerGlobal = 0x82b5ab1c;
constexpr char tileName[] = "menus/help_and_options/buttons/native_video_settings";
constexpr char parentName[] = "menus/help_and_options/buttons/buttons";
uint32_t gui, input, manager, layer, group, groups, screen, parent, master, parentMaster, name, parentText;
uint32_t callerStack;
unsigned requests, forwards, visibleCalls, enabledCalls;
bool focused, visible, enabled, failVisible, failEnabled;

void clobber(PPCContext& ctx) {
    ctx.r5.u64 = 0xdeadbeef; ctx.r6.u64 = 0xbadbad; ctx.r31.u64 = 0xabcabc;
    ctx.lr = 0x82260000; ctx.ctr.u64 = 0x82260004; ctx.f31.f64 = -99.5;
    ctx.fpscr.storeFromGuest(PPC_ROUND_UP);
}
void callback(PPCContext& ctx, uint8_t* base) {
    require(memory && base == memory->base() && ctx.r3.u32 == group,
        "The original ancestor predicate receives the exact cloned group and guest base");
    require(ctx.r1.u32 == callerStack - 512 && memory->valid(ctx.r1.u32 - 512, 1024) &&
        memory->read32(ctx.r1.u32) == callerStack,
        "The predicate uses a separate bounded guest stack with the caller backchain");
}
void unchangedExceptResult(const PPCContext& actual, const PPCContext& before) {
    require(actual.r1.u64 == before.r1.u64 && actual.r4.u64 == before.r4.u64 &&
        actual.r5.u64 == before.r5.u64 && actual.r6.u64 == before.r6.u64 && actual.r31.u64 == before.r31.u64 &&
        actual.lr == before.lr && actual.ctr.u64 == before.ctr.u64 && actual.f31.u64 == before.f31.u64,
        "Native menu activation preserves caller state despite original predicate register clobbers");
    require(actual.fpscr.csr == before.fpscr.csr && PPCFPSCRRegister{}.getcsr() == before.fpscr.csr,
        "Predicate success and failure restore the caller's host floating-point state");
}
void reset(PPCContext& ctx) {
    requests = forwards = visibleCalls = enabledCalls = 0;
    focused = visible = enabled = true; failVisible = failEnabled = false;
    memory->write32(guiGlobal, gui); memory->write32(inputGlobal, input); memory->write32(layerGlobal, manager);
    memory->write32(gui, 0x8213bb34); memory->write32(gui + 16420, 0); memory->write32(gui + 7768, screen);
    memory->write32(input, 0x8214fd54); memory->write32(input + 12, 0);
    for (uint32_t user = 0; user != 4; ++user) memory->write32(manager + 56 + user * 4, layer);
    memory->write32(layer, 0x8215e8fc); memory->write32(layer + 116, 0x81); memory->base()[layer + 196] = 1;
    memory->write32(layer + 140, groups); memory->write32(layer + 144, groups + 4); memory->write32(groups, group);
    memory->write32(group, 0x8214f714); memory->write32(group + 104, parent);
    memory->write32(group + 120, master); memory->write32(group + 124, screen); memory->write32(group + 168, layer);
    memory->base()[group + 128] = 0; memory->base()[group + 173] = 1;
    memory->write32(master, 0x8215bf6c); memory->write32(master + 8, name);
    std::memcpy(memory->base() + name, tileName, sizeof(tileName));
    memory->write32(parent, 0x8214f714); memory->write32(parent + 120, parentMaster); memory->write32(parent + 124, screen);
    memory->write32(parentMaster, 0x8215bf6c); memory->write32(parentMaster + 8, parentText);
    std::memcpy(memory->base() + parentText, parentName, sizeof(parentName));
    ctx.r3.u64 = group; ctx.r4.u64 = 3; ctx.r5.u64 = 0x55555555; ctx.r6.u64 = 0x66666666;
    ctx.r31.u64 = 0x31313131; ctx.lr = 0x82260008; ctx.ctr.u64 = 0x8226000c; ctx.f31.f64 = 31.25;
    ctx.fpscr.storeFromGuest(PPC_ROUND_NEAREST);
    callerStack = ctx.r1.u32;
}
}
namespace Doritos::Native { bool nativeInputFocused() { return focused; } }
namespace Doritos::Graphics { void showNativeVideoSettings() { ++requests; } }
extern "C" PPC_FUNC(__imp__sub_82259748) {
    require(base == memory->base(), "Ordinary RunCmd retains the original guest base");
    ++forwards; clobber(ctx); ctx.r3.u64 = 0xface;
}
PPC_FUNC(sub_82275390) {
    callback(ctx, base); ++visibleCalls; clobber(ctx);
    if (failVisible) throw NativeFailure("Original visibility failure");
    ctx.r3.u64 = visible ? 1 : 0;
}
PPC_FUNC(sub_82275410) {
    callback(ctx, base); ++enabledCalls; clobber(ctx);
    if (failEnabled) throw NativeFailure("Original enabled failure");
    ctx.r3.u64 = enabled ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        Memory guest(loadOriginalXex(argv[1])); memory = &guest;
        constexpr std::array<std::pair<uint32_t, uint32_t>, 6> originalWords{{
            {0x8214f85c, 0x82259678}, {0x82259678, 0x7d8802a6}, {0x82259748, 0x7d8802a6},
            {0x82275390, 0x7d8802a6}, {0x82275410, 0x7d8802a6}, {0x8214f714, 0x822b4bd8}
        }};
        for (const auto& [address, word] : originalWords)
            require(guest.read32(address) == word, "The private original XEX pins RunCmd slot, entry and ancestor predicate ABI");
        PPCContext ctx{}; guest.initializeThread(ctx, 0x100000); currentContext = &ctx;
        gui = guest.allocate(20480); input = guest.allocate(4096); manager = guest.allocate(4096);
        layer = guest.allocate(4096); group = guest.allocate(4096); groups = guest.allocate(4096);
        screen = guest.allocate(4096); parent = guest.allocate(4096); master = guest.allocate(4096);
        parentMaster = guest.allocate(4096); name = guest.allocate(4096); parentText = guest.allocate(4096);
        reset(ctx); const PPCContext caller = ctx;
        sub_82259748(ctx, guest.base());
        require(requests == 1 && !forwards && visibleCalls == 1 && enabledCalls == 1 && ctx.r3.u64 == 1,
            "The owned fifth tile posts exactly one native settings request through its original RunCmd route");
        unchangedExceptResult(ctx, caller);

        // These are independent ownership changes, each of which must reject a
        // known native tile without forwarding a synthetic command to Lua.
        const std::array<std::function<void()>, 20> invalidOwners{{
            [] { focused = false; }, [] { memory->write32(guiGlobal, 0); },
            [] { memory->write32(gui, 0); }, [] { memory->write32(inputGlobal, 0); },
            [] { memory->write32(input, 0); }, [] { memory->write32(input + 12, 1); },
            [] { memory->write32(layerGlobal, 0); }, [] { memory->write32(gui + 16420, 4); },
            [] { memory->write32(manager + 56, 0); }, [] { memory->write32(layer, 0); },
            [] { memory->base()[layer + 196] = 0; }, [] { memory->write32(layer + 116, 0x80); },
            [] { memory->write32(gui + 7768, 0); }, [] { memory->write32(group + 124, parent); },
            [] { memory->write32(group + 168, parent); }, [] { memory->base()[group + 128] = 1; },
            [] { memory->base()[group + 173] = 0; }, [] { memory->write32(group + 104, 0); },
            [] { memory->write32(parent + 124, group); }, [] { memory->base()[parentText] = 'x'; }
        }};
        for (const auto& change : invalidOwners) {
            reset(ctx); change(); const PPCContext before = ctx; sub_82259748(ctx, guest.base());
            require(!requests && !forwards && !visibleCalls && !enabledCalls && ctx.r3.u64 == 0,
                "A known tile with lost focus, stale menu owner or inactive ancestor is rejected before guest predicates");
            unchangedExceptResult(ctx, before);
        }
        for (unsigned malformed = 0; malformed != 5; ++malformed) {
            reset(ctx);
            if (malformed == 0) memory->write32(groups, parent);
            else if (malformed == 1) memory->write32(layer + 144, groups);
            else if (malformed == 2) memory->write32(layer + 144, groups + 5);
            else if (malformed == 3) memory->write32(layer + 144, groups + 4097 * 4);
            else memory->write32(layer + 140, groups + 1);
            sub_82259748(ctx, guest.base());
            require(!requests && !forwards && !visibleCalls && ctx.r3.u64 == 0,
                "A removed tile or malformed current-layer member vector cannot open native settings");
        }
        reset(ctx); visible = false; sub_82259748(ctx, guest.base());
        require(!requests && !forwards && visibleCalls == 1 && !enabledCalls && ctx.r3.u64 == 0,
            "Original inherited invisibility rejects the tile and short circuits the enabled predicate");
        reset(ctx); enabled = false; sub_82259748(ctx, guest.base());
        require(!requests && !forwards && visibleCalls == 1 && enabledCalls == 1 && ctx.r3.u64 == 0,
            "Original inherited disability rejects the tile even when the group is active");

        reset(ctx); memory->write32(gui + 16420, 2); memory->write32(layer + 116, 0x84);
        sub_82259748(ctx, guest.base());
        require(requests == 1 && !forwards, "The actual GUI owner can open native settings for another controller user");
        const uint64_t stack = ctx.r1.u64;
        reset(ctx); ctx.r1.u64 = 512; sub_82259748(ctx, guest.base());
        require(!requests && !forwards && !visibleCalls && ctx.r1.u64 == 512 && ctx.r3.u64 == 0,
            "An invalid guest callback stack rejects safely before original predicates");
        ctx.r1.u64 = stack;

        for (unsigned original = 0; original != 4; ++original) {
            reset(ctx);
            if (original == 0) ctx.r4.u64 = 0;
            else if (original == 1) memory->base()[name + sizeof(tileName) - 1] = 'x';
            else if (original == 2) memory->write32(master, 0);
            else memory->write32(group, 0);
            sub_82259748(ctx, guest.base());
            require(!requests && forwards == 1 && !visibleCalls && ctx.r3.u64 == 0xface && ctx.r31.u64 == 0xabcabc,
                "Original groups, other command indexes and similar names retain the complete authored RunCmd implementation");
        }
        reset(ctx); const PPCContext beforeMismatch = ctx;
        require(!handleNativeVideoMenuCommand(ctx, guest.base() + 32), "A foreign base is never treated as the native tile");
        unchangedExceptResult(ctx, beforeMismatch);
        require(ctx.r3.u64 == beforeMismatch.r3.u64, "An unrecognized request does not alter original arguments");
        for (bool visibilityFailure : {true, false}) {
            reset(ctx); failVisible = visibilityFailure; failEnabled = !visibilityFailure;
            const PPCContext before = ctx; bool failed = false;
            try { sub_82259748(ctx, guest.base()); } catch (const NativeFailure&) { failed = true; }
            require(failed && !requests && !forwards && ctx.r3.u64 == before.r3.u64,
                "Original predicate failures propagate without opening a window or changing caller arguments");
            unchangedExceptResult(ctx, before);
        }
        std::printf("Native video menu entry contracts passed: %zu checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native video menu entry contract failed: %s\n", error.what()); return 1;
    }
}
