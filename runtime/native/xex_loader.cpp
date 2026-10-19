#include "xex_loader.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <xex_patcher.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace Doritos::Native {
namespace {
constexpr size_t kMaximumFile = 128 * 1024 * 1024;
constexpr size_t kMaximumImage = 256 * 1024 * 1024;
constexpr uint32_t kFormatKey = 0x3ff;
constexpr uint32_t kEntryKey = 0x10100;
constexpr uint32_t kBaseKey = 0x10201;
constexpr uint32_t kImportKey = 0x103ff;
constexpr uint32_t kTlsKey = 0x20104;
constexpr uint32_t kStackKey = 0x20200;
constexpr std::array<uint8_t, 16> kRetailKey = {
    0x20, 0xb1, 0x85, 0xa5, 0x9d, 0x28, 0xfd, 0xc3,
    0x40, 0x58, 0x3f, 0xbb, 0x08, 0x96, 0xbf, 0x91};

[[noreturn]] void invalid(std::string_view reason) {
    throw std::runtime_error("Invalid XEX: " + std::string(reason));
}

void range(std::span<const uint8_t> bytes, size_t offset, size_t size,
           std::string_view description) {
    if (offset > bytes.size() || size > bytes.size() - offset) {
        std::ostringstream reason;
        reason << description << " (offset 0x" << std::hex << offset << ", size 0x" << size
               << ", extent 0x" << bytes.size() << ')';
        invalid(reason.str());
    }
}

uint16_t be16(std::span<const uint8_t> bytes, size_t offset) {
    range(bytes, offset, 2, "truncated 16-bit field");
    return uint16_t((uint16_t(bytes[offset]) << 8) | bytes[offset + 1]);
}

uint32_t be32(std::span<const uint8_t> bytes, size_t offset) {
    range(bytes, offset, 4, "truncated 32-bit field");
    return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset + 1]) << 16) |
           (uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

uint16_t le16(std::span<const uint8_t> bytes, size_t offset) {
    range(bytes, offset, 2, "truncated PE field");
    return uint16_t(bytes[offset] | (uint16_t(bytes[offset + 1]) << 8));
}

uint32_t le32(std::span<const uint8_t> bytes, size_t offset) {
    range(bytes, offset, 4, "truncated PE field");
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
           (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
}

void cryptoCheck(NTSTATUS status, std::string_view action) {
    if (status < 0) {
        std::ostringstream message;
        message << action << " failed (BCrypt status 0x" << std::hex
                << uint32_t(status) << ')';
        throw std::runtime_error(message.str());
    }
}

struct Algorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    explicit Algorithm(LPCWSTR name) {
        cryptoCheck(BCryptOpenAlgorithmProvider(&handle, name, nullptr, 0), "Open crypto provider");
    }
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    Algorithm(const Algorithm&) = delete;
    Algorithm& operator=(const Algorithm&) = delete;
    ULONG property(LPCWSTR name) const {
        ULONG value = 0, written = 0;
        cryptoCheck(BCryptGetProperty(handle, name, reinterpret_cast<PUCHAR>(&value),
                                     sizeof(value), &written, 0), "Read crypto property");
        if (written != sizeof(value)) throw std::runtime_error("Unexpected BCrypt property size");
        return value;
    }
};

std::vector<uint8_t> digest(std::span<const uint8_t> input, LPCWSTR algorithm) {
    Algorithm provider(algorithm);
    std::vector<uint8_t> object(provider.property(BCRYPT_OBJECT_LENGTH));
    std::vector<uint8_t> output(provider.property(BCRYPT_HASH_LENGTH));
    BCRYPT_HASH_HANDLE handle = nullptr;
    cryptoCheck(BCryptCreateHash(provider.handle, &handle, object.data(), ULONG(object.size()),
                                nullptr, 0, 0), "Create hash");
    struct Cleanup { BCRYPT_HASH_HANDLE handle; ~Cleanup() { BCryptDestroyHash(handle); } } cleanup{handle};
    cryptoCheck(BCryptHashData(handle, const_cast<PUCHAR>(input.data()), ULONG(input.size()), 0), "Hash bytes");
    cryptoCheck(BCryptFinishHash(handle, output.data(), ULONG(output.size()), 0), "Finish hash");
    return output;
}

std::string sha256(std::span<const uint8_t> input) {
    const auto result = digest(input, BCRYPT_SHA256_ALGORITHM);
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : result) stream << std::setw(2) << unsigned(byte);
    return stream.str();
}

