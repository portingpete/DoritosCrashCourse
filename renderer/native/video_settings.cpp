#include "video_settings.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>

namespace Doritos::Graphics {
namespace {
constexpr size_t maximumConfigBytes = 8192;
constexpr auto previewDuration = std::chrono::seconds(15);
std::atomic<uint64_t> temporarySerial{0};

struct OwnedHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~OwnedHandle() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    bool close() {
        const HANDLE previous = handle;
        handle = INVALID_HANDLE_VALUE;
        return CloseHandle(previous) != FALSE;
    }
};
std::string windowsError(const char* operation, DWORD code) {
    return std::string(operation) + " (Windows error " + std::to_string(code) + ").";
}
std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}
bool unsignedValue(std::string_view text, uint32_t& result) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool floatValue(std::string_view text, float& result) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, std::chars_format::general);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(result);
}
bool boolValue(std::string_view text, bool& result) {
    if (text == "0") { result = false; return true; }
    if (text == "1") { result = true; return true; }
    return false;
}
template <typename T, size_t N>
bool enumValue(std::string_view text, T& result, const std::array<std::pair<std::string_view,T>,N>& names) {
    for (const auto& [name,value] : names) if (text == name) { result = value; return true; }
    return false;
}
constexpr std::array windowModes{
    std::pair<std::string_view,WindowMode>{"windowed",WindowMode::Windowed},
    std::pair<std::string_view,WindowMode>{"borderless",WindowMode::Borderless}};
constexpr std::array vsyncModes{
    std::pair<std::string_view,VSyncMode>{"automatic",VSyncMode::Automatic},
    std::pair<std::string_view,VSyncMode>{"off",VSyncMode::Off},
    std::pair<std::string_view,VSyncMode>{"on",VSyncMode::On}};
constexpr std::array scalingModes{
    std::pair<std::string_view,ScalingMode>{"preserve_aspect",ScalingMode::PreserveAspect},
    std::pair<std::string_view,ScalingMode>{"stretch",ScalingMode::Stretch}};
constexpr std::array filteringModes{
    std::pair<std::string_view,TextureFiltering>{"original",TextureFiltering::Original},
    std::pair<std::string_view,TextureFiltering>{"bilinear",TextureFiltering::Bilinear},
    std::pair<std::string_view,TextureFiltering>{"trilinear",TextureFiltering::Trilinear}};

