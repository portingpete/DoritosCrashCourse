#pragma once
#include "model.h"

namespace Doritos::Engine {
// Identifies the authored value in scripts.dnl. Reused chunk instances keep the
// same reference; their course/chunk/record IDs remain distinct.
struct SceneScriptReference {
    std::string source,type,actor,field;
    uint32_t ordinal=0,index=0,mode=0,flags=0;
    int32_t catalogIndex=-1; // Text scripts are retained as unsupported references.
};
struct SceneTriggerBox {
    uint32_t resource=0,primitiveType=2,collisionMask=0,collisionGroup=0;
    std::string name;
    XMFLOAT3 minimum{},maximum{}; // Native Z-up local coordinates.
    XMFLOAT4X4 transform{};       // Native row-vector local-to-world matrix.
};
struct SceneRecord {
    uint32_t id=0,chunk=0,ordinal=0;
    std::string source,type,name;
    XMFLOAT3 position{};
    XMFLOAT4X4 transform{};
    bool enabled=true,useTimePerActor=false,sendToToucher=false,supported=true;
    int32_t mode=0,maxCommands=0,commandCounter=0;
    XMFLOAT2 waitTime{};
    std::string exclusion;
    std::vector<std::string> resources;
    std::vector<SceneScriptReference> scripts;
    std::vector<SceneTriggerBox> boxes;
};
struct SceneChunk {
    uint32_t id=0,serializedOrdinal=0;
    std::string name,source;
    XMFLOAT3 offset{},start{},end{};
};
struct CourseScene {
    uint32_t id=0;
    std::string name;
    std::vector<SceneChunk> chunks;
    std::vector<SceneRecord> records;
};
struct Scene { std::vector<CourseScene> courses; };
Scene loadScene(const std::filesystem::path& path);
}
