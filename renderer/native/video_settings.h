#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Doritos::Graphics {
enum class WindowMode { Windowed, Borderless };
enum class VSyncMode { Automatic, Off, On };
enum class ScalingMode { PreserveAspect, Stretch };
enum class TextureFiltering { Original, Bilinear, Trilinear };

// Native PC preferences are stored separately from the original title profile.
struct VideoSettings {
    WindowMode windowMode = WindowMode::Windowed;
    uint32_t width = 2560, height = 1440;
    uint32_t monitor = 0;
    VSyncMode vsync = VSyncMode::Automatic;
    uint32_t frameLimit = 60, backgroundFrameLimit = 30;
    ScalingMode scaling = ScalingMode::PreserveAspect;
    TextureFiltering filtering = TextureFiltering::Original;
    uint32_t anisotropy = 0;
    float mipBias = 0;
    bool fxaa = false;
    float sharpness = 0;
    float brightness = 0;
    float contrast = 1, gamma = 1, saturation = 1;
    bool lowLatency = false, showFps = false;
    bool operator==(const VideoSettings&) const = default;
};

bool validateVideoSettings(const VideoSettings& settings);
bool saveVideoSettings(const std::filesystem::path& path, const VideoSettings& settings, std::string& error);
VideoSettings loadVideoSettings(const std::filesystem::path& path, std::string& error);

struct VideoSettingsRequest {
    uint64_t serial = 0;
    VideoSettings settings;
};
struct VideoSettingsStatus {
    bool previewPending = false;
    bool previewActive = false;
    uint32_t secondsRemaining = 0;
    std::string error;
};
using VideoSettingsClock = std::chrono::steady_clock::time_point (*)();

// Startup enqueues the loaded settings. A preview's 15-second confirmation
// period begins only after the renderer successfully applies its serial.
// The optional clock is for deterministic contract tests; production uses the
// steady clock. A stale renderer acknowledgement never changes controller state.
// Rejected startup preferences use applied defaults as the session's undo target.
// A failed restore tries windowed defaults once; recovery never rewrites the INI.
void initializeVideoSettings(const std::filesystem::path& path, VideoSettingsClock clock = nullptr);
VideoSettings currentVideoSettings();
VideoSettings committedVideoSettings();
bool requestVideoSettingsPreview(const VideoSettings& settings);
bool confirmVideoSettingsPreview();
void cancelVideoSettingsPreview();
std::optional<VideoSettingsRequest> takeVideoSettingsRequest();
void completeVideoSettingsRequest(uint64_t serial, bool success, std::string error = {});
VideoSettingsStatus videoSettingsStatus();
void tickVideoSettings();
}
