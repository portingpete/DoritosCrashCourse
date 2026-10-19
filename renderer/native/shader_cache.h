#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Doritos::Graphics {
enum class ShaderCachePlatform { Windows, Xenon };
enum class ShaderStage { Vertex, Pixel };
using ShaderKey = std::array<uint32_t, 3>;
struct ShaderParameter {
    uint32_t id;
    uint32_t firstRegister;
    uint32_t registerCount;
    uint32_t reserved;
    bool operator==(const ShaderParameter&) const = default;
};
struct ShaderVariant {
    ShaderKey key;
    std::vector<ShaderParameter> parameters;
    // Aligned storage is required by IDirect3DDevice9::Create*Shader.
    std::vector<uint32_t> bytecode;
};
struct CachedShader {
    std::string name;
    ShaderStage stage;
    std::vector<ShaderVariant> variants;
};
struct ShaderMatch {
    const CachedShader* shader;
    const ShaderVariant* variant;
};
struct ShaderSelection {
    std::string name;
    ShaderKey key;
};

// Grammar is verified from this title's DIRECTXSHADERMANAGER::LoadShaders and
// sub_82387AE0, and consumed to exact EOF in both original package caches.
class ShaderCache {
public:
    static ShaderCache parse(std::span<const uint8_t> bytes, ShaderCachePlatform platform);
    ShaderCache withWindowsExtension(const ShaderCache& extension) const;
    ShaderCache withXenonSourceExtension(const ShaderCache& extension) const;
    const std::vector<CachedShader>& shaders() const { return shaders_; }
    uint32_t shaderModel() const { return shaderModel_; }
    uint32_t cacheFlags() const { return cacheFlags_; }
    size_t variantCount() const;
    ShaderMatch find(const std::string& name, const ShaderKey& key) const;
    // Original CPU loader identity disambiguates identical Xenon code without
    // choosing an arbitrary native feature variant. Code and stage still match.
    ShaderMatch requireCodeVariant(const ShaderSelection& selection, ShaderStage expectedStage,
        std::span<const uint8_t> code) const;
    // Bytecode alone can match multiple feature keys. Callers must resolve all
    // matches and reject divergent native bindings rather than choosing one.
    std::vector<ShaderMatch> matchCode(std::span<const uint8_t> code) const;
private:
    ShaderCache withExtension(const ShaderCache& extension, ShaderCachePlatform platform) const;
    uint32_t shaderModel_ = 0;
    uint32_t cacheFlags_ = 0;
    ShaderCachePlatform platform_ = ShaderCachePlatform::Windows;
    std::vector<CachedShader> shaders_;
};
}
