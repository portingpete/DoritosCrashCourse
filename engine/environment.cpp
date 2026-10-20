#include "environment.h"
#include "asset_reader.h"

namespace Doritos::Engine {
namespace {
XMFLOAT3 color(AssetReader& input) {
    auto c=input.vector();
    for(float v:{c.x,c.y,c.z})need(v>=0&&v<=1,"Invalid native environment color");
    return c;
}
float bounded(AssetReader& input,float low,float high,const char* message) {
    auto v=input.number();need(v>=low&&v<=high,message);return v;
}
void matrix(AssetReader& input,XMFLOAT4X4& m) {
    for(auto& row:m.m)for(auto& v:row)v=input.number();
    need(m._14==0&&m._24==0&&m._34==0&&m._44==1,"Native light matrix is not affine");
    float determinant=DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(DirectX::XMLoadFloat4x4(&m)));
    need(std::abs(determinant)>1e-12f,"Singular native light transform");
}
}
const EnvironmentStage& Environment::stageForCourse(uint32_t id) const {
    need(id<courseStages.size()&&courseStages[id]<stages.size(),"Native environment has no course stage");
    return stages[courseStages[id]];
}
EnvironmentView environmentView(const EnvironmentStage& stage,XMFLOAT3 focus) {
    using namespace DirectX;
    const auto& camera=stage.camera;
    for(float v:{focus.x,focus.y,focus.z,camera.lookAt.x,camera.lookAt.y,camera.lookAt.z,camera.distance,camera.pitch,camera.yaw,stage.startFov})
        need(std::isfinite(v),"Nonfinite native camera input");
    need(camera.distance>0&&stage.startFov>=1&&stage.startFov<=179,"Invalid native camera placement");
    float pitch=XMConvertToRadians(camera.pitch),yaw=XMConvertToRadians(camera.yaw);
    need(std::abs(std::sin(pitch))>1e-5f,"Native camera orbit is parallel to its up vector");
    auto target=XMVectorAdd(XMLoadFloat3(&focus),XMLoadFloat3(&camera.lookAt));
    auto offset=XMVectorSet(camera.distance*std::sin(pitch)*std::sin(yaw),camera.distance*std::sin(pitch)*std::cos(yaw),camera.distance*std::cos(pitch),0);
    EnvironmentView result;
    // Swapping source Y/Z changes handedness. RH view/projection preserve
    // source +X as screen-right instead of mirroring the entire course.
    XMStoreFloat4x4(&result.view,XMMatrixLookAtRH(XMVectorAdd(target,offset),target,XMVectorSet(0,0,1,0)));
    result.verticalFov=2*std::atan(.75f*std::tan(XMConvertToRadians(stage.startFov)*.5f));
    return result;
}
Environment loadEnvironment(const std::filesystem::path& path) {
    AssetReader input(path);Environment environment;
    need(input.read<uint32_t>()==0x31564e44&&input.read<uint32_t>()==1,"Unsupported native environment asset");
    auto stages=input.count(3);need(stages==3,"Native environment needs three original stages");
    std::array<bool,3> themes{};
    for(uint32_t i=0;i<stages;++i) {
        EnvironmentStage stage;stage.theme=input.count(2);
        need(!themes[stage.theme],"Duplicate native environment theme");themes[stage.theme]=true;
        stage.source=input.string();
        need(stage.source.starts_with("data/maps/")&&stage.source.ends_with(".wbm")&&
            stage.source.find("..") == std::string::npos&&stage.source.find('\\')==std::string::npos,
            "Invalid native stage source name");
        auto bg=color(input);stage.background={bg.x,bg.y,bg.z,1};
        stage.fogEnabled=input.count(1)!=0;stage.fogColor=color(input);
        stage.fogStart=bounded(input,0,100000,"Invalid native fog start");
        stage.fogEnd=bounded(input,stage.fogStart,100000,"Invalid native fog end");
        stage.startFov=bounded(input,1,179,"Invalid native stage FOV");
        auto& c=stage.camera;c.source=input.string();
        need(!c.source.empty(),"Missing native camera provenance");
        c.distance=bounded(input,.001f,100000,"Invalid native camera distance");c.lookAt=input.vector();
        c.pitch=bounded(input,-360,360,"Invalid native camera pitch");c.yaw=bounded(input,-360,360,"Invalid native camera yaw");
        c.distance2=bounded(input,.001f,100000,"Invalid native secondary camera distance");c.lookAt2=input.vector();
        c.pitch2=bounded(input,-360,360,"Invalid native secondary camera pitch");c.yaw2=bounded(input,-360,360,"Invalid native secondary camera yaw");
        c.startFov=bounded(input,1,179,"Invalid native camera template FOV");
        auto lights=input.count(64);
        for(uint32_t j=0;j<lights;++j) {
            EnvironmentLight light;light.actor=input.string();light.resource=input.string();
            light.type=input.count(3);light.lightMask=input.read<uint32_t>();light.renderPassMask=input.read<uint32_t>();
            light.color=color(input);light.specular=color(input);
            light.diffuseFactor=bounded(input,0,1000,"Invalid native light diffuse factor");
            light.specularFactor=bounded(input,0,1000,"Invalid native light specular factor");
            light.range=bounded(input,.001f,100000,"Invalid native light range");light.castShadows=input.count(1)!=0;
            light.attenuation=input.vector();
            for(float v:{light.attenuation.x,light.attenuation.y,light.attenuation.z})need(v>=0,"Invalid native light attenuation");
            matrix(input,light.transform);stage.lights.push_back(std::move(light));
        }
        environment.stages.push_back(std::move(stage));
    }
    auto courses=input.count(15);need(courses==15,"Native environment needs fifteen original courses");
    std::array<bool,15> used{};
    for(uint32_t i=0;i<courses;++i) {
        auto id=input.count(14),stage=input.count(stages-1);
        need(!used[id],"Duplicate native environment course");used[id]=true;environment.courseStages[id]=stage;
    }
    input.end();return environment;
}
}
