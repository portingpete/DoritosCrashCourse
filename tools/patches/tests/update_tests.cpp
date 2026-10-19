#include "ppc_context.template.h"
#include <array>
#include <bit>
#include <stdexcept>
#include <vector>
[[noreturn]] void PPCMemoryFailure(uint32_t,const char*) { throw std::runtime_error("memory"); }
uint64_t PPCReadMmio(uint32_t,unsigned) { throw std::runtime_error("mmio"); }
void PPCWriteMmio(uint32_t,uint64_t,unsigned) { throw std::runtime_error("mmio"); }
[[noreturn]] void PPCRecompFailure(const PPCContext&,uint32_t,const char*) { throw std::runtime_error("semantic"); }
uint64_t PPCQueryTimebase() { return 0; }
struct Fixture { const char* name; unsigned width; bool load,floating,sign,indexed; void(*run)(PPCContext&,uint8_t*); };
#include "translated_updates.inc"
int main() {
    int failures=0,checks=0;
    auto check=[&](bool ok,const char* name) { ++checks; if(!ok) { ++failures; if(failures<20) std::fprintf(stderr,"Failed: %s\n",name); } };
    std::vector<uint8_t> memory(0x20000);
    for(const auto& fixture:fixtures) {
        PPCContext ctx{};ctx.fpscr.loadFromHost();
        ctx.r4.u64=fixture.indexed?0x10000:0x10020;ctx.r5.u64=0x10;
        ctx.r3.u64=0x123456789ABCDEF0ull;ctx.f3.f64=-1.25;
        const uint64_t source=fixture.floating?fixture.width==4?0x3FC00000ull:0x3FF4000000000000ull:0xFEDCBA987654ABCDull;
        for(unsigned i=0;i<fixture.width;++i) memory[0x10010+i]=uint8_t(source>>((fixture.width-i-1)*8));
        fixture.run(ctx,memory.data());
        check(ctx.r4.u64==0x10010,fixture.name);
        if(fixture.load) {
            if(fixture.floating) check(ctx.f3.f64==(fixture.width==4?1.5:1.25),fixture.name);
            else if(fixture.sign) check(ctx.r3.s64==int16_t(0xABCD),fixture.name);
            else { const uint64_t mask=fixture.width==8?UINT64_MAX:(uint64_t(1)<<(fixture.width*8))-1;check(ctx.r3.u64==(source&mask),fixture.name); }
        } else {
            const uint64_t expected=fixture.floating?fixture.width==4?0xBFA00000ull:0xBFF4000000000000ull:0x123456789ABCDEF0ull;
            for(unsigned i=0;i<fixture.width;++i) check(memory[0x10010+i]==uint8_t(expected>>((fixture.width-i-1)*8)),fixture.name);
        }
        // Failed access must not commit the update to the base register.
        ctx.r4.u64=fixture.indexed?0:16;ctx.r5.u64=0;
        const auto before=ctx.r4.u64;
        try {fixture.run(ctx,memory.data());check(false,fixture.name);} catch(const std::runtime_error&) {check(ctx.r4.u64==before,fixture.name);}
    }
    for(int op=0;op<4;++op) for(int bit=0;bit<32;++bit) for(uint64_t count:{0ull,1ull,2ull,0x100000001ull}) for(bool truth:{false,true}) {
        PPCContext ctx{};ctx.ctr.u64=count;
        PPCCRRegister* registers[]={&ctx.cr0,&ctx.cr1,&ctx.cr2,&ctx.cr3,&ctx.cr4,&ctx.cr5,&ctx.cr6,&ctx.cr7};
        uint8_t* fields[]={&registers[bit/4]->lt,&registers[bit/4]->gt,&registers[bit/4]->eq,&registers[bit/4]->so};
        *fields[bit%4]=truth;
        branches[op*32+bit](ctx,memory.data());
        const bool zero=op<2,desiredTruth=(op%2)==1;
        check(ctx.ctr.u64==count-1,"CTR decrement");
        check(bool(ctx.r0.u64)==(((count-1==0)==zero)&&(truth==desiredTruth)),"CR-bit conditional counted branch");
    }
    for(int op=0;op<6;++op) for(uint64_t count:{0ull,1ull,2ull,0x100000001ull}) for(bool truth:{false,true}) {
        PPCContext ctx{};ctx.ctr.u64=count;ctx.cr6.eq=truth;
        tails[op](ctx,memory.data());
        const bool expected=op<4?(((count-1==0)==(op<2))&&(truth==((op%2)==1))):
            ((uint32_t(count-1)==0)==(op==4));
        check(ctx.ctr.u64==count-1,"tail branch decrements CTR once");
        check(bool(ctx.r0.u64)==expected,"counted external branch calls translated tail target and returns");
    }
    std::printf("Emitted update and counted-branch instructions: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
