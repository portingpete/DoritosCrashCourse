#pragma once
#include "world.h"
#include "scene.h"
#include "scripts.h"

namespace Doritos::Engine {
struct ActorTimedEvent { std::string animation,command; float time=0; };
struct ActorModel {
    std::string name,path;
    std::vector<uint32_t> materials;
    std::vector<ActorTimedEvent> events;
    Model model;
};
struct ActorBox {
    uint32_t resource=0,bone=0,group=0,mask=0;
    XMFLOAT4X4 local{};
    XMFLOAT3 minimum{},maximum{};
};
struct HammerInstance {
    uint32_t chunk=0,ordinal=0,model=0,animationResource=0;
    std::string name,animation;
    XMFLOAT4X4 actorWorld{},modelLocal{};
    bool loop=false,startMoving=false;
    float wait=0,initialWait=0;
    std::vector<SceneScriptReference> scripts;
    std::vector<ActorBox> boxes;
};
struct CourseActors { uint32_t id=0; std::vector<HammerInstance> hammers; };
struct Actors {
    std::vector<Material> materials;
    std::vector<ActorModel> models;
    std::vector<CourseActors> courses;
};
// DNA1 retains authored model, script, collision and timed-event identity. It
// only describes the supported skeletal HAMMER resource arrangement.
Actors loadActors(const std::filesystem::path& path);
XMFLOAT4X4 actorModelWorld(const HammerInstance& instance);
XMFLOAT4X4 actorBoxWorld(const HammerInstance& instance,const ActorBox& box,
                       const std::vector<XMFLOAT4X4>& absoluteBones);
struct HammerState {
    bool running=false,firstPending=true,playing=false;
    float countdown=0,animationTime=0;
    uint64_t plays=0;
};
class HammerController {
    bool startsMoving=false,loops=false;
    float initial=0,wait=0,duration=0;
    HammerState current;
public:
    HammerController(const HammerInstance& instance,float animationDuration);
    void reset();
    void pause();
    void resume();
    // Returns true when the authored one-shot action starts. Resource time
    // advances after the controller, including while paused or after Reset.
    bool tick(float seconds);
    const HammerState& state() const { return current; }
};
struct ActorNotice { uint32_t actor=0; std::string field,detail; };
class CourseActorSystem {
    struct Impl;
    std::shared_ptr<Impl> impl;
public:
    // Immutable assets must outlive this system and any copies of the system.
    CourseActorSystem(const Actors& assets,uint32_t course);
    void tick(float seconds);
    ScriptObjectPtr lookup(uint32_t chunk,std::string_view name) const;
    std::vector<HammerState> states() const;
    std::vector<XMFLOAT4X4> pose(size_t actor) const;
    std::vector<XMFLOAT4X4> boxTransforms(size_t actor) const;
    const std::vector<ActorNotice>& notices() const;
};
}