std::vector<uint8_t> decrypt(std::span<const uint8_t> input, std::span<const uint8_t> key) {
    if (input.empty() || input.size() % 16 != 0 || key.size() != 16) invalid("AES block layout");
    Algorithm provider(BCRYPT_AES_ALGORITHM);
    cryptoCheck(BCryptSetProperty(provider.handle, BCRYPT_CHAINING_MODE,
                                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                                sizeof(BCRYPT_CHAIN_MODE_CBC), 0), "Set AES CBC mode");
    std::vector<uint8_t> object(provider.property(BCRYPT_OBJECT_LENGTH));
    BCRYPT_KEY_HANDLE handle = nullptr;
    cryptoCheck(BCryptGenerateSymmetricKey(provider.handle, &handle, object.data(), ULONG(object.size()),
                                         const_cast<PUCHAR>(key.data()), ULONG(key.size()), 0), "Create AES key");
    struct Cleanup { BCRYPT_KEY_HANDLE handle; ~Cleanup() { BCryptDestroyKey(handle); } } cleanup{handle};
    std::array<uint8_t, 16> iv{};
    std::vector<uint8_t> output(input.size());
    ULONG written = 0;
    cryptoCheck(BCryptDecrypt(handle, const_cast<PUCHAR>(input.data()), ULONG(input.size()), nullptr,
                             iv.data(), ULONG(iv.size()), output.data(), ULONG(output.size()), &written, 0),
                "Decode original encrypted XEX");
    if (written != input.size()) invalid("unexpected decoded AES size");
    return output;
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot open original XEX: " + path.string());
    const auto size = stream.tellg();
    if (size < 24 || size > std::streamoff(kMaximumFile)) invalid("file size");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())))
        throw std::runtime_error("Cannot read original XEX");
    return bytes;
}

#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) {ORDINAL, #NAME}
const std::unordered_map<uint32_t, const char*> kKernelExports = {
#include <xbox/xboxkrnl_table.inc>
};
const std::unordered_map<uint32_t, const char*> kXamExports = {
#include <xbox/xam_table.inc>
};
#undef XE_EXPORT

std::string exportName(std::string_view library, uint32_t ordinal) {
    const auto* exports = library == "xboxkrnl.exe" ? &kKernelExports :
                          library == "xam.xex" ? &kXamExports : nullptr;
    if (exports) {
        const auto found = exports->find(ordinal);
        if (found != exports->end()) return found->second;
    }
    return "ordinal_" + std::to_string(ordinal);
}

uint32_t validateImage(std::span<const uint8_t> image, uint32_t base, uint32_t entry) {
    range(image, 0, 64, "DOS header");
    if (image[0] != 'M' || image[1] != 'Z') invalid("decoded image is not PE");
    const size_t pe = le32(image, 0x3c);
    range(image, pe, 24, "PE header");
    if (le32(image, pe) != 0x00004550) invalid("PE signature");
    const size_t sectionCount = le16(image, pe + 6);
    const size_t optionalSize = le16(image, pe + 20);
    if (!sectionCount || sectionCount > 96 || optionalSize < 96) invalid("PE layout");
    range(image, pe + 24, optionalSize, "PE optional header");
    if (le16(image, pe + 24) != 0x10b || le32(image, pe + 24 + 28) != base)
        invalid("PE image base or format");
    const auto mappedSize = le32(image, pe + 24 + 56);
    if (mappedSize < image.size() || mappedSize > kMaximumImage ||
        uint64_t(base) + mappedSize > (uint64_t{1} << 32)) invalid("PE virtual image size");
    const size_t sections = pe + 24 + optionalSize;
    range(image, sections, sectionCount * 40, "PE section table");
    bool entryInCode = false;
    for (size_t i = 0; i < sectionCount; ++i) {
        const size_t section = sections + i * 40;
        const auto bytes = le32(image, section + 8);
        const auto rva = le32(image, section + 12);
        const auto rawSize = le32(image, section + 16);
        const auto flags = le32(image, section + 36);
        if (uint64_t(rva) + bytes > mappedSize) invalid("PE section virtual range");
        // Retail XEX packaging can strip the original PE's discardable .reloc
        // tail. This port pins the original base, so no relocation is applied.
        // Other sections must retain all their mapped file-backed bytes.
        if (rva > image.size() || std::min(rawSize, bytes) > image.size() - rva) {
            std::string sectionName(reinterpret_cast<const char*>(image.data() + section), 8);
            sectionName.resize(sectionName.find('\0') == std::string::npos ? 8 : sectionName.find('\0'));
            const bool discardedRelocations = sectionName == ".reloc" &&
                (flags & 0x02000000) && !(flags & (0x20 | 0x20000000));
            if (discardedRelocations) continue;
            std::ostringstream detail;
            detail << "PE section backed range: name=" << sectionName
                   << ", RVA=0x" << std::hex << rva << ", virtual=0x" << bytes
                   << ", raw=0x" << rawSize << ", raw offset=0x" << le32(image, section + 20)
                   << ", flags=0x" << flags << ", mapped=0x" << mappedSize;
            invalid(detail.str());
        }
        if ((le32(image, section + 36) & 0x20) && entry >= uint64_t(base) + rva &&
            entry < uint64_t(base) + rva + bytes) entryInCode = true;
    }
    if (!entryInCode) invalid("entry point outside executable section");
    return mappedSize;
}

