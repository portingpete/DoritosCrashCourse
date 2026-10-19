// In-game native video menu contract. A hidden synthetic title window owns a
// D3D9Ex device; all UI interaction is sent through pure events or its message
// handler, and actual rendered pixels are read back from the hidden backbuffer.
#include "renderer/native/d3d9_backend.h"
#include "renderer/native/video_presenter.h"
#include "renderer/native/video_settings_ui.h"
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Doritos::Graphics;
namespace {
using Microsoft::WRL::ComPtr;
unsigned checks = 0;
std::chrono::steady_clock::time_point fakeNow{};

void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " failed (" +
        std::to_string(static_cast<uint32_t>(result)) + ")");
}
std::chrono::steady_clock::time_point fakeClock() { return fakeNow; }

struct HiddenTitleWindow {
    HWND window = nullptr;
    HiddenTitleWindow() {
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"DoritosNativeInGameVideoUiContracts";
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Cannot register hidden in-game video fixture class");
        window = CreateWindowW(type.lpszClassName, L"Hidden in-game video fixture", WS_POPUP,
            0, 0, 640, 360, nullptr, nullptr, type.hInstance, nullptr);
        if (!window || IsWindowVisible(window))
            throw std::runtime_error("The synthetic title window must remain hidden");
    }
    ~HiddenTitleWindow() { if (window) DestroyWindow(window); }
};

struct TempConfig {
    std::filesystem::path directory, file;
    TempConfig() {
        directory = std::filesystem::temp_directory_path() /
            (L"DoritosNativeInGameVideoUi-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory);
        file = directory / L"video.ini";
    }
    ~TempConfig() {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        ignored.clear();
        std::filesystem::remove(directory, ignored); // succeeds only when this exact directory is empty
    }
};

struct CapturedFrame {
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> pixels;
};
void capturePixels(IDirect3DDevice9Ex* device, IDirect3DSurface9* output, CapturedFrame& result) {
    D3DSURFACE_DESC description{};
    check(output->GetDesc(&description), "Describe in-game UI output surface");
    require(description.Format == D3DFMT_A8R8G8B8, "In-game UI output format is not BGRA8");
    ComPtr<IDirect3DSurface9> readback;
    check(device->CreateOffscreenPlainSurface(description.Width, description.Height, D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM, &readback, nullptr), "Create hidden in-game UI pixel readback");
    check(device->GetRenderTargetData(output, readback.Get()), "Read hidden in-game UI GPU pixels");
    D3DLOCKED_RECT locked{};
    check(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "Lock hidden in-game UI pixels");
    if (!locked.pBits || locked.Pitch < LONG(description.Width * 4)) {
        readback->UnlockRect();
        throw std::runtime_error("Invalid hidden in-game UI readback pitch");
    }
    result.width = description.Width;
    result.height = description.Height;
    result.pixels.resize(size_t(result.width) * result.height);
    for (uint32_t y = 0; y < result.height; ++y)
        std::memcpy(result.pixels.data() + size_t(y) * result.width,
            static_cast<const uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch, size_t(result.width) * 4);
    check(readback->UnlockRect(), "Unlock hidden in-game UI pixels");
}
void requireMenuPixels(const CapturedFrame& frame) {
    require(frame.width >= 320 && frame.height >= 200 && frame.pixels.size() == size_t(frame.width) * frame.height,
        "In-game menu frame has invalid dimensions or no pixels");
    size_t nonBackground = 0, brightText = 0;
    for (uint32_t value : frame.pixels) {
        const uint32_t rgb = value & 0x00ffffffu;
        if (rgb != 0x00071728u) ++nonBackground;
        const int red = int((rgb >> 16) & 255), green = int((rgb >> 8) & 255), blue = int(rgb & 255);
        if (red > 185 && green > 185 && blue > 185) ++brightText;
    }
    require(nonBackground > size_t(frame.width) * frame.height / 20,
        "In-game settings menu did not paint its panels into the real GPU frame");
    require(brightText > 20, "In-game settings menu did not rasterize readable text into the GPU frame");
}
size_t differingPixels(const CapturedFrame& a, const CapturedFrame& b) {
    require(a.width == b.width && a.height == b.height && a.pixels.size() == b.pixels.size(),
        "Cannot compare menu frames captured at different render sizes");
    size_t changed = 0;
    for (size_t index = 0; index < a.pixels.size(); ++index)
        if ((a.pixels[index] & 0x00ffffffu) != (b.pixels[index] & 0x00ffffffu)) ++changed;
    return changed;
}

void renderOneFrame(D3D9Backend& backend) {
    backend.beginFrame();
    backend.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff102030, 1.f, 0);
    backend.endFrame();
    backend.present();
}

