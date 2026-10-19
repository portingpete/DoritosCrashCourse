#include "runtime/native/runtime.h"
#include "runtime/native/environment.h"
#include "runtime/native/kernel_objects.h"
#include "runtime/native/filesystem.h"
#include "runtime/native/memory_services.h"
#include "runtime/native/audio_services.h"
#include "runtime/native/title_lifecycle.h"
#include "runtime/native/asset_inputs.h"
#include "runtime/native/xma_services.h"
#include "runtime/native/user_services.h"
#include "runtime/native/network_services.h"
#include "renderer/native/title_bridge.h"
#include "renderer/native/video_settings.h"
#include <cstdio>
#include <filesystem>
#include <string>

int wmain(int argc, wchar_t** argv) {
    using namespace Doritos::Native;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        std::filesystem::path game = L"game";
        std::filesystem::path shaders = L"build_native/renderer_inputs/data/art/dxshaders";
        std::filesystem::path assetOverrides;
        bool checkOnly = false;
        bool packageCache = true;
        for (int i = 1; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--game-dir" && i + 1 < argc) game = argv[++i];
            else if (std::wstring(argv[i]) == L"--shader-dir" && i + 1 < argc) shaders = argv[++i];
            else if (std::wstring(argv[i]) == L"--asset-override-dir" && i + 1 < argc) assetOverrides = argv[++i];
            else if (std::wstring(argv[i]) == L"--check") checkOnly = true;
            else if (std::wstring(argv[i]) == L"--no-package-cache") packageCache = false;
            else { fprintf(stderr, "Usage: DoritosNative [--game-dir PATH] [--shader-dir PATH] [--asset-override-dir PATH] [--check] [--no-package-cache]\n"); return 2; }
        }
        const auto image = loadOriginalXex(game / L"default.xex");
        fprintf(stderr, "[XEX] original encrypted revision verified; decoded image sha256=%s\n", image.image_sha256.c_str());
        fprintf(stderr, "[NATIVE] compiled PowerPC functions, native Win32 bridges; no runtime CPU emulator.\n");
        fprintf(stderr, "[STATUS] Development build: native renderer/audio/gameplay remain unverified.\n");
        Memory guest(image);
        memory = &guest;
        struct Services {
            Memory* owner;
            ~Services() {
                shutdownAudioServices(owner);
                shutdownUserServices();
                const auto package = filesystemPackageCacheStats();
                fprintf(stderr, "[PACKAGE CACHE] resident_bytes=%llu reads=%llu read_bytes=%llu\n",
                    package.residentBytes, package.readCalls, package.readBytes);
                const auto frames = Doritos::Graphics::titleGraphicsCounters();
                fprintf(stderr, "[RENDER] %ux%u draws=%llu presents=%llu triangles=%llu initialization=%llu clear_only=%llu\n",
                    frames.width, frames.height, frames.draws, frames.presents, frames.triangles,
                    frames.initializationPresents, frames.clearPresents);
                if (frames.measuredIntervals && frames.measuredNanoseconds) {
                    const double fps = 1e9 * double(frames.measuredIntervals) / double(frames.measuredNanoseconds);
                    fprintf(stderr, "[FRAME TIME] measured=%llu average_fps=%.3f worst_ms=%.3f\n",
                        frames.measuredIntervals, fps, double(frames.worstIntervalNanoseconds) / 1e6);
                }
                shutdownKernelObjects();
                shutdownNetworkServices();
                setNativeMmioHandlers(nullptr, nullptr);
                shutdownXmaServices(owner);
                Doritos::Graphics::shutdownTitleGraphics();
            }
        } services{&guest};
        initializeMemoryServices();
        setNativeMmioHandlers(xmaReadMmio, xmaWriteMmio);
        initializeEnvironment(image);
        const auto saveRoot = std::filesystem::current_path() / "saves";
        Doritos::Graphics::initializeVideoSettings(saveRoot / "video-settings.ini");
        initializeFilesystem(game, saveRoot, assetOverrides);
        initializeUserServices(saveRoot);
        PPCContext ctx{};
        guest.initializeThread(ctx, image.default_stack_size);
        registerMainThread(ctx);
        currentContext = &ctx;
        if (checkOnly) { fprintf(stderr, "[CHECK] Encrypted image, native mapping and thread storage initialized.\n"); return 0; }
        const auto inputs = verifiedRendererInputs(game, shaders, packageCache);
        retainFilesystemPackage(inputs.package);
        TitleCloseCoordinator close([owner = &guest] {
            requestAudioServicesStop(owner);
            // User setters persist and flush before reporting success. Joining
            // the monitor and taking its profile lock also completes an active
            // synchronous profile transaction before process termination.
            shutdownUserServices();
            const auto package = filesystemPackageCacheStats();
            fprintf(stderr, "[PACKAGE CACHE] resident_bytes=%llu reads=%llu read_bytes=%llu\n",
                package.residentBytes, package.readCalls, package.readBytes);
        });
        Doritos::Graphics::prepareTitleGraphics(inputs.windowsCache, inputs.xenonCache, inputs.nativeExtension,inputs.xenonSourceExtension,close.event());
        if (image.entry_point < PPC_CODE_BASE || image.entry_point >= PPC_CODE_BASE + PPC_CODE_SIZE) fail("Entry point outside translated code");
        auto* entry = PPC_LOOKUP_FUNC(guest.base(), image.entry_point);
        if (!entry) fail("Title entry has no compiled function mapping");
        fprintf(stderr, "[BOOT] calling compiled guest entry 0x%08X\n", image.entry_point);
        try {
            return runGuest(ctx, guest.base(), entry);
        } catch (const std::exception& error) {
            // A title failure can unwind while guest workers are still alive.
            // Report it before the service destructor deliberately stops the
            // process to preserve their memory mapping during teardown.
            fprintf(stderr, "[NATIVE ERROR] %s; guest_lr=%08X stack=%08X r3=%016llX r4=%016llX r29=%016llX r30=%016llX\n",
                error.what(), uint32_t(ctx.lr), ctx.r1.u32,
                static_cast<unsigned long long>(ctx.r3.u64), static_cast<unsigned long long>(ctx.r4.u64),
                static_cast<unsigned long long>(ctx.r29.u64), static_cast<unsigned long long>(ctx.r30.u64));
            fflush(stderr); fflush(stdout);
            throw;
        }
    } catch (const std::exception& error) {
        fprintf(stderr, "Startup stopped: %s\n", error.what());
        return 1;
    }
}
