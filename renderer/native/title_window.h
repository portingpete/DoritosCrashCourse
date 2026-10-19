#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace Doritos::Graphics {
using TitleWindowMessageHook = bool (*)(HWND, UINT, WPARAM, LPARAM);
class TitleWindow {
    std::thread thread_;
    std::atomic<HWND> window_{nullptr};
    std::atomic<bool> internalClose_{false};
    std::mutex revealMutex_;
    bool startupRevealQueued_ = false, gameRevealQueued_ = false;
    HANDLE closeRequested_ = nullptr;
    TitleWindowMessageHook messageHook_;
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
public:
    explicit TitleWindow(HANDLE closeRequested = nullptr, TitleWindowMessageHook messageHook = nullptr);
    ~TitleWindow();
    HWND handle() const { return window_.load(); }
    void revealStartup();
    void reveal();
};
}
