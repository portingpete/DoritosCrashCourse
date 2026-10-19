#include "runtime/native/xex_loader.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cwchar>

namespace {
bool sameDestination(const std::filesystem::path& first, const std::filesystem::path& second) {
    const auto firstPath = std::filesystem::weakly_canonical(first);
    const auto secondPath = std::filesystem::weakly_canonical(second);
    return _wcsicmp(firstPath.c_str(), secondPath.c_str()) == 0;
}
void checkDestination(const std::filesystem::path& source, const std::filesystem::path& destination) {
    if (destination.empty()) return;
    const auto sourcePath = std::filesystem::weakly_canonical(source);
    const auto targetPath = std::filesystem::weakly_canonical(destination);
    if (sameDestination(sourcePath, targetPath) ||
        (std::filesystem::exists(destination) && std::filesystem::equivalent(source, destination)))
        throw std::runtime_error("Output cannot overwrite the original XEX.");
}
void write(const std::filesystem::path& path, const char* bytes, size_t size) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(bytes, std::streamsize(size)))
        throw std::runtime_error("Cannot write output: " + path.string());
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: NativeXexInspect <original default.xex> [--output <decoded image>] [--metadata <JSON>]\n";
            return 2;
        }
        const std::filesystem::path source = argv[1];
        std::filesystem::path imagePath, metadataPath;
        for (int i = 2; i < argc; ++i) {
            const std::wstring option = argv[i];
            if ((option != L"--output" && option != L"--metadata") || i + 1 == argc)
                throw std::runtime_error("Unknown or incomplete argument.");
            auto& target = option == L"--output" ? imagePath : metadataPath;
            if (!target.empty()) throw std::runtime_error("Duplicate output argument.");
            target = argv[++i];
        }
        checkDestination(source, imagePath);
        checkDestination(source, metadataPath);
        if (!imagePath.empty() && !metadataPath.empty() &&
            (sameDestination(imagePath, metadataPath) ||
             (std::filesystem::exists(imagePath) && std::filesystem::exists(metadataPath) &&
              std::filesystem::equivalent(imagePath, metadataPath))))
            throw std::runtime_error("Image and metadata outputs must have different paths.");
        const auto loaded = Doritos::Native::loadOriginalXex(source);
        const auto metadata = Doritos::Native::xexMetadataJson(loaded);
        if (!imagePath.empty()) write(imagePath, reinterpret_cast<const char*>(loaded.image.data()), loaded.image.size());
        if (!metadataPath.empty()) write(metadataPath, metadata.data(), metadata.size());
        std::cout << "Verified original encrypted XEX " << loaded.xex_sha256 << '\n'
                  << "Exact decoded image " << loaded.image_sha256 << " (" << loaded.image.size() << " bytes)\n"
                  << "Entry point 0x" << std::hex << loaded.entry_point << std::dec
                  << "; import records " << loaded.imports.size() << '\n';
        if (imagePath.empty() && metadataPath.empty()) std::cout << metadata;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
