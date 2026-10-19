// Hidden synthetic D3D9 draws only. This fixture never launches the title or
// moves the user's pointer; it exercises the actual backend publication API.
#include "renderer/native/d3d9_backend.h"
#include "renderer/native/gui_pointer.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace Doritos::Graphics;
namespace {
using Microsoft::WRL::ComPtr;
void require(bool condition, const char* detail) { if (!condition) throw std::runtime_error(detail); }
void closeTo(float actual, float expected, const char* detail) {
    require(std::isfinite(actual) && std::abs(actual - expected) < .00001f,detail);
}
void word(std::vector<uint8_t>& data, uint32_t value, bool big) {
    for (int byte = 0; byte < 4; ++byte) data.push_back(uint8_t(value >> ((big ? 3 - byte : byte) * 8)));
}
std::vector<uint8_t> compile(const char* source, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(source,std::strlen(source),"gui_pointer_fixture",nullptr,nullptr,
        "main",profile,D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY,0,&code,&errors);
    if (FAILED(result)) throw std::runtime_error(errors ?
        std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()) : "Cannot compile pointer fixture shader");
    const auto* begin = static_cast<const uint8_t*>(code->GetBufferPointer());
    return {begin,begin + code->GetBufferSize()};
}
// Vertex cache keys strip pixel-only _MAP_DIFFUSE; diffuse eligibility comes
// from the independently bound pixel key, as in the shipped native caches.
constexpr ShaderKey primary{0,0,8}, automaticUV{0,0,0x208}, skinned{0,0,0x48}, noUV{0,0,0};
constexpr ShaderKey pixelPrimary{0,2,0}, noDiffuse{0,0,0};
struct CacheShader { const char* name; std::vector<ShaderKey> keys; std::vector<uint8_t> code; bool vertex; };
std::vector<uint8_t> cache(const std::vector<CacheShader>& shaders, bool big) {
    std::vector<uint8_t> data;
    word(data,3,big); word(data,0,big); word(data,uint32_t(shaders.size()),big);
    for (const auto& shader : shaders) {
        const size_t length = std::strlen(shader.name);
        word(data,uint32_t(length + 1),big);
        data.insert(data.end(),shader.name,shader.name + length + 1);
        word(data,uint32_t(shader.keys.size()),big);
        for (const auto& key : shader.keys) {
            for (const auto feature : key) word(data,feature,big);
            word(data,shader.vertex ? 2u : 0u,big);
            if (shader.vertex) {
                // Deliberately nonzero native registers verify reflected lookup,
                // rather than assuming WVP starts at c0 or texture UV at c4.
                for (const auto parameter : {ShaderParameter{7,4,4,0},ShaderParameter{65,1,1,0}})
                    for (const auto field : {parameter.id,parameter.firstRegister,parameter.registerCount,parameter.reserved}) word(data,field,big);
            }
            word(data,uint32_t(shader.code.size()),big);
            data.insert(data.end(),shader.code.begin(),shader.code.end());
        }
    }
    return data;
}
struct Vertex { float x,y,z,nx,ny,nz,u,v; };
template<size_t Count> std::span<const uint8_t> bytes(const std::array<Vertex,Count>& vertices) {
    return {reinterpret_cast<const uint8_t*>(vertices.data()),sizeof(vertices)};
}
constexpr std::array<float,16> identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
// Columns encode x'=x+.25y+.1, y'=y-.2, z'=.25z+.4,
// w'=.5x+1. A transpose gives substantially different projected geometry.
constexpr std::array<float,16> skew{1,0,0,.5f, .25f,1,0,0, 0,0,.25f,0, .1f,-.2f,.4f,1};
constexpr std::array<float,4> originalUV{1,1,0,0};
constexpr std::array<Vertex,3> perspective{{{-.5f,.5f,0,0,0,1,0,0},
    {.75f,.4f,0,0,0,1,1,0},{-.4f,-.7f,0,0,0,1,0,1}}};
constexpr std::array<Vertex,3> flat{{{-1,1,.5f,0,0,1,0,0},
    {1,1,.5f,0,0,1,1,0},{-1,-1,.5f,0,0,1,0,1}}};
constexpr std::array<uint16_t,3> indices{0,1,2};
constexpr float perspectiveX = 14417.f / 31680, perspectiveY = 105.f / 176;
constexpr float perspectiveU = 96.f / 437, perspectiveV = 165.f / 437;

