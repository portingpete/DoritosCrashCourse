#pragma once
#include "video_settings.h"
#include <windows.h>
#include <d3d9.h>

namespace Doritos::Graphics {
enum class NativeVideoSettingsAction { Up, Down, Left, Right, Select, Back, NextPage, PreviousPage };
enum NativeVideoSettingsController : uint32_t {
    VideoUiUp = 1u << 0, VideoUiDown = 1u << 1, VideoUiLeft = 1u << 2, VideoUiRight = 1u << 3,
    VideoUiSelect = 1u << 4, VideoUiBack = 1u << 5, VideoUiNextPage = 1u << 6, VideoUiPreviousPage = 1u << 7,
};
struct NativeVideoSettingsSnapshot {
    bool open = false;
    uint32_t page = 0, selection = 0, rowCount = 0;
    VideoSettings draft;
    VideoSettingsStatus status;
};
// Compatibility with the title's owner/message hook; this API never creates a
// separate window. Visibility is retained only for hidden-fixture compatibility.
void registerNativeVideoSettingsOwner(HWND owner, void (*focusChanged)(bool) = nullptr, bool visible = true);
void showNativeVideoSettings();
bool nativeVideoSettingsOpen();
bool handleNativeVideoSettingsMessage(HWND owner, UINT message, WPARAM wparam, LPARAM lparam);
bool translateNativeVideoSettingsMessage(const MSG& message);
void sendNativeVideoSettingsAction(NativeVideoSettingsAction action);
void updateNativeVideoSettingsController(uint32_t heldButtons);
// Normalized client-space coordinates. Press/release must belong to one item.
void updateNativeVideoSettingsPointer(float x, float y, bool moved, bool pressed, bool released, int wheelSteps = 0);
NativeVideoSettingsSnapshot nativeVideoSettingsSnapshot();
// Render after final frame effects and before PresentEx; restores all D3D state.
void renderNativeVideoSettings(IDirect3DDevice9Ex* device, IDirect3DSurface9* output);
}
