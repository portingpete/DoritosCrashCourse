#pragma once
#include "model.h"
#include <optional>

namespace Doritos::Engine {
struct Triangle { XMFLOAT3 a,b,c; };
struct RayHit { float distance=0; XMFLOAT3 position{},normal{}; uint32_t triangle=0; };
class CollisionScene {
    struct Node { XMFLOAT3 minimum{},maximum{};uint32_t first=0,count=0,left=0,right=0; };
    std::vector<Triangle> triangles;
    std::vector<uint32_t> order;
    std::vector<Node> nodes;
    uint32_t build(uint32_t first,uint32_t count);
public:
    explicit CollisionScene(std::vector<Triangle> source);
    std::optional<RayHit> ray(XMFLOAT3 origin,XMFLOAT3 direction,float maximumDistance) const;
    size_t triangleCount() const { return triangles.size(); }
};
struct CourseCollision { uint32_t id=0;CollisionScene scene; };
std::vector<CourseCollision> loadCollision(const std::filesystem::path& path);
}
