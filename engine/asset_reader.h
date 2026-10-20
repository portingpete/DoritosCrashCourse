#pragma once
#include "model.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace Doritos::Engine {
inline void need(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
class AssetReader {
    std::vector<uint8_t> bytes;
    size_t position=0;
public:
    explicit AssetReader(const std::filesystem::path& path) {
        const auto size=std::filesystem::file_size(path);
        need(size>=8 && size<=64*1024*1024,"Native asset size outside budget");
        std::ifstream stream(path,std::ios::binary); bytes.resize(static_cast<size_t>(size));
        need(bool(stream.read(reinterpret_cast<char*>(bytes.data()),bytes.size())),"Cannot read native engine asset");
    }
    template<class T> T read() {
        need(sizeof(T)<=bytes.size()-position,"Truncated native engine asset");
        T value; std::memcpy(&value,bytes.data()+position,sizeof(T));position+=sizeof(T);return value;
    }
    uint32_t count(uint32_t maximum) { auto v=read<uint32_t>();need(v<=maximum,"Native asset count outside budget");return v; }
    float number() { auto v=read<float>();need(std::isfinite(v),"Nonfinite native asset value");return v; }
    XMFLOAT3 vector() { return {number(),number(),number()}; }
    XMFLOAT4 quaternion() {
        XMFLOAT4 v{number(),number(),number(),number()};
        need(std::abs(v.x*v.x+v.y*v.y+v.z*v.z+v.w*v.w-1)<.01f,"Non-unit native animation quaternion");return v;
    }
    Transform transform() { return {vector(),quaternion()}; }
    std::string string() {
        auto size=count(4096);need(size<=bytes.size()-position,"Truncated native asset string");
        std::string result(reinterpret_cast<char*>(bytes.data()+position),size);position+=size;
        need(result.find('\0')==std::string::npos,"Embedded NUL in native asset name");return result;
    }
    void end() { need(position==bytes.size(),"Trailing native asset data"); }
};
}
