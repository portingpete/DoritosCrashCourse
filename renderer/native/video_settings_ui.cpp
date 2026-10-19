#include "video_settings_ui.h"
#include "video_presenter.h"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Doritos::Graphics {
namespace {
using Microsoft::WRL::ComPtr;
constexpr float canvasWidth = 1280, canvasHeight = 720;
constexpr UINT releaseCaptureMessage = WM_APP + 0x452;
constexpr std::array<uint32_t, 3> rowCounts{7, 5, 6};
constexpr std::array<const wchar_t*, 3> pageNames{L"DISPLAY", L"PERFORMANCE", L"IMAGE"};
constexpr std::array<const wchar_t*, 5> buttonNames{L"APPLY", L"KEEP CHANGES", L"REVERT", L"DEFAULTS", L"BACK"};
constexpr COLORREF background = RGB(7, 23, 40), panel = RGB(12, 38, 59);
constexpr COLORREF selectedPanel = RGB(18, 83, 108), white = RGB(242, 248, 252);
constexpr COLORREF secondary = RGB(162, 195, 215), cyan = RGB(63, 230, 241);
constexpr COLORREF disabled = RGB(83, 112, 130), yellow = RGB(255, 222, 94), red = RGB(255, 147, 137);
struct Resolution { uint32_t width, height; bool operator==(const Resolution&) const = default; };
struct Menu {
    bool open = false, wasPreview = false, controllerInitialized = false, pointerInitialized = false;
    float pointerX = 0, pointerY = 0;
    uint32_t page = 0, selection = 0, controllerHeld = 0;
    uint64_t repeatAt = 0;
    int pressedItem = -1;
    uint32_t pressedPage = 0, outputWidth = 1280, outputHeight = 720;
    VideoContentRect contentRect{0, 0, 1280, 720};
    VideoSettings draft, lastCommitted;
    VideoSettingsStatus status;
    std::string localError;
    std::vector<Resolution> resolutions;
    std::vector<std::wstring> monitors;
    std::array<bool, 256> suppressedKeys{};
};
Menu menu;
std::mutex menuMutex;
std::atomic<bool> openFlag{false};
std::atomic<HWND> ownerWindow{nullptr};
std::atomic<void (*)(bool)> focusCallback{nullptr};
uint64_t lifecycleSerial = 0; // Protected by menuMutex.

VideoContentRect menuContentRect(uint32_t width, uint32_t height) {
    auto result = videoContentRect(width, height, 1280, 720, ScalingMode::PreserveAspect);
    RECT client{};
    if (auto owner = ownerWindow.load(); owner && GetClientRect(owner, &client) && client.right > 0 && client.bottom > 0) {
        auto visible = videoContentRect(client.right, client.bottom, 1280, 720, ScalingMode::PreserveAspect);
        result = {LONG(std::lround(double(visible.x) * width / client.right)), LONG(std::lround(double(visible.y) * height / client.bottom)),
            LONG(std::lround(double(visible.width) * width / client.right)), LONG(std::lround(double(visible.height) * height / client.bottom))};
    }
    return result;
}

void releaseRenderCache();
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return std::wstring(text.begin(), text.end());
    std::wstring result(static_cast<size_t>(count), L' ');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
void addResolution(uint32_t width, uint32_t height) {
    Resolution value{width, height};
    if (width && height && std::find(menu.resolutions.begin(), menu.resolutions.end(), value) == menu.resolutions.end())
        menu.resolutions.push_back(value);
}
BOOL CALLBACK enumerateMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    auto width = static_cast<uint32_t>(info.rcMonitor.right - info.rcMonitor.left);
    auto height = static_cast<uint32_t>(info.rcMonitor.bottom - info.rcMonitor.top);
    menu.monitors.push_back(L"Display " + std::to_wstring(menu.monitors.size() + 1) + L" (" +
        std::to_wstring(width) + L" x " + std::to_wstring(height) + L")");
    addResolution(width, height);
    return TRUE;
}
void refreshLocked() {
    tickVideoSettings();
    auto committed = committedVideoSettings();
    auto status = videoSettingsStatus();
    const bool preview = status.previewPending || status.previewActive;
    if (committed != menu.lastCommitted || (menu.wasPreview && !preview)) {
        menu.draft = committed;
        menu.lastCommitted = committed;
        addResolution(committed.width, committed.height);
    }
    menu.wasPreview = preview;
    menu.status = std::move(status);
    if (!menu.localError.empty()) menu.status.error = menu.localError;
}
bool previewLocked() { return menu.status.previewPending || menu.status.previewActive; }
bool buttonEnabledLocked(uint32_t button) {
    switch (button) {
    case 0: return !previewLocked() && menu.draft != menu.lastCommitted;
    case 1: return menu.status.previewActive;
    case 2: return previewLocked() || menu.draft != menu.lastCommitted;
    case 3: return !previewLocked();
    default: return true;
    }
}
template<class T, size_t N> void cycle(T& value, const std::array<T, N>& choices, int direction) {
    auto found = std::find(choices.begin(), choices.end(), value);
    int index = found == choices.end() ? (direction > 0 ? -1 : 0) : static_cast<int>(found - choices.begin());
    index = (index + direction + static_cast<int>(N)) % static_cast<int>(N);
    value = choices[static_cast<size_t>(index)];
}
void adjustFloat(float& value, float step, float minimum, float maximum, int direction) {
    value = std::clamp(std::round((value + step * direction) / step) * step, minimum, maximum);
}
void adjustRowLocked(uint32_t row, int direction) {
    if (previewLocked()) return;
    menu.localError.clear();
    auto& s = menu.draft;
    if (menu.page == 0) {
        switch (row) {
        case 0: {
            if (menu.resolutions.empty()) break;
            auto found = std::find(menu.resolutions.begin(), menu.resolutions.end(), Resolution{s.width, s.height});
            int count = static_cast<int>(menu.resolutions.size());
            int index = found == menu.resolutions.end() ? 0 : static_cast<int>(found - menu.resolutions.begin());
            const auto& next = menu.resolutions[static_cast<size_t>((index + direction + count) % count)];
            s.width = next.width; s.height = next.height;
            break;
        }
        case 1: s.windowMode = s.windowMode == WindowMode::Windowed ? WindowMode::Borderless : WindowMode::Windowed; break;
        case 2: if (!menu.monitors.empty()) s.monitor = (s.monitor + static_cast<uint32_t>(menu.monitors.size()) + direction) % static_cast<uint32_t>(menu.monitors.size()); break;
        case 3: s.scaling = s.scaling == ScalingMode::PreserveAspect ? ScalingMode::Stretch : ScalingMode::PreserveAspect; break;
        case 4: cycle(s.vsync, std::array{VSyncMode::Automatic, VSyncMode::Off, VSyncMode::On}, direction); break;
        case 5: cycle(s.frameLimit, std::array<uint32_t, 9>{0, 30, 60, 90, 120, 144, 165, 240, 360}, direction); break;
        case 6: cycle(s.backgroundFrameLimit, std::array<uint32_t, 4>{0, 15, 30, 60}, direction); break;
        }
    } else if (menu.page == 1) {
        switch (row) {
        case 0: cycle(s.filtering, std::array{TextureFiltering::Original, TextureFiltering::Bilinear, TextureFiltering::Trilinear}, direction); break;
        case 1: cycle(s.anisotropy, std::array<uint32_t, 5>{0, 2, 4, 8, 16}, direction); break;
        case 2: adjustFloat(s.mipBias, .05f, -1, 2, direction); break;
        case 3: s.lowLatency = !s.lowLatency; break;
        case 4: s.showFps = !s.showFps; break;
        }
    } else {
        switch (row) {
        case 0: s.fxaa = !s.fxaa; break;
        case 1: adjustFloat(s.sharpness, .05f, 0, 1, direction); break;
        case 2: adjustFloat(s.brightness, .01f, -.25f, .25f, direction); break;
        case 3: adjustFloat(s.contrast, .05f, .5f, 1.5f, direction); break;
        case 4: adjustFloat(s.gamma, .05f, .5f, 2, direction); break;
        case 5: adjustFloat(s.saturation, .1f, 0, 2, direction); break;
        }
    }
}
bool closeLocked() {
    if (!menu.open) return false;
    cancelVideoSettingsPreview();
    menu.open = false; menu.pressedItem = -1; menu.controllerInitialized = false;
    ++lifecycleSerial;
    openFlag.store(false, std::memory_order_release);
    if (auto owner = ownerWindow.load()) PostMessageW(owner, releaseCaptureMessage, static_cast<WPARAM>(lifecycleSerial), 0);
    // The owner callback only changes guest-input suspension. Serialize it
    // with opening so an old close cannot unsuspend a newly opened menu.
    if (auto callback = focusCallback.load()) callback(false);
    return true;
}
void didClose(bool closed) {
    if (!closed) return;
    if (GetCapture() == ownerWindow.load()) ReleaseCapture();
    releaseRenderCache();
}
bool activateLocked(uint32_t index) {
    uint32_t rows = rowCounts[menu.page];
    if (index < rows) { adjustRowLocked(index, 1); return false; }
    uint32_t button = index - rows;
    if (!buttonEnabledLocked(button)) return false;
    menu.localError.clear();
    switch (button) {
    case 0:
        if (requestVideoSettingsPreview(menu.draft)) {
            menu.wasPreview = true;
            menu.selection = rows + 1;
        }
        break;
    case 1: confirmVideoSettingsPreview(); break;
    case 2: cancelVideoSettingsPreview(); menu.draft = committedVideoSettings(); menu.wasPreview = false; break;
    case 3: menu.draft = VideoSettings{}; addResolution(menu.draft.width, menu.draft.height); break;
    case 4: return closeLocked();
    }
    refreshLocked();
    return false;
}
bool actionLocked(NativeVideoSettingsAction action) {
    if (!menu.open) return false;
    refreshLocked();
    uint32_t rows = rowCounts[menu.page], count = rows + 5;
    switch (action) {
    case NativeVideoSettingsAction::Up: menu.selection = (menu.selection + count - 1) % count; break;
    case NativeVideoSettingsAction::Down: menu.selection = (menu.selection + 1) % count; break;
    case NativeVideoSettingsAction::Left:
    case NativeVideoSettingsAction::Right: {
        int direction = action == NativeVideoSettingsAction::Right ? 1 : -1;
        if (menu.selection < rows) adjustRowLocked(menu.selection, direction);
        else menu.selection = rows + (menu.selection - rows + 5 + direction) % 5;
        break;
    }
    case NativeVideoSettingsAction::Select: return activateLocked(menu.selection);
    case NativeVideoSettingsAction::Back: return closeLocked();
    case NativeVideoSettingsAction::NextPage:
    case NativeVideoSettingsAction::PreviousPage:
        menu.page = (menu.page + (action == NativeVideoSettingsAction::NextPage ? 1 : 2)) % 3;
        menu.selection = 0; menu.pressedItem = -1;
        break;
    }
    return false;
}
int hitLocked(float normalizedX, float normalizedY) {
    const auto& rect = menu.contentRect;
    float pixelX = normalizedX * menu.outputWidth, pixelY = normalizedY * menu.outputHeight;
    if (rect.width <= 0 || rect.height <= 0 || pixelX < rect.x || pixelY < rect.y || pixelX >= rect.x + rect.width || pixelY >= rect.y + rect.height) return -1;
    float x = (pixelX - rect.x) * canvasWidth / rect.width;
    float y = (pixelY - rect.y) * canvasHeight / rect.height;
    if (y >= 110 && y < 154) for (uint32_t page = 0; page < 3; ++page)
        if (x >= 48 + 396 * page && x < 432 + 396 * page) return 100 + static_cast<int>(page);
    uint32_t rows = rowCounts[menu.page];
    if (x >= 48 && x < 1232) for (uint32_t row = 0; row < rows; ++row)
        if (y >= 178 + 44 * row && y < 218 + 44 * row) {
            if (x >= 678 && x < 724) return 200 + static_cast<int>(row);
            if (x >= 1180) return 300 + static_cast<int>(row);
            return static_cast<int>(row);
        }
    if (y >= 614 && y < 658) for (uint32_t button = 0; button < 5; ++button)
        if (x >= 48 + 240 * button && x < 272 + 240 * button) return static_cast<int>(rows + button);
    return -1;
}
std::wstring number(float value) {
    wchar_t buffer[40]; swprintf_s(buffer, L"%.2f", value);
    return buffer;
}
std::wstring rowValue(uint32_t page, uint32_t row, const VideoSettings& s, const std::vector<std::wstring>& monitors) {
    if (page == 0) switch (row) {
    case 0: return std::to_wstring(s.width) + L" x " + std::to_wstring(s.height);
    case 1: return s.windowMode == WindowMode::Windowed ? L"Windowed" : L"Borderless";
    case 2: return s.monitor < monitors.size() ? monitors[s.monitor] : L"Display " + std::to_wstring(s.monitor + 1);
    case 3: return s.scaling == ScalingMode::PreserveAspect ? L"Preserve aspect ratio" : L"Stretch to fill";
    case 4: return s.vsync == VSyncMode::Automatic ? L"Automatic" : s.vsync == VSyncMode::On ? L"On" : L"Off";
    case 5: return s.frameLimit ? std::to_wstring(s.frameLimit) + L" FPS" : L"Unlimited";
    case 6: return s.backgroundFrameLimit ? std::to_wstring(s.backgroundFrameLimit) + L" FPS" : L"Unlimited";
    }
    if (page == 1) switch (row) {
    case 0: return s.filtering == TextureFiltering::Original ? L"Original" : s.filtering == TextureFiltering::Bilinear ? L"Bilinear" : L"Trilinear";
    case 1: return s.anisotropy ? std::to_wstring(s.anisotropy) + L"x" : L"Original";
    case 2: return number(s.mipBias);
    case 3: return s.lowLatency ? L"On" : L"Off";
    case 4: return s.showFps ? L"On" : L"Off";
    }
    switch (row) {
    case 0: return s.fxaa ? L"On" : L"Off";
    case 1: return number(s.sharpness);
    case 2: return number(s.brightness);
    case 3: return number(s.contrast);
    case 4: return number(s.gamma);
    default: return number(s.saturation);
    }
}
const wchar_t* rowName(uint32_t page, uint32_t row) {
    static constexpr std::array<const wchar_t*, 7> display{L"Render resolution", L"Window mode", L"Monitor", L"Scaling", L"V-sync", L"Frame-rate limit", L"Background frame limit"};
    static constexpr std::array<const wchar_t*, 5> performance{L"Texture filtering", L"Anisotropic filtering", L"Texture mip bias", L"Low-latency presentation", L"FPS overlay"};
    static constexpr std::array<const wchar_t*, 6> image{L"FXAA", L"Sharpness", L"Brightness", L"Contrast", L"Gamma", L"Saturation"};
    return page == 0 ? display[row] : page == 1 ? performance[row] : image[row];
}
const wchar_t* description(uint32_t page, uint32_t selection) {
    if (selection >= rowCounts[page]) switch (selection - rowCounts[page]) {
    case 0: return L"Preview your settings. Keep Changes saves them after the renderer has applied them successfully.";
    case 1: return L"Save the preview. If you do not confirm within 15 seconds, the previous settings return automatically.";
    case 2: return L"Restore the last confirmed settings and discard edits.";
    case 3: return L"Load the original PC defaults into this menu. Choose Apply to preview them.";
    default: return L"Return to the game. Any unconfirmed preview will be reverted.";
    }
    if (page == 0) switch (selection) {
    case 0: return L"Sets scene and presentation resolution. Borderless scales to your desktop. Higher resolutions use more GPU memory.";
    case 1: return L"Windowed uses a movable window. Borderless fills the selected display without changing its desktop mode.";
    case 2: return L"Choose the display used for the game window.";
    case 3: return L"Preserve aspect ratio adds bars when needed. Stretch fills the window.";
    case 4: return L"Automatic enables V-sync on displays at 60 Hz or above. On reduces tearing; Off allows immediate presentation.";
    case 5: return L"Limit game frames per second. Unlimited can use more power; V-sync may impose a lower limit.";
    default: return L"Limit presentation while the game is not the foreground window.";
    }
    if (page == 1) switch (selection) {
    case 0: return L"Original keeps the game's sampler settings. Bilinear and Trilinear smooth supported scene textures.";
    case 1: return L"Improve supported scene textures viewed at an angle. Original keeps the game's settings.";
    case 2: return L"Negative values sharpen distant mipmapped textures; positive values soften them and can reduce shimmer.";
    case 3: return L"Limit the queued presentation frames to reduce input latency. This can affect frame pacing.";
    default: return L"Show a small frame-rate counter during play.";
    }
    switch (selection) {
    case 0: return L"Fast approximate anti-aliasing smooths visible edges in the final game image.";
    case 1: return L"Apply subtle final-image sharpening. Zero disables sharpening.";
    case 2: return L"Adjust final-image brightness. Zero keeps the original image.";
    case 3: return L"Adjust the difference between dark and bright areas. One keeps the original image.";
    case 4: return L"Adjust midtone brightness. One keeps the original image.";
    default: return L"Adjust color intensity. Zero is grayscale; one keeps the original colors.";
    }
}