std::string jsonQuoted(std::string_view value) {
    std::ostringstream stream;
    stream << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') stream << '\\' << char(c);
        else if (c < 0x20) stream << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else stream << char(c);
    }
    stream << '"';
    return stream.str();
}
} // namespace

LoadedXex loadOriginalXex(const std::filesystem::path& path) {
    const auto file = readFile(path);
    const std::span<const uint8_t> bytes(file);
    if (be32(bytes, 0) != 0x58455832) invalid("expected XEX2 signature");
    const size_t headerSize = be32(bytes, 8);
    const size_t security = be32(bytes, 16);
    const size_t count = be32(bytes, 20);
    if (headerSize < 24 || headerSize > bytes.size() || headerSize > 0x100000 ||
        count > (headerSize - 24) / 8) invalid("header layout");
    const auto header = bytes.first(headerSize);
    const size_t tableEnd = 24 + count * 8;
    std::map<uint32_t, uint32_t> fields;
    for (size_t i = 0; i < count; ++i) {
        const auto key = be32(header, 24 + i * 8);
        const auto value = be32(header, 28 + i * 8);
        if (!fields.emplace(key, value).second) invalid("duplicate optional header");
        const auto low = key & 0xff;
        if (low > 1) {
            if (value < tableEnd) invalid("optional header overlaps directory");
            size_t size = size_t(low) * 4;
            if (low == 0xff) size = be32(header, value);
            if (size < 4) invalid("empty optional header");
            range(header, value, size, "optional header range");
        }
    }
    const auto required = [&](uint32_t key) -> uint32_t {
        const auto found = fields.find(key);
        if (found == fields.end()) invalid("required optional header missing");
        return found->second;
    };
    const size_t format = required(kFormatKey);
    range(header, format, 8, "file format header");
    const auto encryption = be16(header, format + 4);
    if (encryption == 0)
        throw std::runtime_error("STOP: default.xex is unencrypted. Supply the regular original encrypted version.");
    if (encryption != 1) invalid("unsupported encryption type");
    LoadedXex result;
    result.xex_sha256 = sha256(bytes);
    if (result.xex_sha256 != kSupportedXexSha256)
        throw std::runtime_error("Original encrypted default.xex differs from the supported AOT revision (SHA-256 " +
                                 result.xex_sha256 + ").");
    if (security < tableEnd) invalid("security header overlaps directory");
    range(header, security, 0x184, "security header");
    const size_t securitySize = be32(header, security);
    if (securitySize < 0x184) invalid("security header size");
    range(header, security, securitySize, "security header size");
    const size_t pageCount = be32(header, security + 0x180);
    if (pageCount > (securitySize - 0x184) / 24) invalid("security page descriptors");
    const size_t imageSize = be32(header, security + 4);
    if (!imageSize || imageSize > kMaximumImage) invalid("decoded image size");
    result.image_base = required(kBaseKey);
    result.entry_point = required(kEntryKey);
    if (uint64_t(result.image_base) + imageSize > (uint64_t{1} << 32) ||
        result.image_base != be32(header, security + 0x110)) invalid("guest image range");
    if (const auto found = fields.find(kStackKey); found != fields.end()) result.default_stack_size = found->second;
    result.header.assign(header.begin(), header.end());
    const auto session = decrypt(header.subspan(security + 0x150, 16), kRetailKey);
    const auto payload = decrypt(bytes.subspan(headerSize), session);
    const std::span<const uint8_t> decoded(payload);
    const auto compression = be16(header, format + 6);
    const size_t formatSize = be32(header, format);
    result.image.resize(imageSize);
    if (compression == 0) {
        range(decoded, 0, imageSize, "uncompressed payload");
        std::copy_n(decoded.begin(), imageSize, result.image.begin());
    } else if (compression == 1) {
        if (formatSize < 16 || (formatSize - 8) % 8) invalid("basic compression block table");
        size_t source = 0, destination = 0;
        for (size_t position = format + 8; position < format + formatSize; position += 8) {
            const size_t dataSize = be32(header, position), zeroSize = be32(header, position + 4);
            range(decoded, source, dataSize, "basic compression source");
            range(result.image, destination, dataSize + zeroSize, "basic compression destination");
            std::copy_n(decoded.begin() + source, dataSize, result.image.begin() + destination);
            source += dataSize;
            destination += dataSize + zeroSize;
        }
        if (decoded.size() - source >= 16) invalid("unexpected basic compression trailing bytes");
    } else if (compression == 2) {
        if (formatSize != 36) invalid("normal compression header size");
        const auto window = be32(header, format + 8);
        if (window < 0x8000 || window > 0x200000 || (window & (window - 1))) invalid("LZX window size");
        uint32_t blockSize = be32(header, format + 12);
        std::array<uint8_t, 20> expectedHash{};
        std::copy_n(header.begin() + format + 16, 20, expectedHash.begin());
        std::vector<uint8_t> compressed;
        compressed.reserve(decoded.size());
        size_t position = 0;
        while (blockSize) {
            if (blockSize < 26) invalid("LZX block size");
            range(decoded, position, blockSize, "LZX block range");
            const auto actualHash = digest(decoded.subspan(position, blockSize), BCRYPT_SHA1_ALGORITHM);
            if (!std::equal(actualHash.begin(), actualHash.end(), expectedHash.begin())) invalid("LZX block digest");
            const auto nextSize = be32(decoded, position);
            std::copy_n(decoded.begin() + position + 4, 20, expectedHash.begin());
            size_t chunk = position + 24;
            const size_t end = position + blockSize;
            for (;;) {
                if (end - chunk < 2) invalid("LZX chunk terminator");
                const size_t chunkSize = be16(decoded, chunk);
                chunk += 2;
                if (!chunkSize) break;
                if (chunkSize > end - chunk) invalid("LZX chunk range");
                compressed.insert(compressed.end(), decoded.begin() + chunk, decoded.begin() + chunk + chunkSize);
                chunk += chunkSize;
            }
            position = end;
            blockSize = nextSize;
        }
        if (compressed.empty() || decoded.size() - position >= 16) invalid("LZX payload extent");
        // Uses the pinned XenonRecomp/libmspack decoder, without its unbounded XEX parser
        // or import-thunk rewriting. The native runtime gets exact original image bytes.
        if (lzxDecompress(compressed.data(), compressed.size(), result.image.data(), result.image.size(),
                          window, nullptr, 0) != 0) invalid("LZX decompression failed");
    } else invalid("unsupported compression type");
    result.mapped_image_size = validateImage(result.image, result.image_base, result.entry_point);
    result.image_sha256 = sha256(result.image);
    if (result.image_sha256 != kSupportedImageSha256)
        throw std::runtime_error("Decoded image differs from the supported AOT revision (SHA-256 " +
                                 result.image_sha256 + ").");
    if (const auto found = fields.find(kTlsKey); found != fields.end()) {
        const size_t tls = found->second;
        result.tls = {be32(header, tls), be32(header, tls + 4), be32(header, tls + 8), be32(header, tls + 12)};
        if (result.tls.slot_count > 4096 || result.tls.raw_data_size > result.tls.data_size) invalid("TLS layout");
        if (result.tls.raw_data_size) {
            if (result.tls.data_address < result.image_base) invalid("TLS address");
            range(result.image, result.tls.data_address - result.image_base, result.tls.raw_data_size, "TLS template");
        }
    }
    if (const auto found = fields.find(kImportKey); found != fields.end()) {
        const size_t offset = found->second;
        const size_t size = be32(header, offset), stringSize = be32(header, offset + 4), libraries = be32(header, offset + 8);
        if (size < 12 || stringSize > size - 12 || libraries > (size - 12 - stringSize) / 40)
            invalid("import directory layout");
        const auto imports = header.subspan(offset, size);
        const auto strings = imports.subspan(12, stringSize);
        std::vector<std::string> names;
        size_t stringOffset = 0;
        for (size_t i = 0; i < libraries; ++i) {
            if (stringOffset >= strings.size()) invalid("import library name");
            const auto zero = std::find(strings.begin() + stringOffset, strings.end(), uint8_t{0});
            if (zero == strings.end()) invalid("unterminated import library name");
            const size_t length = size_t(zero - strings.begin()) - stringOffset;
            if (!length || length > 128) invalid("import library name size");
            names.emplace_back(reinterpret_cast<const char*>(strings.data() + stringOffset), length);
            stringOffset += (length + 1 + 3) & ~size_t{3};
        }
        size_t libraryOffset = 12 + stringSize;
        for (size_t i = 0; i < libraries; ++i) {
            range(imports, libraryOffset, 40, "import library header");
            const size_t librarySize = be32(imports, libraryOffset);
            const size_t nameIndex = be16(imports, libraryOffset + 36);
            const size_t importCount = be16(imports, libraryOffset + 38);
            if (librarySize != 40 + importCount * 4 || nameIndex >= names.size()) invalid("import library layout");
            range(imports, libraryOffset, librarySize, "import library extent");
            for (size_t j = 0; j < importCount; ++j) {
                const auto address = be32(imports, libraryOffset + 40 + j * 4);
                if (address < result.image_base) invalid("import address below image");
                const size_t rva = address - result.image_base;
                const auto descriptor = be32(result.image, rva);
                const auto kind = descriptor >> 24;
                if (kind > 1) invalid("import descriptor type");
                range(result.image, rva, kind ? 16 : 4, "import descriptor extent");
                const auto ordinal = descriptor & 0xffff;
                result.imports.push_back({names[nameIndex], exportName(names[nameIndex], ordinal), ordinal, address, kind});
            }
            libraryOffset += librarySize;
        }
        if (libraryOffset != imports.size()) invalid("import directory trailing bytes");
    }
    return result;
}

