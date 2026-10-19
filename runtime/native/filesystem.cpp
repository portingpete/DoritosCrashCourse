// Native NT file contracts adapted from The Darkness Recomp's GPLv3 native
// filesystem bridge. Xbox layouts are translated explicitly; no console OS
// runtime is involved. Original Doritos callsites establish the six-argument
// NtOpenFile ABI and ANSI OBJECT_ATTRIBUTES/IO_STATUS_BLOCK field offsets.
#include "runtime.h"
#include "filesystem.h"
#include "kernel_objects.h"
#include <winternl.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <malloc.h>
#include <memory>
#include <string>
#include <string_view>

namespace Doritos::Native {
struct ResidentPackage {
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    std::atomic<uint64_t> readCalls{0}, readBytes{0};
};
namespace {
constexpr uint32_t invalidParameter = 0xc000000d, invalidHandle = 0xc0000008;
constexpr uint32_t accessViolation = 0xc0000005, nameInvalid = 0xc0000033;
constexpr uint32_t pathSyntax = 0xc000003b, noDevice = 0xc000000e;
constexpr uint32_t writeProtected = 0xc00000a2, pending = 0x103;
constexpr uint32_t lengthMismatch = 0xc0000004, noMemory = 0xc0000017;
struct Mount { std::filesystem::path path; bool writable = false; };
std::mutex mountMutex;
std::map<std::string, Mount> mounts;
std::filesystem::path assetGameRoot, assetOverrideRoot;
std::shared_ptr<ResidentPackage> residentPackage;
bool traceFiles() {
    static const bool enabled = [] {
        const char* value = std::getenv("DORITOS_FILE_TRACE");
        return value && value[0] == '1';
    }();
    return enabled;
}

template<class Function> Function ntdll(const char* name) {
    const auto function = reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name));
    if (!function) fail("Required native Windows filesystem contract unavailable");
    return function;
}
std::string lower(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}
bool guestSpan(uint32_t address, uint32_t bytes, bool writable) {
    if (!memory || !address || uint64_t(address) + bytes > 0x100000000ull) return false;
    if (!bytes) return true;
    if (!memory->valid(address, bytes)) return false;
    uint64_t cursor = address, end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(memory->base() + cursor, &info, sizeof(info))) return false;
        const DWORD protection = info.Protect & 0xff;
        const bool canWrite = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                              protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (writable ? !canWrite : !(canWrite || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ)) return false;
        const uint64_t next = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize - memory->base();
        if (next <= cursor) return false;
        cursor = std::min(next, end);
    }
    return true;
}
uint32_t readAnsi(uint32_t descriptor, std::string& text) {
    if (!descriptor || !guestSpan(descriptor, 8, false)) return accessViolation;
    const uint32_t lengths = memory->read32(descriptor), buffer = memory->read32(descriptor + 4);
    const uint32_t length = lengths >> 16;
    if (length > (lengths & 0xffff) || length > 4096 || (length && !buffer)) return nameInvalid;
    if (length && !guestSpan(buffer, length, false)) return accessViolation;
    text.assign(length ? reinterpret_cast<const char*>(memory->base() + buffer) : "", length);
    if (text.find('\0') != std::string::npos || std::any_of(text.begin(), text.end(), [](unsigned char c) { return c >= 128; })) return nameInvalid;
    std::replace(text.begin(), text.end(), '/', '\\');
    return 0;
}
void ioStatus(uint32_t pointer, uint32_t status, uint32_t information) {
    memory->write32(pointer, status);
    memory->write32(pointer + 4, information);
}
uint32_t statusPointer(uint32_t pointer) {
    if (!pointer) return invalidParameter;
    return guestSpan(pointer, 8, true) ? 0 : accessViolation;
}
std::string aliasKey(std::string text) {
    text = lower(std::move(text));
    if (text.starts_with("\\??\\")) text.erase(0, 4);
    while (!text.empty() && text.back() == '\\') text.pop_back();
    return text;
}
bool withinRoot(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto rootString = root.wstring(), pathString = path.wstring();
    return !rootString.empty() && pathString.size() >= rootString.size() &&
        !_wcsnicmp(pathString.c_str(), rootString.c_str(), rootString.size()) &&
        (pathString.size() == rootString.size() || rootString.back() == L'\\' || pathString[rootString.size()] == L'\\');
}
bool allowedAssetOverride(const std::filesystem::path& relative) {
    const auto name = lower(relative.generic_string());
    // Exact generated declarations, owned CJ files and the genuine source
    // cache addition. Executable, package and saves retain original mounts.
    return name == "data/templates/guigroup.wbr" || name == "data/templates/scripts.wbr" ||
        name == "data/templates/guilabel.wbr" || name == "data/templates/unistrings.wbr" ||
        name == "data/templates/bonedmodel.wbr" || name == "data/templates/shader.wbr" ||
        name == "data/templates/texture.wbr" ||
        name == "data/art/_avatar/cj_native.cfg" || name == "data/art/_avatar/cj_native.cmf" ||
        name == "data/art/_avatar/cj_native.mat.txt" || name == "data/art/_avatar/cj_native_0.png" ||
        name == "data/art/_avatar/cj_native_1.png" || name == "data/art/_avatar/cj_native_2.png" ||
        name == "data/art/_avatar/cj_native_3.png" || name == "data/art/dxshaders/precache_xenon.bsh";
}
struct PathResult {
    std::filesystem::path path, logicalPath;
    bool writable = false, menuOverrideDirectory = false;
    uint32_t status = 0;
};
PathResult resolveName(std::string name, uint32_t root) {
    PathResult result;
    std::replace(name.begin(), name.end(), '/', '\\');
    if (name.starts_with("\\??\\")) name.erase(0, 4);
    const std::string lowered = lower(name);
    Mount mount;
    std::filesystem::path originalAssets, overrideAssets;
    size_t prefix = 0;
    bool found = false;
    {
        std::lock_guard lock(mountMutex);
        // Longest prefix avoids a volume alias consuming a nested alias.
        for (const auto& [key, candidate] : mounts) {
            if (!lowered.starts_with(key) || key.size() < prefix) continue;
            if (lowered.size() != key.size() && key.back() != ':' && lowered[key.size()] != '\\') continue;
            mount = candidate; prefix = key.size(); found = true;
        }
        originalAssets = assetGameRoot;
        overrideAssets = assetOverrideRoot;
    }
    if (!found && root && root != 0xfffffffd) {
        auto object = lookupObject(root);
        if (!object || object->kind != ObjectKind::Directory || !object->fileState) { result.status = invalidHandle; return result; }
        if (!name.empty() && (name.front() == '\\' || name.find(':') != std::string::npos)) { result.status = pathSyntax; return result; }
        mount = {object->fileState->logicalPath.empty() ? object->fileState->path : object->fileState->logicalPath,
                 object->fileState->writable}; found = true;
    }
    if (!found) {
        result.status = !name.empty() && name.front() != '\\' && name.find(':') == std::string::npos ? pathSyntax : noDevice;
        return result;
    }
    while (prefix < name.size() && name[prefix] == '\\') ++prefix;
    const std::string relative = name.substr(prefix);
    if (relative.find(':') != std::string::npos) { result.status = nameInvalid; return result; }
    for (const auto& component : std::filesystem::path(relative)) {
        if (component == "..") { result.status = nameInvalid; return result; }
    }
    std::error_code error;
    const auto canonicalRoot = std::filesystem::weakly_canonical(mount.path, error);
    if (error) { result.status = nameInvalid; return result; }
    result.path = std::filesystem::weakly_canonical(canonicalRoot / relative, error).make_preferred();
    if (error) { result.status = nameInvalid; return result; }
    if (!withinRoot(canonicalRoot, result.path)) {
        result.status = nameInvalid; return result;
    }
    result.writable = mount.writable;
    result.logicalPath = result.path;
    if (!mount.writable && !overrideAssets.empty() && withinRoot(originalAssets, result.path)) {
        const auto assetRelative = result.path.lexically_relative(originalAssets);
        auto assetName = lower(assetRelative.generic_string());
        while (!assetName.empty() && assetName.back() == '/') assetName.pop_back();
        const bool templateDirectory = assetName == "data/templates";
        if (templateDirectory || allowedAssetOverride(assetRelative)) {
            const auto candidate = std::filesystem::weakly_canonical(overrideAssets / assetRelative, error).make_preferred();
            if (error || !withinRoot(overrideAssets, candidate)) { result.status = nameInvalid; return result; }
            const bool exists = std::filesystem::exists(candidate, error);
            if (error) { result.status = nameInvalid; return result; }
            if (exists) {
                const bool expectedType = templateDirectory ? std::filesystem::is_directory(candidate, error) :
                    std::filesystem::is_regular_file(candidate, error);
                if (!expectedType || error) { result.status = nameInvalid; return result; }
                result.path = candidate;
                result.menuOverrideDirectory = templateDirectory;
            }
        }
    }
    return result;
}
PathResult resolve(uint32_t attributes) {
    PathResult result;
    if (!attributes) { result.status = invalidParameter; return result; }
    if (!guestSpan(attributes, 12, false)) { result.status = accessViolation; return result; }
    std::string name;
    result.status = readAnsi(memory->read32(attributes + 4), name);
    return result.status ? result : resolveName(std::move(name), memory->read32(attributes));
}
std::wstring nativeName(const std::filesystem::path& path) {
    const auto text = path.wstring();
    return text.starts_with(L"\\\\") ? L"\\??\\UNC\\" + text.substr(2) : L"\\??\\" + text;
}
struct NativeAttributes {
    std::wstring path;
    UNICODE_STRING name{};
    OBJECT_ATTRIBUTES attributes{};
    NativeAttributes(const std::filesystem::path& resolved, uint32_t flags) : path(nativeName(resolved)) {
        if (path.size() > 32767) fail("Native file path exceeds NT counted string bound");
        name = {USHORT(path.size() * 2), USHORT(path.size() * 2), path.data()};
        InitializeObjectAttributes(&attributes, &name, flags & 0x40, nullptr, nullptr);
    }
};
uint32_t create(uint32_t output, uint32_t access, uint32_t attributes, uint32_t ios,
                uint32_t allocationPointer, uint32_t fileAttributes, uint32_t sharing,
                uint32_t disposition, uint32_t options) {
    const uint32_t pointerStatus = statusPointer(ios);
    if (pointerStatus) return pointerStatus;
    if (!output) { ioStatus(ios, invalidParameter, 0); return invalidParameter; }
    if (!guestSpan(output, 4, true) || (allocationPointer && !guestSpan(allocationPointer, 8, false))) {
        ioStatus(ios, accessViolation, 0); return accessViolation;
    }
    auto resolved = resolve(attributes);
    uint32_t status = resolved.status;
    constexpr uint32_t writeAccess = GENERIC_WRITE | GENERIC_ALL | DELETE | FILE_WRITE_DATA | FILE_APPEND_DATA |
        FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
    if (!status && !resolved.writable && ((access & writeAccess) || disposition != 1 || (options & 0x1000))) status = writeProtected;
    IO_STATUS_BLOCK hostIo{};
    HANDLE host = nullptr;
    if (!status) {
        NativeAttributes attrs(resolved.path, memory->read32(attributes + 8));
        LARGE_INTEGER allocation{};
        if (allocationPointer) allocation.QuadPart = int64_t(memory->read64(allocationPointer));
        using Create = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
            PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        status = uint32_t(ntdll<Create>("NtCreateFile")(&host, access, &attrs.attributes, &hostIo,
            allocationPointer ? &allocation : nullptr, fileAttributes, sharing, disposition, options, nullptr, 0));
        if (status == pending) {
            // Keep the native stack IOSB alive through a genuine pending open.
            // Original Doritos opens request SYNCHRONIZE; an unjoinable native
            // request cannot safely unwind while Windows still owns this IOSB.
            if (!host || WaitForSingleObject(host, INFINITE) != WAIT_OBJECT_0 || uint32_t(hostIo.Status) == pending) {
                fprintf(stderr, "[FILE STOP] Pending native open cannot be joined safely.\n"); fflush(stderr); ExitProcess(1);
            }
            status = uint32_t(hostIo.Status);
        }
        if (int32_t(status) >= 0) {
            auto state = std::make_shared<NativeFileState>();
            state->path = resolved.path; state->logicalPath = resolved.logicalPath;
            state->writable = resolved.writable; state->openOptions = options;
            state->menuOverrideDirectory = resolved.menuOverrideDirectory;
            FILE_STANDARD_INFO info{};
            const bool directory = GetFileInformationByHandleEx(host, FileStandardInfo, &info, sizeof(info)) && info.Directory;
            if (!directory && !resolved.writable && (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA))) {
                std::lock_guard lock(mountMutex);
                if (residentPackage && !_wcsicmp(resolved.path.c_str(), (assetGameRoot / "data.pak").c_str()))
                    state->package = residentPackage;
            }
            uint32_t id = registerObject(host, directory ? ObjectKind::Directory : ObjectKind::File);
            lookupObject(id)->fileState = std::move(state);
            memory->write32(output, id);
        }
    }
    ioStatus(ios, status, int32_t(status) >= 0 ? uint32_t(hostIo.Information) : 0);
    if (traceFiles() || int32_t(status) < 0)
        fprintf(stderr, "[FILE] open '%ls' access=0x%08X options=0x%08X status=0x%08X\n", resolved.path.c_str(), access, options, status);
    return status;
}
std::shared_ptr<KernelObject> fileObject(uint32_t handle) {
    auto file = lookupObject(handle);
    return file && (file->kind == ObjectKind::File || file->kind == ObjectKind::Directory) && file->fileState ? file : nullptr;
}
struct CompletionEvent {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~CompletionEvent() { if (handle) CloseHandle(handle); }
};
uint32_t finish(uint32_t status, IO_STATUS_BLOCK& io, HANDLE completion) {
    if (status == pending) {
        if (WaitForSingleObject(completion, INFINITE) != WAIT_OBJECT_0 || uint32_t(io.Status) == pending)
            fail("Native filesystem request did not complete");
        status = uint32_t(io.Status);
    }
    return status;
}
bool completionEvent(uint32_t handle, std::shared_ptr<KernelObject>& event) {
    if (!handle) return true;
    event = lookupObject(handle);
    return event && event->kind == ObjectKind::Event;
}
void rejectApc(uint32_t routine) {
    if (routine & ~1u) fail("Guest file APC completion requires a native guest APC queue");
}
uint32_t transfer(PPCContext& ctx, uint8_t* base, bool write) {
    const uint32_t ios = ctx.r7.u32;
    uint32_t status = statusPointer(ios);
    if (status) return status;
    auto file = fileObject(ctx.r3.u32);
    std::shared_ptr<KernelObject> event;
    if (!file || !completionEvent(ctx.r4.u32, event)) { ioStatus(ios, invalidHandle, 0); return invalidHandle; }
    if (write && !file->fileState->writable) { ioStatus(ios, writeProtected, 0); return writeProtected; }
    const uint32_t buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPointer = ctx.r10.u32;
    if (!buffer && length) { ioStatus(ios, invalidParameter, 0); return invalidParameter; }
    if ((length && !guestSpan(buffer, length, !write)) || (offsetPointer && !guestSpan(offsetPointer, 8, false))) {
        ioStatus(ios, accessViolation, 0); return accessViolation;
    }
    rejectApc(ctx.r5.u32);
    LARGE_INTEGER offset{};
    if (offsetPointer) offset.QuadPart = int64_t(memory->read64(offsetPointer));
    const bool unbuffered = (file->fileState->openOptions & 8) != 0;
    if (unbuffered && length && ((length & 511) || (offsetPointer && offset.QuadPart >= 0 && (uint64_t(offset.QuadPart) & 511)))) {
        ioStatus(ios, invalidParameter, 0); return invalidParameter;
    }
    std::lock_guard lock(file->fileState->ioMutex);
    if (!write && file->fileState->package) {
        auto& state = *file->fileState;
        const bool synchronous = (state.openOptions & 0x30) != 0;
        const bool implicit = !offsetPointer || offset.QuadPart == -2;
        const auto& bytes = *state.package->bytes;
        uint64_t position = implicit ? state.packagePosition : uint64_t(offset.QuadPart);
        uint32_t copied = 0;
        if ((implicit && !synchronous) || (!implicit && offset.QuadPart < 0) ||
            (unbuffered && ((position & 511) || (offsetPointer && offset.QuadPart < 0)))) status = invalidParameter;
        else if (length && position >= bytes.size()) {
            // Noncached synchronous Windows reads retain the requested offset
            // even when the request completes at EOF; buffered reads do not.
            if (synchronous && unbuffered) state.packagePosition = position;
            status = 0xc0000011;
        }
        else {
            copied = uint32_t(std::min<uint64_t>(length, position < bytes.size() ? bytes.size() - position : 0));
            if (copied) memcpy(base + buffer, bytes.data() + size_t(position), copied);
            if (synchronous && length) state.packagePosition = position + copied;
            status = 0;
        }
        state.package->readCalls.fetch_add(1, std::memory_order_relaxed);
        state.package->readBytes.fetch_add(copied, std::memory_order_relaxed);
        ioStatus(ios, status, copied);
        if (event && status != invalidParameter) SetEvent(event->handle);
        if (traceFiles() || int32_t(status) < 0)
            fprintf(stderr, "[FILE] read-ram '%ls' bytes=%u status=0x%08X\n", state.path.c_str(), copied, status);
        return status;
    }
    CompletionEvent completion;
    if (!completion.handle) { ioStatus(ios, noMemory, 0); return noMemory; }
    std::unique_ptr<void, decltype(&_aligned_free)> aligned(nullptr, _aligned_free);
    void* data = buffer ? base + buffer : nullptr;
    if (unbuffered && length) {
        aligned.reset(_aligned_malloc(length, 4096));
        if (!aligned) { ioStatus(ios, noMemory, 0); return noMemory; }
        data = aligned.get();
        if (write) memcpy(data, base + buffer, length);
    }
    IO_STATUS_BLOCK io{};
    using Transfer = NTSTATUS (NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
    status = uint32_t(ntdll<Transfer>(write ? "NtWriteFile" : "NtReadFile")(file->handle, completion.handle,
        nullptr, nullptr, &io, data, length, offsetPointer ? &offset : nullptr, nullptr));
    status = finish(status, io, completion.handle);
    if (io.Information > length) fail("Native filesystem transfer exceeded guest buffer");
    if (aligned && !write && io.Information) memcpy(base + buffer, aligned.get(), size_t(io.Information));
    ioStatus(ios, status, uint32_t(io.Information));
    if (event && WaitForSingleObject(completion.handle, 0) == WAIT_OBJECT_0) SetEvent(event->handle);
    if (traceFiles() || int32_t(status) < 0)
        fprintf(stderr, "[FILE] %s '%ls' bytes=%llu status=0x%08X\n", write ? "write" : "read", file->fileState->path.c_str(), uint64_t(io.Information), status);
    return status;
}
uint32_t queryFile(PPCContext& ctx, uint8_t* base) {
    const uint32_t ios = ctx.r4.u32, output = ctx.r5.u32, length = ctx.r6.u32, kind = ctx.r7.u32;
    uint32_t status = statusPointer(ios);
    if (status) return status;
    auto file = fileObject(ctx.r3.u32);
    if (!file) { ioStatus(ios, invalidHandle, 0); return invalidHandle; }
    uint32_t size;
    switch (kind) {
        case 4: size = 40; break; case 5: size = 24; break;
        case 6: case 14: size = 8; break; case 16: case 17: size = 4; break;
        case 34: size = 56; break;
        default: fail("File information class requires a verified native layout");
    }
    if (!output || length < size) { ioStatus(ios, lengthMismatch, 0); return lengthMismatch; }
    if (!guestSpan(output, size, true)) { ioStatus(ios, accessViolation, 0); return accessViolation; }
    alignas(8) uint8_t bytes[56]{};
    IO_STATUS_BLOCK io{};
    std::lock_guard lock(file->fileState->ioMutex);
    if (kind == 14 && file->fileState->package) {
        memory->write64(output, file->fileState->packagePosition);
        ioStatus(ios, 0, 8);
        return 0;
    }
    using Query = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    status = uint32_t(ntdll<Query>("NtQueryInformationFile")(file->handle, &io, bytes, size, FILE_INFORMATION_CLASS(kind)));
    status = finish(status, io, file->handle);
    if (int32_t(status) >= 0) {
        memset(base + output, 0, size);
        const uint32_t qwords = kind == 4 ? 4 : kind == 5 ? 2 : kind == 34 ? 6 : (kind == 6 || kind == 14) ? 1 : 0;
        for (uint32_t i = 0; i < qwords; ++i) { uint64_t value; memcpy(&value, bytes + i * 8, 8); memory->write64(output + i * 8, value); }
        if (kind == 4 || kind == 5 || kind == 34) { uint32_t value; memcpy(&value, bytes + qwords * 8, 4); memory->write32(output + qwords * 8, value); }
        if (kind == 5) memcpy(base + output + 20, bytes + 20, 2);
        if (kind == 16 || kind == 17) { uint32_t value; memcpy(&value, bytes, 4); memory->write32(output, value); }
    }
    ioStatus(ios, status, int32_t(status) >= 0 ? uint32_t(io.Information) : 0);
    return status;
}
uint32_t setFile(PPCContext& ctx, uint8_t* base) {
    const uint32_t ios = ctx.r4.u32, input = ctx.r5.u32, length = ctx.r6.u32, kind = ctx.r7.u32;
    uint32_t status = statusPointer(ios);
    if (status) return status;
    auto file = fileObject(ctx.r3.u32);
    if (!file) { ioStatus(ios, invalidHandle, 0); return invalidHandle; }
    if (kind != 13 && kind != 14 && kind != 19 && kind != 20) fail("Set-file information class requires a verified native layout");
    const uint32_t size = kind == 13 ? 1 : 8;
    if (!input || length != size) { ioStatus(ios, invalidParameter, 0); return invalidParameter; }
    if (!guestSpan(input, size, false)) { ioStatus(ios, accessViolation, 0); return accessViolation; }
    if (kind != 14 && !file->fileState->writable) { ioStatus(ios, writeProtected, 0); return writeProtected; }
    alignas(8) uint8_t bytes[8]{};
    if (kind == 13) bytes[0] = base[input] ? 1 : 0;
    else { const uint64_t value = memory->read64(input); memcpy(bytes, &value, 8); }
    std::lock_guard lock(file->fileState->ioMutex);
    if (kind == 14 && file->fileState->package) {
        const int64_t position = int64_t(memory->read64(input));
        status = position < 0 || ((file->fileState->openOptions & 8) && (uint64_t(position) & 511)) ? invalidParameter : 0;
        if (!status) file->fileState->packagePosition = uint64_t(position);
        ioStatus(ios, status, 0);
        return status;
    }
    IO_STATUS_BLOCK io{};
    using Set = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    status = uint32_t(ntdll<Set>("NtSetInformationFile")(file->handle, &io, bytes, size, FILE_INFORMATION_CLASS(kind)));
    status = finish(status, io, file->handle);
    ioStatus(ios, status, uint32_t(io.Information));
    return status;
}
uint32_t queryDirectory(PPCContext& ctx, uint8_t* base) {
    const uint32_t ios = ctx.r7.u32, output = ctx.r8.u32, length = ctx.r9.u32;
    uint32_t status = statusPointer(ios);
    if (status) return status;
    auto file = fileObject(ctx.r3.u32);
    std::shared_ptr<KernelObject> event;
    if (!file || file->kind != ObjectKind::Directory || !completionEvent(ctx.r4.u32, event)) { ioStatus(ios, invalidHandle, 0); return invalidHandle; }
    if (!output || length < 64) { ioStatus(ios, lengthMismatch, 0); return lengthMismatch; }
    const uint64_t restartPointer = uint64_t(ctx.r1.u32) + 84;
    if (!guestSpan(output, length, true) || restartPointer > UINT32_MAX || !guestSpan(uint32_t(restartPointer), 4, false)) {
        ioStatus(ios, accessViolation, 0); return accessViolation;
    }
    std::string ansi;
    if (ctx.r10.u32) status = readAnsi(ctx.r10.u32, ansi);
    if (status) { ioStatus(ios, status, 0); return status; }
    rejectApc(ctx.r5.u32);
    const bool restart = memory->read32(uint32_t(restartPointer)) != 0;
    std::wstring pattern(ansi.begin(), ansi.end());
    UNICODE_STRING filter{USHORT(pattern.size() * 2), USHORT(pattern.size() * 2), pattern.data()};
    std::lock_guard lock(file->fileState->ioMutex);
    alignas(8) uint8_t native[64 + 65536]{};
    const uint8_t* record = native;
    bool completed = true;
    if (restart || file->fileState->pendingDirectoryEntry.empty()) {
        CompletionEvent completion;
        if (!completion.handle) { ioStatus(ios, noMemory, 0); return noMemory; }
        IO_STATUS_BLOCK io{};
        using Query = NTSTATUS (NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID,
            ULONG, FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);
        bool restartScan = restart;
        do {
            status = uint32_t(ntdll<Query>("NtQueryDirectoryFile")(file->handle, completion.handle, nullptr, nullptr,
                &io, native, sizeof(native), FILE_INFORMATION_CLASS(1), TRUE, pattern.empty() ? nullptr : &filter, restartScan));
            restartScan = false;
            status = finish(status, io, completion.handle);
            completed = WaitForSingleObject(completion.handle, 0) == WAIT_OBJECT_0;
            if (int32_t(status) < 0 || !file->fileState->menuOverrideDirectory) break;
            uint32_t nameBytes; memcpy(&nameBytes, native + 60, 4);
            if (nameBytes > sizeof(native) - 64 || (nameBytes & 1)) fail("Invalid native directory record");
            std::wstring entry(reinterpret_cast<const wchar_t*>(native + 64), nameBytes / 2);
            for (wchar_t& value : entry) if (value >= L'A' && value <= L'Z') value += L'a' - L'A';
            if (entry == L"guigroup.wbr" || entry == L"scripts.wbr" || entry == L"bonedmodel.wbr" || entry == L"shader.wbr" || entry == L"texture.wbr") break;
            // Relative opens still resolve in the original game namespace;
            // enumeration exposes the same exact declaration whitelist.
        } while (true);
        if (restart && (int32_t(status) >= 0 || status == 0x80000006 || status == 0xc000000f)) file->fileState->pendingDirectoryEntry.clear();
    } else record = file->fileState->pendingDirectoryEntry.data();
    uint32_t written = 0;
    if (int32_t(status) >= 0) {
        uint32_t nameBytes; memcpy(&nameBytes, record + 60, 4);
        if (nameBytes > sizeof(native) - 64 || (nameBytes & 1)) fail("Invalid native directory record");
        const wchar_t* name = reinterpret_cast<const wchar_t*>(record + 64);
        const int count = WideCharToMultiByte(CP_UTF8, 0, name, nameBytes / 2, nullptr, 0, nullptr, nullptr);
        if (nameBytes && !count) fail("Native directory filename conversion failed");
        std::string text(count, '\0');
        if (count) WideCharToMultiByte(CP_UTF8, 0, name, nameBytes / 2, text.data(), count, nullptr, nullptr);
        written = 64 + std::min(length - 64, uint32_t(text.size()));
        memset(base + output, 0, written);
        uint32_t value; memcpy(&value, record + 4, 4); memory->write32(output + 4, value);
        for (uint32_t i = 1; i <= 6; ++i) { uint64_t value64; memcpy(&value64, record + i * 8, 8); memory->write64(output + i * 8, value64); }
        memcpy(&value, record + 56, 4); memory->write32(output + 56, value);
        memory->write32(output + 60, uint32_t(text.size()));
        memcpy(base + output + 64, text.data(), written - 64);
        if (written - 64 < text.size()) {
            status = 0x80000005;
            if (record == native) file->fileState->pendingDirectoryEntry.assign(native, native + 64 + nameBytes);
        } else file->fileState->pendingDirectoryEntry.clear();
    }
    ioStatus(ios, status, written);
    if (event && completed) SetEvent(event->handle);
    if (traceFiles()) {
        std::string returned;
        if (written >= 64) returned.assign(reinterpret_cast<const char*>(base + output + 64), written - 64);
        fprintf(stderr, "[FILE] directory '%ls' pattern='%ls' restart=%u status=0x%08X bytes=%u name='%s'\n",
            file->fileState->path.c_str(), pattern.c_str(), unsigned(restart), status, written, returned.c_str());
    }
    return status;
}
}

