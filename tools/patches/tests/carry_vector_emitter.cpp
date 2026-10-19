#include "recompiler.h"
#include <array>
#include <fstream>
#include <stdexcept>

struct Fixture { const char* name; uint32_t word; const char* opcode; };
constexpr uint32_t xo(uint32_t x, uint32_t d=3, uint32_t a=4, uint32_t b=5) {
    return (31u<<26)|(d<<21)|(a<<16)|(b<<11)|(x<<1);
}
constexpr uint32_t vx(uint32_t x, uint32_t d=3, uint32_t a=4, uint32_t b=5, uint32_t op=4) {
    return (op<<26)|(d<<21)|(a<<16)|(b<<11)|x;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ofstream output(argv[1]);
    output << "#include \"ppc_context.h\"\n";
    const Fixture fixtures[] = {
        {"op_addc", xo(10), "addc"}, {"op_addc_record", xo(10)|1, "addc."},
        {"op_addc_alias", xo(10,4), "addc"},
        {"op_addme", xo(234,3,4,0), "addme"}, {"op_addme_record", xo(234,3,4,0)|1, "addme."},
        {"op_addme_alias", xo(234,4,4,0), "addme"},
        {"op_subfze", xo(200,3,4,0), "subfze"}, {"op_subfze_record", xo(200,3,4,0)|1, "subfze."},
        {"op_subfze_alias", xo(200,4,4,0), "subfze"},
        {"op_vslo", vx(1036), "vslo"}, {"op_vslo_source_alias", vx(1036,4), "vslo"},
        {"op_vslo_control_alias", vx(1036,5), "vslo"},
        {"op_vslo128", vx(912,3,4,5,5), "vslo128"},
        {"op_vsubsbs", vx(1792), "vsubsbs"}, {"op_vsubsbs_alias", vx(1792,4), "vsubsbs"},
        {"op_lhbrx", xo(790), "lhbrx"}, {"op_lhbrx_ra0", xo(790,3,0), "lhbrx"},
        {"op_lvehx", xo(39), "lvehx"}, {"op_lvehx_ra0", xo(39,3,0), "lvehx"},
    };
    for (const auto& fixture : fixtures) {
        Recompiler generator;
        std::array<uint8_t,8> bytes{};
        for (unsigned i=0;i<4;i++) bytes[i]=uint8_t(fixture.word >> ((3-i)*8));
        ppc_insn instruction{};
        if (ppc::Disassemble(bytes.data(), 4, 0x1000, instruction) != 4 || !instruction.opcode ||
            std::string(instruction.opcode->name) != fixture.opcode)
            throw std::runtime_error(std::string("Decoder fixture mismatch: ")+fixture.name);
        Function function(0x1000,4);
        auto switch_table = generator.config.switchTables.end();
        RecompilerLocalVariables locals{};
        CSRState csr=CSRState::Unknown;
        if (!generator.Recompile(function,0x1000,instruction,reinterpret_cast<const uint32_t*>(bytes.data()),switch_table,locals,csr) || generator.diagnosticCount)
            throw std::runtime_error(std::string("Emitter rejected fixture: ")+fixture.name);
        output << "void " << fixture.name << "(PPCContext& ctx,uint8_t* base) {\n";
        if (locals.temp) output << "PPCRegister temp{};\n";
        if (locals.vTemp) output << "PPCVRegister vTemp{};\n";
        if (locals.ea) output << "uint32_t ea{};\n";
        output << generator.out << "}\n";
    }
    return output ? 0 : 3;
}
