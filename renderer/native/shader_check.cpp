// Compile original, user-owned HLSL with the native Windows D3D compiler.
// This tool creates bytecode only; it does not create a graphics device or draw.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
constexpr size_t kMaxSource = 4 * 1024 * 1024;

static bool NoLinks(const fs::path& path) {
    fs::path partial;
    for (const auto& component : fs::absolute(path)) {
        partial /= component;
        const DWORD attributes = GetFileAttributesW(partial.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return true;
}

static bool ReadSource(const fs::path& path, std::vector<char>& bytes) {
    if (!NoLinks(path)) return false;
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > kMaxSource) return false;
    bytes.resize(static_cast<size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    return bool(stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())));
}

// Includes are bounded to the original source directory and its descendants.
class SourceIncludes final : public ID3DInclude {
    struct OpenFile { std::vector<char> bytes; fs::path directory; };
    fs::path root_;
    std::map<const void*, std::unique_ptr<OpenFile>> files_;
    size_t total_ = 0;
public:
    explicit SourceIncludes(fs::path root) : root_(std::move(root)) {}
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID parent,
                                   LPCVOID* data, UINT* size) noexcept override {
        try {
        if (!name || !data || !size || files_.size() >= 256) return E_INVALIDARG;
        for (const char* character = name; *character; ++character)
            if (static_cast<unsigned char>(*character) < 32 || *character == ':') return E_ACCESSDENIED;
        const fs::path relative(name);
        if (relative.is_absolute() || relative.has_root_name()) return E_ACCESSDENIED;
        for (const auto& part : relative) if (part == L".." || part == L".") return E_ACCESSDENIED;
        fs::path directory = root_;
        if (auto found = files_.find(parent); found != files_.end()) directory = found->second->directory;
        auto opened = std::make_unique<OpenFile>();
        const fs::path path = directory / relative;
        if (!ReadSource(path, opened->bytes) || opened->bytes.size() > 32 * 1024 * 1024 - total_) return E_FAIL;
        total_ += opened->bytes.size();
        opened->directory = path.parent_path();
        *data = opened->bytes.data();
        *size = static_cast<UINT>(opened->bytes.size());
        files_.emplace(*data, std::move(opened));
        return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID data) noexcept override {
        return files_.erase(data) == 1 ? S_OK : E_INVALIDARG;
    }
};

static bool Identifier(const std::string& text) {
    if (text.empty() || !((text[0] >= 'A' && text[0] <= 'Z') ||
                         (text[0] >= 'a' && text[0] <= 'z') || text[0] == '_')) return false;
    for (char c : text) if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

static std::string Ascii(const wchar_t* text) {
    std::string result;
    for (; *text; ++text) {
        if (*text < 32 || *text > 126) return {};
        result += static_cast<char>(*text);
    }
    return result;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "Usage: NativeShaderCompile SOURCE ENTRY PROFILE OUTPUT [--define NAME[=VALUE]]\n");
        return 2;
    }
    const fs::path source = fs::absolute(argv[1]);
    const fs::path output = fs::absolute(argv[4]);
    const std::string entry = Ascii(argv[2]), profile = Ascii(argv[3]);
    if (!Identifier(entry) || (profile != "vs_3_0" && profile != "ps_3_0") || !NoLinks(output)) {
        std::fprintf(stderr, "Invalid entry/profile or output path. Only native Shader Model 3 targets are supported.\n");
        return 2;
    }
    std::vector<std::pair<std::string, std::string>> definitions;
    for (int index = 5; index < argc; index += 2) {
        if (std::wstring(argv[index]) != L"--define" || index + 1 >= argc || definitions.size() >= 128) return 2;
        std::string definition = Ascii(argv[index + 1]);
        const size_t equal = definition.find('=');
        std::string name = definition.substr(0, equal);
        std::string value = equal == std::string::npos ? "1" : definition.substr(equal + 1);
        if (!Identifier(name) || value.empty() || value.size() > 256) return 2;
        for (const auto& existing : definitions) if (existing.first == name) return 2;
        definitions.emplace_back(std::move(name), std::move(value));
    }
    std::vector<D3D_SHADER_MACRO> macros;
    for (const auto& definition : definitions) macros.push_back({definition.first.c_str(), definition.second.c_str()});
    macros.push_back({nullptr, nullptr});
    std::vector<char> input;
    if (!ReadSource(source, input)) {
        std::fprintf(stderr, "Cannot read source (empty, excessive size, missing file or reparse point).\n");
        return 2;
    }
    SourceIncludes includes(source.parent_path());
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    // Default legacy syntax and normal validation; no unverified packing override.
    const HRESULT result = D3DCompile(input.data(), input.size(), source.string().c_str(), macros.data(),
                                      &includes, entry.c_str(), profile.c_str(), 0, 0, &code, &errors);
    if (errors) {
        const size_t length = errors->GetBufferSize();
        std::fwrite(errors->GetBufferPointer(), 1, length > kMaxSource ? kMaxSource : length, stderr);
        errors->Release();
    }
    if (FAILED(result) || !code) {
        std::fprintf(stderr, "D3DCompile failed: HRESULT 0x%08lX\n", static_cast<unsigned long>(result));
        if (code) code->Release();
        return 3;
    }
    const HANDLE handle = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "Cannot create bytecode output exclusively: Windows error %lu\n", GetLastError());
        code->Release();
        return 4;
    }
    DWORD written = 0;
    const size_t length = code->GetBufferSize();
    const bool saved = length <= MAXDWORD && WriteFile(handle, code->GetBufferPointer(),
        static_cast<DWORD>(length), &written, nullptr) && written == length && FlushFileBuffers(handle);
    CloseHandle(handle);
    code->Release();
    if (!saved) {
        std::fprintf(stderr, "Failed to write complete bytecode output.\n");
        return 4;
    }
    std::printf("{\"entry\":\"%s\",\"profile\":\"%s\",\"compiled_bytes\":%zu,\"compiler\":\"Windows D3DCompile\",\"flags\":0}\n",
        entry.c_str(), profile.c_str(), length);
    return 0;
}