void initializeFilesystem(const std::filesystem::path& gameRoot, const std::filesystem::path& saveRoot,
                          const std::filesystem::path& overrideRoot) {
    auto game = std::filesystem::canonical(gameRoot).make_preferred();
    const auto overrides = overrideRoot.empty() ? std::filesystem::path{} :
        std::filesystem::canonical(overrideRoot).make_preferred();
    if (!overrides.empty() && !std::filesystem::is_directory(overrides))
        fail("Asset override root must be an existing directory");
    std::filesystem::create_directories(saveRoot);
    auto saves = std::filesystem::canonical(saveRoot).make_preferred();
    auto cache = saves / L"cache";
    std::filesystem::create_directories(cache);
    std::lock_guard lock(mountMutex);
    mounts.clear();
    residentPackage.reset();
    assetGameRoot = game;
    assetOverrideRoot = overrides;
    for (const char* key : {"game:", "dvd:", "d:", "\\device\\cdrom0"}) mounts.emplace(key, Mount{game, false});
    mounts.emplace("save:", Mount{saves, true});
    mounts.emplace("cache:", Mount{cache, true});
    mounts.emplace("cache0:", Mount{cache, true});
    fprintf(stderr, "[FILE] game root '%ls'; saves root '%ls'\n", game.c_str(), saves.c_str());
    if (!overrides.empty()) fprintf(stderr, "[FILE] read-only menu overrides '%ls'\n", overrides.c_str());
}

