#pragma once
#include <cstdint>

namespace Doritos::Native {
// Counts owned menu requests routed to the original loose-file fallback. The
// original loader still decides whether opening/reading that file succeeds.
uint64_t assetOverrideAdaptationCount();
}
