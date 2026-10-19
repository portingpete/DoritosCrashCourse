#include "title_window.h"
#include "video_settings_ui.h"
#include <future>
#include <stdexcept>

namespace Doritos::Graphics {
LRESULT CALLBACK TitleWindow::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    auto* owner = reinterpret_cast<TitleWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (owner && owner->messageHook_ && owner->messageHook_(window, message, wparam, lparam) && message!=WM_CLOSE && message!=WM_DESTROY) return 0;
    if (message == WM_CLOSE || message == WM_DESTROY) {
        if (owner && !owner->internalClose_.load() && owner->closeRequested_) SetEvent(owner->closeRequested_);
        if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
        if (owner) owner->window_.store(nullptr);
        PostQuitMessage(0); return 0;
    }
    if (message == WM_APP + 1) {
        SetWindowTextW(window, wparam ? L"Doritos Crash Course" : L"Doritos Crash Course - native renderer starting");
        ShowWindow(window, SW_SHOW);
        if (!wparam) SetForegroundWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
TitleWindow::TitleWindow(HANDLE closeRequested, TitleWindowMessageHook messageHook) : messageHook_(messageHook) {
    if (closeRequested && !DuplicateHandle(GetCurrentProcess(), closeRequested, GetCurrentProcess(),
        &closeRequested_, 0, FALSE, DUPLICATE_SAME_ACCESS))
        throw std::runtime_error("Cannot retain native title close event");
    std::promise<HWND> ready;
    auto result = ready.get_future();
    try {
        thread_ = std::thread([this, ready = std::move(ready)]() mutable {
            try {
                const auto instance = GetModuleHandleW(nullptr);
                WNDCLASSW type{}; type.lpfnWndProc = procedure; type.hInstance = instance;
                type.lpszClassName = L"DoritosNativeTitleWindow";
                type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
                if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                    throw std::runtime_error("Cannot register native title window");
                RECT rectangle{0, 0, 2560, 1440};
                if (!AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE))
                    throw std::runtime_error("Cannot size native title window client");
                HWND window = CreateWindowW(type.lpszClassName, L"Doritos Crash Course", WS_OVERLAPPEDWINDOW,
                    CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
                    nullptr, nullptr, instance, this);
                if (!window) throw std::runtime_error("Cannot create native title window");
                window_.store(window); ready.set_value(window);
            } catch (...) { ready.set_exception(std::current_exception()); return; }
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if(translateNativeVideoSettingsMessage(message)) continue;
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        });
        result.get();
    } catch (...) {
        if (thread_.joinable()) thread_.join();
        if (closeRequested_) CloseHandle(closeRequested_);
        throw;
    }
}
TitleWindow::~TitleWindow() {
    // Internal service teardown has already disarmed its coordinator; also
    // suppress notification here so destroying a test/failed-startup window
    // cannot be mistaken for a user's request to terminate the process.
    internalClose_.store(true);
    if (HWND window = window_.load()) PostMessageW(window, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
    if (closeRequested_) CloseHandle(closeRequested_);
}
void TitleWindow::revealStartup() {
    std::lock_guard lock(revealMutex_);
    if (startupRevealQueued_ || gameRevealQueued_) return;
    const HWND window = window_.load();
    if (window) startupRevealQueued_ = PostMessageW(window, WM_APP + 1, 0, 0) != FALSE;
}
void TitleWindow::reveal() {
    std::lock_guard lock(revealMutex_);
    if (gameRevealQueued_) return;
    // The bridge calls this after every drawn presentation. Queue the actual
    // visibility/title transition once per window, rather than repeatedly
    // showing a window which the user may since have minimized or hidden.
    // Serialize with startup so a late startup request cannot follow game.
    const HWND window = window_.load();
    if (window) gameRevealQueued_ = PostMessageW(window, WM_APP + 1, 1, 0) != FALSE;
}
}
