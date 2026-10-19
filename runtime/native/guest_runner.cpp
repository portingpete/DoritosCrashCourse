#include "runtime.h"
#include <cstdio>

namespace Doritos::Native {
int runGuest(PPCContext& ctx, uint8_t* base, PPCFunc* entry) {
    __try {
        entry(ctx, base);
        fprintf(stderr, "[STOP] Compiled title entry returned; gameplay has not been verified.\n");
        return 4;
    } __except (exceptionFilter(GetExceptionInformation())) {
        return 3;
    }
}
}
