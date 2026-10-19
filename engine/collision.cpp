#include "collision.h"
#include "asset_reader.h"
#include <algorithm>
#include <numeric>
#include <limits>

namespace Doritos::Engine {
namespace {
XMFLOAT3 sub(XMFLOAT3 a,XMFLOAT3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
float dot(XMFLOAT3 a,XMFLOAT3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
XMFLOAT3 cross(XMFLOAT3 a,XMFLOAT3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
bool finite(XMFLOAT3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
float component(XMFLOAT3 a,int axis) { return axis==0?a.x:axis==1?a.y:a.z; }
}
CollisionScene::CollisionScene(std::vector<Triangle> source):triangles(std::move(source)) {
    need(triangles.size()<=1000000,"Native collision geometry exceeds budget");
    for(auto& t:triangles)need(finite(t.a)&&finite(t.b)&&finite(t.c),"Nonfinite collision geometry");
    order.resize(triangles.size());std::iota(order.begin(),order.end(),0u);
    if(!order.empty())build(0,uint32_t(order.size()));
}
uint32_t CollisionScene::build(uint32_t first,uint32_t count) {
    Node n;n.first=first;n.count=count;float high=std::numeric_limits<float>::max();n.minimum={high,high,high};n.maximum={-high,-high,-high};
    for(uint32_t i=first;i<first+count;++i)for(auto p:{triangles[order[i]].a,triangles[order[i]].b,triangles[order[i]].c}) {
        n.minimum={std::min(n.minimum.x,p.x),std::min(n.minimum.y,p.y),std::min(n.minimum.z,p.z)};
        n.maximum={std::max(n.maximum.x,p.x),std::max(n.maximum.y,p.y),std::max(n.maximum.z,p.z)};
    }
    auto index=uint32_t(nodes.size());nodes.push_back(n);
    if(count>8) {
        auto extent=sub(n.maximum,n.minimum);int axis=extent.y>extent.x?1:0;if(extent.z>component(extent,axis))axis=2;
        auto center=[&](uint32_t i){auto& t=triangles[i];return (component(t.a,axis)+component(t.b,axis)+component(t.c,axis))/3;};
        uint32_t middle=first+count/2;
        std::nth_element(order.begin()+first,order.begin()+middle,order.begin()+first+count,[&](auto a,auto b){return center(a)<center(b);});
        auto left=build(first,middle-first),right=build(middle,first+count-middle);
        nodes[index].left=left;nodes[index].right=right;nodes[index].count=0;
    }
    return index;
}
std::optional<RayHit> CollisionScene::ray(XMFLOAT3 origin,XMFLOAT3 direction,float maximumDistance) const {
    need(finite(origin)&&finite(direction)&&std::isfinite(maximumDistance)&&maximumDistance>0,"Invalid native collision ray");
    float length=std::sqrt(dot(direction,direction));need(std::isfinite(length)&&length>1e-8f,"Native collision ray has invalid direction length");
    direction={direction.x/length,direction.y/length,direction.z/length};
    std::optional<RayHit> result;float closest=maximumDistance;
    auto box=[&](const Node& n) {
        float minimum=0,maximum=closest;
        for(int axis=0;axis<3;++axis) {
            float d=component(direction,axis),o=component(origin,axis),low=component(n.minimum,axis),high=component(n.maximum,axis);
            if(std::abs(d)<1e-8f) { if(o<low||o>high)return false; }
            else { float a=(low-o)/d,b=(high-o)/d;if(a>b)std::swap(a,b);minimum=std::max(minimum,a);maximum=std::min(maximum,b);if(minimum>maximum)return false; }
        }
        return true;
    };
    std::vector<uint32_t> stack;if(!nodes.empty())stack.push_back(0);
    while(!stack.empty()) {
        auto n=nodes[stack.back()];stack.pop_back();if(!box(n))continue;
        if(!n.count) { stack.push_back(n.left);stack.push_back(n.right);continue; }
        for(uint32_t i=n.first;i<n.first+n.count;++i) {
            auto& t=triangles[order[i]];auto ab=sub(t.b,t.a),ac=sub(t.c,t.a),p=cross(direction,ac);float determinant=dot(ab,p);
            if(std::abs(determinant)<1e-8f)continue;
            auto offset=sub(origin,t.a);float u=dot(offset,p)/determinant;if(u<0||u>1)continue;
            auto q=cross(offset,ab);float v=dot(direction,q)/determinant;if(v<0||u+v>1)continue;
            float distance=dot(ac,q)/determinant;if(distance<0||distance>closest)continue;
            auto normal=cross(ab,ac);float size=std::sqrt(dot(normal,normal));if(size<1e-12f)continue;
            float sign=dot(normal,direction)>0?-1.f:1.f;
            normal={normal.x/size*sign,normal.y/size*sign,normal.z/size*sign};closest=distance;
            result=RayHit{distance,{origin.x+direction.x*distance,origin.y+direction.y*distance,origin.z+direction.z*distance},normal,order[i]};
        }
    }
    return result;
}
std::vector<CourseCollision> loadCollision(const std::filesystem::path& path) {
    AssetReader input(path);need(input.read<uint32_t>()==0x31434e44&&input.read<uint32_t>()==1,"Unsupported native collision asset");
    auto courses=input.count(15);need(courses==15,"Native collision asset requires 15 courses");std::array<bool,15> used{};std::vector<CourseCollision> result;
    uint64_t total=0;
    for(uint32_t i=0;i<courses;++i) {
        auto id=input.count(14);need(!used[id],"Duplicate collision course");used[id]=true;auto count=input.count(1000000);
        total+=count;need(total<=1000000,"Native collision total exceeds budget");
        std::vector<Triangle> triangles;triangles.reserve(count);
        for(uint32_t j=0;j<count;++j) { Triangle t;t.a=input.vector();t.b=input.vector();t.c=input.vector();triangles.push_back(t); }
        result.push_back({id,CollisionScene(std::move(triangles))});
    }
    std::sort(result.begin(),result.end(),[](auto& a,auto& b){return a.id<b.id;});input.end();return result;
}
}
