#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Doritos::Engine {
struct ScriptChunk {
    std::string source,type,actor,field;
    uint32_t ordinal=0,index=0;
    std::vector<uint8_t> code;
};
std::vector<ScriptChunk> loadScripts(const std::filesystem::path& path);
class ScriptObject;
using ScriptObjectPtr=std::shared_ptr<ScriptObject>;
using ScriptValue=std::variant<std::monostate,bool,double,std::string,ScriptObjectPtr>;
using ScriptFunction=std::function<ScriptValue(std::span<const ScriptValue>)>;
// Host objects expose only their implemented native fields and methods. The
// default operations reject unsupported members; a missing member is not nil.
class ScriptObject {
public:
    virtual ~ScriptObject()=default;
    virtual ScriptValue get(std::string_view name) const;
    virtual void set(std::string_view name,const ScriptValue& value);
    virtual bool hasMethod(std::string_view name) const;
    virtual ScriptValue call(std::string_view name,std::span<const ScriptValue> arguments);
};
using ScriptContext=std::map<std::string,ScriptValue,std::less<>>;
enum class ScriptStatus { Queued,Waiting,Complete,Failed };
struct ScriptTask {
    uint64_t id=0;
    ScriptStatus status=ScriptStatus::Queued;
    double wakeTime=0;
    std::string error;
};
class ScriptVm {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit ScriptVm(size_t memoryLimit=32*1024*1024,uint32_t instructionLimit=100000);
    ~ScriptVm();
    ScriptVm(const ScriptVm&)=delete;
    ScriptVm& operator=(const ScriptVm&)=delete;
    void validate(std::span<const uint8_t> code,const std::string& name);
    void bind(const std::string& name,ScriptFunction function);
    void setGlobal(const std::string& name,const ScriptValue& value);
    uint64_t start(std::span<const uint8_t> code,const std::string& name,ScriptContext context={});
    // Valid only while the current native callback/task runs. Shared Lua
    // functions resolve these same task-specific globals on every invocation.
    const ScriptContext* currentContext() const noexcept;
    // Each call advances one scheduler frame, including tick(0). Lua
    // WaitFrames(n), with integer n in [0,2^53-1], counts subsequent calls;
    // WaitFrames(0) yields once.
    // Wait(seconds) continues to use only the accumulated elapsed seconds.
    void tick(double seconds);
    std::vector<ScriptTask> tasks() const;
    void clearFinished();
    size_t memoryUsed() const;
};
}
