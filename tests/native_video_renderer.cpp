// Synthetic D3D9 presentation contract. All output is rendered to a hidden
// fixture window and read back from GPU surfaces; the title is never launched.
#include "renderer/native/d3d9_backend.h"
#include "renderer/native/video_presenter.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Doritos::Graphics;
namespace {
using Microsoft::WRL::ComPtr;
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(uint32_t(result)) + ")");
}
bool nearByte(uint8_t value, int expected, int tolerance = 4) {
    return std::abs(int(value) - expected) <= tolerance;
}
uint8_t red(uint32_t value) { return uint8_t(value >> 16); }
uint8_t green(uint32_t value) { return uint8_t(value >> 8); }
uint8_t blue(uint32_t value) { return uint8_t(value); }

struct FixtureWindow {
    HWND window = nullptr;
    FixtureWindow() {
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"DoritosNativeVideoRendererContracts";
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Cannot register hidden video renderer fixture");
        window = CreateWindowW(type.lpszClassName, L"Hidden video renderer fixture", WS_POPUP,
            0, 0, 640, 360, nullptr, nullptr, type.hInstance, nullptr);
        if (!window || IsWindowVisible(window)) throw std::runtime_error("Video renderer fixture must remain hidden");
    }
    ~FixtureWindow() { if (window) DestroyWindow(window); }
};

struct DeviceFixture {
    ComPtr<IDirect3D9Ex> d3d;
    ComPtr<IDirect3DDevice9Ex> device;
    ComPtr<IDirect3DSurface9> output;
    ComPtr<IDirect3DSurface9> source;
    DeviceFixture(HWND window) {
        check(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d), "Create fixture D3D9Ex");
        D3DPRESENT_PARAMETERS parameters{};
        parameters.BackBufferWidth = 640;
        parameters.BackBufferHeight = 360;
        parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
        parameters.BackBufferCount = 1;
        parameters.MultiSampleType = D3DMULTISAMPLE_NONE;
        parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.hDeviceWindow = window;
        parameters.Windowed = TRUE;
        parameters.EnableAutoDepthStencil = TRUE;
        parameters.AutoDepthStencilFormat = D3DFMT_D24S8;
        parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        check(d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED,
            &parameters, nullptr, &device), "Create hidden fixture D3D9Ex device");
        check(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &output), "Get fixture output surface");
        check(device->CreateRenderTarget(320, 240, D3DFMT_A8R8G8B8,
            D3DMULTISAMPLE_NONE, 0, FALSE, &source, nullptr), "Create fixture source surface");
    }
};

std::vector<uint32_t> pixels(DeviceFixture& fixture, IDirect3DSurface9* surface, uint32_t width, uint32_t height) {
    ComPtr<IDirect3DSurface9> readback;
    check(fixture.device->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM, &readback, nullptr), "Create fixture readback surface");
    check(fixture.device->GetRenderTargetData(surface, readback.Get()), "Read back fixture GPU pixels");
    D3DLOCKED_RECT locked{};
    check(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "Lock fixture readback pixels");
    if (!locked.pBits || locked.Pitch < LONG(width * 4)) {
        readback->UnlockRect();
        throw std::runtime_error("Invalid fixture GPU readback pitch");
    }
    std::vector<uint32_t> result(size_t(width) * height);
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(result.data() + size_t(y) * width,
            static_cast<const uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch, size_t(width) * 4);
    check(readback->UnlockRect(), "Unlock fixture readback pixels");
    return result;
}
uint32_t pixel(const std::vector<uint32_t>& image, uint32_t width, uint32_t x, uint32_t y) {
    return image[size_t(y) * width + x];
}
std::vector<uint32_t> draw(DeviceFixture& fixture, VideoPresenter& presenter, const VideoSettings& settings,
    HWND window, double fps = 0, double milliseconds = 0) {
    presenter.draw(fixture.device.Get(), fixture.source.Get(), fixture.output.Get(), settings, fps, milliseconds, window);
    return pixels(fixture, fixture.output.Get(), 640, 360);
}
void fillSource(DeviceFixture& fixture, D3DCOLOR color) {
    check(fixture.device->ColorFill(fixture.source.Get(), nullptr, color), "Fill fixture source surface");
}
void verifyToneControls(DeviceFixture& fixture, VideoPresenter& presenter, HWND window) {
    fillSource(fixture, 0xff336699);
    VideoSettings settings{};
    settings.scaling = ScalingMode::Stretch;
    const auto identity = draw(fixture, presenter, settings, window);
    const auto original = pixel(identity, 640, 320, 180);
    require(nearByte(red(original), 0x33) && nearByte(green(original), 0x66) && nearByte(blue(original), 0x99),
        "Default video presentation changed source pixels");

    settings.brightness = .1f;
    auto image = draw(fixture, presenter, settings, window);
    auto sample = pixel(image, 640, 320, 180);
    require(nearByte(red(sample), 77) && nearByte(green(sample), 128) && nearByte(blue(sample), 179),
        "Brightness shader control did not alter actual GPU pixels as authored");

    settings = VideoSettings{}; settings.scaling = ScalingMode::Stretch; settings.contrast = 1.5f;
    image = draw(fixture, presenter, settings, window); sample = pixel(image, 640, 320, 180);
    require(nearByte(red(sample), 13) && nearByte(green(sample), 89) && nearByte(blue(sample), 166),
        "Contrast shader control did not alter actual GPU pixels as authored");

    settings = VideoSettings{}; settings.scaling = ScalingMode::Stretch; settings.gamma = 2;
    image = draw(fixture, presenter, settings, window); sample = pixel(image, 640, 320, 180);
    require(nearByte(red(sample), 114) && nearByte(green(sample), 161) && nearByte(blue(sample), 198),
        "Gamma shader control did not alter actual GPU pixels as authored");

    settings = VideoSettings{}; settings.scaling = ScalingMode::Stretch; settings.saturation = 0;
    image = draw(fixture, presenter, settings, window); sample = pixel(image, 640, 320, 180);
    require(std::abs(int(red(sample)) - int(green(sample))) <= 2 &&
        std::abs(int(green(sample)) - int(blue(sample))) <= 2 && nearByte(red(sample), 93),
        "Zero saturation did not produce the shader's luminance grayscale");
}

