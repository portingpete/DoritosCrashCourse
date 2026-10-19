#include "ppc_context.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>

#define DECL(name) void name(PPCContext&,uint8_t*)
DECL(op_addc); DECL(op_addc_record); DECL(op_addc_alias);
DECL(op_addme); DECL(op_addme_record); DECL(op_addme_alias);
DECL(op_subfze); DECL(op_subfze_record); DECL(op_subfze_alias);
DECL(op_vslo); DECL(op_vslo_source_alias); DECL(op_vslo_control_alias); DECL(op_vslo128);
DECL(op_vsubsbs); DECL(op_vsubsbs_alias);
DECL(op_lhbrx); DECL(op_lhbrx_ra0); DECL(op_lvehx); DECL(op_lvehx_ra0);

[[noreturn]] void PPCMemoryFailure(uint32_t,const char*) { std::abort(); }
uint64_t PPCReadMmio(uint32_t,unsigned) { std::abort(); }
void PPCWriteMmio(uint32_t,uint64_t,unsigned) { std::abort(); }

static uint64_t checks=0;
static void check(bool condition,const char* label) {
    ++checks;
    if(!condition) { std::fprintf(stderr,"Semantic oracle failed: %s\n",label); std::abort(); }
}
using Op=void(*)(PPCContext&,uint8_t*);
static void carry_case(unsigned kind,uint64_t a,uint64_t b,uint8_t carry) {
    const uint64_t mask=0xFFFFFFFFull;
    // Independent architecture equations compute a 33-bit low-word sum.
    uint64_t full=kind==0?a+b:kind==1?a+carry-1:~a+carry;
    uint64_t low=kind==0?(a&mask)+(b&mask):kind==1?(a&mask)+mask+carry:(~a&mask)+carry;
    uint8_t next=uint8_t(low>>32);
    const Op ordinary[]={op_addc,op_addme,op_subfze};
    const Op recorded[]={op_addc_record,op_addme_record,op_subfze_record};
    const Op alias[]={op_addc_alias,op_addme_alias,op_subfze_alias};
    for(unsigned variant=0;variant<3;variant++) {
        PPCContext context{};
        context.r4.u64=a; context.r5.u64=b; context.xer.ca=carry; context.xer.so=1;context.xer.ov=1;
        context.cr0={1,1,1,{1}};
        (variant==0?ordinary[kind]:variant==1?recorded[kind]:alias[kind])(context,nullptr);
        check((variant==2?context.r4.u64:context.r3.u64)==full,"carry result and aliased destination");
        check(context.xer.ca==next,"low-word carry out");
        check(context.xer.so==1 && context.xer.ov==1,"non-overflow instruction preserves SO/OV");
        if(variant==1) {
            int32_t signed_low=int32_t(full);
            check(context.cr0.lt==(signed_low<0)&&context.cr0.gt==(signed_low>0)&&context.cr0.eq==(signed_low==0)&&context.cr0.so==1,"CR0 record bits and SO copy");
        } else check(context.cr0.lt==1&&context.cr0.gt==1&&context.cr0.eq==1&&context.cr0.so==1,"non-record instruction preserves CR0");
    }
}
int main() {
    const std::array<uint64_t,10> edge={0,1,0x7FFFFFFF,0x80000000,0xFFFFFFFF,0x100000000ull,0xFFFFFFFF00000000ull,0x8000000000000000ull,~0ull,0x0123456789ABCDEFull};
    for(unsigned kind=0;kind<3;kind++) for(auto a:edge) for(auto b:edge) for(uint8_t c=0;c<2;c++) carry_case(kind,a,b,c);
    std::mt19937_64 random(0x58410A71);
    for(unsigned i=0;i<10000;i++) for(unsigned kind=0;kind<3;kind++) carry_case(kind,random(),random(),uint8_t(random()&1));

    const Op shifts[]={op_vslo,op_vslo_source_alias,op_vslo_control_alias,op_vslo128};
    for(unsigned control=0;control<256;control++) for(unsigned variant=0;variant<4;variant++) {
        PPCContext context{};
        std::array<uint8_t,16> guest{};
        for(unsigned i=0;i<16;i++) { guest[i]=uint8_t(11+i); context.v4.u8[15-i]=guest[i]; context.v5.u8[i]=0xAA; }
        context.v5.u8[0]=uint8_t(control);
        unsigned bytes=(control&0x78)>>3;
        shifts[variant](context,nullptr);
        auto& result=variant==1?context.v4:variant==2?context.v5:context.v3;
        for(unsigned i=0;i<16;i++) check(result.u8[15-i]==(i+bytes<16?guest[i+bytes]:0),"VSLO endian order, count and aliases");
    }
    for(int a=-128;a<=127;a++) for(int b=-128;b<=127;b++) for(unsigned alias=0;alias<2;alias++) {
        PPCContext context{};
        context.vscr=0x10000; // Non-SAT bits must survive.
        for(unsigned i=0;i<16;i++) { context.v4.s8[i]=int8_t(a);context.v5.s8[i]=int8_t(b); }
        (alias?op_vsubsbs_alias:op_vsubsbs)(context,nullptr);
        int difference=a-b;
        int expected=(std::min)(127,(std::max)(-128,difference));
        auto& result=alias?context.v4:context.v3;
        for(unsigned i=0;i<16;i++) check(result.s8[i]==expected,"VSUBSBS signed clipping and aliases");
        check(context.vscr==(0x10000u|(difference<-128||difference>127?1u:0u)),"VSUBSBS sticky SAT and unrelated VSCR bits");
        context.v4={};context.v5={};context.vscr|=1;
        (alias?op_vsubsbs_alias:op_vsubsbs)(context,nullptr);
        check(context.vscr==0x10001,"SAT remains set after nonsaturating subtraction");
    }

    std::vector<uint8_t> memory(0x10040);
    for(unsigned i=0;i<64;i++) memory[0x10000+i]=uint8_t(0x80+i);
    for(unsigned offset=0;offset<16;offset++) {
        PPCContext context{};context.r4.u64=0x10000;context.r5.u64=offset;
        op_lhbrx(context,memory.data());
        check(context.r3.u64==uint64_t(memory[0x10000+offset])+(uint64_t(memory[0x10001+offset])<<8),"LHBRX endian swap and zero extension");
        context.r0.u64=~0ull;context.r5.u64=0x10000+offset;
        op_lhbrx_ra0(context,memory.data());
        check(context.r3.u64==uint64_t(memory[0x10000+offset])+(uint64_t(memory[0x10001+offset])<<8),"LHBRX zero base ignores r0");
        context.r4.u64=0x10000;context.r5.u64=offset;
        op_lvehx(context,memory.data());
        for(unsigned i=0;i<16;i++) check(context.v3.u8[15-i]==memory[0x10000+i],"Xenon LVEHX aligned full-vector implementation");
        context.r5.u64=0x10000+offset;
        op_lvehx_ra0(context,memory.data());
        for(unsigned i=0;i<16;i++) check(context.v3.u8[15-i]==memory[0x10000+i],"LVEHX zero base and endian order");
    }
    std::printf("Carry/vector generated-code semantic oracle passed (%llu checks).\n",(unsigned long long)checks);
}
