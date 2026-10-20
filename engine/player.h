#pragma once
#include "locomotion.h"
#include "collision.h"

namespace Doritos::Engine {
struct CourseInput { float horizontal=0;bool jumpPressed=false,jumpHeld=false; };
enum class CourseStart { Startup,Ready };
class CoursePlayer {
    const Model& model;
    MovementSettings settings;
    GroundRun run;
    bool jumping=false;
    bool startup=true;
    bool deferLandingInput=false;
    unsigned theme=0;
    float jumpPressAge=1, jumpTime=0, idleTime=0, landTime=0;
    bool heldJumpLatch=false;
    XMFLOAT3 actionVelocity{};
    XMFLOAT3 groundNormal{0,0,1};
    const Clip& clip(const char* name) const;
public:
    XMFLOAT3 position{},velocity{};
    XMFLOAT3 surfaceVelocity{},externalVelocity{};
    uint32_t supportTriangle=UINT32_MAX;
    bool onConveyor=false;
    bool grounded=false;
    float facing=1;
    double clock=0;
    uint64_t jumps=0,landings=0,contacts=0;
    RunSample running;
    std::string animation="fall";
    explicit CoursePlayer(const Model& model,const MovementSettings& settings);
    void reset(XMFLOAT3 spawn,CourseStart start=CourseStart::Startup,unsigned theme=0);
    bool waitingForDirection() const { return startup; }
    XMFLOAT3 relativeVelocity() const { return actionVelocity; }
    void tick(CourseInput input,float seconds,const CollisionScene& collision);
    std::vector<XMFLOAT4X4> pose() const;
};
}