std::pair<float, float> normalizedClientPoint(HWND owner, float logicalX, float logicalY) {
    RECT client{};
    require(GetClientRect(owner, &client) && client.right > 0 && client.bottom > 0,
        "Cannot read hidden title client dimensions for pointer mapping");
    const auto visible = videoContentRect(static_cast<uint32_t>(client.right), static_cast<uint32_t>(client.bottom),
        1280, 720, ScalingMode::PreserveAspect);
    require(visible.width > 0 && visible.height > 0, "Hidden title client has no visible 16:9 pointer area");
    return {(visible.x + logicalX * visible.width / 1280.f) / client.right,
        (visible.y + logicalY * visible.height / 720.f) / client.bottom};
}
void pointerClickLogical(HWND owner, float x, float y) {
    const auto [normalizedX, normalizedY] = normalizedClientPoint(owner, x, y);
    updateNativeVideoSettingsPointer(normalizedX, normalizedY, true, true, false, 0);
    updateNativeVideoSettingsPointer(normalizedX, normalizedY, false, false, true, 0);
}
void clickTab(HWND owner, uint32_t page) {
    pointerClickLogical(owner, 48.f + 396.f * page + 192.f, 132.f);
}
std::optional<std::filesystem::path> captureDirectory() {
    const DWORD length = GetEnvironmentVariableW(L"DORITOS_VIDEO_UI_CAPTURE_DIR", nullptr, 0);
    if (!length || length > 32768) return {};
    std::wstring value(length, L'\0');
    const DWORD copied = GetEnvironmentVariableW(L"DORITOS_VIDEO_UI_CAPTURE_DIR", value.data(), length);
    if (!copied || copied >= length) return {};
    value.resize(copied);
    return std::filesystem::path(value);
}
void saveCaptureIfRequested(const CapturedFrame& frame, const wchar_t* filename) {
    const auto directory = captureDirectory();
    if (!directory) return;
    try {
        std::error_code error;
        std::filesystem::create_directories(*directory, error);
        if (error || !std::filesystem::is_directory(*directory, error) || error) {
            std::fwprintf(stderr, L"[VIDEO UI FIXTURE] Optional capture directory is unavailable: %ls\n", directory->c_str());
            return;
        }
        const uint64_t imageBytes = uint64_t(frame.width) * frame.height * 4;
        if (!frame.width || !frame.height || imageBytes > UINT32_MAX - sizeof(BITMAPFILEHEADER) - sizeof(BITMAPINFOHEADER))
            throw std::runtime_error("In-game UI capture exceeds BMP bounds");
        const auto path = *directory / filename;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) {
                std::fwprintf(stderr, L"[VIDEO UI FIXTURE] Capture exists; skipped without overwriting: %ls\n", path.c_str());
                return;
            }
            throw std::runtime_error("Cannot create in-game UI BMP capture");
        }
        BITMAPFILEHEADER fileHeader{};
        fileHeader.bfType = 0x4d42;
        fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(imageBytes);
        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info); info.biWidth = static_cast<LONG>(frame.width);
        info.biHeight = -static_cast<LONG>(frame.height); info.biPlanes = 1; info.biBitCount = 32;
        info.biCompression = BI_RGB; info.biSizeImage = static_cast<DWORD>(imageBytes);
        DWORD written = 0;
        bool success = WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr) && written == sizeof(fileHeader);
        written = 0;
        success = success && WriteFile(file, &info, sizeof(info), &written, nullptr) && written == sizeof(info);
        written = 0;
        success = success && WriteFile(file, frame.pixels.data(), static_cast<DWORD>(imageBytes), &written, nullptr) && written == imageBytes;
        const DWORD writeError = GetLastError();
        CloseHandle(file);
        if (!success) {
            DeleteFileW(path.c_str());
            throw std::runtime_error("Cannot write full in-game UI BMP capture (Windows error " + std::to_string(writeError) + ")");
        }
        std::fwprintf(stderr, L"[VIDEO UI FIXTURE] Saved in-game page capture: %ls\n", path.c_str());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[VIDEO UI FIXTURE] Optional in-game page capture failed: %s\n", error.what());
    }
}
void moveSelectionTo(uint32_t selection) {
    auto snapshot = nativeVideoSettingsSnapshot();
    require(snapshot.open, "Cannot navigate a closed video settings menu");
    for (uint32_t count = 0; count < snapshot.rowCount + 5 && snapshot.selection != selection; ++count) {
        sendNativeVideoSettingsAction(NativeVideoSettingsAction::Down);
        snapshot = nativeVideoSettingsSnapshot();
    }
    require(nativeVideoSettingsSnapshot().selection == selection, "Video settings selection did not reach the requested row");
}