struct Frame {
    uint32_t page, selection, rows;
    VideoSettings settings;
    VideoSettingsStatus status;
    std::vector<std::wstring> monitors;
    std::array<bool, 5> enabled;
    bool dirty;
};
struct RenderCache {
    ComPtr<IDirect3DDevice9Ex> device;
    ComPtr<IDirect3DTexture9> texture;
    uint32_t width = 0, height = 0;
    std::wstring key;
};
std::mutex renderMutex;
std::unique_ptr<RenderCache> renderCache;
void releaseRenderCache() { std::lock_guard lock(renderMutex); renderCache.reset(); }
void require(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(static_cast<uint32_t>(result)) + ")");
}
class BitmapCanvas {
public:
    HDC dc = nullptr; HBITMAP bitmap = nullptr; HGDIOBJ oldBitmap = nullptr;
    uint32_t* pixels = nullptr; float scale;
    std::vector<HFONT> fonts;
    BitmapCanvas(uint32_t width, uint32_t height) : scale(width / canvasWidth) {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width); info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        if (!dc || !bitmap || !pixels) {
            if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc);
            throw std::runtime_error("Cannot allocate video-menu text image");
        }
        oldBitmap = SelectObject(dc, bitmap);
        SetBkMode(dc, TRANSPARENT);
    }
    ~BitmapCanvas() {
        if (dc) { SelectObject(dc, oldBitmap); DeleteDC(dc); }
        for (auto font : fonts) DeleteObject(font);
        if (bitmap) DeleteObject(bitmap);
    }
    RECT rectangle(float x, float y, float w, float h) const {
        return {static_cast<LONG>(std::lround(x * scale)), static_cast<LONG>(std::lround(y * scale)),
            static_cast<LONG>(std::lround((x + w) * scale)), static_cast<LONG>(std::lround((y + h) * scale))};
    }
    void fill(float x, float y, float w, float h, COLORREF color) {
        RECT rect = rectangle(x, y, w, h);
        HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &rect, brush); DeleteObject(brush);
    }
    void text(const std::wstring& value, float x, float y, float w, float h, int size, COLORREF color, bool bold = false, UINT alignment = DT_LEFT, bool wrap = false) {
        HFONT font = CreateFontW(-static_cast<int>(std::lround(size * scale)), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font) throw std::runtime_error("Cannot allocate video-menu font");
        fonts.push_back(font);
        auto old = SelectObject(dc, font); SetTextColor(dc, color);
        RECT rect = rectangle(x, y, w, h);
        DrawTextW(dc, value.data(), static_cast<int>(value.size()), &rect,
            alignment | DT_NOPREFIX | (wrap ? DT_WORDBREAK : DT_SINGLELINE | DT_VCENTER));
        SelectObject(dc, old);
    }
};
std::wstring statusText(const Frame& frame) {
    if (!frame.status.error.empty()) return L"Could not apply settings: " + widen(frame.status.error);
    if (frame.status.previewPending) return L"Applying preview... The confirmation timer starts when the renderer is ready.";
    if (frame.status.previewActive) return L"Keep these settings? Reverting in " + std::to_wstring(frame.status.secondsRemaining) + L" seconds.";
    return frame.dirty ? L"You have changes. Choose Apply to preview them." : L"These settings are saved. Changes are previewed before they are saved.";
}
std::wstring imageKey(const Frame& frame, uint32_t width, uint32_t height) {
    std::wstring result = std::to_wstring(width) + L":" + std::to_wstring(height) + L":" +
        std::to_wstring(frame.page) + L":" + std::to_wstring(frame.selection) + L":" + statusText(frame);
    for (uint32_t row = 0; row < frame.rows; ++row) result += L"|" + rowValue(frame.page, row, frame.settings, frame.monitors);
    for (bool enabled : frame.enabled) result += enabled ? L"1" : L"0";
    return result;
}
void updateImage(RenderCache& cache, const Frame& frame, uint32_t width, uint32_t height) {
    auto key = imageKey(frame, width, height);
    if (cache.texture && cache.key == key) return;
    BitmapCanvas canvas(width, height);
    canvas.fill(0, 0, canvasWidth, canvasHeight, background);
    canvas.fill(0, 0, canvasWidth, 6, cyan);
    canvas.text(L"DORITOS CRASH COURSE", 48, 19, 900, 27, 20, cyan, true);
    canvas.text(L"VIDEO SETTINGS", 46, 47, 960, 53, 42, white, true);
    canvas.text(L"ESC / B  BACK", 986, 57, 246, 34, 20, secondary, true, DT_RIGHT);
    for (uint32_t page = 0; page < 3; ++page) {
        float x = 48 + 396.f * page;
        canvas.fill(x, 110, 384, 44, frame.page == page ? selectedPanel : panel);
        if (frame.page == page) canvas.fill(x, 150, 384, 4, cyan);
        canvas.text(pageNames[page], x, 110, 384, 40, 23, frame.page == page ? white : secondary, true, DT_CENTER);
    }
    const bool editing = !frame.status.previewPending && !frame.status.previewActive;
    for (uint32_t row = 0; row < frame.rows; ++row) {
        float y = 178 + 44.f * row;
        bool selected = frame.selection == row;
        canvas.fill(48, y, 1184, 40, selected ? selectedPanel : panel);
        if (selected) canvas.fill(48, y, 4, 40, cyan);
        canvas.text(rowName(frame.page, row), 66, y, 600, 40, 26, editing ? white : secondary);
        auto value = rowValue(frame.page, row, frame.settings, frame.monitors);
        canvas.text(value, 728, y, 444, 40, 25, editing ? cyan : secondary, false, DT_RIGHT);
        if (selected && editing) {
            canvas.text(L"<", 678, y, 46, 40, 26, cyan, true, DT_CENTER);
            canvas.text(L">", 1180, y, 52, 40, 26, cyan, true, DT_CENTER);
        }
    }
    canvas.text(description(frame.page, frame.selection), 56, 497, 1168, 56, 20, secondary, false, DT_LEFT, true);
    canvas.text(statusText(frame), 56, 559, 1168, 46, 20,
        !frame.status.error.empty() ? red : frame.status.previewActive ? yellow : cyan, false, DT_LEFT, true);
    for (uint32_t button = 0; button < 5; ++button) {
        float x = 48 + 240.f * button;
        bool selected = frame.selection == frame.rows + button;
        canvas.fill(x, 614, 224, 44, selected ? selectedPanel : panel);
        if (selected) { canvas.fill(x, 614, 224, 2, cyan); canvas.fill(x, 656, 224, 2, cyan); }
        canvas.text(buttonNames[button], x, 614, 224, 44, 22, frame.enabled[button] ? white : disabled, true, DT_CENTER);
    }
    canvas.text(L"ARROWS / D-PAD  CHOOSE & ADJUST      ENTER / A  SELECT      Q,E / LB,RB  TABS", 48, 679, 1184, 27, 18, secondary, false, DT_CENTER);
    GdiFlush();
    if (!cache.texture || cache.width != width || cache.height != height) {
        ComPtr<IDirect3DTexture9> texture;
        require(cache.device->CreateTexture(width, height, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr), "Create video-menu image");
        cache.texture = std::move(texture); cache.width = width; cache.height = height;
    }
    D3DLOCKED_RECT locked{};
    require(cache.texture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD), "Lock video-menu image");
    for (uint32_t y = 0; y < height; ++y) {
        auto destination = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch);
        auto source = canvas.pixels + y * width;
        for (uint32_t x = 0; x < width; ++x) destination[x] = source[x] | 0xff000000u;
    }
    require(cache.texture->UnlockRect(0), "Unlock video-menu image");
    cache.key = std::move(key);
}
class SavedState {
    IDirect3DDevice9Ex* device;
    ComPtr<IDirect3DStateBlock9> state;
    std::array<ComPtr<IDirect3DSurface9>, 4> targets;
    ComPtr<IDirect3DSurface9> depth;
    uint32_t targetCount = 1;
public:
    explicit SavedState(IDirect3DDevice9Ex* value) : device(value) {
        require(device->CreateStateBlock(D3DSBT_ALL, &state), "Capture video-menu render state");
        D3DCAPS9 caps{}; if (SUCCEEDED(device->GetDeviceCaps(&caps))) targetCount = std::clamp<DWORD>(caps.NumSimultaneousRTs, 1, 4);
        for (uint32_t i = 0; i < targetCount; ++i) device->GetRenderTarget(i, &targets[i]);
        device->GetDepthStencilSurface(&depth);
    }
    ~SavedState() {
        device->SetDepthStencilSurface(nullptr);
        for (uint32_t i = 1; i < targetCount; ++i) device->SetRenderTarget(i, nullptr);
        if (targets[0]) device->SetRenderTarget(0, targets[0].Get());
        for (uint32_t i = 1; i < targetCount; ++i) device->SetRenderTarget(i, targets[i].Get());
        device->SetDepthStencilSurface(depth.Get());
        if (state) state->Apply();
    }
    uint32_t count() const { return targetCount; }
};
} // namespace

