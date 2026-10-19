#include "world.h"
#include <windows.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Doritos::Engine;
namespace {
struct Fixture {
    std::vector<uint8_t> data;
    template<class T> void add(T value) { auto start=data.size();data.resize(start+sizeof(value));std::memcpy(data.data()+start,&value,sizeof(value)); }
    void text(const std::string& value) { add(uint32_t(value.size()));data.insert(data.end(),value.begin(),value.end()); }
    Fixture(std::string texture="",uint32_t lastIndex=2,bool affine=true) {
        add(0x31574e44u);add(1u);add(1u);text("Independent material");text(texture);
        for(int i=0;i<4;++i)add(1.f);add(1u);add(0u);
        add(1u);text("Independent triangle");add(3u);add(3u);add(1u);
        for(int i=0;i<3;++i)for(float v:{float(i==1),0.f,float(i==2),0.f,1.f,0.f,0.f,0.f})add(v);
        add(0u);add(1u);add(lastIndex);add(0u);add(0u);add(3u);
        add(15u);
        for(uint32_t course=0;course<15;++course) {
            add(course);for(int i=0;i<6;++i)add(0.f);add(1u);text("Native instance");add(0u);
            for(int i=0;i<16;++i)add(i%5==0?1.f:(!affine&&i==3?1.f:0.f));add(1.f);
        }
    }
};
void expect(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
}
int main() {
    auto path=std::filesystem::temp_directory_path()/(L"doritos-world-contract-"+std::to_wstring(GetCurrentProcessId())+L".dnw");
    if(std::filesystem::exists(path)) { std::cerr<<"Test fixture path already exists\n";return 1; }
    struct Cleanup { std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove(path,error);} } cleanup{path};
    auto load=[&](const Fixture& fixture) {
        { std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char*>(fixture.data.data()),fixture.data.size());expect(bool(file),"Cannot write native test fixture"); }
        return loadWorld(path);
    };
    auto reject=[&](const Fixture& fixture) { bool failed=false;try{load(fixture);}catch(const std::runtime_error&){failed=true;}expect(failed,"Malformed world was accepted"); };
    try {
        auto world=load(Fixture{});expect(world.courses.size()==15&&world.meshes[0].indices.size()==3&&world.courses[14].id==14,"Native world fixture differs");
        reject(Fixture{"textures/../outside.png"});reject(Fixture{"C:/outside.png"});reject(Fixture{"",3});reject(Fixture{"",2,false});
        auto truncated=Fixture{};truncated.data.resize(truncated.data.size()-1);reject(truncated);
        auto trailing=Fixture{};trailing.add(0u);reject(trailing);
        auto oversized=Fixture{};
        uint32_t count=0xffffffffu;std::memcpy(oversized.data.data()+8,&count,sizeof(count));reject(oversized);
        std::cout<<"Independent world geometry, course identity, path, affine transform and malformed-input checks passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
