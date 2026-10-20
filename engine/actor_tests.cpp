#include "actors.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Doritos::Engine;
namespace {
void expect(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
bool almost(float a,float b) { return std::abs(a-b)<.00002f; }
template<class F>void rejects(F action) { bool failed=false;try { action(); }catch(const std::exception&) { failed=true; }expect(failed,"Malformed native actor input accepted"); }
XMFLOAT4X4 identity() { XMFLOAT4X4 m;DirectX::XMStoreFloat4x4(&m,DirectX::XMMatrixIdentity());return m; }
struct Bytes {
    std::vector<uint8_t> data;
    template<class T>size_t add(T value) { auto offset=data.size();auto p=reinterpret_cast<const uint8_t*>(&value);data.insert(data.end(),p,p+sizeof(value));return offset; }
    void string(const std::string& value) { add(uint32_t(value.size()));data.insert(data.end(),value.begin(),value.end()); }
    void vector(XMFLOAT3 v) { add(v.x);add(v.y);add(v.z); }
    void matrix(XMFLOAT4X4 m=identity()) { for(auto& row:m.m)for(float v:row)add(v); }
    void transform(float x=0) { vector({x,0,0});add(0.f);add(0.f);add(0.f);add(1.f); }
    void write(const std::filesystem::path& path) const { std::ofstream stream(path,std::ios::binary|std::ios::trunc);stream.write(reinterpret_cast<const char*>(data.data()),data.size());expect(bool(stream),"Cannot write actor fixture"); }
    void set(size_t offset,uint32_t value) { std::memcpy(data.data()+offset,&value,4); }
};
Bytes modelFixture() {
    Bytes b;b.add(0x31454e44u);b.add(2u);b.add(1u);b.add(1u);b.string("Synthetic attachment");
    b.add(int32_t(-1));b.transform();b.transform();b.add(0u);b.add(3u);b.add(3u);
    for(unsigned i=0;i<3;++i) {
        b.vector({float(i==1),float(i==2),0});b.vector({0,0,1});b.add(0.f);b.add(0.f);
        for(unsigned j=0;j<4;++j)b.add(0u);
        for(unsigned j=0;j<4;++j)b.add(j==0?1.f:0.f);
    }
    b.add(0u);b.add(1u);b.add(2u);b.add(1u);b.string("strike");b.add(1.f);b.add(1u);b.add(0u);b.add(2u);
    b.add(0.f);b.transform();b.add(1.f);b.transform(2);b.add(0u);b.add(0u);return b;
}
struct BundleFixture:Bytes {
    size_t modelIndex=0,bone=0,resource=0,secondId=0,matrixSlot=0,eventTime=0,loop=0;
    BundleFixture() {
        add(0x31414e44u);add(1u);add(1u);string("synthetic");string("");for(unsigned i=0;i<4;++i)add(1.f);add(1u);add(0u);add(0.f);add(0.f);
        add(1u);string("synthetic.cfg");string("actors/test.dne");add(1u);add(0u);add(1u);string("strike");eventTime=add(.13f);string("retained unsupported cue");
        add(15u);
        for(uint32_t i=0;i<15;++i) {
            if(i==1)secondId=data.size();add(i);add(i==0?1u:0u);if(i!=0)continue;
            add(4u);add(10u);string("hammer");modelIndex=add(0u);matrixSlot=data.size();matrix();matrix();string("strike");add(0u);loop=add(1u);add(0u);add(1.25f);add(.01f);
            add(1u);string("OnPlayerHit");add(UINT32_MAX);string("synthetic");string("HAMMERMASTER");string("test");add(0u);add(0u);
            add(1u);resource=add(1u);bone=add(0u);matrix();vector({-.5f,-.5f,-.5f});vector({.5f,.5f,.5f});add(33216u);add(1u);
        }
    }
};
}
int main(int argc,char** argv) {
    const auto root=std::filesystem::temp_directory_path()/(L"doritos-native-actors-"+std::to_wstring(GetCurrentProcessId()));
    if(std::filesystem::exists(root)) { std::cerr<<"Actor fixture already exists\n";return 1; }
    std::filesystem::create_directories(root/L"actors");
    struct Cleanup { std::filesystem::path root;~Cleanup() { std::error_code e;std::filesystem::remove(root/L"actors/test.dne",e);std::filesystem::remove(root/L"actors.dna",e);std::filesystem::remove(root/L"actors",e);std::filesystem::remove(root,e); } } cleanup{root};
    try {
        HammerInstance spec;spec.animation="strike";spec.initialWait=10;spec.wait=.25f;spec.loop=true;
        HammerController controller(spec,1);expect(!controller.state().running&&!controller.state().playing,"Hammer must begin stopped in rest pose");
        controller.resume();expect(controller.tick(.1f)&&almost(controller.state().animationTime,.1f)&&almost(controller.state().countdown,1.25f),"First hammer action incorrectly waited for the initial countdown");
        controller.pause();expect(!controller.tick(.2f)&&almost(controller.state().animationTime,.3f)&&almost(controller.state().countdown,1.25f),"Pause stopped resource time or decremented controller time");
        controller.reset();expect(!controller.state().running&&controller.state().firstPending&&almost(controller.state().animationTime,.3f),"Reset rewound the existing skeletal action");
        controller.tick(.2f);expect(almost(controller.state().animationTime,.5f),"Reset stopped resource animation");
        controller.resume();expect(controller.tick(.1f)&&controller.state().plays==2&&almost(controller.state().animationTime,.1f),"Resume failed to start a fresh action after reset");
        controller.pause();controller.tick(1);controller.tick(1);expect(almost(controller.state().animationTime,1),"Auto-locked skeletal action lost its final pose");
        spec.startMoving=true;spec.loop=false;spec.initialWait=0;HammerController oneShot(spec,1);expect(almost(oneShot.state().countdown,.001f),"Reset did not clamp initial countdown");
        expect(oneShot.tick(.01f),"First non-looping action failed");for(int i=0;i<200;++i)oneShot.tick(.01f);expect(oneShot.state().plays==1&&oneShot.state().countdown<=0&&almost(oneShot.state().animationTime,1),"Non-looping hammer replayed or lost its held pose");
        spec.loop=true;HammerController repeating(spec,1);repeating.tick(.01f);repeating.tick(1);expect(!repeating.tick(.2f)&&repeating.tick(.1f)&&repeating.state().plays==2,"Hammer replay countdown ignored authored duration and wait");
        rejects([&]{controller.tick(-1);});rejects([&]{controller.tick(std::numeric_limits<float>::quiet_NaN());});
        modelFixture().write(root/L"actors/test.dne");BundleFixture fixture;
        auto load=[&](const BundleFixture& value) { value.write(root/L"actors.dna");return loadActors(root/L"actors.dna"); };
        auto assets=load(fixture);expect(assets.courses.size()==15&&assets.models.size()==1&&assets.courses[0].hammers[0].boxes.size()==1,"Actor catalog read failed");
        CourseActorSystem runtime(assets,0);auto actor=runtime.lookup(4,"hammer");expect(bool(actor)&&std::get<bool>(actor->call("Resume",{})),"Native script actor lookup failed");
        runtime.tick(.5f);expect(almost(runtime.boxTransforms(0)[0]._41,1),"Bone attachment used inverse bind or wrong composition");
        expect(!std::get<bool>(actor->call("Pause",{})),"Pause wrapper must return authored false");
        runtime.tick(.25f);expect(almost(runtime.boxTransforms(0)[0]._41,1.5f),"Paused controller froze attached box");
        expect(runtime.notices().size()==1&&runtime.notices()[0].field=="TimedEvent","Authored timed event silently disappeared");
        expect(!runtime.lookup(0,"unknown"),"Unknown actor lookup created a substitute");
        auto ambiguous=assets;ambiguous.courses[0].hammers.push_back(ambiguous.courses[0].hammers[0]);ambiguous.courses[0].hammers.back().ordinal=11;
        CourseActorSystem repeatedNames(ambiguous,0);expect(!repeatedNames.lookup(4,"hammer")&&!repeatedNames.lookup(0,"hammer"),"Ambiguous hammer name silently selected an instance");
        { CourseActorSystem temporary(assets,0);actor=temporary.lookup(4,"hammer"); }
        rejects([&]{actor->call("Reset",{});});
        auto invalid=fixture;invalid.set(invalid.modelIndex,1);rejects([&]{load(invalid);});
        invalid=fixture;invalid.set(invalid.bone,1);rejects([&]{load(invalid);});
        invalid=fixture;invalid.set(invalid.resource,0);rejects([&]{load(invalid);});
        invalid=fixture;invalid.set(invalid.secondId,0);rejects([&]{load(invalid);});
        invalid=fixture;invalid.set(invalid.loop,2);rejects([&]{load(invalid);});
        invalid=fixture;invalid.set(invalid.matrixSlot,0);rejects([&]{load(invalid);});
        invalid=fixture;invalid.set(invalid.eventTime,0x40000000u);rejects([&]{load(invalid);});
        invalid=fixture;invalid.data.pop_back();rejects([&]{load(invalid);});
        if(argc>1) { auto owned=loadActors(std::filesystem::path(argv[1])/"actors.dna");size_t count=0;for(const auto& course:owned.courses) { CourseActorSystem run(owned,course.id);for(const auto& instance:course.hammers)run.lookup(instance.chunk,instance.name)->call("Resume",{});for(int i=0;i<120;++i)run.tick(1.f/60);count+=course.hammers.size(); }expect(count>0,"Owned actors missing");std::cout<<"Owned hammer catalog checked: "<<count<<" instances\n"; }
        std::cout<<"Native actor loading, controller scheduling, held action and absolute bone attachments passed\n";return 0;
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
