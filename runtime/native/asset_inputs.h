#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace Doritos::Native {
struct TitleRendererInputs {
    std::shared_ptr<const std::vector<uint8_t>> package;
    std::vector<uint8_t> windowsCache;
    std::vector<uint8_t> xenonCache;
    std::vector<uint8_t> nativeExtension;
    std::vector<uint8_t> xenonSourceExtension;
};
// Checks the user's supported package and each bounded derived cache against
// their original SHA-256. Prepared files never replace the original game.
TitleRendererInputs verifiedRendererInputs(const std::filesystem::path& game,
                                          const std::filesystem::path& shaderDirectory,
                                          bool retainPackage = true);
}
