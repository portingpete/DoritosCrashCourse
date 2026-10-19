// Synthetic adapter contract verification. A separate hidden prompt fixture
// draws textured quads to offscreen readback targets; no game frame is presented.
#include "renderer/native/d3d9_backend.h"
#include "renderer/native/capture_request.h"
#include "renderer/native/title_input_prompts.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <wrl/client.h>

using namespace Doritos::Graphics;
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Action>
static void rejects(Action action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected,message);
}
static void verifyTitleInputPromptNames() {
    // These are independently recorded from shipped texture.wbr master names
    // and shader.wbr LayersDiffuseName requests. In particular, the primary
    // GUI resources keep both filenames in their registered name at master+8.
    struct NameCase { const char* registered; const char* color; const char* mask; InputPromptKind kind; };
    const std::array<NameCase,7> names{{
        {"gui/xbox_buttons.jpg;xbox_buttons_a.jpg","gui/xbox_buttons.jpg","gui/xbox_buttons_a.jpg",InputPromptKind::ButtonsAtlas},
        {"gui/controls.jpg;controls_a.jpg","gui/controls.jpg","gui/controls_a.jpg",InputPromptKind::ControlsDiagram},
        {"gui/button_lb.jpg;button_lb_a.jpg","gui/button_lb.jpg","gui/button_lb_a.jpg",InputPromptKind::Q},
        {"gui/button_rb.jpg;button_rb_a.jpg","gui/button_rb.jpg","gui/button_rb_a.jpg",InputPromptKind::E},
        {"gui/button_lt.jpg;button_lt_a.jpg","gui/button_lt.jpg","gui/button_lt_a.jpg",InputPromptKind::Shift},
        {"gui/button_rt.jpg;button_rt_a.jpg","gui/button_rt.jpg","gui/button_rt_a.jpg",InputPromptKind::Ctrl},
        {"gui/button_start.jpg;button_start_a.jpg","gui/button_start.jpg","gui/button_start_a.jpg",InputPromptKind::Pause},
    }};
    for (const auto& name : names) {
        const auto registered = titleInputPrompt(name.registered);
        const auto color = titleInputPrompt(name.color);
        const auto mask = titleInputPrompt(name.mask);
        require(registered.kind == name.kind && !registered.alphaMask,
            "Shipped combined TEXTUREMASTER filename did not resolve to its color prompt");
        require(color.kind == name.kind && !color.alphaMask && mask.kind == name.kind && mask.alphaMask,
            "Explicit separate prompt color/mask filenames lost their distinct opacity contract");
    }
    for (const auto name : {"", "gui/xbox_buttons", "gui/controls", "buttona.jpg", "gui/button_a.jpg",
            "fx/targe_usa.jpg;button_a.jpg", "gui/xbox_buttons.jpg;xbox_buttons_a.jpg.extra",
            "data/art/gui/xbox_buttons.jpg", "GUI/XBOX_BUTTONS.JPG", "gui/button_lb.jpg;button_rt_a.jpg",
            "gui/xbox_buttons/a.jpg;a_a.jpg", "_avatar/cj_native_0.png"})
        require(titleInputPrompt(name).kind == InputPromptKind::None,
            "Nonprompt/prefix/shader/unsupported family name was mistaken for an exact shipped prompt texture");
}
static Microsoft::WRL::ComPtr<IDirect3DBaseTexture9> sampledTexture(D3D9Backend& backend, uint32_t sampler) {
    Microsoft::WRL::ComPtr<IDirect3DBaseTexture9> texture;
    backend.getSampledTexture(sampler,&texture);
    return texture;
}
static std::vector<uint32_t> renderSampledPixels(IDirect3DBaseTexture9* sampled, uint32_t width, uint32_t height) {
    using Microsoft::WRL::ComPtr;
    require(sampled != nullptr,"Actual input-prompt sampler is empty");
    ComPtr<IDirect3DDevice9> device;
    ComPtr<IDirect3DTexture9> texture;
    require(SUCCEEDED(sampled->GetDevice(&device)) && SUCCEEDED(sampled->QueryInterface(IID_PPV_ARGS(&texture))),
        "Cannot inspect actual input-prompt sampler device/texture");
    D3DSURFACE_DESC description{};
    require(SUCCEEDED(texture->GetLevelDesc(0,&description)) && description.Width == width && description.Height == height,
        "Actual input-prompt sampling dimensions changed");
    ComPtr<IDirect3DStateBlock9> saved;
    ComPtr<IDirect3DSurface9> priorColor, priorDepth, target, readback;
    D3DVIEWPORT9 priorViewport{};
    require(SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL,&saved)) && SUCCEEDED(saved->Capture()) &&
        SUCCEEDED(device->GetRenderTarget(0,&priorColor)) && SUCCEEDED(device->GetViewport(&priorViewport)),
        "Cannot retain actual prompt-fixture state");
    const HRESULT depthResult = device->GetDepthStencilSurface(&priorDepth);
    require(SUCCEEDED(depthResult) || depthResult == D3DERR_NOTFOUND,"Cannot retain prompt-fixture depth state");
    require(SUCCEEDED(device->CreateRenderTarget(width,height,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,nullptr)) &&
        SUCCEEDED(device->CreateOffscreenPlainSurface(width,height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr)),
        "Cannot allocate actual prompt-fixture render/readback surfaces");
    require(SUCCEEDED(device->SetDepthStencilSurface(nullptr)) && SUCCEEDED(device->SetRenderTarget(0,target.Get())),
        "Cannot select actual prompt-fixture render target");
    const D3DVIEWPORT9 viewport{0,0,width,height,0,1};
    require(SUCCEEDED(device->SetViewport(&viewport)) && SUCCEEDED(device->SetVertexShader(nullptr)) &&
        SUCCEEDED(device->SetPixelShader(nullptr)) && SUCCEEDED(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1)),
        "Cannot select actual prompt-fixture sampling pipeline");
    for (const auto state : {D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_LIGHTING,D3DRS_ALPHABLENDENABLE,
            D3DRS_ALPHATESTENABLE,D3DRS_FOGENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,
            D3DRS_DITHERENABLE,D3DRS_CLIPPLANEENABLE})
        require(SUCCEEDED(device->SetRenderState(state,0)),"Cannot disable unrelated prompt-fixture render effects");
    require(SUCCEEDED(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE,15)),"Cannot select exact prompt-fixture color output");
    require(SUCCEEDED(device->SetTexture(0,sampled)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE)) &&
        SUCCEEDED(device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE)),
        "Cannot select exact actual prompt texture/alpha sampling");
    for (const auto state : {D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})
        require(SUCCEEDED(device->SetSamplerState(0,state,D3DTEXF_POINT)),"Cannot point-sample original prompt pixels");
    require(SUCCEEDED(device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE)) &&
        SUCCEEDED(device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP)) &&
        SUCCEEDED(device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP)) &&
        SUCCEEDED(device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,0)),"Cannot retain exact prompt-fixture sampling coordinates");
    struct Vertex { float x,y,z,rhw,u,v; };
    const std::array<Vertex,4> vertices{{
        {-0.5f,-0.5f,0,1,0,0}, {float(width)-0.5f,-0.5f,0,1,1,0},
        {-0.5f,float(height)-0.5f,0,1,0,1}, {float(width)-0.5f,float(height)-0.5f,0,1,1,1},
    }};
    require(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0)) && SUCCEEDED(device->BeginScene()),
        "Cannot begin actual prompt-fixture render");
    const HRESULT drawResult = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices.data(),sizeof(Vertex));
    const HRESULT endResult = device->EndScene();
    require(SUCCEEDED(drawResult) && SUCCEEDED(endResult) && SUCCEEDED(device->GetRenderTargetData(target.Get(),readback.Get())),
        "Actual prompt fixture draw/readback failed");
    require(SUCCEEDED(device->SetRenderTarget(0,priorColor.Get())) &&
        SUCCEEDED(device->SetDepthStencilSurface(priorDepth.Get())) && SUCCEEDED(saved->Apply()) &&
        SUCCEEDED(device->SetViewport(&priorViewport)),"Cannot restore actual prompt-fixture native state");
    D3DLOCKED_RECT locked{};
    require(SUCCEEDED(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY)) &&
        locked.Pitch >= 0 && uint32_t(locked.Pitch) >= width * 4,"Cannot read actual rendered prompt pixels");
    std::vector<uint32_t> pixels(size_t(width) * height);
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(pixels.data() + size_t(row) * width,
            static_cast<const uint8_t*>(locked.pBits) + size_t(row) * locked.Pitch,size_t(width) * 4);
    require(SUCCEEDED(readback->UnlockRect()),"Cannot finish actual rendered prompt readback");
    return pixels;
}
static uint64_t alphaHash(std::span<const uint32_t> pixels) {
    uint64_t digest = 0xcbf29ce484222325ULL;
    for (const uint32_t pixel : pixels) digest = (digest ^ (pixel >> 24)) * 0x100000001b3ULL;
    return digest;
}
static uint64_t visibleColorHash(std::span<const uint32_t> pixels) {
    uint64_t digest = 0xcbf29ce484222325ULL;
    // Invisible RGB in paletted transparent PNG entries is not displayed;
    // normalize it without changing any visible color or alpha sample.
    for (const uint32_t pixel : pixels) digest = (digest ^ ((pixel >> 24) ? pixel : 0u)) * 0x100000001b3ULL;
    return digest;
}
static void verifyPromptPixels(const std::vector<uint32_t>& pixels, uint64_t expectedAlpha, uint64_t expectedColor) {
    // Independent hashes were calculated from Pillow's checked PNG decoder,
    // not from D3DX or the production replacement/composition functions.
    // Filled keycaps preserve transparent label cutouts; filled mice retain
    // Kenney's red active-button highlights rather than recoloring them.
    require(alphaHash(pixels) == expectedAlpha,"Actual rendered Kenney prompt alpha differs from the checked PNG oracle");
    require(visibleColorHash(pixels) == expectedColor,"Actual rendered Kenney prompt colors differ from the checked filled PNG oracle");
    uint32_t visible = 0;
    for (const uint32_t pixel : pixels) visible += (pixel >> 24) ? 1u : 0u;
    require(visible > 500,"Actual rendered Kenney prompt is blank or truncated");
}
static uint32_t sentinelTexture(D3D9Backend& backend, uint32_t width, uint32_t height, uint32_t levels = 2) {
    const auto handle = backend.createTexture(width,height,levels,D3DFMT_A8R8G8B8,false,false);
    const auto surface = backend.textureSurface(handle,0,0);
    const std::vector<uint32_t> original(size_t(width) * height,0x8f214365u);
    backend.uploadSurface(surface,D3DFMT_A8R8G8B8,width * 4,
        {reinterpret_cast<const uint8_t*>(original.data()),original.size() * 4});
    backend.release(surface);
    backend.generateMipmaps(handle);
    return handle;
}
static void verifyOriginalPixels(const std::vector<uint32_t>& pixels) {
    for (const uint32_t pixel : pixels)
        require(pixel == 0x8f214365u,"Input-device switch modified original texture pixels or alpha");
}
static void verifyInputPrompts(HWND window) {
    D3D9Backend backend;
    backend.initialize(window,2560,1440);
    const auto key = sentinelTexture(backend,64,64);
    const auto ordinary = sentinelTexture(backend,64,64);
    backend.bindTexture(9,key);
    backend.bindTexture(10,ordinary);
    const auto original = sampledTexture(backend,9);
    const auto ordinaryOriginal = sampledTexture(backend,10);
    verifyOriginalPixels(renderSampledPixels(original.Get(),64,64));
    backend.setInputPrompt(key,InputPromptKind::Space);
    require(sampledTexture(backend,9).Get() == original.Get(),"Tagging a prompt changed the active gamepad image");
    backend.setKeyboardMousePrompts(true);
    const auto replacement = sampledTexture(backend,9);
    require(replacement && replacement.Get() != original.Get(),"Keyboard mode did not refresh an already bound prompt");
    const auto keyPixels = renderSampledPixels(replacement.Get(),64,64);
    verifyPromptPixels(keyPixels,0x4d8e6e49d7a6a0c4ULL,0xfd31593434721a44ULL);
    backend.setKeyboardMousePrompts(true);
    backend.setInputPrompt(key,InputPromptKind::Space);
    require(sampledTexture(backend,9).Get() == replacement.Get(),"Idempotent prompt updates recreated the actual texture");
    const auto metadata = backend.textureDescription(key,0);
    require(metadata.Width == 64 && metadata.Height == 64 && metadata.Format == D3DFMT_A8R8G8B8 &&
        metadata.Pool == D3DPOOL_DEFAULT && backend.textureLevelCount(key) == 2,
        "Prompt replacement changed the original native resource metadata");
    rejects([&] { backend.getSampledTexture(16,nullptr); },"Unbounded/null actual sampler diagnostic accepted");
    backend.setKeyboardMousePrompts(false);
    require(sampledTexture(backend,9).Get() == original.Get(),"Gamepad switch did not restore the exact original COM texture");
    verifyOriginalPixels(renderSampledPixels(sampledTexture(backend,9).Get(),64,64));
    backend.setKeyboardMousePrompts(true);
    require(sampledTexture(backend,10).Get() == ordinaryOriginal.Get(),"Device switch replaced an unrelated native texture");
    verifyOriginalPixels(renderSampledPixels(sampledTexture(backend,10).Get(),64,64));
    backend.release(key);
    backend.setKeyboardMousePrompts(false);
    require(sampledTexture(backend,9).Get() == original.Get(),"Released prompt token lost its retained gamepad texture");
    verifyOriginalPixels(renderSampledPixels(sampledTexture(backend,9).Get(),64,64));
    backend.setKeyboardMousePrompts(true);
    require(sampledTexture(backend,9).Get() == replacement.Get(),"Released prompt token lost its retained keyboard texture");
    verifyPromptPixels(renderSampledPixels(sampledTexture(backend,9).Get(),64,64),
        0x4d8e6e49d7a6a0c4ULL,0xfd31593434721a44ULL);
    rejects([&] { backend.bindTexture(9,key); },"Released prompt token remained bindable");
    backend.bindTexture(9,0);
    require(!sampledTexture(backend,9),"Null native bind retained a prompt sampler");

    const auto atlas = sentinelTexture(backend,512,256,4);
    backend.bindTexture(9,atlas);
    backend.setInputPrompt(atlas,InputPromptKind::ButtonsAtlas);
    const auto atlasPixels = renderSampledPixels(sampledTexture(backend,9).Get(),512,256);
    verifyPromptPixels(atlasPixels,0xa681e3dbefe0957aULL,0xf9c676916b02867aULL);
    const std::array<uint64_t,4> slotHashes{0x4d8e6e49d7a6a0c4ULL,0x13e5729ff470f32eULL,
        0x8e2fd6a23c51c52aULL,0x3383d8661cd491afULL};
    const std::array<uint64_t,4> slotColors{0xfd31593434721a44ULL,0x13bcf5aeaf34f53eULL,
        0xae7f8356e5a8f0daULL,0x965e55b133b0962fULL};
    for (uint32_t slot = 0; slot < 32; ++slot) {
        std::vector<uint32_t> cell;
        cell.reserve(64 * 64);
        for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x)
            cell.push_back(atlasPixels[size_t(slot / 8 * 64 + y) * 512 + slot % 8 * 64 + x]);
        if (slot < 4) verifyPromptPixels(cell,slotHashes[slot],slotColors[slot]);
        else for (const auto pixel : cell) require(!(pixel >> 24),"Unused original button-atlas slot became visible");
    }
    backend.setInputPrompt(atlas,InputPromptKind::None);
    verifyOriginalPixels(renderSampledPixels(sampledTexture(backend,9).Get(),512,256));
    backend.bindTexture(9,0); backend.release(atlas);

    const auto guide = sentinelTexture(backend,1024,256,4);
    backend.bindTexture(9,guide);
    backend.setInputPrompt(guide,InputPromptKind::ControlsDiagram);
    const auto guidePixels = renderSampledPixels(sampledTexture(backend,9).Get(),1024,256);
    verifyPromptPixels(guidePixels,0x6157485d8fe9fc18ULL,0x683c9cb90036520eULL);
    for (uint32_t tile = 0; tile < 8; ++tile) {
        uint32_t visible = 0;
        for (uint32_t y = 0; y < 128; ++y) for (uint32_t x = 0; x < 256; ++x)
            visible += guidePixels[size_t(tile / 4 * 128 + y) * 1024 + tile % 4 * 256 + x] >> 24 ? 1u : 0u;
        require(visible > 1000,"Actual control guide lost one of its eight labeled keyboard/mouse tiles");
    }
    backend.setKeyboardMousePrompts(false);
    verifyOriginalPixels(renderSampledPixels(sampledTexture(backend,9).Get(),1024,256));
    backend.bindTexture(9,0); backend.release(guide);

    const auto mask = sentinelTexture(backend,64,64);
    backend.bindTexture(9,mask);
    backend.setInputPrompt(mask,InputPromptKind::Space,true);
    backend.setKeyboardMousePrompts(true);
    const auto maskPixels = renderSampledPixels(sampledTexture(backend,9).Get(),64,64);
    for (size_t pixel = 0; pixel < maskPixels.size(); ++pixel) {
        const uint32_t alpha = keyPixels[pixel] >> 24;
        require(maskPixels[pixel] == (0xff000000u | alpha * 0x10101u),
            "Actual rendered prompt alpha-mask RGB differs from the source alpha channel");
    }
    backend.setKeyboardMousePrompts(false);
    verifyOriginalPixels(renderSampledPixels(sampledTexture(backend,9).Get(),64,64));
    backend.bindTexture(9,0); backend.release(mask);
    backend.bindTexture(10,0); backend.release(ordinary);
    const auto wrongAtlas = sentinelTexture(backend,64,64);
    rejects([&] { backend.setInputPrompt(wrongAtlas,InputPromptKind::ButtonsAtlas); },"Incorrect original atlas layout accepted");
    rejects([&] { backend.setInputPrompt(wrongAtlas,InputPromptKind::ControlsDiagram); },"Incorrect original control guide layout accepted");
    backend.release(wrongAtlas);
    const auto target = backend.createTexture(64,64,1,D3DFMT_A8R8G8B8,false,true);
    rejects([&] { backend.setInputPrompt(target,InputPromptKind::Space); },"Render target accepted as an ordinary input prompt");
    backend.release(target);
    require(backend.counters().draws == 0 && backend.counters().triangles == 0 && backend.counters().presents == 0 &&
        backend.counters().initializationPresents == 0,"Prompt fixtures contaminated title frame accounting");
    std::printf("Native input-prompt hardware oracle passed: actual offscreen quad/readback, key/32-slot atlas/8-tile guide/source-alpha mask, exact original restore, nonprompt stability, retained COM after token release; no game frames\n");
}
static void verifyDepthAliasing(D3D9Backend& backend) {
    // Real resources and binds, with no geometry or game presentation. The
    // separate ignored sampling-effect probe establishes comparison effects;
    // this fixture tests the production API's identities and COM lifetimes.
    rejects([&] { backend.createDepthTexture(0,64); },"Zero-sized native depth texture accepted");
    rejects([&] { backend.createDepthTexture(64,8193); },"Oversized native depth texture accepted");
    rejects([&] { backend.createTexture(64,64,1,D3DFMT_D24S8,false,false); },
        "Generic title texture creation widened to an unverified depth contract");
    const auto texture = backend.createDepthTexture(64,64);
    const auto otherTexture = backend.createDepthTexture(64,64);
    require(texture && otherTexture,"Actual native D24S8 texture allocation unavailable");
    const auto view = backend.textureSurface(texture,0,0);
    const auto retainedView = backend.textureSurface(texture,0,0);
    const auto otherView = backend.textureSurface(otherTexture,0,0);
    require(view && retainedView && otherView,"Actual native D24S8 level0 view allocation unavailable");
    backend.verifyDepthTextureSurfaceAlias(texture,view);
    backend.verifyDepthTextureSurfaceAlias(texture,retainedView);
    rejects([&] { backend.verifyDepthTextureSurfaceAlias(texture,otherView); },
        "Unrelated same-size native depth view accepted as a texture alias");
    const auto description = backend.surfaceDescription(view);
    require(description.Format == D3DFMT_D24S8 && description.Usage == D3DUSAGE_DEPTHSTENCIL &&
        description.Pool == D3DPOOL_DEFAULT && description.MultiSampleType == D3DMULTISAMPLE_NONE &&
        description.Width == 64 && description.Height == 64 && backend.textureLevelCount(texture) == 1,
        "Actual depth texture/view dimensions, usage, samples or levels changed");
    rejects([&] { backend.textureSurface(texture,1,0); },"Depth texture cube face accepted");
    rejects([&] { backend.textureSurface(texture,0,1); },"Depth texture mip1 accepted");
    rejects([&] { backend.generateMipmaps(texture); },"Depth texture mip generation accepted");
    const std::vector<uint8_t> pixels(64 * 64 * 4);
    rejects([&] { backend.uploadSurface(view,D3DFMT_D24S8,64 * 4,pixels); },"Depth texture CPU image upload accepted");
    rejects([&] { backend.resolveRenderTarget(texture); },"Depth texture accepted as a color resolve destination");
    rejects([&] { backend.selectPresentationTexture(texture); },"Depth texture accepted as a game presentation surface");
    const auto color = backend.createSurface(64,64,D3DFMT_X8R8G8B8,false,D3DMULTISAMPLE_NONE);
    backend.bindDepthSurface(0);
    backend.bindRenderTarget(0,color);
    backend.bindDepthSurface(view);
    backend.clear(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,0,1,0);
    rejects([&] { backend.bindTexture(9,texture); },"Attached depth texture accepted for simultaneous sampling");
    backend.bindTexture(9,otherTexture); // A different texture has no alias hazard.
    rejects([&] { backend.bindDepthSurface(otherView); },"Sampled texture accepted for simultaneous depth writes");
    backend.bindTexture(9,0);
    backend.bindDepthSurface(otherView); // Failed bind left valid native state.
    backend.bindDepthSurface(view);
    backend.release(view); // D3D9 retains its genuine bound COM reference.
    rejects([&] { backend.bindTexture(9,texture); },"Released view token hid a still-bound depth identity");
    backend.bindDepthSurface(0);
    backend.bindTexture(9,texture);
    backend.bindTexture(9,texture); // Replacing the same bind must not leak count/state.
    backend.release(texture); // Native sampler and retained view keep their refs.
    require(backend.surfaceDescription(retainedView).Format == D3DFMT_D24S8,
        "Parent-token release destroyed an independently retained depth view");
    rejects([&] { backend.bindDepthSurface(retainedView); },"Released texture token hid a still-sampled identity");
    backend.bindTexture(9,0);
    backend.bindDepthSurface(retainedView);
    backend.release(retainedView);
    backend.bindDepthSurface(0);
    rejects([&] { backend.surfaceDescription(retainedView); },"Released depth view token remained in registry");
    rejects([&] { backend.bindTexture(9,texture); },"Released depth texture token remained bindable");
    backend.release(otherView); // Original view-before-texture cleanup order.
    require(backend.textureDescription(otherTexture,0).Format == D3DFMT_D24S8,
        "View-token release destroyed its independently retained depth texture");
    backend.bindTexture(9,otherTexture);
    backend.bindTexture(9,0);
    backend.release(otherTexture);
    backend.bindRenderTarget(0,0);
    backend.release(color);
    require(backend.counters().draws == 0 && backend.counters().presents == 0,
        "Depth resource/hazard contracts invented game frames");
}
static void verifyDepthCompletion(D3D9Backend& backend) {
    // Real alias targets, clears and identity queries. Completion performs no
    // texture copy, DS detach, submitted geometry or game presentation.
    const auto texture = backend.createDepthTexture(2048,2048);
    const auto view = backend.textureSurface(texture,0,0);
    const auto color = backend.createSurface(2048,2048,D3DFMT_X8R8G8B8,false,D3DMULTISAMPLE_NONE);
    const auto otherColor = backend.createSurface(2048,2048,D3DFMT_X8R8G8B8,false,D3DMULTISAMPLE_NONE);
    const auto otherDepth = backend.createSurface(2048,2048,D3DFMT_D24S8,true,D3DMULTISAMPLE_NONE);
    // Exact original82390020 order: viewport first, then larger2048-square
    // color/depth, although the current backbuffer is2560x1440. The completion
    // clear queries actual native viewport after SetRenderTarget's reset.
    backend.setViewport({0,0,2048,2048,0,1});
    backend.bindDepthSurface(0);
    backend.bindRenderTarget(0,color);
    backend.bindDepthSurface(view);
    rejects([&] { backend.finishDepthTargetAlias(texture,view,color); },"Depth completion invented an original prepared frame");
    backend.prepareFrame();
    rejects([&] { backend.finishDepthTargetAlias(texture,view,color); },"Depth completion accepted an uncleared pass");
    backend.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,0xffffffff,1,0);
    rejects([&] { backend.finishDepthTargetAlias(texture,view,color); },"Unrelated native clear authorized authored shadow completion");
    backend.setViewport({0,0,1024,2048,0,1});
    backend.clearRectangles({},D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,0xffffffff,1,0,0x8237d34c);
    rejects([&] { backend.finishDepthTargetAlias(texture,view,color); },"Partial viewport clear authorized full shadow completion");
    backend.setViewport({0,0,2048,2048,0,1});
    backend.clearRectangles({},D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,0xffffffff,1,0,0x8237d34c);
    rejects([&] { backend.finishDepthTargetAlias(texture,view,otherColor); },"Unbound same-size color companion authorized shadow completion");
    backend.beginFrame();
    rejects([&] { backend.finishDepthTargetAlias(texture,view,color); },"Depth completion accepted an active scene");
    backend.endFrame();
    backend.finishDepthTargetAlias(texture,view,color);
    rejects([&] { backend.bindTexture(9,texture); },"Shadow End fabricated depth target detach");
    backend.bindDepthSurface(otherDepth); // The subsequent original main bind owns detachment.
    backend.bindTexture(9,texture);
    rejects([&] { backend.finishDepthTargetAlias(texture,view,color); },"Detached unrelated depth target authorized shadow completion");
    backend.bindTexture(9,0);
    backend.bindDepthSurface(view);
    rejects([&] { backend.finishDepthTargetAlias(texture,view,color); },"A new shadow bind inherited stale prior completion/clear evidence");
    backend.clearRectangles({},D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,0xffffffff,1,0,0x8237d34c);
    backend.finishDepthTargetAlias(texture,view,color);
    backend.release(view); // Bound device COM and completion state survive guest view release.
    rejects([&] { backend.bindTexture(9,texture); },"Guest view release hid a still-attached completed depth target");
    backend.bindDepthSurface(0);
    backend.bindTexture(9,texture);
    backend.release(texture); // Actual sampling identity remains retained until null bind.
    backend.bindTexture(9,0);
    backend.bindRenderTarget(0,0);
    backend.release(color); backend.release(otherColor); backend.release(otherDepth);
    require(backend.counters().draws == 0 && backend.counters().presents == 0 && backend.counters().triangles == 0,
        "Depth alias completion invented draw, triangle or game presentation accounting");
    std::printf("Native shadow alias completion/clear/scene/COM contracts passed; no geometry or game frames\n");
}
static void verifyPalettePrefix(HWND window) {
    // Verify the native register API's actual prefix/tail behavior without
    // submitting any synthetic geometry or changing title frame counters.
    Microsoft::WRL::ComPtr<IDirect3D9Ex> d3d;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> device;
    require(SUCCEEDED(Direct3DCreate9Ex(D3D_SDK_VERSION,&d3d)),"Cannot create native palette contract D3D9Ex");
    D3DPRESENT_PARAMETERS presentation{};
    presentation.BackBufferWidth = 2560;
    presentation.BackBufferHeight = 1440;
    presentation.BackBufferFormat = D3DFMT_X8R8G8B8;
    presentation.BackBufferCount = 1;
    presentation.SwapEffect = D3DSWAPEFFECT_DISCARD;
    presentation.hDeviceWindow = window;
    presentation.Windowed = TRUE;
    presentation.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    require(SUCCEEDED(d3d->CreateDeviceEx(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING,&presentation,nullptr,&device)),"Cannot create native palette contract HAL device");
    std::array<float,192 * 4> retained;
    retained.fill(7.0f);
    require(SUCCEEDED(device->SetVertexShaderConstantF(0,retained.data(),192)),"Cannot set native palette tail sentinel");
    std::array<float,4096> original{};
    std::array<bool,1024> written{};
    std::array<float,32> actual;
    for (uint32_t index = 0; index < actual.size(); ++index) original[13 * 4 + index] = actual[index] = float(index) / 8;
    std::fill(written.begin() + 13,written.begin() + 21,true);
    const uint32_t rows = titleBonePalettePrefix(ShaderStage::Vertex,"multilayer/vs",{4102,0,203},
        {81,13,192,0},{81,0,192,0},original,written,actual);
    require(rows == 8 && SUCCEEDED(device->SetVertexShaderConstantF(0,original.data() + 13 * 4,rows)),
        "Cannot upload the actual two-bone native palette prefix");
    std::array<float,192 * 4> observed;
    require(SUCCEEDED(device->GetVertexShaderConstantF(0,observed.data(),192)),"Cannot read actual native palette constants");
    for (uint32_t index = 0; index < observed.size(); ++index)
        require(observed[index] == (index < actual.size() ? actual[index] : 7.0f),
            "Native palette prefix upload modified the unused reflected tail");
    // RVMBatch has its own producer/geometry proof. Rebase its current active
    // matrices and read back the real device to prove the unreachable tail is
    // untouched even when its guest written bits are absent.
    std::array<float,768> batchValues;
    for (uint32_t index = 0; index < batchValues.size(); ++index) batchValues[index] = float(index) / 16 + 0.25f;
    for (const uint32_t active : {1u,3u,32u}) {
        std::array<bool,192> batchWritten{};
        std::fill_n(batchWritten.begin(),active * 4,true);
        std::vector<float> batchMatrices(batchValues.begin(),batchValues.begin() + active * 16);
        for (const auto family : {"composite/vs","multilayer/vs"}) {
            require(SUCCEEDED(device->SetVertexShaderConstantF(0,retained.data(),192)),"Cannot reset native RVMBatch tail sentinel");
            const uint32_t batchRows = titleBatchCurrentPalette(ShaderStage::Vertex,family,{0,2,79},
                {81,0,192,0},{81,0,192,0},batchValues,batchWritten,{{77,100,32,active},batchMatrices});
            require(batchRows == active * 4 && SUCCEEDED(device->SetVertexShaderConstantF(0,batchValues.data(),batchRows)),
                "Cannot upload the actual RVMBatch active matrix prefix");
            require(SUCCEEDED(device->GetVertexShaderConstantF(0,observed.data(),192)),"Cannot read native RVMBatch prefix/tail registers");
            for (uint32_t index = 0; index < observed.size(); ++index)
                require(std::bit_cast<uint32_t>(observed[index]) == std::bit_cast<uint32_t>(
                    index < batchMatrices.size() ? batchValues[index] : 7.0f),
                    "Native RVMBatch prefix changed current values or overwrote the unreachable tail");
        }
    }
}
int main() {
    HWND window = nullptr;
    try {
        verifyTitleInputPromptNames();
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"DoritosNativeResourceContracts";
        require(RegisterClassW(&type) != 0, "Cannot register synthetic contract window");
        window = CreateWindowW(type.lpszClassName, L"Native resource contract test", WS_OVERLAPPEDWINDOW,
            0, 0, 2560, 1440, nullptr, nullptr, type.hInstance, nullptr);
        require(window != nullptr, "Cannot create hidden synthetic contract window");
        verifyPalettePrefix(window);
        verifyInputPrompts(window);
        {
            // Keep prepared shadow-pass fixture state separate from the
            // ordinary title initialization-boundary regression below.
            D3D9Backend shadowBackend;
            shadowBackend.initialize(window,2560,1440);
            verifyDepthCompletion(shadowBackend);
        }
        D3D9Backend backend;
        backend.initialize(window, 2560, 1440);
        verifyDepthAliasing(backend);
        HANDLE pendingCapture = nullptr;
        wchar_t onDemand[2]{};
        if (GetEnvironmentVariableW(L"DORITOS_CAPTURE_ON_DEMAND",onDemand,2) == 1 && onDemand[0] == L'1') {
            FILETIME created{}, exited{}, kernel{}, user{};
            require(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user) != 0, "Cannot query capture contract process identity");
            const auto name = captureRequestEventName(GetCurrentProcessId(),uint64_t(created.dwHighDateTime) << 32 | created.dwLowDateTime);
            pendingCapture = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE,FALSE,name.c_str());
            require(pendingCapture && SetEvent(pendingCapture), "Opt-in capture request event was not actually created/signaled");
        }
        uint32_t uploads = 0;
        for (D3DFORMAT format : {D3DFMT_A8R8G8B8,D3DFMT_X8R8G8B8,D3DFMT_DXT1,D3DFMT_DXT5}) {
            for (bool cube : {false,true}) {
                const auto texture = backend.createTexture(64,64,2,format,cube,false);
                for (uint32_t face = 0; face < (cube ? 6u : 1u); ++face) for (uint32_t level = 0; level < 2; ++level) {
                    const auto surface = backend.textureSurface(texture,face,level);
                    const auto description = backend.surfaceDescription(surface);
                    require(description.Width == (64u >> level) && description.Height == (64u >> level), "Native face/mip dimensions changed");
                    const bool compressed = format == D3DFMT_DXT1 || format == D3DFMT_DXT5;
                    const uint32_t pitch = compressed ? ((description.Width + 3) / 4) * (format == D3DFMT_DXT1 ? 8 : 16) : description.Width * 4;
                    const uint32_t rows = compressed ? (description.Height + 3) / 4 : description.Height;
                    std::vector<uint8_t> pixels(size_t(pitch) * rows, 0x55);
                    backend.uploadSurface(surface,format,pitch,pixels);
                    ++uploads;
                    backend.release(surface);
                }
                backend.generateMipmaps(texture);
                require(backend.textureLevelCount(texture) == 2 && backend.textureDescription(texture,1).Width == 32,
                    "Actual generated mip chain lost its native dimensions");
                backend.bindTexture(0,texture);
                backend.bindTexture(0,0);
                backend.release(texture);
            }
        }
        // Actual observed authored RT constructor requests1280x384, Xbox
        // sample enum1 (native2x), opaque color and its matching depth surface.
        const auto msaaColor = backend.createSurface(1280,384,D3DFMT_X8R8G8B8,false,D3DMULTISAMPLE_2_SAMPLES);
        const auto msaaDepth = backend.createSurface(1280,384,D3DFMT_D24S8,true,D3DMULTISAMPLE_2_SAMPLES);
        const auto msaaResolve = backend.createTexture(1280,384,1,D3DFMT_X8R8G8B8,false,true);
        const auto colorDescription = backend.surfaceDescription(msaaColor);
        const auto depthDescription = backend.surfaceDescription(msaaDepth);
        require(colorDescription.Width == 1280 && colorDescription.Height == 384 &&
            colorDescription.Format == D3DFMT_X8R8G8B8 && colorDescription.MultiSampleType == D3DMULTISAMPLE_2_SAMPLES &&
            (colorDescription.Usage & D3DUSAGE_RENDERTARGET), "Observed native multisample color allocation mismatch");
        require(depthDescription.Width == 1280 && depthDescription.Height == 384 &&
            depthDescription.Format == D3DFMT_D24S8 && depthDescription.MultiSampleType == D3DMULTISAMPLE_2_SAMPLES &&
            (depthDescription.Usage & D3DUSAGE_DEPTHSTENCIL), "Observed native multisample depth allocation mismatch");
        backend.bindDepthSurface(0);
        backend.bindRenderTarget(0,msaaColor);
        backend.bindDepthSurface(msaaDepth);
        backend.setViewport({0,0,1280,384,0,1});
        backend.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,0xff123456,1,0);
        backend.resolveRenderTarget(msaaResolve,msaaColor,D3DMULTISAMPLE_2_SAMPLES);
        bool wrongResolveSamplesRefused = false;
        try { backend.resolveRenderTarget(msaaResolve,msaaColor,D3DMULTISAMPLE_NONE); }
        catch (const std::runtime_error&) { wrongResolveSamplesRefused = true; }
        require(wrongResolveSamplesRefused, "MSAA resolve silently accepted a single-sample owner contract");
        backend.bindDepthSurface(0);
        backend.bindRenderTarget(0,0);
        bool wrongResolveSourceRefused = false;
        try { backend.resolveRenderTarget(msaaResolve,msaaColor,D3DMULTISAMPLE_2_SAMPLES); }
        catch (const std::runtime_error&) { wrongResolveSourceRefused = true; }
        require(wrongResolveSourceRefused, "MSAA resolve accepted a different actual bound render target");
        backend.release(msaaColor);
        backend.release(msaaDepth);
        backend.release(msaaResolve);
        bool invalidSurfaceRefused = false;
        try { backend.createSurface(1280,384,D3DFMT_D24S8,false,D3DMULTISAMPLE_2_SAMPLES); }
        catch (const std::runtime_error&) { invalidSurfaceRefused = true; }
        require(invalidSurfaceRefused, "Depth format was accepted for a color render surface");
        const auto color = backend.createSurface(64,64,D3DFMT_X8R8G8B8,false,D3DMULTISAMPLE_NONE);
        const auto depth = backend.createSurface(64,64,D3DFMT_D24S8,true,D3DMULTISAMPLE_NONE);
        const auto resolve = backend.createTexture(64,64,1,D3DFMT_X8R8G8B8,false,true);
        backend.bindDepthSurface(0);
        backend.bindRenderTarget(0,color);
        backend.bindDepthSurface(depth);
        backend.setViewport({0,0,64,64,0,1});
        backend.setRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
        backend.setRenderState(D3DRS_ZENABLE,D3DZB_TRUE);
        backend.setRenderState(D3DRS_ZWRITEENABLE,TRUE);
        backend.setRenderState(D3DRS_CULLMODE,D3DCULL_CCW);
        backend.setRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
        backend.setRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
        backend.setRenderState(D3DRS_ALPHATESTENABLE,TRUE);
        backend.setRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER);
        backend.setRenderState(D3DRS_ALPHAREF,127);
        require(backend.renderState(D3DRS_ALPHAREF) == 127, "Native alpha-test state was not applied");
        backend.setClipPlane(0,{0,1,0,0});
        backend.setRenderState(D3DRS_CLIPPLANEENABLE,1);
        require(backend.renderState(D3DRS_CLIPPLANEENABLE) == 1, "Native clip-plane mask was not applied");
        backend.setRenderState(D3DRS_CLIPPLANEENABLE,0);
        backend.setSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
        backend.setSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_BORDER);
        backend.setSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_MIRROR);
        backend.setSamplerState(0,D3DSAMP_BORDERCOLOR,0xffffffff);
        backend.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,0xff123456,1,0);
        const D3DRECT rectangle{2,2,32,32};
        backend.clearRectangles({&rectangle,1},D3DCLEAR_TARGET,0xff654321,1,0);
        backend.resolveRenderTarget(resolve);
        backend.selectPresentationTexture(resolve);
        backend.presentInitialization();
        backend.finishPhaseFrame();
        require(backend.counters().initializationPresents == 1, "Initialization presentation was not distinguished");
        backend.clearRectangles({},D3DCLEAR_TARGET,0xff112233,1,0,0x8234ad38);
        bool arbitraryClearBoundaryRefused = false;
        try { backend.present(); } catch (const std::runtime_error&) { arbitraryClearBoundaryRefused = true; }
        require(arbitraryClearBoundaryRefused, "An arbitrary native color clear invented a prepared engine frame");
        backend.prepareFrame();
        bool missingBindingsRefused = false;
        try { backend.drawIndexedTriangles(0,3,0,1); }
        catch (const std::runtime_error& error) {
            const std::string diagnostic(error.what());
            missingBindingsRefused = diagnostic.find("prepared=1 scene=0") != std::string::npos &&
                diagnostic.find("vs=0/valid0") != std::string::npos && diagnostic.find("vb=0/valid0") != std::string::npos;
        }
        require(missingBindingsRefused, "A prepared engine frame bypassed missing native draw bindings or omitted exact state diagnostics");
        bool immediateMissingBindingsRefused = false;
        const std::array<uint8_t,36> unusedVertices{};
        const std::array<uint16_t,3> unusedIndices{0,1,2};
        try { backend.drawIndexedImmediate(D3DPT_TRIANGLELIST,unusedVertices,12,unusedIndices,1); }
        catch (const std::runtime_error& error) {
            const std::string diagnostic(error.what());
            immediateMissingBindingsRefused = diagnostic.find("prepared=1 scene=0") != std::string::npos &&
                diagnostic.find("vs=0/valid0") != std::string::npos && diagnostic.find("declaration=0/valid0") != std::string::npos;
        }
        require(immediateMissingBindingsRefused, "Prepared immediate draw bypassed missing shaders/declaration or omitted exact state diagnostics");
        backend.clear(D3DCLEAR_TARGET,0xff778899,1,0);
        backend.resolveRenderTarget(resolve);
        backend.present();
        backend.finishPhaseFrame();
        if (pendingCapture) {
            const DWORD pending = WaitForSingleObject(pendingCapture,0);
            CloseHandle(pendingCapture);
            pendingCapture = nullptr;
            require(pending == WAIT_OBJECT_0, "Clear-only presentation consumed an on-demand GPU capture request");
        }
        require(backend.counters().clearPresents == 1 && backend.counters().presents == 0 && backend.counters().measuredIntervals == 0,
            "Actual clear-only frame contaminated game-frame/FPS counters");
        backend.beginFrame();
        bool activeMissingBindingsRefused = false;
        try { backend.drawIndexedTriangles(0,3,0,1); }
        catch (const std::runtime_error& error) {
            activeMissingBindingsRefused = std::string(error.what()).find("prepared=0 scene=1") != std::string::npos;
        }
        require(activeMissingBindingsRefused, "An explicit scene bypassed native binding validation");
        backend.endFrame();
        bool refused = false;
        try { backend.present(); } catch (const std::runtime_error&) { refused = true; }
        require(refused, "Synthetic empty frame was presented as a title frame");
        const auto counters = backend.counters();
        require(counters.draws == 0 && counters.presents == 0 && counters.triangles == 0, "Contract fixtures contaminated title frame counters");
        require(counters.width == 2560 && counters.height == 1440 && uploads == 56, "Hardware resource contract count or dimensions changed");
        std::printf("Native graphics hardware contracts passed: D24S8_level0_alias_and_retained_COM_hazards=true, uploads=%u, active_palette_rows=8, untouched_palette_tail_rows=184, BOX_mip_textures=8, authored_RT=1280x384_2x_color_depth_resolve, 2560x1440, draws=0, clear_only_presents=1, game_frame_presents=0, gameplay_verified=false\n",uploads);
        backend.bindDepthSurface(0);
        backend.bindRenderTarget(0,0);
        DestroyWindow(window);
        return 0;
    } catch (const std::exception& error) {
        if (window) DestroyWindow(window);
        std::fprintf(stderr,"Native graphics hardware contract failed: %s\n",error.what());
        return 1;
    }
}
