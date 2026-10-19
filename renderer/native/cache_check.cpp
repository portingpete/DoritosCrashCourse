// Inspect the user's original caches; optionally validate all Windows shader
// objects on a real hardware device. No geometry, clear or Present is issued.
#include "d3d9_backend.h"
#include "shadow_target.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <regex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using namespace Doritos::Graphics;
static std::vector<uint8_t> readFile(const fs::path& path) {
    fs::path partial;
    for (const auto& component : fs::absolute(path)) {
        partial /= component;
        const DWORD attributes = GetFileAttributesW(partial.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Shader cache path contains a reparse point");
    }
    const auto length = fs::file_size(path);
    if (!length || length > 16 * 1024 * 1024) throw std::runtime_error("Invalid shader cache file size");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read complete shader cache");
    return bytes;
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 8) {
        std::fprintf(stderr, "Usage: NativeShaderCacheCheck WINDOWS_CACHE XENON_CACHE [--hardware] [--extension NATIVE_CACHE] [--source-extension ORIGINAL_SOURCE_CACHE]\n");
        return 2;
    }
    HWND window = nullptr;
    try {
        bool hardware = false;
        fs::path extensionPath, sourceExtensionPath;
        for (int index = 3; index < argc; ++index) {
            if (std::wstring(argv[index]) == L"--hardware" && !hardware) hardware = true;
            else if (std::wstring(argv[index]) == L"--extension" && extensionPath.empty() && index + 1 < argc)
                extensionPath = argv[++index];
            else if (std::wstring(argv[index]) == L"--source-extension" && sourceExtensionPath.empty() && index + 1 < argc)
                sourceExtensionPath = argv[++index];
            else throw std::runtime_error("Invalid or repeated native cache validation option");
        }
        const auto windowsBytes = readFile(argv[1]);
        const auto xenonBytes = readFile(argv[2]);
        const auto extensionBytes = extensionPath.empty() ? std::vector<uint8_t>{} : readFile(extensionPath);
        const auto sourceExtensionBytes = sourceExtensionPath.empty() ? std::vector<uint8_t>{} : readFile(sourceExtensionPath);
        auto windowsCache = ShaderCache::parse(windowsBytes, ShaderCachePlatform::Windows);
        const size_t originalWindowsVariants = windowsCache.variantCount();
        if (!extensionBytes.empty()) windowsCache = windowsCache.withWindowsExtension(ShaderCache::parse(extensionBytes, ShaderCachePlatform::Windows));
        auto xenonCache = ShaderCache::parse(xenonBytes, ShaderCachePlatform::Xenon);
        if (!sourceExtensionBytes.empty()) xenonCache = xenonCache.withXenonSourceExtension(ShaderCache::parse(sourceExtensionBytes,ShaderCachePlatform::Xenon));
        size_t nativePixelSamplerAudits = 0, mainShadowConsumers = 0;
        for (const auto& shader : windowsCache.shaders()) if (shader.stage == ShaderStage::Pixel)
            for (const auto& variant : shader.variants) {
                const auto code = std::span(reinterpret_cast<const uint8_t*>(variant.bytecode.data()),variant.bytecode.size() * 4);
                NativeShaderSamplers samplers;
                try { samplers = nativeShaderSamplers(code,shader.stage); }
                catch (const std::runtime_error& error) {
                    throw std::runtime_error(std::string(error.what()) + " name=" + shader.name + " version=" + std::to_string(variant.bytecode.front()));
                }
                Microsoft::WRL::ComPtr<ID3DBlob> disassembly;
                if (FAILED(D3DDisassemble(code.data(),code.size(),0,nullptr,&disassembly)))
                    throw std::runtime_error("Primary D3DDisassemble rejected actual native sampler audit code");
                const std::string text(static_cast<const char*>(disassembly->GetBufferPointer()),disassembly->GetBufferSize());
                const uint32_t mask = samplers.mask;
                const std::regex declaration(R"(\bdcl_(2d|cube|volume)\s+s([0-9]+)\b)");
                uint32_t disassembledMask = 0;
                for (std::sregex_iterator entry(text.begin(),text.end(),declaration), end; entry != end; ++entry) {
                    const auto slot = std::stoul((*entry)[2].str());
                    if (slot >= 16) throw std::runtime_error("Primary disassembly sampler slot exceeds pixel bounds");
                    const uint32_t kind = (*entry)[1] == "2d" ? D3DSTT_2D : ((*entry)[1] == "cube" ? D3DSTT_CUBE : D3DSTT_VOLUME);
                    if (samplers.textureTypes[slot] != kind) throw std::runtime_error("Native sampler texture kind disagrees with primary disassembly");
                    disassembledMask |= 1u << slot;
                }
                if (mask != disassembledMask) throw std::runtime_error("Actual native sampler mask disagrees with primary D3D disassembly");
                ++nativePixelSamplerAudits;
                if (isTitleShadowConsumer(shader.stage,shader.name,variant.key)) {
                    if (!(mask & (1u << 9)) || samplers.textureTypes[9] != D3DSTT_2D ||
                        !std::regex_search(text,std::regex(R"(\btexldp(?:_[a-z]+)?\s+[^\r\n]*\bs9\b)")))
                        throw std::runtime_error("Actual eligible main-family shadow key lacks sampler9 2D projected comparison branch");
                    ++mainShadowConsumers;
                }
            }
        std::printf("Native sampler audit: pixel_variants=%zu disassembly_matches=%zu main_nonfloat_shadow_consumers=%zu\n",
            nativePixelSamplerAudits,nativePixelSamplerAudits,mainShadowConsumers);
        size_t originalIdentityVariants = 0, nativeIdentityStageMatches = 0, bonePaletteBindings = 0;
        for (const auto& shader : xenonCache.shaders()) for (const auto& variant : shader.variants) {
            const ShaderSelection identity{shader.name, variant.key};
            const auto code = std::span(reinterpret_cast<const uint8_t*>(variant.bytecode.data()), variant.bytecode.size() * 4);
            const auto selected = xenonCache.requireCodeVariant(identity, shader.stage, code);
            if (selected.shader != &shader || selected.variant != &variant)
                throw std::runtime_error("Actual original shader identity selected a different cache record");
            ++originalIdentityVariants;
            const auto native = windowsCache.find(identity.name, identity.key);
            if (native.variant) {
                if (native.shader->stage != shader.stage)
                    throw std::runtime_error("Actual native counterpart changes the original shader identity stage");
                ++nativeIdentityStageMatches;
                if (isTitleBonePaletteShader(shader.stage,shader.name,variant.key)) {
                    const ShaderParameter* originalPalette = nullptr;
                    const ShaderParameter* nativePalette = nullptr;
                    for (const auto& parameter : variant.parameters) if (parameter.id == 81) originalPalette = &parameter;
                    for (const auto& parameter : native.variant->parameters) if (parameter.id == 81) nativePalette = &parameter;
                    if (!originalPalette || !nativePalette || !isTitleBonePaletteBinding(shader.stage,shader.name,variant.key,
                        *originalPalette,*nativePalette))
                        throw std::runtime_error("Actual main-family skin variant violates verified native palette reflection bounds");
                    ++bonePaletteBindings;
                }
            }
        }
        D3D9Backend backend;
        backend.loadCaches(windowsBytes, xenonBytes, extensionBytes,sourceExtensionBytes);
        size_t hardwareShaders = 0, hardwareIdentityShaders = 0, ambiguousIdentityRejections = 0;
        if (hardware) {
            const auto instance = GetModuleHandleW(nullptr);
            WNDCLASSW type{};
            type.lpfnWndProc = DefWindowProcW;
            type.hInstance = instance;
            type.lpszClassName = L"DoritosNativeShaderValidation";
            if (!RegisterClassW(&type)) throw std::runtime_error("Cannot register native shader validation window");
            window = CreateWindowW(type.lpszClassName, L"Native shader validation", WS_OVERLAPPEDWINDOW,
                0, 0, 2560, 1440, nullptr, nullptr, instance, nullptr);
            if (!window) throw std::runtime_error("Cannot create hidden native shader validation window");
            backend.initialize(window, 2560, 1440);
            for (const auto& shader : windowsCache.shaders()) for (const auto& variant : shader.variants) {
                const auto handle = backend.createShader(shader.name, variant.key);
                backend.release(handle);
                ++hardwareShaders;
            }
            // Use actual shipped duplicate Xenon code, with divergent native
            // feature variants. Both explicit identities must create and bind
            // their own real HAL shader; the unnamed request must still fail.
            for (const auto& original : xenonCache.shaders()) {
                if (hardwareIdentityShaders) break;
                for (const auto& variant : original.variants) {
                    const auto code = std::span(reinterpret_cast<const uint8_t*>(variant.bytecode.data()), variant.bytecode.size() * 4);
                    const auto aliases = xenonCache.matchCode(code);
                    if (aliases.size() < 2) continue;
                    ShaderMatch first{}, second{};
                    bool covered = true;
                    for (const auto& alias : aliases) {
                        const auto native = windowsCache.find(alias.shader->name, alias.variant->key);
                        if (!native.variant || alias.shader->stage != original.stage) { covered = false; break; }
                        if (!first.variant) first = alias;
                        else {
                            const auto baseline = windowsCache.find(first.shader->name, first.variant->key);
                            if (native.variant->bytecode != baseline.variant->bytecode || native.variant->parameters != baseline.variant->parameters)
                                second = alias;
                        }
                    }
                    if (!covered || !second.variant) continue;
                    for (const auto alias : {first, second}) {
                        const ShaderSelection identity{alias.shader->name, alias.variant->key};
                        if (backend.optionalAvatarShaderUnavailable(code, original.stage, &identity))
                            throw std::runtime_error("Real covered shader identity was marked unavailable");
                        const uint32_t handle = backend.createShaderForXenon(code, original.stage, &identity);
                        backend.bindShader(original.stage, handle);
                        backend.bindShader(original.stage, 0);
                        backend.release(handle);
                        ++hardwareIdentityShaders;
                    }
                    auto rejectsAmbiguous = [&](auto request) {
                        try { request(); }
                        catch (const std::runtime_error& error) {
                            const std::string message(error.what());
                            if (message.find("ambiguous") != std::string::npos || message.find("Ambiguous") != std::string::npos) {
                                ++ambiguousIdentityRejections;
                                return;
                            }
                            throw;
                        }
                        throw std::runtime_error("Unnamed divergent shader aliases were accepted");
                    };
                    rejectsAmbiguous([&] { backend.optionalAvatarShaderUnavailable(code, original.stage); });
                    rejectsAmbiguous([&] { backend.createShaderForXenon(code, original.stage); });
                    break;
                }
            }
            if (hardwareIdentityShaders != 2 || ambiguousIdentityRejections != 2)
                throw std::runtime_error("Actual caches lack the required divergent-code-alias hardware contract");
        }
        const auto counters = backend.counters();
        std::printf("{\"windows_shaders\":%zu,\"windows_variants\":%zu,\"original_windows_variants\":%zu,\"native_extension_variants\":%zu,\"xenon_shaders\":%zu,\"xenon_variants\":%zu,"
            "\"exact_key_matches\":%zu,\"original_identity_variants\":%zu,\"native_identity_stage_matches\":%zu,\"bone_palette_bindings\":%zu,\"hardware_shader_objects\":%zu,\"hardware_identity_shader_objects\":%zu,\"ambiguous_identity_rejections\":%zu,\"backbuffer_width\":%u,\"backbuffer_height\":%u,"
            "\"draws\":%llu,\"presents\":%llu,\"gameplay_verified\":false}\n",
            windowsCache.shaders().size(), backend.windowsVariantCount(), originalWindowsVariants, backend.windowsVariantCount() - originalWindowsVariants,
            xenonCache.shaders().size(), backend.xenonVariantCount(),
            backend.matchingVariantCount(), originalIdentityVariants, nativeIdentityStageMatches,bonePaletteBindings,
            hardwareShaders, hardwareIdentityShaders, ambiguousIdentityRejections, counters.width, counters.height,
            static_cast<unsigned long long>(counters.draws), static_cast<unsigned long long>(counters.presents));
        if (window) DestroyWindow(window);
        return 0;
    } catch (const std::exception& error) {
        if (window) DestroyWindow(window);
        std::fprintf(stderr, "Native shader cache validation stopped: %s\n", error.what());
        return 1;
    }
}
