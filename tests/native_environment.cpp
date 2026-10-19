#include "runtime/native/runtime.h"
#include "runtime/native/environment.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
using namespace Doritos::Native;
void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        auto image = loadOriginalXex(argv[1]);
        Memory guest(image); memory = &guest;
        initializeEnvironment(image);
        PPCContext ctx{}; guest.initializeThread(ctx, 0x100000);
        currentContext = &ctx;
        uint32_t data = guest.allocate(4096);
        auto call = [&](const char* name) { require(environmentImport(ctx, guest.base(), name), "unrecognized environment contract"); };
        memcpy(guest.base()+data, "XAM.XEX", 8);
        ctx.r3.u64 = data; ctx.r4.u64 = data+128; call("XexGetModuleHandle");
        require(ctx.r3.u32 == 0, "case insensitive native module lookup");
        uint32_t xam = guest.read32(data+128), ordinal=0, target=0;
        for (const auto& entry : image.imports) if (entry.kind==1 && entry.library=="xam.xex") { ordinal=entry.ordinal;target=entry.address;break; }
        ctx.r3.u64=xam;ctx.r4.u64=ordinal;ctx.r5.u64=data+132;call("XexGetProcedureAddress");
        require(ctx.r3.u32==0 && guest.read32(data+132)==target, "ordinal export must equal original callable thunk");
        ctx.r3.u64=xam;ctx.r4.u64=0xffff;ctx.r5.u64=data+132;call("XexGetProcedureAddress");
        require(ctx.r3.u32==0xc0000263 && guest.read32(data+132)==0, "absent exports must fail and clear output");
        ctx.r3.u64=0;ctx.r4.u64=130;ctx.r5.u64=data+136;call("XamAlloc");
        uint32_t buffer=guest.read32(data+136);
        require(ctx.r3.u32==0 && guest.valid(buffer,130), "XAM allocator output");
        ctx.r3.u64=buffer;call("XamFree");require(!guest.valid(buffer,1), "XAM freed storage inaccessible");
        ctx.r3.u64=3;ctx.r4.u64=9;ctx.r5.u64=data+140;ctx.r6.u64=3;ctx.r7.u64=data+144;call("ExGetXConfigSetting");
        require(ctx.r3.u32==0xc0000023, "short console configuration buffer");
        ctx.r3.u64=3;ctx.r4.u64=9;ctx.r5.u64=data+140;ctx.r6.u64=4;ctx.r7.u64=data+144;call("ExGetXConfigSetting");
        require(ctx.r3.u32==0 && guest.read32(data+140)==1, "English native profile");
        ctx.r3.u64=data+180;call("XGetVideoMode");
        require(guest.read32(data+180)==2560 && guest.read32(data+184)==1440 && guest.read32(data+200)==0x42700000,
                "native requested render mode dimensions and float refresh ABI");
        memcpy(guest.base()+data+256, "abc", 3);
        ctx.r3.u64=data+300;ctx.r4.u64=4;ctx.r5.u64=data+150;ctx.r6.u64=data+256;ctx.r7.u64=3;call("RtlMultiByteToUnicodeN");
        require(ctx.r3.u32==0 && guest.read32(data+150)==4 && guest.read32(data+300)==0x00610062, "Unicode truncation and endian output");
        ctx.r3.u64=data+320;ctx.r4.u64=3;ctx.r5.u64=data+150;ctx.r6.u64=data+300;ctx.r7.u64=4;call("RtlUnicodeToMultiByteN");
        require(ctx.r3.u32==0 && guest.read32(data+150)==2 && memcmp(guest.base()+data+320,"ab",2)==0, "ANSI conversion byte count");
        ctx.r3.u64=data+160;call("XamGetExecutionId");
        uint32_t info=guest.read32(data+160);
        require(ctx.r3.u32==0 && guest.valid(info,24) && guest.read32(info+12)==0x58410a71, "original title execution identity");
        uint16_t fields[]{2026,10,5,12,30,40,123,1};
        for (unsigned i=0;i<8;++i) { auto value=_byteswap_ushort(fields[i]); memcpy(guest.base()+data+400+2*i,&value,2); }
        ctx.r3.u64=data+400;ctx.r4.u64=data+420;call("RtlTimeFieldsToTime");require(ctx.r3.u32==1,"time conversion");
        ctx.r3.u64=data+420;ctx.r4.u64=data+440;call("RtlTimeToTimeFields");
        require(memcmp(guest.base()+data+400,guest.base()+data+440,16)==0,"time round trip");
        currentContext=nullptr;memory=nullptr;
        fprintf(stderr,"Native module, allocation, configuration, string and time contracts passed.\n");return 0;
    } catch (const std::exception& e) { fprintf(stderr,"Environment contract: %s\n",e.what());return 1; }
}
