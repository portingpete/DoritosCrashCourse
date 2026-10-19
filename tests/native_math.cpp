#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <xmmintrin.h>
#include <thread>
#include "runtime/native/math.h"

extern "C" float roundevenf(float);
int main() {
    uint32_t state = 0x1a2b3c4d;
    unsigned checks = 0;
    for (unsigned sample = 0; sample < 65536; ++sample) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        float value = std::bit_cast<float>(state);
        if (!std::isfinite(value)) continue;
        std::fesetround(FE_TONEAREST);
        volatile float input = value;
        uint32_t expected = std::bit_cast<uint32_t>(std::nearbyintf(input));
        for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
            std::fesetround(mode);
            uint32_t actual = std::bit_cast<uint32_t>(roundevenf(input));
            if (actual != expected) {
                fprintf(stderr, "roundevenf mismatch input=%08X expected=%08X actual=%08X mode=%d\n", state, expected, actual, mode);
                return 1;
            }
            ++checks;
        }
    }
    for (float value : {0.5f, -0.5f, 1.5f, -1.5f, 2.5f, -2.5f, 3.5f, -3.5f, 8388607.5f}) {
        std::fesetround(FE_TONEAREST);
        volatile float input = value;
        uint32_t expected = std::bit_cast<uint32_t>(std::nearbyintf(input));
        std::fesetround(FE_UPWARD);
        if (std::bit_cast<uint32_t>(roundevenf(input)) != expected) return 2;
        ++checks;
    }
    std::fesetround(FE_TONEAREST);
    if (!std::isnan(roundevenf(std::nanf(""))) || !std::isinf(roundevenf(INFINITY))) return 3;
    const unsigned original = _mm_getcsr();
    constexpr unsigned ieeeMasks = _MM_MASK_INVALID | _MM_MASK_DIV_ZERO |
        _MM_MASK_OVERFLOW | _MM_MASK_UNDERFLOW | _MM_MASK_INEXACT;
    // Start with each supported trap unmasked without performing arithmetic.
    // Preserve a nondefault rounding/flush/denormal configuration to prove
    // that the imported CRT operation only changes actual IEEE trap masks.
    const unsigned before = ((original & ~(_MM_ROUND_MASK | ieeeMasks | 0x3f)) |
        _MM_ROUND_DOWN | _MM_FLUSH_ZERO_ON) ^ _MM_MASK_DENORM;
    _mm_setcsr(before);
    if (DoritosNativeDisableFpuExceptions()) return 4;
    const unsigned after = _mm_getcsr();
    if ((after & ieeeMasks) != ieeeMasks || (after & ~ieeeMasks) != (before & ~ieeeMasks)) return 5;
    ++checks;
    volatile double zero = 0.0, one = 1.0;
    if (!std::isinf(one / zero) || !std::isnan(zero / zero)) return 6;
    ++checks;
    _mm_setcsr(original);
    unsigned workerAfter = 0;
    std::thread worker([&] {
        const unsigned workerBefore = (_mm_getcsr() & ~(_MM_ROUND_MASK | ieeeMasks | 0x3f)) | _MM_ROUND_UP;
        _mm_setcsr(workerBefore);
        if (DoritosNativeDisableFpuExceptions()) return;
        workerAfter = _mm_getcsr();
    });
    worker.join();
    if ((workerAfter & (ieeeMasks | _MM_ROUND_MASK)) != (ieeeMasks | _MM_ROUND_UP) || _mm_getcsr() != original) return 7;
    ++checks;
    printf("Native nearest-even rounding: %u checks passed\n", checks);
    return 0;
}
