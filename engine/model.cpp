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
    need(input.read<uint32_t>() == 0x31454e44 && input.read<uint32_t>() == 1, "Unsupported independent native asset");
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

std::vector<XMFLOAT4X4> samplePose(const Model& model, const Clip* clip, float seconds) {
    need(std::isfinite(seconds),"Nonfinite animation clock");
    std::vector<Transform> locals; for (const auto& bone:model.bones) locals.push_back(bone.rest);
    if (clip) {
        need(clip->duration>0,"Invalid animation duration");
        float time=std::fmod(std::max(seconds,0.f),clip->duration);
        for (const auto& track:clip->tracks) { need(track.bone<locals.size(),"Invalid sampled bone"); locals[track.bone]=sample(track,time); }
    }
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
