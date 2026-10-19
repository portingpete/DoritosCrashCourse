#pragma once
#include <cstdint>

namespace Doritos::Native {
// Successful original IsCurrentUserLogged queries backed by the actual
// selected Windows offline account. This does not count Live sign-ins.
uint64_t nativeLocalProfileQueryCount();
}
