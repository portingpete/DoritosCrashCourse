#include "runtime/native/runtime.h"
#include "runtime/native/asset_adaptation.h"
#include "runtime/native/filesystem.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
PPC_FUNC(sub_823B1EF8);
using namespace Doritos::Native;
namespace {
void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
bool overridePresent = true;
uint64_t originalResult = 0;
unsigned originalCalls = 0, overrideQueries = 0;
std::string queriedPath;
}
namespace Doritos::Native {
bool filesystemHasAssetOverride(std::string_view path) {
    ++overrideQueries;
    queriedPath = path;
    return overridePresent;
}
}
extern "C" PPC_FUNC(__imp__sub_823B1EF8) {
    ++originalCalls;
    ctx.r3.u64 = originalResult;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        const auto image = loadOriginalXex(argv[1]);
        Memory guest(image); memory = &guest;
        struct OriginalWord { uint32_t address, word; };
        constexpr OriginalWord originalPolicy[] = {
            {0x821643b8, 0x823b2108}, // actual FILELOADERLZMA Open vtable slot
            {0x823b2110, 0x9421fb10}, {0x823b2114, 0x7c7d1b78},
            {0x823b2144, 0x386100b0}, {0x823b215c, 0x482228bd},
            {0x823b2174, 0x890b0000}, {0x823b2180, 0x992b0000},
            {0x823b2198, 0x939d0230}, {0x823b219c, 0x388100b0},
            {0x823b21a0, 0x7fa3eb78}, {0x823b21a4, 0x4bfffd55},
            {0x823b1f10, 0x4bffe469}, {0x823b1f18, 0x2b030000},
            {0x823b1f1c, 0x419a009c}, {0x823b1fb8, 0x38600004},
            {0x823b1fc0, 0x48223b38}, {0x823b21ac, 0x419a0190},
            {0x823b21b0, 0x2f030004}, {0x823b21b4, 0x409a00b8},
            {0x823b220c, 0x4bfff97d}, {0x823b22f4, 0x8bea983b},
            {0x823b22fc, 0x2b1f0000}, {0x823b2300, 0x419a0010},
            {0x823b2304, 0x38600000}, {0x823b231c, 0x4bedd165},
            {0x823b232c, 0x39600001}, {0x823b2330, 0x917d0210},
        };
        for (const auto& original : originalPolicy)
            require(guest.read32(original.address) == original.word,
                "independent original-image packed/absent/loose loader return oracle");
        PPCContext ctx{}; guest.initializeThread(ctx, 0x100000); currentContext = &ctx;
        const uint32_t owner = guest.allocate(4096);
        guest.write32(owner, 0x821643b4);
        guest.write32(owner + 528, 0);
        guest.write32(owner + 560, 0);
        const uint32_t filename = ctx.r1.u32 + 176;
        std::array<uint8_t, 612> ownerBefore{};
        memcpy(ownerBefore.data(), guest.base() + owner, ownerBefore.size());
        auto request = [&](const char* path, uint32_t caller = 0x823b21a8, uint64_t actualOwner = 0,
                           uint64_t callerOwner = 0, uint64_t nameAddress = 0) {
            strcpy(reinterpret_cast<char*>(guest.base() + filename), path);
            ctx.r3.u64 = actualOwner ? actualOwner : owner;
            ctx.r29.u64 = callerOwner ? callerOwner : owner;
            ctx.r4.u64 = nameAddress ? nameAddress : filename;
            ctx.lr = caller; ctx.r5.u64 = 0x12345678; ctx.r31.u64 = 0x23456789;
            const auto stack = ctx.r1.u64;
            sub_823B1EF8(ctx, guest.base());
            require(ctx.r1.u64 == stack && ctx.r4.u64 == (nameAddress ? nameAddress : filename) &&
                ctx.lr == caller && ctx.r5.u64 == 0x12345678 && ctx.r31.u64 == 0x23456789,
                "owned absent return preserves original caller stack and arguments");
            require(!memcmp(ownerBefore.data(), guest.base() + owner, ownerBefore.size()),
                "policy does not mutate actual loader state or ownership");
            return ctx.r3.u64;
        };
        require(request("data/templates/guigroup.wbr") == 4 && originalCalls == 0 && overrideQueries == 1 &&
            queriedPath == "data/templates/guigroup.wbr" && assetOverrideAdaptationCount() == 1,
            "exact owned GUI overlay chooses the original absent/loose fallback contract");
        require(request("data/templates/scripts.wbr") == 4 && originalCalls == 0 && overrideQueries == 2 &&
            assetOverrideAdaptationCount() == 2, "exact owned script overlay uses the same original route");
        overridePresent = false;
        for (uint64_t status : {0u, 1u, 2u, 3u, 4u}) {
            originalResult = status;
            require(request("data/templates/scripts.wbr") == status && assetOverrideAdaptationCount() == 2,
                "missing owned override preserves every actual original return status");
        }
        overridePresent = true; originalResult = 0;
        const auto beforeCalls = originalCalls, beforeQueries = overrideQueries;
        for (const char* path : {"data/templates/actor.wbr", "data/art/_avatar/p_test.csf", "data.pak",
                "Data/templates/guigroup.wbr", "data\\templates\\guigroup.wbr", "data/templates/guigroup.wbr.wzm",
                "data/templates/guigroup.wbr/", "data/templates/../templates/guigroup.wbr", "guigroup.wbr"})
            require(request(path) == 0 && assetOverrideAdaptationCount() == 2,
                "other or noncanonical inputs retain original packed reads");
        require(originalCalls == beforeCalls + 9 && overrideQueries == beforeQueries,
            "nonwhitelisted package inputs do not query native overrides");
        require(request("data/templates/scripts.wbr", 0x823b32ac) == 0 &&
            request("data/templates/scripts.wbr", 0x823b21a8, owner, owner + 4) == 0 &&
            request("data/templates/scripts.wbr", 0x823b21a8, owner, owner, filename + 4) == 0,
            "other package helpers, unowned caller or foreign filename buffer remain original");
        guest.write32(owner, 0x8215f838);
        memcpy(ownerBefore.data(), guest.base() + owner, ownerBefore.size());
        require(request("data/templates/scripts.wbr") == 0 && assetOverrideAdaptationCount() == 2,
            "ordinary FILESYSTEM or another class cannot request LZMA policy adaptation");
        guest.write32(owner, 0x821643b4);
        memcpy(ownerBefore.data(), guest.base() + owner, ownerBefore.size());
        memset(guest.base() + filename, 'a', 512);
        ctx.r3.u64 = owner; ctx.r29.u64 = owner; ctx.r4.u64 = filename; ctx.lr = 0x823b21a8;
        sub_823B1EF8(ctx, guest.base());
        require(ctx.r3.u64 == 0 && overrideQueries == beforeQueries && assetOverrideAdaptationCount() == 2,
            "unterminated normalized filename retains original behavior");
        require(request("data/templates/bonedmodel.wbr") == 4 &&
            queriedPath == "data/templates/bonedmodel.wbr" && assetOverrideAdaptationCount() == 3,
            "owned BoneModel declaration uses the same verified original loose loader");
        overridePresent = false;
        for (uint64_t status : {0u, 1u, 2u, 3u, 4u}) {
            originalResult = status;
            require(request("data/templates/bonedmodel.wbr") == status && assetOverrideAdaptationCount() == 3,
                "absent BoneModel overlay retains every original package return");
        }
        overridePresent = true;
        require(request("data/templates/shader.wbr") == 4 && queriedPath == "data/templates/shader.wbr" && assetOverrideAdaptationCount() == 4,
            "owned texture declarations use original loose loader fallback");
        require(request("data/art/dxshaders/precache_xenon.bsh") == 4 && queriedPath == "data/art/dxshaders/precache_xenon.bsh" && assetOverrideAdaptationCount() == 5,
            "verified source-cache addition uses original cached loader without recompilation");
        overridePresent = false;
        for (uint64_t status : {0u,1u,2u,3u,4u}) {
            originalResult = status;
            require(request("data/templates/shader.wbr") == status && request("data/art/dxshaders/precache_xenon.bsh") == status &&
                assetOverrideAdaptationCount() == 5,"missing shader overrides preserve genuine package statuses");
        }
        overridePresent = true;
        require(request("data/templates/texture.wbr") == 4 && queriedPath == "data/templates/texture.wbr" && assetOverrideAdaptationCount() == 6,
            "owned image declarations use the exact original absent/loose fallback without changing loader state");
        overridePresent = false;
        for (uint64_t status : {0u,1u,2u,3u,4u}) {
            originalResult = status;
            require(request("data/templates/texture.wbr") == status && assetOverrideAdaptationCount() == 6,
                "missing image declarations retain every original package status");
        }
        std::puts("Native owned loose overlay policy and original loader return contracts passed");
        overridePresent=true;
        require(request("data/templates/guilabel.wbr")==4 && queriedPath=="data/templates/guilabel.wbr" && assetOverrideAdaptationCount()==7,
            "native video label follows the exact original absent/loose template route");
        require(request("data/templates/unistrings.wbr")==4 && queriedPath=="data/templates/unistrings.wbr" && assetOverrideAdaptationCount()==8,
            "native video strings follow the exact original absent/loose template route");
        overridePresent=false;originalResult=2;
        require(request("data/templates/guilabel.wbr")==2 && request("data/templates/unistrings.wbr")==2 && assetOverrideAdaptationCount()==8,
            "absent native label/string overlays preserve the original loader status");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
