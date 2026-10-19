#include "scripts.h"
#include "asset_reader.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <new>

namespace Doritos::Engine {
std::vector<ScriptChunk> loadScripts(const std::filesystem::path& path) {
    AssetReader r(path);
    need(r.read<uint32_t>()==0x314c4e44&&r.read<uint32_t>()==1,"Unsupported native script bundle");
    std::vector<ScriptChunk> result;
    auto count=r.count(16384);
    for(uint32_t i=0;i<count;++i) {
        ScriptChunk chunk;
        chunk.source=r.string();chunk.type=r.string();chunk.actor=r.string();chunk.field=r.string();
        chunk.ordinal=r.read<uint32_t>();chunk.index=r.read<uint32_t>();
        auto size=r.count(16*1024*1024);
        chunk.code.reserve(size);
        for(uint32_t n=0;n<size;++n)chunk.code.push_back(r.read<uint8_t>());
        constexpr std::array<uint8_t,12> header{0x1b,'L','u','a',0x51,0,1,4,8,4,8,0};
        need(size>=header.size()&&std::equal(header.begin(),header.end(),chunk.code.begin()),"Unsupported native script layout");
        result.push_back(std::move(chunk));
    }
    r.end();return result;
}
ScriptValue ScriptObject::get(std::string_view name) const {
    throw std::runtime_error("Unsupported native script field: "+std::string(name));
}
void ScriptObject::set(std::string_view name,const ScriptValue&) {
    throw std::runtime_error("Unsupported or read-only native script field: "+std::string(name));
}
bool ScriptObject::hasMethod(std::string_view) const { return false; }
ScriptValue ScriptObject::call(std::string_view name,std::span<const ScriptValue>) {
    throw std::runtime_error("Unsupported native script method: "+std::string(name));
}
struct ScriptVm::Impl {
    struct Work { ScriptTask state;ScriptContext context;lua_State* thread=nullptr;int reference=LUA_NOREF;uint64_t framesRemaining=0;bool waited=false; };
    struct HostValue { ScriptObjectPtr object; };
    static constexpr const char* objectType="Doritos.NativeScriptObject";
    lua_State* root=nullptr;
    int globals=LUA_NOREF;
    size_t memory=0,limit;
    uint32_t instructionLimit,remaining=0;
    double clock=0;
    uint64_t next=1;
    Work* current=nullptr;
    bool ticking=false;
    std::vector<std::unique_ptr<Work>> work;
    std::map<std::string,ScriptFunction> functions;
    static void* allocate(void* data,void* pointer,size_t oldSize,size_t size) {
        auto& vm=*static_cast<Impl*>(data);
        if(!pointer)oldSize=0;
        if(!size) { std::free(pointer);vm.memory-=oldSize;return nullptr; }
        if(size>vm.limit-(vm.memory-oldSize))return nullptr;
        void* result=std::realloc(pointer,size);
        if(result)vm.memory=vm.memory-oldSize+size;
        return result;
    }
    static Impl& owner(lua_State* state) {
        void* data=nullptr;lua_getallocf(state,&data);return *static_cast<Impl*>(data);
    }
    static int panic(lua_State* state) {
        throw std::runtime_error("Unprotected native Lua error: "+owner(state).error(state));
    }
    static void hook(lua_State* state,lua_Debug*) {
        auto& vm=owner(state);
        if(vm.remaining<=100)luaL_error(state,"Native script instruction budget exceeded");
        vm.remaining-=100;
    }
    static int wait(lua_State* state) {
        auto& vm=owner(state);
        double seconds=luaL_checknumber(state,1);
        if(!vm.current||!std::isfinite(seconds)||seconds<0||seconds>86400)
            return luaL_error(state,"Invalid native script Wait interval");
        vm.current->state.wakeTime=vm.clock+seconds;vm.current->framesRemaining=0;vm.current->waited=true;
        return lua_yield(state,0);
    }
    static int waitFrames(lua_State* state) {
        auto& vm=owner(state);
        // Lua numbers represent integers exactly through 2^53-1. A remaining
        // count avoids adding to a frame clock, so even the longest accepted
        // wait cannot overflow a deadline.
        double frames=lua_type(state,1)==LUA_TNUMBER?lua_tonumber(state,1):-1;
        if(!vm.current||!std::isfinite(frames)||frames<0||frames>9007199254740991.0||std::floor(frames)!=frames)
            return luaL_error(state,"Invalid native script WaitFrames count");
        // Zero deliberately cooperates with the scheduler, as Wait(0) does;
        // every frame wait resumes on a later tick, never in this tick.
        vm.current->framesRemaining=std::max(uint64_t(1),static_cast<uint64_t>(frames));
        vm.current->state.wakeTime=vm.clock;vm.current->waited=true;
        return lua_yield(state,0);
    }
    static HostValue* host(lua_State* state,int index) {
        if(lua_type(state,index)!=LUA_TUSERDATA||!lua_getmetatable(state,index))return nullptr;
        luaL_getmetatable(state,objectType);
        bool match=lua_rawequal(state,-1,-2)!=0;lua_pop(state,2);
        return match?static_cast<HostValue*>(lua_touserdata(state,index)):nullptr;
    }
    static std::string_view member(lua_State* state,int index) {
        auto type=lua_type(state,index);
        if(type!=LUA_TSTRING&&type!=LUA_TNUMBER)throw std::runtime_error("Native script member name must be a string or number");
        if(type==LUA_TNUMBER&&!std::isfinite(lua_tonumber(state,index)))throw std::runtime_error("Nonfinite native script member index");
        // Lua's number-to-string conversion canonicalizes integer indices
        // (Resources[2] and Resources[2.0] both address the native field "2").
        size_t length=0;auto name=lua_tolstring(state,index,&length);
        if(!length||length>128||std::memchr(name,0,length))throw std::runtime_error("Native script member name outside budget");
        return {name,length};
    }
    static ScriptValue value(lua_State* state,int index) {
        switch(lua_type(state,index)) {
        case LUA_TNIL:return {};
        case LUA_TBOOLEAN:return lua_toboolean(state,index)!=0;
        case LUA_TNUMBER: {
            double number=lua_tonumber(state,index);
            if(!std::isfinite(number))throw std::runtime_error("Nonfinite native script argument");
            return number;
        }
        case LUA_TSTRING: {
            size_t length=0;auto text=lua_tolstring(state,index,&length);
            if(length>4096)throw std::runtime_error("Native script argument string outside budget");
            return std::string(text,length);
        }
        case LUA_TUSERDATA:if(auto object=host(state,index))return object->object;[[fallthrough]];
        default:throw std::runtime_error("Unsupported native script argument type");
        }
    }
    static void push(lua_State* state,const ScriptValue& result) {
        if(std::holds_alternative<std::monostate>(result))lua_pushnil(state);
        else if(auto v=std::get_if<bool>(&result))lua_pushboolean(state,*v);
        else if(auto v=std::get_if<double>(&result)) {
            if(!std::isfinite(*v))throw std::runtime_error("Nonfinite native script callback result");
            lua_pushnumber(state,*v);
        } else if(auto text=std::get_if<std::string>(&result)) {
            if(text->size()>4096)throw std::runtime_error("Native script result string outside budget");
            lua_pushlstring(state,text->data(),text->size());
        } else {
            const auto& object=std::get<ScriptObjectPtr>(result);
            if(!object) { lua_pushnil(state);return; }
            // Look up the metatable before taking ownership: any allocation
            // failure must occur either before construction or after __gc is
            // installed on this userdata.
            luaL_getmetatable(state,objectType);
            auto storage=lua_newuserdata(state,sizeof(HostValue));
            new(storage) HostValue{object};
            lua_pushvalue(state,-2);lua_setmetatable(state,-2);
            lua_remove(state,-2);
        }
    }
    static std::vector<ScriptValue> arguments(lua_State* state,int first=1) {
        int count=lua_gettop(state)-first+1;
        if(count>32)throw std::runtime_error("Native script argument count outside budget");
        std::vector<ScriptValue> result;result.reserve(count);
        for(int i=first;i<=lua_gettop(state);++i)result.push_back(value(state,i));
        return result;
    }
    static int collectObject(lua_State* state) {
        static_cast<HostValue*>(lua_touserdata(state,1))->~HostValue();return 0;
    }
    static int equalObjects(lua_State* state) {
        auto a=host(state,1),b=host(state,2);
        lua_pushboolean(state,a&&b&&a->object==b->object);return 1;
    }
    static int callMethod(lua_State* state) {
        try {
            auto object=host(state,lua_upvalueindex(1));
            if(!object||!object->object)throw std::runtime_error("Missing native script host object");
            auto name=member(state,lua_upvalueindex(2));
            // Colon syntax supplies the captured receiver. Dot syntax supplies
            // only explicit arguments; both enter the same native method.
            int first=lua_gettop(state)>0&&lua_rawequal(state,1,lua_upvalueindex(1))?2:1;
            auto values=arguments(state,first);
            auto result=object->object->call(name,values);push(state,result);return 1;
        } catch(const std::exception& error) { lua_pushstring(state,error.what()); }
        return lua_error(state);
    }
    static int indexObject(lua_State* state) {
        try {
            auto object=host(state,1);
            if(!object||!object->object)throw std::runtime_error("Missing native script host object");
            auto name=member(state,2);
            if(object->object->hasMethod(name)) {
                lua_pushvalue(state,1);lua_pushvalue(state,2);
                lua_pushcclosure(state,callMethod,2);
            } else { auto result=object->object->get(name);push(state,result); }
            return 1;
        } catch(const std::exception& error) { lua_pushstring(state,error.what()); }
        return lua_error(state);
    }
    static int newIndexObject(lua_State* state) {
        try {
            auto object=host(state,1);
            if(!object||!object->object)throw std::runtime_error("Missing native script host object");
            auto name=member(state,2);auto result=value(state,3);object->object->set(name,result);return 0;
        } catch(const std::exception& error) { lua_pushstring(state,error.what()); }
        return lua_error(state);
    }
    static int indexGlobals(lua_State* state) {
        try {
            auto& vm=owner(state);
            if(vm.current&&lua_type(state,2)==LUA_TSTRING) {
                size_t length=0;auto text=lua_tolstring(state,2,&length);
                auto found=vm.current->context.find(std::string_view(text,length));
                if(found!=vm.current->context.end()) { push(state,found->second);return 1; }
            }
            lua_rawgeti(state,LUA_REGISTRYINDEX,vm.globals);lua_pushvalue(state,2);lua_rawget(state,-2);return 1;
        } catch(const std::exception& error) { lua_pushstring(state,error.what()); }
        return lua_error(state);
    }
    static int newIndexGlobals(lua_State* state) {
        try {
            auto& vm=owner(state);
            if(vm.current&&lua_type(state,2)==LUA_TSTRING) {
                size_t length=0;auto text=lua_tolstring(state,2,&length);
                auto found=vm.current->context.find(std::string_view(text,length));
                if(found!=vm.current->context.end()) { found->second=value(state,3);return 0; }
            }
            lua_rawgeti(state,LUA_REGISTRYINDEX,vm.globals);lua_pushvalue(state,2);lua_pushvalue(state,3);lua_rawset(state,-3);return 0;
        } catch(const std::exception& error) { lua_pushstring(state,error.what()); }
        return lua_error(state);
    }
    static int rawGet(lua_State* state) {
        luaL_checktype(state,1,LUA_TTABLE);luaL_checkany(state,2);lua_settop(state,2);
        lua_pushvalue(state,LUA_GLOBALSINDEX);bool global=lua_rawequal(state,1,-1)!=0;lua_pop(state,1);
        if(global)return indexGlobals(state);
        lua_pushvalue(state,2);lua_rawget(state,1);return 1;
    }
    static int rawSet(lua_State* state) {
        luaL_checktype(state,1,LUA_TTABLE);luaL_checkany(state,2);luaL_checkany(state,3);lua_settop(state,3);
        lua_pushvalue(state,LUA_GLOBALSINDEX);bool global=lua_rawequal(state,1,-1)!=0;lua_pop(state,1);
        if(global)newIndexGlobals(state);
        else { lua_pushvalue(state,2);lua_pushvalue(state,3);lua_rawset(state,1); }
        lua_pushvalue(state,1);return 1;
    }
    static int call(lua_State* state) {
        try {
            auto& vm=owner(state);
            auto name=lua_tostring(state,lua_upvalueindex(1));
            auto found=vm.functions.find(name);
            if(found==vm.functions.end())throw std::runtime_error("Missing native script function");
            auto values=arguments(state);
            auto result=found->second(values);push(state,result);return 1;
        } catch(const std::exception& error) {
            lua_pushstring(state,error.what());
        }
        return lua_error(state);
    }
    struct Binding { const std::string* name; };
    static int prepareBinding(lua_State* state) {
        auto& binding=*static_cast<Binding*>(lua_touserdata(state,1));
        lua_pushlstring(state,binding.name->data(),binding.name->size());
        lua_pushcclosure(state,call,1);lua_setglobal(state,binding.name->c_str());return 0;
    }
    struct Global { const std::string* name;const ScriptValue* value; };
    static int prepareGlobal(lua_State* state) {
        auto& global=*static_cast<Global*>(lua_touserdata(state,1));
        try { push(state,*global.value);lua_setglobal(state,global.name->c_str());return 0; }
        catch(const std::exception& error) { lua_pushstring(state,error.what()); }
        return lua_error(state);
    }
    struct Creation { Work* task;std::span<const uint8_t> code;const std::string* name; };
    static int prepareTask(lua_State* state) {
        auto& creation=*static_cast<Creation*>(lua_touserdata(state,1));
        auto& task=*creation.task;
        task.thread=lua_newthread(state);task.reference=luaL_ref(state,LUA_REGISTRYINDEX);
        int status=luaL_loadbuffer(task.thread,reinterpret_cast<const char*>(creation.code.data()),creation.code.size(),creation.name->c_str());
        if(status) { task.state.status=ScriptStatus::Failed;task.state.error=owner(state).error(task.thread); }
        return 0;
    }
    static int initialize(lua_State* state) {
        auto& vm=owner(state);
        luaopen_base(state);lua_settop(state,0);
        luaopen_math(state);lua_settop(state,0);
        luaopen_string(state);lua_settop(state,0);
        luaopen_table(state);lua_settop(state,0);
        // Keep the reference free-list terminator present. Lua's default nil
        // terminator can require a new registry hash entry during unref, which
        // must still work after a task has exhausted its allocation quota.
        lua_pushinteger(state,0);lua_rawseti(state,LUA_REGISTRYINDEX,0);
        // Scene scripts receive native host functions. Filesystem, process,
        // dynamic module loading and unmanaged coroutines are not exposed.
        // Protected script calls are withheld so they cannot swallow quotas.
        for(auto name:{"dofile","loadfile","load","loadstring","require","module","getfenv","setfenv",
                       "newproxy","coroutine","pcall","xpcall","print"}) {
            lua_pushnil(state);lua_setglobal(state,name);
        }
        lua_pushcfunction(state,wait);lua_setglobal(state,"Wait");
        lua_pushcfunction(state,waitFrames);lua_setglobal(state,"WaitFrames");
        // rawget/rawset on _G must obey the same active context as ordinary
        // globals. Other tables retain the standard raw operations.
        lua_pushcfunction(state,rawGet);lua_setglobal(state,"rawget");
        lua_pushcfunction(state,rawSet);lua_setglobal(state,"rawset");
        luaL_newmetatable(state,objectType);
        lua_pushcfunction(state,collectObject);lua_setfield(state,-2,"__gc");
        lua_pushcfunction(state,indexObject);lua_setfield(state,-2,"__index");
        lua_pushcfunction(state,newIndexObject);lua_setfield(state,-2,"__newindex");
        lua_pushcfunction(state,equalObjects);lua_setfield(state,-2,"__eq");
        lua_pushboolean(state,0);lua_setfield(state,-2,"__metatable");lua_pop(state,1);
        // Every Lua function keeps this single environment. An empty proxy
        // resolves task context dynamically, so a shared function defined by
        // task A observes task B's me when B later invokes it.
        lua_newtable(state);
        lua_pushnil(state);
        while(lua_next(state,LUA_GLOBALSINDEX)) {
            lua_pushvalue(state,-2);lua_pushvalue(state,-2);lua_rawset(state,-5);
            lua_pushvalue(state,-2);lua_pushnil(state);lua_rawset(state,LUA_GLOBALSINDEX);
            lua_pop(state,1);
        }
        vm.globals=luaL_ref(state,LUA_REGISTRYINDEX);
        lua_newtable(state);
        lua_pushcfunction(state,indexGlobals);lua_setfield(state,-2,"__index");
        lua_pushcfunction(state,newIndexGlobals);lua_setfield(state,-2,"__newindex");
        lua_pushboolean(state,0);lua_setfield(state,-2,"__metatable");
        lua_setmetatable(state,LUA_GLOBALSINDEX);return 0;
    }
    explicit Impl(size_t bytes,uint32_t instructions):limit(bytes),instructionLimit(instructions) {
        need(limit>=1024*1024&&limit<=256*1024*1024&&instructionLimit>=1000&&instructionLimit<=10000000,
             "Native script VM limits outside supported range");
        root=lua_newstate(allocate,this);need(root!=nullptr,"Cannot create native Lua state");
        lua_atpanic(root,panic);
        if(lua_cpcall(root,initialize,nullptr)) {
            auto message=error(root);lua_close(root);root=nullptr;throw std::runtime_error(message);
        }
    }
    ~Impl() { if(root)lua_close(root); }
    std::string error(lua_State* state) {
        auto text=lua_type(state,-1)==LUA_TSTRING?lua_tostring(state,-1):nullptr;return text?text:"Native script VM error";
    }
};
ScriptVm::ScriptVm(size_t memoryLimit,uint32_t instructionLimit):impl(std::make_unique<Impl>(memoryLimit,instructionLimit)) {}
ScriptVm::~ScriptVm()=default;
void ScriptVm::validate(std::span<const uint8_t> code,const std::string& name) {
    need(!impl->ticking,"Cannot load scripts during a native callback");
    need(!code.empty()&&code.size()<=16*1024*1024,"Native script size outside budget");
    int status=luaL_loadbuffer(impl->root,reinterpret_cast<const char*>(code.data()),code.size(),name.c_str());
    std::string message=status?impl->error(impl->root):std::string{};
    lua_pop(impl->root,1);lua_gc(impl->root,LUA_GCCOLLECT,0);
    if(status)throw std::runtime_error(message);
}
void ScriptVm::bind(const std::string& name,ScriptFunction function) {
    need(!impl->ticking,"Cannot change script functions during a native callback");
    need(!name.empty()&&name.size()<=128&&name.find('\0')==std::string::npos&&name!="Wait"&&name!="WaitFrames"&&bool(function),"Invalid native script function binding");
    Impl::Binding binding{&name};
    if(lua_cpcall(impl->root,Impl::prepareBinding,&binding)) {
        auto message=impl->error(impl->root);lua_pop(impl->root,1);throw std::runtime_error(message);
    }
    impl->functions[name]=std::move(function);
}
void ScriptVm::setGlobal(const std::string& name,const ScriptValue& value) {
    need(!impl->ticking,"Cannot change shared script globals during a native callback");
    need(!name.empty()&&name.size()<=128&&name.find('\0')==std::string::npos&&name!="Wait"&&name!="WaitFrames"&&name!="_G","Invalid native script global name");
    Impl::Global global{&name,&value};
    if(lua_cpcall(impl->root,Impl::prepareGlobal,&global)) {
        auto message=impl->error(impl->root);lua_pop(impl->root,1);throw std::runtime_error(message);
    }
}
uint64_t ScriptVm::start(std::span<const uint8_t> code,const std::string& name,ScriptContext context) {
    need(!impl->ticking,"Queue new script tasks after the native callback completes");
    need(!code.empty()&&code.size()<=16*1024*1024&&impl->work.size()<4096,"Native script task budget exceeded");
    need(context.size()<=128,"Native script context outside budget");
    for(auto& [key,value]:context) {
        need(!key.empty()&&key.size()<=128&&key.find('\0')==std::string::npos&&key!="_G"&&key!="Wait"&&key!="WaitFrames","Invalid native script context name");
        if(auto number=std::get_if<double>(&value))need(std::isfinite(*number),"Nonfinite native script context value");
        if(auto text=std::get_if<std::string>(&value))need(text->size()<=4096,"Native script context string outside budget");
    }
    auto task=std::make_unique<Impl::Work>();task->state.id=impl->next++;task->state.wakeTime=impl->clock;
    task->context=std::move(context);
    Impl::Creation creation{task.get(),code,&name};
    if(lua_cpcall(impl->root,Impl::prepareTask,&creation)) {
        task->state.status=ScriptStatus::Failed;task->state.error=impl->error(impl->root);lua_pop(impl->root,1);
    }
    auto id=task->state.id;impl->work.push_back(std::move(task));return id;
}
const ScriptContext* ScriptVm::currentContext() const noexcept { return impl->current?&impl->current->context:nullptr; }
void ScriptVm::tick(double seconds) {
    need(!impl->ticking,"Native script scheduler is not reentrant");
    need(std::isfinite(seconds)&&seconds>=0&&seconds<=1,"Invalid native script time step");
    impl->ticking=true;
    struct Clear { bool& flag;~Clear(){flag=false;} } clear{impl->ticking};
    impl->clock+=seconds;
    for(auto& task:impl->work) {
        if(task->state.status!=ScriptStatus::Queued&&task->state.status!=ScriptStatus::Waiting)continue;
        if(task->framesRemaining&&--task->framesRemaining)continue;
        if(task->state.wakeTime>impl->clock+1e-12)continue;
        task->waited=false;impl->current=task.get();impl->remaining=impl->instructionLimit;
        lua_sethook(task->thread,Impl::hook,LUA_MASKCOUNT,100);
        int status=lua_resume(task->thread,0);impl->current=nullptr;
        if(status==LUA_YIELD&&task->waited)task->state.status=ScriptStatus::Waiting;
        else if(!status)task->state.status=ScriptStatus::Complete;
        else { task->state.status=ScriptStatus::Failed;task->state.error=status==LUA_YIELD?"Unmanaged native script yield":impl->error(task->thread); }
    }
}
std::vector<ScriptTask> ScriptVm::tasks() const {
    std::vector<ScriptTask> result;for(auto& task:impl->work)result.push_back(task->state);return result;
}
void ScriptVm::clearFinished() {
    need(!impl->ticking,"Cannot remove native script tasks during a callback");
    for(auto& task:impl->work)if(task->state.status==ScriptStatus::Complete||task->state.status==ScriptStatus::Failed) {
        luaL_unref(impl->root,LUA_REGISTRYINDEX,task->reference);
        task->reference=LUA_NOREF;
    }
    std::erase_if(impl->work,[&](auto& task) { return task->state.status==ScriptStatus::Complete||task->state.status==ScriptStatus::Failed; });
    lua_gc(impl->root,LUA_GCCOLLECT,0);
}
size_t ScriptVm::memoryUsed() const { return impl->memory; }
}
