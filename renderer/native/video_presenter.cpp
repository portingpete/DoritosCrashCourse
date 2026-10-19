#include "video_presenter.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace Doritos::Graphics {
using Microsoft::WRL::ComPtr;
namespace {
void checked(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(uint32_t(result)) + ")");
}
constexpr char shaderSource[] = R"(
sampler2D image : register(s0);
float4 texelAndEffects : register(c0); // xy reciprocal source size, z FXAA, w sharpness
float4 colorAdjust : register(c1); // brightness, contrast, reciprocal gamma, saturation
float3 sampleAt(float2 uv) { return tex2D(image,uv).rgb; }
float luminance(float3 color) { return dot(color,float3(.299,.587,.114)); }
float4 main(float2 uv:TEXCOORD0) : COLOR0 {
    float3 center=sampleAt(uv);
    float3 north=sampleAt(uv-float2(0,texelAndEffects.y));
    float3 south=sampleAt(uv+float2(0,texelAndEffects.y));
    float3 west=sampleAt(uv-float2(texelAndEffects.x,0));
    float3 east=sampleAt(uv+float2(texelAndEffects.x,0));
    float3 result=center;
    if(texelAndEffects.z>.5) {
        float l=luminance(center), n=luminance(north), s=luminance(south), w=luminance(west), e=luminance(east);
        float lo=min(l,min(min(n,s),min(w,e))), hi=max(l,max(max(n,s),max(w,e)));
        if(hi-lo>max(.0312,hi*.125)) {
            float2 direction=float2(-(n-s),w-e);
            float reduce=max((n+s+w+e)*.03125,.0078125);
            direction=clamp(direction/(min(abs(direction.x),abs(direction.y))+reduce),-8,8)*texelAndEffects.xy;
            float3 a=.5*(sampleAt(uv+direction*(-1.0/6.0))+sampleAt(uv+direction*(1.0/6.0)));
            float3 b=a*.5+.25*(sampleAt(uv+direction*(-.5))+sampleAt(uv+direction*.5));
            float bl=luminance(b);
            result=(bl<lo || bl>hi)?a:b;
        }
    }
    result+=texelAndEffects.w*(center-(north+south+west+east)*.25);
    float gray=luminance(result);
    result=lerp(gray.xxx,result,colorAdjust.w);
    result=(result-.5)*colorAdjust.y+.5+colorAdjust.x;
    return float4(pow(saturate(result),colorAdjust.zzz),1);
})";
struct Vertex { float x,y,z,w,u,v; };
constexpr DWORD quadFormat = D3DFVF_XYZRHW | D3DFVF_TEX1;
bool usesEffects(const VideoSettings& settings) {
    return settings.fxaa || settings.sharpness!=0 || settings.brightness!=0 ||
        settings.contrast!=1 || settings.gamma!=1 || settings.saturation!=1;
}
void quad(IDirect3DDevice9* device, VideoContentRect rectangle) {
    const float x=float(rectangle.x)-.5f,y=float(rectangle.y)-.5f;
    const float right=x+rectangle.width,bottom=y+rectangle.height;
    const Vertex vertices[]={{x,y,0,1,0,0},{right,y,0,1,1,0},{x,bottom,0,1,0,1},{right,bottom,0,1,1,1}};
    checked(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex)),"Draw native video presentation");
}
struct SavedState {
    IDirect3DDevice9* device;
    ComPtr<IDirect3DStateBlock9> state;
    std::array<ComPtr<IDirect3DSurface9>,4> targets;
    ComPtr<IDirect3DSurface9> depth;
    uint32_t targetCount=0;
    explicit SavedState(IDirect3DDevice9* value):device(value) {
        D3DCAPS9 caps{};
        checked(device->GetDeviceCaps(&caps),"Read presentation capabilities");
        targetCount=std::min<DWORD>(4,caps.NumSimultaneousRTs);
        checked(device->CreateStateBlock(D3DSBT_ALL,&state),"Retain native presentation state");
        checked(state->Capture(),"Capture native presentation state");
        for(uint32_t i=0;i<targetCount;++i) {
            const HRESULT result=device->GetRenderTarget(i,&targets[i]);
            if(result!=D3DERR_NOTFOUND) checked(result,"Retain presentation render target");
        }
        const HRESULT result=device->GetDepthStencilSurface(&depth);
        if(result!=D3DERR_NOTFOUND) checked(result,"Retain presentation depth target");
    }
    ~SavedState() {
        device->SetDepthStencilSurface(nullptr);
        for(uint32_t i=1;i<targetCount;++i) device->SetRenderTarget(i,nullptr);
        if(targets[0]) device->SetRenderTarget(0,targets[0].Get());
        for(uint32_t i=1;i<targetCount;++i) device->SetRenderTarget(i,targets[i].Get());
        device->SetDepthStencilSurface(depth.Get());
        if(state) state->Apply();
    }
};
}
VideoContentRect videoContentRect(uint32_t ow,uint32_t oh,uint32_t sw,uint32_t sh,ScalingMode mode) {
    if(!ow || !oh || !sw || !sh || ow>16384 || oh>16384) return {};
    uint32_t width=ow,height=oh;
    if(mode==ScalingMode::PreserveAspect) {
        const double scale=std::min(double(ow)/sw,double(oh)/sh);
        width=std::clamp<uint32_t>(uint32_t(std::lround(sw*scale)),1,ow);
        height=std::clamp<uint32_t>(uint32_t(std::lround(sh*scale)),1,oh);
    }
    return {LONG((ow-width)/2),LONG((oh-height)/2),LONG(width),LONG(height)};
}
struct VideoPresenter::Impl {
    ComPtr<IDirect3DTexture9> image, text;
    ComPtr<IDirect3DPixelShader9> shader;
    uint32_t width=0,height=0;
    std::string previousText;
    std::chrono::steady_clock::time_point textUpdated;
    void ensure(IDirect3DDevice9* device,uint32_t w,uint32_t h) {
        if(!shader) {
            ComPtr<ID3DBlob> compiled,errors;
            checked(D3DCompile(shaderSource,sizeof(shaderSource)-1,"native-video",nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&compiled,&errors),"Compile native video shader");
            checked(device->CreatePixelShader(static_cast<const DWORD*>(compiled->GetBufferPointer()),&shader),"Create native video shader");
        }
        if(!image || w!=width || h!=height) {
            ComPtr<IDirect3DTexture9> replacement;
            checked(device->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&replacement,nullptr),"Create native video sampling texture");
            image=std::move(replacement);
            width=w;height=h;
        }
    }
    void updateText(IDirect3DDevice9* device,double fps,double milliseconds) {
        const auto now=std::chrono::steady_clock::now();
        if(text && now-textUpdated<std::chrono::milliseconds(250)) return;
        char label[96];std::snprintf(label,sizeof(label),"%.0f FPS  |  %.1f ms",fps,milliseconds);
        if(previousText==label && text) return;
        if(!text) checked(device->CreateTexture(512,48,1,D3DUSAGE_DYNAMIC,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&text,nullptr),"Create frame counter texture");
        HDC dc=CreateCompatibleDC(nullptr);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=512;info.bmiHeader.biHeight=-48;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;
        HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        HFONT font=CreateFontW(-28,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Consolas");
        if(!dc || !bitmap || !pixels || !font) {
            if(font)DeleteObject(font);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);
            throw std::runtime_error("Cannot create frame counter font");
        }
        auto oldBitmap=SelectObject(dc,bitmap),oldFont=SelectObject(dc,font);
        std::memset(pixels,0,512*48*4);SetTextColor(dc,RGB(255,255,255));SetBkMode(dc,TRANSPARENT);
        TextOutA(dc,10,8,label,int(std::strlen(label)));GdiFlush();
        D3DLOCKED_RECT lock{};
        const HRESULT result=text->LockRect(0,&lock,nullptr,D3DLOCK_DISCARD);
        if(SUCCEEDED(result)) for(uint32_t y=0;y<48;++y) for(uint32_t x=0;x<512;++x) {
            const auto value=static_cast<uint32_t*>(pixels)[y*512+x];
            const uint32_t alpha=std::max({value&255,(value>>8)&255,(value>>16)&255});
            reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch)[x]=(alpha<<24)|0x00ffffff;
        }
        if(SUCCEEDED(result))text->UnlockRect(0);
        SelectObject(dc,oldFont);SelectObject(dc,oldBitmap);DeleteObject(font);DeleteObject(bitmap);DeleteDC(dc);
        checked(result,"Upload frame counter text");previousText=label;textUpdated=now;
    }
};
VideoPresenter::VideoPresenter():impl_(std::make_unique<Impl>()){}
VideoPresenter::~VideoPresenter()=default;
void VideoPresenter::prepare(IDirect3DDevice9Ex* device,uint32_t sourceWidth,uint32_t sourceHeight,
    const VideoSettings& settings) {
    if(usesEffects(settings)) impl_->ensure(device,sourceWidth,sourceHeight);
    if(settings.showFps) impl_->updateText(device,0,0);
}
VideoContentRect VideoPresenter::draw(IDirect3DDevice9Ex* device,IDirect3DSurface9* source,
    IDirect3DSurface9* output,const VideoSettings& settings,double fps,double milliseconds, HWND window) {
    D3DSURFACE_DESC src{},dst{};checked(source->GetDesc(&src),"Describe video source");checked(output->GetDesc(&dst),"Describe video output");
    auto rectangle=videoContentRect(dst.Width,dst.Height,src.Width,src.Height,settings.scaling);
    RECT client{};
    if(window && GetClientRect(window,&client) && client.right>0 && client.bottom>0) {
        const auto visible=videoContentRect(client.right,client.bottom,src.Width,src.Height,settings.scaling);
        rectangle={LONG(std::lround(double(visible.x)*dst.Width/client.right)),LONG(std::lround(double(visible.y)*dst.Height/client.bottom)),
            LONG(std::lround(double(visible.width)*dst.Width/client.right)),LONG(std::lround(double(visible.height)*dst.Height/client.bottom))};
    }
    const RECT target{rectangle.x,rectangle.y,rectangle.x+rectangle.width,rectangle.y+rectangle.height};
    const bool effects=usesEffects(settings);
    const bool letterbox=rectangle.width!=LONG(dst.Width) || rectangle.height!=LONG(dst.Height);
    ComPtr<IDirect3DSurface9> textureSurface;
    // Snapshot an aliased frame before clearing its bars, and snapshot effects
    // inputs before any output writes. The native scene remains untouched.
    if(effects || (source==output && letterbox)) {
        impl_->ensure(device,src.Width,src.Height);
        checked(impl_->image->GetSurfaceLevel(0,&textureSurface),"Get video sampling surface");
        checked(device->StretchRect(source,nullptr,textureSurface.Get(),nullptr,D3DTEXF_NONE),"Copy original frame for video effects");
        source=textureSurface.Get();
    }
    if(letterbox) checked(device->ColorFill(output,nullptr,0xff000000),"Clear video letterbox");
    if(!effects) {
        if(source!=output) checked(device->StretchRect(source,nullptr,output,&target,
            src.Width==uint32_t(rectangle.width) && src.Height==uint32_t(rectangle.height)?D3DTEXF_NONE:D3DTEXF_LINEAR),"Scale original video frame");
        if(!settings.showFps) return rectangle;
    }
    SavedState saved(device);
    checked(device->SetDepthStencilSurface(nullptr),"Detach video presentation depth");
    for(uint32_t i=1;i<saved.targetCount;++i) checked(device->SetRenderTarget(i,nullptr),"Detach video presentation MRT");
    checked(device->SetRenderTarget(0,output),"Select video presentation output");
    D3DVIEWPORT9 viewport{0,0,dst.Width,dst.Height,0,1};checked(device->SetViewport(&viewport),"Set video output viewport");
    checked(device->SetVertexShader(nullptr),"Set video quad vertex pipeline");checked(device->SetFVF(quadFormat),"Set video quad layout");
    for(const auto state:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_LIGHTING,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_FOGENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_DITHERENABLE,D3DRS_CLIPPLANEENABLE})checked(device->SetRenderState(state,0),"Disable unrelated video state");
    checked(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Set video culling");checked(device->SetRenderState(D3DRS_COLORWRITEENABLE,15),"Set video color channels");
    for(const auto state:{D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV})checked(device->SetSamplerState(0,state,D3DTADDRESS_CLAMP),"Clamp video image");
    for(const auto state:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})checked(device->SetSamplerState(0,state,D3DTEXF_LINEAR),"Filter video image");
    checked(device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE),"Disable video mips");checked(device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,0),"Set video color space");
    checked(device->SetSamplerState(0,D3DSAMP_MIPMAPLODBIAS,0),"Reset video mip bias");
    checked(device->BeginScene(),"Begin native video presentation");
    try {
        if(effects) {
            const float constants[]={1.0f/src.Width,1.0f/src.Height,settings.fxaa?1.0f:0.0f,settings.sharpness,
                settings.brightness,settings.contrast,1.0f/settings.gamma,settings.saturation};
            checked(device->SetPixelShader(impl_->shader.Get()),"Bind video effects shader");checked(device->SetPixelShaderConstantF(0,constants,2),"Set video effects controls");
            checked(device->SetTexture(0,impl_->image.Get()),"Bind video image");quad(device,rectangle);
        }
        if(settings.showFps) {
            impl_->updateText(device,fps,milliseconds);
            checked(device->SetPixelShader(nullptr),"Select frame counter pipeline");
            checked(device->SetTexture(0,impl_->text.Get()),"Bind frame counter");
            checked(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1),"Set frame counter color");
            checked(device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE),"Set frame counter color source");
            checked(device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1),"Set frame counter alpha");
            checked(device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE),"Set frame counter alpha source");
            checked(device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE),"Disable frame counter secondary stage");
            checked(device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE),"Blend frame counter");
            checked(device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA),"Set frame counter source blend");
            checked(device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA),"Set frame counter destination blend");
            const LONG w=std::min<LONG>(512,std::max<LONG>(200,dst.Width/4));
            quad(device,{16,16,w,LONG(w*48/512)});
        }
        checked(device->EndScene(),"End native video presentation");
    }catch(...){device->EndScene();throw;}
    return rectangle;
}
}
