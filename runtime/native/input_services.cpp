#include "input_services.h"
#include "runtime.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>

namespace Doritos::Native {
namespace {
std::mutex keyboardMutex;
XINPUT_GAMEPAD previous{};
DWORD packet = 0;
bool packetInitialized = false;
std::atomic<NativeInputDevice> inputDevice{NativeInputDevice::KeyboardMouse};
std::atomic<bool> mouseSelection{false};
std::atomic<bool> gameplayContext{false};
std::atomic<bool> inputSuspended{false};
constexpr std::array<int, 21> keyboardKeys{'P', VK_TAB, VK_SPACE, VK_RETURN, VK_ESCAPE, 'X', 'Y', 'Q', 'E',
    'A', 'D', 'W', 'S', VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_SHIFT, VK_CONTROL, 'R', VK_BACK};
std::array<bool, keyboardKeys.size()> previousKeyboardKeys{};
std::array<bool, keyboardKeys.size()> suppressedOverlayKeys{};
std::array<WORD,XUSER_MAX_COUNT> suppressedOverlayButtons{};
WORD escapeButton = 0;
bool escapeContext = false;
std::array<XINPUT_GAMEPAD, XUSER_MAX_COUNT> previousPhysical{};
std::mutex strokeMutex;
std::deque<XINPUT_KEYSTROKE> strokes;
std::array<bool, 18> oldStrokeKeys{};
std::array<ULONGLONG, 18> repeatAt{};
#ifdef DORITOS_NATIVE_INPUT_TEST_HOST
bool down(int key) { return InputTestHost::keyDown(key); }
bool titleForeground() { return InputTestHost::foreground(); }
DWORD physicalState(DWORD user, XINPUT_STATE* output) { return InputTestHost::state(user,output); }
DWORD physicalCapabilities(DWORD user, XINPUT_CAPABILITIES* output) {
    return InputTestHost::capabilities(user,XINPUT_FLAG_GAMEPAD,output);
}
DWORD physicalKeystroke(DWORD user, XINPUT_KEYSTROKE* output) { return InputTestHost::keystroke(user,0,output); }
ULONGLONG strokeTickCount() { return InputTestHost::tickCount(); }
#else
bool down(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }
bool titleForeground() {
    HWND window = GetForegroundWindow();
    DWORD process = 0;
    if (!window || !GetWindowThreadProcessId(window, &process) || process != GetCurrentProcessId()) return false;
    wchar_t name[64]{};
    return GetClassNameW(window, name, 64) && std::wcscmp(name, L"DoritosNativeTitleWindow") == 0;
}
DWORD physicalState(DWORD user, XINPUT_STATE* output) { return XInputGetState(user,output); }
DWORD physicalCapabilities(DWORD user, XINPUT_CAPABILITIES* output) {
    return XInputGetCapabilities(user,XINPUT_FLAG_GAMEPAD,output);
}
DWORD physicalKeystroke(DWORD user, XINPUT_KEYSTROKE* output) { return XInputGetKeystroke(user,0,output); }
ULONGLONG strokeTickCount() { return GetTickCount64(); }
#endif
void clearKeyboardStrokes() { strokes.clear(); oldStrokeKeys = {}; repeatAt = {}; }
struct KeyboardState {
    XINPUT_GAMEPAD gamepad{};
    bool horizontal = false, vertical = false, foreground = false, pressed = false;
};
KeyboardState keyboardState() {
    KeyboardState keys{};
    keys.foreground = titleForeground();
    if (!keys.foreground) {
        mouseSelection.store(false);
        { std::lock_guard lock(keyboardMutex); previousKeyboardKeys = {}; escapeButton = 0; }
        { std::lock_guard lock(strokeMutex); clearKeyboardStrokes(); }
        return keys;
    }
    std::array<bool, keyboardKeys.size()> held{};
    for (size_t index = 0; index < held.size(); ++index) held[index] = down(keyboardKeys[index]);
    const bool gameplay = gameplayContext.load();
    std::lock_guard lock(keyboardMutex);
    for(size_t index=0;index<held.size();++index) {
        suppressedOverlayKeys[index]=suppressedOverlayKeys[index] && held[index];
        if(suppressedOverlayKeys[index]) held[index]=false;
    }
    // Opening or closing pause changes Escape's binding. End the old button
    // without creating a new press from the same held physical key.
    if (!held[4]) escapeButton = 0;
    else if (!previousKeyboardKeys[4]) escapeButton = gameplay ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_B;
    else if (escapeContext != gameplay) escapeButton = 0;
    escapeContext = gameplay;
    auto button = [&](WORD mask, bool pressed) { if (pressed) keys.gamepad.wButtons |= mask; };
    button(XINPUT_GAMEPAD_START, held[0]);
    keys.gamepad.wButtons |= escapeButton;
    button(XINPUT_GAMEPAD_BACK, held[1] || held[20]);
    button(XINPUT_GAMEPAD_A, held[2] || held[3]);
    button(XINPUT_GAMEPAD_X, held[5]); button(XINPUT_GAMEPAD_Y, held[6] || held[19]);
    button(XINPUT_GAMEPAD_LEFT_SHOULDER, held[7]); button(XINPUT_GAMEPAD_RIGHT_SHOULDER, held[8]);
    const bool left = held[9] || held[13], right = held[10] || held[14];
    const bool up = held[11] || held[15], bottom = held[12] || held[16];
    keys.horizontal = left || right; keys.vertical = up || bottom;
    keys.gamepad.sThumbLX = left == right ? 0 : left ? -32768 : 32767;
    keys.gamepad.sThumbLY = up == bottom ? 0 : up ? 32767 : -32768;
    button(XINPUT_GAMEPAD_DPAD_LEFT, held[13]); button(XINPUT_GAMEPAD_DPAD_RIGHT, held[14]);
    button(XINPUT_GAMEPAD_DPAD_UP, held[15]); button(XINPUT_GAMEPAD_DPAD_DOWN, held[16]);
    keys.gamepad.bLeftTrigger = held[17] ? 255 : 0;
    keys.gamepad.bRightTrigger = held[18] ? 255 : 0;
    for (size_t index = 0; index < held.size(); ++index) {
        if (held[index] && !previousKeyboardKeys[index]) keys.pressed = true;
    }
    previousKeyboardKeys = held;
    return keys;
}
void forgetPhysical(DWORD user) {
    if (user >= previousPhysical.size()) return;
    std::lock_guard lock(keyboardMutex);
    previousPhysical[user] = {};
}
void observePhysical(DWORD user, const XINPUT_GAMEPAD& gamepad, bool foreground) {
    if (user >= previousPhysical.size()) return;
    std::lock_guard lock(keyboardMutex);
    const XINPUT_GAMEPAD& old = previousPhysical[user];
    auto axisUsed = [](SHORT now, SHORT old, int deadzone) {
        return (now > deadzone || now < -deadzone) && now != old;
    };
    const bool activity = (gamepad.wButtons & ~old.wButtons) != 0 ||
        (gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD && gamepad.bLeftTrigger != old.bLeftTrigger) ||
        (gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD && gamepad.bRightTrigger != old.bRightTrigger) ||
        axisUsed(gamepad.sThumbLX, old.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ||
        axisUsed(gamepad.sThumbLY, old.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ||
        axisUsed(gamepad.sThumbRX, old.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) ||
        axisUsed(gamepad.sThumbRY, old.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    if (foreground && activity) {
        inputDevice.store(NativeInputDevice::Gamepad);
        mouseSelection.store(false);
    }
    previousPhysical[user] = gamepad;
}
DWORD nativeCapabilities(DWORD user, XINPUT_CAPABILITIES* caps) {
    if (!caps) return ERROR_BAD_ARGUMENTS;
    const DWORD physical = physicalCapabilities(user,caps);
    if (physical == ERROR_DEVICE_NOT_CONNECTED) forgetPhysical(user);
    if (!nativeKeyboardFallbackAvailable(user,physical)) return physical;
    *caps = {};
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Gamepad.wButtons = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
        XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_LEFT_SHOULDER |
        XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y;
    caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 255;
    caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = SHORT(-1);
    return ERROR_SUCCESS;
}
DWORD nativeKeystroke(DWORD user, XINPUT_KEYSTROKE* stroke) {
    if (!stroke) return ERROR_BAD_ARGUMENTS;
    if(inputSuspended.load()) return ERROR_EMPTY;
    XINPUT_KEYSTROKE candidate{};
    const DWORD physical = physicalKeystroke(user,&candidate);
    if(physical==ERROR_SUCCESS) {
        constexpr std::array<WORD,14> virtualButtons{VK_PAD_DPAD_UP,VK_PAD_DPAD_DOWN,VK_PAD_DPAD_LEFT,VK_PAD_DPAD_RIGHT,
            VK_PAD_START,VK_PAD_BACK,VK_PAD_LSHOULDER,VK_PAD_RSHOULDER,VK_PAD_A,VK_PAD_B,VK_PAD_X,VK_PAD_Y,VK_PAD_LTHUMB_PRESS,VK_PAD_RTHUMB_PRESS};
        constexpr std::array<WORD,14> buttons{XINPUT_GAMEPAD_DPAD_UP,XINPUT_GAMEPAD_DPAD_DOWN,XINPUT_GAMEPAD_DPAD_LEFT,XINPUT_GAMEPAD_DPAD_RIGHT,
            XINPUT_GAMEPAD_START,XINPUT_GAMEPAD_BACK,XINPUT_GAMEPAD_LEFT_SHOULDER,XINPUT_GAMEPAD_RIGHT_SHOULDER,
            XINPUT_GAMEPAD_A,XINPUT_GAMEPAD_B,XINPUT_GAMEPAD_X,XINPUT_GAMEPAD_Y,XINPUT_GAMEPAD_LEFT_THUMB,XINPUT_GAMEPAD_RIGHT_THUMB};
        const DWORD physicalUser=user<XUSER_MAX_COUNT?user:candidate.UserIndex;
        if(physicalUser<XUSER_MAX_COUNT) {
            std::lock_guard lock(keyboardMutex);
            for(size_t index=0;index<buttons.size();++index) if(candidate.VirtualKey==virtualButtons[index] &&
                (suppressedOverlayButtons[physicalUser]&buttons[index])) {
                if(candidate.Flags&XINPUT_KEYSTROKE_KEYUP) suppressedOverlayButtons[physicalUser]&=~buttons[index];
                return ERROR_EMPTY;
            }
        }
        *stroke=candidate;
    }
    if (physical == ERROR_DEVICE_NOT_CONNECTED) forgetPhysical(user);
    if (user < XUSER_MAX_COUNT && physical == ERROR_SUCCESS && (stroke->Flags & XINPUT_KEYSTROKE_KEYDOWN) &&
        !(stroke->Flags & XINPUT_KEYSTROKE_REPEAT) && titleForeground()) {
        inputDevice.store(NativeInputDevice::Gamepad);
        mouseSelection.store(false);
    }
    if (user != 0 || (physical != ERROR_DEVICE_NOT_CONNECTED && physical != ERROR_EMPTY)) {
        std::lock_guard lock(strokeMutex);
        if (user == 0 && (physical != ERROR_SUCCESS || !titleForeground())) clearKeyboardStrokes();
        return physical;
    }
    if (!titleForeground()) {
        std::lock_guard lock(strokeMutex);
        clearKeyboardStrokes();
        return ERROR_EMPTY;
    }
    // Sample keyboard alone: merged controller state would duplicate physical
    // events. Sampling can clear strokes on focus loss, before this lock.
    const KeyboardState state = keyboardState();
    if (state.pressed) {
        inputDevice.store(NativeInputDevice::KeyboardMouse);
        mouseSelection.store(false);
    }
    std::lock_guard lock(strokeMutex);
    if (!titleForeground()) { clearKeyboardStrokes(); return ERROR_EMPTY; }
    constexpr std::array<WORD, 18> virtualKeys{VK_PAD_DPAD_UP, VK_PAD_DPAD_DOWN, VK_PAD_DPAD_LEFT, VK_PAD_DPAD_RIGHT,
        VK_PAD_START, VK_PAD_BACK, VK_PAD_LSHOULDER, VK_PAD_RSHOULDER, VK_PAD_A, VK_PAD_B, VK_PAD_X, VK_PAD_Y,
        VK_PAD_LTRIGGER, VK_PAD_RTRIGGER, VK_PAD_LTHUMB_UP, VK_PAD_LTHUMB_DOWN, VK_PAD_LTHUMB_LEFT, VK_PAD_LTHUMB_RIGHT};
    constexpr std::array<WORD, 12> buttons{XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y};
    std::array<bool, 18> now{};
    for (size_t index = 0; index < buttons.size(); ++index) now[index] = (state.gamepad.wButtons & buttons[index]) != 0;
    now[12] = state.gamepad.bLeftTrigger != 0; now[13] = state.gamepad.bRightTrigger != 0;
    now[14] = state.gamepad.sThumbLY > 0; now[15] = state.gamepad.sThumbLY < 0;
    now[16] = state.gamepad.sThumbLX < 0; now[17] = state.gamepad.sThumbLX > 0;
    const ULONGLONG tick = strokeTickCount();
    for (size_t index = 0; index < now.size(); ++index) {
        WORD flags = 0;
        if (now[index] != oldStrokeKeys[index]) {
            flags = now[index] ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
            repeatAt[index] = now[index] ? tick + 500 : 0;
        } else if (now[index] && tick >= repeatAt[index]) {
            flags = XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT;
            repeatAt[index] = tick + 100;
        }
        if (flags) strokes.push_back({virtualKeys[index], 0, flags, BYTE(user), 0});
    }
    oldStrokeKeys = now;
    if (strokes.empty()) return ERROR_EMPTY;
    *stroke = strokes.front(); strokes.pop_front();
    return ERROR_SUCCESS;
}
void write16(uint32_t address, WORD value) {
    const WORD big = _byteswap_ushort(value);
    memcpy(memory->base() + address, &big, 2);
}
void writeGamepad(uint32_t output, const XINPUT_GAMEPAD& gamepad) {
    write16(output, gamepad.wButtons);
    memory->base()[output + 2] = gamepad.bLeftTrigger; memory->base()[output + 3] = gamepad.bRightTrigger;
    write16(output + 4, WORD(gamepad.sThumbLX)); write16(output + 6, WORD(gamepad.sThumbLY));
    write16(output + 8, WORD(gamepad.sThumbRX)); write16(output + 10, WORD(gamepad.sThumbRY));
}
}
NativeInputDevice nativeInputDevice() { return inputDevice.load(); }
bool nativeInputFocused() {
    const bool focused = !inputSuspended.load() && titleForeground();
    if (!focused) mouseSelection.store(false);
    return focused;
}
void setNativeInputSuspended(bool suspended) {
    if(inputSuspended.load()==suspended) return;
    // Keep guest polling suspended until the held-input suppression snapshot
    // is installed. Publishing false first permits a closing-dialog key leak.
    if(suspended) inputSuspended.store(true);
    mouseSelection.store(false);
    std::array<bool,keyboardKeys.size()> held{};
    std::array<WORD,XUSER_MAX_COUNT> buttons{};
    if(!suspended) {
        for(size_t i=0;i<held.size();++i) held[i]=down(keyboardKeys[i]);
        for(DWORD user=0;user<XUSER_MAX_COUNT;++user) {
            XINPUT_STATE state{};
            if(physicalState(user,&state)==ERROR_SUCCESS) buttons[user]=state.Gamepad.wButtons;
            // Discard physical keystrokes queued while the native dialog had
            // ownership; a held confirm key must not reopen the menu on close.
            XINPUT_KEYSTROKE stroke{};
            for(uint32_t i=0;i<32 && physicalKeystroke(user,&stroke)==ERROR_SUCCESS;++i) {}
        }
    }
    std::scoped_lock lock(keyboardMutex,strokeMutex);
    suppressedOverlayKeys=held;suppressedOverlayButtons=buttons;
    previousKeyboardKeys={};escapeButton=0;clearKeyboardStrokes();
    if(!suspended) inputSuspended.store(false);
}
bool nativeMouseOwnsSelection() { return mouseSelection.load(); }
void clearNativeMouseSelection() { mouseSelection.store(false); }
void notifyNativeMouseInput(bool pointerSelection) {
    if (titleForeground()) {
        inputDevice.store(NativeInputDevice::KeyboardMouse);
        mouseSelection.store(pointerSelection);
    }
}
void setNativeInputGameplay(bool gameplay) { gameplayContext.store(gameplay); }
DWORD nativeInputState(DWORD user, XINPUT_STATE* state) {
    if (!state) return ERROR_BAD_ARGUMENTS;
    const DWORD physical = physicalState(user,state);
    if(inputSuspended.load()) {
        state->Gamepad={};state->dwPacketNumber=0;
        return user==0 && physical==ERROR_DEVICE_NOT_CONNECTED?ERROR_SUCCESS:physical;
    }
    if(user<XUSER_MAX_COUNT && physical==ERROR_SUCCESS) {
        std::lock_guard lock(keyboardMutex);
        suppressedOverlayButtons[user]&=state->Gamepad.wButtons;
        state->Gamepad.wButtons&=~suppressedOverlayButtons[user];
    }
    if (physical == ERROR_DEVICE_NOT_CONNECTED) forgetPhysical(user);
    if (user != 0) {
        if (physical == ERROR_SUCCESS && user < XUSER_MAX_COUNT) observePhysical(user, state->Gamepad, titleForeground());
        return physical;
    }
    if (physical != ERROR_SUCCESS && physical != ERROR_DEVICE_NOT_CONNECTED) return physical;
    const KeyboardState keys = keyboardState();
    XINPUT_GAMEPAD combined = physical == ERROR_SUCCESS ? state->Gamepad : XINPUT_GAMEPAD{};
    // Observe the actual pad before applying keyboard edges so keyboard wins
    // when both devices first become active in the same sample.
    observePhysical(user, combined, keys.foreground);
    if (keys.pressed) {
        inputDevice.store(NativeInputDevice::KeyboardMouse);
        mouseSelection.store(false);
    }
    combined.wButtons |= keys.gamepad.wButtons;
    combined.bLeftTrigger = std::max(combined.bLeftTrigger, keys.gamepad.bLeftTrigger);
    combined.bRightTrigger = std::max(combined.bRightTrigger, keys.gamepad.bRightTrigger);
    if (keys.horizontal) combined.sThumbLX = keys.gamepad.sThumbLX;
    if (keys.vertical) combined.sThumbLY = keys.gamepad.sThumbLY;
    std::lock_guard lock(keyboardMutex);
    if (!packetInitialized && physical == ERROR_SUCCESS) {
        packet = state->dwPacketNumber; previous = state->Gamepad;
    }
    packetInitialized = true;
    if (std::memcmp(&combined, &previous, sizeof(combined))) { previous = combined; ++packet; }
    state->dwPacketNumber = packet;
    state->Gamepad = combined;
    return ERROR_SUCCESS;
}
#ifdef DORITOS_NATIVE_INPUT_TEST_HOST
DWORD nativeInputCapabilitiesForTests(DWORD user, XINPUT_CAPABILITIES* caps) { return nativeCapabilities(user,caps); }
DWORD nativeInputKeystrokeForTests(DWORD user, XINPUT_KEYSTROKE* stroke) { return nativeKeystroke(user,stroke); }
void resetNativeKeyboardInputForTests() {
    std::scoped_lock lock(keyboardMutex,strokeMutex);
    previous = {}; packet = 0; packetInitialized = false; previousKeyboardKeys = {}; previousPhysical = {};
    suppressedOverlayKeys={};suppressedOverlayButtons={};inputSuspended.store(false);
    escapeButton = 0; escapeContext = false;
    inputDevice.store(NativeInputDevice::KeyboardMouse); mouseSelection.store(false);
    gameplayContext.store(false); clearKeyboardStrokes();
}
#endif
bool inputServicesImport(PPCContext& ctx, uint8_t*, const char* name) {
    const bool capabilities = !strcmp(name, "XamInputGetCapabilities"), state = !strcmp(name, "XamInputGetState"),
        vibration = !strcmp(name, "XamInputSetState"), keystroke = !strcmp(name, "XamInputGetKeystrokeEx");
    if (!capabilities && !state && !vibration && !keystroke) return false;
    const DWORD flags = ctx.r4.u32;
    if (!vibration && (flags & 0xff) && !(flags & XINPUT_FLAG_GAMEPAD)) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return true; }
    auto span = [&](uint32_t address, uint32_t bytes) {
        if (!memory || !address || !memory->valid(address, bytes)) fail("Input operation has an invalid guest buffer");
    };
    const uint32_t userPointer = keystroke ? ctx.r3.u32 : 0;
    if (keystroke) { if (!userPointer) { ctx.r3.u64 = ERROR_BAD_ARGUMENTS; return true; } span(userPointer, 4); }
    const DWORD requested = keystroke ? memory->read32(userPointer) : ctx.r3.u32;
    const bool any = (requested & 255) == 255 || (!vibration && (flags & 0x40000000));
    const uint32_t output = ctx.r5.u32;
    if (!output && !state) { ctx.r3.u64 = ERROR_BAD_ARGUMENTS; return true; }
    const DWORD first = any ? 0 : requested, end = any ? 4 : first + 1;
    if (first >= 4) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return true; }
    DWORD status = ERROR_DEVICE_NOT_CONNECTED;
    for (DWORD user = first; user < end; ++user) {
        DWORD current = ERROR_DEVICE_NOT_CONNECTED;
        if (state) {
            XINPUT_STATE input{}; current = nativeInputState(user, &input);
            if (current == ERROR_SUCCESS && output) {
                span(output, 16); memory->write32(output, input.dwPacketNumber); writeGamepad(output + 4, input.Gamepad);
            }
        } else if (capabilities) {
            XINPUT_CAPABILITIES caps{}; current = nativeCapabilities(user, &caps);
            if (current == ERROR_SUCCESS) {
                span(output, 20); memory->base()[output] = caps.Type; memory->base()[output + 1] = caps.SubType;
                write16(output + 2, caps.Flags); writeGamepad(output + 4, caps.Gamepad);
                write16(output + 16, caps.Vibration.wLeftMotorSpeed); write16(output + 18, caps.Vibration.wRightMotorSpeed);
            }
        } else if (vibration) {
            span(output, 4);
            XINPUT_VIBRATION motors{};
            memcpy(&motors, memory->base() + output, 4);
            motors.wLeftMotorSpeed = _byteswap_ushort(motors.wLeftMotorSpeed);
            motors.wRightMotorSpeed = _byteswap_ushort(motors.wRightMotorSpeed);
            current = XInputSetState(user, &motors);
        } else {
            XINPUT_KEYSTROKE stroke{}; current = nativeKeystroke(user, &stroke);
            if (current == ERROR_SUCCESS) {
                span(output, 8); write16(output, stroke.VirtualKey); write16(output + 2, stroke.Unicode);
                write16(output + 4, stroke.Flags); memory->base()[output + 6] = stroke.UserIndex;
                memory->base()[output + 7] = stroke.HidCode; memory->write32(userPointer, stroke.UserIndex);
            }
        }
        if (current == ERROR_SUCCESS) { status = current; break; }
        if (current != ERROR_DEVICE_NOT_CONNECTED) status = current;
    }
    ctx.r3.u64 = status;
    return true;
}
}