bool parseSettings(std::string_view content, VideoSettings& settings) {
    if (content.empty() || content.size() > maximumConfigBytes || content.find('\0') != content.npos) return false;
    std::map<std::string,std::string,std::less<>> values;
    bool section = false;
    while (!content.empty()) {
        const auto end = content.find('\n');
        const auto raw = content.substr(0,end);
        if (raw.size() > 256) return false;
        const auto line = trim(raw);
        if (end == content.npos) content = {};
        else content.remove_prefix(end + 1);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line == "[Video]") {
            if (section || !values.empty()) return false;
            section = true; continue;
        }
        if (!section) return false;
        const auto separator = line.find('=');
        if (separator == line.npos) return false;
        const auto key = trim(line.substr(0,separator)), value = trim(line.substr(separator + 1));
        if (key.empty() || value.empty() || !values.emplace(key,value).second) return false;
    }
    if (!section || values.size() != 20) return false;
    const auto get = [&](std::string_view name) -> std::string_view {
        const auto found = values.find(name);
        return found == values.end() ? std::string_view{} : std::string_view(found->second);
    };
    return get("version") == "1" &&
        enumValue(get("window_mode"),settings.windowMode,windowModes) &&
        unsignedValue(get("width"),settings.width) && unsignedValue(get("height"),settings.height) &&
        unsignedValue(get("monitor"),settings.monitor) && enumValue(get("vsync"),settings.vsync,vsyncModes) &&
        unsignedValue(get("frame_limit"),settings.frameLimit) &&
        unsignedValue(get("background_frame_limit"),settings.backgroundFrameLimit) &&
        enumValue(get("scaling"),settings.scaling,scalingModes) &&
        enumValue(get("filtering"),settings.filtering,filteringModes) &&
        unsignedValue(get("anisotropy"),settings.anisotropy) && floatValue(get("mip_bias"),settings.mipBias) &&
        boolValue(get("fxaa"),settings.fxaa) && floatValue(get("sharpness"),settings.sharpness) &&
        floatValue(get("brightness"),settings.brightness) && floatValue(get("contrast"),settings.contrast) &&
        floatValue(get("gamma"),settings.gamma) && floatValue(get("saturation"),settings.saturation) &&
        boolValue(get("low_latency"),settings.lowLatency) && boolValue(get("show_fps"),settings.showFps) &&
        validateVideoSettings(settings);
}
template <typename T, size_t N>
std::string_view enumName(T value, const std::array<std::pair<std::string_view,T>,N>& names) {
    for (const auto& [name,candidate] : names) if (candidate == value) return name;
    return {};
}
std::string floatText(float value) {
    std::array<char,64> buffer{};
    const auto result = std::to_chars(buffer.data(),buffer.data() + buffer.size(),value,
        std::chars_format::general,std::numeric_limits<float>::max_digits10);
    return std::string(buffer.data(),result.ptr);
}
std::string serializeSettings(const VideoSettings& settings) {
    std::string text = "[Video]\nversion=1\n";
    const auto add = [&](std::string_view key, std::string_view value) {
        text.append(key).append("=").append(value).append("\n");
    };
    const auto number = [&](std::string_view key, uint32_t value) { add(key,std::to_string(value)); };
    const auto real = [&](std::string_view key, float value) { add(key,floatText(value)); };
    add("window_mode",enumName(settings.windowMode,windowModes));
    number("width",settings.width); number("height",settings.height); number("monitor",settings.monitor);
    add("vsync",enumName(settings.vsync,vsyncModes));
    number("frame_limit",settings.frameLimit); number("background_frame_limit",settings.backgroundFrameLimit);
    add("scaling",enumName(settings.scaling,scalingModes)); add("filtering",enumName(settings.filtering,filteringModes));
    number("anisotropy",settings.anisotropy); real("mip_bias",settings.mipBias); number("fxaa",settings.fxaa);
    real("sharpness",settings.sharpness); real("brightness",settings.brightness); real("contrast",settings.contrast);
    real("gamma",settings.gamma); real("saturation",settings.saturation);
    number("low_latency",settings.lowLatency); number("show_fps",settings.showFps);
    return text;
}

enum class RequestKind { Initial, Preview, Restore, Fallback };
struct PendingRequest {
    VideoSettingsRequest request;
    RequestKind kind = RequestKind::Initial;
    bool dispatched = false;
};
struct Controller {
    std::mutex mutex;
    std::filesystem::path path;
    VideoSettings current, committed;
    VideoSettingsClock clock = nullptr;
    uint64_t serial = 0;
    std::optional<PendingRequest> pending;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::string error;
    std::string recoveryError;
    auto now() const { return clock ? clock() : std::chrono::steady_clock::now(); }
    void enqueue(const VideoSettings& settings, RequestKind kind) {
        if (++serial == 0) ++serial;
        pending = PendingRequest{{serial,settings},kind,false};
    }
    void restore() {
        deadline.reset();
        enqueue(committed,RequestKind::Restore);
    }
    void expire() {
        if (deadline && now() >= *deadline) restore();
    }
} controller;
}

bool validateVideoSettings(const VideoSettings& settings) {
    const auto inRange = [](float value, float minimum, float maximum) {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };
    const auto frameLimit = [](uint32_t value) { return value == 0 || (value >= 10 && value <= 1000); };
    return (settings.windowMode == WindowMode::Windowed || settings.windowMode == WindowMode::Borderless) &&
        settings.width >= 320 && settings.width <= 8192 && settings.height >= 200 && settings.height <= 8192 &&
        uint64_t(settings.width) * settings.height <= 67108864 && settings.monitor <= 63 &&
        (settings.vsync == VSyncMode::Automatic || settings.vsync == VSyncMode::Off || settings.vsync == VSyncMode::On) &&
        frameLimit(settings.frameLimit) && frameLimit(settings.backgroundFrameLimit) &&
        (settings.scaling == ScalingMode::PreserveAspect || settings.scaling == ScalingMode::Stretch) &&
        (settings.filtering == TextureFiltering::Original || settings.filtering == TextureFiltering::Bilinear ||
            settings.filtering == TextureFiltering::Trilinear) &&
        (settings.anisotropy == 0 || settings.anisotropy == 2 || settings.anisotropy == 4 ||
            settings.anisotropy == 8 || settings.anisotropy == 16) &&
        inRange(settings.mipBias,-1,2) && inRange(settings.sharpness,0,1) &&
        inRange(settings.brightness,-.25f,.25f) && inRange(settings.contrast,.5f,1.5f) &&
        inRange(settings.gamma,.5f,2) && inRange(settings.saturation,0,2);
}

