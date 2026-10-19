#pragma once
#include <windows.h>
#include <xinput.h>
#include "ppc_context.h"

namespace Doritos::Native {
// Player one accepts keyboard alongside XInput. Keys are sampled only while
// the native title window is foreground; other users retain physical XInput.
constexpr bool nativeKeyboardFallbackAvailable(DWORD user, DWORD physicalStatus) {
    return user == 0 && physicalStatus == ERROR_DEVICE_NOT_CONNECTED;
}
enum class NativeInputDevice { KeyboardMouse, Gamepad };
NativeInputDevice nativeInputDevice();
bool nativeInputFocused();
void setNativeInputSuspended(bool suspended);
// Prompt identity and pointer selection are separate: wheel navigation uses
// keyboard/mouse prompts but must retain the newly navigated selection.
bool nativeMouseOwnsSelection();
void clearNativeMouseSelection();
void notifyNativeMouseInput(bool pointerSelection = true);
// The guest GUI update supplies true only for active, unpaused gameplay:
// Escape maps to Start there, and Back in pause/menu screens. A context change
// suppresses held Escape until release. P maps to Start; Enter/Space map to A.
void setNativeInputGameplay(bool gameplay);
DWORD nativeInputState(DWORD user, XINPUT_STATE* state);
bool inputServicesImport(PPCContext& ctx, uint8_t* base, const char* name);
#ifdef DORITOS_NATIVE_INPUT_TEST_HOST
// Only the isolated test target substitutes host reads. Production continues
// to call the actual Win32/XInput APIs directly and has no injectable host.
namespace InputTestHost {
DWORD state(DWORD user, XINPUT_STATE* output);
DWORD capabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* output);
DWORD keystroke(DWORD user, DWORD flags, XINPUT_KEYSTROKE* output);
bool foreground();
bool keyDown(int key);
ULONGLONG tickCount();
}
DWORD nativeInputCapabilitiesForTests(DWORD user, XINPUT_CAPABILITIES* caps);
DWORD nativeInputKeystrokeForTests(DWORD user, XINPUT_KEYSTROKE* stroke);
void resetNativeKeyboardInputForTests();
#endif
}