void mapping(D3D9Backend& backend, const GuiPointerContext& context, float x, float y,
    GuiPointerMapping expected, const char* detail) {
    require(backend.mapGuiPointer(context,x,y).mapping == expected,detail);
}
void hit(D3D9Backend& backend, const GuiPointerContext& context, float x, float y, float u, float v) {
    const auto result = backend.mapGuiPointer(context,x,y);
    require(result.mapping == GuiPointerMapping::Hit,"Presented native triangle did not produce a hit");
    closeTo(result.x,u,"Actual native projected U differs from independent oracle");
    closeTo(result.y,v,"Actual native projected V differs from independent oracle");
}
void verify(HWND window) {
    const auto vs = compile(R"(
        float4x4 wvp : register(c4); float4 uvTransform : register(c1);
        struct Input { float3 position:POSITION0; float2 uv:TEXCOORD0; };
        struct Output { float4 position:POSITION0; float2 uv:TEXCOORD0; };
        Output main(Input input) { Output output;
            output.position=mul(wvp,float4(input.position,1));
            output.uv=input.uv*uvTransform.xy+uvTransform.zw; return output; }
    )","vs_3_0");
    const auto ps = compile(R"(
        sampler2D diffuse : register(s0);
        float4 main(float2 uv:TEXCOORD0):COLOR0 { return tex2D(diffuse,uv); }
    )","ps_3_0");
    const std::vector<CacheShader> shaders{{"multilayer/vs",{primary,automaticUV,skinned,noUV},vs,true},
        {"fixture/vs",{primary},vs,true},{"multilayer/ps",{pixelPrimary,noDiffuse},ps,false},
        {"fixture/ps",{pixelPrimary},ps,false}};
    D3D9Backend backend;
    backend.initialize(window,320,240);
    backend.loadCaches(cache(shaders,false),cache(shaders,true));
    const auto vertexShader = backend.createShader("multilayer/vs",primary);
    backend.bindShader(ShaderStage::Vertex,vertexShader);
    const auto pixelShader = backend.createShader("multilayer/ps",pixelPrimary);
    backend.bindShader(ShaderStage::Pixel,pixelShader);
    const std::array<D3DVERTEXELEMENT9,4> declaration{{
        {0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},
        {0,12,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,0},
        {0,24,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()}};
    backend.bindDeclaration(backend.createDeclaration(declaration));
    backend.bindDepthSurface(0);
    backend.setRenderState(D3DRS_ZENABLE,FALSE);
    backend.setRenderState(D3DRS_ZWRITEENABLE,FALSE);
    backend.setRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    const auto tagged = backend.createTexture(128,72,1,D3DFMT_A8R8G8B8,false,true);
    const auto ordinary = backend.createTexture(128,72,1,D3DFMT_A8R8G8B8,false,false);
    backend.setGuiScreenTexture(tagged);
    backend.bindTexture(0,tagged);
    GuiPointerContext context{0x1000,0x2000,0x1234567800000001ULL};
    backend.setGuiPointerContext(context);
    const auto start = [&] {
        backend.selectPresentationTexture(0);
        backend.bindRenderTarget(0,0);
        backend.setViewport({0,0,320,240,0,1});
        backend.prepareFrame();
        backend.clear(D3DCLEAR_TARGET,0xff000000,1,0);
    };
    const auto constants = [&](const std::array<float,16>& matrix, const std::array<float,4>& uv = originalUV) {
        backend.setFloatConstants(ShaderStage::Vertex,7,matrix);
        backend.setFloatConstants(ShaderStage::Vertex,65,uv);
    };
    const auto immediate = [&](const std::array<Vertex,3>& vertices) {
        backend.drawIndexedImmediate(D3DPT_TRIANGLELIST,bytes(vertices),sizeof(Vertex),indices,1);
    };

    // A known monitor layer needs a captured primary surface even after a
    // successful clear-only presentation; absence must not invent flat input.
    start(); backend.present();
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Missing monitor geometry enabled incorrect flat input");
    const std::array<Vertex,5> padded{{{4,4,.5f,0,0,1,.99f,.99f},
        {5,5,.5f,0,0,1,.99f,.99f},perspective[0],perspective[1],perspective[2]}};
    const std::array<uint16_t,6> paddedIndices{0,0,0,0,1,2};
    const auto paddedBuffer = backend.createVertexBuffer(bytes(padded),sizeof(Vertex));
    const auto paddedIndexBuffer = backend.createIndexBuffer(paddedIndices);
    backend.bindVertexBuffer(paddedBuffer);
    backend.bindIndexBuffer(paddedIndexBuffer);
    start(); constants(skew);
    backend.drawIndexedTriangles(2,3,3,1);
    mapping(backend,context,perspectiveX,perspectiveY,GuiPointerMapping::Pending,"Unpresented buffered geometry leaked into pointer mapping");
    backend.present();
    // Clip vertices are (-.275,.3,.4,.75),(.95,.2,.4,1.375),
    // (-.475,-.9,.4,.8). At their projected centroid, reciprocal-W texture
    // weights are (176,96,165)/437, independently of the production helper.
    hit(backend,context,perspectiveX,perspectiveY,perspectiveU,perspectiveV);
    mapping(backend,context,.98f,.02f,GuiPointerMapping::Miss,"Pointer outside presented monitor triangles inherited a hit");
    mapping(backend,context,std::numeric_limits<float>::quiet_NaN(),.2f,GuiPointerMapping::Miss,"Nonfinite pointer was mapped");

    // Capturing another draw does not mutate the last presented snapshot.
    auto changedUV = perspective;
    for (auto& vertex : changedUV) { vertex.u = .9f; vertex.v = .1f; }
    start(); constants(skew); immediate(changedUV);
    hit(backend,context,perspectiveX,perspectiveY,perspectiveU,perspectiveV);
    backend.present(); hit(backend,context,perspectiveX,perspectiveY,.9f,.1f);

    // Reupload the same native buffer and retain its nonzero base/start. A
    // stale CPU copy would keep the old UV even though the GPU buffer changed.
    auto updatedBuffer = padded;
    for (size_t index = 2; index < updatedBuffer.size(); ++index) {
        updatedBuffer[index].u = .35f; updatedBuffer[index].v = .65f;
    }
    backend.updateVertexBuffer(paddedBuffer,bytes(updatedBuffer));
    backend.bindVertexBuffer(paddedBuffer);
    backend.bindIndexBuffer(paddedIndexBuffer);
    start(); constants(skew); backend.drawIndexedTriangles(2,3,3,1);
    hit(backend,context,perspectiveX,perspectiveY,.9f,.1f);
    backend.present(); hit(backend,context,perspectiveX,perspectiveY,.35f,.65f);

    ++context.rootHandle;
    backend.setGuiPointerContext(context);
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Recycled root identity inherited prior presented geometry");
    start(); constants(identity); immediate(flat);
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"New generation published before Present");
    backend.present(); hit(backend,context,.25f,.25f,.25f,.25f);
    for (const auto mismatch : {GuiPointerContext{context.screen + 1,context.root,context.rootHandle},
            GuiPointerContext{context.screen,context.root + 1,context.rootHandle},
            GuiPointerContext{context.screen,context.root,context.rootHandle + 1}})
        mapping(backend,mismatch,.25f,.25f,GuiPointerMapping::Pending,"Screen/root/handle mismatch reused another context's hit");

    const GuiPointerContext previousContext = context;
    auto interveningContext = context;
    ++interveningContext.rootHandle;
    backend.setGuiPointerContext(interveningContext);
    backend.setGuiPointerContext(previousContext);
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Context A-to-B-to-A reused a stale A presentation");
    start(); constants(identity); immediate(flat); backend.present();
    hit(backend,context,.25f,.25f,.25f,.25f);
    backend.setGuiPointerContext(context,false);
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Flat,"Flat mode did not bypass projected background coordinates");
    backend.setGuiPointerContext(context,true);
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Same context flat-to-monitor switch reused stale presented geometry");
    start(); constants(identity); immediate(flat); backend.present();
    hit(backend,context,.25f,.25f,.25f,.25f);

    const auto fullSource = backend.createSurface(320,240,D3DFMT_A8R8G8B8,false,D3DMULTISAMPLE_NONE);
    start(); backend.bindRenderTarget(0,fullSource); backend.setViewport({40,30,240,180,0,1});
    constants(skew); immediate(perspective); backend.present();
    hit(backend,context,(40 + 240 * perspectiveX) / 320,(30 + 180 * perspectiveY) / 240,perspectiveU,perspectiveV);
    mapping(backend,context,.02f,.02f,GuiPointerMapping::Miss,"Point outside actual viewport inherited monitor UV");

    const auto otherSource = backend.createSurface(320,240,D3DFMT_A8R8G8B8,false,D3DMULTISAMPLE_NONE);
    const auto presentationTexture = backend.createTexture(320,240,1,D3DFMT_A8R8G8B8,false,true);
    const auto presentationSurface = backend.textureSurface(presentationTexture,0,0);
    auto sourceA = flat, sourceB = flat;
    for (auto& vertex : sourceA) { vertex.z = .2f; vertex.u = .2f; vertex.v = .3f; }
    for (auto& vertex : sourceB) { vertex.z = .8f; vertex.u = .8f; vertex.v = .7f; }
    const auto drawOnTarget = [&](uint32_t target, const std::array<Vertex,3>& vertices) {
        backend.bindRenderTarget(0,target);
        backend.setViewport({0,0,320,240,0,1});
        backend.clear(D3DCLEAR_TARGET,0xff000000,1,0);
        constants(identity); immediate(vertices);
    };

    // Full-size auxiliary draws are not visible merely because they were
    // captured. Presenting the cleared backbuffer must publish no monitor.
    start(); drawOnTarget(fullSource,sourceA); backend.bindRenderTarget(0,0); backend.present();
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Unpresented full-size auxiliary target supplied monitor coordinates");

    // A full resolve transfers the source geometry to the destination texture.
    // Selection follows that texture even when the current target is different.
    start(); drawOnTarget(fullSource,sourceA);
    backend.resolveRenderTarget(presentationTexture,fullSource,D3DMULTISAMPLE_NONE);
    backend.selectPresentationTexture(presentationTexture);
    drawOnTarget(otherSource,sourceB); backend.present();
    hit(backend,context,.25f,.25f,.2f,.3f);

    // Different native surfaces share dimensions and overlap on screen, but
    // only the actual presentation source contributes UV. A is closer than B:
    // combining both groups would incorrectly choose A while presenting B.
    start(); drawOnTarget(fullSource,sourceA); drawOnTarget(otherSource,sourceB); backend.present();
    hit(backend,context,.25f,.25f,.8f,.7f);
    start(); drawOnTarget(fullSource,sourceA); drawOnTarget(otherSource,sourceB);
    backend.bindRenderTarget(0,fullSource); backend.present();
    hit(backend,context,.25f,.25f,.2f,.3f);

    // Resolving another source over the selected texture replaces its group,
    // and a source with no matching draw erases the old destination geometry.
    start(); drawOnTarget(fullSource,sourceA);
    backend.resolveRenderTarget(presentationTexture,fullSource,D3DMULTISAMPLE_NONE);
    drawOnTarget(otherSource,sourceB);
    backend.resolveRenderTarget(presentationTexture,otherSource,D3DMULTISAMPLE_NONE);
    backend.selectPresentationTexture(presentationTexture); backend.present();
    hit(backend,context,.25f,.25f,.8f,.7f);
    start(); drawOnTarget(fullSource,sourceA);
    backend.resolveRenderTarget(presentationTexture,fullSource,D3DMULTISAMPLE_NONE);
    backend.bindRenderTarget(0,otherSource);
    backend.clear(D3DCLEAR_TARGET,0xff000000,1,0);
    backend.resolveRenderTarget(presentationTexture,otherSource,D3DMULTISAMPLE_NONE);
    backend.selectPresentationTexture(presentationTexture); backend.present();
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Unrelated resolve overwrite retained old destination monitor coordinates");

    // Color clears invalidate geometry for that exact surface, including a
    // texture-level alias. Another captured target must not restore its hit.
    start(); drawOnTarget(fullSource,sourceA); drawOnTarget(otherSource,sourceB);
    backend.bindRenderTarget(0,fullSource);
    backend.clear(D3DCLEAR_TARGET,0xff000000,1,0); backend.present();
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Cleared presentation source retained monitor coordinates from another target");
    start(); drawOnTarget(fullSource,sourceA);
    backend.resolveRenderTarget(presentationTexture,fullSource,D3DMULTISAMPLE_NONE);
    backend.bindRenderTarget(0,presentationSurface);
    backend.clear(D3DCLEAR_TARGET,0xff000000,1,0);
    backend.selectPresentationTexture(presentationTexture); backend.present();
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Clearing a resolved texture surface alias retained stale monitor coordinates");

    // Original FrameEnd resolves this frame into A, presents the previously
    // selected B, then selects A for next time. Preserve each resolved texture's
    // geometry rather than assuming this frame's draw is already visible.
    const auto nextPresentationTexture = backend.createTexture(320,240,1,D3DFMT_A8R8G8B8,false,true);
    start(); drawOnTarget(fullSource,sourceA);
    backend.resolveRenderTarget(presentationTexture,fullSource,D3DMULTISAMPLE_NONE);
    backend.selectPresentationTexture(nextPresentationTexture); backend.present();
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Empty previous presentation texture inherited newly resolved A geometry");
    backend.selectPresentationTexture(presentationTexture);
    start(); backend.selectPresentationTexture(presentationTexture);
    drawOnTarget(fullSource,sourceB);
    backend.resolveRenderTarget(nextPresentationTexture,fullSource,D3DMULTISAMPLE_NONE);
    backend.present(); hit(backend,context,.25f,.25f,.2f,.3f);
    backend.selectPresentationTexture(nextPresentationTexture);
    start(); backend.selectPresentationTexture(nextPresentationTexture); backend.present();
    hit(backend,context,.25f,.25f,.8f,.7f);
    ++context.rootHandle;
    backend.setGuiPointerContext(context);
    start(); backend.selectPresentationTexture(presentationTexture); backend.present();
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Resolved texture from an older root supplied stale menu coordinates");

    // The repeated wall variant uses the same GUI texture with scale(5,5)
    // and offset(0,.05). It must not become a primary monitor hit surface.
    start(); constants(identity,{5,5,0,.05f}); immediate(flat); backend.present();
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Repeated wall UV transform enabled incorrect monitor input");
    for (const auto texture : {ordinary,uint32_t(0)}) {
        backend.bindTexture(0,texture);
        backend.bindTexture(1,tagged);
        start(); constants(identity); immediate(flat); backend.present();
        mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Unrelated sampler0 texture enabled incorrect monitor input");
    }
    backend.bindTexture(0,tagged); backend.bindTexture(1,0);
    for (const auto key : {automaticUV,skinned,noUV}) {
        backend.bindShader(ShaderStage::Vertex,backend.createShader("multilayer/vs",key));
        start(); constants(identity); immediate(flat); backend.present();
        mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Unsupported primary monitor semantics fell back to incorrect flat coordinates");
    }
    backend.bindShader(ShaderStage::Vertex,vertexShader);
    for (const auto wrongPixel : {backend.createShader("multilayer/ps",noDiffuse),
            backend.createShader("fixture/ps",pixelPrimary)}) {
        backend.bindShader(ShaderStage::Pixel,wrongPixel);
        start(); constants(identity); immediate(flat); backend.present();
        mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Unrelated or nondiffuse pixel shader supplied monitor UV semantics");
    }
    backend.bindShader(ShaderStage::Pixel,pixelShader);
    backend.bindShader(ShaderStage::Vertex,backend.createShader("fixture/vs",primary));
    start(); constants(identity); immediate(flat); backend.present();
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Unrelated shader family enabled incorrect monitor input");
    backend.bindShader(ShaderStage::Vertex,vertexShader);
    const auto offscreen = backend.createSurface(64,64,D3DFMT_A8R8G8B8,false,D3DMULTISAMPLE_NONE);
    start(); backend.bindRenderTarget(0,offscreen); backend.setViewport({0,0,64,64,0,1});
    constants(identity); immediate(flat); backend.present();
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Pending,"Small offscreen pass enabled incorrect monitor input");
    start(); constants(identity); immediate(flat); backend.present();
    hit(backend,context,.25f,.25f,.25f,.25f);
    // A flat dialog/pause layer can retain the same projected background.
    // Its own hit coordinates remain flat before and after that background draw.
    ++context.rootHandle;
    backend.setGuiPointerContext(context,false);
    mapping(backend,context,.25f,.25f,GuiPointerMapping::Flat,"Flat input layer inherited an older projected background");
    start(); constants(skew); immediate(perspective);
    mapping(backend,context,perspectiveX,perspectiveY,GuiPointerMapping::Flat,"Unpresented background remapped a flat input layer");
    backend.present();
    mapping(backend,context,perspectiveX,perspectiveY,GuiPointerMapping::Flat,"Presented monitor background remapped a flat input layer");
    for (const auto invalidTag : {uint32_t(0),ordinary}) {
        bool rejected = false;
        try { backend.setGuiScreenTexture(invalidTag); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected,"A missing or ordinary texture was tagged as a GUI color render texture");
    }
}
}
int main() {
    HWND window = nullptr;
    try {
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"DoritosNativeGuiPointerContracts";
        require(RegisterClassW(&type) != 0,"Cannot register hidden GUI pointer fixture");
        window = CreateWindowW(type.lpszClassName,L"Hidden native GUI pointer contract",WS_OVERLAPPEDWINDOW,
            0,0,320,240,nullptr,nullptr,type.hInstance,nullptr);
        require(window != nullptr && !IsWindowVisible(window),"GUI pointer fixture must remain hidden");
        verify(window);
        DestroyWindow(window);
        std::puts("Native GUI pointer draw/presentation contracts passed; hidden synthetic fixtures only");
        return 0;
    } catch (const std::exception& error) {
        if (window) DestroyWindow(window);
        std::fprintf(stderr,"Native GUI pointer contract failed: %s\n",error.what());
        return 1;
    }
}