bool saveVideoSettings(const std::filesystem::path& path, const VideoSettings& settings, std::string& error) {
    error.clear();
    if (path.empty() || !path.has_filename()) { error = "Video settings path is empty."; return false; }
    if (!validateVideoSettings(settings)) { error = "Video settings contain an unsupported value."; return false; }
    std::error_code directoryError;
    if (const auto parent = path.parent_path(); !parent.empty()) std::filesystem::create_directories(parent,directoryError);
    if (directoryError) { error = "Cannot create the video settings directory: " + directoryError.message(); return false; }
    const auto text = serializeSettings(settings);
    std::filesystem::path temporary;
    OwnedHandle file;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        temporary = path;
        temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++temporarySerial);
        file.handle = CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file.handle != INVALID_HANDLE_VALUE) break;
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
            error = windowsError("Cannot create temporary video settings",code); return false;
        }
    }
    if (file.handle == INVALID_HANDLE_VALUE) { error = "Cannot allocate a temporary video settings file."; return false; }
    DWORD written = 0;
    if (!WriteFile(file.handle,text.data(),DWORD(text.size()),&written,nullptr) || written != text.size() ||
        !FlushFileBuffers(file.handle)) {
        const DWORD code = GetLastError();
        file.close(); DeleteFileW(temporary.c_str());
        error = windowsError("Cannot write video settings",code); return false;
    }
    if (!file.close()) {
        const DWORD code = GetLastError(); DeleteFileW(temporary.c_str());
        error = windowsError("Cannot close video settings",code); return false;
    }
    // Same-directory rename publishes the complete new file. Windows can report
    // transient access/sharing denial while another thread closes an old file
    // identity, even when that reader allows delete sharing. Give only those
    // errors a brief bounded retry; a persistent lock or permission denial still
    // preserves the previous file and reports failure.
    DWORD replacementError = ERROR_SUCCESS;
    const auto retryUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(64);
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        if (MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        replacementError = GetLastError();
        if ((replacementError != ERROR_ACCESS_DENIED && replacementError != ERROR_SHARING_VIOLATION) ||
            attempt == 31 || std::chrono::steady_clock::now() >= retryUntil) break;
        Sleep(1);
    }
    DeleteFileW(temporary.c_str());
    error = windowsError("Cannot replace video settings",replacementError); return false;
}

VideoSettings loadVideoSettings(const std::filesystem::path& path, std::string& error) {
    error.clear();
    if (path.empty() || !path.has_filename()) { error = "Video settings path is empty."; return {}; }
    OwnedHandle file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
    if (file.handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) error = windowsError("Cannot read video settings",code);
        return {};
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.handle,&size)) { error = windowsError("Cannot size video settings",GetLastError()); return {}; }
    if (size.QuadPart <= 0 || size.QuadPart > LONGLONG(maximumConfigBytes)) { error = "Video settings file has an invalid size."; return {}; }
    std::string content(size_t(size.QuadPart),'\0');
    DWORD read = 0;
    if (!ReadFile(file.handle,content.data(),DWORD(content.size()),&read,nullptr) || read != content.size()) {
        error = windowsError("Cannot read complete video settings",GetLastError()); return {};
    }
    // Parsing no longer needs the old file identity. Release it promptly so a
    // concurrent writer can atomically replace its directory entry.
    if (!file.close()) { error = windowsError("Cannot close video settings",GetLastError()); return {}; }
    VideoSettings settings;
    if (!parseSettings(content,settings)) { error = "Video settings file is malformed or unsupported; defaults were loaded."; return {}; }
    return settings;
}

