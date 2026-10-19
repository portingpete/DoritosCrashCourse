#include "gui_input.h"
#include "input_services.h"
#include "mouse_services.h"
#include "runtime.h"
#include "renderer/native/gui_pointer.h"
#include <bit>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_821A13F0);
extern "C" PPC_FUNC(__imp__sub_821B9A70);
PPC_FUNC(sub_8224DBA8);
PPC_FUNC(sub_82267C50);
PPC_FUNC(sub_82267BC0);
PPC_FUNC(sub_82267B30);
PPC_FUNC(sub_822679E0);
PPC_FUNC(sub_8231ED88);
PPC_FUNC(sub_82268160);
PPC_FUNC(sub_82268448);
PPC_FUNC(sub_82269038);

namespace Doritos::Native {
namespace {
constexpr uint32_t guiGlobal = 0x82b5ca98;
constexpr uint32_t inputGlobal = 0x82bab138;
constexpr uint32_t gameGlobal = 0x82b5caa0;
constexpr uint32_t guiTable = 0x8213bb34;
constexpr uint32_t inputTable = 0x8214fd54;
constexpr uint32_t layerGlobal = 0x82b5ab1c;
constexpr uint32_t layerTable = 0x8215e8fc;
thread_local std::optional<uint32_t> mousePlayer;
struct MousePlayerScope {
    std::optional<uint32_t> previous = mousePlayer;
    explicit MousePlayerScope(uint32_t user) { mousePlayer = user; }
    ~MousePlayerScope() { mousePlayer = previous; }
};

bool span(uint32_t address, uint32_t bytes) {
    return memory && address && !(address & 3) && memory->valid(address, bytes);
}
bool guiOwner(uint32_t gui) {
    return span(guiGlobal, 4) && memory->read32(guiGlobal) == gui &&
        span(gui, 16424) && memory->read32(gui) == guiTable &&
        span(guiTable + 112, 4) && memory->read32(guiTable + 112) == 0x821a13f0;
}
uint32_t receiver(uint32_t gui) {
    if (!guiOwner(gui) || !span(inputGlobal, 4)) return 0;
    const uint32_t input = memory->read32(inputGlobal);
    return span(input, 176) && memory->read32(input) == inputTable &&
        memory->read32(input + 8) == 0x8214fdb8 ? input : 0;
}
uint32_t player(uint32_t gui) {
    const uint32_t owner = memory->read32(gui + 16420);
    return owner < 4 ? owner : 0;
}
uint32_t layer(uint32_t user) {
    if (!span(layerGlobal,4)) return 0;
    const uint32_t manager = memory->read32(layerGlobal);
    if (!span(manager,92)) return 0;
    const uint32_t current = memory->read32(manager + 56 + user*4);
    return span(current,200) && memory->read32(current) == layerTable &&
        memory->base()[current + 196] && (memory->read32(current + 116) & (1u << user)) ? current : 0;
}
std::optional<std::vector<uint32_t>> controls(uint32_t current) {
    if (!span(current,200) || memory->read32(current) != layerTable) return {};
    const uint32_t begin = memory->read32(current + 140), end = memory->read32(current + 144);
    if (begin == end) return std::vector<uint32_t>{};
    if (end < begin || (end-begin)%4 || (end-begin)/4 > 4096 || !span(begin,end-begin)) return {};
    std::vector<uint32_t> result;
    for (uint32_t address = begin; address < end; address += 4) {
        const uint32_t control = memory->read32(address);
        if (!span(control,176) || memory->read32(control + 168) != current) return {};
        result.push_back(control);
    }
    return result;
}
bool cancel(uint32_t current) {
    const auto items = controls(current);
    if (!items) return false;
    // These original pending bytes belong to the layer's GUICLICKABLE
    // recipients, not GUIINPUT. Validate the entire owning vector first.
    for (const uint32_t control : *items) {
        memory->base()[control + 174] = 0;
        memory->base()[control + 175] = 0;
    }
    return true;
}
bool splash(uint32_t gui, uint32_t current) {
    const uint32_t screen = memory->read32(gui + 7768);
    if (!span(screen,104)) return false;
    const auto items = controls(current);
    if (!items) return false;
    for (const uint32_t control : *items) {
        if (memory->read32(control) != 0x8214f714 || memory->base()[control + 128] ||
            !memory->base()[control + 173] || memory->read32(control + 124) != screen) continue;
        const uint32_t master = memory->read32(control + 120);
        if (!span(master,12) || memory->read32(master) != 0x8215bf6c) continue;
        const uint32_t name = memory->read32(master + 8);
        if (!name || !memory->valid(name,64)) continue;
        const char* text = reinterpret_cast<const char*>(memory->base() + name);
        if (!std::memchr(text,0,64)) continue;
        if (!std::strcmp(text,"intro/logos") || !std::strcmp(text,"intro/intro") ||
            !std::strcmp(text,"intro/title_continue")) return true;
    }
    return false;
}
bool activeRace(uint32_t gui) {
    if (!span(gameGlobal, 4)) return false;
    const uint32_t game = memory->read32(gameGlobal);
    if (!span(game, 52) || memory->read32(game) != 0x8213c9cc || memory->base()[game + 48]) return false;
    // Match original Game::IsPaused (821AF738), including the owned GUI
    // pause handle used during fade/close transitions. Escape is Back while
    // paused, so the original pause menu resumes and submenus go back.
    const uint64_t handle = memory->read64(gui + 15784);
    const int16_t index = static_cast<int16_t>(memory->read32(gui + 15788) >> 16);
    if (index < 0 || index >= 1023) return true;
    const uint32_t pause = memory->read32(gui + 1608 + uint32_t(index) * 4);
    return !(span(pause, 24) && memory->read64(pause + 16) == handle);
}

struct CallbackFrame {
    PPCContext context;
    explicit CallbackFrame(const PPCContext& caller) : context(caller) {
        if (context.r1.u32 < 1024 || !span(context.r1.u32 - 1024, 1024))
            fail("Native mouse callback has no original guest call frame");
        const uint32_t stack = context.r1.u32;
        context.r1.u64 = stack - 512;
        memory->write32(context.r1.u32, stack);
    }
};
struct Recipient {
    uint32_t address, master;
    uint64_t handle;
    bool operator==(const Recipient&) const = default;
};
struct LeftPress {
    Doritos::Graphics::GuiPointerContext context;
    uint32_t layer;
    std::vector<Recipient> recipients;
};
void position(PPCContext& ctx, uint8_t* base, uint32_t input, float x, float y) {
    const uint32_t scratch = ctx.r1.u32 + 128;
    memory->write32(scratch, std::bit_cast<uint32_t>(x));
    memory->write32(scratch + 4, std::bit_cast<uint32_t>(y));
    ctx.r3.u64 = scratch; ctx.r4.u64 = scratch + 4;
    // Original pixel-mouse path 822529C0 first normalizes by screen size and
    // then calls this exact authored aspect/letterbox transform.
    sub_8224DBA8(ctx, base);
    ctx.r3.u64 = input;
    ctx.r4.u64 = memory->read32(scratch);
    ctx.r5.u64 = uint64_t(memory->read32(scratch + 4)) << 32;
    ctx.r6.u64 = 0;
    sub_82267C50(ctx, base);
    // Preserve the original INPUTXENON cursor cache used by layer rebuilds,
    // so a stationary pointer remains at its actual position after a menu
    // transition. Match 822529C0's post-dispatch write order; no button writes.
    if (span(0x82b5caa8,4)) {
        const uint32_t raw = memory->read32(0x82b5caa8);
        if (span(raw,1704) && memory->read32(raw) == 0x821612b4) {
            memory->write32(raw + 1696,memory->read32(raw + 1688));
            memory->write32(raw + 1700,memory->read32(raw + 1692));
            memory->write32(raw + 1688,memory->read32(scratch));
            memory->write32(raw + 1692,memory->read32(scratch + 4));
        }
    }
}
Doritos::Graphics::GuiPointerMapping pointerPosition(PPCContext& ctx, uint8_t* base, uint32_t input,
    const Doritos::Graphics::GuiPointerContext& owner, float x, float y, bool clearMiss) {
    using Doritos::Graphics::GuiPointerMapping;
    auto mapping = GuiPointerMapping::Flat;
    const auto presentation = Doritos::Graphics::mapTitlePresentationPointer(x,y);
    if(presentation.mapping == GuiPointerMapping::Miss || presentation.mapping == GuiPointerMapping::Pending) {
        if(clearMiss && presentation.mapping == GuiPointerMapping::Miss) position(ctx,base,input,-8.0f,-8.0f);
        return presentation.mapping;
    }
    if(presentation.mapping == GuiPointerMapping::Hit) {x=presentation.x;y=presentation.y;}
    // Original ready screen homographies already undo their model projection.
    // Otherwise a native presented monitor snapshot supplies texture UV before
    // the original aspect conversion, preserving the authored hit-test ABI.
    if (!memory->base()[owner.screen + 100]) {
        const auto mapped = Doritos::Graphics::mapTitleGuiPointer(owner,x,y);
        mapping = mapped.mapping;
        if (mapping == GuiPointerMapping::Hit) {
            if (!std::isfinite(mapped.x) || !std::isfinite(mapped.y) ||
                mapped.x < 0 || mapped.x > 1 || mapped.y < 0 || mapped.y > 1)
                mapping = GuiPointerMapping::Miss;
            else { x = mapped.x; y = mapped.y; }
        }
        if (mapping == GuiPointerMapping::Pending) return mapping;
        if (mapping == GuiPointerMapping::Miss) {
            if (clearMiss) position(ctx,base,input,-8.0f,-8.0f);
            return mapping;
        }
    }
    position(ctx,base,input,x,y);
    return mapping;
}
void button(PPCContext& ctx, uint8_t* base, uint32_t input, uint32_t user, uint32_t key, bool down) {
    ctx.r3.u64 = input + 8;
    ctx.r4.u64 = (uint64_t(user) << 32) | key;
    ctx.r5.u64 = ctx.r6.u64 = 0;
    if (down) sub_82268160(ctx,base);
    else sub_82268448(ctx,base);
}
}

void updateNativeGuiInput(PPCContext& ctx, uint8_t* base, uint32_t gui) {
    const auto events = takeNativeMouseEvents();
    if (!memory || base != memory->base() || !guiOwner(gui)) return;
    setNativeInputGameplay(activeRace(gui));
    const uint32_t input = receiver(gui);
    if (!input) return;
    const uint32_t user = player(gui), current = layer(user);
    if (!current) return;
    const uint32_t screen = memory->read32(gui + 7768);
    if (!span(screen,104)) { Doritos::Graphics::setTitleGuiPointerContext({}); return; }
    const uint32_t root = memory->read32(screen + 8);
    if (!span(root,24)) { Doritos::Graphics::setTitleGuiPointerContext({}); return; }
    const Doritos::Graphics::GuiPointerContext pointerOwner{screen,root,memory->read64(root + 16)};
    // Publish every valid GUI update, including keyboard-only frames, so the
    // next monitor draw is captured for the actual input screen generation.
    Doritos::Graphics::setTitleGuiPointerContext(pointerOwner);
    static int wheelRemainder = 0;
    static uint32_t wheelLayer = 0;
    // A press discarded outside a monitor or before presentation must not
    // activate on a later release, even after its screen/root is replaced.
    static bool suppressLeftRelease = false;
    static std::optional<LeftPress> leftPressOwner;
    if (wheelLayer != current) { wheelLayer = current; wheelRemainder = 0; }
    if (!nativeInputFocused()) {
        clearNativeMousePosition(); cancel(current); wheelRemainder = 0;
        suppressLeftRelease = false; leftPressOwner.reset(); return;
    }
    CallbackFrame frame(ctx);
    auto& callback = frame.context;
    MousePlayerScope mouseUser(user);
    const uint32_t mask = memory->read32(current + 116);
    if (!(mask & 0x80)) {
        // Original splash SetCursorMask selects the owner's gamepad bit.
        // Retain it and enable the native mouse through the original setter.
        callback.r3.u64 = current; callback.r4.u64 = mask | 0x80;
        sub_8231ED88(callback,base);
    }
    if (events.empty() && !nativeMouseOwnsSelection()) return;
    const uint32_t begin = memory->read32(current + 140), end = memory->read32(current + 144);
    const auto recipients = controls(current);
    if (!recipients) return;
    std::vector<Recipient> identities;
    for (const uint32_t control : *recipients)
        identities.push_back({control,memory->read32(control + 120),memory->read64(control + 16)});
    auto sameLeftPress = [&] {
        return !leftPressOwner || (leftPressOwner->context == pointerOwner &&
            leftPressOwner->layer == current && leftPressOwner->recipients == identities);
    };
    auto sameScreen = [&] {
        if (!(receiver(gui) == input && layer(user) == current &&
            memory->read32(gui + 7768) == screen && memory->read32(screen + 8) == root &&
            memory->read64(root + 16) == pointerOwner.rootHandle &&
            memory->read32(current + 140) == begin && memory->read32(current + 144) == end)) return false;
        const auto items = controls(current);
        if (!items || items->size() != identities.size()) return false;
        for (size_t index = 0; index < identities.size(); ++index) {
            const auto& identity = identities[index];
            if ((*items)[index] != identity.address || memory->read32(identity.address + 120) != identity.master ||
                memory->read64(identity.address + 16) != identity.handle) return false;
        }
        return true;
    };
    if (events.empty()) {
        // Authored screens can enable or move their clickables after the last
        // Win32 motion (fade, animation, or a menu transition). Refresh hover
        // against the current recipients without repeating any mouse action.
        if (!nativeMouseOwnsSelection()) return;
        const auto pointer = latestNativeMousePosition();
        if (!pointer || !std::isfinite(pointer->x) || !std::isfinite(pointer->y) ||
            pointer->x < 0 || pointer->x > 1 || pointer->y < 0 || pointer->y > 1) return;
        if (int32_t(memory->read32(input + 12)) > 0) {
            cancel(current); wheelRemainder = 0; return;
        }
        if (!sameScreen()) { cancel(current); wheelRemainder = 0; return; }
        const auto mapped = pointerPosition(callback,base,input,pointerOwner,pointer->x,pointer->y,true);
        if (mapped == Doritos::Graphics::GuiPointerMapping::Pending ||
            mapped == Doritos::Graphics::GuiPointerMapping::Miss) cancel(current);
        if (!sameScreen()) { cancel(current); wheelRemainder = 0; return; }
        if (!nativeInputFocused()) {
            clearNativeMousePosition(); cancel(current); wheelRemainder = 0;
            suppressLeftRelease = false; leftPressOwner.reset();
        }
        return;
    }
    struct WheelDirectionScope {
        decltype(sameScreen)& stillOwned;
        uint32_t heldAddress, timerAddress, held, timer;
        ~WheelDirectionScope() noexcept {
            // Guest callbacks may change screens or throw. Restore a real
            // held key only while the original receiver and screen still own
            // it; never carry an old screen's direction into its replacement.
            try {
                if (stillOwned()) {
                    memory->write32(heldAddress,held);
                    memory->write32(timerAddress,timer);
                }
            } catch (...) {
                // Preserve an original callback exception if ownership can
                // no longer be validated during unwinding.
            }
        }
    };
    for (const auto& event : events) {
        if (!sameScreen()) { cancel(current); wheelRemainder = 0; return; }
        if (event.kind == NativeMouseEventKind::Cancel || event.kind == NativeMouseEventKind::Leave) {
            if (!cancel(current)) return;
            wheelRemainder = 0; suppressLeftRelease = false; leftPressOwner.reset();
            position(callback, base, input, -8.0f, -8.0f);
            continue;
        }
        if (!std::isfinite(event.x) || !std::isfinite(event.y) ||
            event.x < 0 || event.x > 1 || event.y < 0 || event.y > 1) continue;
        // Resize-only WM_MOUSEMOVE messages can update normalized geometry
        // while a keyboard, controller, or wheel owns menu selection.
        if (event.kind == NativeMouseEventKind::Move && !nativeMouseOwnsSelection()) continue;
        const bool left = (event.kind == NativeMouseEventKind::ButtonDown ||
            event.kind == NativeMouseEventKind::ButtonUp) && event.button == NativeMouseButton::Left;
        // The original block-input counter also protects transitions. Events
        // received during a blocked interval are consumed, never replayed.
        const bool focusedNow = nativeInputFocused();
        if (!focusedNow || int32_t(memory->read32(input + 12)) > 0) {
            if (!focusedNow) { clearNativeMousePosition(); suppressLeftRelease = false; leftPressOwner.reset(); }
            else if (left) {
                suppressLeftRelease = event.kind == NativeMouseEventKind::ButtonDown;
                leftPressOwner.reset();
            }
            cancel(current); wheelRemainder = 0; continue;
        }
        if (event.kind == NativeMouseEventKind::ButtonUp && event.button == NativeMouseButton::Left &&
            !suppressLeftRelease && sameLeftPress() && splash(gui,current)) {
            // Authored splash groups have zero-size hit regions. Their exact
            // named A binding is the original skip/continue action.
            button(callback,base,input,user,0,true);
            if (receiver(gui) == input) button(callback,base,input,user,0,false);
            leftPressOwner.reset();
            continue;
        }
        const bool coordinatesRequired = event.kind == NativeMouseEventKind::Move || left;
        const auto mapped = pointerPosition(callback,base,input,pointerOwner,event.x,event.y,coordinatesRequired);
        const bool excluded = mapped == Doritos::Graphics::GuiPointerMapping::Pending ||
            mapped == Doritos::Graphics::GuiPointerMapping::Miss;
        if (left && excluded) {
            suppressLeftRelease = event.kind == NativeMouseEventKind::ButtonDown;
            leftPressOwner.reset();
        }
        if (!sameScreen()) { cancel(current); return; }
        if (!nativeInputFocused()) {
            clearNativeMousePosition(); cancel(current); wheelRemainder = 0;
            suppressLeftRelease = false; leftPressOwner.reset(); return;
        }
        if (coordinatesRequired && excluded) {
            if (!cancel(current)) return;
            continue;
        }
        if (left) {
            if (event.kind == NativeMouseEventKind::ButtonDown) {
                suppressLeftRelease = false;
                leftPressOwner = LeftPress{pointerOwner,current,identities};
            } else {
                const bool discarded = std::exchange(suppressLeftRelease,false) ||
                    !sameLeftPress();
                leftPressOwner.reset();
                if (discarded) {
                    if (!cancel(current)) return;
                    continue;
                }
            }
            callback.r3.u64 = input;
            if (event.kind == NativeMouseEventKind::ButtonDown) sub_82267BC0(callback, base);
            else sub_82267B30(callback, base);
            callback.r3.u64 = input;
            sub_822679E0(callback, base);
            if (!cancel(current)) return;
        } else if (event.kind == NativeMouseEventKind::ButtonUp && event.button == NativeMouseButton::Right) {
            button(callback,base,input,user,1,true);
            if (receiver(gui) == input) button(callback,base,input,user,1,false);
        } else if (event.kind == NativeMouseEventKind::Wheel) {
            wheelRemainder += std::clamp(event.wheel,-960,960);
            while (std::abs(wheelRemainder) >= 120 && sameScreen()) {
                const bool up = wheelRemainder > 0;
                wheelRemainder += up ? -120 : 120;
                const uint32_t heldAddress = input + 36 + user*4, timerAddress = input + 52 + user*4;
                WheelDirectionScope direction{sameScreen,heldAddress,timerAddress,
                    memory->read32(heldAddress),memory->read32(timerAddress)};
                // The original first-press method processes every held axis,
                // and down/up changes opposite bits and the repeat timer.
                // Give this detent its own direction without cancelling a
                // simultaneously held keyboard or controller direction.
                memory->write32(heldAddress,0);
                memory->write32(timerAddress,0);
                try {
                    button(callback,base,input,user,up ? 10 : 11,true);
                    callback.r3.u64 = input; callback.r4.u64 = user;
                    sub_82269038(callback,base);
                    if (receiver(gui) == input) button(callback,base,input,user,up ? 10 : 11,false);
                } catch (...) {
                    wheelRemainder = 0;
                    throw;
                }
                if (!sameScreen()) { cancel(current); wheelRemainder = 0; return; }
            }
        }
    }
}
}

PPC_FUNC(sub_821A13F0) {
    const uint32_t gui = ctx.r3.u32;
    __imp__sub_821A13F0(ctx, base);
    Doritos::Native::updateNativeGuiInput(ctx, base, gui);
}
PPC_FUNC(sub_821B9A70) {
    __imp__sub_821B9A70(ctx,base);
    // Original mouse context remains device7 for hit-testing/layer dispatch;
    // only the Lua-facing identity query scopes it to the real input owner.
    if (ctx.r3.u32 == 7 && Doritos::Native::mousePlayer) ctx.r3.u64 = *Doritos::Native::mousePlayer;
}