void verifySharpnessAndFxaa(DeviceFixture& fixture, VideoPresenter& presenter, HWND window) {
    fillSource(fixture, 0xff404040);
    const RECT highlight{150, 119, 152, 121};
    check(fixture.device->ColorFill(fixture.source.Get(), &highlight, 0xff808080), "Add sharpness test feature");
    VideoSettings settings{}; settings.scaling = ScalingMode::Stretch;
    const auto unsharp = draw(fixture, presenter, settings, window);
    settings.sharpness = .5f;
    const auto sharpened = draw(fixture, presenter, settings, window);
    const auto baseCenter = red(pixel(unsharp, 640, 301, 179));
    const auto sharpCenter = red(pixel(sharpened, 640, 301, 179));
    require(sharpCenter > baseCenter + 8,
        "Sharpness control did not increase a real high-frequency feature in GPU output");

    // An irregular diagonal edge supplies a deterministic neighborhood with
    // both horizontal and vertical gradients to the FXAA shader.
    fillSource(fixture, 0xff101010);
    for (LONG y = 0; y < 240; ++y) {
        const LONG edge = 35 + (y * 5) / 4;
        if (edge < 320) {
            const RECT row{edge, y, 320, y + 1};
            check(fixture.device->ColorFill(fixture.source.Get(), &row, 0xfff0f0f0), "Draw FXAA diagonal feature");
        }
    }
    settings = VideoSettings{}; settings.scaling = ScalingMode::Stretch;
    const auto aliased = draw(fixture, presenter, settings, window);
    settings.fxaa = true;
    const auto antialiased = draw(fixture, presenter, settings, window);
    size_t changed = 0;
    for (size_t index = 0; index < aliased.size(); ++index) {
        const auto a = aliased[index], b = antialiased[index];
        if (std::abs(int(red(a)) - int(red(b))) > 2 || std::abs(int(green(a)) - int(green(b))) > 2 ||
            std::abs(int(blue(a)) - int(blue(b))) > 2) ++changed;
    }
    require(changed > 8, "FXAA shader control did not change pixels around a synthetic aliased edge");
}

void verifyAspectAndFps(DeviceFixture& fixture, VideoPresenter& presenter, HWND window) {
    fillSource(fixture, 0xff336699);
    VideoSettings settings{};
    const auto rect = presenter.draw(fixture.device.Get(), fixture.source.Get(), fixture.output.Get(), settings, 0, 0, window);
    require(rect == VideoContentRect{80, 0, 480, 360}, "Preserve-aspect output rectangle is not centered 4:3 content");
    const auto image = pixels(fixture, fixture.output.Get(), 640, 360);
    const auto bar = pixel(image, 640, 20, 180), content = pixel(image, 640, 320, 180);
    require(red(bar) <= 1 && green(bar) <= 1 && blue(bar) <= 1,
        "Preserve-aspect mode did not clear the side bar to black");
    require(nearByte(red(content), 0x33) && nearByte(green(content), 0x66) && nearByte(blue(content), 0x99),
        "Preserve-aspect content was not scaled into its computed rectangle");

    fillSource(fixture, 0xff080808);
    settings.showFps = true;
    const auto overlay = draw(fixture, presenter, settings, window, 61.0, 16.4);
    size_t bright = 0;
    for (uint32_t y = 14; y < 60; ++y) for (uint32_t x = 14; x < 230; ++x) {
        const auto value = pixel(overlay, 640, x, y);
        bright += red(value) > 150 && green(value) > 150 && blue(value) > 150 ? 1u : 0u;
    }
    require(bright > 25, "FPS option did not draw visible text into actual GPU output pixels");
}