void retainFilesystemPackage(std::shared_ptr<const std::vector<uint8_t>> package) {
    std::lock_guard lock(mountMutex);
    residentPackage.reset();
    if (package && !package->empty()) {
        residentPackage = std::make_shared<ResidentPackage>();
        residentPackage->bytes = std::move(package);
    }
}

PackageCacheStats filesystemPackageCacheStats() {
    std::lock_guard lock(mountMutex);
    if (!residentPackage) return {};
    return {residentPackage->bytes->size(), residentPackage->readCalls.load(std::memory_order_relaxed),
        residentPackage->readBytes.load(std::memory_order_relaxed)};
}

bool filesystemHasAssetOverride(std::string_view gameRelative) {
    if (gameRelative.empty() || gameRelative.size() > 1024 || gameRelative.find('\0') != std::string_view::npos) return false;
    std::string name(gameRelative);
    std::replace(name.begin(), name.end(), '\\', '/');
    if (!allowedAssetOverride(std::filesystem::path(name))) return false;
    const auto resolved = resolveName("game:\\" + name, 0xfffffffd);
    return !resolved.status && !resolved.writable && !resolved.menuOverrideDirectory &&
        resolved.path != resolved.logicalPath;
}

bool filesystemImport(PPCContext& ctx, uint8_t* base, const char* name) {
    const std::string_view function(name);
    auto result = [&](uint32_t value) { ctx.r3.u64 = value; return true; };
    if (function == "NtOpenFile") return result(create(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0, 0, ctx.r7.u32, 1, ctx.r8.u32));
    if (function == "NtCreateFile") {
        const uint64_t optionPointer = uint64_t(ctx.r1.u32) + 84;
        if (optionPointer > UINT32_MAX || !guestSpan(uint32_t(optionPointer), 4, false)) {
            if (!statusPointer(ctx.r6.u32)) ioStatus(ctx.r6.u32, accessViolation, 0);
            return result(accessViolation);
        }
        return result(create(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                             ctx.r9.u32, ctx.r10.u32, memory->read32(uint32_t(optionPointer))));
    }
    if (function == "NtReadFile" || function == "NtWriteFile") return result(transfer(ctx, base, function == "NtWriteFile"));
    if (function == "NtQueryInformationFile") return result(queryFile(ctx, base));
    if (function == "NtSetInformationFile") return result(setFile(ctx, base));
    if (function == "NtQueryDirectoryFile") return result(queryDirectory(ctx, base));
    if (function == "NtQueryFullAttributesFile") {
        const uint32_t attributes = ctx.r3.u32, output = ctx.r4.u32;
        if (!output) return result(invalidParameter);
        if (!guestSpan(output, 56, true)) return result(accessViolation);
        const auto resolved = resolve(attributes);
        uint32_t status = resolved.status;
        if (!status) {
            NativeAttributes attrs(resolved.path, memory->read32(attributes + 8));
            alignas(8) uint8_t bytes[56]{};
            using Query = NTSTATUS (NTAPI*)(POBJECT_ATTRIBUTES, PVOID);
            status = uint32_t(ntdll<Query>("NtQueryFullAttributesFile")(&attrs.attributes, bytes));
            if (int32_t(status) >= 0) {
                memset(base + output, 0, 56);
                for (uint32_t i = 0; i < 6; ++i) { uint64_t value; memcpy(&value, bytes + i * 8, 8); memory->write64(output + i * 8, value); }
                uint32_t value; memcpy(&value, bytes + 48, 4); memory->write32(output + 48, value);
            }
        }
        return result(status);
    }
    if (function == "NtQueryVolumeInformationFile") {
        const uint32_t ios = ctx.r4.u32, output = ctx.r5.u32, length = ctx.r6.u32, kind = ctx.r7.u32;
        const uint32_t pointerStatus = statusPointer(ios);
        if (pointerStatus) return result(pointerStatus);
        auto file = fileObject(ctx.r3.u32);
        if (!file) { ioStatus(ios, invalidHandle, 0); return result(invalidHandle); }
        if (kind != 3) fail("Volume information class requires a verified native layout");
        if (!output || length < 24) { ioStatus(ios, lengthMismatch, 0); return result(lengthMismatch); }
        if (!guestSpan(output, 24, true)) { ioStatus(ios, accessViolation, 0); return result(accessViolation); }
        alignas(8) uint8_t bytes[24]{};
        IO_STATUS_BLOCK io{};
        std::lock_guard lock(file->fileState->ioMutex);
        using Query = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
        uint32_t status = uint32_t(ntdll<Query>("NtQueryVolumeInformationFile")(file->handle, &io, bytes, sizeof(bytes), kind));
        status = finish(status, io, file->handle);
        if (int32_t(status) >= 0) {
            for (uint32_t i = 0; i < 2; ++i) { uint64_t value; memcpy(&value, bytes + i * 8, 8); memory->write64(output + i * 8, value); }
            for (uint32_t i = 4; i < 6; ++i) { uint32_t value; memcpy(&value, bytes + i * 4, 4); memory->write32(output + i * 4, value); }
        }
        ioStatus(ios, status, int32_t(status) >= 0 ? uint32_t(io.Information) : 0);
        return result(status);
    }
    if (function == "NtFlushBuffersFile") {
        const uint32_t ios = ctx.r4.u32, pointerStatus = statusPointer(ios);
        if (pointerStatus) return result(pointerStatus);
        auto file = fileObject(ctx.r3.u32);
        if (!file) { ioStatus(ios, invalidHandle, 0); return result(invalidHandle); }
        std::lock_guard lock(file->fileState->ioMutex);
        IO_STATUS_BLOCK io{};
        using Flush = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK);
        uint32_t status = uint32_t(ntdll<Flush>("NtFlushBuffersFile")(file->handle, &io));
        status = finish(status, io, file->handle);
        ioStatus(ios, status, uint32_t(io.Information));
        return result(status);
    }
    if (function == "ObCreateSymbolicLink" || function == "ObDeleteSymbolicLink") {
        std::string alias;
        uint32_t status = readAnsi(ctx.r3.u32, alias);
        if (status) return result(status);
        alias = aliasKey(std::move(alias));
        if (alias.empty() || (!alias.ends_with(':') && !alias.starts_with("\\device\\"))) return result(nameInvalid);
        if (function == "ObDeleteSymbolicLink") {
            std::lock_guard lock(mountMutex);
            return result(mounts.erase(alias) ? 0 : 0xc0000034);
        }
        std::string target;
        status = readAnsi(ctx.r4.u32, target);
        if (status) return result(status);
        auto resolved = resolveName(std::move(target), 0xfffffffd);
        if (resolved.status) return result(resolved.status);
        std::error_code error;
        if (!std::filesystem::is_directory(resolved.path, error)) return result(0xc000003a);
        std::lock_guard lock(mountMutex);
        return result(mounts.emplace(alias, Mount{resolved.logicalPath, resolved.writable}).second ? 0 : 0xc0000035);
    }
    if (function == "NtReadFileScatter") fail("Scatter file reads require a verified guest segment layout");
    return false;
}
}
