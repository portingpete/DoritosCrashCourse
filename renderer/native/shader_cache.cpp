#include "shader_cache.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace Doritos::Graphics {
namespace {
constexpr size_t kMaxCacheBytes = 16 * 1024 * 1024;
constexpr uint32_t kMaxShaders = 256, kMaxVariants = 4096, kMaxParameters = 1024;
class Reader {
    std::span<const uint8_t> bytes_;
    size_t position_ = 0;
    bool bigEndian_;
public:
    Reader(std::span<const uint8_t> bytes, bool bigEndian) : bytes_(bytes), bigEndian_(bigEndian) {}
    std::span<const uint8_t> take(size_t count) {
        if (count > bytes_.size() - position_) throw std::runtime_error("Truncated Tongas shader cache");
        const auto result = bytes_.subspan(position_, count);
        position_ += count;
        return result;
    }
    uint32_t word() {
        const auto b = take(4);
        if (bigEndian_) return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
        return uint32_t(b[3]) << 24 | uint32_t(b[2]) << 16 | uint32_t(b[1]) << 8 | b[0];
    }
    bool done() const { return position_ == bytes_.size(); }
};
std::string folded(std::string name) {
    for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return name;
}
ShaderStage stage(const std::string& name) {
    const auto lower = folded(name);
    if (lower.ends_with("/vs") || lower.ends_with("_vs")) return ShaderStage::Vertex;
    if (lower.ends_with("/ps") || lower.ends_with("_ps")) return ShaderStage::Pixel;
    throw std::runtime_error("Unsupported Tongas shader stage name: " + name);
}
void validateWindowsTokens(const ShaderVariant& variant, ShaderStage expectedStage) {
    const auto& code = variant.bytecode;
    if (code.size() < 2) throw std::runtime_error("Truncated Windows shader bytecode");
    const uint32_t tag = code.front() >> 16;
    const uint32_t major = (code.front() >> 8) & 255;
    if (tag != (expectedStage == ShaderStage::Vertex ? 0xfffeu : 0xffffu) || major < 2 || major > 3)
        throw std::runtime_error("Windows shader stage/profile does not match its cache entry");
    // SM2/SM3 instruction tokens encode their operand count. Comments carry a
    // separate 15-bit DWORD count. END must be the final token: this bounds the
    // pointer-only D3D9 CreateShader API before passing user-owned bytecode to it.
    size_t position = 1;
    while (position < code.size()) {
        const uint32_t token = code[position++];
        const uint32_t opcode = token & 65535;
        if (opcode == 65535) {
            if (token != 65535 || position != code.size()) throw std::runtime_error("Windows shader END is not terminal");
            return;
        }
        const uint32_t operands = opcode == 65534 ? (token >> 16) & 32767 : (token >> 24) & 15;
        if (operands > code.size() - position) throw std::runtime_error("Windows shader instruction exceeds bytecode");
        position += operands;
    }
    throw std::runtime_error("Windows shader bytecode has no END token");
}
}

