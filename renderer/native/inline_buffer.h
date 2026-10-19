#pragma once
#include <cstdint>
#include <stdexcept>

namespace Doritos::Graphics {
// Ordinary adapter tokens/backing are graphics allocations. The two original
// AvatarPose palette slots instead live inside a CPU owner and share its real
// physical allocation. Neither address is owned by the renderer.
struct TitleBufferStorage {
    bool ownedToken = true;
    bool ownedData = true;
    uint32_t owner = 0, backingBase = 0, offset = 0;
    bool borrows(uint32_t physicalBase) const {
        return physicalBase && !ownedToken && !ownedData && backingBase == physicalBase;
    }
};

struct TitleAvatarBufferBinding {
    static constexpr uint32_t bytes = 3456, backingBytes = bytes * 2, ownerBytes = 432;
    uint32_t storage, data;
    TitleBufferStorage lifetime;
};

inline TitleAvatarBufferBinding titleAvatarBufferBinding(uint32_t owner, uint32_t storage,
    uint32_t offset, uint32_t physicalBase) {
    if (!owner || !physicalBase || uint64_t(owner) + TitleAvatarBufferBinding::ownerBytes > 0x100000000ULL ||
        uint64_t(physicalBase) + TitleAvatarBufferBinding::backingBytes > 0x100000000ULL ||
        (offset != 0 && offset != TitleAvatarBufferBinding::bytes) ||
        uint64_t(owner) + 336 + (offset ? 32 : 0) != storage)
        throw std::runtime_error("Unverified original AvatarPose inline buffer storage/backing bounds");
    return {storage, physicalBase + offset, {false, false, owner, physicalBase, offset}};
}

// The original physical-free wrapper owns the lifetime boundary. Pending
// registrations also carry backingBase, so failure between header creation
// and address association cannot leave an alias behind. A failed native release
// leaves that entry registered and prevents the original physical free.
template<class Registry, class Release>
uint32_t detachTitleBorrowedBuffers(Registry& registry, uint32_t physicalBase, Release release) {
    uint32_t detached = 0;
    for (auto entry = registry.begin(); entry != registry.end();) {
        if (!entry->second.storage.borrows(physicalBase)) { ++entry; continue; }
        release(entry->first, entry->second);
        entry = registry.erase(entry);
        ++detached;
    }
    return detached;
}
}
