#pragma once
#include "shader_cache.h"
#include "bone_palette.h"
#include "batch_palette.h"
#include "input_prompt_kind.h"
#include "gui_pointer.h"
#include "video_settings.h"
#include <memory>
#include <array>
#include <functional>
#include <span>
#include <utility>
#include <windows.h>
#include <d3d9.h>

namespace Doritos::Graphics {
struct RenderCounters {
    uint64_t draws = 0;
    uint64_t presents = 0;
    uint64_t triangles = 0;
    uint64_t initializationPresents = 0;
    uint64_t clearPresents = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t measuredIntervals = 0;
    uint64_t measuredNanoseconds = 0;
    uint64_t worstIntervalNanoseconds = 0;
};

// Explicit original renderer ownership, rather than a dimension heuristic.
// Auxiliary GUI, reflection and shadow targets retain their authored sizes.
struct SceneResolutionResources {
    uint32_t colorSurface = 0, depthSurface = 0;
    std::array<uint32_t,2> resolveTextures{};
    bool operator==(const SceneResolutionResources&) const = default;
};

// Native title renderer: original PC shader variants and native D3D9 resources.
// It consumes explicit engine contracts, never Xbox GPU command packets.
class D3D9Backend {
public:
    D3D9Backend();
    ~D3D9Backend();
    D3D9Backend(const D3D9Backend&) = delete;
    D3D9Backend& operator=(const D3D9Backend&) = delete;
    void initialize(HWND window, uint32_t width = 2560, uint32_t height = 1440);
    bool applyVideoSettings(const VideoSettings& settings, std::string& error);
    void setSceneResolutionResources(const SceneResolutionResources& resources,
        std::function<void(uint32_t,uint32_t)> updateCPU);
    std::pair<uint32_t,uint32_t> sceneResolution() const;
    void setPresentationUi(std::function<void(IDirect3DDevice9Ex*,IDirect3DSurface9*)> draw);
    VideoSettings appliedVideoSettings() const;
    GuiPointerResult mapPresentationPointer(float x, float y) const;
    void loadCaches(std::span<const uint8_t> windowsCache, std::span<const uint8_t> xenonCache, std::span<const uint8_t> nativeExtension = {}, std::span<const uint8_t> xenonSourceExtension = {});
    uint32_t createShader(const std::string& name, const ShaderKey& key);
    uint32_t createShaderForXenon(std::span<const uint8_t> originalBytecode, ShaderStage expectedStage,
        const ShaderSelection* selection = nullptr);
    bool optionalAvatarShaderUnavailable(std::span<const uint8_t> originalBytecode, ShaderStage expectedStage,
        const ShaderSelection* selection = nullptr) const;
    void bindShader(ShaderStage stage, uint32_t handle);
    void setFloatConstants(ShaderStage stage, uint32_t parameterId, std::span<const float> values);
    void setGuestFloatRegisters(ShaderStage stage, uint32_t firstRegister, std::span<const float> values);
    uint32_t createTexture(uint32_t width, uint32_t height, uint32_t levels, D3DFORMAT format, bool cube, bool renderTarget);
    // Explicit native capability, currently unused by original title hooks.
    // One-level DEFAULT D24S8 depth texture; no CPU upload/mips/color resolve.
    // Actual native allocation exhaustion returns0; other errors throw.
    uint32_t createDepthTexture(uint32_t width, uint32_t height);
    void verifyDepthTextureSurfaceAlias(uint32_t texture, uint32_t surface) const;
    // Genuine type1 End: validate the already-written level0 alias. No copy,
    // clear, target detach or color resolve/presentation accounting occurs.
    void finishDepthTargetAlias(uint32_t texture, uint32_t depthSurface, uint32_t colorSurface);
    uint32_t textureSurface(uint32_t texture, uint32_t face, uint32_t level);
    uint32_t createSurface(uint32_t width, uint32_t height, D3DFORMAT format, bool depth, D3DMULTISAMPLE_TYPE samples);
    D3DSURFACE_DESC surfaceDescription(uint32_t surface) const;
    D3DSURFACE_DESC textureDescription(uint32_t texture, uint32_t level) const;
    uint32_t textureLevelCount(uint32_t texture) const;
    void uploadSurface(uint32_t surface, D3DFORMAT format, uint32_t rowPitch, std::span<const uint8_t> bytes);
    void generateMipmaps(uint32_t texture);
    void bindTexture(uint32_t sampler, uint32_t texture);
    // Exact original prompt textures retain their resource handles and pixels.
    // Only sampling selects a separately owned Kenney keyboard/mouse image.
    void setInputPrompt(uint32_t texture, InputPromptKind kind, bool alphaMask = false);
    void setKeyboardMousePrompts(bool enabled);
    void setGuiScreenTexture(uint32_t texture);
    void setGuiPointerContext(const GuiPointerContext& context, bool monitorScreen = true);
    GuiPointerResult mapGuiPointer(const GuiPointerContext& context, float x, float y) const;
    // Read the actual native sampler for diagnostics; D3D9 AddRefs the output.
    void getSampledTexture(uint32_t sampler, IDirect3DBaseTexture9** texture) const;
    void bindRenderTarget(uint32_t index, uint32_t surface);
    void bindDepthSurface(uint32_t surface);
    void setViewport(const D3DVIEWPORT9& viewport);
    void setRenderState(D3DRENDERSTATETYPE state, uint32_t value);
    uint32_t renderState(D3DRENDERSTATETYPE state) const;
    D3DCAPS9 capabilities() const;
    void setClipPlane(uint32_t index, const std::array<float, 4>& plane);
    void setSamplerState(uint32_t sampler, D3DSAMPLERSTATETYPE state, uint32_t value);
    void getGammaRamp(D3DGAMMARAMP& ramp) const;
    void setGammaRamp(const D3DGAMMARAMP& ramp);
    void resolveRenderTarget(uint32_t destinationTexture, uint32_t expectedColorSurface = 0,
        D3DMULTISAMPLE_TYPE expectedSamples = D3DMULTISAMPLE_NONE);
    void selectPresentationTexture(uint32_t texture);
    uint32_t createVertexBuffer(std::span<const uint8_t> nativeBytes, uint32_t stride);
    uint32_t createIndexBuffer(std::span<const uint16_t> nativeIndices);
    uint32_t createDeclaration(std::span<const D3DVERTEXELEMENT9> elements);
    void bindVertexBuffer(uint32_t handle, uint32_t stream = 0);
    void unbindVertexBuffer(uint32_t stream = 0);
    void bindIndexBuffer(uint32_t handle);
    void unbindIndexBuffer();
    void updateVertexBuffer(uint32_t handle, std::span<const uint8_t> nativeBytes);
    void updateIndexBuffer(uint32_t handle, std::span<const uint16_t> nativeIndices);
    void bindDeclaration(uint32_t handle);
    void release(uint32_t handle);
    void beginFrame();
    void prepareFrame();
    void endFrame();
    void clear(uint32_t flags, uint32_t color, float depth, uint32_t stencil);
    void clearRectangles(std::span<const D3DRECT> rectangles, uint32_t flags, uint32_t color, float depth, uint32_t stencil,
        uint32_t originalCaller = 0);
    bool usesTitleBonePalette() const;
    void reportTitlePaletteState() const;
    void drawIndexedTriangles(int32_t baseVertex, uint32_t vertexCount, uint32_t startIndex, uint32_t triangleCount,
        const TitleBoneDraw* boneDraw = nullptr, const TitleBatchDraw* batchDraw = nullptr, uint32_t cjDiagnosticSerial = 0);
    void drawIndexedTriangleStrip(int32_t baseVertex, uint32_t vertexCount, uint32_t startIndex, uint32_t triangleCount);
    void drawIndexedImmediate(D3DPRIMITIVETYPE topology, std::span<const uint8_t> nativeVertices,
        uint32_t stride, std::span<const uint16_t> nativeIndices, uint32_t triangleCount);
    void present();
    void presentInitialization();
    void recordBridgePhaseTime(uint64_t nanoseconds);
    void recordConstantDecodePhaseTime(uint64_t nanoseconds);
    void finishPhaseFrame();
    RenderCounters counters() const;
    size_t windowsVariantCount() const;
    size_t xenonVariantCount() const;
    size_t matchingVariantCount() const;
private:
    void drawIndexed(D3DPRIMITIVETYPE topology, int32_t baseVertex, uint32_t vertexCount, uint32_t startIndex,
        uint32_t triangleCount, const TitleBoneDraw* boneDraw = nullptr, const TitleBatchDraw* batchDraw = nullptr,
        uint32_t cjDiagnosticSerial = 0);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
