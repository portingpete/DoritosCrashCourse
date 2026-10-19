#pragma once
#include "shader_cache.h"
#include <cctype>
#include <d3d9.h>
#include <span>
#include <string>
#include <stdexcept>

namespace Doritos::Graphics {
struct TitleShadowTarget {
    uint32_t width, height, texture, color, depth;
};
inline TitleShadowTarget titleShadowTarget(std::span<const uint8_t> owner) {
    if (owner.size() != 100) throw std::runtime_error("Original shadow target owner must be exactly100 bytes");
    const auto word = [&](uint32_t offset) {
        return uint32_t(owner[offset]) << 24 | uint32_t(owner[offset + 1]) << 16 |
            uint32_t(owner[offset + 2]) << 8 | owner[offset + 3];
    };
    TitleShadowTarget result{word(8),word(12),word(28),word(44),word(72)};
    if (word(0) != 0x82161380 || word(4) != 1 || word(84) || word(88) != 999 || word(96) != 0x2d200196 ||
        !result.width || !result.height || result.width > 8192 || result.height > 8192 ||
        !result.texture || !result.color || !result.depth || result.texture == result.color ||
        result.texture == result.depth || result.color == result.depth)
        throw std::runtime_error("Unverified original nonfloating type1 shadow target owner");
    return result;
}
struct TitleShadowEndArguments {
    uint32_t caller, flags, sourceRectangle, destination, destinationPoint, slice, level,
        clearColor, stack, extra92, extra100;
    double clearDepth;
};
inline void validateTitleShadowEnd(const TitleShadowTarget& target, const TitleShadowEndArguments& args) {
    if (args.caller != 0x823903dc || args.flags != 20 || args.sourceRectangle || args.destination != target.texture ||
        args.destinationPoint || args.slice || args.level || uint64_t(args.stack) + 128 != args.clearColor ||
        args.extra92 || args.extra100 || args.clearDepth != 1.0)
        throw std::runtime_error("Unverified original type1 depth End flags, caller or unused clear arguments");
}
inline bool isTitleShadowConsumer(ShaderStage stage, std::string name, const ShaderKey& key) {
    for (auto& byte : name) byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
    return stage == ShaderStage::Pixel && (name == "composite/ps" || name == "multilayer/ps") &&
        (key[0] & (1u << 21)) && !(key[0] & ((1u << 22) | (1u << 7) | (1u << 6)));
}
inline bool validateTitleShadowSample(ShaderStage stage, const std::string& name, const ShaderKey& key,
    uint32_t declaredSamplers, uint32_t slot, bool matchedOriginal, bool completed, bool attached,
    uint32_t textureType = D3DSTT_2D) {
    if (slot >= 16 || declaredSamplers & ~0xffffu)
        throw std::runtime_error("Native depth sampler declaration exceeds pixel slots");
    if (!(declaredSamplers & (1u << slot))) return false;
    if (slot != 9 || textureType != D3DSTT_2D || !matchedOriginal || !isTitleShadowConsumer(stage,name,key) || !completed || attached)
        throw std::runtime_error("Actual native depth read lacks verified original shadow shader, End or detached target");
    return true;
}
inline bool isOriginalShadowClear(uint32_t caller, std::span<const D3DRECT> rectangles, uint32_t flags,
    uint32_t color, float depth, uint32_t stencil, const D3DVIEWPORT9& viewport, uint32_t width, uint32_t height) {
    return caller == 0x8237d34c && rectangles.empty() && flags == (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER) &&
        color == 0xffffffff && depth == 1.0f && !stencil && !viewport.X && !viewport.Y &&
        viewport.Width == width && viewport.Height == height && viewport.MinZ == 0.0f && viewport.MaxZ == 1.0f;
}

// Inspect only bounded native SM2/3 instruction declarations. This neither
// interprets Xbox GPU code nor creates a substitute shader. An unused retained
// sampler binding must not be mistaken for a read by the current native PS.
struct NativeShaderSamplers {
    uint32_t mask = 0;
    std::array<uint32_t,16> textureTypes{};
};
inline NativeShaderSamplers nativeShaderSamplers(std::span<const uint8_t> code, ShaderStage stage) {
    const auto word = [&](size_t token) {
        const size_t offset = token * 4;
        return uint32_t(code[offset]) | uint32_t(code[offset + 1]) << 8 |
            uint32_t(code[offset + 2]) << 16 | uint32_t(code[offset + 3]) << 24;
    };
    const uint32_t stageToken = stage == ShaderStage::Pixel ? 0xffff0000u : 0xfffe0000u;
    if (code.size() < 8 || code.size() % 4 || (word(0) != (stageToken | 0x0200) &&
        word(0) != (stageToken | 0x0201) && word(0) != (stageToken | 0x0300)))
        throw std::runtime_error("Depth consumer requires bounded native Shader Model2/3 code");
    NativeShaderSamplers result;
    for (size_t token = 1; token < code.size() / 4;) {
        const uint32_t instruction = word(token), opcode = instruction & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) {
            if (token + 1 != code.size() / 4) throw std::runtime_error("Native sampler declaration code has trailing tokens");
            return result;
        }
        if (opcode == D3DSIO_COMMENT) {
            const uint32_t count = (instruction & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT;
            if (count > code.size() / 4 - token - 1) throw std::runtime_error("Truncated native shader comment");
            token += count + 1;
            continue;
        }
        const uint32_t count = (instruction & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
        if (count > code.size() / 4 - token - 1) throw std::runtime_error("Truncated native shader instruction");
        if (opcode == D3DSIO_DCL) {
            if (count != 2) throw std::runtime_error("Unverified native shader declaration extent");
            const uint32_t parameter = word(token + 2);
            const uint32_t type = ((parameter & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) |
                ((parameter & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2);
            if (type == D3DSPR_SAMPLER) {
                const uint32_t sampler = parameter & D3DSP_REGNUM_MASK;
                if (sampler >= 16) throw std::runtime_error("Native depth consumer sampler exceeds pixel slot bounds");
                const uint32_t textureType = word(token + 1) & D3DSP_TEXTURETYPE_MASK;
                if ((result.mask & (1u << sampler)) ||
                    (textureType != D3DSTT_2D && textureType != D3DSTT_CUBE && textureType != D3DSTT_VOLUME))
                    throw std::runtime_error("Ambiguous or unknown native sampler declaration texture kind");
                result.mask |= 1u << sampler;
                result.textureTypes[sampler] = textureType;
            }
        }
        token += count + 1;
    }
    throw std::runtime_error("Native sampler declaration code has no END token");
}
inline uint32_t nativeShaderSamplerMask(std::span<const uint8_t> code, ShaderStage stage) {
    return nativeShaderSamplers(code,stage).mask;
}
}
