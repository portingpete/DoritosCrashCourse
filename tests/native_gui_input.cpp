#include "runtime/native/gui_input.h"
#include "runtime/native/input_services.h"
#include "runtime/native/mouse_services.h"
#include "runtime/native/runtime.h"
#include "renderer/native/gui_pointer.h"
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
PPC_FUNC(sub_821A13F0);
PPC_FUNC(sub_821B9A70);
using namespace Doritos::Native;
namespace {
constexpr uint32_t guiGlobal = 0x82b5ca98, inputGlobal = 0x82bab138, gameGlobal = 0x82b5caa0;
constexpr uint32_t layerGlobal = 0x82b5ab1c;
constexpr uint32_t guiTable = 0x8213bb34, mouseTable = 0x8214fd54, stickTable = 0x8214fdb8, layerTable = 0x8215e8fc;
size_t checks = 0;
void require(bool value, const char* label) { ++checks; if (!value) throw std::runtime_error(label); }
bool close(float a, float b) { return std::fabs(a - b) < 0.00001f; }
uint32_t gui = 0, input = 0, manager = 0, layer = 0, replacementLayer = 0, control = 0, controls = 0, game = 0;
uint32_t screen = 0, master = 0, name = 0, invalidControl = 0;
uint32_t rawInput = 0;
uint32_t callerStack = 0;
bool focused = true, gameplay = false, failDispatch = false, transitionOnDispatch = false, receiverOnPosition = false;
bool transitionOnButton = false, focusOnPosition = false;
bool pointerSelection = false, motionActivity = true;
std::optional<NativeMousePosition> latestPointer;
unsigned recipientMutation = 0;
unsigned wheelFailure = 0, wheelTransition = 0;
unsigned contextChanges = 0, originalUpdates = 0, originalGetters = 0;
std::vector<NativeMouseEvent> queued;
std::vector<std::string> calls;
std::vector<std::pair<float, float>> positions;
std::vector<std::pair<bool, uint64_t>> buttons;
std::vector<uint32_t> directionStates;
PPCContext originalResult{};
Doritos::Graphics::GuiPointerContext pointerOwner{}, presentedOwner{};
Doritos::Graphics::GuiPointerResult mappedPointer{};
unsigned pointerContexts = 0;
std::vector<std::pair<float,float>> mappingInputs;

void callbackFrame(PPCContext& ctx, uint8_t* base) {
    require(memory && base == memory->base() && ctx.r1.u32 < callerStack && memory->valid(ctx.r1.u32, 256),
        "Authored input callbacks use a valid separate guest stack frame");
    require(memory->read32(ctx.r1.u32) == callerStack, "Callback guest frame retains the caller backchain");
}
void clobber(PPCContext& ctx) {
    ctx.r31.u64 = 0xbad0bad0bad0bad0ULL; ctx.r6.u64 = 0xdeadbeef;
    ctx.lr = 0x82260000; ctx.ctr.u64 = 0x82260004; ctx.f31.f64 = -99.5;
}
void sameContext(const PPCContext& actual, const PPCContext& expected) {
    require(actual.r1.u64 == expected.r1.u64 && actual.r3.u64 == expected.r3.u64 && actual.r4.u64 == expected.r4.u64 &&
        actual.r5.u64 == expected.r5.u64 && actual.r6.u64 == expected.r6.u64 && actual.r31.u64 == expected.r31.u64 &&
        actual.lr == expected.lr && actual.ctr.u64 == expected.ctr.u64 && actual.f31.u64 == expected.f31.u64,
        "Native pointer dispatch preserves the original GUI update's returned context");
}
void owners() {
    memory->write32(guiGlobal, gui); memory->write32(gui, guiTable); memory->write32(gui + 16420, 0);
    memory->write32(gui + 7768, screen); memory->write32(screen + 8, master);
    memory->base()[screen + 100] = 0;
    memory->write64(master + 16,0xa23456789abcdef0ULL);
    memory->write32(0x82b5caa8, rawInput); memory->write32(rawInput, 0x821612b4);
    memory->write32(rawInput + 1684, 0x76543210);
    memory->write32(rawInput + 1688, std::bit_cast<uint32_t>(0.1f)); memory->write32(rawInput + 1692, std::bit_cast<uint32_t>(0.2f));
    memory->write32(rawInput + 1696, std::bit_cast<uint32_t>(0.3f)); memory->write32(rawInput + 1700, std::bit_cast<uint32_t>(0.4f));
    memory->write32(inputGlobal, input); memory->write32(input, mouseTable); memory->write32(input + 8, stickTable);
    memory->write32(input + 12, 0);
    for (uint32_t user = 0; user < 4; ++user) {
        memory->write32(input + 36 + user*4,0);
        memory->write32(input + 52 + user*4,0);
    }
    memory->write32(layerGlobal, manager); memory->write32(manager + 56, layer); memory->write32(manager + 84, layer);
    for (uint32_t object : {layer, replacementLayer}) {
        memory->write32(object, layerTable); memory->write32(object + 116, 0x81); memory->write32(object + 192, 0x81);
        memory->base()[object + 196] = 1; memory->base()[object + 197] = 0;
        memory->write32(object + 140, controls); memory->write32(object + 144, controls + 4); memory->write32(object + 164, 1);
    }
    memory->write32(controls, control); memory->write32(control, 0x8214f714); memory->write32(control + 168, layer);
    memory->write64(control + 16, 0x123456789abcdef0ULL);
    memory->write32(control + 120, 0); memory->write32(control + 124, 0); memory->base()[control + 128] = 0;
    memory->base()[control + 173] = 1; memory->base()[control + 174] = 0; memory->base()[control + 175] = 0;
    memory->write32(gameGlobal, 0);
    memory->base()[game + 48] = 0;
    memory->write64(gui + 15784, UINT64_MAX);
}
void reset() {
    owners(); focused = true; gameplay = false; failDispatch = false; transitionOnDispatch = false; receiverOnPosition = false;
    transitionOnButton = false; focusOnPosition = false;
    pointerSelection = false; motionActivity = true; latestPointer.reset();
    recipientMutation = 0; wheelFailure = 0; wheelTransition = 0;
    contextChanges = 0; originalUpdates = 0; originalGetters = 0; queued.clear(); calls.clear(); positions.clear(); buttons.clear();
    directionStates.clear();
    pointerOwner = {}; presentedOwner = {}; mappedPointer = {}; pointerContexts = 0; mappingInputs.clear();
}
void pointer(PPCContext& ctx, std::initializer_list<NativeMouseEvent> events) {
    queued = events; callerStack = ctx.r1.u32;
    updateNativeGuiInput(ctx, memory->base(), gui);
}
uint32_t currentDevice(PPCContext& ctx) {
    sub_821B9A70(ctx, memory->base()); return ctx.r3.u32;
}
}

