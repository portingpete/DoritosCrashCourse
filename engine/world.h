#pragma once
#include "model.h"

namespace Doritos::Engine {
struct Material {
    std::string name, texture;
    DirectX::XMFLOAT4 color{1,1,1,1};
    bool lit = true, transparent = false;
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
}
