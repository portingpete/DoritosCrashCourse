#include "model.h"
#include "world.h"
#include "collision.h"
#include "locomotion.h"
#include "player.h"
#include "scripts.h"
#include "environment.h"
#include "session.h"
#include "actors.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <xinput.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <optional>
#include <iomanip>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace Doritos::Engine;
namespace {
void check(HRESULT status,const char* message) { if(FAILED(status))throw std::runtime_error(message); }
void localPath(const std::filesystem::path& path) {
    for(auto cursor=std::filesystem::absolute(path);!cursor.empty();) {
        DWORD flags=GetFileAttributesW(cursor.c_str());
        if(flags!=INVALID_FILE_ATTRIBUTES && (flags&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Native engine refuses linked asset/output paths");
        auto parent=cursor.parent_path(); if(parent==cursor)break; cursor=parent;
    }
}
struct Options {
    std::filesystem::path assets=L"build_native/engine_inputs", evidence;
    bool hidden=false, verify=false, check=false, coursePreview=false, verifyCourses=false;
    bool coursePlay=false,verifyPlayer=false,verifyConveyors=false,verifyActors=false;
    unsigned level=0;
};
Options options(int argc,wchar_t** argv) {
    Options result;
    for(int i=1;i<argc;++i) {
        std::wstring argument=argv[i];
        if(argument==L"--hidden")result.hidden=true;
        else if(argument==L"--course-play")result.coursePlay=result.coursePreview=true;
        else if(argument==L"--verify-player" && i+1<argc) { result.verifyPlayer=result.coursePlay=result.coursePreview=result.verify=true;result.evidence=argv[++i]; }
        else if(argument==L"--verify-conveyors" && i+1<argc) { result.verifyConveyors=result.coursePlay=result.coursePreview=result.verify=true;result.evidence=argv[++i]; }
        else if(argument==L"--verify-actors" && i+1<argc) { result.verifyActors=result.coursePreview=result.verify=true;result.evidence=argv[++i]; }
        else if(argument==L"--course-preview")result.coursePreview=true;
        else if(argument==L"--verify-courses" && i+1<argc) { result.verifyCourses=result.coursePreview=result.verify=true;result.evidence=argv[++i]; }
        else if(argument==L"--check")result.check=true;
        else if(argument==L"--verify-run" && i+1<argc) { result.verify=true; result.evidence=argv[++i]; }
        else if(argument==L"--assets" && i+1<argc)result.assets=argv[++i];
        else if(argument==L"--level" && i+1<argc) { auto number=std::wstring(argv[++i]); size_t used=0; result.level=std::stoul(number,&used); if(used!=number.size() || result.level>=15)throw std::runtime_error("Course ID must be 0-14"); }
        else throw std::runtime_error("Unknown native engine argument");
    }
    if((result.coursePlay&&result.verify&&!result.verifyPlayer&&!result.verifyConveyors)||(result.verifyCourses&&result.coursePlay)
       ||(result.verifyPlayer&&result.verifyConveyors)||(result.verifyConveyors&&result.level!=0)
       ||(result.verifyActors&&(result.coursePlay||result.verifyCourses||result.level!=0)))
        throw std::runtime_error("Choose one native verification mode");
    localPath(result.assets);
    if(result.verify) {
        result.evidence=std::filesystem::absolute(result.evidence); localPath(result.evidence);
        if(std::filesystem::exists(result.evidence))throw std::runtime_error("Choose a fresh verification output directory");
        std::filesystem::create_directories(result.evidence);
    }
    return result;
}
LRESULT CALLBACK windowProcedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_CLOSE) { DestroyWindow(window); return 0; }
    if(message==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window,message,wparam,lparam);
}
struct NativeWindow {
    HWND handle=nullptr;
    explicit NativeWindow(bool hidden) {
        WNDCLASSW type{}; type.lpfnWndProc=windowProcedure; type.hInstance=GetModuleHandleW(nullptr);
        type.lpszClassName=L"DoritosIndependentNativeEngine"; type.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
        if(!RegisterClassW(&type))throw std::runtime_error("Cannot register native engine window");
        RECT rectangle{0,0,1280,720}; AdjustWindowRect(&rectangle,WS_OVERLAPPEDWINDOW,FALSE);
        handle=CreateWindowExW(hidden?WS_EX_NOACTIVATE:0,type.lpszClassName,L"Crash Course — Independent Native Engine Preview",
            WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,
            nullptr,nullptr,type.hInstance,nullptr);
        if(!handle)throw std::runtime_error("Cannot create native engine window");
        if(!hidden)ShowWindow(handle,SW_SHOWNOACTIVATE);
    }
    ~NativeWindow() { if(IsWindow(handle))DestroyWindow(handle); }
    bool events() {
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT)return false;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        return true;
    }
};
struct GpuMesh {
    ComPtr<ID3D11Buffer> vertices,indices;
    ComPtr<ID3D11ShaderResourceView> texture;
    uint32_t count=0;
};
struct GpuWorld {
    std::vector<GpuMesh> meshes;
    std::vector<ComPtr<ID3D11ShaderResourceView>> textures;
};
struct Renderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    std::vector<GpuMesh> meshes;
    GpuWorld courseGeometry,environmentGeometry;
    std::vector<std::vector<GpuMesh>> actorGeometry;
    std::vector<ComPtr<ID3D11ShaderResourceView>> actorTextures;
    XMFLOAT4 ambient{.4f,.4f,.4f,0};
    std::array<XMFLOAT4,4> lightDirections{},lightColors{};
    GpuMesh floor;
    uint64_t frames=0,draws=0;
    // Persistent unscaled kernel/effect time, independent of actor/race resets.
    double effectTime=0;
    void advanceEffects(double unscaledDelta) { effectTime=advanceEffectClock(effectTime,unscaledDelta); }
    HFONT font=nullptr;
    explicit Renderer(HWND window) {
        DXGI_SWAP_CHAIN_DESC description{}; description.BufferDesc.Width=1280; description.BufferDesc.Height=720;
        description.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count=1;
        description.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; description.BufferCount=1;
        description.OutputWindow=window; description.Windowed=TRUE; description.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        description.Flags=DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
        D3D_FEATURE_LEVEL feature{};
        check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&description,&swapchain,&device,&feature,&context),"Cannot create native D3D11 device");
        ComPtr<ID3D11Texture2D> color; check(swapchain->GetBuffer(0,IID_PPV_ARGS(&color)),"No native backbuffer");
        check(device->CreateRenderTargetView(color.Get(),nullptr,&target),"Cannot create native render target");
        D3D11_TEXTURE2D_DESC z{}; z.Width=1280; z.Height=720; z.MipLevels=1; z.ArraySize=1;
        z.Format=DXGI_FORMAT_D24_UNORM_S8_UINT; z.SampleDesc.Count=1; z.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> zbuffer; check(device->CreateTexture2D(&z,nullptr,&zbuffer),"Cannot create native depth buffer");
        check(device->CreateDepthStencilView(zbuffer.Get(),nullptr,&depth),"Cannot create native depth view");
        const char* shader=R"(
