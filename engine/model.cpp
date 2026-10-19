#include "model.h"
#include "asset_reader.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>

namespace Doritos::Engine {
using namespace DirectX;
namespace {
XMMATRIX matrix(const Transform& value) {
    // Cal3D stores the inverse quaternion action. Convert once at this data
    // format boundary; the engine itself uses ordinary DirectX row matrices.
    XMVECTOR quaternion = XMQuaternionConjugate(XMLoadFloat4(&value.rotation));
    return XMMatrixRotationQuaternion(quaternion) * XMMatrixTranslation(value.position.x,value.position.y,value.position.z);
}
Transform sample(const Track& track, float time) {
    need(!track.keys.empty(), "Empty native animation track");
    auto right = std::upper_bound(track.keys.begin(),track.keys.end(),time,[](float t,const Key& k){return t < k.time;});
    if (right == track.keys.begin()) return right->transform;
    if (right == track.keys.end()) return track.keys.back().transform;
    const auto& a = *(right-1); const auto& b = *right;
    const float fraction = (time-a.time)/(b.time-a.time);
    Transform result;
    XMStoreFloat3(&result.position, XMVectorLerp(XMLoadFloat3(&a.transform.position),XMLoadFloat3(&b.transform.position),fraction));
    XMStoreFloat4(&result.rotation, XMQuaternionSlerp(XMLoadFloat4(&a.transform.rotation),XMLoadFloat4(&b.transform.rotation),fraction));
    return result;
}
}

Model loadModel(const std::filesystem::path& path) {
    AssetReader input(path); Model model;
    need(input.read<uint32_t>() == 0x31454e44, "Unsupported independent native asset");
    auto version=input.read<uint32_t>();need(version==1||version==2,"Unsupported native character version");
    auto boneCount = input.count(256), meshCount = input.count(4);
    need(boneCount && meshCount, "Native character has no bones or meshes");
    model.name = input.string();
    for (uint32_t i=0;i<boneCount;++i) {
        Bone bone; bone.parent=input.read<int32_t>();
        need(bone.parent >= -1 && bone.parent < int32_t(boneCount) && bone.parent != int32_t(i), "Invalid native bone hierarchy");
        bone.rest=input.transform(); bone.inverseBind=input.transform(); model.bones.push_back(bone);
    }
    uint64_t totalVertices=0,totalIndices=0;
    for (uint32_t i=0;i<meshCount;++i) {
        Mesh mesh; mesh.texture=input.count(3);
        auto vertices=input.count(64000), indices=input.count(192000);
        totalVertices+=vertices; totalIndices+=indices;
        need(vertices && indices && indices%3==0 && totalVertices<=64000 && totalIndices<=192000, "Native character geometry budget exceeded");
        mesh.vertices.resize(vertices); mesh.indices.resize(indices);
        for (auto& vertex:mesh.vertices) {
            vertex.position=input.vector(); vertex.normal=input.vector(); vertex.uv={input.number(),input.number()};
            for (auto& bone:vertex.bones) { bone=input.read<uint32_t>(); need(bone<boneCount,"Native skin index outside skeleton"); }
            float sum=0;
            for (auto& weight:vertex.weights) { weight=input.number(); need(weight>=0 && weight<=1,"Invalid native skin weight"); sum+=weight; }
            need(std::abs(sum-1)<.002f,"Native skin weights are not normalized");
        }
        for (auto& index:mesh.indices) { index=input.read<uint32_t>(); need(index<vertices,"Native triangle index outside mesh"); }
        model.meshes.push_back(std::move(mesh));
    }
    const auto clips=input.count(128);
    for (uint32_t i=0;i<clips;++i) {
        Clip clip; clip.name=input.string(); clip.duration=input.number();
        need(clip.duration>0 && clip.duration<=3600,"Invalid native clip duration");
        auto tracks=input.count(boneCount); std::vector<bool> used(boneCount);
        for (uint32_t j=0;j<tracks;++j) {
            Track track; track.bone=input.read<uint32_t>();
            need(track.bone<boneCount && !used[track.bone],"Invalid native animation track bone"); used[track.bone]=true;
            auto count=input.count(100000); need(count>0,"Empty native animation track");
            for (uint32_t k=0;k<count;++k) {
                Key key; key.time=input.number(); key.transform=input.transform();
                need(key.time>=0 && key.time<=3600 && (track.keys.empty() || key.time>track.keys.back().time),"Invalid native animation key times");
                track.keys.push_back(key);
            }
            clip.tracks.push_back(std::move(track));
        }
        if(version>=2) {
            auto count=input.count(100000);
            for(uint32_t j=0;j<count;++j) {
                MotionKey key;key.time=input.number();key.position=input.vector();
                need(key.time>=0&&key.time<=clip.duration+.001f&&(clip.motion.empty()||key.time>clip.motion.back().time),"Invalid native root-motion times");
                clip.motion.push_back(key);
            }
            need(clip.motion.empty()||(clip.motion.front().time==0&&std::abs(clip.motion.back().time-clip.duration)<.001f),"Native root motion must cover the clip");
        }
        model.clips.push_back(std::move(clip));
    }
    auto courses=input.count(15); std::array<bool,15> used{};
    for (uint32_t i=0;i<courses;++i) {
        Course course; course.id=input.count(14); course.theme=input.count(2); auto chunks=input.count(256);
        need(!used[course.id],"Duplicate native course identity"); used[course.id]=true;
        course.name=input.string();
        for (uint32_t j=0;j<chunks;++j) { Chunk chunk; chunk.name=input.string(); chunk.actors=input.count(65536); course.chunks.push_back(std::move(chunk)); }
        model.courses.push_back(std::move(course));
    }
    input.end(); (void)samplePose(model,nullptr,0); return model;
}

namespace {
std::vector<Transform> localPose(const Model& model,const Clip* clip,float seconds,bool loop) {
    need(std::isfinite(seconds),"Nonfinite animation clock");
    std::vector<Transform> locals; for (const auto& bone:model.bones) locals.push_back(bone.rest);
    if (clip) {
        need(clip->duration>0,"Invalid animation duration");
        float time=loop?std::fmod(std::max(seconds,0.f),clip->duration):std::clamp(seconds,0.f,clip->duration);
        for (const auto& track:clip->tracks) { need(track.bone<locals.size(),"Invalid sampled bone"); locals[track.bone]=sample(track,time); }
    }
    return locals;
}
std::vector<XMFLOAT4X4> composePose(const Model& model,const std::vector<Transform>& locals) {
    std::vector<XMFLOAT4X4> absolute(model.bones.size()), result(model.bones.size());
    std::vector<uint8_t> visited(model.bones.size());
    std::function<void(size_t)> visit=[&](size_t index) {
        need(visited[index]!=1,"Cyclic native bone hierarchy"); if(visited[index]==2)return;
        visited[index]=1; auto transform=matrix(locals[index]); auto parent=model.bones[index].parent;
        if(parent>=0) { need(size_t(parent)<model.bones.size(),"Bone parent outside skeleton"); visit(parent); transform=transform*XMLoadFloat4x4(&absolute[parent]); }
        XMStoreFloat4x4(&absolute[index],transform);
        XMStoreFloat4x4(&result[index],matrix(model.bones[index].inverseBind)*transform);
        visited[index]=2;
    };
    for(size_t i=0;i<model.bones.size();++i)visit(i); return result;
}
XMFLOAT3 motionPosition(const Clip& clip,float time) {
    auto right=std::upper_bound(clip.motion.begin(),clip.motion.end(),time,[](float t,const MotionKey& k){return t<k.time;});
    if(right==clip.motion.begin())return right->position;
    if(right==clip.motion.end())return clip.motion.back().position;
    auto& a=*(right-1);auto& b=*right;XMFLOAT3 value;
    XMStoreFloat3(&value,XMVectorLerp(XMLoadFloat3(&a.position),XMLoadFloat3(&b.position),(time-a.time)/(b.time-a.time)));return value;
}
}
std::vector<XMFLOAT4X4> samplePose(const Model& model,const Clip* clip,float seconds,bool loop) {
    return composePose(model,localPose(model,clip,seconds,loop));
}
std::vector<XMFLOAT4X4> blendPose(const Model& model,const Clip* first,float firstTime,const Clip* second,float secondTime,float weight,bool loop) {
    need(std::isfinite(weight)&&weight>=0&&weight<=1,"Native pose blend outside range");
    auto a=localPose(model,first,firstTime,loop),b=localPose(model,second,secondTime,loop);
    for(size_t i=0;i<a.size();++i) {
        XMStoreFloat3(&a[i].position,XMVectorLerp(XMLoadFloat3(&a[i].position),XMLoadFloat3(&b[i].position),weight));
        XMStoreFloat4(&a[i].rotation,XMQuaternionSlerp(XMLoadFloat4(&a[i].rotation),XMLoadFloat4(&b[i].rotation),weight));
    }
    return composePose(model,a);
}
XMFLOAT3 rootMotionDelta(const Clip& clip,double firstTime,double secondTime,bool loop) {
    need(std::isfinite(firstTime)&&std::isfinite(secondTime)&&std::abs(firstTime)<=1e9&&std::abs(secondTime)<=1e9&&std::isfinite(clip.duration)&&clip.duration>0,"Invalid native root-motion interval");
    if(clip.motion.empty())return {};
    auto position=[&](double time) {
        double cycles=loop?std::floor(time/clip.duration):0;
        float phase=loop?float(time-cycles*clip.duration):float(std::clamp(time,0.,double(clip.duration)));
        auto p=motionPosition(clip,phase),start=clip.motion.front().position,end=clip.motion.back().position;
        return std::array<double,3>{p.x-start.x+cycles*(end.x-start.x),p.y-start.y+cycles*(end.y-start.y),p.z-start.z+cycles*(end.z-start.z)};
    };
    auto a=position(firstTime),b=position(secondTime);return {float(b[0]-a[0]),float(b[1]-a[1]),float(b[2]-a[2])};
}

std::vector<DrawVertex> skin(const Mesh& mesh,const std::vector<XMFLOAT4X4>& pose) {
    std::vector<DrawVertex> result; result.reserve(mesh.vertices.size());
    for(const auto& vertex:mesh.vertices) {
        XMVECTOR position=XMVectorZero(),normal=XMVectorZero();
        for(size_t i=0;i<4;++i)if(vertex.weights[i]>0) {
            need(vertex.bones[i]<pose.size(),"Skin bone outside sampled pose"); auto transform=XMLoadFloat4x4(&pose[vertex.bones[i]]);
            position=XMVectorAdd(position,XMVectorScale(XMVector3TransformCoord(XMLoadFloat3(&vertex.position),transform),vertex.weights[i]));
            normal=XMVectorAdd(normal,XMVectorScale(XMVector3TransformNormal(XMLoadFloat3(&vertex.normal),transform),vertex.weights[i]));
        }
        DrawVertex output; XMStoreFloat3(&output.position,position); XMStoreFloat3(&output.normal,XMVector3Normalize(normal)); output.uv=vertex.uv; result.push_back(output);
    }
    return result;
}

void Player::tick(const Input& input,float seconds) {
    need(std::isfinite(seconds) && seconds>0 && seconds<=.05f && std::isfinite(input.horizontal),"Invalid native simulation step");
    // Animation-lab floor only. Original obstacle/course physics are separate
    // rewrite work and must not be certified from this diagnostic arena.
    clock+=seconds; velocity.x=std::clamp(input.horizontal,-1.f,1.f)*3.f;
    if(input.jump && grounded) { velocity.z=5.5f; grounded=false; }
    if(!grounded)velocity.z-=14.f*seconds;
    position.x+=velocity.x*seconds; position.z+=velocity.z*seconds;
    if(position.z<=0) { position.z=0; velocity.z=0; grounded=true; }
    std::string next=!grounded?"jump":std::abs(velocity.x)>.01f?"run":"idle";
    if(next!=animation) { animation=next; animationTime=0; } else animationTime+=seconds;
}
}
