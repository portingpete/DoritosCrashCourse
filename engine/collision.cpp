#include "collision.h"
#include "asset_reader.h"
#include <algorithm>
#include <numeric>
#include <limits>

namespace Doritos::Engine {
namespace {
XMFLOAT3 sub(XMFLOAT3 a,XMFLOAT3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
XMFLOAT3 add(XMFLOAT3 a,XMFLOAT3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
XMFLOAT3 scale(XMFLOAT3 a,float s) { return {a.x*s,a.y*s,a.z*s}; }
XMFLOAT3 multiply(XMFLOAT3 a,XMFLOAT3 b) { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
XMFLOAT3 divide(XMFLOAT3 a,XMFLOAT3 b) { return {a.x/b.x,a.y/b.y,a.z/b.z}; }
float dot(XMFLOAT3 a,XMFLOAT3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
XMFLOAT3 cross(XMFLOAT3 a,XMFLOAT3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
bool finite(XMFLOAT3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
float component(XMFLOAT3 a,int axis) { return axis==0?a.x:axis==1?a.y:a.z; }
XMFLOAT3 unit(XMFLOAT3 a) { float n=std::sqrt(dot(a,a));return n>1e-12f?scale(a,1/n):XMFLOAT3{}; }
XMFLOAT3 closestSegment(XMFLOAT3 point,XMFLOAT3 a,XMFLOAT3 b) {
    auto ab=sub(b,a);float length=dot(ab,ab);return length>1e-16f?add(a,scale(ab,std::clamp(dot(sub(point,a),ab)/length,0.f,1.f))):a;
}
XMFLOAT3 closestTriangle(XMFLOAT3 p,const Triangle& t) {
    auto ab=sub(t.b,t.a),ac=sub(t.c,t.a),ap=sub(p,t.a);
    if(dot(cross(ab,ac),cross(ab,ac))<1e-16f) {
        auto a=closestSegment(p,t.a,t.b),b=closestSegment(p,t.b,t.c),c=closestSegment(p,t.c,t.a);
        if(dot(sub(p,b),sub(p,b))<dot(sub(p,a),sub(p,a)))a=b;
        return dot(sub(p,c),sub(p,c))<dot(sub(p,a),sub(p,a))?c:a;
    }
    float d1=dot(ab,ap),d2=dot(ac,ap);if(d1<=0&&d2<=0)return t.a;
    auto bp=sub(p,t.b);float d3=dot(ab,bp),d4=dot(ac,bp);if(d3>=0&&d4<=d3)return t.b;
    float vc=d1*d4-d3*d2;if(vc<=0&&d1>=0&&d3<=0)return add(t.a,scale(ab,d1/(d1-d3)));
    auto cp=sub(p,t.c);float d5=dot(ab,cp),d6=dot(ac,cp);if(d6>=0&&d5<=d6)return t.c;
    float vb=d5*d2-d1*d6;if(vb<=0&&d2>=0&&d6<=0)return add(t.a,scale(ac,d2/(d2-d6)));
    float va=d3*d6-d5*d4;if(va<=0&&(d4-d3)>=0&&(d5-d6)>=0)return add(t.b,scale(sub(t.c,t.b),(d4-d3)/((d4-d3)+(d5-d6))));
    float inverse=1/(va+vb+vc);return add(t.a,add(scale(ab,vb*inverse),scale(ac,vc*inverse)));
}
std::optional<float> quadraticFirst(float a,float b,float c,float maximum) {
    if(a<1e-16f)return std::nullopt;
    double discriminant=double(b)*b-4*double(a)*c;if(discriminant<0)return std::nullopt;
    double t=(-double(b)-std::sqrt(discriminant))/(2*double(a));
    if(t<0||t>maximum)return std::nullopt;return float(t);
}
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
std::optional<SweepHit> CollisionScene::sweepEllipsoid(XMFLOAT3 origin,XMFLOAT3 radii,XMFLOAT3 displacement) const {
    need(finite(origin)&&finite(radii)&&finite(displacement)&&radii.x>1e-5f&&radii.y>1e-5f&&radii.z>1e-5f,
         "Invalid native ellipsoid sweep");
    auto end=add(origin,displacement);
    need(finite(end),"Native ellipsoid sweep endpoint overflow");
    XMFLOAT3 minimum{std::min(origin.x,end.x)-radii.x,std::min(origin.y,end.y)-radii.y,std::min(origin.z,end.z)-radii.z};
    XMFLOAT3 maximum{std::max(origin.x,end.x)+radii.x,std::max(origin.y,end.y)+radii.y,std::max(origin.z,end.z)+radii.z};
    auto velocity=divide(displacement,radii);std::optional<SweepHit> result;float first=1,deepest=0;
    std::vector<uint32_t> stack;if(!nodes.empty())stack.push_back(0);
    auto contact=[&](float time,XMFLOAT3 normal,uint32_t index) {
        if(time<0||time>first||dot(velocity,normal)>=-1e-7f||deepest>0)return;
        first=time;result=SweepHit{time,add(origin,scale(displacement,time)),unit(divide(normal,radii)),{},index,false};
    };
    while(!stack.empty()) {
        auto n=nodes[stack.back()];stack.pop_back();
        if(n.maximum.x<minimum.x||n.minimum.x>maximum.x||n.maximum.y<minimum.y||n.minimum.y>maximum.y||n.maximum.z<minimum.z||n.minimum.z>maximum.z)continue;
        if(!n.count) { stack.push_back(n.left);stack.push_back(n.right);continue; }
        for(uint32_t i=n.first;i<n.first+n.count;++i) {
            uint32_t index=order[i];auto& source=triangles[index];
            Triangle t{divide(sub(source.a,origin),radii),divide(sub(source.b,origin),radii),divide(sub(source.c,origin),radii)};
            auto closest=closestTriangle({},t);float distance=std::sqrt(dot(closest,closest));
            auto face=unit(cross(sub(t.b,t.a),sub(t.c,t.a)));
            if(distance<1-1e-5f) {
                float depth=1-distance;
                if(depth>deepest) {
                    auto normal=distance>1e-6f?scale(closest,-1/distance):face;
                    if(distance<=1e-6f&&dot(normal,velocity)>0)normal=scale(normal,-1);
                    deepest=depth;auto correction=multiply(scale(normal,depth+1e-4f),radii);
                    result=SweepHit{0,origin,unit(divide(normal,radii)),correction,index,true};
                }
                continue;
            }
            if(deepest>0)continue;
            float denominator=dot(face,velocity),plane=dot(face,t.a);
            if(std::abs(denominator)>1e-8f)for(float side:{-1.f,1.f}) {
                float time=(plane+side)/denominator;if(time<0||time>first)continue;
                auto normal=scale(face,side),point=sub(scale(velocity,time),normal),projected=closestTriangle(point,t);
                if(dot(sub(point,projected),sub(point,projected))<1e-8f)contact(time,normal,index);
            }
            for(auto vertex:{t.a,t.b,t.c}) {
                auto time=quadraticFirst(dot(velocity,velocity),-2*dot(velocity,vertex),dot(vertex,vertex)-1,first);
                if(time)contact(*time,unit(sub(scale(velocity,*time),vertex)),index);
            }
            for(auto edge:std::array<std::pair<XMFLOAT3,XMFLOAT3>,3>{{{t.a,t.b},{t.b,t.c},{t.c,t.a}}}) {
                auto e=sub(edge.second,edge.first);float length=dot(e,e);if(length<1e-16f)continue;
                auto w=scale(edge.first,-1),a=sub(velocity,scale(e,dot(velocity,e)/length)),b=sub(w,scale(e,dot(w,e)/length));
                auto time=quadraticFirst(dot(a,a),2*dot(a,b),dot(b,b)-1,first);if(!time)continue;
                float along=(dot(w,e)+dot(velocity,e)* *time)/length;if(along<0||along>1)continue;
                auto point=add(edge.first,scale(e,along));contact(*time,unit(sub(scale(velocity,*time),point)),index);
            }
        }
    }
    return result;
}
CollisionMove CollisionScene::moveEllipsoid(XMFLOAT3 origin,XMFLOAT3 radii,XMFLOAT3 displacement) const {
    CollisionMove result;result.position=origin;auto remaining=displacement;
    for(int iteration=0;iteration<12;++iteration) {
        auto hit=sweepEllipsoid(result.position,radii,remaining);
        if(!hit) { result.position=add(result.position,remaining);break; }
        result.constrained=true;result.normals.push_back(hit->normal);
        if(hit->penetrating)result.position=add(result.position,hit->correction);
        else {
            float length=std::sqrt(dot(remaining,remaining));float fraction=std::max(0.f,hit->fraction-(length>0?1e-4f/length:0));
            result.position=add(result.position,scale(remaining,fraction));remaining=scale(remaining,1-fraction);
        }
        for(auto normal:result.normals) { float into=dot(remaining,normal);if(into<0)remaining=sub(remaining,scale(normal,into)); }
        if(dot(remaining,remaining)<1e-12f&&!hit->penetrating)break;
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
