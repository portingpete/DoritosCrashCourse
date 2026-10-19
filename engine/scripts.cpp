#include "scripts.h"
#include "asset_reader.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>

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
struct ScriptVm::Impl {
    struct Work { ScriptTask state;lua_State* thread=nullptr;int reference=LUA_NOREF;bool waited=false; };
    lua_State* root=nullptr;
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
        vm.current->state.wakeTime=vm.clock+seconds;vm.current->waited=true;
        return lua_yield(state,0);
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
        default:throw std::runtime_error("Unsupported native script argument type");
        }
    }
    static void push(lua_State* state,const ScriptValue& result) {
        if(std::holds_alternative<std::monostate>(result))lua_pushnil(state);
        else if(auto v=std::get_if<bool>(&result))lua_pushboolean(state,*v);
        else if(auto v=std::get_if<double>(&result)) {
            if(!std::isfinite(*v))throw std::runtime_error("Nonfinite native script callback result");
            lua_pushnumber(state,*v);
        } else {
            const auto& text=std::get<std::string>(result);
            if(text.size()>4096)throw std::runtime_error("Native script result string outside budget");
            lua_pushlstring(state,text.data(),text.size());
        }
    }
    static int call(lua_State* state) {
        try {
            auto& vm=owner(state);
            auto name=lua_tostring(state,lua_upvalueindex(1));
            auto found=vm.functions.find(name);
            if(found==vm.functions.end())throw std::runtime_error("Missing native script function");
            int count=lua_gettop(state);
            if(count>32)throw std::runtime_error("Native script argument count outside budget");
            std::vector<ScriptValue> arguments;
            for(int i=1;i<=count;++i)arguments.push_back(value(state,i));
            auto result=found->second(arguments);push(state,result);return 1;
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
    struct Creation { Work* task;std::span<const uint8_t> code;const std::string* name; };
    static int prepareTask(lua_State* state) {
        auto& creation=*static_cast<Creation*>(lua_touserdata(state,1));
        auto& task=*creation.task;
        task.thread=lua_newthread(state);task.reference=luaL_ref(state,LUA_REGISTRYINDEX);
        int status=luaL_loadbuffer(task.thread,reinterpret_cast<const char*>(creation.code.data()),creation.code.size(),creation.name->c_str());
        if(status) { task.state.status=ScriptStatus::Failed;task.state.error=owner(state).error(task.thread); }
        return 0;
    }
    explicit Impl(size_t bytes,uint32_t instructions):limit(bytes),instructionLimit(instructions) {
        need(limit>=1024*1024&&limit<=256*1024*1024&&instructionLimit>=1000&&instructionLimit<=10000000,
             "Native script VM limits outside supported range");
        root=lua_newstate(allocate,this);need(root!=nullptr,"Cannot create native Lua state");
        lua_atpanic(root,panic);
        luaopen_base(root);lua_settop(root,0);
        luaopen_math(root);lua_settop(root,0);
        luaopen_string(root);lua_settop(root,0);
        luaopen_table(root);lua_settop(root,0);
        // Keep the reference free-list terminator present. Lua's default nil
        // terminator can require a new registry hash entry during unref, which
        // must still work after a task has exhausted its allocation quota.
        lua_pushinteger(root,0);lua_rawseti(root,LUA_REGISTRYINDEX,0);
        // Scene scripts receive native host functions. Filesystem, process,
        // dynamic module loading and unmanaged coroutines are not exposed.
        // Protected script calls are withheld so they cannot swallow quotas.
        for(auto name:{"dofile","loadfile","load","loadstring","require","module","getfenv","setfenv",
                       "newproxy","coroutine","pcall","xpcall","print"}) {
            lua_pushnil(root);lua_setglobal(root,name);
        }
        lua_pushcfunction(root,wait);lua_setglobal(root,"Wait");
    }
    ~Impl() { if(root)lua_close(root); }
    std::string error(lua_State* state) {
        auto text=lua_tostring(state,-1);return text?text:"Native script VM error";
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
    need(!name.empty()&&name.size()<=128&&name.find('\0')==std::string::npos&&name!="Wait"&&bool(function),"Invalid native script function binding");
    Impl::Binding binding{&name};
    if(lua_cpcall(impl->root,Impl::prepareBinding,&binding)) {
        auto message=impl->error(impl->root);lua_pop(impl->root,1);throw std::runtime_error(message);
    }
    impl->functions[name]=std::move(function);
}
uint64_t ScriptVm::start(std::span<const uint8_t> code,const std::string& name) {
    need(!impl->ticking,"Queue new script tasks after the native callback completes");
    need(!code.empty()&&code.size()<=16*1024*1024&&impl->work.size()<4096,"Native script task budget exceeded");
    auto task=std::make_unique<Impl::Work>();task->state.id=impl->next++;task->state.wakeTime=impl->clock;
    Impl::Creation creation{task.get(),code,&name};
    if(lua_cpcall(impl->root,Impl::prepareTask,&creation)) {
        task->state.status=ScriptStatus::Failed;task->state.error=impl->error(impl->root);lua_pop(impl->root,1);
    }
    auto id=task->state.id;impl->work.push_back(std::move(task));return id;
}
void ScriptVm::tick(double seconds) {
    need(!impl->ticking,"Native script scheduler is not reentrant");
    need(std::isfinite(seconds)&&seconds>=0&&seconds<=1,"Invalid native script time step");
    impl->ticking=true;
    struct Clear { bool& flag;~Clear(){flag=false;} } clear{impl->ticking};
    impl->clock+=seconds;
    for(auto& task:impl->work) {
        if((task->state.status!=ScriptStatus::Queued&&task->state.status!=ScriptStatus::Waiting)||task->state.wakeTime>impl->clock+1e-12)continue;
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
