#pragma once
#include "player.h"
#include "scene.h"
#include "scripts.h"
#include <memory>

namespace Doritos::Engine {
struct SessionIssue {
    uint32_t record=UINT32_MAX;
    uint64_t task=0;
    std::string field,error;
};
struct SessionHudControl {
    std::string name,resource;
    XMFLOAT2 position{};
    float scale=1;
    double createdAt=0;
    uint32_t record=UINT32_MAX;
    uint64_t updates=0;
};
struct SessionTriggerState {
    bool enabled=true,inside=false,supported=true;
    uint64_t contacts=0,commands=0;
    double cooldownUntil=0;
    std::vector<std::string> resources; // Native zero-based reverse authored order.
    std::string error;
};
// A session owns the native script VM and mutable trigger/HUD state. It keeps
// references to immutable authored data, whose lifetime must cover the session.
class CourseSession {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    CourseSession(const CourseScene& scene,const std::vector<ScriptChunk>& scripts,unsigned theme,
                  XMFLOAT3 feetOffset={0,0,.5f},XMFLOAT3 feetRadii={.5f,.5f,.5f});
    ~CourseSession();
    CourseSession(const CourseSession&)=delete;
    CourseSession& operator=(const CourseSession&)=delete;
    void tick(CoursePlayer& player,double seconds,bool timerRunning=true);
    bool requestRespawn();
    const SceneRecord* lastCheckpoint() const;
    double elapsed() const;
    bool finished() const; // Finish-line contact latch; finisher/results remain unsupported.
    bool finisher() const;
    uint64_t respawnCount() const;
    uint64_t checkpointCount() const;
    const std::vector<SessionIssue>& issues() const;
    const std::vector<SessionHudControl>& hud() const;
    const std::vector<SessionTriggerState>& triggers() const;
    std::vector<ScriptTask> tasks() const;
};
}
