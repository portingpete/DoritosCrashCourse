#include "runtime/native/input_services.h"
#include "runtime/native/runtime.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <vector>

PPCFuncMapping PPCFuncMappings[]{{0,nullptr}};
using namespace Doritos::Native;
namespace {
size_t checks = 0;
void require(bool value, const char* label) { ++checks; if (!value) throw std::runtime_error(label); }
struct Host {
    bool foreground = false;
    std::vector<bool> focusSequence;
    size_t focusIndex = 0;
    std::set<int> keys;
    ULONGLONG tick = 1000;
    DWORD stateStatus = ERROR_DEVICE_NOT_CONNECTED, capsStatus = ERROR_DEVICE_NOT_CONNECTED;
    DWORD strokeStatus = ERROR_DEVICE_NOT_CONNECTED;
    XINPUT_STATE state{};
    XINPUT_CAPABILITIES caps{};
    XINPUT_KEYSTROKE stroke{};
    uint32_t keyReads = 0, focusReads = 0, stateReads = 0, capsReads = 0, strokeReads = 0;
    bool pollOnKeyRead = false;
    DWORD stateUser = UINT32_MAX, capsUser = UINT32_MAX, strokeUser = UINT32_MAX;
} host;
void reset() { resetNativeKeyboardInputForTests(); host = {}; }
bool neutral(const XINPUT_GAMEPAD& gamepad) {
    const XINPUT_GAMEPAD zero{};
    return !std::memcmp(&gamepad,&zero,sizeof(zero));
}
void verifyConnectedNeutralBackground() {
    reset(); host.keys = {'D',VK_SPACE,VK_SHIFT};
    XINPUT_STATE state{};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && neutral(state.Gamepad),
        "Keyboard controller remains connected and neutral while background keys are held");
    require(host.keyReads == 0 && state.dwPacketNumber == 0,"Background state does not sample keyboard or invent a packet change");
    const auto focusReads = host.focusReads;
    XINPUT_CAPABILITIES caps{};
    require(nativeInputCapabilitiesForTests(0,&caps) == ERROR_SUCCESS &&
        caps.Type == XINPUT_DEVTYPE_GAMEPAD && caps.SubType == XINPUT_DEVSUBTYPE_GAMEPAD &&
        (caps.Gamepad.wButtons & XINPUT_GAMEPAD_A),"Keyboard capabilities remain available without foreground");
    require(host.keyReads == 0 && host.focusReads == focusReads,"Capabilities do not read keys or depend on foreground");
    host.foreground = true;
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.wButtons == XINPUT_GAMEPAD_A &&
        state.Gamepad.sThumbLX == 32767 && state.Gamepad.bLeftTrigger == 255 && state.dwPacketNumber == 1,
        "Foreground keyboard mapping preserves buttons stick trigger and packet");
    const auto keyReads = host.keyReads;
    host.foreground = false;
    require(nativeInputState(0,&state) == ERROR_SUCCESS && neutral(state.Gamepad) && state.dwPacketNumber == 2,
        "Focus loss reports a changed neutral packet instead of disconnected");
    require(host.keyReads == keyReads,"Focus-loss state reads no keys");
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.dwPacketNumber == 2 && neutral(state.Gamepad),
        "Repeated background neutral state keeps its packet stable");
    host.keys.clear(); host.foreground = true;
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.dwPacketNumber == 2 && neutral(state.Gamepad),
        "Released background keys do not reappear when foreground returns");
}
void verifyStrokeLifetime() {
    for (bool clearThroughState : {false,true}) {
        reset(); host.foreground = true; host.keys = {'X','Y'};
        XINPUT_KEYSTROKE stroke{};
        require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_SUCCESS && stroke.VirtualKey == VK_PAD_X,
            "Foreground event generation leaves a second keyboard stroke queued");
        host.foreground = false;
        const auto keyReads = host.keyReads;
        if (clearThroughState) {
            XINPUT_STATE state{};
            require(nativeInputState(0,&state) == ERROR_SUCCESS && neutral(state.Gamepad),
                "State-only background polling clears stale keyboard events");
        } else {
            const auto before = stroke;
            require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_EMPTY && !std::memcmp(&stroke,&before,sizeof(stroke)),
                "Background keyboard keystroke is empty and leaves output untouched");
        }
        require(host.keyReads == keyReads,"Background event clearing does not sample keys");
        host.keys.clear(); host.foreground = true;
        require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_EMPTY,
            "Neither a queued keydown nor stale keyup survives focus loss");
    }
    reset(); host.foreground = true; host.keys = {VK_SPACE};
    XINPUT_KEYSTROKE stroke{};
    require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_SUCCESS && stroke.VirtualKey == VK_PAD_A &&
        stroke.Flags == XINPUT_KEYSTROKE_KEYDOWN,"Foreground keystroke remains a real keyboard keydown");
    host.tick += 501;
    require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_SUCCESS &&
        stroke.Flags == (XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT),"Foreground held-key repeat is preserved");
    host.keys.clear();
    require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_SUCCESS && stroke.Flags == XINPUT_KEYSTROKE_KEYUP,
        "Foreground key release is preserved");
    require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_EMPTY,"Consumed foreground event queue becomes empty");
    reset(); host.keys = {VK_SPACE}; host.focusSequence = {true,false,false};
    require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_EMPTY && host.keyReads == 0,
        "Focus loss between event and state polling remains empty without key reads");
}
void verifyPhysicalAndErrorPriority() {
    reset();
    host.stateStatus = host.capsStatus = host.strokeStatus = ERROR_SUCCESS;
    host.state.dwPacketNumber = 1234; host.state.Gamepad.wButtons = XINPUT_GAMEPAD_B;
    host.state.Gamepad.sThumbRX = -123; host.state.Gamepad.bRightTrigger = 45;
    host.caps.Type = 9; host.caps.SubType = 7; host.caps.Flags = 0x2468;
    host.stroke = {VK_PAD_Y,WORD('z'),XINPUT_KEYSTROKE_KEYUP,BYTE(0),BYTE(6)};
    XINPUT_STATE state{}; XINPUT_CAPABILITIES caps{}; XINPUT_KEYSTROKE stroke{};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && !std::memcmp(&state,&host.state,sizeof(state)),
        "Physical state remains byte-identical even while title is background");
    require(nativeInputCapabilitiesForTests(0,&caps) == ERROR_SUCCESS && !std::memcmp(&caps,&host.caps,sizeof(caps)),
        "Physical capabilities remain byte-identical");
    require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_SUCCESS && !std::memcmp(&stroke,&host.stroke,sizeof(stroke)),
        "Physical keystroke remains byte-identical");
    host.strokeStatus = ERROR_EMPTY;
    require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_EMPTY,"Connected physical controller's empty queue is preserved");
    require(host.keyReads == 0,"Background physical routes never sample keyboard keys");
    require(nativeInputState(3,&state) == ERROR_SUCCESS && host.stateUser == 3 &&
        nativeInputCapabilitiesForTests(3,&caps) == ERROR_SUCCESS && host.capsUser == 3,
        "Physical controllers retain their actual nonzero user index");
    for (DWORD user : {1u,2u,3u}) {
        reset(); host.foreground = true; host.keys = {VK_SPACE};
        require(nativeInputState(user,&state) == ERROR_DEVICE_NOT_CONNECTED &&
            nativeInputCapabilitiesForTests(user,&caps) == ERROR_DEVICE_NOT_CONNECTED &&
            nativeInputKeystrokeForTests(user,&stroke) == ERROR_DEVICE_NOT_CONNECTED,
            "Other disconnected users do not acquire keyboard fallback");
        require(host.keyReads == 0 && host.focusReads == 0,"Other users do not sample player-one keyboard");
        require(host.stateUser == user && host.capsUser == user && host.strokeUser == user,
            "Disconnected user indices are forwarded unchanged to all physical APIs");
    }
    for (DWORD error : {DWORD(ERROR_BAD_ARGUMENTS),DWORD(ERROR_NOT_SUPPORTED)}) {
        reset(); host.foreground = true; host.keys = {VK_SPACE};
        host.stateStatus = host.capsStatus = host.strokeStatus = error;
        require(nativeInputState(0,&state) == error && nativeInputCapabilitiesForTests(0,&caps) == error &&
            nativeInputKeystrokeForTests(0,&stroke) == error,"Non-disconnection host errors retain their exact return status");
        require(host.keyReads == 0 && host.focusReads == 0,"Host errors do not activate keyboard fallback");
    }
    reset(); host.foreground = true; host.keys = {VK_SPACE};
    host.stateStatus = host.capsStatus = ERROR_EMPTY;
    require(nativeInputState(0,&state) == ERROR_EMPTY && nativeInputCapabilitiesForTests(0,&caps) == ERROR_EMPTY &&
        host.keyReads == 0,"Unexpected EMPTY state/capability errors retain their status without keyboard fallback");
    reset();
    require(nativeInputState(0,nullptr) == ERROR_BAD_ARGUMENTS && host.stateReads == 0,
        "Null state is rejected before any host read");
    require(nativeInputCapabilitiesForTests(0,nullptr) == ERROR_BAD_ARGUMENTS &&
        nativeInputKeystrokeForTests(0,nullptr) == ERROR_BAD_ARGUMENTS && host.keyReads == 0,
        "Host null-output errors preserve status and never activate keyboard fallback");
}
void verifyKeyboardBindingsAndContext() {
    reset(); host.foreground = true;
    XINPUT_STATE state{};
    host.keys = {VK_RETURN};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.wButtons == XINPUT_GAMEPAD_A,
        "Enter confirms without also opening the pause menu");
    host.keys = {'P'};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.wButtons == XINPUT_GAMEPAD_START,
        "P provides a dedicated pause/Start binding");
    host.keys = {VK_ESCAPE};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.wButtons == XINPUT_GAMEPAD_B,
        "Escape goes back in menus");
    host.keys.clear();
    require(nativeInputState(0,&state) == ERROR_SUCCESS && neutral(state.Gamepad),
        "Menu Escape releases before starting a new gameplay press");
    setNativeInputGameplay(true);
    host.keys = {VK_ESCAPE};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.wButtons == XINPUT_GAMEPAD_START,
        "Escape opens pause during active gameplay");
    setNativeInputGameplay(false);
    host.keys = {'W','A'};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.sThumbLX == -32768 &&
        state.Gamepad.sThumbLY == 32767 && state.Gamepad.wButtons == 0,"WASD supplies full directional movement");
    host.keys = {VK_DOWN,VK_RIGHT};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.sThumbLX == 32767 &&
        state.Gamepad.sThumbLY == -32768 && state.Gamepad.wButtons == (XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT),
        "Arrow keys provide stick movement and directional menu navigation");
    host.keys = {VK_TAB,'Q','E','X','Y',VK_SHIFT,VK_CONTROL};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.wButtons ==
        (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y) &&
        state.Gamepad.bLeftTrigger == 255 && state.Gamepad.bRightTrigger == 255,
        "Remaining face buttons shoulders triggers and Back have keyboard bindings");
    host.keys = {'R',VK_BACK};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.wButtons == (XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_BACK),
        "R and Backspace expose checkpoint and level restart aliases");
}
void verifyEscapeContextTransition() {
    for (bool initialGameplay : {false,true}) {
        reset(); host.foreground = true; host.keys = {VK_ESCAPE};
        setNativeInputGameplay(initialGameplay);
        const WORD initialButton = initialGameplay ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_B;
        const WORD nextButton = initialGameplay ? XINPUT_GAMEPAD_B : XINPUT_GAMEPAD_START;
        const WORD initialKey = initialGameplay ? VK_PAD_START : VK_PAD_B;
        const WORD nextKey = initialGameplay ? VK_PAD_B : VK_PAD_START;
        XINPUT_STATE state{};
        XINPUT_KEYSTROKE stroke{};
        require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.wButtons == initialButton,
            "Escape selects the original context on its physical key press");
        require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_SUCCESS && stroke.VirtualKey == initialKey &&
            stroke.Flags == XINPUT_KEYSTROKE_KEYDOWN,"Escape emits the original context keydown");
        setNativeInputGameplay(!initialGameplay);
        require(nativeInputState(0,&state) == ERROR_SUCCESS && neutral(state.Gamepad),
            "Pause or resume context changes end held Escape without a second button press");
        require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_SUCCESS && stroke.VirtualKey == initialKey &&
            stroke.Flags == XINPUT_KEYSTROKE_KEYUP,"Context changes release the original Escape virtual key");
        host.tick += 1000;
        require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_EMPTY,
            "Holding Escape across context changes cannot repeat Start or synthesize Back");
        setNativeInputGameplay(initialGameplay);
        require(nativeInputState(0,&state) == ERROR_SUCCESS && neutral(state.Gamepad),
            "Returning to an earlier context cannot revive suppressed held Escape");
        setNativeInputGameplay(!initialGameplay);
        host.keys.clear();
        require(nativeInputState(0,&state) == ERROR_SUCCESS && neutral(state.Gamepad),
            "Physical Escape release resets its contextual binding");
        host.keys = {VK_ESCAPE};
        require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.wButtons == nextButton,
            "A fresh Escape press uses Back in pause menus or Start in active gameplay");
        require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_SUCCESS && stroke.VirtualKey == nextKey &&
            stroke.Flags == XINPUT_KEYSTROKE_KEYDOWN,"New Escape press emits only the new context keydown");
    }
    reset(); host.foreground = true; host.keys = {'P',VK_ESCAPE};
    setNativeInputGameplay(true);
    XINPUT_STATE state{};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.wButtons == XINPUT_GAMEPAD_START,
        "P and Escape can independently hold the Start button");
    setNativeInputGameplay(false);
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.wButtons == XINPUT_GAMEPAD_START,
        "Suppressing context-changed Escape preserves an independently held P binding");
}
void verifyConcurrentKeyboardAndController() {
    reset(); host.foreground = true; host.stateStatus = ERROR_SUCCESS; host.strokeStatus = ERROR_EMPTY;
    host.state.dwPacketNumber = 41; host.state.Gamepad.wButtons = XINPUT_GAMEPAD_B;
    host.state.Gamepad.sThumbLX = 1000; host.state.Gamepad.sThumbLY = 2000; host.state.Gamepad.sThumbRX = -123;
    host.state.Gamepad.bLeftTrigger = 15; host.state.Gamepad.bRightTrigger = 45;
    host.keys = {'D',VK_SPACE,VK_SHIFT};
    XINPUT_STATE state{};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.wButtons == (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B) &&
        state.Gamepad.sThumbLX == 32767 && state.Gamepad.sThumbLY == 2000 && state.Gamepad.sThumbRX == -123 &&
        state.Gamepad.bLeftTrigger == 255 && state.Gamepad.bRightTrigger == 45 && state.dwPacketNumber == 42,
        "Connected player-one pad and keyboard combine buttons triggers and active keyboard axes");
    host.keys.clear();
    require(nativeInputState(0,&state) == ERROR_SUCCESS && !std::memcmp(&state.Gamepad,&host.state.Gamepad,sizeof(state.Gamepad)) &&
        state.dwPacketNumber == 43,"Keyboard release restores physical axes and increments the effective packet");
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.dwPacketNumber == 43,
        "Unchanged combined state keeps the effective packet stable");
    host.keys = {'A','D'};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && state.Gamepad.sThumbLX == 0,
        "Opposing keyboard directions neutralize their axis instead of leaking held controller movement");
    const auto keyReads = host.keyReads;
    require(nativeInputState(2,&state) == ERROR_SUCCESS && !std::memcmp(&state,&host.state,sizeof(state)) && host.keyReads == keyReads,
        "Connected nonzero users retain byte-identical XInput state without keyboard reads");
    host.keys = {VK_SPACE};
    XINPUT_KEYSTROKE stroke{};
    require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_SUCCESS && stroke.VirtualKey == VK_PAD_A &&
        stroke.Flags == XINPUT_KEYSTROKE_KEYDOWN,"Keyboard generates events while a connected controller queue is empty");
    host.keys.clear();
    require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_SUCCESS && stroke.VirtualKey == VK_PAD_A &&
        stroke.Flags == XINPUT_KEYSTROKE_KEYUP,"Connected-pad keyboard release preserves its event");
    require(nativeInputKeystrokeForTests(0,&stroke) == ERROR_EMPTY,
        "Controller buttons are not duplicated as keyboard keystrokes");
}
void verifyPromptDeviceSwitching() {
    reset(); host.foreground = true; host.stateStatus = ERROR_SUCCESS;
    XINPUT_STATE state{};
    require(nativeInputFocused() && host.keyReads == 0,"Focus API uses the title guard without sampling keyboard keys");
    require(nativeInputDevice() == NativeInputDevice::KeyboardMouse,"Native controls default to keyboard/mouse prompts");
    host.state.Gamepad.sThumbLX = 1000; host.state.Gamepad.bLeftTrigger = 10;
    require(nativeInputState(0,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "Controller stick drift and trigger noise do not change input prompts");
    host.state.Gamepad.wButtons = XINPUT_GAMEPAD_B;
    require(nativeInputState(0,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::Gamepad,
        "A physical button press selects gamepad prompts");
    host.keys = {VK_SPACE};
    require(nativeInputState(0,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "A new keyboard press selects keyboard/mouse prompts even with the controller connected");
    require(nativeInputState(0,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "An unchanged held controller cannot steal prompts from keyboard activity");
    host.state.Gamepad.sThumbRX = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE + 100;
    require(nativeInputState(0,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::Gamepad,
        "Meaningful physical stick movement selects gamepad prompts");
    notifyNativeMouseInput();
    require(nativeInputDevice() == NativeInputDevice::KeyboardMouse,"Focused mouse use selects keyboard/mouse prompts");
    require(nativeInputState(0,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "Held keyboard and physical controls do not overwrite a mouse-selected device");
    host.state.Gamepad.wButtons |= XINPUT_GAMEPAD_X;
    require(nativeInputState(0,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::Gamepad,
        "A later physical button edge switches prompts back to gamepad");
    host.foreground = false;
    require(!nativeInputFocused(),"Focus API rejects background title input");
    const auto keyReads = host.keyReads;
    notifyNativeMouseInput(); host.keys = {'P'}; host.state.Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
    require(nativeInputState(0,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::Gamepad && host.keyReads == keyReads,
        "Background mouse and keyboard do not change prompts or sample keys");
}
void verifyPointerSelectionOwnership() {
    reset(); host.foreground = true; host.stateStatus = ERROR_SUCCESS;
    XINPUT_STATE state{};
    require(!nativeMouseOwnsSelection(), "Native menu selection initially waits for actual pointer activity");
    notifyNativeMouseInput();
    require(nativeMouseOwnsSelection() && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "Actual pointer use acquires hover selection alongside keyboard/mouse prompts");
    host.state.Gamepad.sThumbLX = 1000; host.state.Gamepad.bLeftTrigger = 10;
    nativeInputState(0,&state);
    require(nativeMouseOwnsSelection(), "Controller drift and trigger noise cannot suppress stationary hover");
    host.keys = {VK_DOWN}; nativeInputState(0,&state);
    require(!nativeMouseOwnsSelection() && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "A new keyboard navigation press relinquishes hover selection while retaining keyboard/mouse prompts");
    notifyNativeMouseInput(); nativeInputState(0,&state);
    require(nativeMouseOwnsSelection(), "Unchanged held keyboard and controller states cannot reclaim selection after actual mouse use");
    host.state.Gamepad.wButtons = XINPUT_GAMEPAD_DPAD_UP; nativeInputState(0,&state);
    require(!nativeMouseOwnsSelection() && nativeInputDevice() == NativeInputDevice::Gamepad,
        "A meaningful controller edge takes menu selection away from stationary hover");
    notifyNativeMouseInput(false);
    require(!nativeMouseOwnsSelection() && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "Wheel navigation selects keyboard/mouse prompts without acquiring pointer hover selection");
    notifyNativeMouseInput(); clearNativeMouseSelection();
    require(!nativeMouseOwnsSelection() && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "Pointer cancellation clears hover ownership independently of prompt identity");
    notifyNativeMouseInput(); host.foreground = false;
    require(!nativeInputFocused() && !nativeMouseOwnsSelection(), "Focus polling relinquishes stationary hover ownership");
    host.foreground = true;
    require(nativeInputFocused() && !nativeMouseOwnsSelection(), "Focus restoration cannot revive stale pointer selection");

    reset(); host.foreground = true; host.strokeStatus = ERROR_SUCCESS;
    host.stroke = {VK_PAD_A,0,XINPUT_KEYSTROKE_KEYDOWN,BYTE(2),0};
    XINPUT_KEYSTROKE stroke{}; notifyNativeMouseInput(); nativeInputKeystrokeForTests(2,&stroke);
    require(!nativeMouseOwnsSelection(), "Physical keystroke presses from another controller relinquish pointer hover");
    notifyNativeMouseInput(); host.stroke.Flags |= XINPUT_KEYSTROKE_REPEAT;
    nativeInputKeystrokeForTests(2,&stroke);
    require(nativeMouseOwnsSelection(), "Unchanged physical key repeats do not steal selection from recent pointer use");
    reset(); host.foreground = true; host.keys = {VK_RETURN}; notifyNativeMouseInput();
    nativeInputKeystrokeForTests(0,&stroke);
    require(!nativeMouseOwnsSelection(), "Keyboard-only keystroke sampling relinquishes pointer selection on a new press");
}
void verifyEveryPhysicalUserPromptSwitching() {
    reset(); host.foreground = true; host.stateStatus = ERROR_SUCCESS; host.keys = {VK_SPACE};
    host.state.dwPacketNumber = 77; host.state.Gamepad.wButtons = XINPUT_GAMEPAD_B;
    const XINPUT_STATE firstPad = host.state;
    XINPUT_STATE state{};
    require(nativeInputState(1,&state) == ERROR_SUCCESS && !std::memcmp(&state,&host.state,sizeof(state)) &&
        nativeInputDevice() == NativeInputDevice::Gamepad && host.keyReads == 0,
        "A meaningful player-two pad edge selects gamepad prompts without changing its physical state or sampling keyboard");
    notifyNativeMouseInput();
    require(nativeInputState(1,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::KeyboardMouse &&
        !std::memcmp(&state,&firstPad,sizeof(state)),
        "A held player-two pad cannot steal prompts back after mouse activity");
    host.state = {}; host.state.dwPacketNumber = 91; host.state.Gamepad.sThumbRX = 1000;
    require(nativeInputState(2,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "An additional controller's stick drift does not change prompts");
    host.state.Gamepad.sThumbLX = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE + 100;
    require(nativeInputState(2,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::Gamepad &&
        !std::memcmp(&state,&host.state,sizeof(state)),
        "Meaningful player-three analog input selects gamepad prompts while preserving its physical packet");
    notifyNativeMouseInput();
    require(nativeInputState(2,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "Unchanged additional-controller axes do not overwrite mouse-selected prompts");
    host.stateStatus = ERROR_DEVICE_NOT_CONNECTED;
    const auto beforeDisconnected = state;
    require(nativeInputState(2,&state) == ERROR_DEVICE_NOT_CONNECTED && nativeInputDevice() == NativeInputDevice::KeyboardMouse &&
        !std::memcmp(&state,&beforeDisconnected,sizeof(state)),
        "Additional-controller disconnection preserves the host status and output without changing the prompt device");
    host.stateStatus = ERROR_SUCCESS;
    require(nativeInputState(2,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::Gamepad,
        "A reconnected controller's meaningful state is observed after its own history resets");
    notifyNativeMouseInput(); host.state = firstPad;
    require(nativeInputState(1,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "Disconnecting another controller leaves player-two held-input history intact");
    host.state = {}; host.state.Gamepad.bRightTrigger = XINPUT_GAMEPAD_TRIGGER_THRESHOLD + 1;
    require(nativeInputState(3,&state) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::Gamepad && host.keyReads == 0,
        "Player-four trigger use also selects prompts without enabling keyboard merging for that user");

    reset(); host.foreground = true; host.strokeStatus = ERROR_SUCCESS;
    host.stroke = {VK_PAD_A,0,XINPUT_KEYSTROKE_KEYDOWN,BYTE(3),0};
    XINPUT_KEYSTROKE stroke{};
    require(nativeInputKeystrokeForTests(3,&stroke) == ERROR_SUCCESS && !std::memcmp(&stroke,&host.stroke,sizeof(stroke)) &&
        nativeInputDevice() == NativeInputDevice::Gamepad,
        "Real gamepad keystroke presses select prompts for any physical user and retain their exact ABI");
    notifyNativeMouseInput(); host.stroke.Flags = XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT;
    require(nativeInputKeystrokeForTests(3,&stroke) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "Held-key repeats from other gamepads cannot steal mouse-selected prompts");
    host.stroke.Flags = XINPUT_KEYSTROKE_KEYUP;
    require(nativeInputKeystrokeForTests(3,&stroke) == ERROR_SUCCESS && nativeInputDevice() == NativeInputDevice::KeyboardMouse,
        "Other gamepads' release events preserve the most recent input device");
    host.foreground = false; host.stroke.Flags = XINPUT_KEYSTROKE_KEYDOWN; host.stroke.VirtualKey = VK_PAD_X;
    require(nativeInputKeystrokeForTests(3,&stroke) == ERROR_SUCCESS && !std::memcmp(&stroke,&host.stroke,sizeof(stroke)) &&
        nativeInputDevice() == NativeInputDevice::KeyboardMouse && host.keyReads == 0,
        "Background additional-pad events retain physical behavior without changing prompts or reading keys");
}
}
namespace Doritos::Native::InputTestHost {
DWORD state(DWORD user, XINPUT_STATE* output) {
    ++host.stateReads; host.stateUser = user; if (!output) return ERROR_BAD_ARGUMENTS;
    if (host.stateStatus == ERROR_SUCCESS) *output = host.state;
    return host.stateStatus;
}
DWORD capabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* output) {
    ++host.capsReads; host.capsUser = user;
    require(flags == XINPUT_FLAG_GAMEPAD,"Actual capability host call retains GAMEPAD flag");
    if (!output) return ERROR_BAD_ARGUMENTS;
    if (host.capsStatus == ERROR_SUCCESS) *output = host.caps;
    return host.capsStatus;
}
DWORD keystroke(DWORD user, DWORD flags, XINPUT_KEYSTROKE* output) {
    ++host.strokeReads; host.strokeUser = user;
    require(flags == 0,"Actual keystroke host call retains zero flag");
    if (!output) return ERROR_BAD_ARGUMENTS;
    if (host.strokeStatus == ERROR_SUCCESS) *output = host.stroke;
    return host.strokeStatus;
}
bool foreground() {
    ++host.focusReads;
    if (host.focusIndex < host.focusSequence.size()) return host.focusSequence[host.focusIndex++];
    return host.foreground;
}
bool keyDown(int key) {
    ++host.keyReads;
    if(host.pollOnKeyRead) {
        host.pollOnKeyRead=false;
        XINPUT_STATE state{};
        require(nativeInputState(0,&state)==ERROR_SUCCESS && neutral(state.Gamepad),
            "A guest poll during dialog-close key scanning must remain neutral");
    }
    return host.keys.contains(key);
}
ULONGLONG tickCount() { return host.tick; }
}
int main() {
    try {
        require(nativeKeyboardFallbackAvailable(0,ERROR_DEVICE_NOT_CONNECTED),"Disconnected user0 owns keyboard fallback");
        require(!nativeKeyboardFallbackAvailable(1,ERROR_DEVICE_NOT_CONNECTED) &&
            !nativeKeyboardFallbackAvailable(0,ERROR_SUCCESS),"Capability fallback remains limited to disconnected player one");
        verifyConnectedNeutralBackground(); verifyStrokeLifetime(); verifyPhysicalAndErrorPriority();
        verifyKeyboardBindingsAndContext(); verifyConcurrentKeyboardAndController(); verifyPromptDeviceSwitching();
        verifyEveryPhysicalUserPromptSwitching(); verifyEscapeContextTransition(); verifyPointerSelectionOwnership();
        reset();host.foreground=true;host.keys={VK_SPACE,VK_RIGHT};host.stateStatus=ERROR_SUCCESS;
        host.state.Gamepad.wButtons=XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START;
        setNativeInputSuspended(true);
        XINPUT_STATE suspended{};
        for(DWORD user=0;user<XUSER_MAX_COUNT;++user)
            require(nativeInputState(user,&suspended)==ERROR_SUCCESS && neutral(suspended.Gamepad),
                "Native settings suppress every physical and keyboard controller while open");
        XINPUT_KEYSTROKE untouched{};untouched.VirtualKey=VK_PAD_A;
        require(nativeInputKeystrokeForTests(0,&untouched)==ERROR_EMPTY && untouched.VirtualKey==VK_PAD_A,
            "Native settings consume no controller keystroke or mutate empty output");
        require(!nativeInputFocused() && host.keyReads==0 && host.stateReads==XUSER_MAX_COUNT,
            "Native settings retain connection queries while withholding controller buttons and keyboard input");
        host.pollOnKeyRead=true;
        setNativeInputSuspended(false);
        require(nativeInputState(0,&suspended)==ERROR_SUCCESS && neutral(suspended.Gamepad),
            "Held dialog confirm keys and pad buttons cannot reopen the guest menu on close");
        host.strokeStatus=ERROR_SUCCESS;host.stroke={VK_PAD_A,0,WORD(XINPUT_KEYSTROKE_KEYDOWN|XINPUT_KEYSTROKE_REPEAT),0,0};
        untouched.VirtualKey=VK_PAD_X;
        require(nativeInputKeystrokeForTests(0,&untouched)==ERROR_EMPTY && untouched.VirtualKey==VK_PAD_X,
            "Repeating physical confirm strokes held through dialog close stay suppressed");
        host.stroke.Flags=XINPUT_KEYSTROKE_KEYUP;
        require(nativeInputKeystrokeForTests(0,&untouched)==ERROR_EMPTY && untouched.VirtualKey==VK_PAD_X,
            "A suppressed physical confirm release clears the latch without leaking an event");
        host.stroke.Flags=XINPUT_KEYSTROKE_KEYDOWN;
        require(nativeInputKeystrokeForTests(0,&untouched)==ERROR_SUCCESS && untouched.VirtualKey==VK_PAD_A,
            "A new physical confirm press resumes after its suppressed release");
        host.keys.clear();host.state.Gamepad={};nativeInputState(0,&suspended);
        host.keys={VK_SPACE};host.state.Gamepad.wButtons=XINPUT_GAMEPAD_A;
        require(nativeInputState(0,&suspended)==ERROR_SUCCESS && suspended.Gamepad.wButtons,
            "New guest input resumes after dialog keys have been released");
        std::printf("Native keyboard/controller contracts passed: %zu checks; no OS keyboard or foreground input used\n",checks);
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"Native input contract failed: %s\n",error.what()); return 1; }
}
