#pragma once
#include "model.h"
#include <optional>
#include <utility>

namespace Doritos::Engine {
struct Triangle { XMFLOAT3 a,b,c; };
struct RayHit { float distance=0; XMFLOAT3 position{},normal{}; uint32_t triangle=0; };
struct SweepHit {
    float fraction=1;
    XMFLOAT3 position{},normal{},correction{};
    uint32_t triangle=0;
    bool penetrating=false;
};
struct CollisionMove { XMFLOAT3 position{};std::vector<XMFLOAT3> normals;bool constrained=false; };
struct PlatformSurface {
    std::string actor;
    uint32_t chunk=0,ordinal=0;
    XMFLOAT4X4 orientation{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    XMFLOAT3 slideVelocity{};
    float friction=0;
    bool useForFloor=true,slidable=false,conveyor=false;
};
// Authored slide velocity is local to the platform actor, in native axes.
// Translation, actor scale and resource transforms never affect this vector.
XMFLOAT3 surfaceVelocity(const PlatformSurface& platform);
class CollisionScene {
    struct Node { XMFLOAT3 minimum{},maximum{};uint32_t first=0,count=0,left=0,right=0; };
    std::vector<Triangle> triangles;
    std::vector<PlatformSurface> platforms;
    std::vector<uint32_t> trianglePlatforms;
    std::vector<uint32_t> order;
    std::vector<Node> nodes;
    uint32_t build(uint32_t first,uint32_t count);
public:
    explicit CollisionScene(std::vector<Triangle> source,std::vector<PlatformSurface> surfaces={},std::vector<uint32_t> ownership={});
    std::optional<RayHit> ray(XMFLOAT3 origin,XMFLOAT3 direction,float maximumDistance,bool floorOnly=false) const;
    std::optional<SweepHit> sweepEllipsoid(XMFLOAT3 origin,XMFLOAT3 radii,XMFLOAT3 displacement) const;
    CollisionMove moveEllipsoid(XMFLOAT3 origin,XMFLOAT3 radii,XMFLOAT3 displacement) const;
    size_t triangleCount() const { return triangles.size(); }
    size_t platformCount() const { return platforms.size(); }
    const PlatformSurface& platform(uint32_t index) const;
    const PlatformSurface* platformForTriangle(uint32_t index) const;
    // Invalid platform indices throw; an authored platform with no triangles
    // has no bounds. Values are derived solely from final world triangles.
    std::optional<std::pair<XMFLOAT3,XMFLOAT3>> platformBounds(uint32_t index) const;
};
struct CourseCollision { uint32_t id=0;CollisionScene scene; };
std::vector<CourseCollision> loadCollision(const std::filesystem::path& path);
}
