#include "environment.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

using namespace Doritos::Engine;
namespace {
void expect(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
bool closeFloat(float a,float b) { return std::abs(a-b)<.00001f; }
void equal(const XMFLOAT3& actual,const XMFLOAT3& expected,const char* message) {
    expect(closeFloat(actual.x,expected.x)&&closeFloat(actual.y,expected.y)&&closeFloat(actual.z,expected.z),message);
}
// Independent synthetic values exercise row-vector translation and a rotated,
// nonuniformly scaled basis. They contain no original game data.
constexpr float lightMatrix[16]={0,2,0,0,-3,0,0,0,0,0,4,0,11,-7,5,1};
struct Fixture {
    std::vector<uint8_t> data;
    std::map<std::string,size_t> offsets;
    template<class T> void add(T value) {
        auto offset=data.size();data.resize(offset+sizeof(value));
        std::memcpy(data.data()+offset,&value,sizeof(value));
    }
    void mark(const std::string& name) { offsets.emplace(name,data.size()); }
    void text(const std::string& value) {
        add(uint32_t(value.size()));data.insert(data.end(),value.begin(),value.end());
    }
    void vector(float x,float y,float z) { add(x);add(y);add(z); }
    template<class T> void patch(const std::string& name,T value,size_t relative=0) {
        auto offset=offsets.at(name)+relative;
        expect(offset+sizeof(value)<=data.size(),"Test patch exceeds fixture");
        std::memcpy(data.data()+offset,&value,sizeof(value));
    }
    Fixture() {
        add(0x31564e44u);add(1u);mark("stage-count");add(3u);
        // File stage order deliberately differs from both theme and course IDs.
        constexpr uint32_t themes[]={2,0,1};
        for(uint32_t i=0;i<3;++i) {
            auto prefix="stage-"+std::to_string(i)+"/";
            mark(prefix+"theme");add(themes[i]);
            mark(prefix+"source");text("data/maps/synthetic-stage-"+std::to_string(themes[i])+".wbm");
            mark(prefix+"background");vector(.125f*(i+1),.25f,.75f);
            mark(prefix+"fog-enabled");add(uint32_t(i%2));
            vector(.75f,.5f,.25f);mark(prefix+"fog-start");add(10.f+i);
            mark(prefix+"fog-end");add(100.f+i);mark(prefix+"fov");add(65.f+i);
            text("synthetic/camera-"+std::to_string(i));
            mark(prefix+"distance");add(8.f+i);mark(prefix+"look-at");vector(2.5f,-1.25f,.5f+i);
            mark(prefix+"pitch");add(70.f-i);add(195.f+i);
            add(12.f+i);vector(-3.f,2.f,1.5f+i);add(-35.f+i);add(-175.f-i);add(60.f+i);
            mark(prefix+"light-count");add(4u);
            for(uint32_t j=0;j<4;++j) {
                auto lp=prefix+"light-"+std::to_string(j)+"/";
                text("synthetic/light-"+std::to_string(i)+"-"+std::to_string(j));
                text("synthetic/light-template-"+std::to_string(j));
                mark(lp+"type");add(j);add(0x81u+j);add(0x100u+j);
                mark(lp+"color");vector(.25f,.5f,.75f);vector(.875f,.625f,.375f);
                mark(lp+"diffuse");add(1.5f+j);add(.25f+j);
                mark(lp+"range");add(24.f+j);mark(lp+"shadows");add(uint32_t(j%2));
                mark(lp+"attenuation");vector(.5f,.125f,.03125f);
                mark(lp+"matrix");
                for(size_t n=0;n<16;++n)add(lightMatrix[n]+(n==12?float(i*10+j):0.f));
            }
        }
        mark("course-count");add(15u);
        // Reversed rows prove the ID in each row controls its lookup slot.
        for(uint32_t row=0;row<15;++row) {
            auto id=14-row;mark("course-"+std::to_string(row));add(id);add((id*7+3)%3);
        }
    }
};
template<class Action> void rejects(Action action,const char* message) {
    bool failed=false;try { action(); }catch(const std::runtime_error&) { failed=true; }
    expect(failed,message);
}
}
int main() {
    auto path=std::filesystem::temp_directory_path()/(L"doritos-environment-contract-"+
              std::to_wstring(GetCurrentProcessId())+L".dnv");
    if(std::filesystem::exists(path)) { std::cerr<<"Test fixture path already exists\n";return 1; }
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error;std::filesystem::remove(path,error); }
    } cleanup{path};
    auto load=[&](const Fixture& fixture) {
        { std::ofstream file(path,std::ios::binary|std::ios::trunc);
          file.write(reinterpret_cast<const char*>(fixture.data.data()),fixture.data.size());
          expect(bool(file),"Cannot write native environment fixture"); }
        return loadEnvironment(path);
    };
    auto reject=[&](const Fixture& fixture,const char* message) { rejects([&]{load(fixture);},message); };
    try {
        auto environment=load(Fixture{});
        expect(environment.stages.size()==3,"Native environment lost a stage");
        constexpr uint32_t themes[]={2,0,1};
        for(uint32_t i=0;i<3;++i) {
            const auto& stage=environment.stages[i];
            expect(stage.theme==themes[i]&&stage.source=="data/maps/synthetic-stage-"+std::to_string(themes[i])+".wbm",
                   "Native environment changed stage identity/order");
            expect(closeFloat(stage.background.x,.125f*(i+1))&&closeFloat(stage.background.y,.25f)&&
                   closeFloat(stage.background.z,.75f)&&stage.background.w==1,"Native background color changed");
            expect(stage.fogEnabled==bool(i%2)&&closeFloat(stage.fogStart,10.f+i)&&closeFloat(stage.fogEnd,100.f+i)&&
                   closeFloat(stage.startFov,65.f+i),"Native fog/FOV settings changed");
            equal(stage.fogColor,{.75f,.5f,.25f},"Native fog color changed");
            const auto& camera=stage.camera;
            expect(camera.source=="synthetic/camera-"+std::to_string(i)&&closeFloat(camera.distance,8.f+i)&&
                   closeFloat(camera.pitch,70.f-i)&&closeFloat(camera.yaw,195.f+i)&&closeFloat(camera.startFov,60.f+i),
                   "Native primary camera values/provenance changed");
            equal(camera.lookAt,{2.5f,-1.25f,.5f+i},"Native primary camera basis changed");
            expect(closeFloat(camera.distance2,12.f+i)&&closeFloat(camera.pitch2,-35.f+i)&&closeFloat(camera.yaw2,-175.f-i),
                   "Native secondary camera values changed");
            equal(camera.lookAt2,{-3.f,2.f,1.5f+i},"Native secondary camera basis changed");
            expect(stage.lights.size()==4,"Native environment lost a light");
            for(uint32_t j=0;j<4;++j) {
                const auto& light=stage.lights[j];
                expect(light.actor=="synthetic/light-"+std::to_string(i)+"-"+std::to_string(j)&&
                       light.resource=="synthetic/light-template-"+std::to_string(j)&&light.type==j&&
                       light.lightMask==0x81u+j&&light.renderPassMask==0x100u+j,"Native light identity/masks changed");
                equal(light.color,{.25f,.5f,.75f},"Native diffuse color changed");
                equal(light.specular,{.875f,.625f,.375f},"Native specular color changed");
                equal(light.attenuation,{.5f,.125f,.03125f},"Native attenuation changed");
                expect(closeFloat(light.diffuseFactor,1.5f+j)&&closeFloat(light.specularFactor,.25f+j)&&
                       closeFloat(light.range,24.f+j)&&light.castShadows==bool(j%2),"Native light tuning changed");
                for(size_t row=0;row<4;++row)for(size_t col=0;col<4;++col) {
                    auto n=row*4+col;auto expected=lightMatrix[n]+(n==12?float(i*10+j):0.f);
                    expect(closeFloat(light.transform.m[row][col],expected),"Native light matrix was transposed or changed");
                }
            }
        }
        for(uint32_t id=0;id<15;++id) {
            auto index=(id*7+3)%3;
            expect(environment.courseStages[id]==index&& &environment.stageForCourse(id)==&environment.stages[index],
                   "Native course mapping ignored its explicit stage index");
        }
        rejects([&]{environment.stageForCourse(15);},"Out-of-range environment course was accepted");
        auto badLookup=environment;badLookup.courseStages[0]=3;
        rejects([&]{badLookup.stageForCourse(0);},"Invalid in-memory environment stage was accepted");
        auto mutated=[&](const std::string& field,auto value,const char* message,size_t relative=0) {
            Fixture fixture;fixture.patch(field,value,relative);reject(fixture,message);
        };
        mutated("stage-count",2u,"Incomplete stage list was accepted");
        mutated("stage-0/theme",3u,"Out-of-range theme was accepted");
        mutated("stage-1/theme",2u,"Duplicate theme was accepted");
        mutated("stage-0/background",1.01f,"Color above one was accepted");
        mutated("stage-0/fog-enabled",2u,"Non-boolean fog setting was accepted");
        mutated("stage-0/fog-start",-1.f,"Negative fog start was accepted");
        mutated("stage-0/fog-end",9.f,"Reversed fog bounds were accepted");
        mutated("stage-0/fov",180.f,"Invalid stage FOV was accepted");
        mutated("stage-0/distance",0.f,"Zero camera distance was accepted");
        mutated("stage-0/pitch",361.f,"Out-of-range camera angle was accepted");
        mutated("stage-0/light-count",65u,"Excessive light count was accepted");
        mutated("stage-0/light-0/type",4u,"Unknown light type was accepted");
        mutated("stage-0/light-0/range",0.f,"Zero light range was accepted");
        mutated("stage-0/light-0/shadows",2u,"Non-boolean shadow setting was accepted");
        mutated("stage-0/light-0/attenuation",-.01f,"Negative attenuation was accepted");
        mutated("stage-0/light-0/matrix",1.f,"Non-affine light matrix was accepted",3*sizeof(float));
        mutated("stage-0/light-0/matrix",0.f,"Singular light matrix was accepted",10*sizeof(float));
        mutated("course-count",14u,"Incomplete course mapping was accepted");
        mutated("course-0",15u,"Out-of-range course ID was accepted");
        mutated("course-0",3u,"Out-of-range stage index was accepted",sizeof(uint32_t));
        mutated("course-1",14u,"Duplicate course ID was accepted");
        auto nan=std::numeric_limits<float>::quiet_NaN();
        mutated("stage-0/background",nan,"Nonfinite color was accepted");
        mutated("stage-0/look-at",std::numeric_limits<float>::infinity(),"Nonfinite camera vector was accepted");
        mutated("stage-0/light-0/matrix",nan,"Nonfinite light matrix was accepted",12*sizeof(float));
        Fixture source;source.patch("stage-0/source",uint8_t('x'),sizeof(uint32_t));
        reject(source,"Stage source outside data/maps was accepted");
        for(auto size:{size_t(7),Fixture{}.offsets.at("stage-0/source")+6,
                       Fixture{}.offsets.at("stage-0/light-0/matrix")+31,Fixture{}.data.size()-1}) {
            Fixture fixture;fixture.data.resize(size);reject(fixture,"Truncated environment was accepted");
        }
        Fixture trailing;trailing.add(0u);reject(trailing,"Trailing environment bytes were accepted");
        std::cout<<"Independent environment mapping, camera/light preservation and malformed-input checks passed\n";
        return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