void verifyMenuInputAndRendering(D3D9Backend& backend, HWND owner, HWND foregroundBefore,
    CapturedFrame& initialFrame, bool& captureNext, CapturedFrame& capture) {
    registerNativeVideoSettingsOwner(owner, nullptr, false);
    showNativeVideoSettings();
    auto snapshot = nativeVideoSettingsSnapshot();
    require(snapshot.open && snapshot.page == 0 && snapshot.selection == 0 && snapshot.rowCount == 7,
        "Opening the in-game menu did not select the first Display setting");

    captureNext = true;
    renderOneFrame(backend);
    require(captureNext == false, "In-game menu frame callback did not capture its GPU output");
    initialFrame = capture;
    require(initialFrame.width == 640 && initialFrame.height == 360,
        "Initial native UI frame did not use the configured 640x360 render resolution");
    requireMenuPixels(initialFrame);
    // Exercise the real title key-message adapter by directly dispatching
    // synthetic messages. No OS input queue or foreground window is used.
    handleNativeVideoSettingsMessage(owner, WM_KEYUP, VK_DOWN, 0);
    require(handleNativeVideoSettingsMessage(owner, WM_KEYDOWN, VK_DOWN, 1),
        "In-game menu did not consume a synthetic keyboard navigation message");
    require(nativeVideoSettingsSnapshot().selection == 1,
        "Keyboard Down did not move the in-game settings selection");

    // Controller navigation is edge-latched and should move only once while
    // the same synthetic button remains held.
    updateNativeVideoSettingsController(0);
    updateNativeVideoSettingsController(VideoUiDown);
    updateNativeVideoSettingsController(VideoUiDown);
    require(nativeVideoSettingsSnapshot().selection == 2,
        "Controller-held Down repeated immediately or failed to move once");
    updateNativeVideoSettingsController(0);

    // The first pointer sample anchors the cursor. Repeated stationary samples
    // cannot steal the selection from row 2; genuine motion into row 4 can.
    auto point = normalizedClientPoint(owner, 640.f, 178.f + 44.f * 4.f + 20.f);
    updateNativeVideoSettingsPointer(point.first, point.second, true, false, false, 0);
    updateNativeVideoSettingsPointer(point.first, point.second, true, false, false, 0);
    require(nativeVideoSettingsSnapshot().selection == 2,
        "A stationary mouse pointer stole navigation selection");
    point = normalizedClientPoint(owner, 650.f, 178.f + 44.f * 4.f + 20.f);
    updateNativeVideoSettingsPointer(point.first, point.second, true, false, false, 0);
    require(nativeVideoSettingsSnapshot().selection == 4,
        "Moving the mouse over row 4 did not select the hovered Display row");

    // A release alone and a press on one tab followed by release on another
    // must not switch pages. Matching press/release pairs do switch them.
    const auto tabOne = normalizedClientPoint(owner, 48.f + 396.f + 192.f, 132.f);
    const auto tabTwo = normalizedClientPoint(owner, 48.f + 2.f * 396.f + 192.f, 132.f);
    updateNativeVideoSettingsPointer(tabOne.first, tabOne.second, false, false, true, 0);
    require(nativeVideoSettingsSnapshot().page == 0, "An unowned pointer release changed the menu page");
    updateNativeVideoSettingsPointer(tabOne.first, tabOne.second, true, true, false, 0);
    updateNativeVideoSettingsPointer(tabTwo.first, tabTwo.second, true, false, true, 0);
    require(nativeVideoSettingsSnapshot().page == 0, "Pointer press/release on different tabs activated a page");
    clickTab(owner, 1);
    require(nativeVideoSettingsSnapshot().page == 1 && nativeVideoSettingsSnapshot().rowCount == 5,
        "A matching mouse tab click did not open Performance settings");
    renderOneFrame(backend);
    clickTab(owner, 2);
    require(nativeVideoSettingsSnapshot().page == 2 && nativeVideoSettingsSnapshot().rowCount == 6,
        "A matching mouse tab click did not open Image settings");
    renderOneFrame(backend);

    // Brightness is Image row 2. Its right-arrow hit target and wheel path
    // both alter only the draft; a release without a matching press is inert.
    const float brightnessY = 178.f + 44.f * 2.f + 20.f;
    pointerClickLogical(owner, 1206.f, brightnessY);
    snapshot = nativeVideoSettingsSnapshot();
    require(snapshot.draft.brightness == .01f && snapshot.selection == 2,
        "Mouse right-arrow did not adjust the selected image setting");
    point = normalizedClientPoint(owner, 1206.f, brightnessY);
    updateNativeVideoSettingsPointer(point.first, point.second, false, false, true, 0);
    require(nativeVideoSettingsSnapshot().draft.brightness == .01f,
        "A pointer release without a press repeated a settings adjustment");
    point = normalizedClientPoint(owner, 900.f, brightnessY);
    updateNativeVideoSettingsPointer(point.first, point.second, true, false, false, 1);
    require(nativeVideoSettingsSnapshot().draft.brightness == .02f,
        "Mouse wheel did not adjust the setting under the pointer");

    clickTab(owner, 0);
    require(nativeVideoSettingsSnapshot().page == 0, "Mouse could not return to Display settings");
    // The current 640x360 mode is appended to the supported choices. Advancing
    // once selects 1280x720, and this draft is later applied by backend.present.
    pointerClickLogical(owner, 1206.f, 198.f);
    snapshot = nativeVideoSettingsSnapshot();
    require(snapshot.draft.width == 1280 && snapshot.draft.height == 720 &&
        snapshot.draft.brightness == .02f,
        "Mouse resolution adjustment did not retain the image draft or choose 1280x720");
    require(GetForegroundWindow() == foregroundBefore && !IsWindowVisible(owner),
        "In-game UI input or rendering stole foreground ownership or showed the fixture window");
    (void)backend;
}

