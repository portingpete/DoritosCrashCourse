#include "player.h"
#include "asset_reader.h"
#include <algorithm>

namespace Doritos::Engine {
namespace {
XMFLOAT3 add(XMFLOAT3 a,XMFLOAT3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
XMFLOAT3 sub(XMFLOAT3 a,XMFLOAT3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
XMFLOAT3 scale(XMFLOAT3 a,float n) { return {a.x*n,a.y*n,a.z*n}; }
float dot(XMFLOAT3 a,XMFLOAT3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
float steering(float speed,XMFLOAT2 factors,XMFLOAT2 speeds) {
    float t=std::clamp((speed-speeds.x)/(speeds.y-speeds.x),0.f,1.f);
    // The authored pair stores high-speed then low-speed factors.
    return factors.y+(factors.x-factors.y)*t;
}
}
const Clip& CoursePlayer::clip(const char* name) const {
    for(auto& value:model.clips)if(value.name==name)return value;
    throw std::runtime_error("Native course player is missing a movement clip");
}
CoursePlayer::CoursePlayer(const Model& source,const MovementSettings& tuning):model(source),settings(tuning) {
    need(settings.courseParameters,"Reconvert assets for native course-player settings");
    for(auto name:{"idle","run","run_fast","jump","fall","land"})(void)clip(name);
}
void CoursePlayer::reset(XMFLOAT3 spawn,CourseStart start,unsigned country) {
    need(std::isfinite(spawn.x)&&std::isfinite(spawn.y)&&std::isfinite(spawn.z),"Invalid native player spawn");
    need(country<3,"Invalid native player country");theme=country;startup=start==CourseStart::Startup;
    if(startup)(void)clip(std::array<const char*,3>{"startup_usa","startup_europe","startup_japan"}[theme]);
    position=spawn;velocity={};grounded=false;facing=1;clock=0;jumps=landings=contacts=0;
    jumping=false;queuedJump=jumpTime=idleTime=landTime=0;run.reset();running={};
    animation="fall";groundNormal={0,0,1};
}
void CoursePlayer::tick(CourseInput input,float seconds,const CollisionScene& collision) {
    need(std::isfinite(seconds)&&seconds>0&&seconds<=.05f&&std::isfinite(input.horizontal),"Invalid native course-player step");
    float axis=std::clamp(input.horizontal,-1.f,1.f),magnitude=std::abs(axis);
    bool startupAtInput=startup;
    if(startup&&magnitude>.001f)startup=false;
    auto previousVelocity=velocity;
    clock+=seconds;idleTime+=seconds;landTime=std::max(0.f,landTime-seconds);
    queuedJump=std::max(0.f,queuedJump-seconds);
    if(input.jumpPressed)queuedJump=settings.jumpQueue+seconds;
    bool wasGrounded=grounded,startedJump=false;
    // Input is sampled before the startup action changes to movement. A first
    // direction/jump press can queue a jump for the next tick; Space alone
    // never leaves the startup action.
    if(!startupAtInput&&grounded&&queuedJump>0) {
        queuedJump=0;grounded=false;jumping=true;startedJump=true;jumpTime=0;landTime=0;
        velocity.x*=.9f;velocity.z=settings.gravity*settings.jumpGravityFactor*.9f;
        run.reset();running={};++jumps;
    }
    if(grounded) {
        if(magnitude>.001f)facing=axis<0?-1.f:1.f;
        running=run.advance(clip("run"),clip("run_fast"),settings,magnitude,seconds);
        float tangentLength=std::sqrt(groundNormal.x*groundNormal.x+groundNormal.z*groundNormal.z);
        float speed=running.distance/seconds;
        velocity={facing*speed*groundNormal.z/tangentLength,0,-facing*speed*groundNormal.x/tangentLength};
    } else {
        if(jumping&&!startedJump)jumpTime+=seconds;
        if(jumping&&!startedJump&&input.jumpHeld&&velocity.z>0) {
            float duration=clip("jump").duration*(1-settings.jumpTimeFactor);
            if(jumpTime<duration)velocity.z=settings.gravity*settings.jumpGravityFactor*std::clamp(1-jumpTime/duration,0.f,1.f);
        }
        if(magnitude>.001f)velocity.x+=steering(std::abs(velocity.x),settings.jumpFactors,settings.jumpSpeeds)*5*axis*seconds;
        velocity.y=0;
    }
    bool landed=false;float landingSpeed=0;
    auto physics=[&](float step) {
        bool supported=false;
        // The offset sphere handles walls/obstacles; grounding uses the actor's
        // base point. A falling base is snapped after integration, with support
        // acquired at the next physics substep, matching the original order.
        if(velocity.z< -settings.gravity*.16f||(grounded&&running.active)) {
            auto hit=collision.ray({position.x,position.y,position.z+.15f},{0,0,-1},.3f);
            if(hit&&hit->normal.z>.6f&&(grounded||hit->position.z>=position.z-.00001f)) {
                supported=true;position.z=hit->position.z;groundNormal=hit->normal;
                if(!grounded) { landingSpeed=std::abs(velocity.x);velocity={};landed=true; }
                else if(!running.active)velocity.z=0;
            }
        }
        grounded=supported;
        if(!grounded) {
            velocity.z+=settings.gravity*step;
            velocity.x-=velocity.x*settings.airFriction*step;
        }
        auto previous=position;
        auto movement=collision.moveEllipsoid(add(position,settings.feetOffset),settings.feetRadii,scale(velocity,step));
        position=sub(movement.position,settings.feetOffset);contacts+=movement.normals.size();
        for(auto normal:movement.normals) {
            float into=dot(velocity,normal);if(into<0)velocity=sub(velocity,scale(normal,into));
        }
        if(grounded||velocity.z< -settings.gravity*.16f) {
            float top=std::max(previous.z,position.z)+.001f;
            auto hit=collision.ray({position.x,position.y,top},{0,0,-1},std::max(.002f,top-position.z+.001f));
            if(hit&&hit->normal.z>.6f&&(grounded||hit->position.z>=position.z))position.z=hit->position.z;
        }
    };
    float speed=std::max(std::sqrt(dot(velocity,velocity))-settings.gravity*seconds,std::sqrt(dot(previousVelocity,previousVelocity)));
    // Original movement bounds each integration span to half its .3-unit
    // motion tolerance. Action/held-jump evaluation still occurs once per tick.
    float remaining=seconds,step=speed>0?.3f/speed*.499f:seconds;
    for(int i=0;speed*remaining>.15f&&i<50;++i) { physics(step);remaining-=step; }
    if(remaining>0)physics(remaining);
    if(landed) {
        ++landings;jumping=false;landTime=clip("land").duration;jumpTime=0;
        if(magnitude>.001f)run.resume(landingSpeed*settings.landRunFactor);else run.reset();
        running={};
    } else if(!grounded&&wasGrounded&&!startedJump) {
        jumping=false;run.reset();running={};
    }
    if(startup)animation=std::array<const char*,3>{"startup_usa","startup_europe","startup_japan"}[theme];
    else if(!grounded)animation=jumping?"jump":"fall";
    else if(running.active)animation="run";
    else animation=landTime>0?"land":"idle";
    need(std::isfinite(position.x)&&std::isfinite(position.y)&&std::isfinite(position.z),"Native course player became nonfinite");
}
std::vector<XMFLOAT4X4> CoursePlayer::pose() const {
    if(animation=="run")return blendPose(model,&clip("run"),float(running.phase),&clip("run_fast"),float(running.phase),running.fastBlend);
    if(animation=="jump")return samplePose(model,&clip("jump"),jumpTime,false);
    if(animation=="land")return samplePose(model,&clip("land"),clip("land").duration-landTime,false);
    return samplePose(model,&clip(animation.c_str()),idleTime);
}
}