void verifyAliasedSourceAndOutput(DeviceFixture& fixture, VideoPresenter& presenter, HWND window) {
    // The client is deliberately taller than the fixed backbuffer. The
    // presenter must snapshot the backbuffer before clearing its letterbox;
    // both the direct copy path and the post-process path consume that alias.
    require(SetWindowPos(window, nullptr, 0, 0, 640, 400, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
        "Cannot resize hidden aliasing fixture client");
    RECT client{};
    require(GetClientRect(window, &client) && client.right == 640 && client.bottom == 400,
        "Aliasing fixture client no longer differs from its 640x360 backbuffer");

    VideoContentRect actualRect{};
    auto renderAliased = [&](const VideoSettings& settings) {
        actualRect = presenter.draw(fixture.device.Get(), fixture.output.Get(), fixture.output.Get(), settings, 0, 0, window);
        return pixels(fixture, fixture.output.Get(), 640, 360);
    };
    const VideoSettings original{};
    check(fixture.device->ColorFill(fixture.output.Get(), nullptr, 0xff336699),
        "Seed aliased backbuffer before direct presentation");
    const auto direct = renderAliased(original);
    require(actualRect == VideoContentRect{0, 18, 640, 324},
        "Aliased direct path did not map the taller client to the backbuffer aspect");
    const auto directTopBar = pixel(direct, 640, 320, 8), directBottomBar = pixel(direct, 640, 320, 350);
    require(red(directTopBar) <= 1 && green(directTopBar) <= 1 && blue(directTopBar) <= 1 &&
        red(directBottomBar) <= 1 && green(directBottomBar) <= 1 && blue(directBottomBar) <= 1,
        "Aliased direct path did not clear both client-aspect bars");
    const auto directContent = pixel(direct, 640, 320, 180);
    require(nearByte(red(directContent), 0x33) && nearByte(green(directContent), 0x66) &&
        nearByte(blue(directContent), 0x99),
        "Clearing aliased direct-output bars erased the original backbuffer image");

    VideoSettings adjusted = original;
    adjusted.brightness = .1f;
    check(fixture.device->ColorFill(fixture.output.Get(), nullptr, 0xff336699),
        "Seed aliased backbuffer before brightness presentation");
    const auto effected = renderAliased(adjusted);
    require(actualRect == VideoContentRect{0, 18, 640, 324},
        "Aliased effects path did not map the taller client to the backbuffer aspect");
    const auto effectTopBar = pixel(effected, 640, 320, 8), effectBottomBar = pixel(effected, 640, 320, 350);
    require(red(effectTopBar) <= 1 && green(effectTopBar) <= 1 && blue(effectTopBar) <= 1 &&
        red(effectBottomBar) <= 1 && green(effectBottomBar) <= 1 && blue(effectBottomBar) <= 1,
        "Aliased effects path did not preserve black client-aspect bars");
    const auto effectContent = pixel(effected, 640, 320, 180);
    require(nearByte(red(effectContent), 77) && nearByte(green(effectContent), 128) &&
        nearByte(blue(effectContent), 179),
        "Effects path did not snapshot aliased input before clearing output bars");
    require(SetWindowPos(window, nullptr, 0, 0, 640, 360, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
        "Cannot restore hidden aliasing fixture client size");
}

struct SavedStateProbe {
    ComPtr<IDirect3DSurface9> target0, target1, depth;
    ComPtr<IDirect3DTexture9> texture;
    ComPtr<IDirect3DVertexShader9> vertexShader;
    ComPtr<IDirect3DPixelShader9> pixelShader;
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    ComPtr<IDirect3DVertexBuffer9> vertexBuffer;
    ComPtr<IDirect3DIndexBuffer9> indexBuffer;
    D3DVIEWPORT9 viewport{};
    DWORD renderStates[5]{};
    DWORD sampler0[5]{}, sampler5[3]{}, stageStates[3]{};
    std::array<float, 4> vertexConstants{}, pixelConstants{};
    bool hasTarget1 = false;
};
void saveAndCheckPresentationState(IDirect3DDevice9Ex* device, const SavedStateProbe& before) {
    ComPtr<IDirect3DSurface9> target0, target1, depth;
    check(device->GetRenderTarget(0, &target0), "Read restored presentation RT0");
    require(target0.Get() == before.target0.Get(), "Presenter failed to restore the prior render target");
    if (before.hasTarget1) {
        check(device->GetRenderTarget(1, &target1), "Read restored presentation RT1");
        require(target1.Get() == before.target1.Get(), "Presenter failed to restore a prior MRT target");
    }
    check(device->GetDepthStencilSurface(&depth), "Read restored presentation depth target");
    require(depth.Get() == before.depth.Get(), "Presenter failed to restore the prior depth target");
    D3DVIEWPORT9 viewport{}; check(device->GetViewport(&viewport), "Read restored presentation viewport");
    require(std::memcmp(&viewport, &before.viewport, sizeof(viewport)) == 0,
        "Presenter failed to restore the prior viewport");
    const std::array<D3DRENDERSTATETYPE, 5> stateIds{
        D3DRS_ZENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE};
    for (size_t i = 0; i < stateIds.size(); ++i) {
        DWORD value = 0; check(device->GetRenderState(stateIds[i], &value), "Read restored presentation render state");
        require(value == before.renderStates[i], "Presenter failed to restore a prior render state");
    }
    const std::array<D3DSAMPLERSTATETYPE, 5> sampler0Ids{
        D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_MIPMAPLODBIAS};
    for (size_t i = 0; i < sampler0Ids.size(); ++i) {
        DWORD value = 0; check(device->GetSamplerState(0, sampler0Ids[i], &value), "Read restored sampler 0 state");
        require(value == before.sampler0[i], "Presenter failed to restore game sampler 0 state");
    }
    const std::array<D3DSAMPLERSTATETYPE, 3> sampler5Ids{D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER};
    for (size_t i = 0; i < sampler5Ids.size(); ++i) {
        DWORD value = 0; check(device->GetSamplerState(5, sampler5Ids[i], &value), "Read restored shadow sampler state");
        require(value == before.sampler5[i], "Presenter failed to restore point-filtered shadow sampler state");
    }
    const std::array<D3DTEXTURESTAGESTATETYPE, 3> stageIds{D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP};
    for (size_t i = 0; i < stageIds.size(); ++i) {
        DWORD value = 0; check(device->GetTextureStageState(0, stageIds[i], &value), "Read restored texture-stage state");
        require(value == before.stageStates[i], "Presenter failed to restore texture-stage state");
    }
    ComPtr<IDirect3DBaseTexture9> texture; check(device->GetTexture(0, &texture), "Read restored texture binding");
    require(texture.Get() == before.texture.Get(), "Presenter failed to restore the prior texture binding");
    ComPtr<IDirect3DVertexShader9> vertex; check(device->GetVertexShader(&vertex), "Read restored vertex shader");
    ComPtr<IDirect3DPixelShader9> pixelShader; check(device->GetPixelShader(&pixelShader), "Read restored pixel shader");
    ComPtr<IDirect3DVertexDeclaration9> declaration; check(device->GetVertexDeclaration(&declaration), "Read restored vertex declaration");
    require(vertex.Get() == before.vertexShader.Get() && pixelShader.Get() == before.pixelShader.Get() &&
        declaration.Get() == before.declaration.Get(), "Presenter failed to restore native shaders or vertex declaration");
    ComPtr<IDirect3DVertexBuffer9> vertexBuffer; UINT offset = 0, stride = 0;
    check(device->GetStreamSource(0, &vertexBuffer, &offset, &stride), "Read restored stream source");
    require(vertexBuffer.Get() == before.vertexBuffer.Get(), "Presenter failed to restore the prior vertex stream");
    ComPtr<IDirect3DIndexBuffer9> indexBuffer; check(device->GetIndices(&indexBuffer), "Read restored index binding");
    require(indexBuffer.Get() == before.indexBuffer.Get(), "Presenter failed to restore the prior index buffer");
    std::array<float, 4> vs{}, ps{};
    check(device->GetVertexShaderConstantF(3, vs.data(), 1), "Read restored vertex shader constants");
    check(device->GetPixelShaderConstantF(2, ps.data(), 1), "Read restored pixel shader constants");
    require(vs == before.vertexConstants && ps == before.pixelConstants,
        "Presenter failed to restore vertex/pixel shader constants");
}

std::vector<uint8_t> compile(const char* source, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), "video_reset_fixture", nullptr, nullptr,
        "main", profile, D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY, 0, &code, &errors);
    if (FAILED(result)) throw std::runtime_error(errors ?
        std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "Cannot compile reset fixture shader");
    const auto* begin = static_cast<const uint8_t*>(code->GetBufferPointer());
    return {begin, begin + code->GetBufferSize()};
}
void appendWord(std::vector<uint8_t>& data, uint32_t value, bool bigEndian) {
    for (int byte = 0; byte < 4; ++byte)
        data.push_back(uint8_t(value >> ((bigEndian ? 3 - byte : byte) * 8)));
}
struct CacheShader { const char* name; bool vertex; std::vector<uint8_t> code; };
std::vector<uint8_t> shaderCache(const std::array<CacheShader, 2>& shaders, bool bigEndian) {
    std::vector<uint8_t> data;
    appendWord(data, 3, bigEndian); appendWord(data, 0, bigEndian); appendWord(data, uint32_t(shaders.size()), bigEndian);
    for (const auto& shader : shaders) {
        const size_t length = std::strlen(shader.name);
        appendWord(data, uint32_t(length + 1), bigEndian);
        data.insert(data.end(), shader.name, shader.name + length + 1);
        appendWord(data, 1, bigEndian); // one feature key
        appendWord(data, 0, bigEndian); appendWord(data, 0, bigEndian); appendWord(data, 0, bigEndian);
        appendWord(data, 0, bigEndian); // no reflected parameters
        appendWord(data, uint32_t(shader.code.size()), bigEndian);
        data.insert(data.end(), shader.code.begin(), shader.code.end());
    }
    return data;
}

