#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Doritos::Native {

inline constexpr char kSupportedXexSha256[] =
    "6c4fa5a12ef2ad51db4809d5d654b487300a1ace06c2cdf6cb14258fd273dfc1";
inline constexpr char kSupportedImageSha256[] =
    "e2be3b416bffca18e59c34cbc6d20bc6ff14490a0ec3b1c30e4a0594ce79c8d7";

struct XexImport {
    std::string library;
    std::string name;
    uint32_t ordinal = 0;
    uint32_t address = 0;
    uint32_t kind = 0; // 0: variable/pointer slot; 1: callable thunk.
};

struct XexTls {
    uint32_t slot_count = 0;
    uint32_t data_address = 0;
    uint32_t data_size = 0;
    uint32_t raw_data_size = 0;
};

struct LoadedXex {
    // Exact decoded image. Import descriptors have not been rewritten.
    std::vector<uint8_t> image;
    std::vector<uint8_t> header;
    std::string xex_sha256;
    std::string image_sha256;
    uint32_t image_base = 0;
    uint32_t mapped_image_size = 0; // Original PE SizeOfImage; .reloc tail is discarded.
    uint32_t entry_point = 0;
    uint32_t default_stack_size = 0;
    XexTls tls;
    std::vector<XexImport> imports;
};

// Requires the original encrypted supported revision. Never changes its file.
LoadedXex loadOriginalXex(const std::filesystem::path& path);
std::string xexMetadataJson(const LoadedXex& xex);

} // namespace Doritos::Native
