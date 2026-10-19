#pragma once
#include <windows.h>
#include <optional>
#include <vector>

namespace Doritos::Native {
enum class NativeMouseEventKind { Move, ButtonDown, ButtonUp, Wheel, Leave, Cancel };
enum class NativeMouseButton { Left, Right, Middle };
struct NativeMouseEvent {
    NativeMouseEventKind kind = NativeMouseEventKind::Move;
    NativeMouseButton button = NativeMouseButton::Left;
    float x = 0, y = 0; // Normalized coordinates within the current client area.
    int wheel = 0; // Signed Win32 wheel delta; WHEEL_DELTA is one detent.
};
struct NativeMousePosition { float x = 0, y = 0; };
// Called on the native window thread. Guest GUI handlers consume the queue on
// the guest thread; they are never called by this message handler.
bool handleNativeMouseMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
std::vector<NativeMouseEvent> takeNativeMouseEvents();
// Snapshot reads and invalidation are safe on the guest thread. Captured
// movement outside the client never supplies a stationary hover position.
std::optional<NativeMousePosition> latestNativeMousePosition();
void clearNativeMousePosition();
// Called on the window thread so any capture owned by that thread is released.
void resetNativeMouseInput();

#ifdef DORITOS_NATIVE_MOUSE_TEST_HOST
namespace MouseTestHost {
bool foreground(HWND window);
bool clientRect(HWND window, RECT* rect);
bool screenToClient(HWND window, POINT* point);
HWND captureWindow();
void setCapture(HWND window);
void releaseCapture();
void trackLeave(HWND window);
}
// Available only to the isolated queue test target.
void enqueueNativeMouseEventForTests(const NativeMouseEvent& event);
#endif
}
