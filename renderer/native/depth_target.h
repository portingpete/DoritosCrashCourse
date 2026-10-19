#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <new>

namespace Doritos::Graphics {
struct TitleDepthTargetResources { uint32_t texture = 0, depthSurface = 0; };
struct TitleDepthTargetDescription { uint32_t width, height; };

// Snapshot the resource type at surface creation. A surviving native view must
// not consult a guest parent token which its original owner has released.
enum class TitleSurfaceParent : uint32_t { None = 4, Texture = 3, CubeTexture = 18 };

inline TitleDepthTargetDescription titleDepthTargetDescription(std::span<const uint8_t> owner) {
    if (owner.size() != 100) throw std::runtime_error("Original depth target owner must be exactly100 bytes");
    const auto word = [&](uint32_t offset) {
        return uint32_t(owner[offset]) << 24 | uint32_t(owner[offset + 1]) << 16 |
            uint32_t(owner[offset + 2]) << 8 | owner[offset + 3];
    };
    const uint32_t width = word(8), height = word(12);
    if (word(0) != 0x82161380 || word(4) != 1 || !width || !height || width > 8192 || height > 8192 ||
        word(28) || word(72) || word(84) || word(88) != 999 || word(96) != 0x2d200196)
        throw std::runtime_error("Unverified original D24S8 type1 target allocation contract");
    return {width,height};
}

// Publication is atomic with respect to the caller's registry lock. Native
// handles remain owned by the outer transaction until both tokens are stored.
// Allocate/initialize/free operate only on newly owned token storage.
template<class Registry, class Entry, class MakeView, class Allocate, class Initialize, class Free>
TitleDepthTargetResources publishTitleDepthTargetPair(Registry& registry, Entry texture,
    MakeView makeView, Allocate allocate, Initialize initialize, Free free) {
    TitleDepthTargetResources result;
    bool textureStored = false, viewStored = false;
    try {
        result.texture = allocate();
        initialize(result.texture,texture);
        result.depthSurface = allocate();
        auto view = makeView(result.texture);
        initialize(result.depthSurface,view);
        if (!registry.emplace(result.texture,std::move(texture)).second)
            throw std::runtime_error("Native depth texture token collides with a live resource");
        textureStored = true;
        if (!registry.emplace(result.depthSurface,std::move(view)).second)
            throw std::runtime_error("Native depth view token collides with a live resource");
        viewStored = true;
        return result;
    } catch (...) {
        if (viewStored) registry.erase(result.depthSurface);
        if (textureStored) registry.erase(result.texture);
        if (result.depthSurface) free(result.depthSurface);
        if (result.texture) free(result.texture);
        throw;
    }
}

template<class CreateTexture, class CreateView, class Publish, class Release>
TitleDepthTargetResources allocateTitleDepthTargetPair(CreateTexture createTexture,
    CreateView createView, Publish publish, Release release) {
    uint32_t texture = 0, view = 0;
    try {
        texture = createTexture();
        if (!texture) return {};
        view = createView(texture);
        if (!view) { release(texture); return {}; }
        return publish(texture,view);
    } catch (const std::bad_alloc&) {
        if (view) release(view);
        if (texture) release(texture);
        return {};
    } catch (...) {
        if (view) release(view);
        if (texture) release(texture);
        throw;
    }
}
}