void verifyRendererPreviewAndConfirmation(D3D9Backend& backend, HWND owner,
    bool& captureNext, CapturedFrame& capture, TempConfig& config) {
    auto snapshot = nativeVideoSettingsSnapshot();
    require(snapshot.page == 0 && snapshot.selection == 0 && snapshot.rowCount == 7,
        "Display resolution adjustment did not leave the first row selected");
    moveSelectionTo(snapshot.rowCount); // Apply
    sendNativeVideoSettingsAction(NativeVideoSettingsAction::Select);
    snapshot = nativeVideoSettingsSnapshot();
    require(snapshot.status.previewPending && snapshot.draft.width == 1280 && snapshot.draft.height == 720,
        "Apply did not queue the edited render resolution for the presentation boundary");

    captureNext = true;
    renderOneFrame(backend); // Processes ResetEx and draws the menu into the new native frame.
    require(!captureNext && capture.width == 1280 && capture.height == 720,
        "Applied resolution did not change the actual UI output backbuffer to 1280x720");
    require(backend.counters().width == 1280 && backend.counters().height == 720 &&
        backend.sceneResolution() == std::pair<uint32_t,uint32_t>{1280,720} &&
        backend.appliedVideoSettings().width == 1280 && backend.appliedVideoSettings().height == 720,
        "Presentation boundary did not apply the chosen native render resolution");
    snapshot = nativeVideoSettingsSnapshot();
    require(snapshot.status.previewActive && snapshot.status.secondsRemaining == 15 &&
        snapshot.selection == snapshot.rowCount + 1,
        "Successful in-game preview did not start the full 15-second Keep confirmation");
    requireMenuPixels(capture);
    const auto fifteenSecondFrame = capture;

    fakeNow += std::chrono::seconds(14);
    captureNext = true;
    renderOneFrame(backend);
    snapshot = nativeVideoSettingsSnapshot();
    require(snapshot.status.previewActive && snapshot.status.secondsRemaining == 1,
        "The native confirmation frame did not show the final second before timeout");
    require(differingPixels(fifteenSecondFrame, capture) > 8,
        "The real GPU-rendered confirmation countdown did not change between 15 and 1 seconds");

    sendNativeVideoSettingsAction(NativeVideoSettingsAction::Select); // Keep Changes
    require(!videoSettingsStatus().previewActive && committedVideoSettings().width == 1280 &&
        committedVideoSettings().height == 720 && committedVideoSettings().brightness == .02f,
        "Keep Changes did not commit the successfully applied resolution and image setting");
    std::string error;
    require(loadVideoSettings(config.file, error) == committedVideoSettings() && error.empty(),
        "Keep Changes did not persist the settings displayed in the native menu");

    // Optional screenshots come from the actual GPU-rendered in-game menu,
    // after the confirmed 1280x720 mode is active. The fixture never shows
    // its hidden title window or injects desktop input.
    const auto capturePage = [&](const wchar_t* filename) {
        captureNext = true;
        renderOneFrame(backend);
        require(!captureNext && capture.width == 1280 && capture.height == 720,
            "Confirmed in-game page was not captured at native 1280x720 render resolution");
        requireMenuPixels(capture);
        saveCaptureIfRequested(capture, filename);
    };
    capturePage(L"video-settings-display.bmp");
    sendNativeVideoSettingsAction(NativeVideoSettingsAction::NextPage);
    capturePage(L"video-settings-performance.bmp");
    sendNativeVideoSettingsAction(NativeVideoSettingsAction::NextPage);
    capturePage(L"video-settings-image.bmp");
    sendNativeVideoSettingsAction(NativeVideoSettingsAction::NextPage);
    require(nativeVideoSettingsSnapshot().page == 0 && nativeVideoSettingsSnapshot().selection == 0,
        "Could not return to Display after capturing all native settings pages");

    // A second real preview is allowed to expire. The renderer must process
    // the queued committed-mode restore and redraw the menu at that size.
    snapshot = nativeVideoSettingsSnapshot();
    require(snapshot.selection == 0, "Could not return to the render-resolution row");
    sendNativeVideoSettingsAction(NativeVideoSettingsAction::Left); // 1280x720 -> the retained 640x360 choice
    require(nativeVideoSettingsSnapshot().draft.width == 640 && nativeVideoSettingsSnapshot().draft.height == 360,
        "The original 640x360 mode was not retained as a native render-resolution choice");
    moveSelectionTo(snapshot.rowCount);
    sendNativeVideoSettingsAction(NativeVideoSettingsAction::Select);
    require(nativeVideoSettingsSnapshot().status.previewPending,
        "Second video mode did not enter the renderer preview queue");
    renderOneFrame(backend);
    require(backend.counters().width == 640 && backend.counters().height == 360 &&
        nativeVideoSettingsSnapshot().status.previewActive,
        "Second native render-resolution preview did not apply at the presentation boundary");
    fakeNow += std::chrono::seconds(15);
    renderOneFrame(backend);
    snapshot = nativeVideoSettingsSnapshot();
    require(!snapshot.status.previewActive && !snapshot.status.previewPending &&
        backend.counters().width == 1280 && backend.counters().height == 720 &&
        committedVideoSettings().width == 1280 && committedVideoSettings().height == 720 &&
        currentVideoSettings() == committedVideoSettings(),
        "The 15-second native menu timeout did not restore the last confirmed render resolution");
    require(!IsWindowVisible(owner), "Applying or reverting the hidden native preview showed the fixture window");
}