void registerNativeVideoSettingsOwner(HWND owner, void (*callback)(bool), bool) {
    bool closed = false;
    {
        std::lock_guard lock(menuMutex);
        if (ownerWindow.load() != owner) closed = closeLocked();
        ownerWindow.store(owner); focusCallback.store(callback);
    }
    didClose(closed);
}
void showNativeVideoSettings() {
    {
        std::lock_guard lock(menuMutex);
        if (menu.open) return;
        menu = Menu{}; menu.open = true; ++lifecycleSerial;
        menu.contentRect = menuContentRect(menu.outputWidth, menu.outputHeight);
        menu.draft = menu.lastCommitted = committedVideoSettings();
        for (const auto resolution : std::array<Resolution, 9>{{{1280, 720}, {1600, 900}, {1920, 1080}, {1920, 1200},
            {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}, {5120, 1440}}}) addResolution(resolution.width, resolution.height);
        EnumDisplayMonitors(nullptr, nullptr, enumerateMonitor, 0);
        if (menu.monitors.empty()) menu.monitors.push_back(L"Display 1");
        addResolution(menu.draft.width, menu.draft.height);
        for (uint32_t key = 0; key < menu.suppressedKeys.size(); ++key) menu.suppressedKeys[key] = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        refreshLocked();
        // Suspend title input before any rendering thread sees the open menu.
        if (auto callback = focusCallback.load()) callback(true);
        openFlag.store(true, std::memory_order_release);
    }
}
bool nativeVideoSettingsOpen() { return openFlag.load(std::memory_order_acquire); }
void sendNativeVideoSettingsAction(NativeVideoSettingsAction action) {
    bool closed;
    { std::lock_guard lock(menuMutex); closed = actionLocked(action); }
    didClose(closed);
}
void updateNativeVideoSettingsController(uint32_t heldButtons) {
    bool closed = false;
    {
        std::lock_guard lock(menuMutex);
        if (!menu.open) return;
        auto now = GetTickCount64();
        if (!menu.controllerInitialized) {
            menu.controllerInitialized = true; menu.controllerHeld = heldButtons; menu.repeatAt = now + 400;
            return;
        }
        uint32_t edges = heldButtons & ~menu.controllerHeld;
        constexpr uint32_t directions = VideoUiUp | VideoUiDown | VideoUiLeft | VideoUiRight;
        if ((heldButtons & directions) != (menu.controllerHeld & directions)) menu.repeatAt = now + 400;
        else if ((heldButtons & directions) && now >= menu.repeatAt) { edges |= heldButtons & directions; menu.repeatAt = now + 85; }
        menu.controllerHeld = heldButtons;
        constexpr std::array<NativeVideoSettingsAction, 8> actions{NativeVideoSettingsAction::Up, NativeVideoSettingsAction::Down,
            NativeVideoSettingsAction::Left, NativeVideoSettingsAction::Right, NativeVideoSettingsAction::Select, NativeVideoSettingsAction::Back,
            NativeVideoSettingsAction::NextPage, NativeVideoSettingsAction::PreviousPage};
        for (uint32_t bit = 0; bit < actions.size(); ++bit) if (edges & (1u << bit)) {
            closed = actionLocked(actions[bit]); if (closed) break;
        }
    }
    didClose(closed);
}
void updateNativeVideoSettingsPointer(float x, float y, bool moved, bool pressed, bool released, int wheelSteps) {
    bool closed = false;
    {
        std::lock_guard lock(menuMutex);
        if (!menu.open) return;
        refreshLocked();
        int item = hitLocked(x, y);
        int rowItem = item >= 300 ? item - 300 : item >= 200 ? item - 200 : item;
        bool actualMotion = moved && menu.pointerInitialized && (menu.pointerX != x || menu.pointerY != y);
        if (moved || pressed || released) { menu.pointerX = x; menu.pointerY = y; menu.pointerInitialized = true; }
        if (actualMotion && rowItem >= 0 && rowItem < 100) menu.selection = static_cast<uint32_t>(rowItem);
        if (pressed) { menu.pressedItem = item; menu.pressedPage = menu.page; }
        if (released) {
            bool match = item >= 0 && item == menu.pressedItem && menu.page == menu.pressedPage;
            menu.pressedItem = -1;
            if (match) {
                if (item >= 200) {
                    menu.selection = static_cast<uint32_t>(rowItem);
                    adjustRowLocked(menu.selection, item >= 300 ? 1 : -1);
                } else if (item >= 100) { menu.page = static_cast<uint32_t>(item - 100); menu.selection = 0; }
                else { menu.selection = static_cast<uint32_t>(item); closed = activateLocked(menu.selection); }
            }
        }
        if (wheelSteps && rowItem >= 0 && rowItem < static_cast<int>(rowCounts[menu.page])) {
            menu.selection = static_cast<uint32_t>(rowItem);
            for (int i = 0; i < std::min(std::abs(wheelSteps), 16); ++i) adjustRowLocked(menu.selection, wheelSteps > 0 ? 1 : -1);
        }
    }
    didClose(closed);
}
NativeVideoSettingsSnapshot nativeVideoSettingsSnapshot() {
    std::lock_guard lock(menuMutex);
    if (menu.open) refreshLocked();
    return {menu.open, menu.page, menu.selection, rowCounts[menu.page], menu.draft, menu.status};
}
bool translateNativeVideoSettingsMessage(const MSG&) { return false; }
bool handleNativeVideoSettingsMessage(HWND owner, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == releaseCaptureMessage) {
        bool release = false;
        {
            std::lock_guard lock(menuMutex);
            release = owner == ownerWindow.load() && !menu.open && wparam == static_cast<WPARAM>(lifecycleSerial);
        }
        // Capture belongs to the title's window thread, whereas a controller
        // may close the menu on the rendering thread. Never release a newer
        // menu's gesture when an old posted cleanup finally arrives.
        if (release && GetCapture() == owner) ReleaseCapture();
        return true;
    }
    if (message == WM_DESTROY || message == WM_NCDESTROY) {
        if (owner == ownerWindow.load()) {
            sendNativeVideoSettingsAction(NativeVideoSettingsAction::Back);
            ownerWindow.store(nullptr); focusCallback.store(nullptr);
        }
        return false;
    }
    if (!nativeVideoSettingsOpen()) return false;
    if (message == WM_ACTIVATEAPP || message == WM_KILLFOCUS || message == WM_CANCELMODE || message == WM_CAPTURECHANGED) {
        std::lock_guard lock(menuMutex);
        menu.pressedItem = -1;
        if (message != WM_CAPTURECHANGED) menu.pointerInitialized = false;
        if (message != WM_CAPTURECHANGED) menu.controllerInitialized = false;
        if (message == WM_ACTIVATEAPP && wparam) for (uint32_t key = 0; key < 256; ++key)
            menu.suppressedKeys[key] = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        return false;
    }
    if (message == WM_KEYUP || message == WM_SYSKEYUP) {
        if (wparam < 256) { std::lock_guard lock(menuMutex); menu.suppressedKeys[wparam] = false; }
        return true;
    }
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        if (message == WM_SYSKEYDOWN && wparam == VK_F4) return false;
        { std::lock_guard lock(menuMutex); if (wparam < 256 && menu.suppressedKeys[wparam]) return true; }
        std::optional<NativeVideoSettingsAction> action;
        switch (wparam) {
        case VK_UP: action = NativeVideoSettingsAction::Up; break;
        case VK_DOWN: action = NativeVideoSettingsAction::Down; break;
        case VK_LEFT: action = NativeVideoSettingsAction::Left; break;
        case VK_RIGHT: action = NativeVideoSettingsAction::Right; break;
        case VK_RETURN: case VK_SPACE: action = NativeVideoSettingsAction::Select; break;
        case VK_ESCAPE: action = NativeVideoSettingsAction::Back; break;
        case 'Q': case VK_PRIOR: action = NativeVideoSettingsAction::PreviousPage; break;
        case 'E': case VK_NEXT: action = NativeVideoSettingsAction::NextPage; break;
        case VK_TAB: action = (GetKeyState(VK_SHIFT) & 0x8000) ? NativeVideoSettingsAction::PreviousPage : NativeVideoSettingsAction::NextPage; break;
        }
        bool repeat = (lparam & (static_cast<LPARAM>(1) << 30)) != 0;
        bool direction = wparam >= VK_LEFT && wparam <= VK_DOWN;
        if (action && (!repeat || direction)) sendNativeVideoSettingsAction(*action);
        return true;
    }
    if (message == WM_CHAR || message == WM_SYSCHAR || message == WM_UNICHAR) return true;
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEWHEEL) {
        POINT point{static_cast<SHORT>(LOWORD(lparam)), static_cast<SHORT>(HIWORD(lparam))};
        if (message == WM_MOUSEWHEEL) ScreenToClient(owner, &point);
        RECT client{}; GetClientRect(owner, &client);
        if (client.right > 0 && client.bottom > 0) {
            if (message == WM_LBUTTONDOWN) SetCapture(owner);
            updateNativeVideoSettingsPointer(static_cast<float>(point.x) / client.right, static_cast<float>(point.y) / client.bottom,
                message == WM_MOUSEMOVE, message == WM_LBUTTONDOWN, message == WM_LBUTTONUP,
                message == WM_MOUSEWHEEL ? static_cast<SHORT>(HIWORD(wparam)) / WHEEL_DELTA : 0);
            if (message == WM_LBUTTONUP && GetCapture() == owner) ReleaseCapture();
        }
        return true;
    }
    if (message == WM_RBUTTONUP) { sendNativeVideoSettingsAction(NativeVideoSettingsAction::Back); return true; }
    if (message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_MOUSEHWHEEL)
        return true;
    return false;
}
void renderNativeVideoSettings(IDirect3DDevice9Ex* device, IDirect3DSurface9* output) {
    if (!device || !output || !nativeVideoSettingsOpen()) return;
    D3DSURFACE_DESC outputDesc{}; require(output->GetDesc(&outputDesc), "Read video-menu output");
    const auto rect = menuContentRect(outputDesc.Width, outputDesc.Height);
    Frame frame;
    {
        std::lock_guard lock(menuMutex);
        if (!menu.open) return;
        refreshLocked(); menu.outputWidth = outputDesc.Width; menu.outputHeight = outputDesc.Height; menu.contentRect = rect;
        frame = {menu.page, menu.selection, rowCounts[menu.page], menu.draft, menu.status, menu.monitors, {}, menu.draft != menu.lastCommitted};
        for (uint32_t button = 0; button < 5; ++button) frame.enabled[button] = buttonEnabledLocked(button);
    }
    std::lock_guard lock(renderMutex);
    if (!nativeVideoSettingsOpen()) return;
    if (!renderCache || renderCache->device.Get() != device) {
        renderCache = std::make_unique<RenderCache>(); renderCache->device = device;
    }
    float scale = std::clamp(std::min(outputDesc.Width / canvasWidth, outputDesc.Height / canvasHeight), 1.f, 2.f);
    uint32_t imageWidth = static_cast<uint32_t>(std::lround(canvasWidth * scale));
    uint32_t imageHeight = static_cast<uint32_t>(std::lround(canvasHeight * scale));
    updateImage(*renderCache, frame, imageWidth, imageHeight);
    SavedState saved(device);
    require(device->SetDepthStencilSurface(nullptr), "Detach video-menu depth");
    for (uint32_t i = 1; i < saved.count(); ++i) require(device->SetRenderTarget(i, nullptr), "Detach video-menu MRT");
    require(device->SetRenderTarget(0, output), "Bind video-menu output");
    D3DVIEWPORT9 viewport{0, 0, outputDesc.Width, outputDesc.Height, 0, 1};
    require(device->SetViewport(&viewport), "Set video-menu viewport");
    require(device->ColorFill(output, nullptr, D3DCOLOR_XRGB(7, 23, 40)), "Clear video-menu output");
    for (auto state : {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_LIGHTING, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
        D3DRS_FOGENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_DITHERENABLE, D3DRS_CLIPPLANEENABLE})
        require(device->SetRenderState(state, FALSE), "Set video-menu render state");
    require(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "Set video-menu culling");
    require(device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID), "Set video-menu fill");
    require(device->SetRenderState(D3DRS_COLORWRITEENABLE, 15), "Set video-menu color writes");
    require(device->SetVertexShader(nullptr), "Set video-menu vertex shader");
    require(device->SetPixelShader(nullptr), "Set video-menu pixel shader");
    require(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1), "Set video-menu vertices");
    require(device->SetTexture(0, renderCache->texture.Get()), "Set video-menu texture");
    for (auto state : {D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV}) require(device->SetSamplerState(0, state, D3DTADDRESS_CLAMP), "Set video-menu clamp");
    for (auto state : {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER}) require(device->SetSamplerState(0, state, D3DTEXF_LINEAR), "Set video-menu filtering");
    require(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE), "Set video-menu mip filter");
    require(device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0), "Set video-menu texture color");
    require(device->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0), "Set video-menu mip bias");
    require(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "Set video-menu texture operation");
    require(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE), "Set video-menu texture argument");
    require(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE), "Set video-menu texture alpha");
    require(device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0), "Set video-menu texture coordinates");
    require(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE), "Set video-menu texture transform");
    require(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE), "Disable video-menu extra texture stage");
    struct Vertex { float x, y, z, rhw, u, v; };
    Vertex vertices[]{{rect.x - .5f, rect.y - .5f, 0, 1, 0, 0}, {rect.x + rect.width - .5f, rect.y - .5f, 0, 1, 1, 0},
        {rect.x - .5f, rect.y + rect.height - .5f, 0, 1, 0, 1}, {rect.x + rect.width - .5f, rect.y + rect.height - .5f, 0, 1, 1, 1}};
    require(device->BeginScene(), "Begin video-menu scene");
    HRESULT draw = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    HRESULT end = device->EndScene();
    require(draw, "Draw video-menu image"); require(end, "End video-menu scene");
}
} // namespace Doritos::Graphics
