#pragma once
#include "model.h"

namespace Doritos::Engine {
struct Material {
    std::string name, texture;
    DirectX::XMFLOAT4 color{1,1,1,1};
    bool lit = true, transparent = false;
    XMFLOAT2 uvSpeed{};
};
struct Surface { uint32_t material = 0, first = 0, count = 0; };
struct StaticMesh {
    std::string name;
    std::vector<DrawVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Surface> surfaces;
};
struct Instance { std::string actor; uint32_t mesh=0; XMFLOAT4X4 transform{}; float opacity=1; };
struct CourseWorld { uint32_t id=0; XMFLOAT3 spawn{}, inspection{}; std::vector<Instance> instances; };
struct World {
    std::vector<Material> materials;
    std::vector<StaticMesh> meshes;
    std::vector<CourseWorld> courses;
};
World loadWorld(const std::filesystem::path& path);
// UVEffect0 uses signed remainder and PLUS sampling-coordinate motion. The
// certified path has authored base scale1/offset0; unrelated UV modes differ.
XMFLOAT2 materialUvOffset(const Material& material,double unscaledKernelTime);
// Advances before gameplay pause/time scaling. The renderer owns this clock
// across fighter resets and level changes; only its creation starts at zero.
double advanceEffectClock(double current,double unscaledDelta);
}
