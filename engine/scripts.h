#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Doritos::Engine {
struct ScriptChunk {
    std::string source,type,actor,field;
    uint32_t ordinal=0,index=0;
    std::vector<uint8_t> code;
};
std::vector<ScriptChunk> loadScripts(const std::filesystem::path& path);
using ScriptValue=std::variant<std::monostate,bool,double,std::string>;
using ScriptFunction=std::function<ScriptValue(std::span<const ScriptValue>)>;
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
    uint64_t start(std::span<const uint8_t> code,const std::string& name);
    void tick(double seconds);
    std::vector<ScriptTask> tasks() const;
    void clearFinished();
    size_t memoryUsed() const;
};
}