cbuffer Camera : register(b0) { float4x4 worldViewProjection; float4x4 world; float4 tint; float4 properties; float4 ambient; float4 lightDirections[4]; float4 lightColors[4]; };
struct Input { float3 position:POSITION; float3 normal:NORMAL; float2 uv:TEXCOORD0; };
struct Output { float4 position:SV_POSITION; float3 normal:NORMAL; float2 uv:TEXCOORD0; };
Output vertexMain(Input v) { Output o; o.position=mul(float4(v.position,1),worldViewProjection); o.normal=mul(float4(v.normal,0),world).xyz; o.uv=v.uv+properties.yz; return o; }
Texture2D diffuse:register(t0); SamplerState linearSampler:register(s0);
float4 pixelMain(Output i):SV_TARGET { float4 color=diffuse.Sample(linearSampler,i.uv)*tint; clip(color.a-.02); float3 light=1; if(properties.x>.5) { light=ambient.rgb; float3 normal=normalize(i.normal); [unroll] for(int j=0;j<4;++j)light+=lightColors[j].rgb*saturate(dot(normal,lightDirections[j].xyz)); } return float4(color.rgb*light,color.a); }
)";
        ComPtr<ID3DBlob> vs,ps,error;
        check(D3DCompile(shader,strlen(shader),"IndependentNativeShader",nullptr,nullptr,"vertexMain","vs_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vs,&error),"Cannot compile native vertex shader");
        check(D3DCompile(shader,strlen(shader),"IndependentNativeShader",nullptr,nullptr,"pixelMain","ps_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&ps,&error),"Cannot compile native pixel shader");
        check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertexShader),"Cannot create native vertex shader");
        check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixelShader),"Cannot create native pixel shader");
        D3D11_INPUT_ELEMENT_DESC elements[]{
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};
        check(device->CreateInputLayout(elements,3,vs->GetBufferPointer(),vs->GetBufferSize(),&layout),"Cannot create native input layout");
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth=304; buffer.Usage=D3D11_USAGE_DEFAULT; buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&buffer,nullptr,&constants),"Cannot create native camera buffer");
        D3D11_SAMPLER_DESC filtering{}; filtering.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        filtering.AddressU=filtering.AddressV=filtering.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
        filtering.MaxLOD=D3D11_FLOAT32_MAX; check(device->CreateSamplerState(&filtering,&sampler),"Cannot create native sampler");
        D3D11_RASTERIZER_DESC rasterDescription{}; rasterDescription.FillMode=D3D11_FILL_SOLID;
        rasterDescription.CullMode=D3D11_CULL_NONE; rasterDescription.DepthClipEnable=TRUE;
        check(device->CreateRasterizerState(&rasterDescription,&raster),"Cannot create native rasterizer");
        D3D11_BLEND_DESC blending{};auto& b=blending.RenderTarget[0];b.BlendEnable=TRUE;
        b.SrcBlend=D3D11_BLEND_SRC_ALPHA;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;
        b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device->CreateBlendState(&blending,&blend),"Cannot create native alpha blending");
        font=CreateFontW(20,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    }
    ~Renderer() { if(font)DeleteObject(font); }
    ComPtr<ID3D11ShaderResourceView> rgba(uint32_t width,uint32_t height,const uint8_t* bytes) {
        D3D11_TEXTURE2D_DESC description{}; description.Width=width; description.Height=height;
        description.MipLevels=description.ArraySize=1; description.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count=1; description.Usage=D3D11_USAGE_IMMUTABLE; description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{bytes,width*4,0}; ComPtr<ID3D11Texture2D> image;
        check(device->CreateTexture2D(&description,&data,&image),"Cannot create native texture");
        ComPtr<ID3D11ShaderResourceView> view; check(device->CreateShaderResourceView(image.Get(),nullptr,&view),"Cannot create native texture view"); return view;
    }
    ComPtr<ID3D11ShaderResourceView> image(const std::filesystem::path& path) {
        localPath(path); ComPtr<IWICImagingFactory> factory;
        check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"Cannot create native image decoder");
        ComPtr<IWICBitmapDecoder> decoder; check(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder),"Cannot open character texture");
        ComPtr<IWICBitmapFrameDecode> frame; check(decoder->GetFrame(0,&frame),"Cannot decode character texture");
        UINT width=0,height=0; check(frame->GetSize(&width,&height),"Cannot read character texture size");
        if(!width || !height || width>4096 || height>4096)throw std::runtime_error("Character texture dimensions outside budget");
        ComPtr<IWICFormatConverter> converter; check(factory->CreateFormatConverter(&converter),"Cannot create pixel converter");
        check(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"Cannot convert character pixels");
        std::vector<uint8_t> bytes(size_t(width)*height*4); check(converter->CopyPixels(nullptr,width*4,UINT(bytes.size()),bytes.data()),"Cannot read character pixels");
        return rgba(width,height,bytes.data());
    }
    GpuMesh geometry(const std::vector<DrawVertex>& vertices,const std::vector<uint32_t>& indices) {
        GpuMesh mesh; mesh.count=uint32_t(indices.size());
        D3D11_BUFFER_DESC description{}; description.ByteWidth=UINT(vertices.size()*sizeof(DrawVertex)); description.Usage=D3D11_USAGE_DYNAMIC;
        description.BindFlags=D3D11_BIND_VERTEX_BUFFER; description.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        D3D11_SUBRESOURCE_DATA data{vertices.data(),0,0}; check(device->CreateBuffer(&description,&data,&mesh.vertices),"Cannot create native vertex buffer");
        description.ByteWidth=UINT(indices.size()*4); description.Usage=D3D11_USAGE_IMMUTABLE;
        description.BindFlags=D3D11_BIND_INDEX_BUFFER; description.CPUAccessFlags=0; data.pSysMem=indices.data();
        check(device->CreateBuffer(&description,&data,&mesh.indices),"Cannot create native index buffer"); return mesh;
    }
    void load(const Model& model,const std::filesystem::path& assets) {
        auto pose=samplePose(model,nullptr,0);
        for(const auto& source:model.meshes) {
            auto mesh=geometry(skin(source,pose),source.indices); mesh.texture=image(assets/(L"diffuse-"+std::to_wstring(source.texture)+L".png")); meshes.push_back(std::move(mesh));
        }
        floor=geometry({{{-100,-8,0},{0,0,1},{0,0}},{{100,-8,0},{0,0,1},{40,0}},{{100,8,0},{0,0,1},{40,4}},{{-100,8,0},{0,0,1},{0,4}}},{0,1,2,0,2,3});
        uint8_t pixels[]{74,87,77,255,104,117,99,255,104,117,99,255,74,87,77,255}; floor.texture=rgba(2,2,pixels);
    }
    void loadWorldGeometry(const World& world,const std::filesystem::path& assets,GpuWorld& gpu) {
        for(const auto& source:world.meshes)gpu.meshes.push_back(geometry(source.vertices,source.indices));
        std::map<std::string,ComPtr<ID3D11ShaderResourceView>> cache;
        uint8_t white[]{255,255,255,255};auto fallback=rgba(1,1,white);
        for(const auto& m:world.materials) {
            if(m.texture.empty())gpu.textures.push_back(fallback);
            else {
                if(!cache.contains(m.texture))cache[m.texture]=image(assets/std::filesystem::path(m.texture));
                gpu.textures.push_back(cache.at(m.texture));
            }
        }
    }
    void loadActorGeometry(const Actors& actors,const std::filesystem::path& assets) {
        for(const auto& source:actors.models) {
            std::vector<GpuMesh> groups;const auto pose=samplePose(source.model,nullptr,0);
            for(const auto& mesh:source.model.meshes)groups.push_back(geometry(skin(mesh,pose),mesh.indices));
            actorGeometry.push_back(std::move(groups));
        }
        std::map<std::string,ComPtr<ID3D11ShaderResourceView>> cache;
        uint8_t white[]{255,255,255,255};auto fallback=rgba(1,1,white);
        for(const auto& material:actors.materials) {
            if(material.texture.empty())actorTextures.push_back(fallback);
            else {
                if(!cache.contains(material.texture))cache[material.texture]=image(assets/std::filesystem::path(material.texture));
                actorTextures.push_back(cache.at(material.texture));
            }
        }
    }
    void draw(GpuMesh& mesh,FXMMATRIX world,FXMMATRIX viewProjection,uint32_t first=0,uint32_t count=0,XMFLOAT4 tint={1,1,1,1},bool lit=true,XMFLOAT2 uvOffset={}) {
        struct Camera { XMFLOAT4X4 worldViewProjection,world;XMFLOAT4 tint,properties,ambient;std::array<XMFLOAT4,4> directions,colors; } camera;
        static_assert(sizeof(Camera)==304);
        XMStoreFloat4x4(&camera.worldViewProjection,XMMatrixTranspose(world*viewProjection));
        // Inverse transpose is required for authored nonuniform resource scale.
        XMStoreFloat4x4(&camera.world,XMMatrixInverse(nullptr,world));camera.tint=tint;camera.properties={lit?1.f:0.f,uvOffset.x,uvOffset.y,0};
        camera.ambient=ambient;camera.directions=lightDirections;camera.colors=lightColors;
        context->UpdateSubresource(constants.Get(),0,nullptr,&camera,0,0);
        UINT stride=sizeof(DrawVertex),offset=0; context->IASetVertexBuffers(0,1,mesh.vertices.GetAddressOf(),&stride,&offset);
        context->IASetIndexBuffer(mesh.indices.Get(),DXGI_FORMAT_R32_UINT,0); context->PSSetShaderResources(0,1,mesh.texture.GetAddressOf());
        context->DrawIndexed(count?count:mesh.count,first,0); ++draws;
    }
    void drawWorld(const World& world,GpuWorld& gpu,unsigned course,FXMMATRIX viewProjection,XMFLOAT3 focus,bool cullDistant) {
        for(const auto& instance:world.courses.at(course).instances) {
            if(cullDistant&&std::abs(instance.transform._41-focus.x)>100)continue;
            auto& mesh=gpu.meshes.at(instance.mesh);
            for(const auto& surface:world.meshes[instance.mesh].surfaces) {
                const auto& material=world.materials[surface.material];mesh.texture=gpu.textures.at(surface.material);
                auto tint=material.color;tint.w*=instance.opacity;
                draw(mesh,XMLoadFloat4x4(&instance.transform),viewProjection,surface.first,surface.count,tint,material.lit,materialUvOffset(material,effectTime));
            }
        }
    }
    void drawActors(const Actors& actors,const CourseActorSystem* runtime,unsigned course,FXMMATRIX viewProjection,XMFLOAT3 focus) {
        const auto& instances=actors.courses.at(course).hammers;
        for(size_t i=0;i<instances.size();++i) {
            const auto& instance=instances[i];if(std::abs(instance.actorWorld._41-focus.x)>100)continue;
            const auto& source=actors.models.at(instance.model);
            auto pose=runtime?runtime->pose(i):samplePose(source.model,nullptr,0);auto transform=actorModelWorld(instance);
            for(size_t j=0;j<source.model.meshes.size();++j) {
                auto& mesh=actorGeometry.at(instance.model).at(j);const auto vertices=skin(source.model.meshes[j],pose);
                D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(mesh.vertices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Cannot update native obstacle geometry");
                memcpy(mapped.pData,vertices.data(),vertices.size()*sizeof(DrawVertex));context->Unmap(mesh.vertices.Get(),0);
                const auto materialIndex=source.materials.at(j);const auto& material=actors.materials.at(materialIndex);mesh.texture=actorTextures.at(materialIndex);
                draw(mesh,XMLoadFloat4x4(&transform),viewProjection,0,0,material.color,material.lit,materialUvOffset(material,effectTime));
            }
        }
    }
    void lighting(const EnvironmentStage* stage) {
        lightDirections={};lightColors={};ambient={.4f,.4f,.4f,0};
        if(!stage) {
            XMFLOAT3 direction;XMStoreFloat3(&direction,XMVector3Normalize(XMVectorSet(-.4f,-.6f,1,0)));
            lightDirections[0]={direction.x,direction.y,direction.z,0};lightColors[0]={.6f,.6f,.6f,0};return;
        }
        ambient={};size_t directional=0;
        for(const auto& light:stage->lights) {
            XMFLOAT4 color{light.color.x*light.diffuseFactor,light.color.y*light.diffuseFactor,light.color.z*light.diffuseFactor,0};
            if(light.type==0) { ambient.x+=color.x;ambient.y+=color.y;ambient.z+=color.z; }
            else if(light.type==1&&directional<lightDirections.size()) {
                // Source light travel direction is +Z, which becomes native +Y.
                auto direction=XMVector3Normalize(XMVectorNegate(XMVector3TransformNormal(XMVectorSet(0,1,0,0),XMLoadFloat4x4(&light.transform))));
                XMStoreFloat4(&lightDirections[directional],direction);lightColors[directional++]=color;
            }
        }
    }
    void frame(const Model& model,const Player& player,unsigned course,const World* world=nullptr,XMFLOAT3 focus={},const CoursePlayer* actor=nullptr,
               const World* environment=nullptr,const EnvironmentStage* stage=nullptr,const CourseSession* session=nullptr,
               const Actors* actors=nullptr,const CourseActorSystem* dynamics=nullptr) {
        XMFLOAT4 color=stage?stage->background:XMFLOAT4{.075f,.095f,.085f,1};context->ClearRenderTargetView(target.Get(),&color.x);
        lighting(stage);
        context->ClearDepthStencilView(depth.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
        context->OMSetRenderTargets(1,target.GetAddressOf(),depth.Get()); D3D11_VIEWPORT viewport{0,0,1280,720,0,1}; context->RSSetViewports(1,&viewport);
        context->RSSetState(raster.Get()); context->IASetInputLayout(layout.Get()); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertexShader.Get(),nullptr,0); context->PSSetShader(pixelShader.Get(),nullptr,0);
        context->VSSetConstantBuffers(0,1,constants.GetAddressOf()); context->PSSetSamplers(0,1,sampler.GetAddressOf());
        context->PSSetConstantBuffers(0,1,constants.GetAddressOf());context->OMSetBlendState(blend.Get(),nullptr,0xffffffff);
        auto view=world ? XMMatrixLookAtRH(XMVectorSet(focus.x+3,-17,focus.z+6,1),XMVectorSet(focus.x,0,focus.z+1,1),XMVectorSet(0,0,1,0)) : XMMatrixLookAtRH(XMVectorSet(player.position.x+3,-5,2.3f,1),XMVectorSet(player.position.x,0,1,1),XMVectorSet(0,0,1,0));
        float fov=XM_PIDIV4;
        if(world&&stage) {
            auto camera=environmentView(*stage,focus);view=XMLoadFloat4x4(&camera.view);fov=camera.verticalFov;
        }
        auto projection=XMMatrixPerspectiveFovRH(fov,1280.f/720,.05f,250.f), viewProjection=view*projection;
        if(world) {
            if(environment)drawWorld(*environment,environmentGeometry,course,viewProjection,focus,false);
            drawWorld(*world,courseGeometry,course,viewProjection,focus,true);
            if(actors)drawActors(*actors,dynamics,course,viewProjection,focus);
        } else draw(floor,XMMatrixIdentity(),viewProjection);
        const Clip* clip=nullptr; for(const auto& c:model.clips)if(c.name==player.animation)clip=&c;
        auto pose=actor?actor->pose():samplePose(model,clip,player.animationTime);
        for(size_t i=0;i<meshes.size();++i) {
            auto vertices=skin(model.meshes[i],pose); D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(meshes[i].vertices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Cannot update native skinned geometry");
            memcpy(mapped.pData,vertices.data(),vertices.size()*sizeof(DrawVertex)); context->Unmap(meshes[i].vertices.Get(),0);
            auto origin=actor?actor->position:world?world->courses[course].spawn:player.position;
            auto transform=characterWorld(origin,actor?actor->facing:player.facing);
            draw(meshes[i],XMLoadFloat4x4(&transform),viewProjection);
        }
        context->OMSetRenderTargets(0,nullptr,nullptr);
        ComPtr<IDXGISurface1> surface; check(swapchain->GetBuffer(0,IID_PPV_ARGS(&surface)),"Cannot access native HUD surface");
        HDC dc{}; check(surface->GetDC(FALSE,&dc),"Cannot draw native HUD"); auto old=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,RGB(224,237,205));
        std::wstring text=actor?L"INDEPENDENT NATIVE ENGINE / COURSE MOVEMENT\n":world?L"INDEPENDENT NATIVE ENGINE / STATIC COURSE INSPECTION\n":L"INDEPENDENT NATIVE ENGINE / CHARACTER PREVIEW\n";
        if(course<model.courses.size()) { auto& level=model.courses[course]; text+=std::wstring(level.name.begin(),level.name.end())+L" / "+std::to_wstring(level.chunks.size())+L" original chunk definitions loaded\n"; }
        text+=actor?L"A/D: move   Space: jump (hold for height)   R: checkpoint respawn   Tab: next course   Esc: close\n":world?L"A/D: pan camera   W/S: camera height   Tab: next course   Esc: close\n":L"A/D: move   Space: jump   Tab: course catalog   Esc: close\n";
        text+=actor?L"Course movement, scene triggers and skeletal hammer preview. Actor groups, impacts, effects, audio and full gameplay are pending.\n":world?L"Course, environment and skeletal actor preview. Actor groups, impacts, effects and full gameplay are pending.\n":L"D3D11 / PC animation / native simulation arena. Use --course-preview for course inspection.\n";
        text+=L"Time "+std::to_wstring(player.clock)+L"  X "+std::to_wstring(player.position.x)+L"  "+std::wstring(player.animation.begin(),player.animation.end());
        if(session) {
            text+=L"\nRace "+std::to_wstring(session->elapsed())+L"  Checkpoints "+std::to_wstring(session->checkpointCount())+L"  Respawns "+std::to_wstring(session->respawnCount());
            if(session->finished())text+=L"  Finish line crossed";
            if(!session->hud().empty())text+=L"  Checkpoint";
            if(!session->issues().empty())text+=L"  Script issues "+std::to_wstring(session->issues().size());
        }
        RECT rect{24,20,1250,180}; DrawTextW(dc,text.c_str(),-1,&rect,DT_LEFT|DT_TOP|DT_NOPREFIX); SelectObject(dc,old);
        check(surface->ReleaseDC(nullptr),"Cannot release native HUD surface"); ++frames;
    }
    void capture(const std::filesystem::path& path) {
        localPath(path); if(std::filesystem::exists(path))throw std::runtime_error("Capture output already exists");
        ComPtr<ID3D11Texture2D> source; check(swapchain->GetBuffer(0,IID_PPV_ARGS(&source)),"No native capture source");
        D3D11_TEXTURE2D_DESC description{}; source->GetDesc(&description); description.Usage=D3D11_USAGE_STAGING;
        description.BindFlags=0; description.CPUAccessFlags=D3D11_CPU_ACCESS_READ; description.MiscFlags=0;
        ComPtr<ID3D11Texture2D> staging; check(device->CreateTexture2D(&description,nullptr,&staging),"Cannot create native capture staging");
        context->CopyResource(staging.Get(),source.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Cannot read native GPU capture");
        BITMAPFILEHEADER file{}; file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER); file.bfSize=file.bfOffBits+description.Width*description.Height*4;
        BITMAPINFOHEADER image{}; image.biSize=sizeof(image); image.biWidth=description.Width; image.biHeight=-LONG(description.Height);
        image.biPlanes=1; image.biBitCount=32; image.biCompression=BI_RGB;
        std::ofstream output(path,std::ios::binary); output.write(reinterpret_cast<char*>(&file),sizeof(file)); output.write(reinterpret_cast<char*>(&image),sizeof(image));
        for(UINT row=0;row<description.Height;++row)output.write(reinterpret_cast<char*>(mapped.pData)+row*mapped.RowPitch,description.Width*4);
        context->Unmap(staging.Get(),0); if(!output)throw std::runtime_error("Cannot finish native GPU capture");
    }
    void present() { check(swapchain->Present(1,0),"Native D3D11 presentation failed"); }
};

