#include "asset_inputs.h"
#include "shader_inputs.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace Doritos::Native {
namespace {
struct File {
    HANDLE handle;
    explicit File(const std::filesystem::path& path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open verified renderer input: " + path.string() +
                                     "; run tools/prepare_renderer.py");
    }
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
struct Hash {
    BCRYPT_ALG_HANDLE provider = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Hash() {
        if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("Cannot open renderer SHA-256 provider");
        if (BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(provider, 0);
            throw std::runtime_error("Cannot create renderer SHA-256 hash");
        }
    }
    ~Hash() { if (hash) BCryptDestroyHash(hash); if (provider) BCryptCloseAlgorithmProvider(provider, 0); }
};
std::vector<uint8_t> verifiedFile(const std::filesystem::path& path, uint64_t maximum,
                                  const char* expected, bool collect) {
    File file(path);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.handle, &size) || size.QuadPart <= 0 || uint64_t(size.QuadPart) > maximum)
        throw std::runtime_error("Renderer input has an invalid or excessive size: " + path.string());
    std::vector<uint8_t> output;
    if (collect) output.reserve(size_t(size.QuadPart));
    Hash hash;
    std::array<uint8_t, 64 * 1024> block{};
    uint64_t total = 0;
    for (;;) {
        DWORD bytes = 0;
        if (!ReadFile(file.handle, block.data(), DWORD(block.size()), &bytes, nullptr))
            throw std::runtime_error("Renderer input read failed: " + path.string());
        if (!bytes) break;
        total += bytes;
        if (total > uint64_t(size.QuadPart) || BCryptHashData(hash.hash, block.data(), bytes, 0) < 0)
            throw std::runtime_error("Renderer input changed or failed SHA-256: " + path.string());
        if (collect) output.insert(output.end(), block.begin(), block.begin() + bytes);
    }
    std::array<uint8_t, 32> digest{};
    if (total != uint64_t(size.QuadPart) || BCryptFinishHash(hash.hash, digest.data(), DWORD(digest.size()), 0) < 0)
        throw std::runtime_error("Renderer input was truncated or failed SHA-256: " + path.string());
    std::string hexadecimal;
    for (uint8_t value : digest) {
        hexadecimal += "0123456789abcdef"[value >> 4];
        hexadecimal += "0123456789abcdef"[value & 15];
    }
    if (hexadecimal != expected)
        throw std::runtime_error("Renderer input differs from the supported original: " + path.string());
    return output;
}
std::vector<uint8_t> verifiedExtension(const std::filesystem::path& shaderDirectory,
                                     const wchar_t* configuredPath, const char* expected) {
    const std::filesystem::path developmentPath(configuredPath);
    const auto portablePath = shaderDirectory / developmentPath.filename();
    // A packaged cache takes precedence. An invalid local copy must fail its
    // hash check, rather than silently falling back to the development tree.
    return verifiedFile(std::filesystem::exists(portablePath) ? portablePath : developmentPath,
                        4 * 1024 * 1024, expected, true);
}
}
TitleRendererInputs verifiedRendererInputs(const std::filesystem::path& game,
                                          const std::filesystem::path& shaderDirectory,
                                          bool retainPackage) {
    LARGE_INTEGER started{}, finished{}, frequency{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    auto package = verifiedFile(game / "data.pak", 512 * 1024 * 1024,
                 "0819b4e9f90695b70e083690e98ac6f76f0df0cf2bbb817f196316c930a54fc3", retainPackage);
    QueryPerformanceCounter(&finished);
    TitleRendererInputs result;
    if (retainPackage) result.package = std::make_shared<const std::vector<uint8_t>>(std::move(package));
    std::fprintf(stderr, "[PACKAGE] mode=%s resident_bytes=%zu verify_ms=%.3f sha256=0819b4e9f90695b70e083690e98ac6f76f0df0cf2bbb817f196316c930a54fc3\n",
        retainPackage ? "ram" : "disk", result.package ? result.package->size() : 0,
        1000.0 * double(finished.QuadPart - started.QuadPart) / double(frequency.QuadPart));
    result.windowsCache = verifiedFile(shaderDirectory / "precache_win32_3.bsh", 4 * 1024 * 1024,
        "3ab1cbbb7a7db7a328e0596695556eb2a9c3ebc1e097a65bc5c45afe6c5c0c2d", true);
    result.xenonCache = verifiedFile(shaderDirectory / "precache_xenon.bsh", 4 * 1024 * 1024,
        "e0b23484c4b87727b98f6379211bba06e8818c32dcc58cf768b767cb279a5b27", true);
    if (ShaderInputs::path[0]) {
        result.nativeExtension = verifiedExtension(shaderDirectory, ShaderInputs::path, ShaderInputs::sha256);
        std::fprintf(stderr, "[ASSETS] offline native shader extension verified; bytes=%zu\n", result.nativeExtension.size());
    }
    if (ShaderInputs::sourcePath[0]) {
        result.xenonSourceExtension = verifiedExtension(shaderDirectory,ShaderInputs::sourcePath,ShaderInputs::sourceSha256);
        std::fprintf(stderr,"[ASSETS] original source-compiled Xenon shader extension verified; bytes=%zu\n",result.xenonSourceExtension.size());
    }
    std::fprintf(stderr, "[ASSETS] supported original package and both native renderer shader inputs verified\n");
    return result;
}
}