void initializeVideoSettings(const std::filesystem::path& path, VideoSettingsClock clock) {
    std::lock_guard lock(controller.mutex);
    controller.path = path;
    controller.current = {};
    controller.committed = loadVideoSettings(path,controller.error);
    controller.clock = clock;
    controller.pending.reset(); controller.deadline.reset();
    controller.recoveryError.clear();
    controller.enqueue(controller.committed,RequestKind::Initial);
}
VideoSettings currentVideoSettings() {
    std::lock_guard lock(controller.mutex); controller.expire(); return controller.current;
}
VideoSettings committedVideoSettings() {
    std::lock_guard lock(controller.mutex); return controller.committed;
}
bool requestVideoSettingsPreview(const VideoSettings& settings) {
    std::lock_guard lock(controller.mutex); controller.expire();
    if (!validateVideoSettings(settings)) { controller.error = "Video settings contain an unsupported value."; return false; }
    controller.error.clear(); controller.recoveryError.clear(); controller.enqueue(settings,RequestKind::Preview); return true;
}
bool confirmVideoSettingsPreview() {
    std::lock_guard lock(controller.mutex); controller.expire();
    if (!controller.deadline || controller.pending) return false;
    if (!saveVideoSettings(controller.path,controller.current,controller.error)) return false;
    controller.committed = controller.current; controller.deadline.reset(); return true;
}
void cancelVideoSettingsPreview() {
    std::lock_guard lock(controller.mutex); controller.expire();
    if (controller.deadline || (controller.pending && controller.pending->kind == RequestKind::Preview)) controller.restore();
}
std::optional<VideoSettingsRequest> takeVideoSettingsRequest() {
    std::lock_guard lock(controller.mutex); controller.expire();
    if (!controller.pending || controller.pending->dispatched) return {};
    controller.pending->dispatched = true; return controller.pending->request;
}
void completeVideoSettingsRequest(uint64_t serial, bool success, std::string error) {
    std::lock_guard lock(controller.mutex); controller.expire();
    if (!controller.pending || !controller.pending->dispatched || controller.pending->request.serial != serial) return;
    const auto applied = *controller.pending; controller.pending.reset();
    if (!success) {
        const std::string failure = error.empty() ? "The renderer could not apply these video settings." : std::move(error);
        if (applied.kind == RequestKind::Initial) {
            // The renderer's startup defaults remain applied. An unavailable
            // saved monitor/GPU option must not become the session's undo target.
            controller.committed = controller.current;
            controller.error = failure + " Default video settings remain active for this session. Apply and Keep to save them.";
        } else if (applied.kind == RequestKind::Restore) {
            // A display can disappear after settings were confirmed. Try one
            // conservative windowed/primary-display recovery, without silently
            // changing the file or retrying a failing restore indefinitely.
            controller.recoveryError = failure;
            controller.error = failure + " Returning to windowed defaults.";
            controller.enqueue(VideoSettings{},RequestKind::Fallback);
        } else if (applied.kind == RequestKind::Fallback) {
            controller.error = controller.recoveryError + " Default recovery also failed: " + failure;
            controller.recoveryError.clear();
        } else controller.error = failure;
        return;
    }
    controller.current = applied.request.settings;
    if (applied.kind == RequestKind::Fallback) {
        controller.committed = controller.current;
        controller.error = controller.recoveryError + " Windowed defaults are active for this session. Apply and Keep to save them.";
        controller.recoveryError.clear();
        return;
    }
    controller.error.clear();
    if (applied.kind == RequestKind::Preview) controller.deadline = controller.now() + previewDuration;
}
VideoSettingsStatus videoSettingsStatus() {
    std::lock_guard lock(controller.mutex); controller.expire();
    VideoSettingsStatus status;
    status.previewPending = controller.pending && controller.pending->kind == RequestKind::Preview;
    status.previewActive = controller.deadline.has_value();
    if (controller.deadline) {
        const auto remaining = std::chrono::ceil<std::chrono::seconds>(*controller.deadline - controller.now()).count();
        status.secondsRemaining = remaining > 0 ? uint32_t(remaining) : 0;
    }
    status.error = controller.error;
    return status;
}
void tickVideoSettings() { std::lock_guard lock(controller.mutex); controller.expire(); }
}