void verifyPresenterState(DeviceFixture& fixture, VideoPresenter& presenter, HWND window) {
    auto* device = fixture.device.Get();
    D3DCAPS9 caps{}; check(device->GetDeviceCaps(&caps), "Read video state fixture caps");
    SavedStateProbe before{};
    check(device->CreateRenderTarget(640, 360, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
        &before.target0, nullptr), "Create retained fixture RT0");
    if (caps.NumSimultaneousRTs >= 2) {
        check(device->CreateRenderTarget(640, 360, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
            &before.target1, nullptr), "Create retained fixture MRT");
        before.hasTarget1 = true;
    }
    check(device->CreateDepthStencilSurface(640, 360, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE,
        &before.depth, nullptr), "Create retained fixture depth target");
    check(device->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &before.texture, nullptr), "Create retained fixture texture");
    check(device->CreateVertexShader(reinterpret_cast<const DWORD*>(compile(
        "float4 main(float3 p:POSITION0):POSITION0{return float4(p,1);}", "vs_3_0").data()),
        &before.vertexShader), "Create retained fixture vertex shader");
    const auto pixelCode = compile("float4 main():COLOR0{return float4(.2,.4,.6,1);}", "ps_3_0");
    check(device->CreatePixelShader(reinterpret_cast<const DWORD*>(pixelCode.data()), &before.pixelShader),
        "Create retained fixture pixel shader");
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    check(device->CreateVertexDeclaration(elements, &before.declaration), "Create retained fixture declaration");
    const std::array<float, 9> points{-0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0, 0.5f, 0.5f};
    check(device->CreateVertexBuffer(sizeof(points), 0, 0, D3DPOOL_DEFAULT, &before.vertexBuffer, nullptr),
        "Create retained fixture vertex buffer");
    void* locked = nullptr; check(before.vertexBuffer->Lock(0, sizeof(points), &locked, 0), "Lock fixture vertex buffer");
    std::memcpy(locked, points.data(), sizeof(points)); check(before.vertexBuffer->Unlock(), "Unlock fixture vertex buffer");
    const std::array<uint16_t, 3> indices{0, 1, 2};
    check(device->CreateIndexBuffer(sizeof(indices), 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT,
        &before.indexBuffer, nullptr), "Create retained fixture index buffer");
    check(before.indexBuffer->Lock(0, sizeof(indices), &locked, 0), "Lock fixture index buffer");
    std::memcpy(locked, indices.data(), sizeof(indices)); check(before.indexBuffer->Unlock(), "Unlock fixture index buffer");

    check(device->SetDepthStencilSurface(nullptr), "Detach default fixture depth");
    if (before.hasTarget1) check(device->SetRenderTarget(1, nullptr), "Clear fixture MRT slot");
    check(device->SetRenderTarget(0, before.target0.Get()), "Select retained fixture RT0");
    if (before.hasTarget1) check(device->SetRenderTarget(1, before.target1.Get()), "Select retained fixture MRT");
    check(device->SetDepthStencilSurface(before.depth.Get()), "Select retained fixture depth target");
    before.viewport = {17, 19, 521, 311, .125f, .875f};
    check(device->SetViewport(&before.viewport), "Set retained fixture viewport");
    const std::array<D3DRENDERSTATETYPE, 5> stateIds{
        D3DRS_ZENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE};
    const std::array<DWORD, 5> stateValues{D3DZB_TRUE, TRUE, D3DCULL_CW, TRUE, FALSE};
    for (size_t i = 0; i < stateIds.size(); ++i) {
        before.renderStates[i] = stateValues[i];
        check(device->SetRenderState(stateIds[i], stateValues[i]), "Set retained fixture render state");
    }
    const std::array<D3DSAMPLERSTATETYPE, 5> sampler0Ids{
        D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_MIPMAPLODBIAS};
    const std::array<DWORD, 5> sampler0Values{D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_POINT,
        D3DTADDRESS_WRAP, std::bit_cast<DWORD>(.75f)};
    for (size_t i = 0; i < sampler0Ids.size(); ++i) {
        before.sampler0[i] = sampler0Values[i];
        check(device->SetSamplerState(0, sampler0Ids[i], sampler0Values[i]), "Set retained fixture game sampler");
    }
    const std::array<D3DSAMPLERSTATETYPE, 3> sampler5Ids{D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER};
    for (size_t i = 0; i < sampler5Ids.size(); ++i) {
        before.sampler5[i] = D3DTEXF_POINT;
        check(device->SetSamplerState(5, sampler5Ids[i], D3DTEXF_POINT), "Set retained shadow point sampler");
    }
    const std::array<D3DTEXTURESTAGESTATETYPE, 3> stageIds{D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP};
    const std::array<DWORD, 3> stageValues{D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTOP_SELECTARG1};
    for (size_t i = 0; i < stageIds.size(); ++i) {
        before.stageStates[i] = stageValues[i];
        check(device->SetTextureStageState(0, stageIds[i], stageValues[i]), "Set retained fixture texture-stage state");
    }
    check(device->SetTexture(0, before.texture.Get()), "Set retained fixture texture");
    check(device->SetVertexDeclaration(before.declaration.Get()), "Set retained fixture declaration");
    check(device->SetVertexShader(before.vertexShader.Get()), "Set retained fixture vertex shader");
    check(device->SetPixelShader(before.pixelShader.Get()), "Set retained fixture pixel shader");
    check(device->SetStreamSource(0, before.vertexBuffer.Get(), 0, 12), "Set retained fixture vertex stream");
    check(device->SetIndices(before.indexBuffer.Get()), "Set retained fixture index buffer");
    before.vertexConstants = {.125f, .25f, .5f, .75f};
    before.pixelConstants = {.2f, .4f, .6f, .8f};
    check(device->SetVertexShaderConstantF(3, before.vertexConstants.data(), 1), "Set retained vertex constants");
    check(device->SetPixelShaderConstantF(2, before.pixelConstants.data(), 1), "Set retained pixel constants");

    fillSource(fixture, 0xff506070);
    VideoSettings settings{}; settings.scaling = ScalingMode::Stretch;
    settings.fxaa = true; settings.sharpness = .25f; settings.gamma = 1.1f; settings.showFps = true;
    draw(fixture, presenter, settings, window, 60, 16.7);
    saveAndCheckPresentationState(device, before);
}

