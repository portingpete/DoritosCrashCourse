#include "runtime/native/game_observation.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
using namespace Doritos::Native;
namespace {
void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
void position(Memory& guest, uint32_t fighter, float x, float y, float z) {
    for (const auto [offset, value] : std::array<std::pair<uint32_t, float>, 3>{{{136, x}, {140, y}, {144, z}}}) {
        uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); guest.write32(fighter + offset, bits);
    }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        Memory guest(loadOriginalXex(argv[1])); memory = &guest;
        // Independent original byte oracles, not copied from helper behavior.
        const std::array<std::pair<uint32_t, uint32_t>, 20> words{{
            {0x8213c9cc, 0x821b02a0}, {0x8214582c, 0x821f6958}, {0x8214d6e8, 0x82231718},
            {0x821b6c28, 0x8169a900}, {0x821b6c34, 0x3863d334}, {0x821b6c3c, 0x4808a64d},
            {0x8224128c, 0xa161001c}, {0x82241290, 0x7d6b0734}, {0x8224129c, 0x2f0b1770},
            {0x822412c0, 0x806b0004}, {0x822412cc, 0xe9630010}, {0x822412d0, 0x7f245800},
            {0x822054c8, 0x90830718}, {0x822054fc, 0x816b0014}, {0x82205508, 0x816b0004},
            {0x82205514, 0x917f071c}, {0x821b5a14, 0x8170caa4}, {0x821b5a24, 0x90780014},
            {0x821b55a8, 0xe97f0010}, {0x821b55b0, 0xf9780000}
        }};
        for (const auto [address, expected] : words)
            require(guest.read32(address) == expected, "original Game/registry/profile/spawn word oracle");
        require(guest.read32(0x821a6964) == 0xc01f0088 && guest.read32(0x821a696c) == 0xc1bf008c &&
            guest.read32(0x821a6974) == 0xc19f0090, "original marker XYZ offsets");

        // Private ownership fixtures: no actual game, input or movement claim.
        const uint32_t game = guest.allocate(4096), scene = guest.allocate(0x26000);
        const uint32_t profiles = guest.allocate(4096), profile = guest.allocate(4096);
        const uint32_t other = guest.allocate(4096), fighter = guest.allocate(4096);
        constexpr uint32_t user = 1, index = 1, slot = 47;
        constexpr uint64_t handle = 0x12345678002f9abcULL;
        const uint32_t record = game + 368 + index * 56, entry = scene + 0xd334 + slot * 16 + 4;
        guest.write32(0x82b5caa0, game); guest.write32(0x82baa900, scene); guest.write32(0x82b5caa4, profiles);
        guest.write32(game, 0x8213c9cc); guest.write32(game + 364, 2);
        guest.write32(game + 368 + 20, other); guest.write32(profiles + user * 4, profile);
        guest.write32(profile, 0x8214d6e8); guest.write32(profile + 4, user);
        guest.write64(record, handle); guest.write32(record + 20, profile); guest.write32(entry, fighter);
        guest.write32(fighter, 0x8214582c); guest.write64(fighter + 16, handle);
        guest.write32(fighter + 1816, index); guest.write32(fighter + 1820, user);
        position(guest, fighter, -12.25f, 3.5f, 0.0f);
        auto query = [&] { return observeOriginalPlayerPosition(guest, user); };
        const auto original = query();
        require(original && original->game == game && original->scene == scene && original->fighter == fighter &&
            original->profile == profile && original->playerIndex == index && original->user == user &&
            original->handle == handle && original->x == -12.25f && original->y == 3.5f && original->z == 0.0f,
            "exact real ownership associations and big-endian finite position decode");

