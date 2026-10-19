#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>
#include "ppc_context.h"

namespace Doritos::Native {
struct ResidentPackage;
// Shared by duplicate guest handles. Windows owns ordinary file positions;
// retained package handles keep their cursor here under the same I/O lock.
// Directory enumeration retains an entry on overflow.
struct NativeFileState {
    std::filesystem::path path;
    // Guest directory-relative opens retain their original game namespace,
    // even when Windows reads an authored asset from an override directory.
    std::filesystem::path logicalPath;
    bool writable = false;
    bool menuOverrideDirectory = false;
    uint32_t openOptions = 0;
    std::shared_ptr<ResidentPackage> package;
    uint64_t packagePosition = 0;
    std::mutex ioMutex;
    std::vector<uint8_t> pendingDirectoryEntry;
};
void initializeFilesystem(const std::filesystem::path& gameRoot,
                          const std::filesystem::path& saveRoot,
                          const std::filesystem::path& assetOverrideRoot = {});
// Publish only the immutable bytes returned by verifiedRendererInputs, before
// starting title threads. Existing handles retain their original backing.
void retainFilesystemPackage(std::shared_ptr<const std::vector<uint8_t>> package);
struct PackageCacheStats { uint64_t residentBytes, readCalls, readBytes; };
PackageCacheStats filesystemPackageCacheStats();
// Exact read-only asset policy used by the original package-first loader hook.
// True only when the same NT open resolver selects an existing allowed file.
bool filesystemHasAssetOverride(std::string_view gameRelative);
bool filesystemImport(PPCContext& ctx, uint8_t* base, const char* name);
}
