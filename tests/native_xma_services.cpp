#include "runtime/native/runtime.h"
#include "runtime/native/memory_services.h"
#include "runtime/native/xma_services.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
using namespace Doritos::Native;
namespace {
void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
struct XmaScope { Memory* owner; ~XmaScope() { shutdownXmaServices(owner); } };
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        const auto image = loadOriginalXex(argv[1]);
        Memory guest(image); memory = &guest;
        initializeMemoryServices();
        XmaScope xma{&guest};
        PPCContext ctx{}; guest.initializeThread(ctx, 0x100000); currentContext = &ctx;
        const uint32_t output = guest.allocate(4096);
        auto call = [&](const char* name, uint32_t argument) {
            ctx.r3.u64 = argument;
            require(xmaServicesImport(ctx, guest.base(), name), "unrecognized native XMA operation");
            return ctx.r3.u32;
        };
        uint64_t poolRegister = 0;
        require(!xmaWriteMmio(0x7fc00000,0,4) && !xmaReadMmio(0x7fc00000,4,poolRegister), "XMA bridge leaves unrelated device boundaries explicit");
        require(xmaWriteMmio(0x7fea1804,0x02000000,4) && xmaWriteMmio(0x7fea1804,0x03000000,4),
            "original generic audio manager can pause and resume a genuine empty native workload");
        require(call("XMACreateContext",0x70000000) == 0xc000000d, "context allocation validates writable guest output");
        require(xmaReadMmio(0x7fea1800,4,poolRegister), "native manager publishes actual owned physical context storage");
        const uint32_t physical = _byteswap_ulong(uint32_t(poolRegister));
        const uint32_t pool = resolvePhysicalAddress(physical,320*64);
        require(pool && guest.valid(pool,320*64), "native pool register resolves to real committed storage");
        std::array<uint32_t,320> allocated{};
        for (uint32_t slot=0;slot<320;++slot) {
            require(call("XMACreateContext",output) == 0, "native context slot allocation");
            allocated[slot]=guest.read32(output);
            require(allocated[slot] == pool + slot*64 && guest.read32(allocated[slot]) == 0, "owned context address is aligned, unique and initialized");
        }
        require(call("XMACreateContext",output) == 0xc0000017 && guest.read32(output) == 0, "real fixed pool exhaustion fails");
        require(call("XMAReleaseContext",pool+1) == 0xc000000d, "interior context release cannot discard its owner");
        const uint32_t context=allocated[17];
        for (uint32_t word=0;word<16;++word) guest.write32(context+word*4,word ? 0x12340000+word : 0x04400000);
        std::array<uint8_t,64> before{}; memcpy(before.data(),guest.base()+context,64);
        require(xmaWriteMmio(0x7fea1940,_byteswap_ulong(1u<<17),4), "empty native context work accepted");
        require(memcmp(before.data(),guest.base()+context,64)==0, "empty work cannot invent PCM or advance any guest context field");
        require(xmaWriteMmio(0x7fea1964,_byteswap_ulong(1u<<31),4) && guest.read32(allocated[319])==0,
            "last native command bank selects the actual final owned context");
        xmaWriteMmio(0x7fea1804,0x02000000,4);
        xmaWriteMmio(0x7fea1940,_byteswap_ulong(1u<<17),4);
        xmaWriteMmio(0x7fea1804,0x03000000,4);
        xmaWriteMmio(0x7fea1a40,_byteswap_ulong(1u<<17),4);
        xmaWriteMmio(0x7fea1a80,_byteswap_ulong(1u<<17),4);
        require(memcmp(before.data(),guest.base()+context,64)==0, "native queued empty work and lock/clear preserve actual guest image");
        guest.write32(context,guest.read32(context)|0x00100000);
        memcpy(before.data(),guest.base()+context,64);
        bool rejected=false;
        try { xmaWriteMmio(0x7fea1940,_byteswap_ulong(1u<<17),4); } catch (const NativeFailure&) { rejected=true; }
        require(rejected && memcmp(before.data(),guest.base()+context,64)==0, "compressed XMA boundary fails before publishing synthetic progress");
        rejected=false;
        try { xmaReadMmio(0x7fea1818,4,poolRegister); } catch (const NativeFailure&) { rejected=true; }
        require(rejected,"unverified hardware current-index query is never fabricated");
        for (uint32_t slot=0;slot<320;++slot) require(call("XMAReleaseContext",allocated[slot])==0,"release actual owned slot");
        require(call("XMAReleaseContext",context)==0xc000000d,"released context cannot be released again");
        rejected=false;
        try { xmaWriteMmio(0x7fea1940,1,4); } catch (const NativeFailure&) { rejected=true; }
        require(rejected,"native command rejects an unowned slot");
        shutdownXmaServices(&guest);
        require(!guest.valid(pool,1) && resolvePhysicalAddress(physical,64)==0,"native context shutdown actually decommits owned physical pool");
        currentContext=nullptr;
        fprintf(stderr,"Native XMA empty-workload, pool ownership, endian command and explicit compressed boundary contracts passed.\n");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr,"Native XMA manager contract: %s\n",error.what()); return 1;
    }
}
