#include "asset_adaptation.h"
#include "filesystem.h"
#include "runtime.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>

extern "C" PPC_FUNC(__imp__sub_823B1EF8);

namespace Doritos::Native {
namespace {
constexpr uint32_t originalOpenCaller = 0x823b21a8;
constexpr uint32_t originalLzmaLoader = 0x821643b4;
std::atomic<uint64_t> overrideRequests{0};

std::string_view ownedLooseRequest(const PPCContext& ctx, uint8_t* base) {
    // Actual FILELOADERLZMA::Open823B2108 stores its owner in r29 and passes
    // its normalized, bounded filename buffer at caller stack+176. Other
    // package helpers and callers keep their original lookup/return contract.
    if (ctx.lr != originalOpenCaller || !memory || !ctx.r3.u32 ||
        ctx.r3.u64 != ctx.r29.u64 || !memory->valid(ctx.r3.u32, 612) ||
        memory->read32(ctx.r3.u32) != originalLzmaLoader)
        return {};
    const uint64_t filename = uint64_t(ctx.r1.u32) + 176;
    if (filename > UINT32_MAX || ctx.r4.u64 != filename ||
        !memory->valid(uint32_t(filename), 512)) return {};
    const char* name = reinterpret_cast<const char*>(base + uint32_t(filename));
    const char* end = static_cast<const char*>(memchr(name, 0, 512));
    if (!end) return {};
    const std::string_view path(name, size_t(end - name));
    if (path != "data/templates/guigroup.wbr" && path != "data/templates/scripts.wbr" &&
        path != "data/templates/guilabel.wbr" && path != "data/templates/unistrings.wbr" &&
        path != "data/templates/bonedmodel.wbr" && path != "data/templates/shader.wbr" &&
        path != "data/templates/texture.wbr" &&
        path != "data/art/dxshaders/precache_xenon.bsh") return {};
    return filesystemHasAssetOverride(path) ? path : std::string_view{};
}
}
uint64_t assetOverrideAdaptationCount() { return overrideRequests.load(std::memory_order_relaxed); }
}

PPC_FUNC(sub_823B1EF8) {
    using namespace Doritos::Native;
    const auto path = ownedLooseRequest(ctx, base);
    if (path.empty()) { __imp__sub_823B1EF8(ctx, base); return; }
    // Original823B1FB8 returns4 when no actual package record exists, without
    // touching the loader. Open subsequently tries .wzm and then its actual
    // base FILELOADER::Open8228F480, including the unchanged load-pause check.
    // Preserve package records, their cached/decompressed bytes and ownership.
    const uint32_t owner = ctx.r3.u32;
    ctx.r3.u64 = 4;
    const uint64_t count = overrideRequests.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 16) {
        std::fprintf(stderr, "[native-assets] Original loose fallback selected for owned override #%llu; loader=%08X path=%.*s\n",
            static_cast<unsigned long long>(count), owner, int(path.size()), path.data());
        std::fflush(stderr);
    }
}
