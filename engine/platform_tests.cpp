#include "collision.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Doritos::Engine;
namespace {
void expect(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
void nearVector(XMFLOAT3 actual,XMFLOAT3 expected,const char* message) {
    expect(std::abs(actual.x-expected.x)<.0001f&&std::abs(actual.y-expected.y)<.0001f&&std::abs(actual.z-expected.z)<.0001f,message);
}
template<class Action> void rejects(Action action,const char* message) {
    bool failed=false;try{action();}catch(const std::runtime_error&){failed=true;}expect(failed,message);
}
XMFLOAT4X4 rotation(float radians) {
    float c=std::cos(radians),s=std::sin(radians);
    return {c,s,0,0,-s,c,0,0,0,0,1,0,0,0,0,1};
}
PlatformSurface surface(uint32_t ordinal=0) {
    PlatformSurface value;value.actor="Authored platform";value.ordinal=ordinal;
    value.orientation=rotation(1.5707963267948966f);value.slideVelocity={2,0,0};
    value.friction=.3f;value.slidable=true;value.conveyor=true;return value;
}
Triangle floorTriangle(float x=0,float z=0) { return {{x-2,-2,z},{x+2,-2,z},{x,2,z}}; }
struct Fixture {
    std::vector<uint8_t> data;
    size_t platformCount=0,chunk=0,ordinal=0,secondOrdinal=0,flags=0,friction=0,velocity=0,orientation=0,owner=0;
    template<class T> void add(T value) { auto start=data.size();data.resize(start+sizeof(value));std::memcpy(data.data()+start,&value,sizeof(value)); }
    template<class T> void replace(size_t offset,T value) { expect(offset+sizeof(value)<=data.size(),"Test fixture mutation outside buffer");std::memcpy(data.data()+offset,&value,sizeof(value)); }
    void text(const std::string& value) { add(uint32_t(value.size()));data.insert(data.end(),value.begin(),value.end()); }
    explicit Fixture(uint32_t version=2) {
        add(0x31434e44u);add(version);add(15u);
        for(uint32_t i=0;i<15;++i) {
            add(14u-i);
            if(version>=2) {
                if(i==0)platformCount=data.size();add(2u);
                for(uint32_t j=0;j<2;++j) {
                    text("Authored platform");
                    if(i==0&&j==0)chunk=data.size();add(0u);
                    if(i==0) { if(j==0)ordinal=data.size();else secondOrdinal=data.size(); }add(j);
                    if(i==0&&j==0)flags=data.size();add(j==0?3u:6u);
                    if(i==0&&j==0)friction=data.size();add(.3f);
                    if(i==0&&j==0)velocity=data.size();add(2.f);add(0.f);add(0.f);
                    if(i==0&&j==0)orientation=data.size();
                    auto m=rotation(j==0?1.5707963267948966f:-1.5707963267948966f);
                    for(const auto& row:m.m)for(float value:row)add(value);
                }
            }
            add(2u);
            for(uint32_t j=0;j<2;++j) {
                auto triangle=floorTriangle(0,float(j));
                for(auto point:{triangle.a,triangle.b,triangle.c})for(float value:{point.x,point.y,point.z})add(value);
                if(version>=2) { if(i==0&&j==0)owner=data.size();add(j); }
            }
        }
    }
};
}
int main(int argc,char** argv) {
    auto path=std::filesystem::temp_directory_path()/(L"doritos-platform-contract-"+std::to_wstring(GetCurrentProcessId())+L".dnc");
    if(std::filesystem::exists(path)) { std::cerr<<"Test fixture path already exists\n";return 1; }
    struct Cleanup { std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove(path,error);} } cleanup{path};
    auto load=[&](const Fixture& fixture) {
        { std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char*>(fixture.data.data()),fixture.data.size());expect(bool(file),"Cannot write native platform fixture"); }
        return loadCollision(path);
    };
    auto reject=[&](const Fixture& fixture) { rejects([&]{load(fixture);},"Malformed native platform fixture was accepted"); };
    try {
        auto forward=surface(),reverse=surface(1);reverse.orientation=rotation(-1.5707963267948966f);
        nearVector(surfaceVelocity(forward),{0,2,0},"Native platform local velocity used the wrong rotation");
        nearVector(surfaceVelocity(reverse),{0,-2,0},"Native opposing platform local velocity lost direction");
        forward.slideVelocity={1,2,3};nearVector(surfaceVelocity(forward),{-2,1,3},"Native platform rotation changed vertical velocity");
        auto invalid=surface();invalid.orientation._11=2;
        rejects([&]{surfaceVelocity(invalid);},"Platform surface velocity accepted actor scale");
        invalid=surface();invalid.orientation._41=100;
        rejects([&]{surfaceVelocity(invalid);},"Platform surface velocity accepted translation");
        invalid=surface();invalid.orientation._33=-1;
        rejects([&]{surfaceVelocity(invalid);},"Platform surface velocity accepted a reflection");
        invalid=surface();invalid.orientation._21+=.2f;
        rejects([&]{surfaceVelocity(invalid);},"Platform surface velocity accepted shear");
        auto a=surface(),b=surface(1);b.useForFloor=false;
        CollisionScene layers({floorTriangle(0,0),floorTriangle(0,1)},{a,b},{0,1});
        auto any=layers.ray({0,0,3},{0,0,-1},5),support=layers.ray({0,0,3},{0,0,-1},5,true);
        expect(any&&support&&any->triangle==1&&support->triangle==0,"Native floor-only ray failed platform filtering");
        expect(layers.platformForTriangle(any->triangle)==&layers.platform(1)&&layers.platformCount()==2,"Native ray hit lost its platform owner");
        auto bounds=layers.platformBounds(1);expect(bounds.has_value(),"Native platform lost its triangle-derived bounds");
        nearVector(bounds->first,{-2,-2,1},"Native platform minimum includes another owner");
        nearVector(bounds->second,{2,2,1},"Native platform maximum includes another owner");
        CollisionScene noTriangles({},{a},{});expect(!noTriangles.platformBounds(0),"Empty platform acquired invented bounds");
        rejects([&]{layers.platformBounds(2);},"Native platform bounds accepted an invalid index");
        auto sweep=layers.sweepEllipsoid({0,0,3},{.2f,.2f,.2f},{0,0,-4});
        expect(sweep&&sweep->triangle==1,"Nonfloor platform was removed from physical collision");
        expect(layers.platformForTriangle(2)==nullptr,"Native invalid triangle resolved an owner");
        rejects([&]{layers.platform(2);},"Native invalid platform index was accepted");
        CollisionScene legacy({floorTriangle()});
        expect(legacy.platformCount()==0&&legacy.platformForTriangle(0)==nullptr&&legacy.ray({0,0,2},{0,0,-1},3,true).has_value(),"Legacy collision platform behavior changed");
        std::vector<Triangle> triangles;std::vector<uint32_t> ownership;
        for(uint32_t i=0;i<24;++i) { triangles.push_back(floorTriangle(float((23-i)*5)));ownership.push_back(i%2); }
        CollisionScene reordered(triangles,{a,b},ownership);
        for(uint32_t i=0;i<triangles.size();++i) {
            float x=float((23-i)*5);auto hit=reordered.ray({x,0,3},{0,0,-1},5);
            expect(hit&&hit->triangle==i,"BVH changed an original triangle identity");
            expect(reordered.platformForTriangle(hit->triangle)->ordinal==i%2,"BVH reordered platform ownership separately from geometry");
            expect(reordered.ray({x,0,3},{0,0,-1},5,true).has_value()==(i%2==0),"BVH floor filtering used a reordered platform index");
        }
        rejects([&]{CollisionScene invalidScene({floorTriangle()},{a},{});},"Missing native triangle ownership accepted");
        rejects([&]{CollisionScene invalidScene({floorTriangle()},{a},{1});},"Out-of-range native triangle ownership accepted");
        rejects([&]{CollisionScene invalidScene({floorTriangle()},{a,a},{0});},"Duplicate native platform identity accepted");
        invalid=surface();invalid.friction=-.1f;
        rejects([&]{CollisionScene invalidScene({floorTriangle()},{invalid},{0});},"Negative native platform friction accepted");
        invalid=surface();invalid.slideVelocity.x=std::numeric_limits<float>::infinity();
        rejects([&]{CollisionScene invalidScene({floorTriangle()},{invalid},{0});},"Nonfinite native slide velocity accepted");
        auto courses=load(Fixture{});
        expect(courses.size()==15&&courses.front().id==0&&courses.back().id==14,"Native platform courses were not sorted or complete");
        for(const auto& course:courses) {
            expect(course.scene.platformCount()==2&&course.scene.triangleCount()==2,"Native platform fixture counts changed");
            const auto& p=course.scene.platform(0);
            expect(p.useForFloor&&p.slidable&&!p.conveyor&&p.friction==.3f,"Native platform flags or friction were decoded incorrectly");
            expect(course.scene.platform(1).conveyor&&!course.scene.platform(1).useForFloor,"Native conveyor/nonfloor flags were lost");
            nearVector(surfaceVelocity(p),{0,2,0},"Native platform reader changed the authored rotation");
        }
        auto old=load(Fixture{1});expect(old.size()==15&&old[0].scene.platformCount()==0,"Native collision v1 compatibility failed");
        auto bad=Fixture{};bad.replace(4,3u);reject(bad);
        bad=Fixture{};bad.replace(8,14u);reject(bad);
        bad=Fixture{};bad.replace(12,13u);reject(bad);
        bad=Fixture{};bad.replace(bad.platformCount,16385u);reject(bad);
        bad=Fixture{};bad.replace(bad.flags,8u);reject(bad);
        bad=Fixture{};bad.replace(bad.friction,-.01f);reject(bad);
        bad=Fixture{};bad.replace(bad.friction,std::numeric_limits<float>::infinity());reject(bad);
        bad=Fixture{};bad.replace(bad.velocity,std::numeric_limits<float>::quiet_NaN());reject(bad);
        bad=Fixture{};bad.replace(bad.chunk,4096u);reject(bad);
        bad=Fixture{};bad.replace(bad.ordinal,65536u);reject(bad);
        bad=Fixture{};bad.replace(bad.secondOrdinal,0u);reject(bad);
        bad=Fixture{};bad.replace(bad.owner,2u);reject(bad);
        bad=Fixture{};bad.replace(bad.orientation+15*sizeof(float),0.f);reject(bad);
        bad=Fixture{};bad.replace(bad.orientation+12*sizeof(float),1.f);reject(bad);
        bad=Fixture{};bad.replace(bad.orientation,2.f);reject(bad);
        bad=Fixture{};bad.replace(bad.orientation+10*sizeof(float),-1.f);reject(bad);
        bad=Fixture{};bad.data.pop_back();reject(bad);
        bad=Fixture{};bad.add(0u);reject(bad);
        if(argc==2) {
            auto owned=loadCollision(std::filesystem::path(argv[1])/"static-collision.dnc");size_t platformTotal=0,triangleTotal=0;
            expect(owned.size()==15,"Owned native platform catalog is incomplete");
            for(const auto& course:owned) {
                expect(course.scene.platformCount()>0,"Owned native platform metadata is missing");
                platformTotal+=course.scene.platformCount();triangleTotal+=course.scene.triangleCount();
                for(uint32_t i=0;i<course.scene.triangleCount();++i)expect(course.scene.platformForTriangle(i)!=nullptr,"Owned triangle lacks its authored platform");
            }
            std::cout<<"Owned native platforms="<<platformTotal<<" triangles="<<triangleTotal<<'\n';
        } else expect(argc==1,"Use NativePlatformTests [asset-directory]");
        std::cout<<"Native DNC1 v1/v2 platform identity, flags, rotation, local velocity, floor filtering, stable BVH ownership and malformed-input checks passed\n";
        return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
