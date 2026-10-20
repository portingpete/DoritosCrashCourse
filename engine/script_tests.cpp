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
struct NativeObject:ScriptObject {
    ScriptContext fields;
    std::vector<std::vector<ScriptValue>> calls;
    explicit NativeObject(std::string name):fields{{"Name",std::move(name)},{"Amount",1.0},{"Enabled",true},{"Empty",std::monostate{}},{"Peer",ScriptObjectPtr{}},{"2",2.0}} {}
    ScriptValue get(std::string_view name) const override {
        auto found=fields.find(name);return found==fields.end()?ScriptObject::get(name):found->second;
    }
    void set(std::string_view name,const ScriptValue& value) override {
        auto found=fields.find(name);
        if(found==fields.end())return ScriptObject::set(name,value);
        if(name!="Empty"&&!(name=="Peer"&&std::holds_alternative<std::monostate>(value)))
            check(found->second.index()==value.index(),"Native object field type differs");
        found->second=value;
    }
    bool hasMethod(std::string_view name) const override { return name=="Echo"||name=="Record"; }
    ScriptValue call(std::string_view name,std::span<const ScriptValue> arguments) override {
        if(name=="Echo") { check(arguments.size()==1,"Native Echo argument count differs");return arguments[0]; }
        if(name=="Record") { calls.emplace_back(arguments.begin(),arguments.end());return double(arguments.size()); }
        return ScriptObject::call(name,arguments);
    }
};
struct TrackedObject:NativeObject {
    int& alive;
    TrackedObject(int& count):NativeObject("tracked"),alive(count) { ++alive; }
    ~TrackedObject() override { --alive; }
};
void objects() {
    ScriptVm vm;
    auto object=std::make_shared<NativeObject>("first"),peer=std::make_shared<NativeObject>("peer");
    object->fields["Peer"]=ScriptObjectPtr(peer);
    vm.setGlobal("object",ScriptObjectPtr(object));
    vm.bind("Peer",[&](auto)->ScriptValue { return ScriptObjectPtr(peer); });
    std::string good=R"(
        assert(type(object)=='userdata' and getmetatable(object)==false)
        assert(object.Name=='first' and object.Amount==1 and object.Enabled and object.Empty==nil)
        object.Name='changed'; object.Amount=42.5; object.Enabled=false
        object[2.0]=17; assert(object[2]==17 and object['2']==17)
        assert(object.Name=='changed' and object.Amount==42.5 and object.Enabled==false)
        assert(object.Peer.Name=='peer' and object.Peer==Peer())
        assert(object.Echo('dot')=='dot' and object:Echo('colon')=='colon')
        assert(object:Echo(object.Peer)==object.Peer)
        local saved=object.Record
        assert(saved(true,2,'dot',object.Peer)==4)
        assert(object:Record(false,3,'colon')==3)
    )";
    vm.start(bytes(good),"native-host-object-fields");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Complete,"Native host object field or method contract failed");
    check(std::get<std::string>(object->get("Name"))=="changed"&&std::get<double>(object->get("2"))==17,"Lua field writes did not reach native object");
    check(object->calls.size()==2&&object->calls[0].size()==4&&object->calls[1].size()==3,"Native dot/colon argument counts differ");
    check(std::get<bool>(object->calls[0][0])&&std::get<double>(object->calls[0][1])==2&&std::get<std::string>(object->calls[1][2])=="colon","Native method argument types differ");
    check(std::get<ScriptObjectPtr>(object->calls[0][3])==peer,"Native method object argument identity differs");
    vm.clearFinished();
    for(auto script:{"return object.Unknown", "object.Unknown=1", "object.Unknown()", "object.Enabled=7", "return object[true]"}) {
        std::string failure=script;vm.start(bytes(failure),"native-host-object-invalid-member");vm.tick(0);
        check(vm.tasks()[0].status==ScriptStatus::Failed&&!vm.tasks()[0].error.empty(),"Unsupported native object operation reported success");
        vm.clearFinished();
    }
    rejects([&] { vm.setGlobal("invalid",std::nan("")); });
    rejects([&] { vm.setGlobal("Wait",true); });
    vm.bind("ChangeGlobal",[&](auto)->ScriptValue { vm.setGlobal("changed",true);return {}; });
    std::string reenter="ChangeGlobal()";vm.start(bytes(reenter),"native-shared-global-reentry");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Failed,"Native callback changed shared globals while running");
}
void contexts() {
    ScriptVm vm;
    auto scene=std::make_shared<NativeObject>("scene"),fallback=std::make_shared<NativeObject>("fallback");
    vm.setGlobal("scene",ScriptObjectPtr(scene));vm.setGlobal("me",ScriptObjectPtr(fallback));
    auto definition=std::make_shared<NativeObject>("definition");std::weak_ptr<NativeObject> definitionWeak=definition;
    std::string common="function Common(label) scene.Record(me.Name,label); Callback(label); return me.Name end";
    vm.start(bytes(common),"native-common-function",{{"me",ScriptObjectPtr(definition)}});vm.tick(0);vm.clearFinished();definition.reset();
    check(definitionWeak.expired(),"Shared Lua function retained the defining task's context");
    std::vector<std::string> callbacks;
    vm.bind("Callback",[&](std::span<const ScriptValue> args)->ScriptValue {
        auto context=vm.currentContext();check(context&&context->size()==3,"Native callback lost its task context");
        auto object=std::get<ScriptObjectPtr>(context->at("me"));
        auto collided=std::get<ScriptObjectPtr>(context->at("collided"));
        callbacks.push_back(std::get<std::string>(object->get("Name"))+":"+std::get<std::string>(args[0]));
        check(std::get<std::string>(collided->get("Name"))==std::get<std::string>(object->get("Name"))+"-contact","Native callback mixed collision contexts");
        check(std::get<std::string>(context->at("tag"))==std::get<std::string>(object->get("Name"))+"-local","Context write did not reach current native callback");
        return {};
    });
    auto a=std::make_shared<NativeObject>("alpha"),b=std::make_shared<NativeObject>("beta");
    auto ca=std::make_shared<NativeObject>("alpha-contact"),cb=std::make_shared<NativeObject>("beta-contact");
    std::string first=R"(tag='alpha-local'; assert(_G.me==me and rawget(_G,'me')==me); assert(Common('start')=='alpha'); Wait(.2); assert(Common('finish')=='alpha' and tag=='alpha-local'))";
    std::string second=R"(rawset(_G,'tag','beta-local'); assert(Common('start')=='beta'); Wait(.1); assert(Common('finish')=='beta' and tag=='beta-local'))";
    vm.start(bytes(first),"native-context-alpha",{{"me",ScriptObjectPtr(a)},{"collided",ScriptObjectPtr(ca)},{"tag",std::string("initial")}});
    vm.start(bytes(second),"native-context-beta",{{"me",ScriptObjectPtr(b)},{"collided",ScriptObjectPtr(cb)},{"tag",std::string("initial")}});
    check(vm.currentContext()==nullptr,"Native context escaped callback lifetime");
    vm.tick(0);check(vm.tasks()[0].status==ScriptStatus::Waiting&&vm.tasks()[1].status==ScriptStatus::Waiting,"Context tasks did not reach Wait");
    vm.tick(.1);check(vm.tasks()[0].status==ScriptStatus::Waiting&&vm.tasks()[1].status==ScriptStatus::Complete,"Context tasks resumed at wrong deadlines");
    vm.tick(.1);check(vm.tasks()[0].status==ScriptStatus::Complete&&vm.currentContext()==nullptr,"Native task context was not restored after resume");
    check(callbacks==std::vector<std::string>{"alpha:start","beta:start","beta:finish","alpha:finish"},"Shared Lua/native functions retained another task's context");
    check(scene->calls.size()==4&&std::get<std::string>(scene->calls[2][0])=="beta","Shared Lua function used the definition environment");
    vm.clearFinished();
    std::string shared="assert(me.Name=='fallback' and tag==nil and rawget(_G,'tag')==nil)";
    vm.start(bytes(shared),"native-context-fallback");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Complete,"Named task context leaked into shared globals");
}
void objectLifetime() {
    int alive=0;
    {
        ScriptVm vm;
        auto object=std::make_shared<TrackedObject>(alive);std::weak_ptr<TrackedObject> weak=object;
        vm.setGlobal("object",ScriptObjectPtr(object));object.reset();
        std::string script="SavedMethod=object.Echo; object=nil; Wait(0); assert(SavedMethod('alive')=='alive'); SavedMethod=nil";
        vm.start(bytes(script),"native-host-method-lifetime");vm.tick(0);
        check(alive==1&&!weak.expired(),"Bound method did not retain its native receiver across Wait");
        vm.tick(0);check(vm.tasks()[0].status==ScriptStatus::Complete,"Retained native host method did not resume");vm.clearFinished();
        check(alive==0&&weak.expired(),"Native host userdata was not released by GC");
        vm.setGlobal("object",ScriptObjectPtr(std::make_shared<TrackedObject>(alive)));
        check(alive==1,"Native host fixture did not enter the VM");
    }
    check(alive==0,"Closing the native VM leaked a host object");
    ScriptVm vm(1024*1024,10000000);
    vm.bind("NewObject",[&](auto)->ScriptValue { return ScriptObjectPtr(std::make_shared<TrackedObject>(alive)); });
    std::string growth="local t={} for i=1,1000000 do t[i]=NewObject() end";
    vm.start(bytes(growth),"native-host-object-memory-quota");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Failed&&vm.memoryUsed()<=1024*1024,"Native host userdata escaped the Lua memory quota");
    vm.clearFinished();check(alive==0,"Native host object leaked during allocation failure");
    vm.setGlobal("object",ScriptObjectPtr(std::make_shared<TrackedObject>(alive)));
    std::string recovered="assert(object:Echo('recovered')=='recovered')";
    vm.start(bytes(recovered),"native-host-object-quota-recovery");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Complete,"Native host userdata did not recover after quota failure");
    vm.setGlobal("object",std::monostate{});vm.clearFinished();check(alive==0,"Recovered native host object was not released");
}
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
void frameScheduler() {
    ScriptVm vm;
    std::vector<std::string> events;
    vm.bind("Event",[&](std::span<const ScriptValue> arguments)->ScriptValue {
        auto context=vm.currentContext();check(context&&arguments.size()==1,"Frame wait lost native callback context");
        events.push_back(std::get<std::string>(context->at("name"))+":"+std::get<std::string>(arguments[0]));return {};
    });
    std::string common="function FrameCommon(n) Event('before'); WaitFrames(n); Event('after') end";
    vm.start(bytes(common),"native-shared-frame-function");vm.tick(0);vm.clearFinished();
    std::string one="FrameCommon(1); Wait(.4); Event('timed'); WaitFrames(2); Event('again')";
    std::string two="FrameCommon(2)",zero="FrameCommon(0)";
    vm.start(bytes(one),"native-frame-one",{{"name",std::string("one")}});
    vm.start(bytes(two),"native-frame-two",{{"name",std::string("two")}});
    vm.start(bytes(zero),"native-frame-zero",{{"name",std::string("zero")}});
    vm.tick(0);
    check(events==std::vector<std::string>{"one:before","two:before","zero:before"},"Frame wait resumed in its yielding tick");
    rejects([&] { vm.tick(-.1); });rejects([&] { vm.tick(std::nan("")); });
    vm.tick(.25);
    check(events==std::vector<std::string>{"one:before","two:before","zero:before","one:after","zero:after"},"One/zero frame waits did not resume on the next frame");
    check(vm.tasks()[0].status==ScriptStatus::Waiting&&vm.tasks()[1].status==ScriptStatus::Waiting&&vm.tasks()[2].status==ScriptStatus::Complete,"Frame wait state differs after one subsequent tick");
    vm.tick(0);
    check(events.back()=="two:after"&&vm.tasks()[1].status==ScriptStatus::Complete,"Two frame wait did not count a zero-second tick");
    check(vm.tasks()[0].status==ScriptStatus::Waiting,"Frame scheduling changed elapsed-second Wait behavior");
    vm.tick(.4);
    check(events.back()=="one:timed"&&vm.tasks()[0].status==ScriptStatus::Waiting,"Seconds wait did not resume at its elapsed deadline");
    vm.tick(1);
    check(events.back()=="one:timed"&&vm.tasks()[0].status==ScriptStatus::Waiting,"Elapsed seconds incorrectly satisfied a two-frame wait");
    vm.tick(0);
    check(events.back()=="one:again"&&events.size()==8&&vm.tasks()[0].status==ScriptStatus::Complete,"Repeated frame wait did not resume on its second subsequent tick");
    check(vm.currentContext()==nullptr,"Frame wait retained an active native callback context");vm.clearFinished();
    for(auto source:{"WaitFrames(-1)","WaitFrames(.5)","WaitFrames(0/0)","WaitFrames(math.huge)","WaitFrames(true)","WaitFrames(nil)","WaitFrames('2')","WaitFrames(9007199254740992)"}) {
        std::string invalid=source;vm.start(bytes(invalid),"native-invalid-frame-count");vm.tick(0);
        check(vm.tasks()[0].status==ScriptStatus::Failed&&vm.tasks()[0].error.find("WaitFrames")!=std::string::npos,"Invalid frame count was accepted");vm.clearFinished();
    }
    rejects([&] { vm.bind("WaitFrames",[](auto)->ScriptValue { return {}; }); });
    rejects([&] { vm.setGlobal("WaitFrames",true); });
    rejects([&] { vm.start(bytes(two),"native-invalid-frame-context",{{"WaitFrames",true}}); });
    std::string longest="WaitFrames(9007199254740991)";
    vm.start(bytes(longest),"native-long-frame-count");vm.tick(0);vm.tick(1);
    check(vm.tasks()[0].status==ScriptStatus::Waiting,"Large exact frame count overflowed its scheduler deadline");

    ScriptVm bounded(1024*1024,1000);
    std::string loop="WaitFrames(1); while true do end",recover="WaitFrames(2); assert(true)";
    bounded.start(bytes(loop),"native-frame-resume-instruction-quota");bounded.start(bytes(recover),"native-frame-resume-after-failure");bounded.tick(0);bounded.tick(.9);
    check(bounded.tasks()[0].status==ScriptStatus::Failed&&bounded.tasks()[0].error.find("instruction budget")!=std::string::npos,"Frame wait bypassed the resumed instruction quota");
    check(bounded.tasks()[1].status==ScriptStatus::Waiting,"Quota failure resumed another frame task too soon");bounded.tick(0);
    check(bounded.tasks()[1].status==ScriptStatus::Complete,"Frame scheduler did not recover from another task's quota failure");bounded.clearFinished();
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
void logsAndOwnedDelays() {
    ScriptVm vm;
    // The original release sink emits nothing, but Log returns true and reads
    // only a string at argument one. Native retention is observable telemetry.
    std::string logging="assert(Log('first',{})==true); WaitFrames(1); assert(Log('a'..string.char(0)..'b')); Log(42); Log()";
    auto logTask=vm.start(bytes(logging),"native-log",{},71);vm.tick(.1);vm.tick(.2);
    check(vm.tasks()[0].status==ScriptStatus::Complete&&vm.logs().size()==4,"Native Log contract failed");
    check(vm.logs()[0].message=="first"&&vm.logs()[1].message=="a"&&vm.logs()[2].message.empty()&&vm.logs()[3].message.empty(),
          "Native Log string extraction differs");
    check(vm.logs()[0].task==logTask&&vm.logs()[0].owner==71&&std::abs(vm.logs()[0].time-.1)<1e-9&&
          std::abs(vm.logs()[1].time-.3)<1e-9,"Native Log lost task ownership or scheduler time");
    vm.clearFinished();check(vm.logs().size()==4,"Task collection erased native logs");vm.clearLogs();
    std::string saturation="for i=1,1030 do Log('bounded') end";
    vm.start(bytes(saturation),"native-log-budget");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Complete&&vm.logs().size()==1024&&vm.droppedLogs()==6,"Native Log retention is unbounded or changes dispatch");
    vm.clearFinished();vm.clearLogs();check(vm.logs().empty()&&!vm.droppedLogs(),"Native log reset retained old overflow");
    std::string oversized="Log(string.rep('x',4097))";
    vm.start(bytes(oversized),"native-log-string-budget");vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Failed&&vm.tasks()[0].error.find("Log string outside budget")!=std::string::npos,"Native Log bypassed host string quota");
    vm.clearFinished();

    std::string timeWait="Wait(1); Log('time-resumed')",frameWait="WaitFrames(3); Log('frame-resumed')";
    // Same low bits, different upper bits: the complete owner tag must match.
    constexpr uint64_t alpha=0x100000007ULL,beta=0x200000007ULL;
    vm.start(bytes(timeWait),"alpha-time",{},alpha);vm.start(bytes(frameWait),"alpha-frame",{},alpha);
    vm.start(bytes(frameWait),"beta-frame",{},beta);vm.start(bytes(timeWait),"kernel-time");vm.tick(0);
    check(vm.cancelDelays(alpha)==2&&vm.cancelDelays(alpha)==0,"Owned delay cancellation did not match exactly or was not idempotent");
    auto states=vm.tasks();check(states[0].status==ScriptStatus::Cancelled&&states[1].status==ScriptStatus::Cancelled&&
        states[2].status==ScriptStatus::Waiting&&states[3].status==ScriptStatus::Waiting,"Owned cancellation crossed actor/kernel boundaries");
    vm.clearFinished();check(vm.tasks().size()==2,"Cancelled coroutine references were not collected");
    std::string clean="assert(CleanMyDelays()); Log('kernel-current'); Wait(0); Log('kernel-next')";
    vm.start(bytes(clean),"native-global-clean");vm.tick(0);
    states=vm.tasks();check(states[0].status==ScriptStatus::Waiting&&states[1].status==ScriptStatus::Cancelled&&states[2].status==ScriptStatus::Waiting,
          "Global CleanMyDelays canceled another owner or its current invocation");
    vm.tick(0);vm.tick(0);vm.tick(1);
    check(vm.logs().size()==3&&vm.logs()[0].message=="kernel-current"&&vm.logs()[1].message=="kernel-next"&&vm.logs()[2].owner==beta,
          "Canceled delay resumed or protected invocation could not wait again");
    vm.clearFinished();vm.clearLogs();

    // A task that has not yet called Wait has no delayed continuation to clean.
    vm.start(bytes(frameWait),"queued-owner",{},alpha);
    check(vm.cancelDelays(alpha)==0&&vm.tasks()[0].status==ScriptStatus::Queued,"Cancellation removed work before it created a delay");
    vm.tick(0);check(vm.cancelDelays(alpha)==1,"Queued work could not later become a cancellable delay");vm.clearFinished();
    // A resumed task still has Waiting status during its callback; protection
    // therefore requires identity, rather than a status-only filter.
    vm.bind("CleanOwner",[&](auto)->ScriptValue { return double(vm.cancelDelays(alpha)); });
    std::string resumed="WaitFrames(1); assert(CleanOwner()==0); WaitFrames(1); Log('protected-resume')";
    vm.start(bytes(resumed),"native-current-owner",{},alpha);vm.tick(0);vm.tick(0);vm.tick(0);
    check(vm.tasks()[0].status==ScriptStatus::Complete&&vm.logs().size()==1,"Owner cleanup canceled its resumed active coroutine");
    vm.clearFinished();
    int alive=0;auto object=std::make_shared<TrackedObject>(alive);std::weak_ptr<TrackedObject> weak=object;
    vm.start(bytes(timeWait),"native-cancel-gc",{{"me",ScriptObjectPtr(object)}},alpha);object.reset();vm.tick(0);vm.cancelDelays(alpha);vm.clearFinished();
    check(weak.expired()&&alive==0,"Cancelled coroutine retained its native context");
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
        scheduler();frameScheduler();failures();logsAndOwnedDelays();allocation();objects();contexts();objectLifetime();bundle();
        if(argc>1) {
            auto chunks=loadScripts(argv[1]);ScriptVm vm;
            for(auto& chunk:chunks)vm.validate(chunk.code,chunk.source+":"+chunk.actor+":"+chunk.field);
            std::cout<<"Native Lua asset prototypes validated="<<chunks.size()<<"\n";
        }
        std::cout<<"Native script callbacks, host objects, task contexts, time/frame waits, quotas and failure recovery passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<"\n";return 1; }
}