ShaderCache ShaderCache::parse(std::span<const uint8_t> bytes, ShaderCachePlatform platform) {
    if (bytes.size() < 12 || bytes.size() > kMaxCacheBytes) throw std::runtime_error("Invalid Tongas shader cache size");
    Reader reader(bytes, platform == ShaderCachePlatform::Xenon);
    ShaderCache result;
    result.platform_ = platform;
    result.shaderModel_ = reader.word();
    result.cacheFlags_ = reader.word();
    if (result.shaderModel_ != 3 || result.cacheFlags_ != 0) throw std::runtime_error("Unsupported Tongas shader cache header");
    const uint32_t shaderCount = reader.word();
    if (!shaderCount || shaderCount > kMaxShaders) throw std::runtime_error("Invalid Tongas cached shader count");
    uint32_t totalVariants = 0;
    std::set<std::string> names;
    for (uint32_t index = 0; index < shaderCount; ++index) {
        const uint32_t nameBytes = reader.word();
        if (nameBytes < 2 || nameBytes > 256) throw std::runtime_error("Invalid Tongas cached shader name length");
        const auto name = reader.take(nameBytes);
        if (name.back() != 0) throw std::runtime_error("Tongas cached shader name has no terminator");
        for (size_t n = 0; n + 1 < name.size(); ++n)
            if (name[n] < 33 || name[n] > 126 || name[n] == '\\' || name[n] == ':')
                throw std::runtime_error("Invalid Tongas cached shader name");
        CachedShader shader;
        shader.name.assign(reinterpret_cast<const char*>(name.data()), name.size() - 1);
        shader.stage = stage(shader.name);
        if (!names.insert(folded(shader.name)).second) throw std::runtime_error("Duplicate Tongas cached shader name");
        const uint32_t count = reader.word();
        if (!count || count > kMaxVariants - totalVariants) throw std::runtime_error("Invalid Tongas shader variant count");
        totalVariants += count;
        std::set<ShaderKey> keys;
        for (uint32_t variantIndex = 0; variantIndex < count; ++variantIndex) {
            ShaderVariant variant;
            for (auto& feature : variant.key) feature = reader.word();
            if (!keys.insert(variant.key).second) throw std::runtime_error("Duplicate Tongas shader feature key");
            const uint32_t parameterCount = reader.word();
            if (parameterCount > kMaxParameters) throw std::runtime_error("Excessive Tongas shader parameter count");
            std::set<uint32_t> parameters;
            for (uint32_t parameterIndex = 0; parameterIndex < parameterCount; ++parameterIndex) {
                ShaderParameter parameter{reader.word(), reader.word(), reader.word(), reader.word()};
                if (!parameters.insert(parameter.id).second || !parameter.registerCount || parameter.firstRegister > 4096 ||
                    parameter.registerCount > 4096 - parameter.firstRegister || parameter.reserved != 0)
                    throw std::runtime_error("Invalid Tongas shader register binding");
                variant.parameters.push_back(parameter);
            }
            const uint32_t codeBytes = reader.word();
            if (codeBytes < 8 || codeBytes > 1024 * 1024 || codeBytes % 4) throw std::runtime_error("Invalid Tongas shader bytecode size");
            const auto code = reader.take(codeBytes);
            variant.bytecode.resize(codeBytes / 4);
            // Code bytes remain in their original platform encoding. Windows is
            // little endian; Xenon is matched as an opaque original byte string.
            std::memcpy(variant.bytecode.data(), code.data(), codeBytes);
            if (platform == ShaderCachePlatform::Windows) validateWindowsTokens(variant, shader.stage);
            shader.variants.push_back(std::move(variant));
        }
        result.shaders_.push_back(std::move(shader));
    }
    if (!reader.done()) throw std::runtime_error("Trailing bytes in Tongas shader cache");
    return result;
}
ShaderCache ShaderCache::withWindowsExtension(const ShaderCache& extension) const {
    return withExtension(extension,ShaderCachePlatform::Windows);
}
ShaderCache ShaderCache::withXenonSourceExtension(const ShaderCache& extension) const {
    for (const auto& shader : extension.shaders_) {
        if (shader.name != "multilayer/vs" && shader.name != "multilayer/ps" &&
            shader.name != "composite/vs" && shader.name != "composite/ps")
            throw std::runtime_error("Unverified original source shader extension family");
    }
    return withExtension(extension,ShaderCachePlatform::Xenon);
}
ShaderCache ShaderCache::withExtension(const ShaderCache& extension, ShaderCachePlatform platform) const {
    if (platform_ != platform || extension.platform_ != platform ||
        shaderModel_ != 3 || extension.shaderModel_ != 3 || cacheFlags_ || extension.cacheFlags_ ||
        variantCount() > kMaxVariants - extension.variantCount())
        throw std::runtime_error("Invalid or excessive native shader cache extension platform");
    ShaderCache result = *this;
    for (const auto& addition : extension.shaders_) {
        CachedShader* destination = nullptr;
        for (auto& shader : result.shaders_) if (folded(shader.name) == folded(addition.name)) { destination = &shader; break; }
        if (!destination) {
            if (result.shaders_.size() == kMaxShaders) throw std::runtime_error("Native shader extension exceeds shader count limit");
            result.shaders_.push_back(addition);
            continue;
        }
        if (destination->stage != addition.stage) throw std::runtime_error("Native shader extension changes an existing stage");
        for (const auto& variant : addition.variants) {
            for (const auto& existing : destination->variants) if (existing.key == variant.key)
                throw std::runtime_error("Native shader extension would replace an existing original feature key");
            destination->variants.push_back(variant);
        }
    }
    uint64_t bytes = 12;
    for (const auto& shader : result.shaders_) {
        bytes += 9 + shader.name.size();
        for (const auto& variant : shader.variants)
            bytes += 20 + variant.parameters.size() * 16 + variant.bytecode.size() * 4;
    }
    if (bytes > kMaxCacheBytes) throw std::runtime_error("Combined native shader cache exceeds original parser byte limit");
    return result;
}
size_t ShaderCache::variantCount() const {
    size_t count = 0;
    for (const auto& shader : shaders_) count += shader.variants.size();
    return count;
}
ShaderMatch ShaderCache::find(const std::string& name, const ShaderKey& key) const {
    const auto lower = folded(name);
    for (const auto& shader : shaders_) {
        if (folded(shader.name) != lower) continue;
        for (const auto& variant : shader.variants) if (variant.key == key) return {&shader, &variant};
    }
    return {nullptr, nullptr};
}
ShaderMatch ShaderCache::requireCodeVariant(const ShaderSelection& selection, ShaderStage expectedStage,
    std::span<const uint8_t> code) const {
    if (platform_ != ShaderCachePlatform::Xenon)
        throw std::runtime_error("Original shader identity requires the Xenon cache");
    const auto match = find(selection.name, selection.key);
    const std::string description = selection.name + " [" + std::to_string(selection.key[0]) + "," +
        std::to_string(selection.key[1]) + "," + std::to_string(selection.key[2]) + "]";
    if (!match.variant) throw std::runtime_error("Original shader loader requested an uncached feature key: " + description);
    if (match.shader->stage != expectedStage)
        throw std::runtime_error("Original shader loader identity has the wrong stage: " + description);
    if (match.variant->bytecode.size() * sizeof(uint32_t) != code.size() ||
        std::memcmp(match.variant->bytecode.data(), code.data(), code.size()))
        throw std::runtime_error("Original shader loader identity does not match its exact Xenon bytecode: " + description);
    return match;
}
std::vector<ShaderMatch> ShaderCache::matchCode(std::span<const uint8_t> code) const {
    std::vector<ShaderMatch> matches;
    for (const auto& shader : shaders_) for (const auto& variant : shader.variants)
        if (variant.bytecode.size() * sizeof(uint32_t) == code.size() &&
            std::memcmp(variant.bytecode.data(), code.data(), code.size()) == 0) matches.push_back({&shader, &variant});
    return matches;
}
}
