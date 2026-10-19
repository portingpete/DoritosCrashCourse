#include "world.h"
#include "asset_reader.h"
#include <algorithm>

namespace Doritos::Engine {
World loadWorld(const std::filesystem::path& path) {
    AssetReader input(path);World world;
    need(input.read<uint32_t>()==0x31574e44 && input.read<uint32_t>()==1,"Unsupported independent world asset");
    auto materials=input.count(1024);need(materials>0,"Empty native material table");
    for(uint32_t i=0;i<materials;++i) {
        Material m;m.name=input.string();m.texture=input.string();
        if(!m.texture.empty()) {
            std::filesystem::path texture(m.texture);
            need(!texture.is_absolute() && !texture.has_root_name() && m.texture.starts_with("textures/") &&
                m.texture.find("..") == std::string::npos && m.texture.find('\\')==std::string::npos,
                "Native texture path outside asset root");
        }
        m.color={input.number(),input.number(),input.number(),input.number()};
        for(float v:{m.color.x,m.color.y,m.color.z,m.color.w})need(v>=0&&v<=1,"Invalid native material color");
        m.lit=input.count(1)!=0;m.transparent=input.count(1)!=0;world.materials.push_back(std::move(m));
    }
    uint64_t totalVertices=0,totalIndices=0;
    auto meshes=input.count(2048);
    for(uint32_t i=0;i<meshes;++i) {
        StaticMesh m;m.name=input.string();auto vertices=input.count(65536),indices=input.count(393216),surfaces=input.count(256);
        totalVertices+=vertices;totalIndices+=indices;
        need(vertices&&indices&&indices%3==0&&totalVertices<=1000000&&totalIndices<=4000000,"Native world geometry exceeds budget");
        for(uint32_t j=0;j<vertices;++j) { DrawVertex v;v.position=input.vector();v.normal=input.vector();v.uv={input.number(),input.number()};m.vertices.push_back(v); }
        for(uint32_t j=0;j<indices;++j) { auto index=input.read<uint32_t>();need(index<vertices,"Native world index outside geometry");m.indices.push_back(index); }
        for(uint32_t j=0;j<surfaces;++j) {
            Surface s;s.material=input.read<uint32_t>();s.first=input.read<uint32_t>();s.count=input.read<uint32_t>();
            need(s.material<materials&&s.first<=indices&&s.count<=indices-s.first&&s.count%3==0&&s.first%3==0,"Invalid native world surface");
            m.surfaces.push_back(s);
        }
        world.meshes.push_back(std::move(m));
    }
    auto courses=input.count(15);std::array<bool,15> used{};
    for(uint32_t i=0;i<courses;++i) {
        CourseWorld c;c.id=input.count(14);need(!used[c.id],"Duplicate native world course");used[c.id]=true;
        c.spawn=input.vector();c.inspection=input.vector();auto instances=input.count(16384);
        for(uint32_t j=0;j<instances;++j) {
            Instance instance;instance.actor=input.string();instance.mesh=input.read<uint32_t>();need(instance.mesh<meshes,"Native instance has no mesh");
            for(auto& row:instance.transform.m)for(auto& v:row)v=input.number();instance.opacity=input.number();
            need(instance.opacity>=0&&instance.opacity<=1,"Invalid native instance opacity");
            auto& m=instance.transform;
            need(m._14==0&&m._24==0&&m._34==0&&m._44==1,"Native instance matrix is not affine");
            float determinant=DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(DirectX::XMLoadFloat4x4(&m)));
            need(std::abs(determinant)>1e-12f,"Singular native instance transform");
            c.instances.push_back(std::move(instance));
        }
        world.courses.push_back(std::move(c));
    }
    need(courses==15,"Native world requires 15 courses");
    std::sort(world.courses.begin(),world.courses.end(),[](auto& a,auto& b){return a.id<b.id;});
    input.end();return world;
}
}