// A bounded probe of the first authored conveyor section. It uses the same
// player, collision ownership and renderer as course play, with explicit
// starting positions derived from the converted platform triangles.
int verifyConveyors(const Options& option,NativeWindow& window,Renderer& renderer,const Model& model,
                    const World& world,const World& backdrop,const Environment& environment,
                    CoursePlayer& actor,const CollisionScene& collision) {
    auto require=[](bool value,const char* message) { if(!value)throw std::runtime_error(message); };
    struct Belt { uint32_t index;XMFLOAT3 spawn,motor; };
    std::vector<Belt> belts;uint32_t firstChunk=UINT32_MAX;
    for(uint32_t i=0;i<collision.platformCount();++i) {
        const auto& platform=collision.platform(i);auto motor=surfaceVelocity(platform);
        if(!platform.conveyor||!platform.useForFloor||std::abs(std::abs(motor.x)-3)>.01f)continue;
        auto bounds=collision.platformBounds(i);if(!bounds)continue;
        XMFLOAT3 origin{(bounds->first.x+bounds->second.x)*.5f,(bounds->first.y+bounds->second.y)*.5f,bounds->second.z+.5f};
        auto hit=collision.ray(origin,{0,0,-1},bounds->second.z-bounds->first.z+1,true);
        if(!hit||hit->normal.z<.99f||collision.platformForTriangle(hit->triangle)!=&platform)continue;
        if(platform.chunk<firstChunk) { firstChunk=platform.chunk;belts.clear(); }
        if(platform.chunk==firstChunk)belts.push_back({i,{hit->position.x,hit->position.y,hit->position.z+.05f},motor});
    }
    require(belts.size()==4,"First USA conveyor section does not contain four owner-verified belts");
    require(std::count_if(belts.begin(),belts.end(),[](const auto& value){return value.motor.x>0;})==2,
            "First USA conveyor section lacks its two opposing motor pairs");
    std::ofstream trace(option.evidence/L"positions.csv"),report(option.evidence/L"result.json");
    require(bool(trace)&&bool(report),"Cannot create native conveyor evidence");
    trace<<"tick,case,platform,chunk,ordinal,x,y,z,vx,vy,vz,relative_x,external_x,surface_x,grounded,on_conveyor,support_triangle,horizontal,pressed,held,effect_time\n"<<std::setprecision(10);
    uint64_t ticks=0;unsigned captures=0,cases=0,supportedTransitions=0,airTransitions=0;Player player;
    std::vector<std::pair<double,double>> uvTimes;
    std::string caseName;const Belt* current=nullptr;
    auto previous=std::chrono::steady_clock::now();
    auto render=[&] {
        require(window.events(),"Conveyor verification window closed early");
        auto now=std::chrono::steady_clock::now();
        renderer.advanceEffects(std::chrono::duration<double>(now-previous).count());previous=now;
        player.position=actor.position;player.velocity=actor.velocity;player.clock=float(actor.clock);player.animation=actor.animation;
        renderer.frame(model,player,0,&world,actor.position,&actor,&backdrop,&environment.stageForCourse(0));
    };
    auto step=[&](CourseInput input=CourseInput{}) {
        actor.tick(input,1.f/60,collision);++ticks;render();
        auto relative=actor.relativeVelocity();const auto& platform=collision.platform(current->index);
        trace<<ticks<<','<<caseName<<','<<current->index<<','<<platform.chunk<<','<<platform.ordinal<<','
             <<actor.position.x<<','<<actor.position.y<<','<<actor.position.z<<','<<actor.velocity.x<<','<<actor.velocity.y<<','<<actor.velocity.z<<','
             <<relative.x<<','<<actor.externalVelocity.x<<','<<actor.surfaceVelocity.x<<','<<actor.grounded<<','<<actor.onConveyor<<','
             <<actor.supportTriangle<<','<<input.horizontal<<','<<input.jumpPressed<<','<<input.jumpHeld<<','<<renderer.effectTime<<'\n';
        renderer.present();
    };
    auto capture=[&](const std::wstring& suffix) {
        render();renderer.capture(option.evidence/(L"belt-"+std::to_wstring(current->index)+L"-"+suffix+L".bmp"));++captures;renderer.present();
    };
    auto reset=[&](const char* name,CourseStart start=CourseStart::Ready) {
        caseName=name;double effects=renderer.effectTime;actor.reset(current->spawn,start,0);
        require(renderer.effectTime==effects&&actor.surfaceVelocity.x==0&&actor.externalVelocity.x==0
                &&actor.supportTriangle==UINT32_MAX,"Conveyor reset retained carry or reset the material clock");
        for(unsigned i=0;i<120&&!actor.grounded;++i)step();
        require(actor.grounded&&actor.onConveyor&&collision.platformForTriangle(actor.supportTriangle)==&collision.platform(current->index),
                "Conveyor starting position acquired another platform");
        for(unsigned i=0;i<2;++i)step();
    };
    auto nearValue=[&](float actual,float expected,float tolerance,const char* message) {
        require(std::isfinite(actual)&&std::abs(actual-expected)<=tolerance,message);
    };
    for(const auto& belt:belts) {
        current=&belt;float against=belt.motor.x>0?-1.f:1.f;
        reset("idle");auto begin=actor.position;
        for(unsigned i=0;i<12;++i)step();
        require(actor.grounded&&actor.onConveyor,"Idle conveyor probe lost support");
        nearValue(actor.velocity.x,belt.motor.x,.03f,"Idle conveyor speed differs from its authored motor");
        nearValue(actor.position.x-begin.x,belt.motor.x*.2f,.015f,"Idle conveyor displacement differs from its authored motor");
        capture(L"idle");++cases;

        reset("against");
        for(unsigned i=0;i<16;++i)step({against,false,false});
        require(actor.grounded&&actor.relativeVelocity().x*against>0,"Against-belt movement lost root motion or support");
        nearValue(actor.velocity.x,actor.relativeVelocity().x+belt.motor.x+actor.externalVelocity.x,.03f,
                  "Against-belt velocity did not combine action and surface motion");
        capture(L"against");++cases;

        float expected=actor.velocity.x*.9f;
        if(std::abs(expected)<1||expected*belt.motor.x<0)expected+=.2f*belt.motor.x;
        caseName="fresh-jump";step({0,true,true});
        require(actor.jumps==1&&!actor.grounded,"Fresh conveyor jump did not dispatch");
        nearValue(actor.velocity.x,expected,.1f,"Fresh conveyor jump has an incorrect horizontal impulse");
        require(actor.externalVelocity.x==0&&actor.surfaceVelocity.x==0,"Fresh conveyor jump inherited belt carry twice");
        for(unsigned i=0;i<6;++i)step({0,false,true});
        require(actor.jumps==1&&actor.externalVelocity.x==0,"Held conveyor jump duplicated carry");
        capture(L"jump");++cases;

        reset("buffered-jump",CourseStart::Startup);
        step({against,true,true});require(actor.jumps==0,"Startup conveyor press jumped before dispatch was eligible");
        expected=actor.velocity.x*.9f;
        if(std::abs(expected)<1||expected*belt.motor.x<0)expected+=1.6f*belt.motor.x;
        step({0,false,true});
        require(actor.jumps==1&&!actor.grounded,"Buffered conveyor jump did not dispatch on its later action tick");
        nearValue(actor.velocity.x,expected,.15f,"Buffered conveyor jump has an incorrect horizontal impulse");
        require(actor.externalVelocity.x==0&&actor.surfaceVelocity.x==0,"Buffered conveyor jump duplicated external carry");
        capture(L"buffered-jump");++cases;

        reset("belt-transition");
        for(unsigned i=0;i<240&&actor.grounded&&actor.onConveyor;++i)step();
        require(!actor.onConveyor&&actor.surfaceVelocity.x==0,"Conveyor probe did not leave the finite belt section");
        if(actor.grounded) {
            ++supportedTransitions;
            require(collision.platformForTriangle(actor.supportTriangle)!=nullptr,"Conveyor exit lost its static platform identity");
            for(unsigned i=0;i<6;++i) {
                step();require(actor.grounded&&!actor.onConveyor,"Supported belt exit unexpectedly lost its static floor");
                nearValue(actor.externalVelocity.x,0,.001f,"Supported belt exit retained external carry");
                nearValue(actor.velocity.x,0,.03f,"Idle player did not stop on the adjacent static platform");
            }
        } else {
            ++airTransitions;
            require(actor.externalVelocity.x*belt.motor.x>0&&std::abs(actor.externalVelocity.x)<=std::abs(belt.motor.x)+.01f,
                    "Walking off a conveyor omitted or duplicated its external carry");
            float priorCarry=std::abs(actor.externalVelocity.x);
            for(unsigned i=0;i<6;++i) {
                step();require(!actor.grounded&&actor.surfaceVelocity.x==0,"Conveyor air-carry probe reached another floor too soon");
                require(std::abs(actor.externalVelocity.x)<=priorCarry+.001f,"Prior conveyor carry accumulated in successive air spans");
                priorCarry=std::abs(actor.externalVelocity.x);
            }
        }
        capture(L"belt-transition");++cases;

        reset("reset");nearValue(actor.velocity.x,belt.motor.x,.03f,"Conveyor reset failed to reacquire the motor");
        capture(L"reset");++cases;

        // Hold simulation and camera still so capture differences isolate the
        // material clock. This clock continues across every actor reset above.
        capture(L"uv-0");double uvBegin=renderer.effectTime;
        auto uvDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(renderer.effectTime-uvBegin<.3) {
            require(std::chrono::steady_clock::now()<uvDeadline,"Conveyor UV capture clock did not advance");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));render();renderer.present();
        }
        capture(L"uv-1");uvTimes.emplace_back(uvBegin,renderer.effectTime);
    }
    trace.flush();require(bool(trace),"Cannot finish native conveyor trace");
    report<<"{\"passed\":true,\"runtime\":\"independent-native\",\"scope\":\"bounded-first-USA-conveyor-section\","
          <<"\"course\":0,\"chunk\":"<<firstChunk<<",\"platforms\":"<<belts.size()<<",\"cases\":"<<cases
          <<",\"native_ticks\":"<<ticks<<",\"captures\":"<<captures<<",\"frames\":"<<renderer.frames<<",\"draws\":"<<renderer.draws
          <<",\"supported_belt_exits\":"<<supportedTransitions<<",\"air_belt_exits\":"<<airTransitions
          <<",\"effect_time\":"<<renderer.effectTime<<",\"uses_xex\":false,\"uses_guest_memory\":false,\"uv_capture_times\":[";
    for(size_t i=0;i<uvTimes.size();++i) { if(i)report<<',';report<<'['<<uvTimes[i].first<<','<<uvTimes[i].second<<']'; }report<<"]}\n";
    require(bool(report),"Cannot finish native conveyor report");
    std::cout<<"[NATIVE CONVEYORS] platforms="<<belts.size()<<" cases="<<cases<<" ticks="<<ticks<<" captures="<<captures<<" passed=1\n";
    return 0;
}
}

