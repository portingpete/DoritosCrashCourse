#include "runtime/native/title_lifecycle.h"
#include "renderer/native/title_window.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace Doritos::Native;
using namespace Doritos::Graphics;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct RevealProbe {
    std::atomic<unsigned> startup{0}, game{0};
    HANDLE barrier = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~RevealProbe() { if (barrier) CloseHandle(barrier); }
};
RevealProbe* revealProbe = nullptr;
bool observeReveal(HWND, UINT message, WPARAM wparam, LPARAM) {
    if (message == WM_APP + 1) {
        if (wparam) ++revealProbe->game; else ++revealProbe->startup;
        return true; // Exercise the real queue without showing a test window.
    }
    if (message == WM_APP + 2) { SetEvent(revealProbe->barrier); return true; }
    return false;
}
void verifyRevealRequests() {
    RevealProbe probe;
    require(probe.barrier != nullptr, "create hidden reveal queue barrier");
    revealProbe = &probe;
    {
        TitleWindow window(nullptr, observeReveal);
        for (unsigned frame = 0; frame < 100; ++frame) window.revealStartup();
        std::thread a([&] { for (unsigned frame = 0; frame < 1000; ++frame) window.reveal(); });
        std::thread b([&] { for (unsigned frame = 0; frame < 1000; ++frame) window.reveal(); });
        a.join(); b.join();
        for (unsigned frame = 0; frame < 100; ++frame) window.revealStartup();
        require(PostMessageW(window.handle(), WM_APP + 2, 0, 0) &&
            WaitForSingleObject(probe.barrier, 1000) == WAIT_OBJECT_0, "drain real hidden reveal message queue");
        require(probe.startup.load() == 1 && probe.game.load() == 1,
            "repeated frame notifications flooded startup/game visibility messages");
        require(!IsWindowVisible(window.handle()), "hidden reveal fixture unexpectedly became visible");
    }
    probe.startup = probe.game = 0;
    {
        TitleWindow window(nullptr, observeReveal);
        window.reveal();
        window.revealStartup(); // A new window has its own state; game wins over late startup.
        require(PostMessageW(window.handle(), WM_APP + 2, 0, 0) &&
            WaitForSingleObject(probe.barrier, 1000) == WAIT_OBJECT_0, "drain new window reveal queue");
        require(probe.startup.load() == 0 && probe.game.load() == 1,
            "reveal state leaked between windows or late startup downgraded the game");
    }
    revealProbe = nullptr;
}
void durableMarker(const std::filesystem::path& path, const char* text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "close callback executes once");
    DWORD written = 0;
    bool success = WriteFile(file, text, DWORD(strlen(text)), &written, nullptr) && written == strlen(text) && FlushFileBuffers(file);
    CloseHandle(file); require(success, "close callback flushes its persistence marker");
}
[[noreturn]] void child(const std::wstring& mode, const std::filesystem::path& marker) {
    TitleCloseCoordinator coordinator([&] {
        if (mode == L"--child-success") {
            Sleep(75); // Simulate completion of an in-progress profile transaction.
            durableMarker(marker, "audio-stopped;profile-flushed");
        } else {
            durableMarker(marker, "audio-stopped");
            if (mode == L"--child-error") throw std::runtime_error("isolated service failure");
            if (mode != L"--child-stdio") Sleep(INFINITE); // A stuck service must not keep this child alive.
        }
    }, mode == L"--child-timeout" || mode == L"--child-stdio" ? 150 : 1000);
    // A live worker owns its storage until the whole process is terminated.
    // In the stdio case it also retains a FILE lock: even the final log flush
    // must remain covered by the close deadline.
    if (mode == L"--child-stdio") {
        FILE* redirected = nullptr;
        const auto output = std::filesystem::path(marker.wstring() + L".stdout");
        require(_wfreopen_s(&redirected, output.c_str(), L"wb", stdout) == 0 && redirected,
            "blocked stdio fixture owns an actual output file");
        // UCRT returns before locking an empty/non-flushable stream. Populate
        // its buffered writer state so the final fflush must acquire the lock.
        require(fputs("buffered close oracle", stdout) >= 0, "blocked stdio fixture has pending output");
    }
    std::atomic<bool> stdioLocked{false};
    std::thread guestWorker([&] {
        if (mode == L"--child-stdio") { _lock_file(stdout); stdioLocked.store(true); }
        for (;;) Sleep(10);
    });
    if (mode == L"--child-stdio") while (!stdioLocked.load()) SwitchToThread();
    TitleWindow window(coordinator.event());
    require(window.handle() && !IsWindowVisible(window.handle()), "isolated close fixture stays hidden");
    require(PostMessageW(window.handle(), WM_CLOSE, 0, 0), "post actual native window close");
    PostMessageW(window.handle(), WM_CLOSE, 0, 0); // Manual-reset request is idempotent.
    for (;;) Sleep(1000);
}
void verifyChild(const wchar_t* executable, const wchar_t* mode, DWORD expected, const char* contents) {
    wchar_t root[MAX_PATH]{}; require(GetTempPathW(MAX_PATH, root), "close contract temporary directory");
    auto marker = std::filesystem::path(root) / (L"DoritosNativeClose-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L".txt");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " + mode + L" \"" + marker.wstring() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    const uint64_t began = GetTickCount64();
    require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
        &startup, &process), "start isolated close lifecycle child");
    CloseHandle(process.hThread);
    DWORD wait = WaitForSingleObject(process.hProcess, 5000), status = 0xffffffff;
    if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 99); WaitForSingleObject(process.hProcess, 1000); }
    bool queried = GetExitCodeProcess(process.hProcess, &status) != FALSE; CloseHandle(process.hProcess);
    std::ifstream input(marker, std::ios::binary); std::string actual((std::istreambuf_iterator<char>(input)), {}); input.close();
    DeleteFileW(marker.c_str());
    DeleteFileW((marker.wstring() + L".stdout").c_str());
    if (wait != WAIT_OBJECT_0 || !queried || status != expected)
        fwprintf(stderr, L"Close child mode=%ls wait=%08lX queried=%u exit=%lu expected=%lu elapsed=%llu ms\n",
            mode, wait, unsigned(queried), status, expected, GetTickCount64() - began);
    require(wait == WAIT_OBJECT_0 && queried && status == expected, "closing the actual window terminates child and all guest workers");
    require(actual == contents, "close completes the persistence barrier before successful termination");
    require(GetTickCount64() - began < 5000, "process close remains bounded even when its service stalls");
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3) child(argv[1], argv[2]);
        require(argc == 1, "close contract arguments");
        verifyRevealRequests();
        std::atomic<unsigned> callbacks{0};
        { TitleCloseCoordinator coordinator([&] { ++callbacks; }); }
        require(callbacks.load() == 0, "normal scope teardown disarms close coordinator without running services");
        HANDLE request = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(request != nullptr, "window close notification fixture");
        { TitleWindow window(request); }
        require(WaitForSingleObject(request, 0) == WAIT_TIMEOUT, "internal window destruction does not request process termination");
        HANDLE observer = nullptr;
        require(DuplicateHandle(GetCurrentProcess(), request, GetCurrentProcess(), &observer, 0, FALSE, DUPLICATE_SAME_ACCESS),
            "retain close observer");
        {
            TitleWindow window(request); CloseHandle(request); request = nullptr;
            require(PostMessageW(window.handle(), WM_CLOSE, 0, 0), "external close of retained event fixture");
            require(WaitForSingleObject(observer, 1000) == WAIT_OBJECT_0, "window retains its close event after caller handle closes");
        }
        CloseHandle(observer);
        wchar_t executable[32768]{};
        require(GetModuleFileNameW(nullptr, executable, 32768), "close contract executable path");
        verifyChild(executable, L"--child-success", 0, "audio-stopped;profile-flushed");
        verifyChild(executable, L"--child-timeout", 2, "audio-stopped");
        verifyChild(executable, L"--child-error", 1, "audio-stopped");
        verifyChild(executable, L"--child-stdio", 2, "audio-stopped");
        printf("Native title close contracts passed: actual hidden window close, worker termination, persistence ordering and bounded fallback.\n");
        return 0;
    } catch (const std::exception& error) { fprintf(stderr, "Native title close contract: %s\n", error.what()); return 1; }
}
