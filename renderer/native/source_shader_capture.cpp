#include "source_shader_capture.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <sstream>

namespace Doritos::Graphics {
namespace {
bool enabled() {
    static const bool value = [] {
        const DWORD saved = GetLastError();
        wchar_t setting[2]{};
        const bool result = GetEnvironmentVariableW(L"DORITOS_SHADER_SOURCE_CAPTURE",setting,2) == 1 && setting[0] == L'1';
        SetLastError(saved);
        return result;
    }();
    return value;
}
void checkDirectoryAncestors(const std::filesystem::path& path) {
    for (auto cursor = path; !cursor.empty();) {
        const DWORD flags = GetFileAttributesW(cursor.c_str());
        if (flags == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                throw std::runtime_error("Cannot inspect source shader capture ancestors");
        } else if ((flags & FILE_ATTRIBUTE_REPARSE_POINT) || !(flags & FILE_ATTRIBUTE_DIRECTORY)) {
            throw std::runtime_error("Source shader capture refuses linked/non-directory ancestors");
        }
        const auto parent = cursor.parent_path();
        if (parent == cursor) break;
        cursor = parent;
    }
}
std::string sha256(std::span<const uint8_t> bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    struct Cleanup {
        BCRYPT_ALG_HANDLE& algorithm;
        BCRYPT_HASH_HANDLE& hash;
        ~Cleanup() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
    } cleanup{algorithm,hash};
    std::array<uint8_t,32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0) < 0 ||
        BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0) < 0 ||
        BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0) < 0 ||
        BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0) < 0)
        throw std::runtime_error("Cannot hash original source shader capture");
    constexpr char hex[] = "0123456789abcdef";
    std::string value;
    for (uint8_t byte : digest) { value += hex[byte >> 4]; value += hex[byte & 15]; }
    return value;
}
void writeNew(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    checkDirectoryAncestors(path.parent_path());
    HANDLE file = CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create unique source shader capture");
    struct Cleanup { HANDLE file; ~Cleanup() { CloseHandle(file); } } cleanup{file};
    DWORD written{};
    if (!WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) || written != bytes.size() || !FlushFileBuffers(file))
        throw std::runtime_error("Cannot finish original source shader capture");
}
}
void captureOriginalSourceShader(const ShaderSelection& selection, ShaderStage stage,
    uint32_t master, uint32_t variantAddress, std::span<const uint8_t> variant, std::span<const uint8_t> code) {
    if (!enabled()) return;
    const ShaderVariant actual = originalSourceShader(selection,stage,master,variant,code);
    const auto parent = std::filesystem::absolute(L"build_native/source_shader_capture").lexically_normal();
    checkDirectoryAncestors(parent);
    std::filesystem::create_directories(parent);
    checkDirectoryAncestors(parent);
    const auto folder = parent/(std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    if (!std::filesystem::create_directory(folder)) throw std::runtime_error("Source shader capture folder is not unique");
    checkDirectoryAncestors(folder);
    // Literal basenames; the title's shader identifier never becomes a file path.
    writeNew(folder/L"original.xenon.bin",code);
    std::ostringstream report;
    report << "{\"schema\":1,\"provenance\":\"original_source_loader\","
        "\"source_xex_sha256\":\"6c4fa5a12ef2ad51db4809d5d654b487300a1ace06c2cdf6cb14258fd273dfc1\","
        "\"package_sha256\":\"0819b4e9f90695b70e083690e98ac6f76f0df0cf2bbb817f196316c930a54fc3\","
        "\"image_sha256\":\"e2be3b416bffca18e59c34cbc6d20bc6ff14490a0ec3b1c30e4a0594ce79c8d7\","
        "\"loader_return_address\":" << (stage == ShaderStage::Vertex ? 0x82387638u : 0x82387628u) << ','
        << "\"compiler_address\":" << 0x82648DA8u << ",\"master_address\":" << master
        << ",\"variant_address\":" << variantAddress << ",\"variant_owner_address\":" << master
        << ",\"shader_name\":\"" << selection.name << "\",\"stage\":\""
        << (stage == ShaderStage::Vertex ? "vertex" : "pixel") << "\",\"key\":["
        << selection.key[0] << ',' << selection.key[1] << ',' << selection.key[2] << "],\"parameters\":[";
    bool first = true;
    for (const auto& parameter : actual.parameters) {
        if (!first) report << ',';
        first = false;
        report << '[' << parameter.id << ',' << parameter.firstRegister << ',' << parameter.registerCount << ',' << parameter.reserved << ']';
    }
    report << "],\"bytecode_file\":\"original.xenon.bin\",\"bytecode_bytes\":" << code.size()
        << ",\"bytecode_sha256\":\"" << sha256(code) << "\"}\n";
    const std::string json = report.str();
    writeNew(folder/L"capture.json",{reinterpret_cast<const uint8_t*>(json.data()),json.size()});
    std::fprintf(stderr,"[ORIGINAL SOURCE SHADER CAPTURE] %s [%u,%u,%u] report=%s\n",
        selection.name.c_str(),selection.key[0],selection.key[1],selection.key[2],(folder/L"capture.json").string().c_str());
    std::fflush(stderr);
}
}
