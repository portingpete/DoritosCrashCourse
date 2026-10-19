#include "model.h"
#include "world.h"
#include "collision.h"
#include "locomotion.h"
#include "player.h"
#include "scripts.h"
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
    bool coursePlay=false,verifyPlayer=false;
    unsigned level=0;
};
Options options(int argc,wchar_t** argv) {
    Options result;
    for(int i=1;i<argc;++i) {
        std::wstring argument=argv[i];
        if(argument==L"--hidden")result.hidden=true;
        else if(argument==L"--course-play")result.coursePlay=result.coursePreview=true;
        else if(argument==L"--verify-player" && i+1<argc) { result.verifyPlayer=result.coursePlay=result.coursePreview=result.verify=true;result.evidence=argv[++i]; }
        else if(argument==L"--course-preview")result.coursePreview=true;
        else if(argument==L"--verify-courses" && i+1<argc) { result.verifyCourses=result.coursePreview=result.verify=true;result.evidence=argv[++i]; }
        else if(argument==L"--check")result.check=true;
        else if(argument==L"--verify-run" && i+1<argc) { result.verify=true; result.evidence=argv[++i]; }
        else if(argument==L"--assets" && i+1<argc)result.assets=argv[++i];
        else if(argument==L"--level" && i+1<argc) { auto number=std::wstring(argv[++i]); size_t used=0; result.level=std::stoul(number,&used); if(used!=number.size() || result.level>=15)throw std::runtime_error("Course ID must be 0-14"); }
        else throw std::runtime_error("Unknown native engine argument");
    }
    if((result.coursePlay&&result.verify&&!result.verifyPlayer)||(result.verifyCourses&&result.coursePlay))
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
    std::vector<GpuMesh> worldMeshes;
    std::vector<ComPtr<ID3D11ShaderResourceView>> worldTextures;
    GpuMesh floor;
    uint64_t frames=0,draws=0;
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
cbuffer Camera : register(b0) { float4x4 worldViewProjection; float4x4 world; float4 tint; float4 properties; };
struct Input { float3 position:POSITION; float3 normal:NORMAL; float2 uv:TEXCOORD0; };
struct Output { float4 position:SV_POSITION; float3 normal:NORMAL; float2 uv:TEXCOORD0; };
Output vertexMain(Input v) { Output o; o.position=mul(float4(v.position,1),worldViewProjection); o.normal=mul(float4(v.normal,0),world).xyz; o.uv=v.uv; return o; }
Texture2D diffuse:register(t0); SamplerState linearSampler:register(s0);
float4 pixelMain(Output i):SV_TARGET { float4 color=diffuse.Sample(linearSampler,i.uv)*tint; clip(color.a-.02); float light=properties.x>.5 ? .4+.6*saturate(dot(normalize(i.normal),normalize(float3(-.4,-.6,1)))) : 1; return float4(color.rgb*light,color.a); }
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
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth=160; buffer.Usage=D3D11_USAGE_DEFAULT; buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
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
    void loadWorldGeometry(const World& world,const std::filesystem::path& assets) {
        for(const auto& source:world.meshes)worldMeshes.push_back(geometry(source.vertices,source.indices));
        std::map<std::string,ComPtr<ID3D11ShaderResourceView>> cache;
        uint8_t white[]{255,255,255,255};auto fallback=rgba(1,1,white);
        for(const auto& m:world.materials) {
            if(m.texture.empty())worldTextures.push_back(fallback);
            else {
                if(!cache.contains(m.texture))cache[m.texture]=image(assets/std::filesystem::path(m.texture));
                worldTextures.push_back(cache.at(m.texture));
            }
        }
    }
    void draw(GpuMesh& mesh,FXMMATRIX world,FXMMATRIX viewProjection,uint32_t first=0,uint32_t count=0,XMFLOAT4 tint={1,1,1,1},bool lit=true) {
        struct Camera { XMFLOAT4X4 worldViewProjection,world;XMFLOAT4 tint,properties; } camera;
        XMStoreFloat4x4(&camera.worldViewProjection,XMMatrixTranspose(world*viewProjection));
        // Inverse transpose is required for authored nonuniform resource scale.
        XMStoreFloat4x4(&camera.world,XMMatrixInverse(nullptr,world));camera.tint=tint;camera.properties={lit?1.f:0.f,0,0,0};
        context->UpdateSubresource(constants.Get(),0,nullptr,&camera,0,0);
        UINT stride=sizeof(DrawVertex),offset=0; context->IASetVertexBuffers(0,1,mesh.vertices.GetAddressOf(),&stride,&offset);
        context->IASetIndexBuffer(mesh.indices.Get(),DXGI_FORMAT_R32_UINT,0); context->PSSetShaderResources(0,1,mesh.texture.GetAddressOf());
        context->DrawIndexed(count?count:mesh.count,first,0); ++draws;
    }
    void frame(const Model& model,const Player& player,unsigned course,const World* world=nullptr,XMFLOAT3 focus={},const CoursePlayer* actor=nullptr) {
        const float color[]{.075f,.095f,.085f,1}; context->ClearRenderTargetView(target.Get(),color);
        context->ClearDepthStencilView(depth.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
        context->OMSetRenderTargets(1,target.GetAddressOf(),depth.Get()); D3D11_VIEWPORT viewport{0,0,1280,720,0,1}; context->RSSetViewports(1,&viewport);
        context->RSSetState(raster.Get()); context->IASetInputLayout(layout.Get()); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertexShader.Get(),nullptr,0); context->PSSetShader(pixelShader.Get(),nullptr,0);
        context->VSSetConstantBuffers(0,1,constants.GetAddressOf()); context->PSSetSamplers(0,1,sampler.GetAddressOf());
        context->PSSetConstantBuffers(0,1,constants.GetAddressOf());context->OMSetBlendState(blend.Get(),nullptr,0xffffffff);
        auto view=world ? XMMatrixLookAtLH(XMVectorSet(focus.x+3,-17,focus.z+6,1),XMVectorSet(focus.x,0,focus.z+1,1),XMVectorSet(0,0,1,0)) : XMMatrixLookAtLH(XMVectorSet(player.position.x+3,-5,2.3f,1),XMVectorSet(player.position.x,0,1,1),XMVectorSet(0,0,1,0));
        auto projection=XMMatrixPerspectiveFovLH(XM_PIDIV4,1280.f/720,.05f,250.f), viewProjection=view*projection;
        if(world) {
            for(const auto& instance:world->courses[course].instances) {
                if(std::abs(instance.transform._41-focus.x)>100)continue;
                auto& gpu=worldMeshes[instance.mesh];
                for(const auto& surface:world->meshes[instance.mesh].surfaces) {
                    const auto& m=world->materials[surface.material];gpu.texture=worldTextures[surface.material];auto tint=m.color;tint.w*=instance.opacity;
                    draw(gpu,XMLoadFloat4x4(&instance.transform),viewProjection,surface.first,surface.count,tint,m.lit);
                }
            }
        } else draw(floor,XMMatrixIdentity(),viewProjection);
        const Clip* clip=nullptr; for(const auto& c:model.clips)if(c.name==player.animation)clip=&c;
        auto pose=actor?actor->pose():samplePose(model,clip,player.animationTime);
        for(size_t i=0;i<meshes.size();++i) {
            auto vertices=skin(model.meshes[i],pose); D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(meshes[i].vertices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Cannot update native skinned geometry");
            memcpy(mapped.pData,vertices.data(),vertices.size()*sizeof(DrawVertex)); context->Unmap(meshes[i].vertices.Get(),0);
            auto origin=actor?actor->position:world?world->courses[course].spawn:player.position;
            auto rotation=actor&&actor->facing<0?XMMatrixRotationZ(XM_PI):XMMatrixIdentity();
            draw(meshes[i],rotation*XMMatrixTranslation(origin.x,origin.y,origin.z),viewProjection);
        }
        context->OMSetRenderTargets(0,nullptr,nullptr);
        ComPtr<IDXGISurface1> surface; check(swapchain->GetBuffer(0,IID_PPV_ARGS(&surface)),"Cannot access native HUD surface");
        HDC dc{}; check(surface->GetDC(FALSE,&dc),"Cannot draw native HUD"); auto old=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,RGB(224,237,205));
        std::wstring text=actor?L"INDEPENDENT NATIVE ENGINE / COURSE MOVEMENT\n":world?L"INDEPENDENT NATIVE ENGINE / STATIC COURSE INSPECTION\n":L"INDEPENDENT NATIVE ENGINE / CHARACTER PREVIEW\n";
        if(course<model.courses.size()) { auto& level=model.courses[course]; text+=std::wstring(level.name.begin(),level.name.end())+L" / "+std::to_wstring(level.chunks.size())+L" original chunk definitions loaded\n"; }
        text+=actor?L"A/D: move   Space: jump (hold for height)   R: restart   Tab: next course   Esc: close\n":world?L"A/D: pan camera   W/S: camera height   Tab: next course   Esc: close\n":L"A/D: move   Space: jump   Tab: course catalog   Esc: close\n";
        text+=actor?L"Static platforms and native movement. Dynamic obstacles, environment, progression and full-game behavior are pending.\n":world?L"Original static meshes and chunk links. Dynamic actors, collision, original lighting and gameplay are pending.\n":L"D3D11 / PC animation / native simulation arena. Use --course-preview for static course inspection.\n";
        text+=L"Time "+std::to_wstring(player.clock)+L"  X "+std::to_wstring(player.position.x)+L"  "+std::wstring(player.animation.begin(),player.animation.end());
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
}

int wmain(int argc,wchar_t** argv) {
    try {
        auto option=options(argc,argv); localPath(option.assets/L"character.dne"); auto model=loadModel(option.assets/L"character.dne");
        std::cout<<"[INDEPENDENT ENGINE] bones="<<model.bones.size()<<" courses="<<model.courses.size()<<" clips="<<model.clips.size()<<" no_xex=1 no_guest_memory=1\n";
        if(option.check) {
            if(std::filesystem::exists(option.assets/L"scripts.dnl")) {
                localPath(option.assets/L"scripts.dnl");auto chunks=loadScripts(option.assets/L"scripts.dnl");ScriptVm vm;
                for(auto& chunk:chunks)vm.validate(chunk.code,chunk.source+":"+chunk.actor+":"+chunk.field);
                std::cout<<"[NATIVE SCRIPT CHECK] compiled_chunks="<<chunks.size()<<" native_lua=1 scene_bindings_pending=1\n";
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
                    CoursePlayer actor(model,settings);actor.reset(world.courses[option.level].spawn);
                    for(int i=0;i<120;++i)actor.tick({},1.f/60,collisions[option.level].scene);
                    if(!actor.grounded)throw std::runtime_error("Native course player cannot settle on the starting platform");
                    std::cout<<"[NATIVE PLAYER CHECK] course="<<option.level<<" settled_z="<<actor.position.z<<" grounded=1\n";
                }
            }
            std::cout<<"[NATIVE CHECK] rest_pose_max_error="<<worst<<" passed=1\n"; return 0;
        }
        check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"Cannot initialize native image services");
        struct ComCleanup { ~ComCleanup(){CoUninitialize();} } cleanup;
        NativeWindow window(option.hidden); Renderer renderer(window.handle); renderer.load(model,option.assets);
        std::optional<World> world;
        if(option.coursePreview) { localPath(option.assets/L"world.dnw");world=loadWorld(option.assets/L"world.dnw");renderer.loadWorldGeometry(*world,option.assets); }
        std::unique_ptr<CoursePlayer> actor;std::vector<CourseCollision> collisions;float baseHeight=0;
        if(option.coursePlay) {
            localPath(option.assets/L"movement.dnp");localPath(option.assets/L"static-collision.dnc");
            auto settings=loadMovementSettings(option.assets/L"movement.dnp");collisions=loadCollision(option.assets/L"static-collision.dnc");
            actor=std::make_unique<CoursePlayer>(model,settings);actor->reset(world->courses[option.level].spawn);
            auto hit=collisions[option.level].scene.ray(actor->position,{0,0,-1},10);
            if(!hit)throw std::runtime_error("Native player start has no static floor");baseHeight=hit->position.z;
        }
        Player player; bool previousJump=false,previousTab=false,pendingJump=false,previousRestart=false,scriptHeld=false; unsigned course=option.level,captured=0;
        XMFLOAT3 focus=world?world->courses[course].spawn:XMFLOAT3{};
        float maximumHeight=0; uint64_t ticks=0; float accumulator=0;
        std::ofstream trace;
        if(option.verifyPlayer) { trace.open(option.evidence/L"positions.csv");trace<<"tick,time,course,x,y,z,vx,vz,grounded,jumps,horizontal,jump_pressed,jump_held\n"<<std::setprecision(10);if(!trace)throw std::runtime_error("Cannot create native player trace"); }
        std::array<uint64_t,15> courseDraws{};
        auto previous=std::chrono::steady_clock::now(),deadline=previous;
        while(window.events()) {
            auto now=std::chrono::steady_clock::now(); float elapsed=std::chrono::duration<float>(now-previous).count(); previous=now;
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
            if(tab&&!previousTab) { course=(course+1)%unsigned(model.courses.size());if(world)focus=world->courses[course].spawn;if(actor)actor->reset(focus); } previousTab=tab;
            if(restart&&!previousRestart&&actor)actor->reset(world->courses[course].spawn);previousRestart=restart;
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
                    player.position=actor->position;player.velocity=actor->velocity;player.clock=float(actor->clock);player.animation=actor->animation;
                    focus=actor->position;
                    if(option.verifyPlayer)trace<<ticks<<','<<actor->clock<<','<<course<<','<<actor->position.x<<','<<actor->position.y<<','<<actor->position.z<<','
                        <<actor->velocity.x<<','<<actor->velocity.z<<','<<actor->grounded<<','<<actor->jumps<<','<<controls.horizontal<<','<<controls.jumpPressed<<','<<controls.jumpHeld<<'\n';
                } else player.tick(world?Input{}:input,step);
                pendingJump=false;accumulator-=step;++ticks;maximumHeight=std::max(maximumHeight,player.position.z-baseHeight);
            }
            if(option.verifyCourses) { course=std::min(unsigned(player.clock),14u);focus=world->courses[course].inspection; }
            auto beforeDraws=renderer.draws;renderer.frame(model,player,course,world?&*world:nullptr,focus,actor.get());
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
            report<<",\"course_draws\":[";
            for(size_t i=0;i<courseDraws.size();++i) { if(i)report<<',';report<<courseDraws[i]; }report<<"]}\n";
            if(!report || !passed)throw std::runtime_error("Independent native verification did not complete");
        }
        return 0;
    } catch(const std::exception& error) { std::cerr<<"[NATIVE ENGINE ERROR] "<<error.what()<<'\n'; return 1; }
}
