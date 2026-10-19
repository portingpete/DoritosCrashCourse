#pragma once
#include <cstdint>

namespace Doritos::Native {
// Counts actual GUI/Fighter instances returned from the title's existing Cal3D
// BoneModel factory. Authored Avatar metadata and SDK state remain untouched.
uint64_t avatarRenderAdaptationCount();
// Counts only guarded Fighter visual-slot-0 selections: gamepad0..3 and the
// original random fallback when its actual Avatar SDK load has failed.
uint64_t avatarFighterAdaptationCount();
}
