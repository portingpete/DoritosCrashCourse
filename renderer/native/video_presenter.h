#pragma once
#include "video_settings.h"
#include <d3d9.h>
#include <memory>

namespace Doritos::Graphics {
struct VideoContentRect {
    LONG x = 0, y = 0, width = 0, height = 0;
    bool operator==(const VideoContentRect&) const = default;
};
VideoContentRect videoContentRect(uint32_t outputWidth, uint32_t outputHeight,
    uint32_t sourceWidth, uint32_t sourceHeight, ScalingMode mode);
class VideoPresenter {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    VideoPresenter();
    ~VideoPresenter();
    void prepare(IDirect3DDevice9Ex* device, uint32_t sourceWidth, uint32_t sourceHeight,
        const VideoSettings& settings);
    VideoContentRect draw(IDirect3DDevice9Ex* device, IDirect3DSurface9* source,
        IDirect3DSurface9* output, const VideoSettings& settings, double fps, double milliseconds, HWND window = nullptr);
};
}
