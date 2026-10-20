#pragma once
#include "world.h"

namespace Doritos::Engine {
// Authored GAMECONFIG camera values. Angles remain in source degrees; lookAt
// is converted to the same Z-up basis as World. Orbit semantics are separate.
struct EnvironmentCamera {
    std::string source;
    float distance=0, pitch=0, yaw=0, startFov=0;
    XMFLOAT3 lookAt{};
    float distance2=0, pitch2=0, yaw2=0;
    XMFLOAT3 lookAt2{};
};
struct EnvironmentLight {
    std::string actor, resource;
    uint32_t type=0, lightMask=0, renderPassMask=0;
    XMFLOAT3 color{}, specular{}, attenuation{};
    float diffuseFactor=0, specularFactor=0, range=0;
    bool castShadows=false;
    // Native row-vector transform; no assumed light forward axis is baked in.
    XMFLOAT4X4 transform{};
};
struct EnvironmentStage {
    uint32_t theme=0;
    std::string source;
    XMFLOAT4 background{};
    bool fogEnabled=false;
    XMFLOAT3 fogColor{};
    float fogStart=0, fogEnd=0, startFov=0;
    EnvironmentCamera camera;
    std::vector<EnvironmentLight> lights;
};
struct Environment {
    std::vector<EnvironmentStage> stages;
    std::array<uint32_t,15> courseStages{};
    const EnvironmentStage& stageForCourse(uint32_t id) const;
};
// Geometry is intentionally loaded separately with loadWorld from the adjacent
// environment-world.dnw, so the same GPU/static-mesh renderer can consume it.
Environment loadEnvironment(const std::filesystem::path& path);
struct EnvironmentView { XMFLOAT4X4 view{};float verticalFov=0; };
EnvironmentView environmentView(const EnvironmentStage& stage,XMFLOAT3 focus);
}
