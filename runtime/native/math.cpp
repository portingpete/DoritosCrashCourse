#include <bit>
#include <cstdint>
#include <float.h>
#include "math.h"

int DoritosNativeDisableFpuExceptions() {
    unsigned int current = 0;
    // On x64 the UCRT applies these masks to MXCSR. _controlfp_s leaves
    // denormal-operand masking and rounding/flush control unchanged.
    return _controlfp_s(&current, _MCW_EM, _MCW_EM);
}

// New Clang versions lower SIMDe nearest-even vector rounding to this C23
// symbol, which Windows' UCRT does not provide. Compute it from IEEE bits so
// the result is independent of the caller's floating-point rounding mode.
extern "C" float roundevenf(float value) {
    uint32_t bits = std::bit_cast<uint32_t>(value);
    uint32_t sign = bits & 0x80000000u;
    uint32_t magnitude = bits & 0x7fffffffu;
    uint32_t exponent = magnitude >> 23;
    if (exponent == 255) return value + value;
    if (exponent >= 150) return value;
    if (exponent < 126) return std::bit_cast<float>(sign);
    if (exponent == 126) return std::bit_cast<float>(sign | (magnitude > 0x3f000000u ? 0x3f800000u : 0));
    uint32_t unit = 1u << (150 - exponent);
    uint32_t fraction = magnitude & (unit - 1);
    uint32_t integral = magnitude & ~(unit - 1);
    if (fraction > unit / 2 || (fraction == unit / 2 && (integral & unit))) integral += unit;
    return std::bit_cast<float>(sign | integral);
}