        // Snapshot every reachable fixture, plus all three globals. The reader
        // cannot alter guest state, allocate handles or make platform calls.
        const std::array<std::pair<uint32_t, uint32_t>, 9> ranges{{{game,4096},{scene,0x26000},{profiles,4096},
            {profile,4096},{other,4096},{fighter,4096},{0x82b5caa0,8},{0x82baa900,4},{0x82b5caa4,4}}};
        std::vector<std::vector<uint8_t>> before;
        for (const auto [address, bytes] : ranges) before.emplace_back(guest.base()+address, guest.base()+address+bytes);
        const auto reservations = guest.reservationSnapshot();
        SetLastError(12345); for (int i=0; i<8; ++i) require(query().has_value(), "repeat read remains available");
        require(GetLastError() == 12345, "observation leaves host error unchanged");
        for (size_t i=0; i<ranges.size(); ++i)
            require(!std::memcmp(before[i].data(), guest.base()+ranges[i].first, before[i].size()), "all guest bytes preserved");
        require(guest.reservationSnapshot() == reservations, "read-only observation allocates no guest storage");

        auto reject32 = [&](uint32_t address, uint32_t value, const char* detail) {
            const uint32_t saved = guest.read32(address); guest.write32(address,value);
            require(!query(),detail); guest.write32(address,saved);
        };
        reject32(game, 0x8213bb34, "unrelated Game class excluded");
        for (uint32_t count : {0u,5u,0xffffffffu}) reject32(game+364,count,"invalid original player count excluded");
        reject32(fighter, 0x8215233c, "authored ACTOR/AvatarPose is not a Fighter");
        reject32(fighter+1816,0,"wrong compact player index excluded");
        reject32(fighter+1820,0,"wrong profile user excluded");
        reject32(record+20,other,"remote or unrelated profile excluded");
        reject32(profile,0x82163924,"base platform profile is not title profile");
        reject32(profile+4,0,"profile user identity mismatch excluded");
        reject32(profiles+user*4,other,"stale profile array association excluded");
        reject32(entry,0,"empty scene registry entry excluded");
        reject32(0x82b5caa0,0,"absent Game yields no observation");
        reject32(0x82baa900,0xfffff000,"registry addition cannot wrap into low memory");
        reject32(fighter+136,0x7fc00001,"NaN XYZ excluded");
        reject32(fighter+140,0x7f800000,"infinite XYZ excluded");
        guest.write64(fighter+16,handle ^ 0x100000000ULL);
        require(!query(),"full high64 handle mismatch excluded despite equal low32"); guest.write64(fighter+16,handle);
        for (uint32_t badSlot : {6000u,0x8000u,0xffffu}) {
            guest.write64(record,(handle & ~0xffff0000ULL) | (uint64_t(badSlot)<<16));
            require(!query(),"out-of-range or signed-negative registry index excluded");
        }
        guest.write64(record,handle);
        guest.write32(game+368+20,profile); require(!query(),"duplicate profile association is ambiguous");
        guest.write32(game+368+20,other);
        require(!observeOriginalPlayerPosition(guest,4) && !observeOriginalPlayerPosition(guest,0xffffffff),
            "unbounded user index excluded");

        const uint64_t lastHandle = (handle & ~0xffff0000ULL) | (uint64_t{5999} << 16);
        guest.write64(record,lastHandle); guest.write64(fighter+16,lastHandle);
        guest.write32(scene+0xd334+5999*16+4,fighter);
        require(query().has_value(),"last real registry slot5999 remains valid");
        guest.write64(record,handle); guest.write64(fighter+16,handle);

        // Final isolated CPU reservation: prove invalidity before invoking the
        // reader, with no later guest allocation to accidentally cover its end.
        const uint32_t terminal = guest.allocate(4096), shortFighter = terminal+4096-1820;
        require(guest.valid(shortFighter,1820) && !guest.valid(shortFighter,1824), "fighter terminal-span fixture really truncated");
        guest.write32(entry,shortFighter); require(!query(),"full fighter identity span required before position access");
        guest.write32(entry,fighter);
        const uint32_t shortGame = terminal+4096-476;
        require(guest.valid(shortGame,476) && !guest.valid(shortGame,480),"two-player record span really truncated");
        guest.write32(shortGame,0x8213c9cc); guest.write32(shortGame+364,2);
        guest.write32(0x82b5caa0,shortGame); require(!query(),"full bounded player-record span required");
        guest.write32(0x82b5caa0,game);
        require(query().has_value(),"excluded cases leave valid original fixture usable");
        std::puts("Original player observation ownership contracts passed; isolated fixtures only.");
        memory = nullptr;
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"Original player observation contract failed: %s\n",error.what()); return 1;
    }
}
