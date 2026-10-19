#include "renderer/native/title_texture.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>

using namespace Doritos::Graphics;
namespace {
unsigned checks = 0;
void require(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
void rejects(const std::function<void()>& action, const char* label) {
    ++checks;
    try { action(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error(label);
}
void write32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes.at(offset+index) = uint8_t(value >> (index*8));
}
std::vector<uint8_t> fixture(bool cube = false, uint32_t levels = 1) {
    std::vector<uint8_t> bytes(128);
    write32(bytes,0,0x20534444); write32(bytes,4,124); write32(bytes,8,0x81007);
    write32(bytes,12,4); write32(bytes,16,4); write32(bytes,20,16); write32(bytes,28,levels);
    write32(bytes,76,32); write32(bytes,80,4); write32(bytes,84,0x35545844);
    write32(bytes,108,0x1000); write32(bytes,112,cube ? 0xfe00 : 0);
    for (uint32_t face = 0; face < (cube ? 6u : 1u); ++face)
        for (uint32_t level = 0; level < levels; ++level)
            bytes.insert(bytes.end(),16,uint8_t(face * 16 + level));
    return bytes;
}
}
int main(int argc, char** argv) {
    try {
        auto color = decodeTitleTextureFormat(0x18280186);
        require(color.native == D3DFMT_A8R8G8B8 && color.tiled && color.endian == 2, "Original alpha format fields");
        require(decodeTitleTextureFormat(0x28280106).native == D3DFMT_X8R8G8B8, "Original RTT opaque format");
        require(decodeTitleTextureFormat(0x2d200196).native == D3DFMT_D24S8, "Original title depth format");
        rejects([] { decodeTitleTextureFormat(0x1a220197); }, "Unverified floating-depth format accepted");
        rejects([] { decodeTitleTextureFormat(0); }, "Unknown texture format accepted");
        const std::vector<uint8_t> argb{0x11,0x22,0x33,0x44, 0x55,0x66,0x77,0x88, 0xaa,0xbb,0xcc,0xdd,
            0x99,0x12,0x34,0x56, 0xfe,0xab,0xcd,0xef};
        auto alpha = convertTitleLinearTexture(argb,2,2,12,0x18280086);
        const std::vector<uint8_t> bgra{0x44,0x33,0x22,0x11, 0x88,0x77,0x66,0x55, 0x56,0x34,0x12,0x99, 0xef,0xcd,0xab,0xfe};
        require(alpha.rowPitch == 8 && alpha.bytes == bgra, "Original ARGB row conversion or padding");
        auto opaque = convertTitleLinearTexture(argb,2,2,12,0x28280086);
        for (size_t pixel = 0; pixel < 4; ++pixel) require(opaque.bytes[pixel*4+3] == 255, "Opaque swizzle alpha must be one");
        rejects([&] { convertTitleLinearTexture(argb,2,2,12,0x18280186); }, "Tiled source interpreted as linear pixels");
        rejects([&] { convertTitleLinearTexture(std::span(argb).first(19),2,2,12,0x18280086); }, "Truncated source rows accepted");
        rejects([&] { convertTitleLinearTexture(argb,2,2,4,0x18280086); }, "Short source pitch accepted");
        rejects([&] { convertTitleLinearTexture(argb,8193,1,12,0x18280086); }, "Oversized texture accepted");
        const std::vector<uint8_t> xboxBlock{0xf8,0,0x07,0xe0,0x01,0x23,0x45,0x67};
        auto dxt = convertTitleLinearTexture(xboxBlock,4,4,8,0x1a200052);
        require(dxt.format == D3DFMT_DXT1 && dxt.bytes == std::vector<uint8_t>({0,0xf8,0xe0,0x07,0x23,0x01,0x67,0x45}), "DXT fetch swaps pairs without decoding or modifying blocks");
        const std::vector<uint8_t> rectBytes{0,0,0,0,0,0,0,0,0,0,0,128,0,0,1,0};
        auto rect = decodeTitleTextureRect(rectBytes);
        require(rect.left == 0 && rect.top == 0 && rect.right == 128 && rect.bottom == 256, "Original BE texture rectangle");
        rejects([&] { decodeTitleTextureRect(std::span(rectBytes).first(15)); }, "Truncated rectangle accepted");
        auto bytes = fixture(true,3);
        auto parsed = parseTitleDDS(bytes);
        require(parsed.faces == 6 && parsed.levels == 3 && parsed.images.size() == 18, "DDS cubemap/mip dimensions");
        require(parsed.images[4].face == 1 && parsed.images[4].level == 1 && parsed.images[4].width == 2 && parsed.images[4].bytes == 16 && bytes[parsed.images[4].offset] == 17, "DDS face-major mip range");
        for (size_t size = 0; size < bytes.size(); ++size)
            rejects([&] { parseTitleDDS(std::span(bytes).first(size)); }, "Truncated DDS accepted");
        auto changed = bytes; changed.push_back(0);
        rejects([&] { parseTitleDDS(changed); }, "DDS trailing data accepted");
        changed = bytes; write32(changed,112,0xbe00);
        rejects([&] { parseTitleDDS(changed); }, "Partial cubemap accepted");
        changed = bytes; write32(changed,28,20);
        rejects([&] { parseTitleDDS(changed); }, "Excessive DDS mips accepted");
        changed = bytes; write32(changed,84,0x30315844);
        rejects([&] { parseTitleDDS(changed); }, "Unknown DDS format accepted");
        for (int index = 1; index < argc; ++index) {
            std::ifstream stream(argv[index],std::ios::binary);
            require(bool(stream), "Cannot open supplied stock DDS");
            std::vector<uint8_t> stock((std::istreambuf_iterator<char>(stream)),{});
            auto data = parseTitleDDS(stock);
            require(data.format == D3DFMT_DXT5 && data.levels == 1, "Stock DDS format/mip mismatch");
            require((data.faces == 6 && data.width == 128 && data.height == 128) ||
                (data.faces == 1 && data.width == 256 && data.height == 256), "Stock DDS layout mismatch");
        }
        std::printf("Passed %u native texture checks\n",checks);
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"Texture contract failure: %s\n",error.what()); return 1; }
}