void verify(D3D9Backend& backend, HWND owner, TempConfig& config) {
    VideoSettings initial{};
    initial.width = 640; initial.height = 360;
    std::string error;
    require(saveVideoSettings(config.file, initial, error) && error.empty(),
        "Cannot seed the isolated 640x360 video settings fixture");
    initializeVideoSettings(config.file, fakeClock);
    const auto startup = takeVideoSettingsRequest();
    require(startup && startup->settings == initial, "Startup did not load the isolated render-resolution fixture");
    completeVideoSettingsRequest(startup->serial, true);

    HWND foregroundBefore = GetForegroundWindow();
    backend.initialize(owner, 640, 360);
    require(!IsWindowVisible(owner) && GetForegroundWindow() == foregroundBefore,
        "Initializing the hidden D3D9Ex title fixture changed desktop visibility or focus");
    renderOneFrame(backend); // Apply startup settings through the native present boundary.
    require(backend.counters().width == 640 && backend.counters().height == 360,
        "Native fixture did not start at its configured 640x360 backbuffer");

    CapturedFrame capture;
    bool captureNext = false;
    backend.setPresentationUi([&](IDirect3DDevice9Ex* device, IDirect3DSurface9* output) {
        if (!nativeVideoSettingsOpen()) return;
        ComPtr<IDirect3DSurface9> targetBefore, targetAfter;
        D3DVIEWPORT9 viewportBefore{}, viewportAfter{};
        check(device->GetRenderTarget(0, &targetBefore), "Capture pre-menu render target");
        check(device->GetViewport(&viewportBefore), "Capture pre-menu viewport");
        renderNativeVideoSettings(device, output);
        check(device->GetRenderTarget(0, &targetAfter), "Capture post-menu render target");
        check(device->GetViewport(&viewportAfter), "Capture post-menu viewport");
        require(targetBefore.Get() == targetAfter.Get() &&
            std::memcmp(&viewportBefore, &viewportAfter, sizeof(viewportBefore)) == 0,
            "In-game menu renderer did not restore the title render target and viewport");
        if (captureNext) {
            capturePixels(device, output, capture);
            captureNext = false;
        }
    });

    const HWND foregroundAfterInitialize = GetForegroundWindow();
    CapturedFrame initialFrame;
    verifyMenuInputAndRendering(backend, owner, foregroundAfterInitialize,
        initialFrame, captureNext, capture);
    verifyRendererPreviewAndConfirmation(backend, owner, captureNext, capture, config);

    sendNativeVideoSettingsAction(NativeVideoSettingsAction::Back);
    require(!nativeVideoSettingsOpen(), "Back did not close the in-game video settings menu");
    require(GetForegroundWindow() == foregroundAfterInitialize && !IsWindowVisible(owner),
        "Closing the in-game menu changed the desktop foreground or exposed the hidden title fixture");
    registerNativeVideoSettingsOwner(nullptr, nullptr, false);
    backend.setPresentationUi({});
}
}

int main() {
    HiddenTitleWindow window;
    try {
        TempConfig config;
        D3D9Backend backend;
        verify(backend, window.window, config);
        std::printf("Native in-game video UI contract: %u checks passed; hidden D3D9Ex title frame and synthetic events only\n", checks);
        return 0;
    } catch (const std::exception& error) {
        if (nativeVideoSettingsOpen()) sendNativeVideoSettingsAction(NativeVideoSettingsAction::Back);
        registerNativeVideoSettingsOwner(nullptr, nullptr, false);
        std::fprintf(stderr, "Native in-game video UI contract failed after %u checks: %s\n", checks, error.what());
        return 1;
    }
}
