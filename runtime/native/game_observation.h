#pragma once
#include "runtime.h"
#include <cmath>
#include <cstring>
#include <optional>

namespace Doritos::Native {
struct OriginalPlayerPosition {
    uint32_t game, scene, fighter, profile, playerIndex, user;
    uint64_t handle;
    float x, y, z;
};

// Read from the original guest update/present thread, while it owns the scene.
// This samples original CPU fields only. Availability does not assert a game
// phase, successful rendering, movement, or an atomic physics snapshot.
inline std::optional<OriginalPlayerPosition> observeOriginalPlayerPosition(const Memory& guest, uint32_t user) {
    constexpr uint32_t gameGlobal = 0x82b5caa0, sceneGlobal = 0x82baa900, profilesGlobal = 0x82b5caa4;
    constexpr uint32_t gameTable = 0x8213c9cc, fighterTable = 0x8214582c, profileTable = 0x8214d6e8;
    const auto span = [&](uint64_t address, uint32_t bytes) {
        return address && !(address & 3) && address <= UINT32_MAX &&
            address + bytes <= (uint64_t{1} << 32) && guest.valid(uint32_t(address), bytes);
    };
    if (user >= 4 || !span(gameGlobal, 4) || !span(sceneGlobal, 4) || !span(profilesGlobal, 4))
        return std::nullopt;
    try {
        const uint32_t game = guest.read32(gameGlobal), scene = guest.read32(sceneGlobal);
        const uint32_t profiles = guest.read32(profilesGlobal);
        if (!span(game, 368) || !span(profiles, 16) || !scene || (scene & 3) || guest.read32(game) != gameTable)
            return std::nullopt;
        const uint32_t count = guest.read32(game + 364), profile = guest.read32(profiles + user * 4);
        if (!count || count > 4 || !span(uint64_t(game) + 368, count * 56) || !span(profile, 8) ||
            guest.read32(profile) != profileTable || guest.read32(profile + 4) != user)
            return std::nullopt;

        // 821B57A8 records a genuine local title profile at record+20. Remote
        // records use a different profile source and cannot satisfy this link.
        std::optional<OriginalPlayerPosition> sample;
        uint32_t sampledRecord = 0, sampledEntry = 0;
        for (uint32_t index = 0; index < count; ++index) {
            const uint32_t record = game + 368 + index * 56;
            if (guest.read32(record + 20) != profile) continue;
            if (sample) return std::nullopt; // An ambiguous profile association is not a local-player observation.
            const uint64_t handle = guest.read64(record);
            const uint32_t slot = uint32_t(handle >> 16) & 0xffff;
            // Exact 82241288 ABI: signed16 handle bits16..31, 6000 entries,
            // registry pointer+4, followed by a full64 object+16 comparison.
            if (slot >= 6000) return std::nullopt;
            const uint64_t entry = uint64_t(scene) + 0xd334 + uint64_t(slot) * 16 + 4;
            if (!span(entry, 4)) return std::nullopt;
            const uint32_t fighter = guest.read32(uint32_t(entry));
            if (!span(fighter, 1824) || guest.read32(fighter) != fighterTable ||
                guest.read64(fighter + 16) != handle || guest.read32(fighter + 1816) != index ||
                guest.read32(fighter + 1820) != user)
                return std::nullopt;
            const auto position = [&](uint32_t offset) {
                const uint32_t bits = guest.read32(fighter + offset);
                float value; std::memcpy(&value, &bits, sizeof(value)); return value;
            };
            const float x = position(136), y = position(140), z = position(144);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return std::nullopt;
            sample = OriginalPlayerPosition{game, scene, fighter, profile, index, user, handle, x, y, z};
            sampledRecord = record; sampledEntry = uint32_t(entry);
        }
        if (!sample) return std::nullopt;
        // Recheck ownership after the position reads, so a replaced handle,
        // compacted player table or switched profile does not get reported.
        const auto& value = *sample;
        if (guest.read32(gameGlobal) != game || guest.read32(sceneGlobal) != scene ||
            guest.read32(profilesGlobal) != profiles || guest.read32(game) != gameTable ||
            guest.read32(game + 364) != count || guest.read32(profiles + user * 4) != profile ||
            guest.read32(profile) != profileTable || guest.read32(profile + 4) != user ||
            guest.read64(sampledRecord) != value.handle || guest.read32(sampledRecord + 20) != profile ||
            guest.read32(sampledEntry) != value.fighter || guest.read32(value.fighter) != fighterTable ||
            guest.read64(value.fighter + 16) != value.handle ||
            guest.read32(value.fighter + 1816) != value.playerIndex || guest.read32(value.fighter + 1820) != user)
            return std::nullopt;
        return sample;
    } catch (const NativeFailure&) {
        return std::nullopt;
    }
}
}