int wmain(int argc,wchar_t** argv) {
    try {
        auto option=options(argc,argv); localPath(option.assets/L"character.dne"); auto model=loadModel(option.assets/L"character.dne");
        std::cout<<"[INDEPENDENT ENGINE] bones="<<model.bones.size()<<" courses="<<model.courses.size()<<" clips="<<model.clips.size()<<" no_xex=1 no_guest_memory=1\n";
        if(option.check) {
            if(std::filesystem::exists(option.assets/L"scripts.dnl")) {
                localPath(option.assets/L"scripts.dnl");auto chunks=loadScripts(option.assets/L"scripts.dnl");ScriptVm vm;
                for(auto& chunk:chunks)vm.validate(chunk.code,chunk.source+":"+chunk.actor+":"+chunk.field);
                std::cout<<"[NATIVE SCRIPT CHECK] compiled_chunks="<<chunks.size()<<" native_lua=1 scene_bindings=partial\n";
            }
            float worst=0; auto pose=samplePose(model,nullptr,0);
            for(auto& mesh:model.meshes) { auto vertices=skin(mesh,pose); for(size_t i=0;i<vertices.size();++i) { auto a=XMLoadFloat3(&vertices[i].position),b=XMLoadFloat3(&mesh.vertices[i].position); worst=std::max(worst,XMVectorGetX(XMVector3Length(XMVectorSubtract(a,b)))); } }
            if(worst>.002f || model.courses.size()!=15)throw std::runtime_error("Native rest pose/course check failed");
            if(std::filesystem::exists(option.assets/L"movement.dnp")) {
                localPath(option.assets/L"movement.dnp");auto settings=loadMovementSettings(option.assets/L"movement.dnp");
                const Clip *slow=nullptr,*fast=nullptr;
                for(auto& clip:model.clips) { if(clip.name=="run")slow=&clip;if(clip.name=="run_fast")fast=&clip; }
                if(!slow||!fast)throw std::runtime_error("Native movement settings require slow and fast clips");
                GroundRun run;float distance=0;
                for(int i=0;i<240;++i)distance+=run.advance(*slow,*fast,settings,i<180?1.f:0.f,1.f/120).distance;
                if(!std::isfinite(distance)||distance<=0||run.amount()!=0)throw std::runtime_error("Native authored locomotion check failed");
                std::cout<<"[NATIVE LOCOMOTION CHECK] motion_distance="<<distance<<" stopped=1 authored_settings=1\n";
            }
            if(option.coursePreview) {
                localPath(option.assets/L"world.dnw");auto world=loadWorld(option.assets/L"world.dnw");
                size_t count=0;for(auto& c:world.courses)count+=c.instances.size();
                std::cout<<"[NATIVE WORLD CHECK] courses="<<world.courses.size()<<" meshes="<<world.meshes.size()<<" materials="<<world.materials.size()<<" instances="<<count<<'\n';
                localPath(option.assets/L"static-collision.dnc");auto collisions=loadCollision(option.assets/L"static-collision.dnc");size_t triangles=0;
                for(size_t i=0;i<collisions.size();++i) {
                    triangles+=collisions[i].scene.triangleCount();auto hit=collisions[i].scene.ray(world.courses[i].spawn,{0,0,-1},10);
                    if(!hit||hit->distance>2||hit->normal.z<.5f)throw std::runtime_error("Native collision cannot find authored starting platform");
                }
                std::cout<<"[NATIVE COLLISION CHECK] courses="<<collisions.size()<<" static_triangles="<<triangles<<" spawn_floor_rays=15\n";
                if(option.coursePlay) {
                    localPath(option.assets/L"movement.dnp");auto settings=loadMovementSettings(option.assets/L"movement.dnp");
                    CoursePlayer actor(model,settings);actor.reset(world.courses[option.level].spawn,CourseStart::Startup,model.courses[option.level].theme);
                    for(int i=0;i<120;++i)actor.tick({},1.f/60,collisions[option.level].scene);
                    if(!actor.grounded)throw std::runtime_error("Native course player cannot settle on the starting platform");
                    std::cout<<"[NATIVE PLAYER CHECK] course="<<option.level<<" settled_z="<<actor.position.z<<" grounded=1\n";
                }
                for(const auto* file:{L"environment.dnv",L"environment-world.dnw",L"scene.dns"})localPath(option.assets/file);
                auto environment=loadEnvironment(option.assets/L"environment.dnv");auto backdrop=loadWorld(option.assets/L"environment-world.dnw");
                auto scene=loadScene(option.assets/L"scene.dns");size_t records=0,boxes=0;
                for(const auto& level:scene.courses) { records+=level.records.size();for(const auto& record:level.records)boxes+=record.boxes.size(); }
                if(backdrop.courses.size()!=15||scene.courses.size()!=15)throw std::runtime_error("Native scene catalog is incomplete");
                std::cout<<"[NATIVE SCENE CHECK] stages="<<environment.stages.size()<<" environment_meshes="<<backdrop.meshes.size()<<" trigger_records="<<records<<" trigger_boxes="<<boxes<<'\n';
                if(std::filesystem::exists(option.assets/L"actors.dna")) {
                    auto actors=loadActors(option.assets/L"actors.dna");size_t instances=0,attachments=0;
                    for(const auto& course:actors.courses)for(const auto& instance:course.hammers) { ++instances;attachments+=instance.boxes.size(); }
                    std::cout<<"[NATIVE ACTOR CHECK] models="<<actors.models.size()<<" hammers="<<instances<<" bone_boxes="<<attachments<<" impacts_pending=1\n";
                }
            }
            std::cout<<"[NATIVE CHECK] rest_pose_max_error="<<worst<<" passed=1\n"; return 0;
        }
        check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"Cannot initialize native image services");
        struct ComCleanup { ~ComCleanup(){CoUninitialize();} } cleanup;
        NativeWindow window(option.hidden); Renderer renderer(window.handle); renderer.load(model,option.assets);
        std::optional<World> world,backdrop;std::optional<Environment> environment;
        std::optional<Actors> actors;std::unique_ptr<CourseActorSystem> dynamics;
        if(option.coursePreview) {
            for(const auto* file:{L"world.dnw",L"environment.dnv",L"environment-world.dnw"})localPath(option.assets/file);
            world=loadWorld(option.assets/L"world.dnw");renderer.loadWorldGeometry(*world,option.assets,renderer.courseGeometry);
            backdrop=loadWorld(option.assets/L"environment-world.dnw");renderer.loadWorldGeometry(*backdrop,option.assets,renderer.environmentGeometry);
            environment=loadEnvironment(option.assets/L"environment.dnv");
            if(std::filesystem::exists(option.assets/L"actors.dna")) {
                actors=loadActors(option.assets/L"actors.dna");renderer.loadActorGeometry(*actors,option.assets);
                dynamics=std::make_unique<CourseActorSystem>(*actors,option.level);
            }
        }
        if(option.verifyActors) {
            if(!actors||!dynamics)throw std::runtime_error("Actor verification requires actors.dna");
            const auto& hammers=actors->courses[0].hammers;
            auto found=std::find_if(hammers.begin(),hammers.end(),[](const auto& h){return h.chunk==4&&h.ordinal==10;});
            if(found==hammers.end())throw std::runtime_error("Authored USA1 hammer verification target is absent");
            const size_t index=size_t(found-hammers.begin());auto object=dynamics->lookup(found->chunk,found->name);
            if(!object)throw std::runtime_error("Native hammer script lookup failed");
            std::ofstream trace(option.evidence/L"actors.csv");trace<<"case,running,first_pending,playing,countdown,animation_time,plays,box,x,y,z\n"<<std::setprecision(10);
            Player preview;XMFLOAT3 focus{found->actorWorld._41,found->actorWorld._42,found->actorWorld._43};
            auto capture=[&](const wchar_t* label) {
                if(!window.events())throw std::runtime_error("Actor verification window closed");
                const auto state=dynamics->states().at(index);auto boxes=dynamics->boxTransforms(index);
                for(size_t i=0;i<boxes.size();++i) {
                    const auto& m=boxes[i];trace<<std::filesystem::path(label).string()<<','<<state.running<<','<<state.firstPending<<','<<state.playing<<','<<state.countdown<<','<<state.animationTime<<','<<state.plays<<','<<i<<','<<m._41<<','<<m._42<<','<<m._43<<'\n';
                }
                renderer.frame(model,preview,0,&*world,focus,nullptr,&*backdrop,&environment->stageForCourse(0),nullptr,&*actors,dynamics.get());
                renderer.capture(option.evidence/(std::wstring(label)+L".bmp"));renderer.present();
            };
            auto command=[&](const char* name,bool expected) { auto value=object->call(name,{});if(!std::holds_alternative<bool>(value)||std::get<bool>(value)!=expected)throw std::runtime_error("Hammer command return differs from authored wrapper"); };
            capture(L"rest");command("Resume",true);dynamics->tick(.1f);capture(L"playing");
            command("Pause",false);dynamics->tick(.2f);capture(L"paused-resource-advances");
            command("Reset",true);dynamics->tick(.2f);capture(L"reset-resource-advances");
            command("Resume",true);dynamics->tick(.05f);capture(L"resumed-restarts");
            command("Pause",false);dynamics->tick(.75f);capture(L"held-final-pose");trace.flush();
            auto state=dynamics->states().at(index);
            if(!trace||state.plays!=2||state.running||!state.playing)throw std::runtime_error("Native hammer sequence failed");
            std::ofstream report(option.evidence/L"result.json");report<<"{\"passed\":true,\"scope\":\"controlled-skeletal-hammer-preview\",\"captures\":6,\"plays\":"<<state.plays<<",\"animation_time\":"<<state.animationTime<<",\"notices\":"<<dynamics->notices().size()<<",\"impact_response_tested\":false,\"actor_group_lifecycle_tested\":false}\n";
            if(!report)throw std::runtime_error("Cannot write native actor result");return 0;
        }
        std::unique_ptr<CoursePlayer> actor;std::vector<CourseCollision> collisions;float baseHeight=0;
        std::optional<Scene> scene;std::vector<ScriptChunk> scriptChunks;std::unique_ptr<CourseSession> session;
        if(option.coursePlay) {
            for(const auto* file:{L"movement.dnp",L"static-collision.dnc",L"scene.dns",L"scripts.dnl"})localPath(option.assets/file);
            auto settings=loadMovementSettings(option.assets/L"movement.dnp");collisions=loadCollision(option.assets/L"static-collision.dnc");
            actor=std::make_unique<CoursePlayer>(model,settings);actor->reset(world->courses[option.level].spawn,CourseStart::Startup,model.courses[option.level].theme);
            scene=loadScene(option.assets/L"scene.dns");scriptChunks=loadScripts(option.assets/L"scripts.dnl");
            session=std::make_unique<CourseSession>(scene->courses[option.level],scriptChunks,model.courses[option.level].theme,settings.feetOffset,settings.feetRadii,
                [&](uint32_t chunk,std::string_view name){return dynamics?dynamics->lookup(chunk,name):ScriptObjectPtr{};});
            auto hit=collisions[option.level].scene.ray(actor->position,{0,0,-1},10);
            if(!hit)throw std::runtime_error("Native player start has no static floor");baseHeight=hit->position.z;
        }
        if(option.verifyConveyors)return verifyConveyors(option,window,renderer,model,*world,*backdrop,*environment,*actor,collisions[0].scene);
        Player player; bool previousJump=false,previousTab=false,pendingJump=false,previousRestart=false,scriptHeld=false; unsigned course=option.level,captured=0;
        XMFLOAT3 focus=world?world->courses[course].spawn:XMFLOAT3{};
        float maximumHeight=0; uint64_t ticks=0; float accumulator=0;size_t loggedIssues=0;
        std::ofstream trace;
        if(option.verifyPlayer) { trace.open(option.evidence/L"positions.csv");trace<<"tick,time,course,x,y,z,vx,vz,grounded,jumps,horizontal,jump_pressed,jump_held\n"<<std::setprecision(10);if(!trace)throw std::runtime_error("Cannot create native player trace"); }
        std::array<uint64_t,15> courseDraws{};
        auto previous=std::chrono::steady_clock::now(),deadline=previous;
        while(window.events()) {
            auto now=std::chrono::steady_clock::now(); float elapsed=std::chrono::duration<float>(now-previous).count(); previous=now;
            renderer.advanceEffects(elapsed);
            accumulator+=std::min(elapsed,.1f); Input input; bool jump=false,tab=false,restart=false;
            float vertical=0;
            if(option.verify && !world) { input.horizontal=player.clock>=2 && player.clock<7?1.f:0.f; jump=player.clock>=4 && player.clock<4.05f; }
            else if(!option.verify && GetForegroundWindow()==window.handle) {
                if(GetAsyncKeyState(VK_ESCAPE)&0x8000)break;
                input.horizontal=float(bool(GetAsyncKeyState('D')&0x8000))-float(bool(GetAsyncKeyState('A')&0x8000));
                vertical=float(bool(GetAsyncKeyState('W')&0x8000))-float(bool(GetAsyncKeyState('S')&0x8000));
                jump=(GetAsyncKeyState(VK_SPACE)&0x8000)!=0; tab=(GetAsyncKeyState(VK_TAB)&0x8000)!=0;
                restart=(GetAsyncKeyState('R')&0x8000)!=0;
                XINPUT_STATE state{}; if(XInputGetState(0,&state)==ERROR_SUCCESS) { if(std::abs(int(state.Gamepad.sThumbLX))>XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)input.horizontal=float(state.Gamepad.sThumbLX)/32767.f; jump|=(state.Gamepad.wButtons&XINPUT_GAMEPAD_A)!=0; }
            }
            pendingJump|=jump&&!previousJump; previousJump=jump;
            if(tab&&!previousTab) {
                course=(course+1)%unsigned(model.courses.size());if(world)focus=world->courses[course].spawn;
                if(actors)dynamics=std::make_unique<CourseActorSystem>(*actors,course);
                if(actor) {
                    actor->reset(focus,CourseStart::Startup,model.courses[course].theme);
                    auto settings=loadMovementSettings(option.assets/L"movement.dnp");
                    session=std::make_unique<CourseSession>(scene->courses[course],scriptChunks,model.courses[course].theme,settings.feetOffset,settings.feetRadii,
                        [&](uint32_t chunk,std::string_view name){return dynamics?dynamics->lookup(chunk,name):ScriptObjectPtr{};});
                    loggedIssues=0;
                }
            } previousTab=tab;
            if(restart&&!previousRestart&&actor&&session)session->requestRespawn();previousRestart=restart;
            if(world && !actor && !option.verify) { focus.x+=input.horizontal*elapsed*15;focus.z+=vertical*elapsed*10; }
            const float step=actor?1.f/60:1.f/120;
            while(accumulator>=step) {
                input.jump=pendingJump;
                if(actor) {
                    CourseInput controls{input.horizontal,pendingJump,jump};
                    if(option.verifyPlayer) {
                        double t=actor->clock,start=1.5,jumpStart=start+16./60,end=jumpStart+9./60;
                        controls.horizontal=t>=start&&t<end?1.f:0.f;controls.jumpHeld=t>=jumpStart&&t<end;
                        controls.jumpPressed=controls.jumpHeld&&!scriptHeld;scriptHeld=controls.jumpHeld;
                    }
                    actor->tick(controls,step,collisions[course].scene);
                    session->tick(*actor,step);
                    if(dynamics)dynamics->tick(step);
                    while(loggedIssues<session->issues().size()) {
                        const auto& issue=session->issues()[loggedIssues++];
                        std::cerr<<"[NATIVE SCENE ISSUE] record="<<issue.record<<" field="<<issue.field<<" task="<<issue.task<<" error="<<issue.error<<'\n';
                    }
                    player.position=actor->position;player.velocity=actor->velocity;player.clock=float(actor->clock);player.animation=actor->animation;
                    focus=actor->position;
                    if(option.verifyPlayer)trace<<ticks<<','<<actor->clock<<','<<course<<','<<actor->position.x<<','<<actor->position.y<<','<<actor->position.z<<','
                        <<actor->velocity.x<<','<<actor->velocity.z<<','<<actor->grounded<<','<<actor->jumps<<','<<controls.horizontal<<','<<controls.jumpPressed<<','<<controls.jumpHeld<<'\n';
                } else player.tick(world?Input{}:input,step);
                pendingJump=false;accumulator-=step;++ticks;maximumHeight=std::max(maximumHeight,player.position.z-baseHeight);
            }
            if(option.verifyCourses) { course=std::min(unsigned(player.clock),14u);focus=world->courses[course].inspection; }
            auto beforeDraws=renderer.draws;renderer.frame(model,player,course,world?&*world:nullptr,focus,actor.get(),backdrop?&*backdrop:nullptr,
                environment?&environment->stageForCourse(course):nullptr,session.get(),actors?&*actors:nullptr,actor?dynamics.get():nullptr);
            if(world)courseDraws[course]+=renderer.draws-beforeDraws;
            if(option.verifyPlayer) {
                if(captured<4 && actor->clock>=std::array<double,4>{1.4,1.7,2.15,3.6}[captured]) { renderer.capture(option.evidence/(L"player-"+std::to_wstring(captured)+L".bmp"));++captured; }
            } else if(option.verifyCourses) {
                if(captured<15 && course==captured && player.clock>=captured+.4f) { renderer.capture(option.evidence/(L"course-"+std::to_wstring(captured)+L".bmp"));++captured; }
            } else if(option.verify && captured<3 && player.clock>=std::array<float,3>{1,4.3f,9}[captured]) { renderer.capture(option.evidence/(L"frame-"+std::to_wstring(captured)+L".bmp")); ++captured; }
            renderer.present();
            if(option.verify && player.clock>=(option.verifyPlayer?5.f:option.verifyCourses?15.f:11.f))break;
            deadline+=std::chrono::microseconds(16667); if(deadline<now)deadline=now; std::this_thread::sleep_until(deadline);
        }
        if(option.verify) {
            bool passed=world ? captured==(option.verifyCourses?15u:3u) && renderer.draws>1000 : captured==3 && player.position.x>14 && maximumHeight>.7f && ticks>1200;
            if(option.verifyPlayer) { trace.flush();passed=captured==4&&actor->jumps==1&&actor->landings>=2&&actor->grounded&&ticks>=300
                &&actor->position.x-world->courses[course].spawn.x>2&&maximumHeight>1&&maximumHeight<3&&bool(trace); }
            if(option.verifyCourses)passed &= std::all_of(courseDraws.begin(),courseDraws.end(),[](auto draws){return draws>4;});
            std::ofstream report(option.evidence/L"result.json"); report<<"{\"passed\":"<<(passed?"true":"false")<<",\"runtime\":\"independent-native\",\"scope\":\""<<(actor?"starting-platform-native-movement":world?"static-course-inspection":"character-animation-arena")<<"\",\"native_ticks\":"<<ticks<<",\"draws\":"<<renderer.draws<<",\"frames\":"<<renderer.frames<<",\"player_x\":"<<player.position.x<<",\"maximum_height\":"<<maximumHeight<<",\"captures\":"<<captured<<",\"courses_in_catalog\":"<<model.courses.size()<<",\"uses_xex\":false,\"uses_guest_memory\":false";
            if(actor)report<<",\"course\":"<<course<<",\"player_z\":"<<actor->position.z<<",\"grounded\":"<<(actor->grounded?"true":"false")<<",\"jumps\":"<<actor->jumps<<",\"landings\":"<<actor->landings;
            if(session)report<<",\"scene_records\":"<<scene->courses[course].records.size()<<",\"script_issues\":"<<session->issues().size()<<",\"checkpoints\":"<<session->checkpointCount()<<",\"respawns\":"<<session->respawnCount();
            report<<",\"course_draws\":[";
            for(size_t i=0;i<courseDraws.size();++i) { if(i)report<<',';report<<courseDraws[i]; }report<<"]}\n";
            if(!report || !passed)throw std::runtime_error("Independent native verification did not complete");
        }
        return 0;
    } catch(const std::exception& error) { std::cerr<<"[NATIVE ENGINE ERROR] "<<error.what()<<'\n'; return 1; }
}
