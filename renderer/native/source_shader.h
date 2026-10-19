#pragma once
#include "shader_cache.h"
#include "guest_span.h"
#include <cstring>
#include <set>
#include <stdexcept>

namespace Doritos::Graphics {
// Original source loader82387140 owns a 2544-byte variant; checked prefix2540:
// key+8,
// parameter count+52, 155 inline 16-byte records+56, master owner+2536.
// Its cached loader uses a different allocation contract and cannot harvest.
inline ShaderVariant originalSourceShader(const ShaderSelection& selection, ShaderStage stage,
    uint32_t master, std::span<const uint8_t> variant, std::span<const uint8_t> code) {
    if (!master || variant.size() != 2540 || titleGuestWord(variant,2536) != master ||
        (selection.name != (stage == ShaderStage::Vertex ? "multilayer/vs" : "multilayer/ps") &&
         selection.name != (stage == ShaderStage::Vertex ? "composite/vs" : "composite/ps")))
        throw std::runtime_error("Unverified original source shader owner/family/stage");
    for (size_t word = 0; word < 3; ++word)
        if (titleGuestWord(variant,8 + word * 4) != selection.key[word])
            throw std::runtime_error("Original source shader key disagrees with its owner");
    if (code.size() < 12 || code.size() > 1024 * 1024 || code.size() % 4)
        throw std::runtime_error("Invalid original source shader code size");
    const uint32_t header = titleGuestWord(code,4), instructions = titleGuestWord(code,8);
    if (header < 12 || header >= code.size() || header % 4 || instructions % 4 || instructions != code.size() - header)
        throw std::runtime_error("Original source shader code header disagrees with its span");
    const uint32_t count = titleGuestWord(variant,52);
    if (count > 155) throw std::runtime_error("Original source shader reflection exceeds inline records");
    ShaderVariant result{selection.key};
    std::set<uint32_t> ids;
    for (uint32_t index = 0; index < count; ++index) {
        const size_t offset = 56 + index * 16;
        ShaderParameter parameter{titleGuestWord(variant,offset),titleGuestWord(variant,offset + 4),
            titleGuestWord(variant,offset + 8),titleGuestWord(variant,offset + 12)};
        // ID154's fourth word may be a real custom-parameter index. Capture it
        // verbatim; offline native conversion separately rejects that contract.
        if (parameter.id >= 155 || !ids.insert(parameter.id).second ||
            !parameter.registerCount || parameter.firstRegister > 256 ||
            parameter.registerCount > 256 - parameter.firstRegister)
            throw std::runtime_error("Invalid original source shader reflected binding");
        result.parameters.push_back(parameter);
    }
    result.bytecode.resize(code.size()/4);
    std::memcpy(result.bytecode.data(),code.data(),code.size());
    return result;
}
}