std::string xexMetadataJson(const LoadedXex& xex) {
    std::ostringstream json;
    json << "{\n  \"xex_sha256\": " << jsonQuoted(xex.xex_sha256)
         << ",\n  \"image_sha256\": " << jsonQuoted(xex.image_sha256)
         << ",\n  \"encryption\": 1,\n  \"image_base\": " << xex.image_base
         << ",\n  \"image_size\": " << xex.image.size()
         << ",\n  \"mapped_image_size\": " << xex.mapped_image_size
         << ",\n  \"entry_point\": " << xex.entry_point
         << ",\n  \"default_stack_size\": " << xex.default_stack_size
         << ",\n  \"tls\": {\"slot_count\": " << xex.tls.slot_count
         << ", \"data_address\": " << xex.tls.data_address
         << ", \"data_size\": " << xex.tls.data_size
         << ", \"raw_data_size\": " << xex.tls.raw_data_size << "},\n  \"imports\": [";
    for (size_t i = 0; i < xex.imports.size(); ++i) {
        const auto& item = xex.imports[i];
        json << (i ? ",\n" : "\n") << "    {\"library\": " << jsonQuoted(item.library)
             << ", \"name\": " << jsonQuoted(item.name) << ", \"ordinal\": " << item.ordinal
             << ", \"address\": " << item.address << ", \"kind\": " << item.kind << '}';
    }
    json << "\n  ]\n}\n";
    return json.str();
}
} // namespace Doritos::Native
