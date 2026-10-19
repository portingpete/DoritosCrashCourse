#pragma once
#include "model.h"

namespace Doritos::Engine {
struct MovementSettings {
    float inertiaTime=1, dragNoInput=0, dragOverRun=0;
    float slope=0, slopeNoInput=0, slopeBoost=0, boostRange=1, boostFactor=0;
    float gravity=-1, airFriction=0, jumpGravityFactor=-1, jumpTimeFactor=.5f;
    float speedChange=.5f, minimumSpeed=1, maximumBoostSpeed=1, easeIn=0, easeOut=0;
};
MovementSettings loadMovementSettings(const std::filesystem::path& path);
struct RunSample {
    float distance=0, fastBlend=0, playbackSpeed=1;
    double phase=0;
    bool active=false;
};
// Ground locomotion only. Turning, boost actions and airborne control have
// separate action rules and must not be inferred from this component.
class GroundRun {
    double phase=0;
    float inertia=0;
    bool active=false;
public:
    void reset();
    float amount() const { return inertia; }
    RunSample advance(const Clip& slow,const Clip& fast,const MovementSettings& settings,
                      float inputMagnitude,float seconds,float forwardSlope=0);
};
}
