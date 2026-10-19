#include "mouse_services.h"
#include "input_services.h"
#include <algorithm>
#include <cwchar>
#include <mutex>

namespace Doritos::Native {
namespace {
constexpr size_t maximumPendingEvents = 128;
std::mutex mouseMutex;
std::vector<NativeMouseEvent> pending;
unsigned heldButtons = 0;
HWND inputWindow = nullptr;
bool trackingLeave = false, havePosition = false, positionInside = false;
float positionX = 0, positionY = 0;
POINT clientPosition{};

#ifdef DORITOS_NATIVE_MOUSE_TEST_HOST
bool foreground(HWND window) { return MouseTestHost::foreground(window); }
bool clientRect(HWND window, RECT* rect) { return MouseTestHost::clientRect(window, rect); }
bool screenToClient(HWND window, POINT* point) { return MouseTestHost::screenToClient(window, point); }
HWND captureWindow() { return MouseTestHost::captureWindow(); }
void setCapture(HWND window) { MouseTestHost::setCapture(window); }
void releaseCapture() { MouseTestHost::releaseCapture(); }
void trackLeave(HWND window) { MouseTestHost::trackLeave(window); }
#else
bool foreground(HWND window) {
    if (!window || GetForegroundWindow() != window) return false;
    DWORD process = 0;
    if (!GetWindowThreadProcessId(window, &process) || process != GetCurrentProcessId()) return false;
    wchar_t name[64]{};
    return GetClassNameW(window, name, 64) && !std::wcscmp(name, L"DoritosNativeTitleWindow");
}
bool clientRect(HWND window, RECT* rect) { return GetClientRect(window, rect) != FALSE; }
bool screenToClient(HWND window, POINT* point) { return ScreenToClient(window, point) != FALSE; }
HWND captureWindow() { return GetCapture(); }
void setCapture(HWND window) { SetCapture(window); }
void releaseCapture() { ReleaseCapture(); }
void trackLeave(HWND window) {
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
    TrackMouseEvent(&tracking);
}
#endif
void cancelLocked() {
    pending.clear();
    pending.push_back({NativeMouseEventKind::Cancel, NativeMouseButton::Left, positionX, positionY, 0});
    heldButtons = 0;
    trackingLeave = false;
    havePosition = false;
    positionInside = false;
    clearNativeMouseSelection();
}
// Consecutive motion carries only the newest position. If actions exhaust the
// bounded queue, cancel the gesture rather than retaining an unmatched down.
bool enqueueLocked(const NativeMouseEvent& event) {
    if (event.kind == NativeMouseEventKind::Move && !pending.empty() && pending.back().kind == NativeMouseEventKind::Move) {
        pending.back() = event;
        return true;
    }
    if (pending.size() == maximumPendingEvents) {
        const auto motion = std::find_if(pending.begin(), pending.end(), [](const auto& item) {
            return item.kind == NativeMouseEventKind::Move;
        });
        if (motion != pending.end()) pending.erase(motion);
        else { cancelLocked(); return false; }
    }
    pending.push_back(event);
    return true;
}
void releaseOwnedCapture(HWND window) {
    if (window && captureWindow() == window) releaseCapture();
}
bool coordinates(HWND window, UINT message, LPARAM lparam, NativeMouseEvent& event, bool& inside, POINT& point) {
    RECT rect{};
    if (!clientRect(window, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) return false;
    point = {SHORT(LOWORD(lparam)), SHORT(HIWORD(lparam))};
    if (message == WM_MOUSEWHEEL && !screenToClient(window, &point)) return false;
    inside = point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
    event.x = std::clamp(float(point.x - rect.left) / float(rect.right - rect.left), 0.0f, 1.0f);
    event.y = std::clamp(float(point.y - rect.top) / float(rect.bottom - rect.top), 0.0f, 1.0f);
    return true;
}
}
void resetNativeMouseInput() {
    HWND window = nullptr;
    {
        std::lock_guard lock(mouseMutex);
        window = inputWindow;
        inputWindow = nullptr;
        cancelLocked();
    }
    releaseOwnedCapture(window);
}
std::vector<NativeMouseEvent> takeNativeMouseEvents() {
    std::lock_guard lock(mouseMutex);
    std::vector<NativeMouseEvent> events;
    events.swap(pending);
    return events;
}
std::optional<NativeMousePosition> latestNativeMousePosition() {
    std::lock_guard lock(mouseMutex);
    if (!havePosition || !positionInside) return {};
    return NativeMousePosition{positionX, positionY};
}
void clearNativeMousePosition() {
    std::lock_guard lock(mouseMutex);
    havePosition = false;
    positionInside = false;
    clearNativeMouseSelection();
}
bool handleNativeMouseMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_KILLFOCUS || message == WM_CANCELMODE || message == WM_DESTROY) {
        resetNativeMouseInput();
        return false; // Preserve the window's normal focus/lifetime handling.
    }
    if (message == WM_CAPTURECHANGED) {
        bool cancel = false;
        {
            std::lock_guard lock(mouseMutex);
            cancel = inputWindow == window && heldButtons && reinterpret_cast<HWND>(lparam) != window;
        }
        if (cancel) resetNativeMouseInput();
        return false;
    }
    if (message == WM_MOUSELEAVE) {
        if (!foreground(window)) { resetNativeMouseInput(); return true; }
        bool overflow = false;
        {
            std::lock_guard lock(mouseMutex);
            trackingLeave = false;
            if (havePosition) overflow = !enqueueLocked({NativeMouseEventKind::Leave, NativeMouseButton::Left, positionX, positionY, 0});
            havePosition = false;
            positionInside = false;
            clearNativeMouseSelection();
        }
        if (overflow) releaseOwnedCapture(window);
        return true;
    }
    NativeMouseEvent event{};
    switch (message) {
    case WM_MOUSEMOVE: event.kind = NativeMouseEventKind::Move; break;
    case WM_LBUTTONDOWN: event.kind = NativeMouseEventKind::ButtonDown; event.button = NativeMouseButton::Left; break;
    case WM_RBUTTONDOWN: event.kind = NativeMouseEventKind::ButtonDown; event.button = NativeMouseButton::Right; break;
    case WM_MBUTTONDOWN: event.kind = NativeMouseEventKind::ButtonDown; event.button = NativeMouseButton::Middle; break;
    case WM_LBUTTONUP: event.kind = NativeMouseEventKind::ButtonUp; event.button = NativeMouseButton::Left; break;
    case WM_RBUTTONUP: event.kind = NativeMouseEventKind::ButtonUp; event.button = NativeMouseButton::Right; break;
    case WM_MBUTTONUP: event.kind = NativeMouseEventKind::ButtonUp; event.button = NativeMouseButton::Middle; break;
    case WM_MOUSEWHEEL: event.kind = NativeMouseEventKind::Wheel; event.wheel = SHORT(HIWORD(wparam)); break;
    default: return false;
    }
    if (!foreground(window)) { resetNativeMouseInput(); return true; }
    bool inside = false;
    POINT point{};
    if (!coordinates(window, message, lparam, event, inside, point)) {
        clearNativeMousePosition();
        return true;
    }
    bool accepted = false, overflow = false, capture = false, release = false, track = false, outsideRelease = false, mouseUsed = true;
    {
        std::lock_guard lock(mouseMutex);
        const unsigned button = 1u << unsigned(event.button);
        if (!inside) {
            positionInside = false;
            clearNativeMouseSelection();
        }
        if (event.kind == NativeMouseEventKind::Move) {
            mouseUsed = !havePosition || point.x != clientPosition.x || point.y != clientPosition.y;
            if ((inside || (inputWindow == window && heldButtons)) &&
                (mouseUsed || event.x != positionX || event.y != positionY)) accepted = true;
        } else if (event.kind == NativeMouseEventKind::ButtonDown) {
            if (inside && !(heldButtons & button)) { heldButtons |= button; accepted = true; capture = true; }
        } else if (event.kind == NativeMouseEventKind::ButtonUp) {
            if (inputWindow == window && (heldButtons & button)) {
                if (!inside) outsideRelease = true;
                else { heldButtons &= ~button; accepted = true; release = heldButtons == 0; }
            }
        } else if (event.kind == NativeMouseEventKind::Wheel) accepted = inside && event.wheel != 0;
        if (accepted) {
            inputWindow = window;
            positionX = event.x; positionY = event.y; havePosition = true;
            positionInside = inside;
            clientPosition = point;
            overflow = !enqueueLocked(event);
            // Publish selection with the accepted snapshot, before window
            // bookkeeping can run alongside newer keyboard/controller input.
            // This notification only reads foreground state and stores atomics;
            // it never takes the keyboard lock or calls a guest handler.
            if (!overflow && mouseUsed) notifyNativeMouseInput(inside && (event.kind == NativeMouseEventKind::Move ||
                ((event.kind == NativeMouseEventKind::ButtonDown || event.kind == NativeMouseEventKind::ButtonUp) &&
                 event.button != NativeMouseButton::Middle)));
            if (inside && !trackingLeave && !overflow) { trackingLeave = true; track = true; }
        }
    }
    if (outsideRelease) { resetNativeMouseInput(); return true; }
    if (!accepted) return true;
    if (overflow) { releaseOwnedCapture(window); return true; }
    if (capture) {
        // Recheck before claiming capture, in case focus changed during dispatch.
        if (!foreground(window)) { resetNativeMouseInput(); return true; }
        if (captureWindow() != window) setCapture(window);
        if (captureWindow() != window) { resetNativeMouseInput(); return true; }
    }
    if (release) releaseOwnedCapture(window);
    if (track) trackLeave(window);
    return true;
}
#ifdef DORITOS_NATIVE_MOUSE_TEST_HOST
void enqueueNativeMouseEventForTests(const NativeMouseEvent& event) {
    HWND window = nullptr;
    bool overflow = false;
    {
        std::lock_guard lock(mouseMutex);
        window = inputWindow;
        overflow = !enqueueLocked(event);
    }
    if (overflow) releaseOwnedCapture(window);
}
#endif
}
