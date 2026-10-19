#include "scripts.h"
#include <windows.h>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace Doritos::Engine;
namespace {
void check(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
std::span<const uint8_t> bytes(const std::string& text) {
    return {reinterpret_cast<const uint8_t*>(text.data()),text.size()};
}
template<class F> void rejects(F&& call) {
    bool failed=false;try { call(); }catch(const std::exception&) { failed=true; }
    check(failed,"Invalid native script input was accepted");
}
struct Data {
    std::vector<uint8_t> bytes;
    template<class T> void add(T value) { auto offset=bytes.size();bytes.resize(offset+sizeof(value));std::memcpy(bytes.data()+offset,&value,sizeof(value)); }
    void text(const std::string& text) { add(uint32_t(text.size()));bytes.insert(bytes.end(),text.begin(),text.end()); }
};
void scheduler() {
    ScriptVm vm;
    std::vector<std::string> events;
    vm.bind("Checkpoint",[&](std::span<const ScriptValue> args)->ScriptValue {
        check(args.size()==1,"Native checkpoint callback argument count differs");
        events.push_back(std::get<std::string>(args[0]));return {};
    });
    std::string sequence="Checkpoint('start'); Wait(0.2); Checkpoint('finish')";
    vm.start(bytes(sequence),"native-checkpoint-sequence");
    vm.tick(0);check(events==std::vector<std::string>{"start"},"Native command did not run before Wait");
    check(vm.tasks()[0].status==ScriptStatus::Waiting,"Wait did not suspend native task");
    vm.tick(.1);check(events.size()==1,"Wait resumed too early");
    vm.tick(.1);check(events==std::vector<std::string>{"start","finish"},"Native command did not resume at deadline");
    check(vm.tasks()[0].status==ScriptStatus::Complete,"Native task did not complete");
    vm.clearFinished();check(vm.tasks().empty(),"Completed native task retained after clear");
    vm.bind("Sum",[](std::span<const ScriptValue> args)->ScriptValue {
        return std::get<double>(args[0])+std::get<double>(args[1]);
    });
    std::string math="assert(Sum(2,3)==5); assert(os==nil and io==nil and package==nil and coroutine==nil and pcall==nil); assert(math.abs(-2)==2)";
    vm.start(bytes(math),"native-callback-values");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Complete,"Native callback/library values differ");
    rejects([&] { vm.tick(-1); });rejects([&] { vm.tick(std::nan("")); });
    rejects([&] { vm.bind("Wait",[](auto)->ScriptValue { return {}; }); });
}
void failures() {
    ScriptVm vm(1024*1024,1000);
    std::string loop="while true do end",after="Wait(0); assert(true)",invalid="Wait(-1)";
    vm.start(bytes(loop),"native-infinite-loop");vm.start(bytes(after),"native-next-task");
    vm.start(bytes(invalid),"native-invalid-delay");vm.tick(0);
    auto tasks=vm.tasks();
    check(tasks[0].status==ScriptStatus::Failed&&tasks[0].error.find("instruction budget")!=std::string::npos,"Unbounded script did not fail its instruction quota");
    check(tasks[1].status==ScriptStatus::Waiting,"Failed task blocked another native task");
    check(tasks[2].status==ScriptStatus::Failed,"Invalid Wait accepted");
    vm.tick(0);check(vm.tasks()[1].status==ScriptStatus::Complete,"Zero Wait did not resume on next scheduler tick");
    vm.clearFinished();
    std::string missing="UnsupportedGameBinding()";
    vm.start(bytes(missing),"native-missing-binding");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Failed,"Missing game binding reported success");
    rejects([&] { std::string corrupt="\x1bLua-invalid";vm.validate(bytes(corrupt),"corrupt"); });
    vm.clearFinished();
    vm.bind("HostError",[](auto)->ScriptValue { throw std::runtime_error("native callback rejected"); });
    std::string failure="HostError()";vm.start(bytes(failure),"native-host-error");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Failed&&vm.tasks()[0].error.find("native callback rejected")!=std::string::npos,"Native callback exception did not become a script failure");
    vm.clearFinished();
    vm.bind("Reenter",[&](auto)->ScriptValue { vm.tick(0);return {}; });
    std::string reenter="Reenter()";vm.start(bytes(reenter),"native-reentry");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Failed&&vm.tasks()[0].error.find("not reentrant")!=std::string::npos,"Native scheduler allowed callback reentry");
}
void allocation() {
    ScriptVm vm(1024*1024,10000000);
    std::string growth="local t={} for i=1,1000000 do t[i]=string.rep('x',1000)..i end";
    vm.start(bytes(growth),"native-memory-limit");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Failed,"Unbounded script memory did not fail");
    check(vm.memoryUsed()<=1024*1024,"Native allocator exceeded its memory limit");
    vm.clearFinished();
    std::string good="assert(2+2==4)";vm.start(bytes(good),"native-memory-recovery");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Complete,"Native VM did not recover after allocation failure");
    vm.clearFinished();
    for(int i=0;i<2000;++i)vm.start(bytes(good),"native-task-allocation");
    auto states=vm.tasks();
    check(std::any_of(states.begin(),states.end(),[](auto& task) { return task.status==ScriptStatus::Failed; }),"Task allocation exhaustion fixture did not exercise its limit");
    check(vm.memoryUsed()<=1024*1024,"Native task creation exceeded memory quota");
    vm.tick(0);vm.clearFinished();
    vm.bind("Recovered",[](auto)->ScriptValue { return true; });
    good="assert(Recovered())";vm.start(bytes(good),"native-task-allocation-recovery");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Complete,"Native task allocation failure was not recoverable");
}
void bundle() {
    Data data;
    data.add(0x314c4e44u);data.add(1u);data.add(1u);
    for(auto name:{"native/fixture","ACTOR","test","OnActivate"})data.text(name);
    data.add(0u);data.add(0u);
    Data chunk;
    for(uint8_t v:std::array<uint8_t,12>{0x1b,'L','u','a',0x51,0,1,4,8,4,8,0})chunk.add(v);
    chunk.add(uint64_t(0));chunk.add(0u);chunk.add(0u);
    for(uint8_t v:{0,0,2,2})chunk.add(v);
    chunk.add(1u);chunk.add(0x0080001eu);
    for(int i=0;i<5;++i)chunk.add(0u);
    data.add(uint32_t(chunk.bytes.size()));data.bytes.insert(data.bytes.end(),chunk.bytes.begin(),chunk.bytes.end());
    auto path=std::filesystem::temp_directory_path()/(L"doritos-script-contract-"+std::to_wstring(GetCurrentProcessId())+L".dnl");
    check(!std::filesystem::exists(path),"Native script fixture path already exists");
    struct Cleanup { std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove(path,error);} } cleanup{path};
    auto load=[&](const std::vector<uint8_t>& bytes) {
        { std::ofstream output(path,std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());check(bool(output),"Cannot write native script fixture"); }
        return loadScripts(path);
    };
    auto chunks=load(data.bytes);check(chunks.size()==1&&chunks[0].actor=="test","Native script bundle identity differs");
    ScriptVm vm;vm.validate(chunks[0].code,"native-binary-fixture");vm.start(chunks[0].code,"native-binary-execution");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Complete,"Native compiled fixture did not execute");
    for(size_t size=0;size<data.bytes.size();++size)rejects([&] { load({data.bytes.begin(),data.bytes.begin()+size}); });
    auto bad=data.bytes;bad.push_back(0);rejects([&] { load(bad); });
    bad=data.bytes;bad[8]=0xff;rejects([&] { load(bad); });
    bad=data.bytes;bad[bad.size()-chunk.bytes.size()+8]=4;rejects([&] { load(bad); });
}
}
int main(int argc,char** argv) {
    try {
        scheduler();failures();allocation();bundle();
        if(argc>1) {
            auto chunks=loadScripts(argv[1]);ScriptVm vm;
            for(auto& chunk:chunks)vm.validate(chunk.code,chunk.source+":"+chunk.actor+":"+chunk.field);
            std::cout<<"Native Lua asset prototypes validated="<<chunks.size()<<"\n";
        }
        std::cout<<"Native script callbacks, Wait scheduling, quotas and failure recovery passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<"\n";return 1; }
}
