#include "runtime/native/asset_inputs.h"
#include "shader_inputs.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

using namespace Doritos::Native;
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    try {
        const std::filesystem::path game(argv[1]), shaders(argv[2]);
        const auto original = verifiedRendererInputs(game, shaders);
        if (!original.package || original.package->size() != 55672832 ||
            std::memcmp(original.package->data(), "tongas_pack_v30000", 18))
            throw std::runtime_error("Verified package was not retained intact");
        const auto uncached = verifiedRendererInputs(game, shaders, false);
        if (uncached.package || uncached.windowsCache != original.windowsCache || uncached.xenonCache != original.xenonCache)
            throw std::runtime_error("Cache-off comparison changed shader inputs or retained the package");
        if (original.windowsCache.size() != 298142 || original.xenonCache.size() != 446928)
            throw std::runtime_error("Original shader cache extents differ");
        const auto temporary = std::filesystem::temp_directory_path() /
            (L"DoritosRendererContract-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(temporary)) throw std::runtime_error("Cannot create isolated input contract folder");
        struct Cleanup {
            std::filesystem::path folder;
            ~Cleanup() {
                std::error_code ignored;
                std::filesystem::remove(folder / "precache_win32_3.bsh", ignored);
                std::filesystem::remove(folder / "precache_xenon.bsh", ignored);
                if (ShaderInputs::path[0]) std::filesystem::remove(folder / std::filesystem::path(ShaderInputs::path).filename(), ignored);
                if (ShaderInputs::sourcePath[0]) std::filesystem::remove(folder / std::filesystem::path(ShaderInputs::sourcePath).filename(), ignored);
                std::filesystem::remove(folder, ignored);
            }
        } cleanup{temporary};
        std::filesystem::copy_file(shaders / "precache_win32_3.bsh", temporary / "precache_win32_3.bsh");
        std::filesystem::copy_file(shaders / "precache_xenon.bsh", temporary / "precache_xenon.bsh");
        for (const wchar_t* source : {ShaderInputs::path, ShaderInputs::sourcePath}) {
            if (!source[0]) continue;
            const std::filesystem::path configured(source);
            const auto local = shaders / configured.filename();
            std::filesystem::copy_file(std::filesystem::exists(local) ? local : configured,
                                       temporary / configured.filename());
        }
        const auto portable = verifiedRendererInputs(game, temporary);
        if (!portable.package || *portable.package != *original.package ||
            portable.windowsCache != original.windowsCache || portable.xenonCache != original.xenonCache ||
            portable.nativeExtension != original.nativeExtension || portable.xenonSourceExtension != original.xenonSourceExtension)
            throw std::runtime_error("Relocated shader inputs differ from their verified originals");
        for (const wchar_t* source : {ShaderInputs::path, ShaderInputs::sourcePath}) {
            if (!source[0]) continue;
            const auto local = temporary / std::filesystem::path(source).filename();
            {
                std::fstream altered(local, std::ios::in | std::ios::out | std::ios::binary);
                char value = 0; altered.read(&value, 1); value ^= 1;
                altered.seekp(0); altered.write(&value, 1);
            }
            bool rejected = false;
            try { verifiedRendererInputs(game, temporary); }
            catch (const std::runtime_error&) { rejected = true; }
            if (!rejected) throw std::runtime_error("Modified portable extension fell back to a valid development copy");
            std::filesystem::copy_file(std::filesystem::exists(shaders / local.filename()) ? shaders / local.filename() :
                                       std::filesystem::path(source), local, std::filesystem::copy_options::overwrite_existing);
        }
        {
            std::fstream altered(temporary / "precache_win32_3.bsh", std::ios::in | std::ios::out | std::ios::binary);
            const char changed = char(original.windowsCache[64] ^ 1);
            altered.seekp(64); altered.write(&changed, 1);
        }
        auto reject = [&](const char* detail) {
            try { verifiedRendererInputs(game, temporary); }
            catch (const std::runtime_error&) { return; }
            throw std::runtime_error(detail);
        };
        reject("Modified cache was accepted");
        std::filesystem::resize_file(temporary / "precache_win32_3.bsh", 0);
        reject("Empty cache was accepted");
        std::filesystem::remove(temporary / "precache_win32_3.bsh");
        reject("Missing cache was accepted");
        std::fprintf(stderr, "Original package/cache identity and altered/empty/missing cache rejection passed.\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native renderer input contract: %s\n", error.what());
        return 1;
    }
}