void verifyBackendReset() {
    FixtureWindow resetWindow;
    D3D9Backend backend;
    backend.initialize(resetWindow.window, 320, 240);
    const auto renderSurface = backend.createSurface(320, 240, D3DFMT_A8R8G8B8, false, D3DMULTISAMPLE_NONE);
    const auto depthSurface = backend.createSurface(320, 240, D3DFMT_D24S8, true, D3DMULTISAMPLE_NONE);
    const auto renderTexture = backend.createTexture(320, 240, 1, D3DFMT_A8R8G8B8, false, true);
    const auto secondResolveTexture = backend.createTexture(320, 240, 1, D3DFMT_A8R8G8B8, false, true);
    const auto textureSurface = backend.textureSurface(renderTexture, 0, 0);
    const auto auxiliarySurface = backend.createSurface(96, 64, D3DFMT_A8R8G8B8, false, D3DMULTISAMPLE_NONE);
    const auto auxiliaryTexture = backend.createTexture(80, 48, 1, D3DFMT_A8R8G8B8, false, true);
    const SceneResolutionResources primaryResources{renderSurface, depthSurface, {renderTexture, secondResolveTexture}};
    uint32_t cpuSceneWidth = 320, cpuSceneHeight = 240;
    bool rejectNextSceneResolution = false;
    std::vector<std::pair<uint32_t,uint32_t>> cpuResolutionUpdates;
    backend.setSceneResolutionResources(primaryResources, [&](uint32_t width, uint32_t height) {
        cpuResolutionUpdates.emplace_back(width, height);
        cpuSceneWidth = width; cpuSceneHeight = height;
        if (rejectNextSceneResolution && width == 800 && height == 600) {
            rejectNextSceneResolution = false;
            throw std::runtime_error("fixture rejected CPU scene resize");
        }
    });
    require(backend.sceneResolution() == std::pair<uint32_t,uint32_t>{320,240} && cpuResolutionUpdates.empty(),
        "Registering actual primary scene resources preserves the initial scene dimensions");
    struct Vertex { float x, y, z; };
    const std::array<Vertex, 3> vertices{{{-0.6f, -0.6f, .5f}, {.6f, -.6f, .5f}, {0, .6f, .5f}}};
    const auto vertexBuffer = backend.createVertexBuffer(
        {reinterpret_cast<const uint8_t*>(vertices.data()), sizeof(vertices)}, sizeof(Vertex));
    const std::array<uint16_t, 3> indices{0, 1, 2};
    const auto indexBuffer = backend.createIndexBuffer(indices);
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    const auto declaration = backend.createDeclaration(elements);
    const std::array<CacheShader, 2> shaders{{
        {"native_video_fixture/vs", true, compile(
            "float4 main(float3 p:POSITION0):POSITION0{return float4(p,1);}", "vs_3_0")},
        {"native_video_fixture/ps", false, compile(
            "float4 main():COLOR0{return float4(.25,.5,.75,1);}", "ps_3_0")},
    }};
    const auto windowsCache = shaderCache(shaders, false), xenonCache = shaderCache(shaders, true);
    backend.loadCaches(windowsCache, xenonCache);
    const ShaderKey key{0, 0, 0};
    const auto vertexShader = backend.createShader("native_video_fixture/vs", key);
    const auto pixelShader = backend.createShader("native_video_fixture/ps", key);
    backend.bindRenderTarget(0, renderSurface);
    backend.bindDepthSurface(depthSurface);
    backend.setViewport({0, 0, 320, 240, 0, 1});
    backend.bindVertexBuffer(vertexBuffer);
    backend.bindIndexBuffer(indexBuffer);
    backend.bindDeclaration(declaration);
    backend.bindShader(ShaderStage::Vertex, vertexShader);
    backend.bindShader(ShaderStage::Pixel, pixelShader);

    const auto beforePreflightSettings = backend.appliedVideoSettings();
    const auto beforePreflightCounters = backend.counters();
    VideoSettings missingMonitor = beforePreflightSettings;
    missingMonitor.width = 320; missingMonitor.height = 240; missingMonitor.monitor = 63;
    require(validateVideoSettings(missingMonitor), "Missing-monitor fixture is not a valid settings request");
    std::string error;
    require(!backend.applyVideoSettings(missingMonitor, error) && !error.empty(),
        "Settings preflight accepted a valid request for a disconnected monitor");
    auto afterPreflightCounters = backend.counters();
    require(backend.appliedVideoSettings() == beforePreflightSettings &&
        afterPreflightCounters.width == beforePreflightCounters.width &&
        afterPreflightCounters.height == beforePreflightCounters.height &&
        afterPreflightCounters.presents == beforePreflightCounters.presents &&
        afterPreflightCounters.draws == beforePreflightCounters.draws,
        "Missing-monitor rejection mutated applied settings or renderer counters");
    VideoSettings invalidWidth = beforePreflightSettings;
    invalidWidth.width = 1;
    error.clear();
    require(!backend.applyVideoSettings(invalidWidth, error) && !error.empty(),
        "Settings preflight accepted an invalid output width");
    afterPreflightCounters = backend.counters();
    require(backend.appliedVideoSettings() == beforePreflightSettings &&
        afterPreflightCounters.width == beforePreflightCounters.width &&
        afterPreflightCounters.height == beforePreflightCounters.height &&
        afterPreflightCounters.presents == beforePreflightCounters.presents &&
        afterPreflightCounters.draws == beforePreflightCounters.draws,
        "Invalid-width rejection mutated applied settings or renderer counters");

    VideoSettings wrongThread = backend.appliedVideoSettings();
    wrongThread.vsync = VSyncMode::Off;
    bool wrongThreadAccepted = true;
    std::thread worker([&] {
        std::string error;
        wrongThreadAccepted = backend.applyVideoSettings(wrongThread, error);
    });
    worker.join();
    require(!wrongThreadAccepted, "Video settings ResetEx was accepted off the device creation thread");

    VideoSettings resized = backend.appliedVideoSettings();
    resized.width = 640; resized.height = 360; resized.vsync = VSyncMode::Off;
    resized.frameLimit = 0; resized.backgroundFrameLimit = 0;
    backend.bindTexture(5, renderTexture);
    backend.bindTexture(7, auxiliaryTexture);
    ComPtr<IDirect3DBaseTexture9> primaryIdentityBefore, auxiliaryIdentityBefore;
    backend.getSampledTexture(5, &primaryIdentityBefore);
    backend.getSampledTexture(7, &auxiliaryIdentityBefore);
    error.clear();
    require(backend.applyVideoSettings(resized, error) && error.empty(),
        "Backend failed a hidden 320x240 to 640x360 ResetEx preview");
    const auto counters = backend.counters();
    require(counters.width == 640 && counters.height == 360 && backend.appliedVideoSettings() == resized,
        "Backend did not publish the successful resized video mode");
    require(backend.sceneResolution() == std::pair<uint32_t,uint32_t>{640,360} &&
        cpuSceneWidth == 640 && cpuSceneHeight == 360 &&
        cpuResolutionUpdates == std::vector<std::pair<uint32_t,uint32_t>>{{640,360}},
        "Video preview did not update the primary scene and original CPU dimensions together");
    const auto retainedColor = backend.surfaceDescription(renderSurface);
    const auto retainedDepth = backend.surfaceDescription(depthSurface);
    const auto retainedTexture = backend.textureDescription(renderTexture, 0);
    const auto retainedSecondTexture = backend.textureDescription(secondResolveTexture, 0);
    require(retainedColor.Width == 640 && retainedColor.Height == 360 &&
        retainedDepth.Width == 640 && retainedDepth.Height == 360 &&
        retainedTexture.Width == 640 && retainedTexture.Height == 360 &&
        retainedSecondTexture.Width == 640 && retainedSecondTexture.Height == 360 &&
        backend.surfaceDescription(textureSurface).Width == 640 &&
        backend.surfaceDescription(textureSurface).Height == 360,
        "ResetEx did not resize all registered primary scene and level0 resolve resources behind stable handles");
    const auto retainedTextureSurface = backend.surfaceDescription(textureSurface);
    require(retainedTextureSurface.Width == 640 && retainedTextureSurface.Height == 360,
        "ResetEx did not preserve the original render texture surface handle at the new scene size");
    require(backend.surfaceDescription(auxiliarySurface).Width == 96 &&
        backend.surfaceDescription(auxiliarySurface).Height == 64 &&
        backend.textureDescription(auxiliaryTexture, 0).Width == 80 &&
        backend.textureDescription(auxiliaryTexture, 0).Height == 48,
        "Changing render resolution rescaled an auxiliary render target");
    ComPtr<IDirect3DBaseTexture9> primaryIdentityAfter, auxiliaryIdentityAfter;
    backend.getSampledTexture(5, &primaryIdentityAfter);
    backend.getSampledTexture(7, &auxiliaryIdentityAfter);
    require(primaryIdentityBefore.Get() != primaryIdentityAfter.Get() &&
        auxiliaryIdentityBefore.Get() == auxiliaryIdentityAfter.Get(),
        "Primary resolve COM identity did not change while unrelated auxiliary texture identity was retained");

    const auto countersBeforeRejectedSceneResize = backend.counters();
    rejectNextSceneResolution = true;
    VideoSettings rejectedSceneResize = resized;
    rejectedSceneResize.width = 800; rejectedSceneResize.height = 600;
    error.clear();
    require(!backend.applyVideoSettings(rejectedSceneResize, error) && !error.empty(),
        "A failed original CPU scene-resolution update was published as a successful mode");
    const auto countersAfterRejectedSceneResize = backend.counters();
    require(backend.sceneResolution() == std::pair<uint32_t,uint32_t>{640,360} &&
        backend.appliedVideoSettings() == resized && cpuSceneWidth == 640 && cpuSceneHeight == 360 &&
        countersAfterRejectedSceneResize.width == countersBeforeRejectedSceneResize.width &&
        countersAfterRejectedSceneResize.height == countersBeforeRejectedSceneResize.height &&
        countersAfterRejectedSceneResize.presents == countersBeforeRejectedSceneResize.presents &&
        countersAfterRejectedSceneResize.draws == countersBeforeRejectedSceneResize.draws,
        "Failed scene-resolution preview left CPU, renderer mode or counters partly committed");
    require(cpuResolutionUpdates.size() == 3 && cpuResolutionUpdates[1] == std::pair<uint32_t,uint32_t>{800,600} &&
        cpuResolutionUpdates[2] == std::pair<uint32_t,uint32_t>{640,360},
        "Failed CPU scene-resolution update did not invoke the original-dimensions rollback callback");
    ComPtr<IDirect3DBaseTexture9> primaryIdentityAfterRollback, auxiliaryIdentityAfterRollback;
    backend.getSampledTexture(5, &primaryIdentityAfterRollback);
    backend.getSampledTexture(7, &auxiliaryIdentityAfterRollback);
    require(primaryIdentityAfterRollback.Get() == primaryIdentityAfter.Get() &&
        auxiliaryIdentityAfterRollback.Get() == auxiliaryIdentityAfter.Get() &&
        backend.surfaceDescription(renderSurface).Width == 640 &&
        backend.surfaceDescription(renderSurface).Height == 360 &&
        backend.textureDescription(renderTexture,0).Width == 640 &&
        backend.textureDescription(renderTexture,0).Height == 360,
        "Failed scene-resolution preview did not restore primary and auxiliary COM references");
    const std::array<Vertex, 3> updatedVertices{{{-.5f, -.5f, .5f}, {.5f, -.5f, .5f}, {0, .5f, .5f}}};
    backend.updateVertexBuffer(vertexBuffer,
        {reinterpret_cast<const uint8_t*>(updatedVertices.data()), sizeof(updatedVertices)});
    backend.updateIndexBuffer(indexBuffer, indices);
    backend.bindRenderTarget(0, renderSurface);
    backend.bindDepthSurface(depthSurface);
    backend.setViewport({0, 0, 640, 360, 0, 1});
    backend.bindVertexBuffer(vertexBuffer); backend.bindIndexBuffer(indexBuffer); backend.bindDeclaration(declaration);
    backend.bindShader(ShaderStage::Vertex, vertexShader); backend.bindShader(ShaderStage::Pixel, pixelShader);
    backend.beginFrame();
    backend.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff102030, 1, 0);
    backend.drawIndexedTriangles(0, 3, 0, 1);
    backend.endFrame();
    backend.resolveRenderTarget(renderTexture, renderSurface, D3DMULTISAMPLE_NONE);
    backend.selectPresentationTexture(renderTexture);
    backend.present();
    require(backend.counters().presents == 1,
        "Retained RT, texture, vertex/index buffers, declaration and shaders were unusable after ResetEx");
}

