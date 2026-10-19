#include "recompiler.h"
#include <stdexcept>
struct Spec { const char* name; int opcode; int width; bool load, floating, sign, indexed; };
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const Spec specs[] = {
        {"lbzux", PPC_INST_LBZUX,1,true,false,false,true}, {"ldux",PPC_INST_LDUX,8,true,false,false,true},
        {"lfdu",PPC_INST_LFDU,8,true,true,false,false}, {"lfsu",PPC_INST_LFSU,4,true,true,false,false},
        {"lfsux",PPC_INST_LFSUX,4,true,true,false,true}, {"lhau",PPC_INST_LHAU,2,true,false,true,false},
        {"lhzu",PPC_INST_LHZU,2,true,false,false,false}, {"lhzux",PPC_INST_LHZUX,2,true,false,false,true},
        {"lwzux",PPC_INST_LWZUX,4,true,false,false,true}, {"stbux",PPC_INST_STBUX,1,false,false,false,true},
        {"stdux",PPC_INST_STDUX,8,false,false,false,true}, {"stfdu",PPC_INST_STFDU,8,false,true,false,false},
        {"stfsu",PPC_INST_STFSU,4,false,true,false,false}, {"stfsux",PPC_INST_STFSUX,4,false,true,false,true},
        {"sthu",PPC_INST_STHU,2,false,false,false,false}, {"sthux",PPC_INST_STHUX,2,false,false,false,true},
        {"lfdux",PPC_INST_LFDUX,8,true,true,false,true}};
    constexpr size_t fixtureCount = std::size(specs), tailBase = fixtureCount + 128;
    std::ofstream output(argv[1]);
    output << "static void tail_target(PPCContext& ctx,uint8_t*) {ctx.r0.u64=1;}\n";
    auto emit = [&](const char* name, int opcode, int a, int b, int c, size_t index, bool branch) {
        Recompiler compiler;
        if(branch) compiler.instructionAddresses.emplace(0x2000);
        else if(index >= tailBase) compiler.image.symbols.emplace("tail_target",0x2000,4,Symbol_Function);
        powerpc_opcode operation{}; operation.name=name; operation.id=opcode;
        ppc_insn instruction{}; instruction.opcode=&operation;
        instruction.operands[0]=a; instruction.operands[1]=b; instruction.operands[2]=c;
        RecompilerLocalVariables locals{}; CSRState state=CSRState::Unknown;
        auto table=compiler.config.switchTables.end(); uint32_t data[2]{};
        if(!compiler.Recompile(Function(0x1000,4),0x1000,instruction,data,table,locals,state) || compiler.diagnosticCount)
            throw std::runtime_error(name);
        output<<"static void translated_"<<index<<"(PPCContext& ctx,uint8_t* base) {\n"
                  "[[maybe_unused]] PPCRegister temp{}; [[maybe_unused]] uint32_t ea{};\n"<<compiler.out;
        if(branch) output<<"return; loc_2000: ctx.r0.u64=1;\n";
        output<<"}\n";
    };
    for(size_t i=0;i<std::size(specs);++i) {
        const auto& s=specs[i]; emit(s.name,s.opcode,3,s.indexed?4:-16,s.indexed?5:4,i,false);
    }
    const char* branches[]={"bdzf","bdzt","bdnzf","bdnzt"};
    const int opcodes[]={PPC_INST_BDZF,PPC_INST_BDZT,PPC_INST_BDNZF,PPC_INST_BDNZT};
    for(int op=0;op<4;++op) for(int bit=0;bit<32;++bit)
        emit(branches[op],opcodes[op],bit,0x2000,0,fixtureCount+op*32+bit,true);
    for(int op=0;op<4;++op) emit(branches[op],opcodes[op],26,0x2000,0,tailBase+op,false);
    emit("bdz",PPC_INST_BDZ,0x2000,0,0,tailBase+4,false);
    emit("bdnz",PPC_INST_BDNZ,0x2000,0,0,tailBase+5,false);
    output<<"static const Fixture fixtures[]={\n";
    for(size_t i=0;i<std::size(specs);++i) {
        const auto& s=specs[i]; output<<"{\""<<s.name<<"\","<<s.width<<","<<s.load<<","<<s.floating
            <<","<<s.sign<<","<<s.indexed<<",translated_"<<i<<"},\n";
    }
    output<<"};\nstatic void (*const branches[])(PPCContext&,uint8_t*)={\n";
    for(size_t i=fixtureCount;i<tailBase;++i) output<<"translated_"<<i<<",\n";
    output<<"};\nstatic void (*const tails[])(PPCContext&,uint8_t*)={\n";
    for(size_t i=tailBase;i<tailBase+6;++i) output<<"translated_"<<i<<",\n";
    output<<"};\n";
    return output.good()?0:1;
}
