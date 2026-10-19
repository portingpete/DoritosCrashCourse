#include "runtime/native/mouse_services.h"
#include "runtime/native/input_services.h"
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <stdexcept>

using namespace Doritos::Native;
namespace {
size_t checks = 0;
void require(bool value, const char* label) { ++checks; if (!value) throw std::runtime_error(label); }
bool close(float a, float b) { return std::fabs(a - b) < 0.00001f; }
const HWND title = reinterpret_cast<HWND>(uintptr_t(1));
const HWND foreign = reinterpret_cast<HWND>(uintptr_t(2));
struct Host {
    bool focused = true, rectAvailable = true, conversionAvailable = true, captureAvailable = true, pointerSelection = false;
    bool keyboardNavigationOnTrack = false;
    RECT rect{0, 0, 800, 400};
    POINT origin{100, 200};
    HWND capture = nullptr;
    size_t captures = 0, releases = 0, tracks = 0, deviceNotifications = 0;
} host;
LPARAM point(int x, int y) { return MAKELPARAM(WORD(x), WORD(y)); }
WPARAM wheel(int delta) { return MAKEWPARAM(0, WORD(delta)); }
void reset() {
    resetNativeMouseInput();
    takeNativeMouseEvents();
    host = {};
}
void verifyMotionAndActionOrder() {
    reset();
    require(!handleNativeMouseMessage(title, WM_KEYDOWN, 'A', 0), "Non-mouse messages retain their normal window route");
    require(handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(100, 50)), "Native motion message is consumed");
    handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(100, 50));
    handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(200, 100));
    require(host.deviceNotifications == 2 && host.tracks == 1, "Only changed motion notifies the input device and leave tracking begins once");
    handleNativeMouseMessage(title, WM_LBUTTONDOWN, 0, point(200, 100));
    handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(300, 150));
    handleNativeMouseMessage(title, WM_LBUTTONUP, 0, point(300, 150));
    const auto events = takeNativeMouseEvents();
    require(events.size() == 4 && events[0].kind == NativeMouseEventKind::Move && events[1].kind == NativeMouseEventKind::ButtonDown &&
        events[2].kind == NativeMouseEventKind::Move && events[3].kind == NativeMouseEventKind::ButtonUp,
        "Motion coalescing preserves motion/down/motion/up ordering");
    require(close(events[0].x, 0.25f) && close(events[0].y, 0.25f) && close(events[3].x, 0.375f),
        "Queued coordinates normalize against the actual client area");
    require(host.captures == 1 && host.releases == 1 && host.capture == nullptr,
        "Accepted down captures the title and the final up releases owned capture");
    require(takeNativeMouseEvents().empty(), "Taking events drains the queue exactly once");
    const auto latest = latestNativeMousePosition();
    require(latest && close(latest->x,0.375f) && close(latest->y,0.375f) && nativeMouseOwnsSelection(),
        "Draining actions retains the validated latest in-client position and pointer selection ownership");
    clearNativeMouseSelection();
    handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(300, 150));
    require(takeNativeMouseEvents().empty() && !nativeMouseOwnsSelection(),
        "A stationary duplicate motion cannot reclaim selection after keyboard or controller navigation");
    handleNativeMouseMessage(title, WM_LBUTTONUP, 0, point(300, 150));
    require(takeNativeMouseEvents().empty(), "Unmatched button-up messages do not invent mouse gestures");
}
void verifySelectionPublicationOrder() {
    reset(); host.keyboardNavigationOnTrack = true;
    handleNativeMouseMessage(title,WM_MOUSEMOVE,0,point(100,50));
    const auto events = takeNativeMouseEvents();
    require(events.size() == 1 && events[0].kind == NativeMouseEventKind::Move && latestNativeMousePosition() &&
        host.deviceNotifications == 1 && !nativeMouseOwnsSelection(),
        "Newer keyboard navigation after accepted snapshot publication cannot be overwritten by delayed mouse ownership");
}
void verifyCoordinatesAndWheel() {
    reset();
    handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(400, 200));
    auto events = takeNativeMouseEvents();
    require(events.size() == 1 && close(events[0].x, 0.5f) && close(events[0].y, 0.5f), "Initial client center maps to normalized center");
    host.rect = {0, 0, 1600, 800};
    handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(800, 400));
    events = takeNativeMouseEvents();
    require(events.size() == 1 && close(events[0].x, 0.5f) && close(events[0].y, 0.5f) && host.deviceNotifications == 2,
        "A resized client uses current dimensions and preserves genuine motion at the same normalized position");
    host.rect = {0, 0, 800, 400};
    handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(400, 200));
    takeNativeMouseEvents();
    const auto notifications = host.deviceNotifications;
    clearNativeMouseSelection();
    host.rect = {0, 0, 1600, 800};
    handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(400, 200));
    events = takeNativeMouseEvents();
    require(events.size() == 1 && close(events[0].x, 0.25f) && host.deviceNotifications == notifications,
        "Resize-only normalized-coordinate changes do not steal prompts from another input device");
    require(!nativeMouseOwnsSelection(), "Resize-only motion cannot reclaim stationary pointer selection");
    auto latest = latestNativeMousePosition();
    require(latest && close(latest->x,0.25f) && close(latest->y,0.25f),
        "Resize-only normalized geometry still updates the latest valid pointer snapshot");
    handleNativeMouseMessage(title, WM_MOUSEWHEEL, wheel(120), point(900, 600));
    handleNativeMouseMessage(title, WM_MOUSEWHEEL, wheel(-240), point(500, 400));
    events = takeNativeMouseEvents();
    require(events.size() == 2 && events[0].kind == NativeMouseEventKind::Wheel && events[0].wheel == 120 && events[1].wheel == -240 &&
        close(events[0].x, 0.5f) && close(events[0].y, 0.5f) && close(events[1].x, 0.25f),
        "Wheel screen coordinates convert to client coordinates while signed raw deltas remain ordered");
    require(!nativeMouseOwnsSelection() && latestNativeMousePosition().has_value(),
        "Wheel navigation preserves pointer geometry while relinquishing stationary hover selection");
    host.conversionAvailable = false;
    handleNativeMouseMessage(title, WM_MOUSEWHEEL, wheel(120), point(900, 600));
    require(takeNativeMouseEvents().empty(), "Failed screen conversion does not enqueue fabricated wheel coordinates");
    require(!latestNativeMousePosition(), "Failed current geometry invalidates the stationary hover snapshot");
    host.rect = {0, 0, 0, 0};
    handleNativeMouseMessage(title, WM_LBUTTONDOWN, 0, point(0, 0));
    require(takeNativeMouseEvents().empty() && host.captures == 0, "Zero-sized clients cannot create clicks or capture");
    host.rectAvailable = false;
    handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(20, 20));
    require(takeNativeMouseEvents().empty(), "Unavailable client geometry does not enqueue motion");
}
void verifyCaptureAndCancellation() {
    reset();
    handleNativeMouseMessage(title, WM_LBUTTONDOWN, 0, point(-1, 10));
    handleNativeMouseMessage(title, WM_RBUTTONDOWN, 0, point(800, 10));
    handleNativeMouseMessage(title, WM_MOUSEWHEEL, wheel(120), point(99, 200));
    require(takeNativeMouseEvents().empty() && host.captures == 0 && host.deviceNotifications == 0,
        "Out-of-client clicks and wheel events do not activate controls or capture");
    handleNativeMouseMessage(title, WM_RBUTTONDOWN, 0, point(100, 50));
    handleNativeMouseMessage(title, WM_MBUTTONDOWN, 0, point(100, 50));
    handleNativeMouseMessage(title, WM_RBUTTONUP, 0, point(100, 50));
    require(host.capture == title && host.captures == 1 && host.releases == 0, "Capture stays owned until every accepted button is released");
    handleNativeMouseMessage(title, WM_MBUTTONUP, 0, point(100, 50));
    auto events = takeNativeMouseEvents();
    require(events.size() == 4 && events[0].button == NativeMouseButton::Right && events[1].button == NativeMouseButton::Middle &&
        host.releases == 1, "Independent right and middle buttons preserve their ordered lifecycle");
    handleNativeMouseMessage(title, WM_LBUTTONDOWN, 0, point(100, 50));
    handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(-20, 900));
    events = takeNativeMouseEvents();
    require(events.size() == 2 && events[1].kind == NativeMouseEventKind::Move && close(events[1].x, 0) && close(events[1].y, 1),
        "Captured out-of-client movement stays within the normalized coordinate contract");
    require(!latestNativeMousePosition(), "Captured out-of-client movement cannot become a stationary edge hover");
    handleNativeMouseMessage(title, WM_LBUTTONUP, 0, point(-20, 900));
    events = takeNativeMouseEvents();
    require(events.size() == 1 && events[0].kind == NativeMouseEventKind::Cancel && host.capture == nullptr,
        "Release outside the client cancels a gesture without activating an edge control");
    handleNativeMouseMessage(title, WM_LBUTTONDOWN, 0, point(100, 50));
    handleNativeMouseMessage(title, WM_KILLFOCUS, 0, 0);
    events = takeNativeMouseEvents();
    require(events.size() == 1 && events[0].kind == NativeMouseEventKind::Cancel && host.capture == nullptr,
        "Focus loss drops queued actions and reports cancellation before releasing capture");
    require(!latestNativeMousePosition() && !nativeMouseOwnsSelection(),
        "Focus cancellation clears the pointer snapshot and its selection ownership");
    host.focused = false;
    const auto notifications = host.deviceNotifications;
    handleNativeMouseMessage(title, WM_LBUTTONDOWN, 0, point(100, 50));
    events = takeNativeMouseEvents();
    require(events.size() == 1 && events[0].kind == NativeMouseEventKind::Cancel && host.deviceNotifications == notifications &&
        host.capture == nullptr, "Background mouse messages cannot notify devices, click, or capture");
    host.focused = true;
    handleNativeMouseMessage(title, WM_LBUTTONDOWN, 0, point(100, 50));
    host.capture = foreign;
    handleNativeMouseMessage(title, WM_CAPTURECHANGED, 0, reinterpret_cast<LPARAM>(foreign));
    events = takeNativeMouseEvents();
    require(events.size() == 1 && events[0].kind == NativeMouseEventKind::Cancel && host.capture == foreign,
        "Losing capture cancels input without releasing another window's capture");
    handleNativeMouseMessage(title, WM_LBUTTONUP, 0, point(100, 50));
    require(takeNativeMouseEvents().empty(), "Capture-loss cancellation clears held-button bookkeeping");
    host.capture = nullptr; host.captureAvailable = false;
    handleNativeMouseMessage(title, WM_LBUTTONDOWN, 0, point(100, 50));
    events = takeNativeMouseEvents();
    require(events.size() == 1 && events[0].kind == NativeMouseEventKind::Cancel,
        "Failure to obtain capture cancels the gesture instead of retaining a button with no guaranteed release");
}
void verifyLeaveAndQueueBounds() {
    reset();
    handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(100, 50));
    handleNativeMouseMessage(title, WM_MOUSELEAVE, 0, 0);
    auto events = takeNativeMouseEvents();
    require(events.size() == 2 && events[1].kind == NativeMouseEventKind::Leave && host.deviceNotifications == 1,
        "Leave follows prior motion without being treated as new mouse use");
    require(!latestNativeMousePosition() && !nativeMouseOwnsSelection(),
        "Leaving the client clears the pointer snapshot and stationary hover ownership");
    handleNativeMouseMessage(title, WM_MOUSEMOVE, 0, point(100, 50));
    events = takeNativeMouseEvents();
    require(events.size() == 1 && events[0].kind == NativeMouseEventKind::Move && host.tracks == 2,
        "Re-entering at the previous position restores motion and leave tracking");
    for (int index = 0; index < 2000; ++index)
        enqueueNativeMouseEventForTests({NativeMouseEventKind::Move, NativeMouseButton::Left, float(index) / 2000, 0.5f, 0});
    events = takeNativeMouseEvents();
    require(events.size() == 1 && close(events[0].x, 1999.0f / 2000), "A large motion burst remains one latest-position event");
    enqueueNativeMouseEventForTests({NativeMouseEventKind::Move, NativeMouseButton::Left, 0.2f, 0.3f, 0});
    for (int index = 1; index <= 128; ++index)
        enqueueNativeMouseEventForTests({NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.2f, 0.3f, index});
    events = takeNativeMouseEvents();
    require(events.size() == 128 && events.front().wheel == 1 && events.back().wheel == 128,
        "Queue pressure discards motion while preserving the order of all retained actions");
    handleNativeMouseMessage(title, WM_LBUTTONDOWN, 0, point(100, 50));
    takeNativeMouseEvents();
    for (int index = 0; index < 128; ++index)
        enqueueNativeMouseEventForTests({NativeMouseEventKind::Wheel, NativeMouseButton::Left, 0.2f, 0.3f, 120});
    handleNativeMouseMessage(title, WM_LBUTTONUP, 0, point(100, 50));
    events = takeNativeMouseEvents();
    require(events.size() == 1 && events[0].kind == NativeMouseEventKind::Cancel && host.capture == nullptr,
        "Action overflow replaces the gesture with cancellation and cannot retain a stuck button");
    handleNativeMouseMessage(title, WM_LBUTTONUP, 0, point(100, 50));
    require(takeNativeMouseEvents().empty(), "Overflow cancellation resets held-button state");
    enqueueNativeMouseEventForTests({NativeMouseEventKind::ButtonDown, NativeMouseButton::Left, 0.5f, 0.5f, 0});
    resetNativeMouseInput();
    events = takeNativeMouseEvents();
    require(events.size() == 1 && events[0].kind == NativeMouseEventKind::Cancel, "Explicit reset discards pending input and leaves a single cancellation");
}
}
namespace Doritos::Native {
void notifyNativeMouseInput(bool pointerSelection) { ++host.deviceNotifications; host.pointerSelection = pointerSelection; }
bool nativeMouseOwnsSelection() { return host.pointerSelection; }
void clearNativeMouseSelection() { host.pointerSelection = false; }
namespace MouseTestHost {
bool foreground(HWND window) { return window == title && host.focused; }
bool clientRect(HWND, RECT* rect) { if (!host.rectAvailable) return false; *rect = host.rect; return true; }
bool screenToClient(HWND, POINT* point) {
    if (!host.conversionAvailable) return false;
    point->x -= host.origin.x; point->y -= host.origin.y; return true;
}
HWND captureWindow() { return host.capture; }
void setCapture(HWND window) { ++host.captures; if (host.captureAvailable) host.capture = window; }
void releaseCapture() { ++host.releases; host.capture = nullptr; }
void trackLeave(HWND) {
    ++host.tracks;
    if (host.keyboardNavigationOnTrack) {
        require(nativeMouseOwnsSelection(), "Accepted pointer ownership is already published before leave-tracking bookkeeping");
        clearNativeMouseSelection();
    }
}
}
}
int main() {
    try {
        verifyMotionAndActionOrder(); verifyCoordinatesAndWheel(); verifyCaptureAndCancellation(); verifyLeaveAndQueueBounds();
        verifySelectionPublicationOrder();
        std::printf("Native mouse queue contracts passed: %zu checks; no OS pointer or window input used\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native mouse queue contract failed: %s\n", error.what()); return 1;
    }
}