// Only this isolated test target replaces host queue reads and authored guest
// CPU boundaries. Production dispatch uses actual Win32 events and original
// GUI methods; the XEX checks below pin their true ABI and ownership.
namespace Doritos::Native {
std::vector<NativeMouseEvent> takeNativeMouseEvents() {
    // Model the window-thread snapshot independently of guest event dispatch:
    // blocked actions are still consumed, but their current position remains.
    for (const auto& event : queued) {
        if (event.kind == NativeMouseEventKind::Leave || event.kind == NativeMouseEventKind::Cancel) {
            latestPointer.reset(); pointerSelection = false; continue;
        }
        if (!std::isfinite(event.x) || !std::isfinite(event.y) ||
            event.x < 0 || event.x > 1 || event.y < 0 || event.y > 1) continue;
        latestPointer = NativeMousePosition{event.x,event.y};
        if (event.kind == NativeMouseEventKind::Wheel) pointerSelection = false;
        else if ((event.kind == NativeMouseEventKind::Move && motionActivity) ||
            ((event.kind == NativeMouseEventKind::ButtonDown || event.kind == NativeMouseEventKind::ButtonUp) &&
             event.button != NativeMouseButton::Middle)) pointerSelection = true;
    }
    return std::exchange(queued, {});
}
std::optional<NativeMousePosition> latestNativeMousePosition() { return latestPointer; }
void clearNativeMousePosition() { latestPointer.reset(); pointerSelection = false; }
bool nativeMouseOwnsSelection() { return pointerSelection; }
bool nativeInputFocused() { return focused; }
void setNativeInputGameplay(bool value) { gameplay = value; ++contextChanges; }
}
namespace Doritos::Graphics {
GuiPointerResult mapTitlePresentationPointer(float x,float y) {return {GuiPointerMapping::Hit,x,y};}
void setTitleGuiPointerContext(const GuiPointerContext& context) { pointerOwner = context; ++pointerContexts; }
GuiPointerResult mapTitleGuiPointer(const GuiPointerContext& context, float x, float y) {
    require(context == pointerOwner, "Pointer projection uses the current published GUI screen and full root generation");
    mappingInputs.emplace_back(x,y);
    if (mappedPointer.mapping != GuiPointerMapping::Flat && context != presentedOwner)
        return {GuiPointerMapping::Pending,0,0};
    return mappedPointer;
}
}
extern "C" PPC_FUNC(__imp__sub_821A13F0) {
    require(ctx.r3.u32 == gui, "Original GUI update receives its registered owner");
    ++originalUpdates;
    ctx.r3.u64 = 0xfeed0001; ctx.r4.u64 = 0xfeed0002; ctx.r5.u64 = 0xfeed0003; ctx.r6.u64 = 0xfeed0004;
    ctx.r31.u64 = 0xfeed0031; ctx.lr = 0x821a1400; ctx.ctr.u64 = 0xfeed0020; ctx.f31.f64 = 19.5;
    originalResult = ctx;
}
extern "C" PPC_FUNC(__imp__sub_821B9A70) { ++originalGetters; ctx.r3.u64 = 7; }
PPC_FUNC(sub_8231ED88) {
    callbackFrame(ctx, base); calls.push_back("bind");
    require(ctx.r3.u32 == layer && ctx.r4.u32 == (memory->read32(layer + 116) | 0x80),
        "Mouse ownership uses the original layer-mask setter and preserves authored input bits");
    memory->write32(layer + 116, ctx.r4.u32); memory->write32(layer + 192, ctx.r4.u32);
    memory->write32(manager + 84, layer); clobber(ctx);
}
PPC_FUNC(sub_8224DBA8) {
    callbackFrame(ctx, base); calls.push_back("transform");
    const uint32_t x = ctx.r3.u32, y = ctx.r4.u32;
    require(memory->valid(x, 4) && memory->valid(y, 4) && y == x + 4,
        "Authored letterbox transform receives adjacent genuine guest float pointers");
    const float horizontal = std::bit_cast<float>(memory->read32(x));
    const float vertical = std::bit_cast<float>(memory->read32(y));
    memory->write32(x, std::bit_cast<uint32_t>(horizontal * 2 + 0.125f));
    memory->write32(y, std::bit_cast<uint32_t>(vertical * 0.5f - 0.125f));
    clobber(ctx);
}
PPC_FUNC(sub_82267C50) {
    callbackFrame(ctx, base); calls.push_back("position");
    require(ctx.r3.u32 == input && !(ctx.r4.u64 >> 32) && !uint32_t(ctx.r5.u64) && ctx.r6.u64 == 0,
        "Original pointer ABI receives input owner, low-word X float and high-word Y float");
    positions.emplace_back(std::bit_cast<float>(ctx.r4.u32), std::bit_cast<float>(uint32_t(ctx.r5.u64 >> 32)));
    if (receiverOnPosition) memory->write32(inputGlobal, 0);
    if (focusOnPosition) focused = false;
    clobber(ctx);
}
PPC_FUNC(sub_82267BC0) {
    callbackFrame(ctx, base); calls.push_back("down");
    require(ctx.r3.u32 == input, "Original left-down callback receives the actual GUIINPUT owner");
    memory->base()[control + 174] = 1; clobber(ctx);
}
PPC_FUNC(sub_82267B30) {
    callbackFrame(ctx, base); calls.push_back("up");
    require(ctx.r3.u32 == input, "Original left-up callback receives the actual GUIINPUT owner");
    memory->base()[control + 175] = 1; clobber(ctx);
}
PPC_FUNC(sub_822679E0) {
    callbackFrame(ctx, base); calls.push_back("dispatch");
    require(ctx.r3.u32 == input, "Original mouse dispatch receives the actual GUIINPUT owner");
    PPCContext getter = ctx;
    require(currentDevice(getter) == 0, "Current-device getter remaps mouse only within native dispatch to the actual GUI user");
    if (failDispatch) throw NativeFailure("isolated original GUI callback failure");
    memory->base()[control + 174] = 0; memory->base()[control + 175] = 0;
    if (transitionOnDispatch) {
        memory->write32(manager + 56, replacementLayer); memory->write32(manager + 84, replacementLayer);
    }
    if (recipientMutation == 1) memory->write32(control + 120, master);
    else if (recipientMutation == 2) memory->write64(control + 16, memory->read64(control + 16) ^ (uint64_t(1) << 40));
    else if (recipientMutation == 3) {
        memory->write32(invalidControl, 0x8214f714); memory->write32(invalidControl + 168, layer);
        memory->write32(controls, invalidControl);
    } else if (recipientMutation == 4) memory->write32(screen + 8, replacementLayer);
    else if (recipientMutation == 5) memory->write64(master + 16,memory->read64(master + 16) ^ (uint64_t(1) << 40));
    clobber(ctx);
}
PPC_FUNC(sub_82268160) {
    callbackFrame(ctx, base); calls.push_back("buttonDown");
    require(ctx.r3.u32 == input + 8 && ctx.r5.u64 == 0 && ctx.r6.u64 == 0,
        "Authored GUI button-down callback retains the embedded joystick and packed button ABI");
    buttons.emplace_back(true, ctx.r4.u64);
    const uint32_t user = uint32_t(ctx.r4.u64 >> 32), key = ctx.r4.u32;
    if (key >= 10 && key <= 13) {
        require(user < 4, "An authored directional key targets an actual controller user");
        const uint32_t bit = 1u << (key-10), opposite = bit ^ (key < 12 ? 3u : 12u);
        const uint32_t address = input + 36 + user*4;
        memory->write32(address,(memory->read32(address) & ~opposite) | bit);
        if (wheelFailure == 1) throw NativeFailure("isolated original directional down failure");
    }
    clobber(ctx);
    if (transitionOnButton) {
        memory->write32(manager + 56, replacementLayer); memory->write32(manager + 84, replacementLayer);
    }
}
PPC_FUNC(sub_82268448) {
    callbackFrame(ctx, base); calls.push_back("buttonUp");
    require(ctx.r3.u32 == input + 8 && ctx.r5.u64 == 0 && ctx.r6.u64 == 0,
        "Authored GUI button-up callback retains the embedded joystick and packed button ABI");
    buttons.emplace_back(false, ctx.r4.u64);
    const uint32_t user = uint32_t(ctx.r4.u64 >> 32), key = ctx.r4.u32;
    if (key >= 10 && key <= 13) {
        require(user < 4, "An authored directional release targets an actual controller user");
        const uint32_t bit = 1u << (key-10), address = input + 36 + user*4;
        if (memory->read32(address) & bit) {
            memory->write32(address,memory->read32(address) & ~bit);
            memory->write32(input + 52 + user*4,std::bit_cast<uint32_t>(0.1f));
        }
        if (wheelFailure == 3) throw NativeFailure("isolated original directional up failure");
    }
    clobber(ctx);
}
PPC_FUNC(sub_82269038) {
    callbackFrame(ctx, base); calls.push_back("direction");
    require(ctx.r3.u32 == input && ctx.r4.u32 == memory->read32(gui + 16420),
        "Authored first directional message receives the true GUIINPUT and player index");
    const uint32_t user = ctx.r4.u32, held = memory->read32(input + 36 + user*4);
    directionStates.push_back(held);
    require(held == 1 || held == 2, "A wheel detent isolates one vertical direction from every held axis");
    memory->write32(input + 52 + user*4,std::bit_cast<uint32_t>(0.3f));
    if (wheelTransition) {
        memory->write32(input + 36 + user*4,0);
        memory->write32(input + 52 + user*4,std::bit_cast<uint32_t>(0.7f));
        if (wheelTransition == 1) {
            memory->write32(manager + 56 + user*4,replacementLayer);
            memory->write32(manager + 84,replacementLayer);
        } else if (wheelTransition == 2) memory->write32(screen + 8,replacementLayer);
        else memory->write32(inputGlobal,0);
    }
    if (wheelFailure == 2) throw NativeFailure("isolated original directional processing failure");
    clobber(ctx);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        Memory guest(loadOriginalXex(argv[1])); memory = &guest;
        constexpr std::array<std::pair<uint32_t, uint32_t>, 20> originalWords{{
            {0x8213bba4, 0x821a13f0}, {0x8214fd54, 0x822676b8}, {0x8224dba8, 0x3d6082bb},
            {0x82267c50, 0x7d8802a6}, {0x82267bc0, 0x7d8802a6}, {0x82267b30, 0x7d8802a6},
            {0x822679e0, 0x7d8802a6}, {0x82267a08, 0x38600007}, {0x82267b58, 0x38600007},
            {0x82267be8, 0x38600007}, {0x82267a88, 0x814b0118}, {0x82267ac8, 0x814b0114},
            {0x82267c1c, 0x9bcb00ae}, {0x8214fdb8, 0x82269db8}, {0x8215e8fc, 0x8231ec88},
            {0x8214f714, 0x822b4bd8}, {0x8231ed88, 0x7d8802a6}, {0x82268160, 0x7d8802a6},
            {0x82268448, 0x7d8802a6}, {0x821b9a70, 0x3d6082bb}
        }};
        for (const auto& [address, word] : originalWords)
            require(guest.read32(address) == word, "Owned original XEX pins GUI update, mouse owner, transform, device-seven and widget ABI");
        const uint32_t inputHigh = guest.read32(0x821b9a70), inputLoad = guest.read32(0x821b9a78);
        require(inputHigh == 0x3d6082bb && inputLoad == 0x816bb138 &&
            uint32_t((inputHigh & 0xffff) << 16) + int16_t(inputLoad & 0xffff) == inputGlobal,
            "Original getter's lis plus signed lwz displacement resolves the real GUIINPUT global, not an unsigned-offset alias");
        require(guest.read32(0x8231f08c) == inputHigh && guest.read32(0x8231f090) == inputLoad,
            "Original layer pointer dispatch independently resolves the same signed GUIINPUT global");
        const auto originalAddress = [&](uint32_t highAddress, uint32_t loadAddress) {
            return uint32_t((guest.read32(highAddress) & 0xffff) << 16) + int16_t(guest.read32(loadAddress) & 0xffff);
        };
        require(guest.read32(0x82267c88) == 0x3d6082b6 && guest.read32(0x82267c8c) == 0x816bab1c &&
            originalAddress(0x82267c88, 0x82267c8c) == layerGlobal,
            "Original mouse hit-test instructions resolve the real layer-manager global using signed addressing");
        require(guest.read32(0x822529dc) == 0x3ce082b6 && guest.read32(0x82252a00) == 0x83e7caa8 &&
            originalAddress(0x822529dc, 0x82252a00) == 0x82b5caa8,
            "Original platform pointer dispatch resolves the exact raw-input cursor-cache owner global");
        constexpr std::array<std::pair<uint32_t,uint32_t>,17> originalDirectionWords{{
            {0x822681a4,0x394b0007}, {0x822681a8,0x554b103a}, {0x822681ac,0x7d4bf82e},
            {0x82268508,0x390b000b}, {0x8226850c,0x7ca9f92e}, {0x82268514,0x5506103a},
            {0x82268510,0x3ce082aa}, {0x82268518,0xc007095c}, {0x8226851c,0x7c06fd2e},
            {0x82269080,0x397e0009}, {0x82269084,0x3d4082aa}, {0x82269088,0x557c103a},
            {0x82269090,0xc3ea04a0}, {0x82269104,0x397e000d}, {0x8226910c,0x7feafd2e},
            {0x82aa04a0,0x3e99999a}, {0x82aa095c,0x3dcccccd}
        }};
        for (const auto& [address,word] : originalDirectionWords)
            require(guest.read32(address) == word,
                "Original instructions pin embedded joystick held bits at base+36, repeat timer at base+52, and first/release delays");
        constexpr std::array<std::pair<uint32_t,uint32_t>,12> originalPauseWords{{
            {0x8213ca5c,0x821af738}, {0x821af744,0x89630030}, {0x821af750,0x4bff52a9},
            {0x821a4a00,0xe92a3da8}, {0x821a4a08,0xa101fff4}, {0x821a4a0c,0x7d0b0734},
            {0x821a4a18,0x2f0b03ff}, {0x821a4a20,0x396b0192}, {0x821a4a24,0x5568103a},
            {0x821a4a28,0x7d68502e}, {0x821a4a34,0xe94b0010}, {0x821a4a38,0x7f2a4800}
        }};
        for (const auto& [address,word] : originalPauseWords)
            require(guest.read32(address) == word,
                "Original paused predicate checks byte+48 and the full-generation GUI pause handle with a signed bounded index");
        PPCContext ctx{}; guest.initializeThread(ctx, 0x100000); currentContext = &ctx;
        gui = guest.allocate(20480); input = guest.allocate(4096); manager = guest.allocate(4096);
        layer = guest.allocate(4096); replacementLayer = guest.allocate(4096); control = guest.allocate(4096);
        controls = guest.allocate(4096); game = guest.allocate(4096);
        screen = guest.allocate(4096); master = guest.allocate(4096); name = guest.allocate(4096); invalidControl = guest.allocate(4096);
        rawInput = guest.allocate(4096);
        reset();
        queued = {{NativeMouseEventKind::Move, NativeMouseButton::Left, 0.25f, 0.75f, 0},
            {NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.25f, 0.75f, 0},
            {NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}};
        callerStack = ctx.r1.u32; ctx.r3.u64 = gui;
        sub_821A13F0(ctx, guest.base());
        require(originalUpdates == 1 && calls == std::vector<std::string>{"transform", "position", "transform", "position", "down", "dispatch",
            "transform", "position", "up", "dispatch"}, "Original GUI update precedes ordered authored hover/down/up dispatch");
        require(positions.size() == 3 && close(positions[0].first, 0.625f) && close(positions[0].second, 0.25f),
            "Pointer callback uses the authored transform's result instead of uncorrected window fractions");
        require(buttons.empty(), "Ordinary menu clicks use original hit testing and never become generic A presses");
        sameContext(ctx, originalResult);
        PPCContext getter = ctx;
        require(currentDevice(getter) == 7 && originalGetters == 3, "Original current-device getter remains device seven outside scoped native dispatch");

        reset();
        pointer(ctx, {{NativeMouseEventKind::Move, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        require(close(std::bit_cast<float>(memory->read32(rawInput + 1688)), 0.625f) &&
            close(std::bit_cast<float>(memory->read32(rawInput + 1692)), 0.25f) &&
            close(std::bit_cast<float>(memory->read32(rawInput + 1696)), 0.1f) &&
            close(std::bit_cast<float>(memory->read32(rawInput + 1700)), 0.2f) && memory->read32(rawInput + 1684) == 0x76543210,
            "Validated original raw-input cursor history stores post-aspect coordinates without fabricating mouse buttons");

        reset(); pointer(ctx, {});
        require(pointerContexts == 1 && pointerOwner == Doritos::Graphics::GuiPointerContext{screen,master,memory->read64(master + 16)} &&
            mappingInputs.empty(), "Every valid GUI update publishes its screen and root generation before the idle keyboard-only return");
        presentedOwner = pointerOwner; mappedPointer = {Doritos::Graphics::GuiPointerMapping::Hit,0.375f,0.625f};
        pointer(ctx, {{NativeMouseEventKind::Move, NativeMouseButton::Left, 0.25f, 0.75f, 0},
            {NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.25f, 0.75f, 0},
            {NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        require(mappingInputs == std::vector<std::pair<float,float>>(3,{0.25f,0.75f}) &&
            positions == std::vector<std::pair<float,float>>(3,{0.875f,0.1875f}) &&
            std::count(calls.begin(),calls.end(),"down") == 1 && std::count(calls.begin(),calls.end(),"up") == 1,
            "Presented monitor texture UV feeds the original aspect transform for both hover and ordinary left clicks");
        calls.clear(); positions.clear(); mappedPointer = {Doritos::Graphics::GuiPointerMapping::Hit,0.225f,0.825f};
        pointer(ctx, {});
        require(calls == std::vector<std::string>{"transform","position"} &&
            close(positions[0].first,0.575f) && close(positions[0].second,0.2875f),
            "Stationary hover follows an updated presented monitor projection without replaying click actions");
        calls.clear(); positions.clear(); memory->write64(master + 16,memory->read64(master + 16) ^ (uint64_t(1) << 40));
        pointer(ctx, {});
        require(calls.empty() && latestPointer && pointerSelection && pointerOwner != presentedOwner,
            "A reused screen root with a new full handle waits for its own presented projection while retaining stationary hover");
        presentedOwner = pointerOwner; pointer(ctx, {});
        require(calls == std::vector<std::string>{"transform","position"},
            "A matching projection publication restores hover for the new root generation without another mouse event");
        for (const unsigned changedOwner : {0u,1u,2u}) {
            reset(); pointer(ctx, {}); presentedOwner = pointerOwner;
            mappedPointer = {Doritos::Graphics::GuiPointerMapping::Hit,0.375f,0.625f};
            pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
            calls.clear();
            if (changedOwner == 0) {
                memory->write32(invalidControl + 8,master); memory->write32(gui + 7768,invalidControl);
            } else if (changedOwner == 1) memory->write32(screen + 8,replacementLayer);
            else memory->write64(master + 16,memory->read64(master + 16) ^ (uint64_t(1) << 40));
            pointer(ctx, {}); presentedOwner = pointerOwner;
            pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
            require(calls == std::vector<std::string>{"transform","position"},
                "A valid press on one screen cannot activate another screen, replacement root or new root generation on its later release");
        }
        for (const unsigned changedRecipients : {0u,1u,2u,3u}) {
            reset(); pointer(ctx, {}); presentedOwner = pointerOwner;
            const auto acceptedContext = pointerOwner;
            mappedPointer = {Doritos::Graphics::GuiPointerMapping::Hit,0.375f,0.625f};
            pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
            calls.clear();
            if (changedRecipients == 0)
                memory->write64(control + 16,memory->read64(control + 16) ^ (uint64_t(1) << 40));
            else if (changedRecipients == 1) {
                memory->write32(control + 120,master); memory->write32(control + 124,screen);
                memory->write32(master,0x8215bf6c); memory->write32(master + 8,name);
                std::strcpy(reinterpret_cast<char*>(memory->base() + name),"intro/title_continue");
            } else if (changedRecipients == 2) {
                memory->write32(controls + 4,invalidControl); memory->write32(invalidControl + 168,layer);
                memory->write32(layer + 144,controls + 8);
            } else {
                memory->write32(manager + 56,replacementLayer); memory->write32(control + 168,replacementLayer);
            }
            pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
            require(pointerOwner == acceptedContext && calls == std::vector<std::string>{"transform","position"} && buttons.empty(),
                "An accepted press cannot activate rebuilt recipients or another layer on release, including a replacement splash, while the screen and root remain unchanged");
        }

        reset(); pointer(ctx, {}); presentedOwner = pointerOwner;
        mappedPointer = {Doritos::Graphics::GuiPointerMapping::Pending,0,0};
        memory->base()[control + 174] = memory->base()[control + 175] = 1;
        pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        pointer(ctx, {});
        require(calls.empty() && latestPointer && pointerSelection &&
            !memory->base()[control + 174] && !memory->base()[control + 175],
            "Pending monitor projections consume left presses and clear pending actions while retaining the physical pointer snapshot");
        mappedPointer = {Doritos::Graphics::GuiPointerMapping::Hit,0.375f,0.625f};
        memory->write64(master + 16,memory->read64(master + 16) ^ (uint64_t(1) << 40));
        pointer(ctx, {}); presentedOwner = pointerOwner;
        pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        require(calls == std::vector<std::string>{"transform","position"},
            "A release cannot activate a pending press even when a new screen generation now has a valid projection");
        calls.clear();
        mappedPointer = {Doritos::Graphics::GuiPointerMapping::Pending,0,0};
        pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        mappedPointer = {Doritos::Graphics::GuiPointerMapping::Hit,0.375f,0.625f};
        pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.25f, 0.75f, 0},
            {NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        require(std::count(calls.begin(),calls.end(),"dispatch") == 2,
            "A new valid projected click clears dropped-gesture suppression and dispatches its own down and up");

        reset(); pointer(ctx, {}); presentedOwner = pointerOwner;
        mappedPointer = {Doritos::Graphics::GuiPointerMapping::Miss,0,0};
        memory->base()[control + 174] = memory->base()[control + 175] = 1;
        pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.25f, 0.75f, 0},
            {NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        require(calls == std::vector<std::string>{"transform","position","transform","position"} &&
            positions == std::vector<std::pair<float,float>>(2,{-15.875f,-4.125f}) &&
            !memory->base()[control + 174] && !memory->base()[control + 175] && buttons.empty(),
            "Clicks outside the projected monitor clear authored hover with the original sentinel and never activate the old selection");
        calls.clear();
        pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        calls.clear(); mappedPointer = {Doritos::Graphics::GuiPointerMapping::Hit,0.375f,0.625f};
        pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        require(calls == std::vector<std::string>{"transform","position"},
            "A press outside the monitor cannot activate when its release moves over a valid projected tile");
        for (const bool loseFocus : {false,true}) {
            reset(); pointer(ctx, {}); presentedOwner = pointerOwner;
            mappedPointer = {Doritos::Graphics::GuiPointerMapping::Pending,0,0};
            pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
            if (loseFocus) { focused = false; pointer(ctx, {}); focused = true; }
            else pointer(ctx, {{NativeMouseEventKind::Cancel, NativeMouseButton::Left, 0, 0, 0}});
            calls.clear(); mappedPointer = {Doritos::Graphics::GuiPointerMapping::Hit,0.375f,0.625f};
            pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
            require(calls == std::vector<std::string>{"transform","position","up","dispatch"},
                "Focus cancellation and explicit mouse cancellation release dropped-gesture suppression without retaining an old press");
        }
        for (const auto mapping : {Doritos::Graphics::GuiPointerMapping::Pending,Doritos::Graphics::GuiPointerMapping::Miss}) {
            reset(); pointer(ctx, {}); presentedOwner = pointerOwner; mappedPointer = {mapping,0,0};
            pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Right, 0.25f, 0.75f, 0},
                {NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.25f, 0.75f, 120}});
            require(positions.empty() && buttons == std::vector<std::pair<bool,uint64_t>>{{true,1},{false,1},{true,10},{false,10}},
                "A missing monitor hit or pending projection preserves right-button Back and wheel navigation without changing hover");
        }
        reset(); pointer(ctx, {}); presentedOwner = pointerOwner;
        mappedPointer = {Doritos::Graphics::GuiPointerMapping::Miss,0,0}; memory->base()[screen + 100] = 1;
        pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        require(mappingInputs.empty() && calls == std::vector<std::string>{"transform","position","up","dispatch"} &&
            close(positions[0].first,0.625f) && close(positions[0].second,0.25f),
            "Original ready screen homographies retain their genuine inverse route without applying native projection twice");
        reset(); memory->write32(rawInput, 0);
        std::array<uint8_t, 20> rawBefore{}; std::memcpy(rawBefore.data(), guest.base() + rawInput + 1684, rawBefore.size());
        pointer(ctx, {{NativeMouseEventKind::Move, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        require(!std::memcmp(rawBefore.data(), guest.base() + rawInput + 1684, rawBefore.size()),
            "An unrelated raw-input vtable cannot receive cursor-cache mutations");

        reset();
        pointer(ctx, {{NativeMouseEventKind::Move, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        calls.clear(); positions.clear();
        const PPCContext beforeStationary = ctx;
        pointer(ctx, {});
        require(calls == std::vector<std::string>{"transform", "position"} && positions.size() == 1 &&
            close(positions[0].first,0.625f) && close(positions[0].second,0.25f) && buttons.empty(),
            "A stationary pointer refreshes authored hit testing once per frame without inventing mouse or pad actions");
        sameContext(ctx,beforeStationary);
        calls.clear(); positions.clear();
        memory->write32(screen + 8,replacementLayer); memory->write32(control + 120,master);
        memory->write64(control + 16,0x23456789abcdef01ULL);
        pointer(ctx, {});
        require(calls == std::vector<std::string>{"transform", "position"} && positions.size() == 1 && buttons.empty(),
            "A new screen root and rebuilt clickable identities re-hit-test a stationary pointer against their current owners");
        calls.clear(); pointerSelection = false;
        pointer(ctx, {});
        require(calls.empty(), "Keyboard or controller ownership preserves its selection across stationary mouse frames");
        motionActivity = false;
        pointer(ctx, {{NativeMouseEventKind::Move, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
        require(calls.empty(), "A resize-only queued motion cannot steal selection from keyboard or controller navigation");
        for (const auto invalid : {NativeMousePosition{std::numeric_limits<float>::quiet_NaN(),0.5f},
            NativeMousePosition{0.5f,std::numeric_limits<float>::infinity()},
            NativeMousePosition{-0.1f,0.5f},NativeMousePosition{0.5f,1.1f}}) {
            reset(); latestPointer = invalid; pointerSelection = true;
            pointer(ctx, {});
            require(calls.empty(), "Nonfinite and out-of-client stationary snapshots never reach authored hit testing");
        }

        reset();
        memory->write32(layer + 116, 1); memory->write32(layer + 192, 1); memory->write32(manager + 84, 0);
        const PPCContext beforeBinding = ctx;
        pointer(ctx, {});
        require(calls == std::vector<std::string>{"bind"} && memory->read32(layer + 116) == 0x81 && memory->read32(manager + 84) == layer,
            "Enabling native mouse retains the original gamepad mask through the authored layer setter");
        sameContext(ctx, beforeBinding);

        reset(); pointer(ctx, {});
        require(contextChanges == 1 && !gameplay, "A valid menu GUI supplies menu context even without mouse events");
        memory->write32(game, 0x8213c9cc); memory->write32(gameGlobal, game); pointer(ctx, {});
        require(gameplay, "Only an actual Game vtable supplies gameplay Escape context");
        memory->base()[game + 48] = 1; pointer(ctx, {});
        require(!gameplay, "A paused game supplies Back context so Escape resumes and navigates pause submenus");
        memory->base()[game + 48] = 0;
        const uint64_t pauseHandle = (uint64_t(0x76543210) << 32) | (uint64_t(7) << 16) | 0x42;
        memory->write64(gui + 15784, pauseHandle); memory->write32(gui + 1608 + 7*4, control);
        memory->write64(control + 16, pauseHandle); pointer(ctx, {});
        require(!gameplay, "A still-owned pause group keeps Back context after the pause byte clears during a transition");
        memory->write64(control + 16, pauseHandle ^ (uint64_t(1) << 32)); pointer(ctx, {});
        require(gameplay, "An old-generation pause handle cannot keep an unrelated reused GUI object paused");
        memory->write32(gui + 1608 + 7*4, 0); pointer(ctx, {});
        require(gameplay, "A removed pause registry object releases the original pause context");
        for (uint32_t index : {1023u,32768u,65535u}) {
            memory->write64(gui + 15784, (uint64_t(0x76543210) << 32) | (uint64_t(index) << 16)); pointer(ctx, {});
            require(gameplay, "Out-of-range and negative signed pause indices never read outside the owning GUI registry");
        }
        memory->write32(game, 0); pointer(ctx, {});
        require(!gameplay, "An unrelated allocated object cannot claim gameplay context");

        reset();
        memory->base()[input + 174] = 0x5a; memory->base()[input + 175] = 0xa5;
        memory->base()[control + 174] = memory->base()[control + 175] = 1;
        pointer(ctx, {{NativeMouseEventKind::Cancel, NativeMouseButton::Left, 0, 0, 0}});
        require(!memory->base()[control + 174] && !memory->base()[control + 175] &&
            memory->base()[input + 174] == 0x5a && memory->base()[input + 175] == 0xa5,
            "Cancellation clears owned clickable pending flags while preserving unrelated GUIINPUT bytes");
        require(positions.size() == 1 && positions[0].first < 0 && positions[0].second < 0 && buttons.empty(),
            "Cancellation removes authored hover without triggering a click or pad action");
        reset(); memory->base()[control + 174] = 1;
        pointer(ctx, {{NativeMouseEventKind::Leave, NativeMouseButton::Left, 0, 0, 0}});
        require(!memory->base()[control + 174] && calls == std::vector<std::string>{"transform", "position"},
            "Leaving the client clears pending widget actions through the authored hover route");
        reset(); memory->base()[control + 174] = 1;
        memory->write32(controls + 4, invalidControl); memory->write32(invalidControl + 168, 0);
        memory->write32(layer + 144, controls + 8);
        pointer(ctx, {{NativeMouseEventKind::Cancel, NativeMouseButton::Left, 0, 0, 0}});
        require(memory->base()[control + 174] == 1 && calls.empty(),
            "Cancellation validates every vector recipient before changing any widget flag");

        reset(); focused = false; memory->base()[control + 174] = 1;
        pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
        require(calls.empty() && !memory->base()[control + 174] && queued.empty(),
            "Background GUI polling cancels pending widgets and consumes queued clicks without authored actions");
        focused = true; pointer(ctx, {});
        require(calls.empty(), "Focus restoration cannot replay background mouse clicks");
        reset(); memory->write32(input + 12, 1); memory->base()[control + 174] = 1;
        pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, 0.5f, 0},
            {NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.5f, 0.5f, 120},
            {NativeMouseEventKind::ButtonUp, NativeMouseButton::Right, 0.5f, 0.5f, 0}});
        require(calls.empty() && !memory->base()[control + 174], "Original input blocking consumes every pointer action and cancels pending clicks");
        memory->write32(input + 12, 0); pointer(ctx, {});
        require(calls == std::vector<std::string>{"transform", "position"} && buttons.empty(),
            "Unblocking input restores stationary hover without replaying consumed transition clicks");
        reset(); pointer(ctx, {}); presentedOwner = pointerOwner;
        mappedPointer = {Doritos::Graphics::GuiPointerMapping::Hit,0.375f,0.625f};
        memory->write32(input + 12,1);
        pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        memory->write32(input + 12,0);
        pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        require(calls == std::vector<std::string>{"transform","position"},
            "A press consumed during blocked input cannot activate on its later unblocked projected release");
        calls.clear();
        pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        calls.clear(); memory->write32(input + 12,1);
        pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        require(calls.empty(), "A blocked release clears an accepted press without dispatching its action");
        memory->write32(input + 12,0);
        memory->write64(master + 16,memory->read64(master + 16) ^ (uint64_t(1) << 40));
        pointer(ctx, {}); presentedOwner = pointerOwner; calls.clear();
        pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        require(calls == std::vector<std::string>{"transform","position","up","dispatch"},
            "A consumed blocked release retains neither dropped-gesture suppression nor an old press owner");
        reset(); memory->write32(input + 12, 1);
        pointer(ctx, {{NativeMouseEventKind::Move, NativeMouseButton::Left, 0.25f, 0.75f, 0}});
        pointer(ctx, {});
        require(calls.empty(), "A stationary pointer cannot bypass an original blocked-input interval");
        memory->write32(input + 12,0); pointer(ctx, {});
        require(calls == std::vector<std::string>{"transform", "position"} && positions.size() == 1 && buttons.empty(),
            "Hover first received during a fade becomes selectable when input unblocks without a new motion or click");
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            reset(); pointer(ctx, {{NativeMouseEventKind::Move, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
            calls.clear(); focused = false; pointer(ctx, {});
            focused = true; pointer(ctx, {});
            require(calls.empty() && !latestPointer && !pointerSelection,
                "Repeated focus loss and restoration cannot revive a prior stationary pointer snapshot");
            pointer(ctx, {{NativeMouseEventKind::Move, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
            pointer(ctx, {{NativeMouseEventKind::Leave, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
            calls.clear(); pointer(ctx, {});
            require(calls.empty() && !latestPointer && !pointerSelection,
                "Repeated client leave cancellation cannot revive hover or replay a pointer action");
        }
        reset();
        pointer(ctx, {{NativeMouseEventKind::Move, NativeMouseButton::Left, std::numeric_limits<float>::quiet_NaN(), 0.5f, 0},
            {NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, std::numeric_limits<float>::infinity(), 0},
            {NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, -0.1f, 0.5f, 0},
            {NativeMouseEventKind::Wheel, NativeMouseButton::Left, 1.1f, 0.5f, 120}});
        require(calls.empty() && queued.empty(), "Nonfinite and out-of-client normalized events cannot reach original guest callbacks");

        for (unsigned invalidOwner = 0; invalidOwner < 7; ++invalidOwner) {
            reset();
            if (invalidOwner == 0) memory->write32(guiGlobal, 0);
            else if (invalidOwner == 1) memory->write32(gui, 0);
            else if (invalidOwner == 2) memory->write32(input, 0);
            else if (invalidOwner == 3) memory->write32(input + 8, 0);
            else if (invalidOwner == 4) memory->write32(layer, 0);
            else if (invalidOwner == 5) memory->base()[layer + 196] = 0;
            else memory->write32(layer + 116, 0x80);
            pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
            require(calls.empty() && queued.empty(), "Unregistered GUI, wrong native vtables, inactive layers and missing owner masks reject queued actions");
        }
        reset(); queued = {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, 0.5f, 0}};
        updateNativeGuiInput(ctx, guest.base() + 4, gui);
        require(calls.empty() && queued.empty(), "A mismatched guest memory base cannot dispatch mouse events");
        reset(); queued = {{NativeMouseEventKind::Move, NativeMouseButton::Left, 0.5f, 0.5f, 0}};
        memory = nullptr; updateNativeGuiInput(ctx, guest.base(), gui); memory = &guest;
        require(calls.empty() && queued.empty(), "No guest memory means no authored pointer dispatch");

        reset(); receiverOnPosition = true;
        pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, 0.5f, 0},
            {NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
        require(calls == std::vector<std::string>{"transform", "position"} && buttons.empty(),
            "Receiver destruction during original hit testing prevents later callbacks from using stale ownership");
        reset(); transitionOnDispatch = true;
        pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, 0.5f, 0},
            {NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
        require(calls == std::vector<std::string>{"transform", "position", "down", "dispatch"},
            "A layer transition during an original click discards remaining queued events");
        for (unsigned mutation = 1; mutation <= 5; ++mutation) {
            reset(); recipientMutation = mutation;
            pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, 0.5f, 0},
                {NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
            require(calls == std::vector<std::string>{"transform", "position", "down", "dispatch"},
                "Reused layer vectors cannot deliver remaining events after master, full-handle, recipient or screen-root generation changes");
        }
        reset(); focusOnPosition = true;
        pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
        require(calls == std::vector<std::string>{"transform", "position"},
            "Losing foreground during original hit testing cancels before any button action");

        reset();
        pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Right, 0.5f, 0.5f, 0}});
        require(buttons == std::vector<std::pair<bool, uint64_t>>{{true, 1}, {false, 1}},
            "Right-button release invokes an authored B down/up pair for the actual input owner");
        reset(); transitionOnButton = true;
        pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Right, 0.5f, 0.5f, 0},
            {NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
        require(buttons == std::vector<std::pair<bool, uint64_t>>{{true, 1}, {false, 1}} &&
            calls == std::vector<std::string>{"transform", "position", "buttonDown", "buttonUp"},
            "Immediate Back transitions still release the synthetic held key and reject the next queued click");
        reset();
        pointer(ctx, {{NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.5f, 0.5f, 60}});
        require(buttons.empty(), "A partial high-resolution wheel detent waits for the remaining delta");
        pointer(ctx, {{NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.5f, 0.5f, 60},
            {NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.5f, 0.5f, -240}});
        require(buttons == std::vector<std::pair<bool, uint64_t>>{{true, 10}, {false, 10}, {true, 11}, {false, 11}, {true, 11}, {false, 11}},
            "Accumulated wheel detents invoke authored first navigation messages and release every directional key");
        require(std::count(calls.begin(), calls.end(), "direction") == 3,
            "Each wheel detent sends exactly one original first-press direction message");
        const auto afterWheelCalls = calls;
        pointer(ctx, {});
        require(calls == afterWheelCalls && !pointerSelection,
            "Stationary hover cannot undo a wheel-selected item on the following frame");

        constexpr uint32_t heldTimer = 0x3e123456;
        for (const uint32_t held : {5u,10u,15u}) for (const int delta : {-240,240}) {
            reset(); memory->write32(input + 36,held); memory->write32(input + 52,heldTimer);
            memory->write32(input + 40,9); memory->write32(input + 56,0x3f012345);
            pointer(ctx, {{NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.5f, 0.5f, delta}});
            require(directionStates == std::vector<uint32_t>(2,delta > 0 ? 1u : 2u),
                "Scrolling while vertical and horizontal keys are held sends only the requested wheel axis per detent");
            require(memory->read32(input + 36) == held && memory->read32(input + 52) == heldTimer &&
                memory->read32(input + 40) == 9 && memory->read32(input + 56) == 0x3f012345,
                "Wheel down/opposite-bit clearing, repeat delay and up mutations preserve real held state and every other user");
        }
        reset(); memory->write32(gui + 16420,2); memory->write32(manager + 64,layer);
        memory->write32(layer + 116,0x84); memory->write32(layer + 192,0x84);
        memory->write32(input + 36,9); memory->write32(input + 52,0x3f012345);
        memory->write32(input + 44,6); memory->write32(input + 60,heldTimer);
        pointer(ctx, {{NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.5f, 0.5f, 120}});
        require(buttons == std::vector<std::pair<bool,uint64_t>>{{true,(uint64_t(2) << 32) | 10},
            {false,(uint64_t(2) << 32) | 10}} && memory->read32(input + 44) == 6 &&
            memory->read32(input + 60) == heldTimer && memory->read32(input + 36) == 9 &&
            memory->read32(input + 52) == 0x3f012345,
            "Wheel state isolation follows the actual GUI owner and preserves controller zero for another owner");
        for (unsigned failure = 1; failure <= 3; ++failure) {
            reset(); wheelFailure = failure;
            memory->write32(input + 36,15); memory->write32(input + 52,heldTimer);
            const PPCContext beforeWheelFailure = ctx;
            bool failed = false;
            try { pointer(ctx, {{NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.5f, 0.5f, 960}}); }
            catch (const NativeFailure&) { failed = true; }
            require(failed && memory->read32(input + 36) == 15 && memory->read32(input + 52) == heldTimer,
                "Original down, direction and up failures restore valid held state without swallowing the callback error");
            sameContext(ctx,beforeWheelFailure);
            wheelFailure = 0; calls.clear(); buttons.clear(); directionStates.clear();
            pointer(ctx, {{NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.5f, 0.5f, 60}});
            require(buttons.empty(), "A failed wheel callback cannot replay its unused detents later");
        }
        for (unsigned transition = 1; transition <= 3; ++transition) {
            reset(); wheelTransition = transition;
            memory->write32(input + 36,15); memory->write32(input + 52,heldTimer);
            pointer(ctx, {{NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.5f, 0.5f, 360},
                {NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
            require(directionStates == std::vector<uint32_t>{1} && memory->read32(input + 36) == 0 &&
                memory->read32(input + 52) == std::bit_cast<uint32_t>(0.7f) &&
                std::count(calls.begin(),calls.end(),"down") == 0,
                "A layer, reused-screen or receiver transition cancels the batch without resurrecting old held state");
            reset();
            pointer(ctx, {{NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.5f, 0.5f, 60}});
            require(buttons.empty(), "A transitioned wheel batch discards leftover detents even when the layer is reused");
        }
        reset(); wheelTransition = 2; wheelFailure = 2;
        memory->write32(input + 36,15); memory->write32(input + 52,heldTimer);
        bool wheelTransitionFailed = false;
        try { pointer(ctx, {{NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.5f, 0.5f, 120}}); }
        catch (const NativeFailure&) { wheelTransitionFailed = true; }
        require(wheelTransitionFailed && memory->read32(input + 36) == 0 &&
            memory->read32(input + 52) == std::bit_cast<uint32_t>(0.7f),
            "Exception cleanup cannot restore an old screen's held direction after an authored transition");

        for (const char* splashName : {"intro/logos", "intro/intro", "intro/title_continue"}) {
            reset(); memory->write32(control + 120, master); memory->write32(control + 124, screen);
            memory->write32(master, 0x8215bf6c); memory->write32(master + 8, name);
            std::memcpy(guest.base() + name, splashName, std::strlen(splashName) + 1);
            pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
            require(buttons == std::vector<std::pair<bool, uint64_t>>{{true, 0}, {false, 0}} &&
                calls == std::vector<std::string>{"buttonDown", "buttonUp"},
                "Only exact original owned splash groups use their authored A continuation binding");
        }
        reset(); memory->write32(control + 120, master); memory->write32(control + 124, screen);
        memory->write32(master, 0x8215bf6c); memory->write32(master + 8, name);
        std::memcpy(guest.base() + name, "intro/logo_other", 17);
        pointer(ctx, {{NativeMouseEventKind::ButtonUp, NativeMouseButton::Left, 0.5f, 0.5f, 0}});
        require(buttons.empty() && calls == std::vector<std::string>{"transform", "position", "up", "dispatch"},
            "Similar group names cannot replace real menu hit testing with a generic A press");

        reset(); failDispatch = true; const PPCContext beforeFailure = ctx;
        bool failed = false;
        try { pointer(ctx, {{NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, 0.5f, 0}}); }
        catch (const NativeFailure&) { failed = true; }
        require(failed, "Original GUI callback failures propagate instead of reporting a successful native click");
        sameContext(ctx, beforeFailure); getter = ctx;
        require(currentDevice(getter) == 7, "A guest callback exception restores the original unscoped device identity");
        reset(); const uint64_t validStack = ctx.r1.u64; ctx.r1.u64 = 512; failed = false;
        try { pointer(ctx, {{NativeMouseEventKind::Move, NativeMouseButton::Left, 0.5f, 0.5f, 0}}); }
        catch (const NativeFailure&) { failed = true; }
        require(failed && calls.empty() && ctx.r1.u64 == 512, "Invalid guest stacks fail before a callback and never alter caller state");
        ctx.r1.u64 = validStack;
        std::printf("Native GUI input contracts passed: %zu checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native GUI input contract failed: %s\n", error.what()); return 1;
    }
}
