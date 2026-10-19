#include "d3d9_backend.h"
#include "phase_times.h"
#include "indexed_draw.h"
#include "geometry_validation_cache.h"
#include "recent_frame_times.h"
#include "frame_pacer.h"
#include "capture_request.h"
#include "shadow_target.h"
#include "input_prompts.h"
#include "gui_projection.h"
#include "video_presenter.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>

namespace Doritos::Graphics {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char error[256];
        std::snprintf(error, sizeof(error), "%s failed: HRESULT 0x%08lX", operation, static_cast<unsigned long>(result));
        throw std::runtime_error(error);
    }
}
std::string featureDescription(const std::string& name, const ShaderKey& key) {
    return name + " [" + std::to_string(key[0]) + "," + std::to_string(key[1]) + "," + std::to_string(key[2]) + "]";
}
bool optionalAvatarFamily(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return name == "avatar/avatar_vs" || name == "avatar/avatar_ps" ||
        name == "billboard/avatarpose_xenon_vs" || name == "billboard/avatarpose_xenon_ps";
}
bool nullableDepthAllocation(HRESULT result) {
    return result == E_OUTOFMEMORY || result == D3DERR_OUTOFVIDEOMEMORY;
}
std::string shaderCandidates(std::span<const ShaderMatch> matches) {
    std::string result;
    for (size_t index = 0; index < std::min<size_t>(matches.size(), 16); ++index)
        result += (index ? "; " : ": ") + featureDescription(matches[index].shader->name, matches[index].variant->key);
    if (matches.size() > 16) result += "; additional aliases omitted";
    return result;
}
void validateCaptureDirectory(const std::filesystem::path& directory) {
    const auto drive = directory.root_name().wstring();
    if (drive.size() != 2 || drive[1] != L':')
        throw std::runtime_error("Native frame capture requires a local absolute directory");
    std::filesystem::path partial;
    for (const auto& component : directory) {
        partial /= component;
        const DWORD attributes = GetFileAttributesW(partial.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                throw std::runtime_error("Cannot inspect native frame capture directory");
        } else if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("Native frame capture directory contains a reparse point or file");
    }
    std::filesystem::create_directories(directory);
    partial.clear();
    for (const auto& component : directory) {
        partial /= component;
        const DWORD attributes = GetFileAttributesW(partial.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("Native frame capture directory verification failed");
    }
}
void writeFrameBMP(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const uint8_t> pixels) {
    static_assert(sizeof(BITMAPFILEHEADER) == 14 && sizeof(BITMAPINFOHEADER) == 40);
    if (pixels.size() != static_cast<size_t>(width) * height * 4 || pixels.size() > 256 * 1024 * 1024)
        throw std::runtime_error("Native captured frame exceeds bitmap bounds");
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = static_cast<DWORD>(file.bfOffBits + pixels.size());
    BITMAPINFOHEADER bitmap{};
    bitmap.biSize = sizeof(bitmap);
    bitmap.biWidth = static_cast<LONG>(width);
    bitmap.biHeight = -static_cast<LONG>(height); // Top-down BGRA from D3D9 readback.
    bitmap.biPlanes = 1;
    bitmap.biBitCount = 32;
    bitmap.biCompression = BI_RGB;
    bitmap.biSizeImage = static_cast<DWORD>(pixels.size());
    const HANDLE output = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (output == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create a fresh native captured frame file");
    auto write = [&](const void* bytes, DWORD count) {
        DWORD written = 0;
        return WriteFile(output, bytes, count, &written, nullptr) && written == count;
    };
    const bool complete = write(&file, sizeof(file)) && write(&bitmap, sizeof(bitmap)) &&
        write(pixels.data(), static_cast<DWORD>(pixels.size())) && FlushFileBuffers(output);
    const bool closed = CloseHandle(output) != 0;
    if (!complete || !closed) throw std::runtime_error("Native captured frame file write was incomplete");
}
}
struct D3D9Backend::Impl {
    struct Shader {
        ShaderStage stage;
        const ShaderVariant* variant;
        ComPtr<IDirect3DVertexShader9> vertex;
        ComPtr<IDirect3DPixelShader9> pixel;
        const ShaderVariant* original = nullptr;
        std::string name;
        std::string originalName;
        uint32_t handle = 0;
        uint32_t declaredSamplers = 0;
        std::array<uint32_t,16> samplerTextureTypes{};
    };
    struct DepthTargetState {
        ComPtr<IUnknown> identity;
        uint32_t width, height;
        bool originalClear = false, completed = false;
    };
    struct VertexBuffer { ComPtr<IDirect3DVertexBuffer9> resource; uint32_t stride; uint32_t count; std::vector<uint8_t> bytes; bool dynamic = false; uint64_t revision = 1; };
    struct IndexBuffer { ComPtr<IDirect3DIndexBuffer9> resource; uint32_t count; std::vector<uint16_t> indices; bool dynamic = false; uint64_t revision = 1; };
    struct Texture {
        ComPtr<IDirect3DTexture9> flat;
        ComPtr<IDirect3DCubeTexture9> cube;
        ComPtr<IDirect3DTexture9> stagingFlat;
        ComPtr<IDirect3DCubeTexture9> stagingCube;
        uint32_t width, height, levels;
        D3DFORMAT format;
        bool renderTarget;
        bool depthStencil = false;
        std::vector<bool> uploaded;
        uint64_t lastResolveSerial = 0, resolvedDraws = 0;
        std::shared_ptr<DepthTargetState> depthState;
        ComPtr<IDirect3DTexture9> inputPrompt;
        InputPromptKind inputPromptKind = InputPromptKind::None;
        bool inputPromptAlphaMask = false;
        bool guiScreen = false;
    };
    struct Surface {
        ComPtr<IDirect3DSurface9> resource;
        uint32_t parent = 0, face = 0, level = 0;
        // A real retained COM identity survives release of the parent handle.
        ComPtr<IUnknown> depthTextureIdentity;
        std::shared_ptr<DepthTargetState> depthState;
    };
    struct Declaration {
        ComPtr<IDirect3DVertexDeclaration9> resource;
        std::vector<D3DVERTEXELEMENT9> elements;
    };
    mutable std::recursive_mutex mutex;
    ComPtr<IDirect3D9Ex> d3d;
    ComPtr<IDirect3DDevice9Ex> device;
    ShaderCache windowsCache, xenonCache;
    std::map<uint32_t, Shader> shaders;
    std::map<uint32_t, VertexBuffer> vertexBuffers;
    std::map<uint32_t, IndexBuffer> indexBuffers;
    GeometryValidationCache<> geometryValidation;
    std::map<uint32_t, Declaration> declarations;
    std::map<uint32_t, Texture> textures;
    std::map<uint32_t, Surface> surfaces;
    ComPtr<IDirect3DSurface9> backbuffer;
    HWND window = nullptr;
    DWORD deviceThread = 0;
    uint32_t sceneWidth = 0, sceneHeight = 0;
    SceneResolutionResources sceneResources;
    std::function<void(uint32_t,uint32_t)> updateSceneCPU;
    D3DPRESENT_PARAMETERS presentation{};
    VideoSettings video;
    VideoPresenter videoPresenter;
    std::function<void(IDirect3DDevice9Ex*,IDirect3DSurface9*)> presentationUi;
    VideoContentRect presentationRect;
    std::array<std::map<D3DSAMPLERSTATETYPE,DWORD>,16> originalSamplerStates;
    uint32_t videoSamplerDirty=0xffff;
    bool restoreVideoSamplers=false;
    D3DCAPS9 videoCaps{};
    uint32_t presentationTexture = 0;
    uint32_t lastResolveTexture = 0;
    uint64_t resolveSerial = 0;
    uint32_t nextHandle = 1;
    uint32_t vertexShader = 0, pixelShader = 0, vertexBuffer = 0, indexBuffer = 0, declaration = 0;
    // These mirror successful native binds and retain the same real identities
    // which D3D9 itself holds. Guest token release does not invent an unbind.
    ComPtr<IUnknown> boundDepthTexture;
    std::array<ComPtr<IUnknown>,16> sampledDepthTextures;
    std::shared_ptr<DepthTargetState> boundDepthState;
    std::array<std::shared_ptr<DepthTargetState>,16> sampledDepthStates;
    uint32_t sampledDepthTextureCount = 0;
    struct SampledTexture {
        uint32_t handle = 0;
        ComPtr<IDirect3DBaseTexture9> original;
        ComPtr<IDirect3DTexture9> inputPrompt;
    };
    std::array<SampledTexture,16> sampledTextures;
    struct GuiTriangle {
        std::array<GuiProjectionVertex,3> vertices;
        std::array<float,4> viewport;
    };
    struct GuiSource {
        ComPtr<IUnknown> identity;
        bool invalid = false;
        std::vector<GuiTriangle> triangles;
        GuiPointerContext context;
    };
    struct GuiFrame {
        GuiPointerContext context;
        bool seen = false, invalid = false;
        std::vector<GuiTriangle> triangles;
        std::vector<GuiSource> sources;
    };
    GuiPointerContext guiContext;
    bool guiMonitorScreen = false;
    GuiFrame guiFrame, presentedGuiFrame;
    std::map<uint32_t,GuiSource> guiResolvedTextures;
    bool guiPresented = false;
    uint32_t guiProjectionDiagnostics = 0;
    std::optional<GuiPointerContext> reportedGuiContext;
    bool keyboardMousePrompts = false;
    struct PromptImage { uint32_t width, height; std::vector<uint32_t> pixels; };
    std::map<std::string,PromptImage> inputPromptPixels;
    bool inFrame = false;
    bool frameEnded = false;
    bool framePrepared = false, frameCleared = false, previousGamePresent = false;
    uint64_t frameDraws = 0;
    std::array<float, 4096> vertexConstants{}, pixelConstants{};
    std::array<bool, 1024> vertexConstantsWritten{}, pixelConstantsWritten{};
    RenderCounters counters;
    RecentFrameTimes recentFrameTimes;
    FramePacer framePacer;
    FrameWaiter frameWaiter;
    std::chrono::steady_clock::time_point lastPresented;
    double overlayMilliseconds=0;
    std::filesystem::path captureDirectory;
    uint64_t captureFrame = 1;
    bool captured = false;
    HANDLE captureRequestEvent = nullptr;
    bool reportFrameTimes = false;
    bool reportPhases = false, phaseFramePending = false, phaseGameFrame = false, previousPhaseGame = false;
    PhaseFrame phaseFrame;
    RecentPhaseTimes recentPhases;
    std::chrono::steady_clock::time_point lastPhaseFrame;
    uint32_t shaderIdentityDiagnostics = 0;
    uint32_t indexedDrawDiagnostics = 0;
    uint32_t immediateDrawDiagnostics = 0;
    uint32_t unpreparedClearDiagnostics = 0;
    uint32_t bonePaletteDiagnostics = 0;
    uint32_t batchPaletteDiagnostics = 0;
    HMODULE d3dx = nullptr;
    // Called only for a bounded, opt-in draw which the bridge has proved belongs
    // to its exact owned CJ Cal3D master, after actual constant uploads. Every
    // device query is read-only; HRESULTs distinguish unavailable state from0.
    void reportCJDrawState(uint32_t serial) const {
        if (!serial || serial > 128) return;
        struct LastErrorRestore { DWORD value = GetLastError(); ~LastErrorRestore() { SetLastError(value); } } lastErrorRestore;
        const auto vertex = shaders.find(vertexShader), pixel = shaders.find(pixelShader);
        ComPtr<IDirect3DVertexShader9> actualVertex;
        ComPtr<IDirect3DPixelShader9> actualPixel;
        const HRESULT vertexResult = device->GetVertexShader(&actualVertex);
        const HRESULT pixelResult = device->GetPixelShader(&actualPixel);
        const auto shaderReport = [&](const char* stage, uint32_t handle, const auto& found, HRESULT result, bool matches) {
            if (found == shaders.end()) {
                std::fprintf(stderr,"[NATIVE CJ SHADER] serial=%u stage=%s handle=%u verified_handle=0 actual_query_hr=%08lX\n",
                    serial,stage,handle,static_cast<unsigned long>(result));
                return;
            }
            const auto& shader = found->second;
            std::fprintf(stderr,
                "[NATIVE CJ SHADER] serial=%u stage=%s handle=%u name=%s original_name=%s key=[%u,%u,%u] original_key=[%u,%u,%u] actual_query_hr=%08lX actual_matches_verified_handle=%u declared_sampler_mask=%08X\n",
                serial,stage,handle,shader.name.c_str(),shader.originalName.c_str(),shader.variant->key[0],shader.variant->key[1],shader.variant->key[2],
                shader.original ? shader.original->key[0] : UINT32_MAX,shader.original ? shader.original->key[1] : UINT32_MAX,
                shader.original ? shader.original->key[2] : UINT32_MAX,static_cast<unsigned long>(result),matches ? 1u : 0u,shader.declaredSamplers);
        };
        shaderReport("vertex",vertexShader,vertex,vertexResult,SUCCEEDED(vertexResult) && vertex != shaders.end() && actualVertex.Get() == vertex->second.vertex.Get());
        shaderReport("pixel",pixelShader,pixel,pixelResult,SUCCEEDED(pixelResult) && pixel != shaders.end() && actualPixel.Get() == pixel->second.pixel.Get());
        if (pixel != shaders.end() && pixel->second.original) {
            const auto& shader = pixel->second;
            uint32_t reported = 0;
            for (const auto& native : shader.variant->parameters) {
                if (reported++ == 32) break;
                const ShaderParameter* source = nullptr;
                for (const auto& candidate : shader.original->parameters) if (candidate.id == native.id) { source = &candidate; break; }
                if (!source) continue;
                const uint32_t rows = std::min(native.registerCount,4u);
                std::fprintf(stderr,"[NATIVE CJ PIXEL PARAMETER] serial=%u id=%u original_first=%u original_count=%u native_first=%u native_count=%u reported_rows=%u\n",
                    serial,native.id,source->firstRegister,source->registerCount,native.firstRegister,native.registerCount,rows);
                for (uint32_t row = 0; row < rows; ++row) {
                    const uint32_t sourceRegister = source->firstRegister + row;
                    const bool sourceReadable = source->firstRegister < pixelConstantsWritten.size() &&
                        row < pixelConstantsWritten.size() - source->firstRegister;
                    const bool sourceWritten = sourceReadable && pixelConstantsWritten[sourceRegister];
                    std::array<float,4> actual{};
                    const HRESULT result = device->GetPixelShaderConstantF(native.firstRegister + row,actual.data(),1);
                    std::fprintf(stderr,"[NATIVE CJ PIXEL CONSTANT] serial=%u id=%u row=%u original_register=%u source_readable=%u source_written=%u native_register=%u actual_query_hr=%08lX",
                        serial,native.id,row,sourceRegister,sourceReadable ? 1u : 0u,sourceWritten ? 1u : 0u,native.firstRegister + row,static_cast<unsigned long>(result));
                    if (sourceWritten) {
                        const float* values = pixelConstants.data() + size_t(sourceRegister) * 4;
                        std::fprintf(stderr," original_value=[%.9g,%.9g,%.9g,%.9g]",values[0],values[1],values[2],values[3]);
                    }
                    if (SUCCEEDED(result)) std::fprintf(stderr," actual_native_value=[%.9g,%.9g,%.9g,%.9g]",actual[0],actual[1],actual[2],actual[3]);
                    std::fputc('\n',stderr);
                }
            }
        }
        const uint32_t samplerMask = 1u | (pixel != shaders.end() ? pixel->second.declaredSamplers : 0u);
        for (uint32_t slot = 0; slot < 16; ++slot) {
            if (!(samplerMask & (1u << slot))) continue;
            ComPtr<IDirect3DBaseTexture9> actual;
            ComPtr<IUnknown> identity;
            const HRESULT result = device->GetTexture(slot,&actual);
            const HRESULT identityResult = actual ? actual.As(&identity) : S_FALSE;
            uint32_t handle = 0;
            if (SUCCEEDED(result) && SUCCEEDED(identityResult) && identity) for (const auto& [candidate,value] : textures) {
                ComPtr<IUnknown> expected;
                const HRESULT expectedResult = value.flat ? value.flat.As(&expected) : value.cube ? value.cube.As(&expected) : E_POINTER;
                if (SUCCEEDED(expectedResult) && expected.Get() == identity.Get()) { handle = candidate; break; }
            }
            std::fprintf(stderr,"[NATIVE CJ SAMPLER] serial=%u slot=%u declared_by_pixel=%u actual_query_hr=%08lX actual_texture_present=%u identity_query_hr=%08lX mapped_native_handle=%u",
                serial,slot,pixel != shaders.end() && (pixel->second.declaredSamplers & (1u << slot)) ? 1u : 0u,
                static_cast<unsigned long>(result),actual ? 1u : 0u,static_cast<unsigned long>(identityResult),handle);
            if (handle) {
                const auto& texture = textures.at(handle);
                std::fprintf(stderr," mapped_width=%u mapped_height=%u mapped_levels=%u mapped_format=%u mapped_render_target=%u",
                    texture.width,texture.height,texture.levels,static_cast<unsigned>(texture.format),texture.renderTarget ? 1u : 0u);
            }
            std::fputc('\n',stderr);
            for (const auto state : {D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,D3DSAMP_SRGBTEXTURE}) {
                DWORD value = 0;
                const HRESULT samplerResult = device->GetSamplerState(slot,state,&value);
                std::fprintf(stderr,"[NATIVE CJ SAMPLER STATE] serial=%u slot=%u state=%u actual_query_hr=%08lX",
                    serial,slot,static_cast<unsigned>(state),static_cast<unsigned long>(samplerResult));
                if (SUCCEEDED(samplerResult)) std::fprintf(stderr," actual_value=%lu",static_cast<unsigned long>(value));
                std::fputc('\n',stderr);
            }
        }
        for (const auto state : {D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_SEPARATEALPHABLENDENABLE,
            D3DRS_SRCBLENDALPHA,D3DRS_DESTBLENDALPHA,D3DRS_ALPHATESTENABLE,D3DRS_ALPHAFUNC,D3DRS_ALPHAREF,D3DRS_ZWRITEENABLE,D3DRS_CULLMODE}) {
            DWORD value = 0;
            const HRESULT result = device->GetRenderState(state,&value);
            std::fprintf(stderr,"[NATIVE CJ RENDER STATE] serial=%u state=%u actual_query_hr=%08lX",serial,static_cast<unsigned>(state),static_cast<unsigned long>(result));
            if (SUCCEEDED(result)) std::fprintf(stderr," actual_value=%lu",static_cast<unsigned long>(value));
            std::fputc('\n',stderr);
        }
        std::fflush(stderr);
    }
    void requireIntegratedDepthSampling() const {
        if (!sampledDepthTextureCount) return;
        const auto shader = shaders.find(pixelShader);
        if (shader == shaders.end()) throw std::runtime_error("Depth sampling requires an actual native pixel shader");
        const auto& pixel = shader->second;
        const bool matched = pixel.stage == ShaderStage::Pixel && pixel.original && pixel.variant &&
            pixel.original->key == pixel.variant->key && !_stricmp(pixel.name.c_str(),pixel.originalName.c_str());
        for (uint32_t slot = 0; slot < sampledDepthStates.size(); ++slot) {
            const auto& sampled = sampledDepthStates[slot];
            if (!sampled) continue;
            if (!validateTitleShadowSample(pixel.stage,pixel.name,pixel.variant->key,pixel.declaredSamplers,
                slot,matched,sampled->completed,sampled->identity.Get() == boundDepthTexture.Get(),pixel.samplerTextureTypes[slot])) continue;
            ComPtr<IDirect3DBaseTexture9> actual;
            ComPtr<IUnknown> identity;
            check(device->GetTexture(slot,&actual), "Read actual original shadow sampling bind");
            if (!actual) throw std::runtime_error("Actual shadow sampler no longer holds its retained depth texture");
            check(actual.As(&identity), "Identify actual original shadow sampler");
            if (identity.Get() != sampled->identity.Get())
                throw std::runtime_error("Actual shadow sampler identity disagrees with retained completion ownership");
            ComPtr<IDirect3DTexture9> actualTexture;
            ComPtr<IDirect3DSurface9> sampledLevel, actualDepth;
            ComPtr<IUnknown> sampledLevelIdentity, actualDepthIdentity;
            check(actual.As(&actualTexture), "Read actual one-level shadow sampling texture");
            check(actualTexture->GetSurfaceLevel(0,&sampledLevel), "Read actual sampled shadow level0");
            check(sampledLevel.As(&sampledLevelIdentity), "Identify actual sampled shadow level0");
            const HRESULT depthResult = device->GetDepthStencilSurface(&actualDepth);
            if (depthResult != D3DERR_NOTFOUND) check(depthResult, "Read actual depth target before shadow consumer draw");
            if (actualDepth) {
                check(actualDepth.As(&actualDepthIdentity), "Identify actual depth target before shadow sampling");
                if (actualDepthIdentity.Get() == sampledLevelIdentity.Get())
                    throw std::runtime_error("Actual sampled shadow level0 remains attached as native depth target");
            }
        }
    }
    ~Impl() { if (captureRequestEvent) CloseHandle(captureRequestEvent); if (d3dx) FreeLibrary(d3dx); }
    // Observe only the primary monitor's real diffuse draw. Repeated wall
    // images use the same RTT with a different UV transform and are excluded.
    // Constants are read from the actual native shader registers after upload,
    // so matrix packing and animated camera/model transforms match the GPU.
    void captureGuiDraw(D3DPRIMITIVETYPE topology, std::span<const uint8_t> bytes,
        uint32_t stride, std::span<const uint16_t> indices, int32_t baseVertex,
        uint32_t startIndex, uint32_t triangleCount) {
        const auto texture = textures.find(sampledTextures[0].handle);
        if (!guiMonitorScreen || !guiContext.screen || texture == textures.end() || !texture->second.guiScreen) return;
        const auto vertex = shaders.find(vertexShader);
        if (vertex == shaders.end() || vertex->second.name != "multilayer/vs") return;
        const auto& shader = vertex->second;
        const ShaderParameter *matrix = nullptr, *uvTransform = nullptr;
        for (const auto& parameter : shader.variant->parameters) {
            if (parameter.id == 7 && parameter.registerCount == 4) matrix = &parameter;
            if (parameter.id == 65 && parameter.registerCount == 1) uvTransform = &parameter;
        }
        std::array<float,4> uv{};
        if (!uvTransform || FAILED(device->GetVertexShaderConstantF(uvTransform->firstRegister,uv.data(),1))) return;
        if (uv != std::array<float,4>{1,1,0,0}) return;
        ComPtr<IDirect3DSurface9> target;
        D3DSURFACE_DESC description{};
        if (FAILED(device->GetRenderTarget(0,&target)) || FAILED(target->GetDesc(&description)) ||
            description.Width != sceneWidth || description.Height != sceneHeight) return;
        ComPtr<IUnknown> identity;
        if (FAILED(target.As(&identity))) return;
        auto source = std::find_if(guiFrame.sources.begin(),guiFrame.sources.end(),
            [&](const auto& item) { return item.identity.Get() == identity.Get(); });
        if (source == guiFrame.sources.end()) {
            if (guiFrame.sources.size() >= 32) return;
            guiFrame.sources.push_back({std::move(identity),false,{},guiContext});
            source = guiFrame.sources.end() - 1;
        }
        auto& projectedSource = *source;
        guiFrame.context = guiContext;
        const auto pixel = shaders.find(pixelShader);
        // These source defines alter position or primary UV semantics.
        constexpr uint32_t unsupported = 0x40 | 0x3e00 | 0x4000 | 0x1000000 | 0x2000000;
        if (!matrix || topology != D3DPT_TRIANGLELIST || pixel == shaders.end() ||
            pixel->second.name != "multilayer/ps" || !(pixel->second.variant->key[1] & 2) ||
            !(shader.variant->key[2] & 8) || (shader.variant->key[2] & unsupported) ||
            triangleCount > 4096 || projectedSource.triangles.size() + triangleCount > 4096) {
            projectedSource.invalid = true; return;
        }
        const auto declarationFound = declarations.find(declaration);
        const D3DVERTEXELEMENT9 *position = nullptr, *texcoord = nullptr;
        if (declarationFound != declarations.end()) for (const auto& element : declarationFound->second.elements) {
            if (element.Stream != 0 || element.Method != D3DDECLMETHOD_DEFAULT) continue;
            if (element.Usage == D3DDECLUSAGE_POSITION && !element.UsageIndex && element.Type == D3DDECLTYPE_FLOAT3) position = &element;
            if (element.Usage == D3DDECLUSAGE_TEXCOORD && !element.UsageIndex && element.Type == D3DDECLTYPE_FLOAT2) texcoord = &element;
        }
        std::array<float,16> transform{};
        D3DVIEWPORT9 viewport{};
        if (!position || !texcoord || !stride || uint32_t(position->Offset) + 12 > stride ||
            uint32_t(texcoord->Offset) + 8 > stride ||
            FAILED(device->GetVertexShaderConstantF(matrix->firstRegister,transform.data(),4)) ||
            FAILED(device->GetViewport(&viewport)) || !viewport.Width || !viewport.Height ||
            uint64_t(viewport.X) + viewport.Width > description.Width ||
            uint64_t(viewport.Y) + viewport.Height > description.Height ||
            viewport.MinZ != 0 || viewport.MaxZ != 1) {
            projectedSource.invalid = true; return;
        }
        for (const float value : transform) if (!std::isfinite(value)) { projectedSource.invalid = true; return; }
        const float width = float(description.Width), height = float(description.Height);
        const float sx = viewport.Width / width, sy = viewport.Height / height;
        const float ox = (2.0f * viewport.X + viewport.Width) / width - 1;
        const float oy = 1 - (2.0f * viewport.Y + viewport.Height) / height;
        const std::array<float,4> bounds{viewport.X / width,viewport.Y / height,
            (viewport.X + viewport.Width) / width,(viewport.Y + viewport.Height) / height};
        projectedSource.triangles.reserve(projectedSource.triangles.size() + triangleCount);
        for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
            GuiTriangle projected{{},bounds};
            for (uint32_t corner = 0; corner < 3; ++corner) {
                const uint64_t index = uint64_t(startIndex) + uint64_t(triangle) * 3 + corner;
                if (index >= indices.size()) { projectedSource.invalid = true; return; }
                const int64_t vertexIndex = int64_t(baseVertex) + indices[size_t(index)];
                if (vertexIndex < 0 || uint64_t(vertexIndex) >= bytes.size() / stride) { projectedSource.invalid = true; return; }
                const auto* source = bytes.data() + size_t(vertexIndex) * stride;
                std::array<float,4> point{0,0,0,1};
                std::memcpy(point.data(),source + position->Offset,12);
                auto& output = projected.vertices[corner];
                std::memcpy(output.uv.data(),source + texcoord->Offset,8);
                for (size_t axis = 0; axis < 4; ++axis) for (size_t column = 0; column < 4; ++column)
                    output.clip[axis] += transform[column * 4 + axis] * point[column];
                output.clip[0] = output.clip[0] * sx + output.clip[3] * ox;
                output.clip[1] = output.clip[1] * sy + output.clip[3] * oy;
                for (const float value : output.clip) if (!std::isfinite(value)) { projectedSource.invalid = true; return; }
                for (const float value : output.uv) if (!std::isfinite(value)) { projectedSource.invalid = true; return; }
            }
            projectedSource.triangles.push_back(projected);
        }
        if (guiProjectionDiagnostics < 8 && (!reportedGuiContext || *reportedGuiContext != guiContext)) {
            ++guiProjectionDiagnostics;
            reportedGuiContext = guiContext;
            std::fprintf(stderr,"[NATIVE GUI POINTER] screen=%08X root=%08X generation=%016llX triangles=%u stride=%u viewport=%u,%u,%u,%u native_wvp=c%u native_uv=c%u vs_key=[%u,%u,%u] ps_key=[%u,%u,%u]\n",
                guiContext.screen,guiContext.root,static_cast<unsigned long long>(guiContext.rootHandle),triangleCount,stride,
                uint32_t(viewport.X),uint32_t(viewport.Y),uint32_t(viewport.Width),uint32_t(viewport.Height),matrix->firstRegister,uvTransform->firstRegister,
                shader.variant->key[0],shader.variant->key[1],shader.variant->key[2],
                pixel->second.variant->key[0],pixel->second.variant->key[1],pixel->second.variant->key[2]);
        }
    }
    void copyGuiResolve(uint32_t texture, IDirect3DSurface9* source, IDirect3DSurface9* destination) {
        if (guiFrame.sources.empty() && guiResolvedTextures.empty()) return;
        ComPtr<IUnknown> from, to;
        if (FAILED(source->QueryInterface(IID_PPV_ARGS(&from))) ||
            FAILED(destination->QueryInterface(IID_PPV_ARGS(&to)))) {
            guiResolvedTextures.erase(texture); return;
        }
        const auto found = std::find_if(guiFrame.sources.begin(),guiFrame.sources.end(),
            [&](const auto& item) { return item.identity.Get() == from.Get(); });
        std::optional<GuiSource> copied;
        if (found != guiFrame.sources.end()) copied = *found;
        else for (const auto& [handle, retained] : guiResolvedTextures)
            if (retained.identity.Get() == from.Get() && retained.context == guiContext) { copied = retained; break; }
        if (copied) {
            copied->identity = to;
            guiResolvedTextures[texture] = *copied;
        } else guiResolvedTextures.erase(texture);
        std::erase_if(guiFrame.sources,[&](const auto& item) { return item.identity.Get() == to.Get(); });
        if (copied && guiFrame.sources.size() < 32) guiFrame.sources.push_back(std::move(*copied));
    }
    void discardGuiTarget() {
        if (guiFrame.sources.empty() && guiResolvedTextures.empty()) return;
        ComPtr<IDirect3DSurface9> target;
        ComPtr<IUnknown> identity;
        if (FAILED(device->GetRenderTarget(0,&target)) || FAILED(target.As(&identity))) {
            guiFrame.sources.clear(); guiResolvedTextures.clear(); return;
        }
        std::erase_if(guiFrame.sources,[&](const auto& item) { return item.identity.Get() == identity.Get(); });
        std::erase_if(guiResolvedTextures,[&](const auto& item) { return item.second.identity.Get() == identity.Get(); });
    }
    void selectGuiPresentation(IDirect3DSurface9* source) {
        ComPtr<IUnknown> identity;
        if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&identity)))) {
            const auto found = std::find_if(guiFrame.sources.begin(),guiFrame.sources.end(),
                [&](const auto& item) { return item.identity.Get() == identity.Get(); });
            if (found != guiFrame.sources.end()) {
                guiFrame.seen = true;
                guiFrame.invalid = found->invalid;
                guiFrame.triangles = std::move(found->triangles);
            } else {
                // Original FrameEnd resolves the next texture, presents the
                // previously selected texture, then swaps front/back. Retain
                // that exact texture's geometry until its pixels are replaced.
                const auto retained = guiResolvedTextures.find(presentationTexture);
                if (retained != guiResolvedTextures.end() && retained->second.identity.Get() == identity.Get() &&
                    retained->second.context == guiContext) {
                    guiFrame.seen = true;
                    guiFrame.invalid = retained->second.invalid;
                    guiFrame.triangles = retained->second.triangles;
                }
            }
        }
        guiFrame.sources.clear();
    }
    void requireDevice() const { if (!device) throw std::runtime_error("Native renderer has no initialized hardware device"); }
    void applyVideoSamplers() {
        const bool overrides=video.filtering!=TextureFiltering::Original || video.anisotropy || video.mipBias;
        if(!overrides && !restoreVideoSamplers) {videoSamplerDirty=0;return;}
        for(uint32_t sampler=0;sampler<16;++sampler) {
            if(!(videoSamplerDirty&(1u<<sampler))) continue;
            auto states=originalSamplerStates[sampler];
            const auto texture=textures.find(sampledTextures[sampler].handle);
            // Preserve point-sampled glyphs, GUI render textures and shadow maps.
            const bool eligible=texture!=textures.end() && !texture->second.renderTarget &&
                !texture->second.depthStencil && !sampledTextures[sampler].inputPrompt &&
                states.contains(D3DSAMP_MINFILTER) && states[D3DSAMP_MINFILTER]!=D3DTEXF_POINT;
            if(eligible) {
                if(video.filtering!=TextureFiltering::Original) {
                    states[D3DSAMP_MINFILTER]=D3DTEXF_LINEAR;states[D3DSAMP_MAGFILTER]=D3DTEXF_LINEAR;
                    states[D3DSAMP_MIPFILTER]=video.filtering==TextureFiltering::Trilinear?D3DTEXF_LINEAR:D3DTEXF_POINT;
                }
                if(video.anisotropy) {
                    states[D3DSAMP_MINFILTER]=D3DTEXF_ANISOTROPIC;
                    states[D3DSAMP_MAXANISOTROPY]=std::min<uint32_t>(video.anisotropy,videoCaps.MaxAnisotropy);
                }
                if(video.mipBias) states[D3DSAMP_MIPMAPLODBIAS]=std::bit_cast<uint32_t>(video.mipBias);
            }
            for(const auto& [kind,value]:states) check(device->SetSamplerState(sampler,kind,value),"Apply native video texture filtering");
        }
        videoSamplerDirty=0;restoreVideoSamplers=false;
    }
    const PromptImage& promptPixels(const char* name) {
        const auto cached = inputPromptPixels.find(name);
        if (cached != inputPromptPixels.end()) return cached->second;
        const InputPrompts::Descriptor* source = nullptr;
        for (const auto& candidate : InputPrompts::descriptors)
            if (!std::strcmp(candidate.name,name)) { source = &candidate; break; }
        if (!source || !source->width || !source->height || source->width > 1024 ||
            source->height > 256 || source->size > UINT32_MAX)
            throw std::runtime_error("Checked embedded Kenney input prompt is unavailable");
        if (!d3dx) d3dx = LoadLibraryExW(L"d3dx9_43.dll",nullptr,
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!d3dx) throw std::runtime_error("Native input prompts require packaged or installed d3dx9_43.dll");
        using CreateFromMemory = HRESULT(WINAPI*)(IDirect3DDevice9*,const void*,UINT,UINT,UINT,UINT,
            DWORD,D3DFORMAT,D3DPOOL,DWORD,DWORD,D3DCOLOR,void*,PALETTEENTRY*,IDirect3DTexture9**);
        const auto decode = reinterpret_cast<CreateFromMemory>(GetProcAddress(d3dx,"D3DXCreateTextureFromFileInMemoryEx"));
        if (!decode) throw std::runtime_error("Native Kenney PNG decoder entry is unavailable");
        ComPtr<IDirect3DTexture9> decoded;
        // D3D9Ex does not support MANAGED resources. Decode into a lockable
        // SYSTEMMEM texture; GPU prompts use independently uploaded DEFAULTs.
        check(decode(device.Get(),source->bytes,static_cast<UINT>(source->size),source->width,source->height,1,0,
            D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,1,1,0,nullptr,nullptr,&decoded), "Decode checked embedded Kenney PNG");
        D3DLOCKED_RECT locked{};
        check(decoded->LockRect(0,&locked,nullptr,D3DLOCK_READONLY), "Read decoded Kenney PNG pixels");
        if (locked.Pitch < 0 || uint32_t(locked.Pitch) < source->width * 4) {
            decoded->UnlockRect(0);
            throw std::runtime_error("Decoded Kenney PNG row is shorter than its checked frame");
        }
        PromptImage image{source->width,source->height,std::vector<uint32_t>(size_t(source->width) * source->height)};
        for (uint32_t row = 0; row < source->height; ++row)
            std::memcpy(image.pixels.data() + size_t(row) * source->width,
                static_cast<const uint8_t*>(locked.pBits) + size_t(row) * locked.Pitch,size_t(source->width) * 4);
        check(decoded->UnlockRect(0), "Finish reading checked Kenney PNG pixels");
        return inputPromptPixels.emplace(name,std::move(image)).first->second;
    }
    ComPtr<IDirect3DTexture9> createInputPrompt(const Texture& original, InputPromptKind kind, bool alphaMask) {
        if (original.cube || !original.flat || original.renderTarget || original.depthStencil ||
            !original.width || !original.height || original.width > 2048 || original.height > 1024)
            throw std::runtime_error("Native input prompt requires a bounded ordinary 2D texture");
        std::vector<uint32_t> pixels(size_t(original.width) * original.height,0);
        const auto copyIcon = [&](const char* name, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
            if (!width || !height || x > original.width || width > original.width - x ||
                y > original.height || height > original.height - y)
                throw std::runtime_error("Kenney prompt placement exceeds its original texture frame");
            const auto& source = promptPixels(name);
            for (uint32_t row = 0; row < height; ++row)
                for (uint32_t column = 0; column < width; ++column)
                    pixels[size_t(y + row) * original.width + x + column] =
                        source.pixels[size_t(uint64_t(row) * source.height / height) * source.width +
                            uint64_t(column) * source.width / width];
        };
        const char* icon = nullptr;
        switch (kind) {
        case InputPromptKind::Space: icon = "keyboard_space"; break;
        case InputPromptKind::Enter: icon = "keyboard_enter"; break;
        case InputPromptKind::Escape: icon = "keyboard_escape"; break;
        case InputPromptKind::X: icon = "keyboard_x"; break;
        case InputPromptKind::Y: icon = "keyboard_r"; break;
        case InputPromptKind::Q: icon = "keyboard_q"; break;
        case InputPromptKind::E: icon = "keyboard_e"; break;
        case InputPromptKind::Shift: icon = "keyboard_shift"; break;
        case InputPromptKind::Ctrl: icon = "keyboard_ctrl"; break;
        case InputPromptKind::Pause: icon = "keyboard_p"; break;
        case InputPromptKind::Tab: icon = "keyboard_backspace"; break;
        case InputPromptKind::ButtonsAtlas:
            if (original.width != 512 || original.height != 256)
                throw std::runtime_error("Original Xbox button atlas differs from its verified 8 by 4 layout");
            icon = "keyboard_buttons_atlas";
            break;
        case InputPromptKind::ControlsDiagram:
            if (original.width != 1024 || original.height != 256)
                throw std::runtime_error("Original controller guide differs from its verified 1024 by 256 frame");
            icon = "keyboard_controls_diagram";
            break;
        default: throw std::runtime_error("Unsupported native input prompt image kind");
        }
        if (icon) copyIcon(icon,0,0,original.width,original.height);
        if (alphaMask) for (auto& pixel : pixels) {
            const uint32_t alpha = pixel >> 24;
            pixel = 0xff000000u | alpha * 0x00010101u;
        }
        ComPtr<IDirect3DTexture9> staging, replacement;
        check(device->CreateTexture(original.width,original.height,original.levels,0,D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,&staging,nullptr), "Create Kenney prompt upload staging");
        check(device->CreateTexture(original.width,original.height,original.levels,0,D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,&replacement,nullptr), "Create actual native Kenney prompt texture");
        D3DLOCKED_RECT locked{};
        check(staging->LockRect(0,&locked,nullptr,0), "Lock native Kenney prompt upload");
        if (locked.Pitch < 0 || uint32_t(locked.Pitch) < original.width * 4) {
            staging->UnlockRect(0);
            throw std::runtime_error("Native Kenney prompt upload row is shorter than its original frame");
        }
        for (uint32_t row = 0; row < original.height; ++row)
            std::memcpy(static_cast<uint8_t*>(locked.pBits) + size_t(row) * locked.Pitch,
                pixels.data() + size_t(row) * original.width,size_t(original.width) * 4);
        check(staging->UnlockRect(0), "Unlock native Kenney prompt upload");
        if (original.levels > 1) {
            using FilterTexture = HRESULT(WINAPI*)(IDirect3DBaseTexture9*,const PALETTEENTRY*,UINT,DWORD);
            const auto filter = reinterpret_cast<FilterTexture>(GetProcAddress(d3dx,"D3DXFilterTexture"));
            if (!filter) throw std::runtime_error("Native Kenney prompt mip generator is unavailable");
            check(filter(staging.Get(),nullptr,0,5), "Generate native Kenney prompt BOX mip chain");
        }
        check(staging->AddDirtyRect(nullptr), "Mark native Kenney prompt mip chain dirty");
        check(device->UpdateTexture(staging.Get(),replacement.Get()), "Upload actual native Kenney prompt texture");
        return replacement;
    }
    void refreshPromptSampler(uint32_t sampler, bool enabled) {
        const auto& sampled = sampledTextures[sampler];
        if (!sampled.inputPrompt) return;
        IDirect3DBaseTexture9* image = enabled ? sampled.inputPrompt.Get() : sampled.original.Get();
        check(device->SetTexture(sampler,image), "Refresh native prompt for the active input device");
    }
    uint32_t handle() {
        if (nextHandle == UINT32_MAX) throw std::runtime_error("Native graphics resource handles exhausted");
        return nextHandle++;
    }
    void applyGuestConstants(Shader& shader, bool requireComplete, const TitleBoneDraw* boneDraw = nullptr,
        const TitleBatchDraw* batchDraw = nullptr) {
        PhaseScope phase(phaseFrame,RenderPhase::Constants,reportPhases);
        if (!shader.original) return;
        const auto& values = shader.stage == ShaderStage::Vertex ? vertexConstants : pixelConstants;
        const auto& written = shader.stage == ShaderStage::Vertex ? vertexConstantsWritten : pixelConstantsWritten;
        std::string missingFailure, writtenRanges;
        uint32_t missingDiagnostics = 0;
        for (const auto& parameter : shader.variant->parameters) {
            const ShaderParameter* original = nullptr;
            for (const auto& source : shader.original->parameters) if (source.id == parameter.id) { original = &source; break; }
            if (!original || parameter.registerCount != original->registerCount ||
                original->firstRegister > written.size() || original->registerCount > written.size() - original->firstRegister)
                throw std::runtime_error("Original/native shader parameter bindings cannot be matched exactly");
            uint32_t registerCount = original->registerCount;
            if (boneDraw && parameter.id == 81) {
                registerCount = titleBonePalettePrefix(shader.stage,shader.name,shader.variant->key,
                    *original,parameter,values,written,boneDraw->matrices);
                if (bonePaletteDiagnostics++ < 16) {
                    std::fprintf(stderr,"[NATIVE BONE PALETTE] name=%s key=[%u,%u,%u] original_first=%u native_first=%u active_bones=%u uploaded_rows=%u reflected_rows=%u\n",
                        shader.name.c_str(),shader.variant->key[0],shader.variant->key[1],shader.variant->key[2],
                        original->firstRegister,parameter.firstRegister,boneDraw->geometry.boneCount,registerCount,parameter.registerCount);
                    std::fflush(stderr);
                }
            } else if (batchDraw && parameter.id == 81) {
                registerCount = titleBatchCurrentPalette(shader.stage,shader.name,shader.variant->key,
                    *original,parameter,values,written,*batchDraw);
                if (batchPaletteDiagnostics++ < 16) {
                    std::fprintf(stderr,"[NATIVE BATCH PALETTE] name=%s key=[%u,%u,%u] original_first=%u native_first=%u active_instances=%u uploaded_rows=%u reflected_rows=%u\n",
                        shader.name.c_str(),shader.variant->key[0],shader.variant->key[1],shader.variant->key[2],
                        original->firstRegister,parameter.firstRegister,batchDraw->geometry.activeInstances,registerCount,parameter.registerCount);
                    std::fflush(stderr);
                }
            }
            bool complete = true;
            for (uint32_t index = 0; index < registerCount; ++index) complete &= written[original->firstRegister + index];
            if (!complete) {
                if (requireComplete) {
                    if (writtenRanges.empty()) {
                        uint32_t ranges = 0;
                        for (uint32_t first = 0; first < written.size();) {
                            if (!written[first]) { ++first; continue; }
                            uint32_t end = first + 1;
                            while (end < written.size() && written[end]) ++end;
                            if (ranges++ == 32) { writtenRanges += ";additional_ranges_omitted"; break; }
                            writtenRanges += (writtenRanges.empty() ? "" : ";") + std::to_string(first) + ":" + std::to_string(end);
                            first = end;
                        }
                        if (writtenRanges.empty()) writtenRanges = "none";
                    }
                    uint32_t missingFirst = UINT32_MAX, missingCount = 0;
                    for (uint32_t index = 0; index < original->registerCount; ++index)
                        if (!written[original->firstRegister + index]) {
                            missingFirst = std::min(missingFirst,original->firstRegister + index); ++missingCount;
                        }
                    char diagnostic[1024];
                    std::snprintf(diagnostic,sizeof(diagnostic),
                        "stage=%s name=\"%s\" key=[%u,%u,%u] native_handle=%u parameter=%u original=%u:%u native=%u:%u missing_first=%u missing_count=%u written=%s",
                        shader.stage == ShaderStage::Vertex ? "vertex" : "pixel",shader.name.c_str(),
                        shader.variant->key[0],shader.variant->key[1],shader.variant->key[2],shader.handle,parameter.id,
                        original->firstRegister,original->firstRegister + original->registerCount,
                        parameter.firstRegister,parameter.firstRegister + parameter.registerCount,missingFirst,missingCount,writtenRanges.c_str());
                    if (missingFailure.empty()) missingFailure = diagnostic;
                    if (missingDiagnostics++ < 32) std::fprintf(stderr,"[NATIVE MISSING CONSTANT] %s\n",diagnostic);
                }
                continue;
            }
            const float* data = values.data() + original->firstRegister * 4;
            if (shader.stage == ShaderStage::Vertex)
                check(device->SetVertexShaderConstantF(parameter.firstRegister, data, registerCount), "Map original vertex constants to native registers");
            else check(device->SetPixelShaderConstantF(parameter.firstRegister, data, registerCount), "Map original pixel constants to native registers");
        }
        if (!missingFailure.empty()) {
            std::fflush(stderr);
            throw std::runtime_error("Original title did not upload required native shader constants: " + missingFailure);
        }
    }
};
D3D9Backend::D3D9Backend() : impl_(std::make_unique<Impl>()) {}
D3D9Backend::~D3D9Backend() = default;
void D3D9Backend::initialize(HWND window, uint32_t width, uint32_t height) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->device) throw std::runtime_error("Native graphics device is already initialized");
    if (!IsWindow(window) || !width || !height || width > 8192 || height > 8192)
        throw std::runtime_error("Invalid native graphics window or dimensions");
    wchar_t capture[32768];
    const DWORD captureLength = GetEnvironmentVariableW(L"DORITOS_CAPTURE_DIR", capture, static_cast<DWORD>(std::size(capture)));
    if (captureLength >= std::size(capture)) throw std::runtime_error("Native frame capture directory exceeds bounds");
    if (captureLength) {
        const std::filesystem::path requested(capture);
        if (!requested.is_absolute()) throw std::runtime_error("Native frame capture requires an absolute directory");
        impl_->captureDirectory = requested.lexically_normal();
    }
    wchar_t captureFrame[24]{};
    const DWORD frameLength = GetEnvironmentVariableW(L"DORITOS_CAPTURE_FRAME",captureFrame,static_cast<DWORD>(std::size(captureFrame)));
    if (frameLength >= std::size(captureFrame)) throw std::runtime_error("Native capture frame number exceeds bounds");
    if (frameLength) {
        uint64_t value = 0;
        for (DWORD index = 0; index < frameLength; ++index) {
            if (captureFrame[index] < L'0' || captureFrame[index] > L'9' || value > (1000000u - (captureFrame[index] - L'0')) / 10)
                throw std::runtime_error("Native capture frame must be a positive integer at most1000000");
            value = value * 10 + (captureFrame[index] - L'0');
        }
        if (!value) throw std::runtime_error("Native capture frame must be positive");
        impl_->captureFrame = value;
    }
    wchar_t onDemand[2]{};
    if (GetEnvironmentVariableW(L"DORITOS_CAPTURE_ON_DEMAND", onDemand, 2) == 1 && onDemand[0] == L'1') {
        if (impl_->captureDirectory.empty()) throw std::runtime_error("On-demand native capture requires DORITOS_CAPTURE_DIR");
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            throw std::runtime_error("Cannot read native capture process creation identity");
        const uint64_t creationTime = uint64_t(created.dwHighDateTime) << 32 | created.dwLowDateTime;
        const auto name = captureRequestEventName(GetCurrentProcessId(), creationTime);
        SetLastError(ERROR_SUCCESS);
        impl_->captureRequestEvent = CreateEventW(nullptr, FALSE, FALSE, name.c_str());
        if (!impl_->captureRequestEvent || GetLastError() == ERROR_ALREADY_EXISTS)
            throw std::runtime_error("Cannot create a fresh native capture request event");
        std::fwprintf(stderr, L"[NATIVE CAPTURE SERVICE] pid=%lu creation_time_100ns=%llu event=\"%ls\"\n",
            static_cast<unsigned long>(GetCurrentProcessId()), static_cast<unsigned long long>(creationTime), name.c_str());
        std::fflush(stderr);
    }
    wchar_t frameReport[2]{};
    impl_->reportFrameTimes = GetEnvironmentVariableW(L"DORITOS_FPS_REPORT", frameReport, 2) == 1 && frameReport[0] == L'1';
    impl_->reportPhases = GetEnvironmentVariableW(L"DORITOS_PHASE_REPORT", frameReport, 2) == 1 && frameReport[0] == L'1';
    check(Direct3DCreate9Ex(D3D_SDK_VERSION, &impl_->d3d), "Direct3DCreate9Ex");
    D3DCAPS9 caps{};
    check(impl_->d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps), "GetDeviceCaps");
    if (caps.VertexShaderVersion < D3DVS_VERSION(3, 0) || caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
        throw std::runtime_error("Native hardware does not support required Shader Model 3");
    D3DDISPLAYMODE presentationDisplay{};
    check(impl_->d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &presentationDisplay), "Get native presentation display mode");
    D3DPRESENT_PARAMETERS parameters{};
    parameters.BackBufferWidth = width;
    parameters.BackBufferHeight = height;
    parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    parameters.BackBufferCount = 1;
    parameters.MultiSampleType = D3DMULTISAMPLE_NONE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.Windowed = TRUE;
    parameters.EnableAutoDepthStencil = TRUE;
    parameters.AutoDepthStencilFormat = D3DFMT_D24S8;
    // A known slow remote display must not impose its refresh rate on the
    // native 60 FPS render cap. The desktop still scans out at its own rate.
    parameters.PresentationInterval = presentationDisplay.RefreshRate && presentationDisplay.RefreshRate < 60 ?
        D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
    check(impl_->d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED,
        &parameters, nullptr, &impl_->device), "CreateDeviceEx");
    check(impl_->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &impl_->backbuffer), "Get native backbuffer");
    D3DDEVICE_CREATION_PARAMETERS creation{};
    check(impl_->device->GetCreationParameters(&creation), "Get native device adapter");
    D3DADAPTER_IDENTIFIER9 adapter{};
    check(impl_->d3d->GetAdapterIdentifier(creation.AdapterOrdinal, 0, &adapter), "Get native adapter identifier");
    D3DDISPLAYMODE display{};
    check(impl_->d3d->GetAdapterDisplayMode(creation.AdapterOrdinal, &display), "Get native adapter display mode");
    MONITORINFOEXW adapterMonitor{}, windowMonitor{};
    adapterMonitor.cbSize = sizeof(adapterMonitor);
    windowMonitor.cbSize = sizeof(windowMonitor);
    const HMONITOR adapterScreen = impl_->d3d->GetAdapterMonitor(creation.AdapterOrdinal);
    const HMONITOR windowScreen = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    const bool adapterScreenKnown = adapterScreen && GetMonitorInfoW(adapterScreen, &adapterMonitor);
    const bool windowScreenKnown = windowScreen && GetMonitorInfoW(windowScreen, &windowMonitor);
    std::fprintf(stderr,
        "[NATIVE DISPLAY] adapter=%u gpu=\"%.*s\" device=\"%.*s\" mode=%ux%u@%u adapter_monitor=\"%ls\" window_monitor=\"%ls\" vsync=%s render_cap=60 backbuffer=%ux%u display_refresh_is_separate=true\n",
        creation.AdapterOrdinal, static_cast<int>(sizeof(adapter.Description)), adapter.Description,
        static_cast<int>(sizeof(adapter.DeviceName)), adapter.DeviceName,
        display.Width, display.Height, display.RefreshRate,
        adapterScreenKnown ? adapterMonitor.szDevice : L"unavailable",
        windowScreenKnown ? windowMonitor.szDevice : L"unavailable",
        parameters.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? "immediate" : "one", width, height);
    std::fflush(stderr);
    impl_->counters.width = width;
    impl_->counters.height = height;
    impl_->window = window;
    impl_->deviceThread = GetCurrentThreadId();
    impl_->sceneWidth = width; impl_->sceneHeight = height;
    impl_->videoCaps=caps;
    for(uint32_t sampler=0;sampler<16;++sampler) for(const auto kind:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,D3DSAMP_MAXANISOTROPY,D3DSAMP_MIPMAPLODBIAS}) {
        DWORD value=0;check(impl_->device->GetSamplerState(sampler,kind,&value),"Retain original default texture filter");
        impl_->originalSamplerStates[sampler][kind]=value;
    }
    impl_->presentation = parameters;
    impl_->presentation.BackBufferWidth = width; impl_->presentation.BackBufferHeight = height;
    impl_->presentation.BackBufferCount = 1;
    impl_->presentationRect = {0,0,LONG(width),LONG(height)};
    impl_->framePacer.reset();
}
void D3D9Backend::setSceneResolutionResources(const SceneResolutionResources& resources,
    std::function<void(uint32_t,uint32_t)> updateCPU) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (!updateCPU || !resources.colorSurface || !resources.depthSurface ||
        resources.colorSurface == resources.depthSurface || !resources.resolveTextures[0] ||
        !resources.resolveTextures[1] || resources.resolveTextures[0] == resources.resolveTextures[1])
        throw std::runtime_error("Primary scene resolution requires distinct owned color, depth and resolve resources");
    if (impl_->updateSceneCPU && impl_->sceneResources != resources)
        throw std::runtime_error("Cannot replace the original primary scene resource ownership");
    const auto color = surfaceDescription(resources.colorSurface), depth = surfaceDescription(resources.depthSurface);
    if (impl_->surfaces.at(resources.colorSurface).parent || impl_->surfaces.at(resources.depthSurface).parent ||
        color.Width != impl_->sceneWidth || color.Height != impl_->sceneHeight ||
        depth.Width != color.Width || depth.Height != color.Height || depth.Format != D3DFMT_D24S8 ||
        color.Usage != D3DUSAGE_RENDERTARGET || depth.Usage != D3DUSAGE_DEPTHSTENCIL ||
        color.MultiSampleType != D3DMULTISAMPLE_NONE || depth.MultiSampleType != D3DMULTISAMPLE_NONE)
        throw std::runtime_error("Original primary scene dimensions, ownership, samples or formats disagree");
    for (const uint32_t handle : resources.resolveTextures) {
        const auto& texture = impl_->textures.at(handle);
        const auto description = textureDescription(handle,0);
        if (!texture.flat || texture.cube || !texture.renderTarget || texture.depthStencil || texture.levels != 1 ||
            description.Width != color.Width || description.Height != color.Height || description.Format != color.Format)
            throw std::runtime_error("Original primary scene resolve is not an owned full-size color render texture");
    }
    impl_->sceneResources = resources;
    impl_->updateSceneCPU = std::move(updateCPU);
}
std::pair<uint32_t,uint32_t> D3D9Backend::sceneResolution() const {
    std::lock_guard lock(impl_->mutex);
    return {impl_->sceneWidth,impl_->sceneHeight};
}
void D3D9Backend::setPresentationUi(std::function<void(IDirect3DDevice9Ex*,IDirect3DSurface9*)> draw) {
    std::lock_guard lock(impl_->mutex);
    impl_->presentationUi=std::move(draw);
}
bool D3D9Backend::applyVideoSettings(const VideoSettings& requested, std::string& error) {
    std::lock_guard lock(impl_->mutex);
    error.clear();
    if(!validateVideoSettings(requested)) {error="Video settings contain an unsupported value.";return false;}
    if(!impl_->device || impl_->inFrame || GetCurrentThreadId()!=impl_->deviceThread) {
        error="Video settings must be applied at the renderer's presentation boundary.";return false;
    }
    D3DCAPS9 caps{};
    if(FAILED(impl_->device->GetDeviceCaps(&caps)) || requested.width>caps.MaxTextureWidth || requested.height>caps.MaxTextureHeight) {
        error="The GPU does not support this render resolution.";return false;
    }
    if(requested.anisotropy && (requested.anisotropy>caps.MaxAnisotropy || !(caps.TextureFilterCaps&D3DPTFILTERCAPS_MINFANISOTROPIC))) {
        error="The GPU does not support this anisotropic filtering level.";return false;
    }
    std::vector<MONITORINFOEXW> monitors;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL {
        MONITORINFOEXW info{};info.cbSize=sizeof(info);
        if(GetMonitorInfoW(monitor,&info)) reinterpret_cast<std::vector<MONITORINFOEXW>*>(data)->push_back(info);
        return TRUE;
    },reinterpret_cast<LPARAM>(&monitors));
    if(requested.monitor>=monitors.size()) {error="The selected display is no longer connected.";return false;}
    const auto previous=impl_->presentation;
    auto next=previous;next.BackBufferWidth=requested.width;next.BackBufferHeight=requested.height;next.BackBufferCount=1;
    DEVMODEW mode{};mode.dmSize=sizeof(mode);
    const bool displayKnown=EnumDisplaySettingsW(monitors[requested.monitor].szDevice,ENUM_CURRENT_SETTINGS,&mode)!=FALSE;
    next.PresentationInterval=requested.vsync==VSyncMode::Off ||
        (requested.vsync==VSyncMode::Automatic && displayKnown && mode.dmDisplayFrequency && mode.dmDisplayFrequency<60)?
        D3DPRESENT_INTERVAL_IMMEDIATE:D3DPRESENT_INTERVAL_ONE;
    const bool reset=next.BackBufferWidth!=previous.BackBufferWidth || next.BackBufferHeight!=previous.BackBufferHeight || next.PresentationInterval!=previous.PresentationInterval;
    const bool placeWindow=requested.windowMode!=impl_->video.windowMode || requested.width!=impl_->video.width ||
        requested.height!=impl_->video.height || requested.monitor!=impl_->video.monitor;
    LONG_PTR previousStyle=0;
    DWORD nextStyle=0;
    RECT previousWindow{}, nextWindow{};
    UINT previousLatency=3;
    bool latencyTouched=false, windowTouched=false, resetAttempted=false;
    const bool sceneResize=requested.width!=impl_->sceneWidth || requested.height!=impl_->sceneHeight;
    bool sceneCPUChanged=false, sceneBindingsChanged=false;
    std::map<uint32_t,Impl::Texture> sceneTextures;
    std::map<uint32_t,Impl::Surface> sceneSurfaces;
    std::array<ComPtr<IDirect3DBaseTexture9>,16> sampledTextures;
    bool implicitColor=false, implicitDepth=false, hadDepth=false;
    std::array<ComPtr<IDirect3DSurface9>,4> targets;
    const uint32_t targetCount=std::min<DWORD>(4,caps.NumSimultaneousRTs);
    ComPtr<IDirect3DSurface9> depth, replacementBackbuffer;
    D3DVIEWPORT9 viewport{};
    auto replacementSurface=[&](IDirect3DSurface9* original,bool recovery)->IDirect3DSurface9* {
        if (!recovery) for (const auto& [handle,surface] : sceneSurfaces)
            if (impl_->surfaces.at(handle).resource.Get()==original) return surface.resource.Get();
        return original;
    };
    auto restoreSamplers=[&](bool recovery) {
        for(uint32_t sampler=0;sampler<16;++sampler) {
            IDirect3DBaseTexture9* texture=sampledTextures[sampler].Get();
            if (!recovery) for(const auto& [handle,replacement] : sceneTextures)
                if(impl_->textures.at(handle).flat.Get()==texture) {texture=replacement.flat.Get();break;}
            check(impl_->device->SetTexture(sampler,texture),"Restore texture sampling after render resolution change");
        }
    };
    auto restoreTargets=[&](IDirect3DSurface9* backbuffer, bool recovery) {
        ComPtr<IDirect3DSurface9> automaticDepth;
        if(implicitDepth) {
            const HRESULT result=impl_->device->GetDepthStencilSurface(&automaticDepth);
            if(result!=D3DERR_NOTFOUND) check(result,"Retain automatic depth after video reset");
        }
        check(impl_->device->SetDepthStencilSurface(nullptr),"Detach depth after video reset");
        for(uint32_t index=1;index<targetCount;++index)
            check(impl_->device->SetRenderTarget(index,nullptr),"Detach MRT after video reset");
        check(impl_->device->SetRenderTarget(0,implicitColor?backbuffer:replacementSurface(targets[0].Get(),recovery)),"Restore original scene color after video reset");
        for(uint32_t index=1;index<targetCount;++index)
            check(impl_->device->SetRenderTarget(index,replacementSurface(targets[index].Get(),recovery)),"Restore original scene MRT after video reset");
        check(impl_->device->SetDepthStencilSurface(hadDepth?(implicitDepth?automaticDepth.Get():replacementSurface(depth.Get(),recovery)):nullptr),
            "Restore original scene depth after video reset");
        // The swap chain's new implicit target gets its new full-size viewport.
        // Authored external scene targets retain their exact original viewport.
        if(!implicitColor || recovery) {
            auto restored=viewport;
            if (!recovery && replacementSurface(targets[0].Get(),false)!=targets[0].Get()) {
                const auto scaled=[](DWORD value,uint32_t requestedDimension,uint32_t originalDimension) {
                    return DWORD(std::llround(double(value)*requestedDimension/originalDimension));
                };
                restored.X=scaled(viewport.X,requested.width,impl_->sceneWidth);
                restored.Y=scaled(viewport.Y,requested.height,impl_->sceneHeight);
                restored.Width=scaled(viewport.Width,requested.width,impl_->sceneWidth);
                restored.Height=scaled(viewport.Height,requested.height,impl_->sceneHeight);
                restored.X=std::min<DWORD>(restored.X,requested.width-1);
                restored.Y=std::min<DWORD>(restored.Y,requested.height-1);
                restored.Width=std::max<DWORD>(1,std::min<DWORD>(restored.Width,requested.width-restored.X));
                restored.Height=std::max<DWORD>(1,std::min<DWORD>(restored.Height,requested.height-restored.Y));
            }
            check(impl_->device->SetViewport(&restored),"Restore original scene viewport after video reset");
        }
    };
    try {
        SetLastError(ERROR_SUCCESS);
        previousStyle=GetWindowLongPtrW(impl_->window,GWL_STYLE);
        if(!previousStyle && GetLastError()!=ERROR_SUCCESS) throw std::runtime_error("Cannot retain native video window mode");
        nextStyle=(requested.windowMode==WindowMode::Borderless?WS_POPUP:WS_OVERLAPPEDWINDOW) |
            (DWORD(previousStyle)&(WS_VISIBLE|WS_DISABLED|WS_CLIPCHILDREN|WS_CLIPSIBLINGS));
        if(!GetWindowRect(impl_->window,&previousWindow)) throw std::runtime_error("Cannot retain native video window placement");
        check(impl_->device->GetMaximumFrameLatency(&previousLatency),"Retain native maximum frame latency");
        for(uint32_t index=0;index<targetCount;++index) {
            const HRESULT result=impl_->device->GetRenderTarget(index,&targets[index]);
            if(result!=D3DERR_NOTFOUND) check(result,"Retain video reset render target");
        }
        if(!targets[0]) throw std::runtime_error("Cannot retain native video color target");
        implicitColor=targets[0].Get()==impl_->backbuffer.Get();
        const HRESULT depthResult=impl_->device->GetDepthStencilSurface(&depth);
        if(depthResult!=D3DERR_NOTFOUND) check(depthResult,"Retain video reset depth target");
        hadDepth=depth.Get()!=nullptr;
        implicitDepth=hadDepth && implicitColor && !impl_->boundDepthState && !impl_->boundDepthTexture &&
            std::none_of(impl_->surfaces.begin(),impl_->surfaces.end(),[&](const auto& item) {return item.second.resource.Get()==depth.Get();});
        check(impl_->device->GetViewport(&viewport),"Retain video reset viewport");
        if(sceneResize && impl_->updateSceneCPU) {
            // Prepare every replacement before mutating either guest CPU mode
            // or live GPU bindings. Native handles and guest tokens stay stable.
            for(const uint32_t handle : impl_->sceneResources.resolveTextures) {
                const auto& original=impl_->textures.at(handle);
                auto replacement=original;
                replacement.flat.Reset();replacement.stagingFlat.Reset();
                replacement.width=requested.width;replacement.height=requested.height;
                replacement.uploaded.assign(1,false);
                check(impl_->device->CreateTexture(requested.width,requested.height,1,D3DUSAGE_RENDERTARGET,
                    original.format,D3DPOOL_DEFAULT,&replacement.flat,nullptr),"Create changed primary scene resolve texture");
                ComPtr<IDirect3DSurface9> oldLevel,newLevel;
                check(original.flat->GetSurfaceLevel(0,&oldLevel),"Retain primary resolve before render resolution change");
                check(replacement.flat->GetSurfaceLevel(0,&newLevel),"Read changed primary resolve texture");
                check(impl_->device->StretchRect(oldLevel.Get(),nullptr,newLevel.Get(),nullptr,D3DTEXF_LINEAR),
                    "Preserve last frame while changing render resolution");
                sceneTextures.emplace(handle,std::move(replacement));
            }
            for(const uint32_t handle : {impl_->sceneResources.colorSurface,impl_->sceneResources.depthSurface}) {
                const auto& original=impl_->surfaces.at(handle);
                D3DSURFACE_DESC description{};
                check(original.resource->GetDesc(&description),"Describe primary scene surface before render resolution change");
                auto replacement=original;
                replacement.resource.Reset();
                if(handle==impl_->sceneResources.depthSurface)
                    check(impl_->device->CreateDepthStencilSurface(requested.width,requested.height,description.Format,
                        description.MultiSampleType,description.MultiSampleQuality,TRUE,&replacement.resource,nullptr),
                        "Create changed primary scene depth surface");
                else {
                    check(impl_->device->CreateRenderTarget(requested.width,requested.height,description.Format,
                        description.MultiSampleType,description.MultiSampleQuality,FALSE,&replacement.resource,nullptr),
                        "Create changed primary scene color surface");
                    check(impl_->device->StretchRect(original.resource.Get(),nullptr,replacement.resource.Get(),nullptr,D3DTEXF_LINEAR),
                        "Preserve primary scene color while changing render resolution");
                }
                sceneSurfaces.emplace(handle,std::move(replacement));
            }
            // Original texture GetSurfaceLevel callers own separate handles.
            // Repoint those aliases to the new parent level without changing
            // the original title's ownership/release relationships.
            for(const auto& [handle,original] : impl_->surfaces) {
                const auto replacement=sceneTextures.find(original.parent);
                if(replacement==sceneTextures.end()) continue;
                if(original.level || original.face || original.depthState)
                    throw std::runtime_error("Unverified primary resolve texture view during render resolution change");
                auto surface=original;surface.resource.Reset();
                check(replacement->second.flat->GetSurfaceLevel(0,&surface.resource),"Recreate primary resolve level0 view");
                sceneSurfaces.emplace(handle,std::move(surface));
            }
            for(uint32_t sampler=0;sampler<16;++sampler)
                check(impl_->device->GetTexture(sampler,&sampledTextures[sampler]),"Retain actual sampled resources before render resolution change");
        }
        ComPtr<IDirect3DSurface9> source=targets[0];
        if(impl_->presentationTexture) {
            const auto& texture=impl_->textures.at(impl_->presentationTexture);
            source.Reset();
            check(texture.flat->GetSurfaceLevel(0,&source),"Retain video preview presentation source");
        }
        D3DSURFACE_DESC sourceDescription{};
        check(source->GetDesc(&sourceDescription),"Describe video preview source");
        impl_->videoPresenter.prepare(impl_->device.Get(),sceneResize?requested.width:sourceDescription.Width,
            sceneResize?requested.height:sourceDescription.Height,requested);
        source.Reset();
        if(placeWindow) {
            const auto& screen=monitors[requested.monitor];
            nextWindow=screen.rcMonitor;
            if(requested.windowMode==WindowMode::Windowed) {
                nextWindow={0,0,LONG(requested.width),LONG(requested.height)};
                if(!AdjustWindowRect(&nextWindow,nextStyle,FALSE)) throw std::runtime_error("Cannot size native video window");
                const LONG width=std::min(nextWindow.right-nextWindow.left,screen.rcWork.right-screen.rcWork.left);
                const LONG height=std::min(nextWindow.bottom-nextWindow.top,screen.rcWork.bottom-screen.rcWork.top);
                nextWindow={screen.rcWork.left+(screen.rcWork.right-screen.rcWork.left-width)/2,
                    screen.rcWork.top+(screen.rcWork.bottom-screen.rcWork.top-height)/2,0,0};
                nextWindow.right=nextWindow.left+width;nextWindow.bottom=nextWindow.top+height;
            }
        }
        latencyTouched=true;
        check(impl_->device->SetMaximumFrameLatency(requested.lowLatency?1:3),"Set native maximum frame latency");
        if(reset) {
            if(implicitColor) targets[0].Reset();
            if(implicitDepth) depth.Reset();
            resetAttempted=true;
            impl_->backbuffer.Reset();
            check(impl_->device->ResetEx(&next,nullptr),"Apply native video swap chain");
            check(impl_->device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&replacementBackbuffer),"Get changed native video backbuffer");
            restoreTargets(replacementBackbuffer.Get(),false);
        }
        if(!sceneTextures.empty()) {
            // ResetEx preserves resources but leaves textures bound to the old
            // COM allocations. Restore all actual sampler bindings by identity.
            sceneBindingsChanged=true;
            if(!reset) restoreTargets(impl_->backbuffer.Get(),false);
            restoreSamplers(false);
            sceneCPUChanged=true;
            impl_->updateSceneCPU(requested.width,requested.height);
        }
        if(placeWindow) {
            windowTouched=true;
            SetLastError(ERROR_SUCCESS);
            if(!SetWindowLongPtrW(impl_->window,GWL_STYLE,nextStyle) && GetLastError()!=ERROR_SUCCESS) throw std::runtime_error("Cannot apply native window mode");
            if(!SetWindowPos(impl_->window,nullptr,nextWindow.left,nextWindow.top,nextWindow.right-nextWindow.left,nextWindow.bottom-nextWindow.top,
                SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED)) throw std::runtime_error("Cannot apply native video window placement");
        }
        // Publish only after native scene resources, original CPU mode, swap
        // chain and HWND agree. All candidates remain alive for rollback.
        for(auto& [handle,replacement] : sceneTextures) impl_->textures.at(handle)=std::move(replacement);
        for(auto& [handle,replacement] : sceneSurfaces) impl_->surfaces.at(handle)=std::move(replacement);
        if(sceneResize) {
            impl_->sceneWidth=requested.width;impl_->sceneHeight=requested.height;
            for(auto& sampled : impl_->sampledTextures)
                if(sceneTextures.contains(sampled.handle)) sampled.original=impl_->textures.at(sampled.handle).flat;
            impl_->guiFrame={};impl_->presentedGuiFrame={};impl_->guiResolvedTextures.clear();impl_->guiPresented=false;
        }
        if(reset) {
            impl_->backbuffer=std::move(replacementBackbuffer);
            impl_->presentation=next;impl_->presentation.BackBufferWidth=requested.width;
            impl_->presentation.BackBufferHeight=requested.height;impl_->presentation.BackBufferCount=1;
            impl_->counters.width=requested.width;impl_->counters.height=requested.height;
            impl_->guiPresented=false;
        }
        impl_->restoreVideoSamplers=true;impl_->videoSamplerDirty=0xffff;
        impl_->video=requested;
        impl_->framePacer.reset();
        return true;
    }catch(const std::exception& problem) {
        error=problem.what();
        auto failedRecovery=[&](const char* step, HRESULT result) {
            if(FAILED(result)) {
                char detail[192]{};
                std::snprintf(detail,sizeof(detail)," Previous settings recovery: %s failed (HRESULT 0x%08lX).",step,
                    static_cast<unsigned long>(result));
                error+=detail;
            }
        };
        if(sceneCPUChanged) {
            try {impl_->updateSceneCPU(impl_->sceneWidth,impl_->sceneHeight);}
            catch(const std::exception& recoveryProblem) {error+=" Previous settings recovery: original CPU resolution ";error+=recoveryProblem.what();}
        }
        if(windowTouched) {
            SetLastError(ERROR_SUCCESS);
            if(!SetWindowLongPtrW(impl_->window,GWL_STYLE,previousStyle) && GetLastError()!=ERROR_SUCCESS)
                error+=" Previous settings recovery: native window style could not be restored.";
            if(!SetWindowPos(impl_->window,nullptr,previousWindow.left,previousWindow.top,
                previousWindow.right-previousWindow.left,previousWindow.bottom-previousWindow.top,
                SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED))
                error+=" Previous settings recovery: native window placement could not be restored.";
        }
        if(resetAttempted) {
            replacementBackbuffer.Reset();impl_->backbuffer.Reset();
            auto recovery=previous;
            const HRESULT result=impl_->device->ResetEx(&recovery,nullptr);
            failedRecovery("swap chain",result);
            // A failed ResetEx leaves the device lost: only another reset,
            // CheckDeviceState, or release is legal until recovery succeeds.
            if(FAILED(result)) return false;
            const HRESULT backbufferResult=impl_->device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&impl_->backbuffer);
            failedRecovery("backbuffer",backbufferResult);
            if(SUCCEEDED(backbufferResult)) {
                try {restoreTargets(impl_->backbuffer.Get(),true);}
                catch(const std::exception& recoveryProblem) {error+=" Previous settings recovery: ";error+=recoveryProblem.what();}
            }
        }
        if(sceneBindingsChanged) {
            try {
                if(!resetAttempted) restoreTargets(impl_->backbuffer.Get(),true);
                restoreSamplers(true);
            }catch(const std::exception& recoveryProblem) {error+=" Previous settings recovery: primary scene resources ";error+=recoveryProblem.what();}
        }
        if(latencyTouched) failedRecovery("maximum frame latency",impl_->device->SetMaximumFrameLatency(previousLatency));
        return false;
    }
}
VideoSettings D3D9Backend::appliedVideoSettings() const {std::lock_guard lock(impl_->mutex);return impl_->video;}
GuiPointerResult D3D9Backend::mapPresentationPointer(float x,float y) const {
    std::lock_guard lock(impl_->mutex);
    const auto r=impl_->presentationRect;
    if(!std::isfinite(x) || !std::isfinite(y) || !r.width || !r.height) return {GuiPointerMapping::Miss};
    x=(x*impl_->counters.width-r.x)/r.width;y=(y*impl_->counters.height-r.y)/r.height;
    if(x<0 || x>1 || y<0 || y>1) return {GuiPointerMapping::Miss};
    return {GuiPointerMapping::Hit,x,y};
}
void D3D9Backend::loadCaches(std::span<const uint8_t> windowsCache, std::span<const uint8_t> xenonCache, std::span<const uint8_t> nativeExtension, std::span<const uint8_t> xenonSourceExtension) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->shaders.empty()) throw std::runtime_error("Cannot replace native shader caches with live shader resources");
    auto windows = ShaderCache::parse(windowsCache, ShaderCachePlatform::Windows);
    if (!nativeExtension.empty()) windows = windows.withWindowsExtension(ShaderCache::parse(nativeExtension, ShaderCachePlatform::Windows));
    auto xenon = ShaderCache::parse(xenonCache, ShaderCachePlatform::Xenon);
    if (!xenonSourceExtension.empty()) xenon = xenon.withXenonSourceExtension(ShaderCache::parse(xenonSourceExtension,ShaderCachePlatform::Xenon));
    impl_->windowsCache = std::move(windows);
    impl_->xenonCache = std::move(xenon);
}
uint32_t D3D9Backend::createShader(const std::string& name, const ShaderKey& key) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto match = impl_->windowsCache.find(name, key);
    if (!match.variant) throw std::runtime_error("Original Windows shader variant is unavailable: " + featureDescription(name, key));
    // D3DDisassemble receives a bounded length and validates the original code
    // before the pointer-only CreateShader API consumes it.
    ComPtr<ID3DBlob> disassembly;
    check(D3DDisassemble(match.variant->bytecode.data(), match.variant->bytecode.size() * sizeof(uint32_t),
        0, nullptr, &disassembly), "Validate original Windows shader bytecode");
    Impl::Shader shader{match.shader->stage, match.variant};
    if (shader.stage == ShaderStage::Pixel) {
        const auto samplers = nativeShaderSamplers({reinterpret_cast<const uint8_t*>(match.variant->bytecode.data()),
            match.variant->bytecode.size() * sizeof(uint32_t)},shader.stage);
        shader.declaredSamplers = samplers.mask;
        shader.samplerTextureTypes = samplers.textureTypes;
    }
    static_assert(sizeof(DWORD) == sizeof(uint32_t));
    const auto* tokens = reinterpret_cast<const DWORD*>(match.variant->bytecode.data());
    if (shader.stage == ShaderStage::Vertex)
        check(impl_->device->CreateVertexShader(tokens, &shader.vertex), "CreateVertexShader");
    else check(impl_->device->CreatePixelShader(tokens, &shader.pixel), "CreatePixelShader");
    const uint32_t handle = impl_->handle();
    shader.name = match.shader->name;
    shader.handle = handle;
    impl_->shaders.emplace(handle, std::move(shader));
    return handle;
}
uint32_t D3D9Backend::createShaderForXenon(std::span<const uint8_t> originalBytecode, ShaderStage expectedStage,
    const ShaderSelection* selection) {
    std::lock_guard lock(impl_->mutex);
    const auto matches = impl_->xenonCache.matchCode(originalBytecode);
    if (selection) {
        ShaderMatch original{};
        try { original = impl_->xenonCache.requireCodeVariant(*selection, expectedStage, originalBytecode); }
        catch (const std::runtime_error& error) { throw std::runtime_error(std::string(error.what()) + shaderCandidates(matches)); }
        const auto native = impl_->windowsCache.find(selection->name, selection->key);
        if (!native.variant) throw std::runtime_error("Original Windows shader variant is unavailable: " +
            featureDescription(selection->name, selection->key));
        const uint32_t handle = createShader(native.shader->name, native.variant->key);
        impl_->shaders.at(handle).original = original.variant;
        impl_->shaders.at(handle).originalName = original.shader->name;
        if (matches.size() > 1 && impl_->shaderIdentityDiagnostics++ < 8) {
            std::fprintf(stderr, "[NATIVE SHADER IDENTITY] %s [%u,%u,%u] stage=%s exact_code_aliases=%zu\n",
                original.shader->name.c_str(), selection->key[0], selection->key[1], selection->key[2],
                expectedStage == ShaderStage::Vertex ? "vertex" : "pixel", matches.size());
            std::fflush(stderr);
        }
        return handle;
    }
    ShaderMatch selected{nullptr, nullptr};
    const ShaderVariant* original = nullptr;
    std::string originalName;
    for (const auto& match : matches) {
        if (match.shader->stage != expectedStage) throw std::runtime_error("Original Xenon shader does not match requested stage");
        const auto native = impl_->windowsCache.find(match.shader->name, match.variant->key);
        if (!native.variant) throw std::runtime_error("Original Windows shader variant is unavailable: " +
            featureDescription(match.shader->name, match.variant->key));
        if (selected.variant && (selected.variant->bytecode != native.variant->bytecode ||
            selected.variant->parameters != native.variant->parameters || original->parameters != match.variant->parameters))
            throw std::runtime_error("Xenon shader bytecode matches ambiguous native feature keys; title shader selection hook is required" + shaderCandidates(matches));
        selected = native;
        original = match.variant;
        originalName = match.shader->name;
    }
    if (!selected.variant) throw std::runtime_error("Unknown original Xenon shader bytecode; cache-backed native conversion unavailable");
    const uint32_t handle = createShader(selected.shader->name, selected.variant->key);
    impl_->shaders.at(handle).original = original;
    impl_->shaders.at(handle).originalName = std::move(originalName);
    return handle;
}
bool D3D9Backend::optionalAvatarShaderUnavailable(std::span<const uint8_t> code, ShaderStage stage,
    const ShaderSelection* selection) const {
    std::lock_guard lock(impl_->mutex);
    const auto matches = impl_->xenonCache.matchCode(code);
    if (selection) {
        ShaderMatch original{};
        try { original = impl_->xenonCache.requireCodeVariant(*selection, stage, code); }
        catch (const std::runtime_error& error) { throw std::runtime_error(std::string(error.what()) + shaderCandidates(matches)); }
        if (impl_->windowsCache.find(selection->name, selection->key).variant) return false;
        if (optionalAvatarFamily(original.shader->name)) return true;
        throw std::runtime_error("Required original Windows shader variant is unavailable: " + featureDescription(selection->name, selection->key));
    }
    if (matches.empty()) throw std::runtime_error("Unknown original Xenon shader bytecode; optional shader failure is not applicable");
    ShaderMatch selected{};
    bool missing = false;
    for (const auto& match : matches) {
        if (match.shader->stage != stage) throw std::runtime_error("Original Xenon shader does not match requested stage");
        const auto native = impl_->windowsCache.find(match.shader->name, match.variant->key);
        if (!native.variant) {
            if (!optionalAvatarFamily(match.shader->name))
                throw std::runtime_error("Required original Windows shader variant is unavailable: " + featureDescription(match.shader->name, match.variant->key));
            missing = true;
        } else {
            if (selected.variant && (selected.variant->bytecode != native.variant->bytecode || selected.variant->parameters != native.variant->parameters))
                throw std::runtime_error("Ambiguous original shader matches divergent native variants" + shaderCandidates(matches));
            selected = native;
        }
    }
    // A partly available bytecode match cannot prove which key was requested.
    if (missing && selected.variant) throw std::runtime_error("Ambiguous original shader matches both unavailable and native variants");
    return missing;
}
void D3D9Backend::bindShader(ShaderStage stage, uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto found = impl_->shaders.find(handle);
    if (handle && (found == impl_->shaders.end() || found->second.stage != stage)) throw std::runtime_error("Invalid native shader resource handle/stage");
    if (stage == ShaderStage::Vertex) {
        check(impl_->device->SetVertexShader(handle ? found->second.vertex.Get() : nullptr), "SetVertexShader");
        impl_->vertexShader = handle;
    } else {
        check(impl_->device->SetPixelShader(handle ? found->second.pixel.Get() : nullptr), "SetPixelShader");
        impl_->pixelShader = handle;
    }
    if (handle) impl_->applyGuestConstants(found->second, false);
}
void D3D9Backend::setFloatConstants(ShaderStage stage, uint32_t parameterId, std::span<const float> values) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto found = impl_->shaders.find(stage == ShaderStage::Vertex ? impl_->vertexShader : impl_->pixelShader);
    if (found == impl_->shaders.end()) throw std::runtime_error("No native shader is bound for constant upload");
    for (const auto& parameter : found->second.variant->parameters) {
        if (parameter.id != parameterId) continue;
        if (values.size() != size_t(parameter.registerCount) * 4)
            throw std::runtime_error("Native shader constant count does not match original Windows variant metadata");
        if (stage == ShaderStage::Vertex)
            check(impl_->device->SetVertexShaderConstantF(parameter.firstRegister, values.data(), parameter.registerCount), "SetVertexShaderConstantF");
        else check(impl_->device->SetPixelShaderConstantF(parameter.firstRegister, values.data(), parameter.registerCount), "SetPixelShaderConstantF");
        return;
    }
    throw std::runtime_error("Parameter is absent from original Windows shader variant metadata");
}
void D3D9Backend::setGuestFloatRegisters(ShaderStage stage, uint32_t first, std::span<const float> values) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (values.empty() || values.size() % 4 || first > 1024 || values.size() / 4 > 1024 - first)
        throw std::runtime_error("Original shader register upload exceeds native adapter bounds");
    auto& destination = stage == ShaderStage::Vertex ? impl_->vertexConstants : impl_->pixelConstants;
    auto& written = stage == ShaderStage::Vertex ? impl_->vertexConstantsWritten : impl_->pixelConstantsWritten;
    std::copy(values.begin(), values.end(), destination.begin() + first * 4);
    std::fill(written.begin() + first, written.begin() + first + values.size() / 4, true);
    const auto shader = impl_->shaders.find(stage == ShaderStage::Vertex ? impl_->vertexShader : impl_->pixelShader);
    if (shader != impl_->shaders.end()) impl_->applyGuestConstants(shader->second, false);
}
uint32_t D3D9Backend::createTexture(uint32_t width, uint32_t height, uint32_t levels, D3DFORMAT format, bool cube, bool renderTarget) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (!width || !height || width > 8192 || height > 8192 || (cube && width != height) ||
        levels > 14 || (format != D3DFMT_A8R8G8B8 && format != D3DFMT_X8R8G8B8 &&
        format != D3DFMT_DXT1 && format != D3DFMT_DXT5) ||
        (renderTarget && (format == D3DFMT_DXT1 || format == D3DFMT_DXT5)))
        throw std::runtime_error("Unsupported native title texture creation contract");
    Impl::Texture texture{};
    texture.width = width; texture.height = height; texture.levels = levels;
    texture.format = format; texture.renderTarget = renderTarget;
    const DWORD usage = renderTarget ? D3DUSAGE_RENDERTARGET : 0;
    if (cube) {
        check(impl_->device->CreateCubeTexture(width, levels, usage, format, D3DPOOL_DEFAULT, &texture.cube, nullptr), "Create original title cube texture");
        texture.levels = texture.cube->GetLevelCount();
    } else {
        check(impl_->device->CreateTexture(width, height, levels, usage, format, D3DPOOL_DEFAULT, &texture.flat, nullptr), "Create original title texture");
        texture.levels = texture.flat->GetLevelCount();
    }
    const uint32_t handle = impl_->handle();
    texture.uploaded.assign(texture.levels * (cube ? 6 : 1), false);
    impl_->textures.emplace(handle, std::move(texture));
    return handle;
}
uint32_t D3D9Backend::createDepthTexture(uint32_t width, uint32_t height) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (!width || !height || width > 8192 || height > 8192)
        throw std::runtime_error("Native D24S8 depth texture dimensions exceed the verified contract");
    D3DDEVICE_CREATION_PARAMETERS creation{};
    D3DDISPLAYMODE display{};
    check(impl_->device->GetCreationParameters(&creation), "Get actual depth texture device adapter");
    check(impl_->d3d->GetAdapterDisplayMode(creation.AdapterOrdinal,&display), "Get actual depth texture adapter format");
    check(impl_->d3d->CheckDeviceFormat(creation.AdapterOrdinal,creation.DeviceType,display.Format,
        D3DUSAGE_DEPTHSTENCIL,D3DRTYPE_TEXTURE,D3DFMT_D24S8), "Check actual D24S8 depth texture support");
    check(impl_->d3d->CheckDeviceFormat(creation.AdapterOrdinal,creation.DeviceType,display.Format,
        D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_QUERY_FILTER,D3DRTYPE_TEXTURE,D3DFMT_D24S8), "Check actual D24S8 depth texture filter support");
    check(impl_->d3d->CheckDepthStencilMatch(creation.AdapterOrdinal,creation.DeviceType,display.Format,
        D3DFMT_X8R8G8B8,D3DFMT_D24S8), "Check actual D24S8/X8R8G8B8 depth match");
    Impl::Texture texture{};
    texture.width = width; texture.height = height; texture.levels = 1;
    texture.format = D3DFMT_D24S8; texture.depthStencil = true;
    const HRESULT result = impl_->device->CreateTexture(width,height,1,D3DUSAGE_DEPTHSTENCIL,
        D3DFMT_D24S8,D3DPOOL_DEFAULT,&texture.flat,nullptr);
    if (nullableDepthAllocation(result)) return 0;
    check(result, "Create actual one-level D24S8 depth texture");
    texture.depthState = std::make_shared<Impl::DepthTargetState>();
    texture.depthState->width = width; texture.depthState->height = height;
    check(texture.flat.As(&texture.depthState->identity), "Retain actual depth target completion identity");
    const uint32_t handle = impl_->handle();
    impl_->textures.emplace(handle,std::move(texture));
    return handle;
}
void D3D9Backend::verifyDepthTextureSurfaceAlias(uint32_t handle, uint32_t view) const {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto texture = impl_->textures.find(handle);
    const auto surface = impl_->surfaces.find(view);
    if (texture == impl_->textures.end() || !texture->second.depthStencil || texture->second.cube ||
        texture->second.levels != 1 || surface == impl_->surfaces.end() || surface->second.parent != handle ||
        surface->second.face || surface->second.level || !surface->second.depthTextureIdentity ||
        !texture->second.depthState || surface->second.depthState != texture->second.depthState)
        throw std::runtime_error("Native depth texture/view metadata does not describe an owned level0 alias");
    ComPtr<IDirect3DTexture9> container;
    ComPtr<IDirect3DSurface9> level;
    ComPtr<IUnknown> textureIdentity, containerIdentity, levelIdentity, viewIdentity;
    check(surface->second.resource->GetContainer(__uuidof(IDirect3DTexture9),
        reinterpret_cast<void**>(container.GetAddressOf())), "Read actual depth view container");
    check(texture->second.flat->GetSurfaceLevel(0,&level), "Read actual depth texture level0");
    check(texture->second.flat.As(&textureIdentity), "Identify actual depth texture");
    check(container.As(&containerIdentity), "Identify actual depth view container");
    check(level.As(&levelIdentity), "Identify actual depth texture level0");
    check(surface->second.resource.As(&viewIdentity), "Identify actual owned depth view");
    const auto description = surfaceDescription(view);
    if (textureIdentity.Get() != containerIdentity.Get() || levelIdentity.Get() != viewIdentity.Get() ||
        textureIdentity.Get() != surface->second.depthTextureIdentity.Get() ||
        description.Width != texture->second.width || description.Height != texture->second.height ||
        description.Format != D3DFMT_D24S8 || description.Usage != D3DUSAGE_DEPTHSTENCIL ||
        description.Pool != D3DPOOL_DEFAULT || description.MultiSampleType != D3DMULTISAMPLE_NONE)
        throw std::runtime_error("Native depth texture/view COM identities or actual descriptions disagree");
}
void D3D9Backend::finishDepthTargetAlias(uint32_t texture, uint32_t depth, uint32_t color) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    verifyDepthTextureSurfaceAlias(texture,depth);
    const auto target = impl_->textures.find(texture);
    const auto depthView = impl_->surfaces.find(depth);
    const auto colorView = impl_->surfaces.find(color);
    if (colorView == impl_->surfaces.end() || impl_->inFrame || !impl_->framePrepared ||
        impl_->boundDepthState != target->second.depthState || !target->second.depthState->originalClear)
        throw std::runtime_error("Native shadow End lacks a prepared, fully cleared actual depth alias outside a scene");
    const auto colorDescription = surfaceDescription(color);
    if (colorDescription.Width != target->second.width || colorDescription.Height != target->second.height ||
        colorDescription.Format != D3DFMT_X8R8G8B8 || colorDescription.Usage != D3DUSAGE_RENDERTARGET ||
        colorDescription.Pool != D3DPOOL_DEFAULT || colorDescription.MultiSampleType != D3DMULTISAMPLE_NONE)
        throw std::runtime_error("Native shadow End color companion dimensions, samples or format disagree");
    ComPtr<IDirect3DSurface9> actualColor, actualDepth;
    ComPtr<IUnknown> colorIdentity, expectedColorIdentity, depthIdentity, expectedDepthIdentity;
    check(impl_->device->GetRenderTarget(0,&actualColor), "Read actual color companion at shadow End");
    check(impl_->device->GetDepthStencilSurface(&actualDepth), "Read actual depth target at shadow End");
    check(actualColor.As(&colorIdentity), "Identify actual shadow color target");
    check(colorView->second.resource.As(&expectedColorIdentity), "Identify owned shadow color companion");
    check(actualDepth.As(&depthIdentity), "Identify actual shadow depth target");
    check(depthView->second.resource.As(&expectedDepthIdentity), "Identify owned shadow depth alias");
    if (colorIdentity.Get() != expectedColorIdentity.Get() || depthIdentity.Get() != expectedDepthIdentity.Get())
        throw std::runtime_error("Native shadow End is not completing the actually bound owned color/depth targets");
    // The original CPU End leaves this exact level0 DS attached. The image is
    // already present in the texture; no illegal mip-surface StretchRect,
    // manufactured clear/detach, resolve serial or game frame is produced.
    target->second.depthState->completed = true;
}
uint32_t D3D9Backend::textureSurface(uint32_t handle, uint32_t face, uint32_t level) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto texture = impl_->textures.find(handle);
    if (texture == impl_->textures.end() || level >= texture->second.levels ||
        face >= (texture->second.cube ? 6u : 1u)) throw std::runtime_error("Invalid native texture face/mip level");
    Impl::Surface surface{{}, handle, face, level};
    if (texture->second.cube)
        check(texture->second.cube->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face), level, &surface.resource), "Get original title cube surface");
    else {
        const HRESULT result = texture->second.flat->GetSurfaceLevel(level,&surface.resource);
        if (texture->second.depthStencil && nullableDepthAllocation(result)) return 0;
        check(result, "Get original title texture surface");
    }
    if (texture->second.depthStencil)
        check(texture->second.flat.As(&surface.depthTextureIdentity), "Retain actual depth view parent identity");
    surface.depthState = texture->second.depthState;
    const uint32_t result = impl_->handle();
    impl_->surfaces.emplace(result, std::move(surface));
    return result;
}
uint32_t D3D9Backend::createSurface(uint32_t width, uint32_t height, D3DFORMAT format, bool depth, D3DMULTISAMPLE_TYPE samples) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (!width || !height || width > 8192 || height > 8192 ||
        (depth ? format != D3DFMT_D24S8 : (format != D3DFMT_A8R8G8B8 && format != D3DFMT_X8R8G8B8)) ||
        (samples != D3DMULTISAMPLE_NONE && samples != D3DMULTISAMPLE_2_SAMPLES && samples != D3DMULTISAMPLE_4_SAMPLES))
        throw std::runtime_error("Unsupported native title render/depth surface contract");
    if (samples != D3DMULTISAMPLE_NONE) {
        DWORD qualityLevels = 0;
        check(impl_->d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            format, TRUE, samples, &qualityLevels), "Check actual title surface multisample support");
        if (!qualityLevels) throw std::runtime_error("Actual native title surface multisample quality is unavailable");
    }
    Impl::Surface surface;
    if (depth) check(impl_->device->CreateDepthStencilSurface(width, height, format, samples, 0, TRUE, &surface.resource, nullptr), "Create original title depth surface");
    else check(impl_->device->CreateRenderTarget(width, height, format, samples, 0, FALSE, &surface.resource, nullptr), "Create original title render surface");
    const uint32_t result = impl_->handle();
    impl_->surfaces.emplace(result, std::move(surface));
    return result;
}
D3DSURFACE_DESC D3D9Backend::surfaceDescription(uint32_t handle) const {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto surface = impl_->surfaces.find(handle);
    if (surface == impl_->surfaces.end()) throw std::runtime_error("Invalid native title surface description handle");
    D3DSURFACE_DESC description{};
    check(surface->second.resource->GetDesc(&description), "Read original title native surface description");
    return description;
}
D3DSURFACE_DESC D3D9Backend::textureDescription(uint32_t handle, uint32_t level) const {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto texture = impl_->textures.find(handle);
    if (texture == impl_->textures.end() || level >= texture->second.levels)
        throw std::runtime_error("Invalid native title texture description level");
    D3DSURFACE_DESC description{};
    if (texture->second.cube) check(texture->second.cube->GetLevelDesc(level, &description), "Get original title native cube description");
    else check(texture->second.flat->GetLevelDesc(level, &description), "Get original title native texture description");
    return description;
}
uint32_t D3D9Backend::textureLevelCount(uint32_t handle) const {
    std::lock_guard lock(impl_->mutex);
    const auto texture = impl_->textures.find(handle);
    if (texture == impl_->textures.end()) throw std::runtime_error("Invalid native title texture level query");
    return texture->second.levels;
}
void D3D9Backend::uploadSurface(uint32_t handle, D3DFORMAT format, uint32_t pitch, std::span<const uint8_t> bytes) {
    std::lock_guard lock(impl_->mutex);
    const auto surface = impl_->surfaces.find(handle);
    const auto description = surfaceDescription(handle);
    const bool compressed = format == D3DFMT_DXT1 || format == D3DFMT_DXT5;
    const uint32_t rows = compressed ? (description.Height + 3) / 4 : description.Height;
    const uint32_t expectedPitch = compressed ? ((description.Width + 3) / 4) * (format == D3DFMT_DXT1 ? 8 : 16) : description.Width * 4;
    if (format != description.Format || pitch != expectedPitch || bytes.size() != size_t(rows) * pitch ||
        description.MultiSampleType != D3DMULTISAMPLE_NONE || (description.Usage & (D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_RENDERTARGET)))
        throw std::runtime_error("Original/native texture upload dimensions, format or usage do not match");
    const auto parent = impl_->textures.find(surface->second.parent);
    if (parent == impl_->textures.end() || parent->second.renderTarget)
        throw std::runtime_error("Native upload requires an ordinary texture surface parent");
    auto& texture = parent->second;
    if (!texture.stagingFlat && !texture.stagingCube) {
        if (texture.cube) check(impl_->device->CreateCubeTexture(texture.width, texture.levels, 0, format,
            D3DPOOL_SYSTEMMEM, &texture.stagingCube, nullptr), "Create actual cube upload and mip staging");
        else check(impl_->device->CreateTexture(texture.width, texture.height, texture.levels, 0, format,
            D3DPOOL_SYSTEMMEM, &texture.stagingFlat, nullptr), "Create actual texture upload and mip staging");
    }
    ComPtr<IDirect3DSurface9> source;
    if (texture.cube) check(texture.stagingCube->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(surface->second.face),
        surface->second.level, &source), "Get actual cube upload staging face");
    else check(texture.stagingFlat->GetSurfaceLevel(surface->second.level, &source), "Get actual texture upload staging level");
    D3DLOCKED_RECT destination{};
    check(source->LockRect(&destination, nullptr, 0), "Lock native texture upload staging");
    if (destination.Pitch < 0 || static_cast<uint32_t>(destination.Pitch) < pitch) {
        source->UnlockRect();
        throw std::runtime_error("Native texture staging pitch is shorter than original rows");
    }
    for (uint32_t row = 0; row < rows; ++row)
        std::memcpy(static_cast<uint8_t*>(destination.pBits) + size_t(row) * destination.Pitch, bytes.data() + size_t(row) * pitch, pitch);
    check(source->UnlockRect(), "Unlock native texture upload staging");
    check(impl_->device->UpdateSurface(source.Get(), nullptr, surface->second.resource.Get(), nullptr), "Upload original texture pixels to native resource");
    texture.uploaded[surface->second.face * texture.levels + surface->second.level] = true;
}
void D3D9Backend::generateMipmaps(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto found = impl_->textures.find(handle);
    if (found == impl_->textures.end() || found->second.renderTarget || found->second.depthStencil)
        throw std::runtime_error("Invalid native title mip generation texture");
    auto& texture = found->second;
    const uint32_t faces = texture.cube ? 6 : 1;
    for (uint32_t face = 0; face < faces; ++face)
        if (!texture.uploaded[face * texture.levels]) throw std::runtime_error("Original title mip generation is missing a real source face");
    if (texture.levels == 1) return; // The actual resource has no lower level to generate.
    if (!impl_->d3dx) impl_->d3dx = LoadLibraryExW(L"d3dx9_43.dll", nullptr,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!impl_->d3dx) throw std::runtime_error("Native D3DX BOX mip generation requires packaged or installed d3dx9_43.dll");
    using FilterTexture = HRESULT(WINAPI*)(IDirect3DBaseTexture9*, const PALETTEENTRY*, UINT, DWORD);
    const auto filter = reinterpret_cast<FilterTexture>(GetProcAddress(impl_->d3dx, "D3DXFilterTexture"));
    if (!filter) throw std::runtime_error("Native D3DXFilterTexture entry is unavailable");
    IDirect3DBaseTexture9* source = texture.cube ? static_cast<IDirect3DBaseTexture9*>(texture.stagingCube.Get()) : texture.stagingFlat.Get();
    IDirect3DBaseTexture9* destination = texture.cube ? static_cast<IDirect3DBaseTexture9*>(texture.cube.Get()) : texture.flat.Get();
    check(filter(source, nullptr, 0, 5), "Generate actual original BOX texture mip levels");
    if (texture.cube) for (uint32_t face = 0; face < 6; ++face)
        check(texture.stagingCube->AddDirtyRect(static_cast<D3DCUBEMAP_FACES>(face), nullptr), "Mark actual generated cube face dirty");
    else check(texture.stagingFlat->AddDirtyRect(nullptr), "Mark actual generated texture mip chain dirty");
    check(impl_->device->UpdateTexture(source, destination), "Upload actual generated mip chain to native texture");
    std::fill(texture.uploaded.begin(), texture.uploaded.end(), true);
}
void D3D9Backend::bindTexture(uint32_t sampler, uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (sampler >= 16) throw std::runtime_error("Unverified native title texture sampler slot");
    const auto found = impl_->textures.find(handle);
    if (handle && found == impl_->textures.end()) throw std::runtime_error("Invalid native title texture binding handle");
    IDirect3DBaseTexture9* originalTexture = handle ? (found->second.cube ?
        static_cast<IDirect3DBaseTexture9*>(found->second.cube.Get()) : found->second.flat.Get()) : nullptr;
    IDirect3DBaseTexture9* texture = handle && impl_->keyboardMousePrompts && found->second.inputPrompt ?
        found->second.inputPrompt.Get() : originalTexture;
    ComPtr<IUnknown> depthIdentity;
    std::shared_ptr<Impl::DepthTargetState> depthState;
    if (handle && found->second.depthStencil) {
        depthState = found->second.depthState;
        check(found->second.flat.As(&depthIdentity), "Identify depth texture before actual sampling bind");
        if (depthIdentity.Get() == impl_->boundDepthTexture.Get())
            throw std::runtime_error("Native depth texture is still attached as the depth target at sampling bind");
    }
    check(impl_->device->SetTexture(sampler, texture), "Bind original title texture");
    if (impl_->sampledDepthTextures[sampler]) --impl_->sampledDepthTextureCount;
    impl_->sampledDepthTextures[sampler] = std::move(depthIdentity);
    impl_->sampledDepthStates[sampler] = std::move(depthState);
    if (impl_->sampledDepthTextures[sampler]) ++impl_->sampledDepthTextureCount;
    impl_->sampledTextures[sampler].handle = handle;
    impl_->sampledTextures[sampler].original = originalTexture;
    impl_->sampledTextures[sampler].inputPrompt = handle ? found->second.inputPrompt : nullptr;
    impl_->videoSamplerDirty|=1u<<sampler;
}
void D3D9Backend::setInputPrompt(uint32_t handle, InputPromptKind kind, bool alphaMask) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto found = impl_->textures.find(handle);
    if (!handle || found == impl_->textures.end())
        throw std::runtime_error("Invalid native input prompt texture handle");
    auto& texture = found->second;
    if (texture.inputPromptKind == kind && texture.inputPromptAlphaMask == alphaMask) return;
    ComPtr<IDirect3DTexture9> replacement;
    if (kind != InputPromptKind::None) replacement = impl_->createInputPrompt(texture,kind,alphaMask);
    // The original CPU bind cache may already hold this same texture. Replace
    // current native sampler pointers immediately without changing its handle,
    // mip/source metadata, shadow identities, draw or presentation counters.
    for (uint32_t sampler = 0; sampler < impl_->sampledTextures.size(); ++sampler) {
        auto& sampled = impl_->sampledTextures[sampler];
        if (sampled.handle != handle) continue;
        IDirect3DBaseTexture9* image = impl_->keyboardMousePrompts && replacement ?
            replacement.Get() : sampled.original.Get();
        check(impl_->device->SetTexture(sampler,image), "Apply exact original input prompt substitution");
        sampled.inputPrompt = replacement;
    }
    texture.inputPrompt = std::move(replacement);
    texture.inputPromptKind = kind;
    texture.inputPromptAlphaMask = alphaMask;
}
void D3D9Backend::setKeyboardMousePrompts(bool enabled) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->keyboardMousePrompts == enabled) return;
    if (impl_->device)
        for (uint32_t sampler = 0; sampler < impl_->sampledTextures.size(); ++sampler)
            impl_->refreshPromptSampler(sampler,enabled);
    impl_->keyboardMousePrompts = enabled;
}
void D3D9Backend::setGuiScreenTexture(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->textures.find(handle);
    if (found == impl_->textures.end() || !found->second.flat || !found->second.renderTarget || found->second.depthStencil)
        throw std::runtime_error("GUI monitor requires its actual color render texture");
    found->second.guiScreen = true;
}
void D3D9Backend::setGuiPointerContext(const GuiPointerContext& context, bool monitorScreen) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->guiContext == context && impl_->guiMonitorScreen == monitorScreen) return;
    impl_->guiContext = context;
    impl_->guiMonitorScreen = monitorScreen;
    impl_->guiFrame = {};
    impl_->guiPresented = false;
}
GuiPointerResult D3D9Backend::mapGuiPointer(const GuiPointerContext& context, float x, float y) const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->guiMonitorScreen) return {GuiPointerMapping::Flat};
    const auto& frame = impl_->presentedGuiFrame;
    if (context != impl_->guiContext || !impl_->guiPresented || frame.context != context)
        return {GuiPointerMapping::Pending};
    // guiScreens is a monitor-backed input screen. A missing or unsupported
    // primary draw must never silently restore the misaligned flat mapping.
    if (!frame.seen || frame.invalid || frame.triangles.empty()) return {GuiPointerMapping::Pending};
    std::optional<GuiProjectionHit> closest;
    for (const auto& triangle : frame.triangles) {
        const auto& bounds = triangle.viewport;
        if (x < bounds[0] || x > bounds[2] || y < bounds[1] || y > bounds[3]) continue;
        updateClosestProjectedGuiHit(triangle.vertices,{x,y},closest);
    }
    if (!closest || closest->uv[0] < 0 || closest->uv[0] > 1 || closest->uv[1] < 0 || closest->uv[1] > 1)
        return {GuiPointerMapping::Miss};
    return {GuiPointerMapping::Hit,closest->uv[0],closest->uv[1]};
}
void D3D9Backend::getSampledTexture(uint32_t sampler, IDirect3DBaseTexture9** texture) const {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (sampler >= 16 || !texture) throw std::runtime_error("Invalid native sampler diagnostic query");
    check(impl_->device->GetTexture(sampler,texture), "Read actual native sampled texture");
}
void D3D9Backend::bindRenderTarget(uint32_t index, uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto found = impl_->surfaces.find(handle);
    if (index >= 4 || (handle && (found == impl_->surfaces.end() || !(surfaceDescription(handle).Usage & D3DUSAGE_RENDERTARGET))))
        throw std::runtime_error("Invalid native title render target binding");
    check(impl_->device->SetRenderTarget(index, handle ? found->second.resource.Get() : (index ? nullptr : impl_->backbuffer.Get())), "Bind original title render target");
}
void D3D9Backend::bindDepthSurface(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto found = impl_->surfaces.find(handle);
    if (handle && (found == impl_->surfaces.end() || !(surfaceDescription(handle).Usage & D3DUSAGE_DEPTHSTENCIL)))
        throw std::runtime_error("Invalid native title depth surface binding");
    ComPtr<IUnknown> depthIdentity;
    if (handle) depthIdentity = found->second.depthTextureIdentity;
    if (depthIdentity && impl_->sampledDepthTextureCount)
        for (const auto& sampled : impl_->sampledDepthTextures)
            if (sampled.Get() == depthIdentity.Get())
                throw std::runtime_error("Native depth target is still bound for sampling at depth rebind");
    check(impl_->device->SetDepthStencilSurface(handle ? found->second.resource.Get() : nullptr), "Bind original title depth surface");
    impl_->boundDepthTexture = std::move(depthIdentity);
    impl_->boundDepthState = handle ? found->second.depthState : nullptr;
    if (impl_->boundDepthState) {
        // A successful genuine DS bind starts a new pass; an old completed
        // image cannot authorize sampling after a later partial/new write.
        impl_->boundDepthState->originalClear = false;
        impl_->boundDepthState->completed = false;
    }
}
void D3D9Backend::setViewport(const D3DVIEWPORT9& viewport) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (!viewport.Width || !viewport.Height || viewport.Width > 8192 || viewport.Height > 8192 ||
        !(viewport.MinZ >= 0 && viewport.MinZ <= 1 && viewport.MaxZ >= 0 && viewport.MaxZ <= 1))
        throw std::runtime_error("Invalid original title native viewport");
    check(impl_->device->SetViewport(&viewport), "Set original title viewport");
}
void D3D9Backend::setRenderState(D3DRENDERSTATETYPE state, uint32_t value) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    check(impl_->device->SetRenderState(state, value), "Set original title native render state");
}
uint32_t D3D9Backend::renderState(D3DRENDERSTATETYPE state) const {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    DWORD value = 0;
    check(impl_->device->GetRenderState(state, &value), "Get original title native render state");
    return value;
}
D3DCAPS9 D3D9Backend::capabilities() const {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    D3DCAPS9 caps{};
    check(impl_->device->GetDeviceCaps(&caps), "Query actual native device capabilities");
    return caps;
}
void D3D9Backend::setClipPlane(uint32_t index, const std::array<float, 4>& plane) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (index >= 6) throw std::runtime_error("Unverified original title native clip plane index");
    check(impl_->device->SetClipPlane(index, plane.data()), "Set original title native clip plane");
}
void D3D9Backend::setSamplerState(uint32_t sampler, D3DSAMPLERSTATETYPE state, uint32_t value) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (sampler >= 16) throw std::runtime_error("Unverified native sampler state slot");
    check(impl_->device->SetSamplerState(sampler, state, value), "Set original title native sampler state");
    impl_->originalSamplerStates[sampler][state]=value;
    impl_->videoSamplerDirty|=1u<<sampler;
}
void D3D9Backend::getGammaRamp(D3DGAMMARAMP& ramp) const {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    impl_->device->GetGammaRamp(0, &ramp);
}
void D3D9Backend::setGammaRamp(const D3DGAMMARAMP& ramp) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    impl_->device->SetGammaRamp(0, D3DSGR_NO_CALIBRATION, &ramp);
}
void D3D9Backend::resolveRenderTarget(uint32_t handle, uint32_t expectedColorSurface, D3DMULTISAMPLE_TYPE expectedSamples) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto texture = impl_->textures.find(handle);
    if (texture == impl_->textures.end() || texture->second.cube || !texture->second.renderTarget)
        throw std::runtime_error("Unverified native title render resolve destination");
    if (impl_->inFrame) throw std::runtime_error("Original render resolve occurred inside a native BeginScene");
    ComPtr<IDirect3DSurface9> source, destination;
    check(impl_->device->GetRenderTarget(0, &source), "Get original title resolve source");
    check(texture->second.flat->GetSurfaceLevel(0, &destination), "Get original title resolve destination");
    if (expectedColorSurface) {
        const auto expected = impl_->surfaces.find(expectedColorSurface);
        if (expected == impl_->surfaces.end()) throw std::runtime_error("Invalid original title expected resolve surface");
        ComPtr<IUnknown> actualIdentity, expectedIdentity;
        check(source.As(&actualIdentity), "Read actual resolve surface identity");
        check(expected->second.resource.As(&expectedIdentity), "Read expected title resolve surface identity");
        D3DSURFACE_DESC sourceDescription{}, destinationDescription{};
        check(source->GetDesc(&sourceDescription), "Describe actual title resolve source");
        check(destination->GetDesc(&destinationDescription), "Describe actual title resolve destination");
        if (actualIdentity.Get() != expectedIdentity.Get() || sourceDescription.MultiSampleType != expectedSamples ||
            !(sourceDescription.Usage & D3DUSAGE_RENDERTARGET) || sourceDescription.Width != destinationDescription.Width ||
            sourceDescription.Height != destinationDescription.Height || sourceDescription.Format != destinationDescription.Format)
            throw std::runtime_error("Actual title resolve source binding, samples or destination dimensions disagree");
    }
    {
        PhaseScope phase(impl_->phaseFrame,RenderPhase::Resolve,impl_->reportPhases);
        check(impl_->device->StretchRect(source.Get(), nullptr, destination.Get(), nullptr, D3DTEXF_NONE), "Resolve original title render target");
    }
    impl_->copyGuiResolve(handle,source.Get(),destination.Get());
    texture->second.lastResolveSerial = ++impl_->resolveSerial;
    texture->second.resolvedDraws = impl_->frameDraws;
    impl_->lastResolveTexture = handle;
}
void D3D9Backend::selectPresentationTexture(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto texture = impl_->textures.find(handle);
    if (handle && (texture == impl_->textures.end() || texture->second.cube || !texture->second.renderTarget))
        throw std::runtime_error("Unverified original title presentation texture");
    impl_->presentationTexture = handle;
}
uint32_t D3D9Backend::createVertexBuffer(std::span<const uint8_t> bytes, uint32_t stride) {
    std::lock_guard lock(impl_->mutex);
    PhaseScope phase(impl_->phaseFrame,RenderPhase::BufferUpload,impl_->reportPhases);
    impl_->requireDevice();
    if (!stride || stride > 2048 || bytes.empty() || bytes.size() > 128 * 1024 * 1024 || bytes.size() % stride)
        throw std::runtime_error("Invalid native vertex buffer length/stride");
    Impl::VertexBuffer buffer{{}, stride, static_cast<uint32_t>(bytes.size() / stride)};
    // Keep the same bounded bytes sent to the GPU for monitor projection as
    // well as the existing Cal3D/RVMBatch geometry ownership checks.
    buffer.bytes.assign(bytes.begin(),bytes.end());
    check(impl_->device->CreateVertexBuffer(static_cast<UINT>(bytes.size()), D3DUSAGE_WRITEONLY, 0,
        D3DPOOL_DEFAULT, &buffer.resource, nullptr), "CreateVertexBuffer");
    void* destination = nullptr;
    check(buffer.resource->Lock(0, static_cast<UINT>(bytes.size()), &destination, 0), "Lock native vertex buffer");
    std::memcpy(destination, bytes.data(), bytes.size());
    check(buffer.resource->Unlock(), "Unlock native vertex buffer");
    const uint32_t handle = impl_->handle();
    impl_->vertexBuffers.emplace(handle, std::move(buffer));
    return handle;
}
uint32_t D3D9Backend::createIndexBuffer(std::span<const uint16_t> indices) {
    std::lock_guard lock(impl_->mutex);
    PhaseScope phase(impl_->phaseFrame,RenderPhase::BufferUpload,impl_->reportPhases);
    impl_->requireDevice();
    if (indices.empty() || indices.size() > 64 * 1024 * 1024) throw std::runtime_error("Invalid native index buffer count");
    Impl::IndexBuffer buffer{{}, static_cast<uint32_t>(indices.size())};
    const UINT bytes = static_cast<UINT>(indices.size_bytes());
    check(impl_->device->CreateIndexBuffer(bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
        D3DPOOL_DEFAULT, &buffer.resource, nullptr), "CreateIndexBuffer");
    void* destination = nullptr;
    check(buffer.resource->Lock(0, bytes, &destination, 0), "Lock native index buffer");
    std::memcpy(destination, indices.data(), bytes);
    check(buffer.resource->Unlock(), "Unlock native index buffer");
    buffer.indices.assign(indices.begin(), indices.end());
    const uint32_t handle = impl_->handle();
    impl_->indexBuffers.emplace(handle, std::move(buffer));
    return handle;
}
uint32_t D3D9Backend::createDeclaration(std::span<const D3DVERTEXELEMENT9> elements) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (elements.empty() || elements.size() > MAXD3DDECLLENGTH + 1 || elements.back().Stream != 255 ||
        elements.back().Type != D3DDECLTYPE_UNUSED) throw std::runtime_error("Native vertex declaration has no terminal element");
    for (size_t i = 0; i + 1 < elements.size(); ++i)
        if (elements[i].Stream >= 16 || elements[i].Type >= D3DDECLTYPE_UNUSED ||
            elements[i].Method != D3DDECLMETHOD_DEFAULT || elements[i].Usage > D3DDECLUSAGE_SAMPLE)
            throw std::runtime_error("Unsupported native vertex declaration element");
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    check(impl_->device->CreateVertexDeclaration(elements.data(), &declaration), "CreateVertexDeclaration");
    const uint32_t handle = impl_->handle();
    impl_->declarations.emplace(handle, Impl::Declaration{std::move(declaration),{elements.begin(),elements.end()}});
    return handle;
}
void D3D9Backend::bindVertexBuffer(uint32_t handle, uint32_t stream) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto found = impl_->vertexBuffers.find(handle);
    if (stream >= 16 || found == impl_->vertexBuffers.end()) throw std::runtime_error("Invalid native vertex stream binding");
    check(impl_->device->SetStreamSource(stream, found->second.resource.Get(), 0, found->second.stride), "SetStreamSource");
    if (stream == 0) impl_->vertexBuffer = handle;
}
void D3D9Backend::bindIndexBuffer(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto found = impl_->indexBuffers.find(handle);
    if (found == impl_->indexBuffers.end()) throw std::runtime_error("Invalid native index buffer binding");
    check(impl_->device->SetIndices(found->second.resource.Get()), "SetIndices");
    impl_->indexBuffer = handle;
}
void D3D9Backend::unbindVertexBuffer(uint32_t stream) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (stream >= 16) throw std::runtime_error("Invalid native vertex stream unbinding");
    check(impl_->device->SetStreamSource(stream, nullptr, 0, 0), "Unbind native vertex stream");
    if (stream == 0) impl_->vertexBuffer = 0;
}
void D3D9Backend::unbindIndexBuffer() {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    check(impl_->device->SetIndices(nullptr), "Unbind native index buffer");
    impl_->indexBuffer = 0;
}
void D3D9Backend::updateVertexBuffer(uint32_t handle, std::span<const uint8_t> bytes) {
    std::lock_guard lock(impl_->mutex);
    PhaseScope phase(impl_->phaseFrame,RenderPhase::BufferUpload,impl_->reportPhases);
    impl_->requireDevice();
    const auto found = impl_->vertexBuffers.find(handle);
    if (found == impl_->vertexBuffers.end() || bytes.size() != size_t(found->second.stride) * found->second.count)
        throw std::runtime_error("Original/native vertex reupload length mismatch");
    auto& buffer = found->second;
    if (std::equal(bytes.begin(), bytes.end(), buffer.bytes.begin())) return;
    if (!buffer.dynamic) {
        ComPtr<IDirect3DVertexBuffer9> replacement;
        check(impl_->device->CreateVertexBuffer(static_cast<UINT>(bytes.size()), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
            0, D3DPOOL_DEFAULT, &replacement, nullptr), "Create streaming vertex buffer");
        void* destination = nullptr;
        check(replacement->Lock(0, static_cast<UINT>(bytes.size()), &destination, D3DLOCK_DISCARD), "Lock first streaming vertex upload");
        std::memcpy(destination, bytes.data(), bytes.size());
        check(replacement->Unlock(), "Unlock first streaming vertex upload");
        // Promotion must also work for callers updating an already bound
        // resource, including aliases on additional streams. Queued draws keep
        // the old allocation; subsequent draws see the replacement.
        for (UINT stream = 0; stream < impl_->videoCaps.MaxStreams; ++stream) {
            ComPtr<IDirect3DVertexBuffer9> bound;
            UINT offset = 0, stride = 0;
            check(impl_->device->GetStreamSource(stream, &bound, &offset, &stride), "Retain streaming vertex binding");
            if (bound.Get() == buffer.resource.Get())
                check(impl_->device->SetStreamSource(stream, replacement.Get(), offset, stride), "Promote bound streaming vertex buffer");
        }
        buffer.resource = std::move(replacement);
        buffer.dynamic = true;
        buffer.bytes.assign(bytes.begin(), bytes.end());
        ++buffer.revision;
        return;
    }
    void* destination = nullptr;
    check(found->second.resource->Lock(0, static_cast<UINT>(bytes.size()), &destination, D3DLOCK_DISCARD), "Lock native vertex reupload");
    std::memcpy(destination, bytes.data(), bytes.size());
    check(found->second.resource->Unlock(), "Unlock native vertex reupload");
    found->second.bytes.assign(bytes.begin(),bytes.end());
    ++buffer.revision;
}
void D3D9Backend::updateIndexBuffer(uint32_t handle, std::span<const uint16_t> indices) {
    std::lock_guard lock(impl_->mutex);
    PhaseScope phase(impl_->phaseFrame,RenderPhase::BufferUpload,impl_->reportPhases);
    impl_->requireDevice();
    const auto found = impl_->indexBuffers.find(handle);
    if (found == impl_->indexBuffers.end() || indices.size() != found->second.count)
        throw std::runtime_error("Original/native index reupload length mismatch");
    auto& buffer = found->second;
    if (std::equal(indices.begin(), indices.end(), buffer.indices.begin())) return;
    if (!buffer.dynamic) {
        ComPtr<IDirect3DIndexBuffer9> replacement;
        check(impl_->device->CreateIndexBuffer(static_cast<UINT>(indices.size_bytes()), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
            D3DFMT_INDEX16, D3DPOOL_DEFAULT, &replacement, nullptr), "Create streaming index buffer");
        void* destination = nullptr;
        check(replacement->Lock(0, static_cast<UINT>(indices.size_bytes()), &destination, D3DLOCK_DISCARD), "Lock first streaming index upload");
        std::memcpy(destination, indices.data(), indices.size_bytes());
        check(replacement->Unlock(), "Unlock first streaming index upload");
        ComPtr<IDirect3DIndexBuffer9> bound;
        check(impl_->device->GetIndices(&bound), "Retain streaming index binding");
        if (bound.Get() == buffer.resource.Get())
            check(impl_->device->SetIndices(replacement.Get()), "Promote bound streaming index buffer");
        buffer.resource = std::move(replacement);
        buffer.dynamic = true;
        buffer.indices.assign(indices.begin(), indices.end());
        ++buffer.revision;
        return;
    }
    void* destination = nullptr;
    check(found->second.resource->Lock(0, static_cast<UINT>(indices.size_bytes()), &destination, D3DLOCK_DISCARD), "Lock native index reupload");
    std::memcpy(destination, indices.data(), indices.size_bytes());
    check(found->second.resource->Unlock(), "Unlock native index reupload");
    found->second.indices.assign(indices.begin(), indices.end());
    ++buffer.revision;
}
void D3D9Backend::bindDeclaration(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    const auto found = impl_->declarations.find(handle);
    if (found == impl_->declarations.end()) throw std::runtime_error("Invalid native vertex declaration binding");
    check(impl_->device->SetVertexDeclaration(found->second.resource.Get()), "SetVertexDeclaration");
    impl_->declaration = handle;
}
void D3D9Backend::release(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    impl_->guiResolvedTextures.erase(handle);
    if (handle == impl_->vertexShader || handle == impl_->pixelShader || handle == impl_->vertexBuffer ||
        handle == impl_->indexBuffer || handle == impl_->declaration)
        throw std::runtime_error("Unbind native graphics resource before releasing it");
    if (!impl_->shaders.erase(handle) && !impl_->vertexBuffers.erase(handle) &&
        !impl_->indexBuffers.erase(handle) && !impl_->declarations.erase(handle) &&
        !impl_->textures.erase(handle) && !impl_->surfaces.erase(handle))
        throw std::runtime_error("Invalid native graphics resource release");
}
void D3D9Backend::beginFrame() {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (impl_->inFrame) throw std::runtime_error("Native renderer frame is already active");
    {
        PhaseScope phase(impl_->phaseFrame,RenderPhase::Scene,impl_->reportPhases);
        check(impl_->device->BeginScene(), "BeginScene");
    }
    impl_->inFrame = true;
    impl_->frameEnded = false;
}
void D3D9Backend::prepareFrame() {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    impl_->framePrepared = true;
}
void D3D9Backend::endFrame() {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (!impl_->inFrame) throw std::runtime_error("Native renderer has no active frame to end");
    {
        PhaseScope phase(impl_->phaseFrame,RenderPhase::Scene,impl_->reportPhases);
        check(impl_->device->EndScene(), "EndScene");
    }
    impl_->inFrame = false;
    impl_->frameEnded = true;
}
void D3D9Backend::clear(uint32_t flags, uint32_t color, float depth, uint32_t stencil) {
    clearRectangles({}, flags, color, depth, stencil);
}
void D3D9Backend::clearRectangles(std::span<const D3DRECT> rectangles, uint32_t flags, uint32_t color, float depth, uint32_t stencil,
    uint32_t originalCaller) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (!flags || (flags & ~(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) ||
        !(depth >= 0 && depth <= 1) || stencil > 255 || rectangles.size() > 4096)
        throw std::runtime_error("Invalid native title clear contract");
    for (const auto& rectangle : rectangles)
        if (rectangle.x1 < 0 || rectangle.y1 < 0 || rectangle.x2 <= rectangle.x1 || rectangle.y2 <= rectangle.y1 ||
            rectangle.x2 > 8192 || rectangle.y2 > 8192) throw std::runtime_error("Invalid original title native clear rectangle");
    check(impl_->device->Clear(static_cast<DWORD>(rectangles.size()), rectangles.empty() ? nullptr : rectangles.data(), flags, color, depth, stencil), "Clear title render target");
    if (flags & D3DCLEAR_TARGET) impl_->discardGuiTarget();
    if (impl_->boundDepthState && (flags & D3DCLEAR_ZBUFFER)) {
        D3DVIEWPORT9 viewport{};
        check(impl_->device->GetViewport(&viewport), "Read actual full shadow clear viewport");
        impl_->boundDepthState->originalClear = isOriginalShadowClear(originalCaller,rectangles,flags,color,depth,stencil,
            viewport,impl_->boundDepthState->width,impl_->boundDepthState->height);
        impl_->boundDepthState->completed = false;
    }
    if (flags & D3DCLEAR_TARGET) impl_->frameCleared = true;
    if (originalCaller && (flags & D3DCLEAR_TARGET) && !impl_->framePrepared && !impl_->inFrame &&
        impl_->unpreparedClearDiagnostics++ < 32) {
        ComPtr<IDirect3DSurface9> actual;
        check(impl_->device->GetRenderTarget(0,&actual), "Query actual unprepared color clear target");
        D3DSURFACE_DESC description{};
        check(actual->GetDesc(&description), "Describe actual unprepared color clear target");
        ComPtr<IUnknown> actualIdentity;
        check(actual.As(&actualIdentity), "Identify actual unprepared color clear target");
        uint32_t nativeTarget = 0;
        for (const auto& [handle,surface] : impl_->surfaces) {
            ComPtr<IUnknown> identity;
            check(surface.resource.As(&identity), "Identify registered native color surface");
            if (identity.Get() == actualIdentity.Get()) { nativeTarget = handle; break; }
        }
        std::fprintf(stderr,
            "[NATIVE UNPREPARED CLEAR] lr=%08X native_target=%u identity=%p size=%ux%u format=%u samples=%u flags=%u ended=%u frame_draws=%llu presents=%llu\n",
            originalCaller,nativeTarget,static_cast<void*>(actualIdentity.Get()),description.Width,description.Height,
            static_cast<uint32_t>(description.Format),static_cast<uint32_t>(description.MultiSampleType),flags,
            static_cast<uint32_t>(impl_->frameEnded),static_cast<unsigned long long>(impl_->frameDraws),
            static_cast<unsigned long long>(impl_->counters.presents));
        std::fflush(stderr);
    }
}
bool D3D9Backend::usesTitleBonePalette() const {
    std::lock_guard lock(impl_->mutex);
    const auto shader = impl_->shaders.find(impl_->vertexShader);
    if (shader == impl_->shaders.end() || !shader->second.original) return false;
    for (const auto& original : shader->second.original->parameters)
        for (const auto& native : shader->second.variant->parameters)
            if (isTitleBonePaletteBinding(shader->second.stage,shader->second.name,shader->second.variant->key,original,native)) return true;
    return false;
}
void D3D9Backend::reportTitlePaletteState() const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->shaders.find(impl_->vertexShader);
    if (found == impl_->shaders.end()) {
        std::fprintf(stderr,"[NATIVE PALETTE SHADER] vertex_handle=%u valid=0\n",impl_->vertexShader);
        std::fflush(stderr);
        return;
    }
    const auto& shader = found->second;
    std::fprintf(stderr,"[NATIVE PALETTE SHADER] vertex_handle=%u stage=%s name=%s original_name=%s key=[%u,%u,%u] original_key=[%u,%u,%u]\n",
        impl_->vertexShader,shader.stage == ShaderStage::Vertex ? "vertex" : "pixel",shader.name.c_str(),shader.originalName.c_str(),
        shader.variant->key[0],shader.variant->key[1],shader.variant->key[2],
        shader.original ? shader.original->key[0] : UINT32_MAX,shader.original ? shader.original->key[1] : UINT32_MAX,
        shader.original ? shader.original->key[2] : UINT32_MAX);
    const auto pixel = impl_->shaders.find(impl_->pixelShader);
    if (pixel == impl_->shaders.end()) std::fprintf(stderr,"[NATIVE PALETTE PIXEL] native_handle=%u valid=0\n",impl_->pixelShader);
    else {
        const auto& value = pixel->second;
        std::fprintf(stderr,"[NATIVE PALETTE PIXEL] native_handle=%u stage=%s name=%s original_name=%s key=[%u,%u,%u] original_key=[%u,%u,%u]\n",
            impl_->pixelShader,value.stage == ShaderStage::Pixel ? "pixel" : "vertex",value.name.c_str(),value.originalName.c_str(),
            value.variant->key[0],value.variant->key[1],value.variant->key[2],
            value.original ? value.original->key[0] : UINT32_MAX,value.original ? value.original->key[1] : UINT32_MAX,
            value.original ? value.original->key[2] : UINT32_MAX);
    }
    if (shader.original) for (const auto& parameter : shader.original->parameters) if (parameter.id == 81) {
        const ShaderParameter* native = nullptr;
        for (const auto& candidate : shader.variant->parameters) if (candidate.id == 81) native = &candidate;
        uint32_t writtenCount = 0, missingFirst = UINT32_MAX;
        std::string writtenRanges;
        const uint32_t end = parameter.firstRegister + parameter.registerCount;
        if (end > impl_->vertexConstantsWritten.size()) throw std::runtime_error("Palette diagnostic reflection exceeds guest registers");
        for (uint32_t index = parameter.firstRegister; index < end;) {
            if (!impl_->vertexConstantsWritten[index]) { missingFirst = std::min(missingFirst,index++); continue; }
            const uint32_t first = index;
            do { ++writtenCount; ++index; } while (index < end && impl_->vertexConstantsWritten[index]);
            writtenRanges += (writtenRanges.empty() ? "" : ";") + std::to_string(first) + ":" + std::to_string(index);
        }
        std::fprintf(stderr,"[NATIVE PALETTE REGISTERS] id=81 original_first=%u original_count=%u native_first=%u native_count=%u written_count=%u missing_first=%u written_ranges=%s\n",
            parameter.firstRegister,parameter.registerCount,native ? native->firstRegister : UINT32_MAX,
            native ? native->registerCount : UINT32_MAX,writtenCount,missingFirst,writtenRanges.empty() ? "none" : writtenRanges.c_str());
    }
    std::fflush(stderr);
}
void D3D9Backend::drawIndexedTriangles(int32_t baseVertex, uint32_t vertexCount, uint32_t startIndex,
    uint32_t triangleCount, const TitleBoneDraw* boneDraw, const TitleBatchDraw* batchDraw, uint32_t cjDiagnosticSerial) {
    drawIndexed(D3DPT_TRIANGLELIST, baseVertex, vertexCount, startIndex, triangleCount,boneDraw,batchDraw,cjDiagnosticSerial);
}
void D3D9Backend::drawIndexedTriangleStrip(int32_t baseVertex, uint32_t vertexCount, uint32_t startIndex, uint32_t triangleCount) {
    drawIndexed(D3DPT_TRIANGLESTRIP, baseVertex, vertexCount, startIndex, triangleCount);
}
void D3D9Backend::drawIndexedImmediate(D3DPRIMITIVETYPE topology, std::span<const uint8_t> vertices,
    uint32_t stride, std::span<const uint16_t> indices, uint32_t triangleCount) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    PhaseScope validation(impl_->phaseFrame,RenderPhase::Validation,impl_->reportPhases);
    const auto vertexShader = impl_->shaders.find(impl_->vertexShader), pixelShader = impl_->shaders.find(impl_->pixelShader);
    const bool validVertexShader = vertexShader != impl_->shaders.end() && vertexShader->second.stage == ShaderStage::Vertex;
    const bool validPixelShader = pixelShader != impl_->shaders.end() && pixelShader->second.stage == ShaderStage::Pixel;
    const bool validDeclaration = impl_->declarations.contains(impl_->declaration);
    const bool missing = (!impl_->inFrame && !impl_->framePrepared) || !validVertexShader || !validPixelShader || !validDeclaration;
    char bindings[320];
    std::snprintf(bindings,sizeof(bindings),
        "prepared=%u scene=%u ended=%u cleared=%u vs=%u/valid%u ps=%u/valid%u declaration=%u/valid%u frame_draws=%llu presents=%llu",
        static_cast<uint32_t>(impl_->framePrepared),static_cast<uint32_t>(impl_->inFrame),
        static_cast<uint32_t>(impl_->frameEnded),static_cast<uint32_t>(impl_->frameCleared),
        impl_->vertexShader,static_cast<uint32_t>(validVertexShader),impl_->pixelShader,static_cast<uint32_t>(validPixelShader),
        impl_->declaration,static_cast<uint32_t>(validDeclaration),static_cast<unsigned long long>(impl_->frameDraws),
        static_cast<unsigned long long>(impl_->counters.presents));
    if (impl_->immediateDrawDiagnostics++ < 8 || missing) {
        std::fprintf(stderr,"[NATIVE IMMEDIATE STATE] topology=%u bytes=%zu stride=%u indices=%zu triangles=%u %s\n",
            static_cast<uint32_t>(topology),vertices.size(),stride,indices.size(),triangleCount,bindings);
        std::fflush(stderr);
    }
    if (missing) throw std::runtime_error(std::string("Native immediate title draw is missing verified state: ") + bindings);
    impl_->requireIntegratedDepthSampling();
    if (!stride || stride > 2048 || vertices.empty() || vertices.size() > 128 * 1024 * 1024 || vertices.size() % stride)
        throw std::runtime_error("Native immediate title vertices exceed stride/byte bounds");
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size() / stride);
    if (validateIndexedTriangleRange(topology,0,vertexCount,vertexCount,indices,0,triangleCount) != indices.size())
        throw std::runtime_error("Native immediate title indices do not match their primitive extent");
    validation.stop();
    impl_->applyGuestConstants(impl_->shaders.at(impl_->vertexShader), true);
    impl_->applyGuestConstants(impl_->shaders.at(impl_->pixelShader), true);
    impl_->applyVideoSamplers();
    // Xbox needs no BeginScene API around this original draw. On native D3D9,
    // the proven engine frame boundary permits a real bracket for this batch;
    // end it before the original frame's subsequent StretchRect resolve.
    const bool ownScene = !impl_->inFrame;
    if (ownScene) beginFrame();
    {
        PhaseScope phase(impl_->phaseFrame,RenderPhase::Draw,impl_->reportPhases);
        check(impl_->device->DrawIndexedPrimitiveUP(topology,0,vertexCount,triangleCount,
            indices.data(),D3DFMT_INDEX16,vertices.data(),stride), "DrawIndexedPrimitiveUP original title batch");
    }
    impl_->captureGuiDraw(topology,vertices,stride,indices,0,0,triangleCount);
    // Native DrawIndexedPrimitiveUP resets stream0 and the index binding.
    impl_->vertexBuffer = impl_->indexBuffer = 0;
    ++impl_->counters.draws;
    if (impl_->boundDepthState) impl_->boundDepthState->completed = false;
    ++impl_->frameDraws;
    impl_->counters.triangles += triangleCount;
    if (ownScene) endFrame();
}
void D3D9Backend::drawIndexed(D3DPRIMITIVETYPE topology, int32_t baseVertex, uint32_t vertexCount,
    uint32_t startIndex, uint32_t triangleCount, const TitleBoneDraw* boneDraw, const TitleBatchDraw* batchDraw, uint32_t cjDiagnosticSerial) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    PhaseScope validation(impl_->phaseFrame,RenderPhase::Validation,impl_->reportPhases);
    const auto vertex = impl_->vertexBuffers.find(impl_->vertexBuffer);
    const auto index = impl_->indexBuffers.find(impl_->indexBuffer);
    const auto vertexShader = impl_->shaders.find(impl_->vertexShader);
    const auto pixelShader = impl_->shaders.find(impl_->pixelShader);
    const bool validVertexShader = vertexShader != impl_->shaders.end() && vertexShader->second.stage == ShaderStage::Vertex;
    const bool validPixelShader = pixelShader != impl_->shaders.end() && pixelShader->second.stage == ShaderStage::Pixel;
    const bool validDeclaration = impl_->declarations.contains(impl_->declaration);
    const bool validVertex = vertex != impl_->vertexBuffers.end(), validIndex = index != impl_->indexBuffers.end();
    const bool missing = (!impl_->inFrame && !impl_->framePrepared) || !validVertexShader || !validPixelShader ||
        !validDeclaration || !validVertex || !validIndex;
    char bindings[320];
    std::snprintf(bindings,sizeof(bindings),
        "prepared=%u scene=%u vs=%u/valid%u ps=%u/valid%u declaration=%u/valid%u vb=%u/valid%u ib=%u/valid%u",
        static_cast<uint32_t>(impl_->framePrepared),static_cast<uint32_t>(impl_->inFrame),
        impl_->vertexShader,static_cast<uint32_t>(validVertexShader),impl_->pixelShader,static_cast<uint32_t>(validPixelShader),
        impl_->declaration,static_cast<uint32_t>(validDeclaration),impl_->vertexBuffer,static_cast<uint32_t>(validVertex),
        impl_->indexBuffer,static_cast<uint32_t>(validIndex));
    if (impl_->indexedDrawDiagnostics++ < 8 || missing) {
        std::fprintf(stderr,"[NATIVE INDEXED DRAW] topology=%u base=%d vertices=%u start=%u triangles=%u %s\n",
            static_cast<uint32_t>(topology),baseVertex,vertexCount,startIndex,triangleCount,bindings);
        std::fflush(stderr);
    }
    if (missing) throw std::runtime_error(std::string("Native title indexed draw is missing verified state: ") + bindings);
    impl_->requireIntegratedDepthSampling();
    if (boneDraw && batchDraw) throw std::runtime_error("Conflicting original Cal3D and RVMBatch ownership proofs");
    if (batchDraw) {
        const auto& shader = vertexShader->second;
        const auto& geometry = batchDraw->geometry;
        if (!shader.original || !isTitleBonePaletteShader(shader.stage,shader.name,shader.variant->key) || shader.originalName != shader.name ||
            shader.original->key != shader.variant->key || topology != D3DPT_TRIANGLELIST ||
            vertex->second.stride != 84 || !isTitleBatchDeclaration(impl_->declarations.at(impl_->declaration).elements) ||
            baseVertex || startIndex || vertexCount != titleBatchVertexCount(geometry) ||
            uint64_t(triangleCount) * 3 != titleBatchIndexCount(geometry)) {
            std::fprintf(stderr,"[NATIVE BATCH SHADER STOP] name=%s original_name=%s native_key=[%u,%u,%u] original_key=[%u,%u,%u] original=%u topology=%u stride=%u declaration=%u base=%d start=%u vertices=%u expected_vertices=%u triangles=%u expected_indices=%u\n",
                shader.name.c_str(),shader.originalName.c_str(),shader.variant->key[0],shader.variant->key[1],shader.variant->key[2],
                shader.original ? shader.original->key[0] : 0,shader.original ? shader.original->key[1] : 0,shader.original ? shader.original->key[2] : 0,
                shader.original ? 1u : 0u,static_cast<unsigned>(topology),vertex->second.stride,
                isTitleBatchDeclaration(impl_->declarations.at(impl_->declaration).elements) ? 1u : 0u,
                baseVertex,startIndex,vertexCount,titleBatchVertexCount(geometry),triangleCount,titleBatchIndexCount(geometry));
            std::fflush(stderr);
            throw std::runtime_error("Original RVMBatch proof does not match its immutable shader and actual native draw");
        }
        const ShaderParameter* original = nullptr;
        const ShaderParameter* native = nullptr;
        for (const auto& parameter : shader.original->parameters) if (parameter.id == 81) original = &parameter;
        for (const auto& parameter : shader.variant->parameters) if (parameter.id == 81) native = &parameter;
        if (!original || !native) throw std::runtime_error("Original RVMBatch has no matching reflected matrix palette");
        titleBatchCurrentPalette(shader.stage,shader.name,shader.variant->key,*original,*native,
            impl_->vertexConstants,impl_->vertexConstantsWritten,*batchDraw);
    }
    if (boneDraw) {
        const auto& geometry = boneDraw->geometry;
        if (!usesTitleBonePalette() || topology != D3DPT_TRIANGLELIST || vertex->second.stride != 80 ||
            baseVertex < 0 || geometry.baseVertex != static_cast<uint32_t>(baseVertex) || geometry.vertexCount != vertexCount ||
            geometry.startIndex != startIndex || uint64_t(triangleCount) * 3 != geometry.indexCount ||
            boneDraw->matrices.size() != size_t(geometry.boneCount) * 16)
            throw std::runtime_error("Original Cal3D palette proof does not match its actual native draw bindings");
    }
    GeometryValidationKey geometryKey{impl_->vertexBuffer,impl_->indexBuffer,vertex->second.revision,index->second.revision,
        static_cast<uint32_t>(topology),static_cast<uint32_t>(baseVertex),vertexCount,startIndex,triangleCount};
    if (boneDraw) {
        const auto& geometry = boneDraw->geometry;
        geometryKey.producer = 1;
        geometryKey.geometry = {geometry.totalVertices,geometry.totalTriangles,geometry.baseVertex,geometry.vertexCount,
            geometry.startIndex,geometry.indexCount,geometry.boneCount};
    } else if (batchDraw) {
        const auto& geometry = batchDraw->geometry;
        geometryKey.producer = 2;
        geometryKey.geometry = {geometry.verticesPerInstance,geometry.trianglesPerInstance,geometry.capacity,geometry.activeInstances};
    }
    // Only immutable byte/range proofs survive a draw. Native bindings, guest
    // producer ownership and current matrix palettes are still checked above.
    impl_->geometryValidation.validate(geometryKey,[&] {
        validateIndexedTriangleRange(topology,baseVertex,vertexCount,vertex->second.count,index->second.indices,startIndex,triangleCount);
        if (batchDraw) validateTitleBatchGeometry(batchDraw->geometry,vertex->second.bytes,index->second.indices);
        if (boneDraw) validateTitleBoneGeometry(boneDraw->geometry,vertex->second.bytes,index->second.indices);
    });
    validation.stop();
    impl_->applyGuestConstants(impl_->shaders.at(impl_->vertexShader), true,boneDraw,batchDraw);
    impl_->applyGuestConstants(impl_->shaders.at(impl_->pixelShader), true);
    if (boneDraw && cjDiagnosticSerial) impl_->reportCJDrawState(cjDiagnosticSerial);
    impl_->applyVideoSamplers();
    // Native TRIANGLESTRIP retains the authored order and alternates winding
    // exactly as Xbox type6. Reordering triangles would change culling.
    // Original world/model submission does not require Xbox SDK Begin/End.
    // As for the observed GUI batch, a prepared engine frame permits a genuine
    // native scene around this draw. Buffered bindings survive that bracket;
    // an existing explicit native scene keeps its original lifetime.
    const bool ownScene = !impl_->inFrame;
    if (ownScene) beginFrame();
    {
        PhaseScope phase(impl_->phaseFrame,RenderPhase::Draw,impl_->reportPhases);
        check(impl_->device->DrawIndexedPrimitive(topology, baseVertex, 0, vertexCount,
            startIndex, triangleCount), "DrawIndexedPrimitive title triangles");
    }
    impl_->captureGuiDraw(topology,vertex->second.bytes,vertex->second.stride,index->second.indices,
        baseVertex,startIndex,triangleCount);
    ++impl_->counters.draws;
    if (impl_->boundDepthState) impl_->boundDepthState->completed = false;
    ++impl_->frameDraws;
    impl_->counters.triangles += triangleCount;
    if (ownScene) endFrame();
}
void D3D9Backend::present() {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if ((!impl_->inFrame && !impl_->frameEnded && !impl_->framePrepared) || (!impl_->frameDraws && !impl_->frameCleared))
        throw std::runtime_error("Refusing to present without an actual frame boundary and GPU draw or color clear");
    const bool gameFrame = impl_->frameDraws != 0;
    if (impl_->inFrame) endFrame();
    tickVideoSettings();
    if(const auto request=takeVideoSettingsRequest()) {
        std::string error;
        const bool applied=applyVideoSettings(request->settings,error);
        completeVideoSettingsRequest(request->serial,applied,std::move(error));
    }
    ComPtr<IDirect3DSurface9> source;
    if (impl_->presentationTexture) {
        const auto& texture = impl_->textures.at(impl_->presentationTexture);
        check(texture.flat->GetSurfaceLevel(0, &source), "Get original title frame presentation surface");
    } else check(impl_->device->GetRenderTarget(0, &source), "Get original title frame render surface");
    bool requestedCapture = false;
    if (gameFrame && impl_->captureRequestEvent) {
        const DWORD requested = WaitForSingleObject(impl_->captureRequestEvent, 0);
        if (requested == WAIT_FAILED) throw std::runtime_error("Cannot poll actual native capture request event");
        requestedCapture = requested == WAIT_OBJECT_0;
    }
    const uint64_t capturedFrameNumber = impl_->counters.presents + 1;
    const bool automaticCapture = gameFrame && !impl_->captureDirectory.empty() && !impl_->captured &&
        capturedFrameNumber == impl_->captureFrame;
    const bool captureFrame = automaticCapture || requestedCapture;
    if (captureFrame) {
        D3DSURFACE_DESC description{};
        check(source->GetDesc(&description), "Describe actual captured presentation source");
        const auto selected = impl_->textures.find(impl_->presentationTexture);
        std::fprintf(stderr,
            "[NATIVE CAPTURE PATH] drawn_present=%llu frame_draws=%llu automatic=%u requested=%u selected_texture=%u source=%ux%u format=%u selected_resolve_serial=%llu selected_resolve_draws=%llu latest_resolve_texture=%u latest_resolve_serial=%llu\n",
            static_cast<unsigned long long>(capturedFrameNumber),static_cast<unsigned long long>(impl_->frameDraws),
            static_cast<uint32_t>(automaticCapture),static_cast<uint32_t>(requestedCapture),
            impl_->presentationTexture,description.Width,description.Height,static_cast<uint32_t>(description.Format),
            static_cast<unsigned long long>(selected == impl_->textures.end() ? 0 : selected->second.lastResolveSerial),
            static_cast<unsigned long long>(selected == impl_->textures.end() ? 0 : selected->second.resolvedDraws),
            impl_->lastResolveTexture,static_cast<unsigned long long>(impl_->resolveSerial));
        std::fflush(stderr);
    }
    {
        PhaseScope phase(impl_->phaseFrame,RenderPhase::PresentationCopy,impl_->reportPhases);
        impl_->presentationRect=impl_->videoPresenter.draw(impl_->device.Get(),source.Get(),impl_->backbuffer.Get(),impl_->video,
            impl_->overlayMilliseconds>0?1000/impl_->overlayMilliseconds:0,impl_->overlayMilliseconds,impl_->window);
        if(impl_->presentationUi) impl_->presentationUi(impl_->device.Get(),impl_->backbuffer.Get());
    }
    std::vector<uint8_t> capturedPixels;
    if (captureFrame) {
        PhaseScope phase(impl_->phaseFrame,RenderPhase::CaptureRead,impl_->reportPhases);
        validateCaptureDirectory(impl_->captureDirectory);
        ComPtr<IDirect3DSurface9> readback;
        check(impl_->device->CreateOffscreenPlainSurface(impl_->counters.width, impl_->counters.height,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr), "Create actual frame readback surface");
        check(impl_->device->GetRenderTargetData(impl_->backbuffer.Get(), readback.Get()), "Read actual title frame pixels");
        D3DLOCKED_RECT locked{};
        check(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "Lock actual frame readback pixels");
        const uint32_t pitch = impl_->counters.width * 4;
        if (!locked.pBits || locked.Pitch < static_cast<LONG>(pitch)) {
            readback->UnlockRect();
            throw std::runtime_error("Invalid native frame readback row pitch");
        }
        capturedPixels.resize(static_cast<size_t>(pitch) * impl_->counters.height);
        for (uint32_t row = 0; row < impl_->counters.height; ++row)
            std::memcpy(capturedPixels.data() + static_cast<size_t>(row) * pitch,
                static_cast<const uint8_t*>(locked.pBits) + static_cast<size_t>(row) * locked.Pitch, pitch);
        check(readback->UnlockRect(), "Unlock actual frame readback pixels");
    }
    const HWND foreground=GetForegroundWindow();
    const bool active=foreground==impl_->window || (foreground && GetWindow(foreground,GW_OWNER)==impl_->window);
    const uint32_t frameLimit=active?impl_->video.frameLimit:impl_->video.backgroundFrameLimit;
    const auto now = std::chrono::steady_clock::now();
    const auto deadline = impl_->framePacer.deadline(now, frameLimit);
    if (now < deadline) {
        PhaseScope phase(impl_->phaseFrame,RenderPhase::Pacing,impl_->reportPhases);
        impl_->frameWaiter.waitUntil(deadline);
    }
    const auto submitted = std::chrono::steady_clock::now();
    {
        PhaseScope phase(impl_->phaseFrame,RenderPhase::Present,impl_->reportPhases);
        check(impl_->device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0), "PresentEx title frame");
    }
    const auto presented = std::chrono::steady_clock::now();
    // No geometry becomes selectable until the frame using it is visible.
    impl_->selectGuiPresentation(source.Get());
    impl_->guiFrame.context = impl_->guiContext;
    impl_->presentedGuiFrame = std::move(impl_->guiFrame);
    impl_->guiFrame = {};
    impl_->guiPresented = true;
    // VSync already spaces visible frames. Do not add another whole software
    // interval after the driver's blocking wait; cap its submission cadence.
    // Immediate presentation instead rebases against completion after stalls.
    impl_->framePacer.presented(impl_->presentation.PresentationInterval == D3DPRESENT_INTERVAL_ONE ? submitted : presented);
    if (gameFrame && impl_->previousGamePresent) {
        const uint64_t interval = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(presented - impl_->lastPresented).count());
        ++impl_->counters.measuredIntervals;
        impl_->counters.measuredNanoseconds += interval;
        impl_->counters.worstIntervalNanoseconds = std::max(impl_->counters.worstIntervalNanoseconds, interval);
        impl_->recentFrameTimes.record(interval);
        const double milliseconds=interval/1e6;
        impl_->overlayMilliseconds=impl_->overlayMilliseconds>0?.9*impl_->overlayMilliseconds+.1*milliseconds:milliseconds;
    }
    if (!gameFrame) impl_->recentFrameTimes.reset();
    impl_->lastPresented = presented;
    impl_->previousGamePresent = gameFrame;
    if (gameFrame) ++impl_->counters.presents;
    else ++impl_->counters.clearPresents;
    if (gameFrame && impl_->reportFrameTimes && impl_->counters.presents % 300 == 0 &&
        impl_->counters.measuredIntervals && impl_->counters.measuredNanoseconds) {
        const auto& metrics = impl_->counters;
        const double averageMilliseconds = double(metrics.measuredNanoseconds) / double(metrics.measuredIntervals) / 1000000.0;
        const double fps = 1000000000.0 * double(metrics.measuredIntervals) / double(metrics.measuredNanoseconds);
        std::fprintf(stderr, "[FRAME TIME LIVE] size=%ux%u game_presents=%llu measured=%llu average_ms=%.3f average_fps=%.3f worst_ms=%.3f\n",
            metrics.width, metrics.height, static_cast<unsigned long long>(metrics.presents),
            static_cast<unsigned long long>(metrics.measuredIntervals), averageMilliseconds, fps,
            double(metrics.worstIntervalNanoseconds) / 1000000.0);
        const auto recent = impl_->recentFrameTimes.summary();
        if (recent.intervals && recent.nanoseconds) {
            std::fprintf(stderr, "[FRAME TIME RECENT] size=%ux%u game_presents=%llu consecutive_intervals=%u window_capacity=%u complete=%u average_ms=%.3f average_fps=%.3f worst_ms=%.3f median_ms=%.3f p95_ms=%.3f p99_ms=%.3f\n",
                metrics.width, metrics.height, static_cast<unsigned long long>(metrics.presents),
                recent.intervals, RecentFrameTimes::capacity, static_cast<uint32_t>(recent.intervals == RecentFrameTimes::capacity),
                double(recent.nanoseconds) / double(recent.intervals) / 1000000.0,
                1000000000.0 * double(recent.intervals) / double(recent.nanoseconds),
                double(recent.worstNanoseconds) / 1000000.0,
                double(recent.medianNanoseconds) / 1000000.0,
                double(recent.p95Nanoseconds) / 1000000.0,
                double(recent.p99Nanoseconds) / 1000000.0);
        }
        std::fflush(stderr);
    }
    impl_->frameEnded = false;
    impl_->framePrepared = impl_->frameCleared = false;
    impl_->frameDraws = 0;
    impl_->phaseFramePending = impl_->reportPhases;
    impl_->phaseGameFrame = gameFrame;
    if (!capturedPixels.empty()) {
        PhaseScope phase(impl_->phaseFrame,RenderPhase::CaptureWrite,impl_->reportPhases);
        const auto path = impl_->captureDirectory / (L"frame-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(capturedFrameNumber) + L".bmp");
        writeFrameBMP(path, impl_->counters.width, impl_->counters.height, capturedPixels);
        if (automaticCapture) impl_->captured = true;
        std::fwprintf(stderr, L"Actual title frame capture: %ls\n", path.c_str());
    }
}
void D3D9Backend::presentInitialization() {
    std::lock_guard lock(impl_->mutex);
    impl_->requireDevice();
    if (impl_->inFrame || impl_->frameEnded || impl_->counters.draws || impl_->counters.initializationPresents)
        throw std::runtime_error("Original title initialization present is outside its verified boundary");
    check(impl_->device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0), "PresentEx original hidden initialization");
    ++impl_->counters.initializationPresents;
    impl_->framePrepared = impl_->frameCleared = false;
    impl_->phaseFramePending = impl_->reportPhases;
    impl_->phaseGameFrame = false;
}
void D3D9Backend::recordBridgePhaseTime(uint64_t nanoseconds) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->reportPhases) impl_->phaseFrame.bridgeNanoseconds += nanoseconds;
}
void D3D9Backend::recordConstantDecodePhaseTime(uint64_t nanoseconds) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->reportPhases) return;
    auto& phase = impl_->phaseFrame.phases[static_cast<size_t>(RenderPhase::ConstantDecode)];
    ++phase.calls;
    phase.nanoseconds += nanoseconds;
    phase.worstNanoseconds = std::max(phase.worstNanoseconds,nanoseconds);
}
void D3D9Backend::finishPhaseFrame() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->reportPhases || !impl_->phaseFramePending) return;
    const auto now = std::chrono::steady_clock::now();
    if (impl_->phaseGameFrame && impl_->previousPhaseGame) {
        impl_->phaseFrame.wallNanoseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - impl_->lastPhaseFrame).count());
        impl_->recentPhases.record(impl_->phaseFrame);
    } else impl_->recentPhases.reset();
    impl_->lastPhaseFrame = now;
    impl_->previousPhaseGame = impl_->phaseGameFrame;
    impl_->phaseFramePending = false;
    impl_->phaseFrame = {};
    if (!impl_->phaseGameFrame || impl_->counters.presents % 300 || !impl_->recentPhases.count()) return;
    const auto summary = impl_->recentPhases.summary();
    const double divisor = double(impl_->recentPhases.count()) * 1000000.0;
    uint64_t phaseSum = 0;
    for (const auto& phase : summary.phases) phaseSum += phase.nanoseconds;
    std::fprintf(stderr,"[RENDER PHASE COVERAGE] presents=%llu consecutive_cycles=%u complete=%u cycle_wall_ms=%.3f bridge_hook_ms=%.3f selected_phase_sum_ms=%.3f outside_bridge_ms=%.3f scope=cpu_wall_calls gpu_execution_measured=false outside_includes_original_cpu_and_other_waits=true\n",
        static_cast<unsigned long long>(impl_->counters.presents),impl_->recentPhases.count(),
        static_cast<uint32_t>(impl_->recentPhases.count() == RecentPhaseTimes::capacity),
        summary.wallNanoseconds / divisor,summary.bridgeNanoseconds / divisor,phaseSum / divisor,
        double(summary.wallNanoseconds > summary.bridgeNanoseconds ? summary.wallNanoseconds - summary.bridgeNanoseconds : 0) / divisor);
    for (size_t index = 0; index < summary.phases.size(); ++index) {
        const auto& phase = summary.phases[index];
        std::fprintf(stderr,"[RENDER PHASE] name=%s calls=%llu average_ms_per_cycle=%.3f worst_call_ms=%.3f\n",
            renderPhaseNames[index],static_cast<unsigned long long>(phase.calls),phase.nanoseconds / divisor,
            phase.worstNanoseconds / 1000000.0);
    }
    std::fflush(stderr);
}
RenderCounters D3D9Backend::counters() const { std::lock_guard lock(impl_->mutex); return impl_->counters; }
size_t D3D9Backend::windowsVariantCount() const { std::lock_guard lock(impl_->mutex); return impl_->windowsCache.variantCount(); }
size_t D3D9Backend::xenonVariantCount() const { std::lock_guard lock(impl_->mutex); return impl_->xenonCache.variantCount(); }
size_t D3D9Backend::matchingVariantCount() const {
    std::lock_guard lock(impl_->mutex);
    size_t count = 0;
    for (const auto& shader : impl_->xenonCache.shaders()) for (const auto& variant : shader.variants)
        if (impl_->windowsCache.find(shader.name, variant.key).variant) ++count;
    return count;
}
}
