#pragma once
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Doritos::Engine {
using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;
using DirectX::XMFLOAT4X4;
struct Transform { XMFLOAT3 position{}; XMFLOAT4 rotation{0,0,0,1}; };
struct Bone { int32_t parent = -1; Transform rest, inverseBind; };
struct Vertex {
    XMFLOAT3 position{}, normal{};
    XMFLOAT2 uv{};
    std::array<uint32_t,4> bones{};
    std::array<float,4> weights{1,0,0,0};
};
struct DrawVertex { XMFLOAT3 position{}, normal{}; XMFLOAT2 uv{}; };
struct Mesh { uint32_t texture = 0; std::vector<Vertex> vertices; std::vector<uint32_t> indices; };
struct Key { float time = 0; Transform transform; };
struct Track { uint32_t bone = 0; std::vector<Key> keys; };
struct MotionKey { float time=0; XMFLOAT3 position{}; };
struct Clip { std::string name; float duration = 0; std::vector<Track> tracks; std::vector<MotionKey> motion; };
struct Chunk { std::string name; uint32_t actors = 0; };
struct Course { uint32_t id = 0, theme = 0; std::string name; std::vector<Chunk> chunks; };
struct Model {
    std::string name;
    std::vector<Bone> bones;
    std::vector<Mesh> meshes;
    std::vector<Clip> clips;
    std::vector<Course> courses;
};
Model loadModel(const std::filesystem::path& path);
std::vector<XMFLOAT4X4> samplePose(const Model& model, const Clip* clip, float seconds, bool loop=true);
std::vector<XMFLOAT4X4> blendPose(const Model& model,const Clip* first,float firstTime,const Clip* second,float secondTime,float weight,bool loop=true);
XMFLOAT3 rootMotionDelta(const Clip& clip,double firstTime,double secondTime,bool loop=true);
std::vector<DrawVertex> skin(const Mesh& mesh, const std::vector<XMFLOAT4X4>& pose);
struct Input { float horizontal = 0; bool jump = false; };
struct Player {
    XMFLOAT3 position{}, velocity{};
    float clock = 0, animationTime = 0;
    bool grounded = true;
    std::string animation = "idle";
    void tick(const Input& input, float seconds);
};
}