void verifyRectMath() {
    require(videoContentRect(640, 360, 320, 240, ScalingMode::PreserveAspect) == VideoContentRect{80, 0, 480, 360},
        "Pure preserve-aspect calculation lost 4:3 content centering");
    require(videoContentRect(640, 360, 320, 240, ScalingMode::Stretch) == VideoContentRect{0, 0, 640, 360},
        "Pure stretch calculation did not fill the output");
    require(videoContentRect(0, 360, 320, 240, ScalingMode::PreserveAspect) == VideoContentRect{},
        "Pure video geometry accepted a zero-sized output");
}
}

int main() {
    FixtureWindow window;
    try {
        verifyRectMath();
        DeviceFixture fixture(window.window);
        VideoPresenter presenter;
        verifyToneControls(fixture, presenter, window.window);
        verifySharpnessAndFxaa(fixture, presenter, window.window);
        verifyAspectAndFps(fixture, presenter, window.window);
        verifyAliasedSourceAndOutput(fixture, presenter, window.window);
        verifyPresenterState(fixture, presenter, window.window);
        verifyBackendReset();
        std::printf("Native video renderer contract: %u checks passed; hidden synthetic D3D9 fixtures only\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native video renderer contract failed after %u checks: %s\n", checks, error.what());
        return 1;
    }
}
