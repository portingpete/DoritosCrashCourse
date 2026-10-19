#include "runtime/native/runtime.h"
#include "runtime/native/user_services.h"
#include "runtime/native/kernel_objects.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <future>
#include <stdexcept>
#include <thread>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
using namespace Doritos::Native;
namespace {
void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
struct StopProfile { ~StopProfile() { shutdownUserServices(); } };
struct TestDesktop {
    HDESK handle = nullptr;
    TestDesktop() {
        const auto name = L"DoritosProfileContract-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        // No SWITCHDESKTOP access is requested. This desktop is never exposed
        // to the user and no system keyboard/mouse or foreground API is used.
        handle = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
            DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
        require(handle != nullptr, "private non-input desktop creation");
    }
    ~TestDesktop() { if (handle) CloseDesktop(handle); }
};
HWND desktopDialog(HDESK desktop) {
    HWND result = nullptr;
    EnumDesktopWindows(desktop, [](HWND window, LPARAM parameter) -> BOOL {
        DWORD process = 0; wchar_t type[64]{};
        if (GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId() &&
            GetClassNameW(window, type, 64) && !wcscmp(type, L"DoritosNativeSigninDialog")) {
            *reinterpret_cast<HWND*>(parameter) = window; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}
uint32_t privateSigninDialog(HDESK desktop, PPCContext& original, bool confirm) {
    std::promise<uint32_t> completed;
    auto result = completed.get_future();
    std::jthread worker([&] {
        HWND owner = nullptr;
        try {
            require(SetThreadDesktop(desktop), "isolated UI thread desktop");
            WNDCLASSW type{}; type.lpfnWndProc = DefWindowProcW; type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"DoritosNativeTitleWindow";
            require(RegisterClassW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "isolated test title class");
            owner = CreateWindowW(type.lpszClassName, L"Native profile contract parent", WS_OVERLAPPEDWINDOW,
                0, 0, 640, 480, nullptr, nullptr, type.hInstance, nullptr);
            require(owner != nullptr, "isolated test owner window");
            ShowWindow(owner, SW_SHOWNOACTIVATE);
            PPCContext ctx = original;
            ctx.lr = 0; // Explicit chooser coverage remains outside the exact splash request.
            ctx.r3.u64 = 1; ctx.r4.u64 = 0x01000000; ctx.r5.u64 = 0xffffffff; ctx.r6.u64 = 0xfffffffe;
            require(userServicesImport(ctx, memory->base(), "XamShowSigninUI"), "actual native modal UI dispatch");
            require(ctx.r5.u32 == 0xffffffff && ctx.r6.u32 == 0xfffffffe, "real modal UI has no unpassed output arguments");
            const uint32_t status = ctx.r3.u32;
            require(DestroyWindow(owner), "isolated owned title window destruction"); owner = nullptr;
            // This fresh UI worker now exits; it performs no work on the user
            // desktop. Win32/IME may retain system helper windows until thread
            // termination, preventing SetThreadDesktop even after our owned
            // dialog/title are destroyed. Join releases the private association.
            completed.set_value(status);
        } catch (...) {
            if (owner) DestroyWindow(owner);
            completed.set_exception(std::current_exception());
        }
    });
    struct CloseOnFailure {
        HDESK desktop;
        ~CloseOnFailure() { if (auto window = desktopDialog(desktop)) PostMessageW(window, WM_CLOSE, 0, 0); }
    } cleanup{desktop};
    HWND dialog = nullptr;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline && result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        const auto created = desktopDialog(desktop);
        if (created && IsWindowVisible(created) && GetDlgItem(created, IDOK) && GetDlgItem(created, IDCANCEL)) {
            dialog = created; break;
        }
        Sleep(1);
    }
    if (!dialog && result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) return result.get();
    require(dialog != nullptr, "real modal profile dialog created on private desktop");
    DWORD process = 0; HWND owner = GetWindow(dialog, GW_OWNER); wchar_t ownerType[64]{};
    require(owner && GetWindowThreadProcessId(owner, &process) && process == GetCurrentProcessId() &&
        GetClassNameW(owner, ownerType, 64) && !wcscmp(ownerType, L"DoritosNativeTitleWindow"),
        "actual chooser uses a same-process genuine title-class owner");
    require(IsWindowVisible(dialog) && GetDlgItem(dialog, IDOK) && GetDlgItem(dialog, IDCANCEL),
        "real modal template creates visible confirmation and cancellation controls on isolated desktop");
    wchar_t text[32768]{};
    require(GetDlgItemTextW(dialog, 100, text, 32768) && wcsstr(text, L"Windows account:") && wcsstr(text, L"local play and saves"),
        "actual dialog identifies the Windows local account and scope");
    require(LOWORD(SendMessageW(dialog, DM_GETDEFID, 0, 0)) == IDOK,
        "actual confirmation is the native default dialog action");
    // Test-only window messages remain on the inactive desktop. The production
    // modal message loop must handle Enter itself; no SendInput or game tap.
    require(PostMessageW(dialog, confirm ? WM_KEYDOWN : WM_CLOSE, confirm ? VK_RETURN : 0, 0),
        "isolated confirmation/cancel message dispatch");
    require(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "real modal action completes");
    const uint32_t status = result.get();
    worker.join();
    return status;
}
void musicOwnershipContract(Memory& guest, PPCContext& ctx) {
    constexpr std::array<std::pair<uint32_t, uint32_t>, 24> originalWords{{
        {0x824CF70C,0x90810054},{0x824CF714,0x90610050},{0x824CF718,0x38C00000},
        {0x824CF71C,0x90A10058},{0x824CF720,0x38A10050},{0x824CF724,0x6084001B},
        {0x824CF728,0x386000FA},{0x824CF72C,0x48527071},{0x824CF730,0x2C030000},
        {0x824CF77C,0x38A10054},{0x824CF780,0x38810050},{0x824CF784,0x38600002},
        {0x824CF788,0x4BFFFF79},{0x824CF790,0x40820028},{0x824CF794,0x81610050},
        {0x824CF79C,0x409A0014},{0x824CF7A0,0x81610054},{0x824CF7AC,0x419A0008},
        {0x824CF7B0,0x39600000},{0x824CF7B4,0x917F0000},{0x8257A768,0x4BF55001},
        {0x8257A778,0x419A000C},{0x8257A77C,0x93BF02E0},{0x8257A784,0x917F02E0}}};
    for (const auto& [address, word] : originalWords)
        require(guest.read32(address) == word,
            "original XMP packet, title-ownership BOOL and FMOD music-mute inversion instruction oracle");
    const PPCContext caller = ctx;
    const uint32_t storage = guest.allocate(4096), stack = storage + 64, packet = stack + 80;
    const uint32_t controller = stack + 192, locked = stack + 196;
    const auto arguments = [&] {
        ctx = caller; ctx.r1.u64 = stack; ctx.lr = 0x824CF730;
        ctx.r3.u64 = 0xfa; ctx.r4.u64 = 0x0007001b; ctx.r5.u64 = packet; ctx.r6.u64 = 0;
        guest.write32(packet, 2); guest.write32(packet + 4, controller); guest.write32(packet + 8, locked);
    };
    std::array<uint8_t, 320> before{};
    for (const uint32_t previous : {0u, 1u, 0xffffffffu}) {
        memset(guest.base() + storage, 0xa5, before.size()); arguments();
        guest.write32(controller, previous); guest.write32(locked, previous);
        memcpy(before.data(), guest.base() + storage, before.size());
        const PPCContext incoming = ctx;
        require(userServicesImport(ctx, guest.base(), "XMsgInProcessCall") && ctx.r3.u64 == 0,
            "native title music playback ownership query succeeds");
        const bool originalTitleOwnsMusic = !guest.read32(controller) && !guest.read32(locked);
        require(originalTitleOwnsMusic,
            "original XMP wrapper enables title-owned music instead of the FMOD dashboard mute path");
        memset(before.data() + controller - storage, 0, 8);
        PPCContext expected = incoming; expected.r3.u64 = 0;
        require(!memcmp(&ctx, &expected, sizeof(ctx)) &&
            !memcmp(before.data(), guest.base() + storage, before.size()),
            "music query updates only the two original DWORD outputs and return value");
    }
    for (unsigned unknown = 0; unknown < 12; ++unknown) {
        arguments();
        if (unknown == 0) ctx.lr += 4;
        else if (unknown == 1) ctx.r3.u64 += 0x100000000ull;
        else if (unknown == 2) ctx.r4.u64 += 0x100000000ull;
        else if (unknown == 3) ctx.r5.u64 += 0x100000000ull;
        else if (unknown == 4) ctx.r6.u64 = 12;
        else if (unknown == 5) ctx.r6.u64 = 0x100000000ull;
        else if (unknown == 6) ctx.r1.u64 += 0x100000000ull;
        else if (unknown == 7) { ++ctx.r1.u64; ++ctx.r5.u64; }
        else if (unknown == 8) guest.write32(packet, 0);
        else if (unknown == 9) guest.write32(packet + 4, packet);
        else if (unknown == 10) guest.write32(packet + 8, controller);
        else ctx.r4.u64 += 1;
        const PPCContext incoming = ctx;
        memcpy(before.data(), guest.base() + storage, before.size());
        require(!userServicesImport(ctx, guest.base(), "XMsgInProcessCall") &&
            !memcmp(&ctx, &incoming, sizeof(ctx)) && !memcmp(before.data(), guest.base() + storage, before.size()),
            "unobserved XMP caller, client, message, high bits or aliased outputs remain unsupported without mutation");
    }
    arguments(); memcpy(before.data(), guest.base() + storage, before.size());
    DWORD prior = 0, discarded = 0;
    require(VirtualProtect(guest.base() + storage, 4096, PAGE_READONLY, &prior), "read-only music output fixture");
    const bool handled = userServicesImport(ctx, guest.base(), "XMsgInProcessCall");
    const uint32_t status = ctx.r3.u32;
    require(VirtualProtect(guest.base() + storage, 4096, prior, &discarded), "restore music output fixture access");
    require(handled && status == 0x80070057 && !memcmp(before.data(), guest.base() + storage, before.size()),
        "inaccessible nested music outputs reject before either output changes");
    ctx = caller; guest.release(storage);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        const auto image = loadOriginalXex(argv[1]);
        Memory guest(image); memory = &guest;
        wchar_t temp[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, temp) > 0, "native profile temporary root");
        const auto saves = std::filesystem::path(temp) / (L"DoritosNativeProfileContract-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        StopProfile stop;
        initializeUserServices(saves, false);
        PPCContext ctx{}; guest.initializeThread(ctx, 0x100000); currentContext = &ctx;
        const uint32_t output = guest.allocate(4096);
        auto call = [&](const char* name) {
            require(userServicesImport(ctx, guest.base(), name), "unrecognized native local user operation");
            return ctx.r3.u32;
        };
        memset(guest.base() + output, 0xa5, 128);
        ctx.r3.u64 = 4; ctx.r4.u64 = 0; ctx.r5.u64 = 5;
        ctx.r6.u64 = output; ctx.r7.u64 = output + 32; ctx.r8.u64 = 96;
        require(call("XamAvatarInitialize") == uint32_t(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)),
            "unavailable native OS-avatar service returns an actual unsupported failure");
        require(std::all_of(guest.base() + output, guest.base() + output + 128, [](uint8_t value) { return value == 0xa5; }),
            "failed avatar initialization creates no callback, metadata or geometry output");
        call("XamAvatarShutdown");
        guest.write32(output, 0);
        std::array<uint8_t, 128> messagePayload{};
        memcpy(messagePayload.data(), guest.base() + output, messagePayload.size());
        ctx.r3.u64 = 0xfe; ctx.r4.u64 = 0x00021012; ctx.r5.u64 = output; ctx.r6.u64 = 0;
        const uint32_t messageStatus = call("XMsgInProcessCall");
        require(messageStatus == uint32_t(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) &&
            (uint32_t(int32_t(messageStatus) >> 31) & ERROR_FUNCTION_FAILED) == ERROR_FUNCTION_FAILED,
            "unavailable proven XAM message reaches the original signed-failure branch");
        require(!memcmp(messagePayload.data(), guest.base() + output, messagePayload.size()) &&
            ctx.r4.u32 == 0x00021012 && ctx.r5.u32 == output && ctx.r6.u32 == 0,
            "failed synchronous XAM message invents no output and preserves input arguments");
        ctx.r3.u64 = 0xfe; ctx.r4.u64 = 0x00021013;
        require(!userServicesImport(ctx, guest.base(), "XMsgInProcessCall") && ctx.r3.u32 == 0xfe,
            "unknown XAM message remains an explicit unsupported import");
        ctx.r4.u64 = 0x00021012; ctx.r6.u64 = 4;
        require(!userServicesImport(ctx, guest.base(), "XMsgInProcessCall") && ctx.r3.u32 == 0xfe,
            "unverified synchronous message argument layout remains unsupported");
        musicOwnershipContract(guest, ctx);
        auto xuid = [&](uint32_t mask = 1, uint32_t user = 0) {
            ctx.r3.u64 = user; ctx.r4.u64 = mask; ctx.r5.u64 = output;
            return call("XamUserGetXUID");
        };
        require(xuid() == 0, "activated native local profile exists");
        const uint64_t id = guest.read64(output);
        require((id >> 56) == 0xe0, "persisted identity uses offline local namespace");
        require(xuid(7) == 0 && guest.read64(output) == id, "original offline SDK mask7 resolves the same local identity");
        require(xuid(2) == 0x80070525 && guest.read64(output) == 0, "native profile cannot claim an online identity");
        require(xuid(7, 1) == 0x80070525 && guest.read64(output) == 0, "absent local profiles remain absent");
        require(nativeSelectedLocalProfile(0) == 0 && nativeSelectedLocalProfile(1) == 0,
            "account initialization does not claim an explicit native profile choice");
        ctx.r3.u64 = 0; require(call("XamUserGetSigninState") == 1, "actual account is locally signed in");
        ctx.r3.u64 = 1; require(call("XamUserGetSigninState") == 0, "additional users are not fabricated");
        require(guest.read32(0x82327D18) == 0x91610060 && guest.read32(0x82327D24) == 0x2b030000 &&
            guest.read32(0x82327D28) == 0x409a0038 && guest.read32(0x82327D2C) == 0x81410060 &&
            guest.read32(0x82327D38) == 0x997f0004 && guest.read32(0x82327D48) == 0x997f0005 &&
            guest.read32(0x8241AC40) == 0x4e800020,
            "actual license caller prezeros its mask and branches on error before mask reads or cached license writes");
        const auto licenseReservations = guest.reservationSnapshot();
        const uint64_t licenseSelection = nativeSelectedLocalProfile(0);
        auto licenseArguments = [&] {
            ctx.lr = 0x82327D24; ctx.r3.u64 = output + 4; ctx.r4.u64 = 0;
            ctx.r5.u64 = 0x123456789abcdef0ull; ctx.r6.u64 = output + 64;
            ctx.r7.u64 = 7; ctx.r8.u64 = 8; ctx.r9.u64 = 0x82060000; ctx.r10.u64 = 0x3fa;
        };
        for (const uint32_t previous : {0u, 1u, 0xffffffffu}) {
            memset(guest.base() + output, 0xa5, 128);
            guest.write32(output + 4, previous);
            std::array<uint8_t, 128> before{}; memcpy(before.data(), guest.base() + output, before.size());
            licenseArguments(); const PPCContext licenseCaller = ctx;
            require(call("XamContentGetLicenseMask") == ERROR_NOT_SUPPORTED && ctx.r3.u64 == ERROR_NOT_SUPPORTED &&
                !memcmp(before.data(), guest.base() + output, before.size()) && ctx.lr == licenseCaller.lr &&
                ctx.r4.u64 == licenseCaller.r4.u64 && ctx.r5.u64 == licenseCaller.r5.u64 &&
                ctx.r6.u64 == licenseCaller.r6.u64 && ctx.r7.u64 == licenseCaller.r7.u64 &&
                ctx.r8.u64 == licenseCaller.r8.u64 && ctx.r9.u64 == licenseCaller.r9.u64 && ctx.r10.u64 == licenseCaller.r10.u64,
                "absent license service returns a real DWORD error, preserves mask and neighbors, and ignores unpassed output arguments");
        }
        licenseArguments(); ctx.r4.u64 = output + 64;
        const PPCContext asynchronousLicenseCaller = ctx;
        require(!userServicesImport(ctx, guest.base(), "XamContentGetLicenseMask") &&
            ctx.r3.u64 == asynchronousLicenseCaller.r3.u64 && ctx.r4.u64 == asynchronousLicenseCaller.r4.u64 &&
            std::all_of(guest.base() + output + 64, guest.base() + output + 128, [](uint8_t value) { return value == 0xa5; }),
            "unproved overlapped license query remains unsupported without creating callback or completion output");
        licenseArguments(); ctx.lr = 0x82327D28;
        require(!userServicesImport(ctx, guest.base(), "XamContentGetLicenseMask") && ctx.r3.u64 == output + 4,
            "unobserved license caller remains an explicit native boundary");
        licenseArguments(); ctx.r3.u64 = 0;
        require(call("XamContentGetLicenseMask") == ERROR_INVALID_PARAMETER,
            "proven synchronous license ABI rejects a null mask pointer");
        const uint32_t licenseTail = output + guest.allocationSize(output) - 3;
        memset(guest.base() + licenseTail, 0xa5, 3);
        licenseArguments(); ctx.r3.u64 = licenseTail;
        bool invalidLicense = false;
        try { call("XamContentGetLicenseMask"); } catch (const NativeFailure&) { invalidLicense = true; }
        require(invalidLicense && ctx.r3.u64 == licenseTail &&
            std::all_of(guest.base() + licenseTail, guest.base() + licenseTail + 3, [](uint8_t value) { return value == 0xa5; }),
            "license query requires its complete owned DWORD span and stops before partial output mutation");
        require(guest.reservationSnapshot() == licenseReservations && nativeSelectedLocalProfile(0) == licenseSelection &&
            xuid() == 0 && guest.read64(output) == id,
            "license absence creates no storage or identity, entitlement or profile-selection transition");
        ctx.r3.u64 = 0;
        require(call("XamUserGetSigninState") == 1, "license query failure cannot manufacture Live sign-in");
        require(guest.read32(0x82255C88) == 0x2b030000 && guest.read32(0x82255C8C) == 0x409a00b0 &&
            guest.read32(0x8241AC40) == 0x4e800020,
            "actual achievement creation failure exits through the original no-op before reading enumeration outputs");
        const auto achievementReservations = guest.reservationSnapshot();
        auto achievementArguments = [&] {
            ctx.r3.u64 = 0; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0xffffffff;
            ctx.r7.u64 = 0; ctx.r8.u64 = 12; ctx.r9.u64 = output; ctx.r10.u64 = output + 4;
        };
        memset(guest.base() + output, 0xa5, 128);
        achievementArguments(); const PPCContext achievementCaller = ctx;
        require(call("XamUserCreateAchievementEnumerator") == ERROR_NOT_SUPPORTED && ctx.r3.u64 == ERROR_NOT_SUPPORTED &&
            ctx.r4.u64 == achievementCaller.r4.u64 && ctx.r5.u64 == achievementCaller.r5.u64 &&
            ctx.r6.u64 == achievementCaller.r6.u64 && ctx.r7.u64 == achievementCaller.r7.u64 &&
            ctx.r8.u64 == achievementCaller.r8.u64 && ctx.r9.u64 == achievementCaller.r9.u64 && ctx.r10.u64 == achievementCaller.r10.u64 &&
            std::all_of(guest.base() + output, guest.base() + output + 128, [](uint8_t value) { return value == 0xa5; }) &&
            guest.reservationSnapshot() == achievementReservations,
            "unavailable achievement provider returns a DWORD failure without metadata, enumerator storage, output or argument mutation");
        for (const auto unknown : {0u, 1u, 2u, 3u, 4u}) {
            achievementArguments();
            if (unknown == 0) ctx.r5.u64 = 0x100000000ull;
            else if (unknown == 1) ctx.r3.u64 = 0xfffe07d1;
            else if (unknown == 2) ctx.r6.u64 = 0;
            else if (unknown == 3) ctx.r7.u64 = 1;
            else ctx.r8.u64 = 24;
            const uint32_t title = ctx.r3.u32;
            require(!userServicesImport(ctx, guest.base(), "XamUserCreateAchievementEnumerator") && ctx.r3.u32 == title,
                "unverified achievement title, flags, range or full-width XUID remains an explicit native boundary");
        }
        for (const auto invalid : {0u, 1u, 2u, 3u}) {
            achievementArguments();
            if (invalid == 0) ctx.r4.u64 = 4;
            else if (invalid == 1) ctx.r8.u64 = 0;
            else if (invalid == 2) ctx.r9.u64 = 0;
            else ctx.r10.u64 = 0;
            require(call("XamUserCreateAchievementEnumerator") == ERROR_INVALID_PARAMETER,
                "verified achievement ABI rejects invalid index, zero count or null output pointers");
        }
        require(std::all_of(guest.base() + output, guest.base() + output + 128, [](uint8_t value) { return value == 0xa5; }) &&
            nativeSelectedLocalProfile(0) == 0 && xuid() == 0 && guest.read64(output) == id,
            "failed achievement operations preserve identity, selection and every caller output sentinel");
        ctx.r3.u64 = 0; require(call("XamUserGetSigninState") == 1, "achievement absence does not fabricate Live state");
        require(guest.read32(0x821A32A0) == 0x3c800100 && guest.read32(0x821A32A4) == 0x80610050 &&
            guest.read32(0x821A32A8) == 0x4823d791 && guest.read32(0x821A32AC) == 0x7c7e1b78 &&
            guest.read32(0x823E0A38) == 0x48615d04 && guest.read32(0x823A9870) == 0x823a9894 &&
            guest.read32(0x823A9894) == 0x38600001 && guest.read32(0x823A9898) == 0x4bffe139 &&
            guest.read32(0x823A7A4C) == 0x807f0004 && guest.read32(0x823A7A50) == 0x480393c9 &&
            guest.read32(0x823A7A58) == 0x387f0448 && guest.read32(0x823A7A5C) == 0x4beae885 &&
            guest.read32(0x822562F8) == 0x80630004 && guest.read32(0x822562FC) == 0x4818ab1d &&
            guest.read32(0x8225633C) == 0x93df0438 && guest.read32(0x823E0E18) == 0x48615a04,
            "original exact GUI request and event0A route re-query the real SDK and refresh embedded profile settings");
        ctx.r3.u64 = 1; ctx.r4.u64 = 6;
        const uint32_t dialogListener = call("XamNotifyCreateListener");
        auto dialogEvent = lookupObject(dialogListener);
        require(dialogEvent && WaitForSingleObject(dialogEvent->handle, 0) == WAIT_TIMEOUT,
            "local chooser contract starts with an actual empty system listener");
        memset(guest.base() + output, 0xa5, 128);
        for (const uint32_t count : {0u, 3u, 5u}) {
            ctx.r3.u64 = count; ctx.r4.u64 = 0x01000000; ctx.r5.u64 = output; ctx.r6.u64 = 0xffffffff;
            require(call("XamShowSigninUI") == ERROR_INVALID_PARAMETER && ctx.r5.u32 == output && ctx.r6.u32 == 0xffffffff,
                "signin UI validates users_needed without reading unpassed guest outputs or overlapped arguments");
        }
        for (const uint32_t count : {2u, 4u}) {
            ctx.r3.u64 = count; ctx.r4.u64 = 0x01000000;
            require(call("XamShowSigninUI") == ERROR_NOT_SUPPORTED,
                "single actual Windows account cannot satisfy a multiple-account chooser");
        }
        require(guest.read32(0x8222AA94) == 0x3c800100 && guest.read32(0x8222AA98) == 0x38600001 &&
            guest.read32(0x8222AA9C) == 0x60840002,
            "original alternate chooser caller supplies one user and exact flags01000002");
        for (const uint32_t count : {0u,1u,2u,3u,4u,5u}) {
            ctx.lr = 0x8222AAA4; ctx.r3.u64 = count; ctx.r4.u64 = 0x01000002;
            ctx.r5.u64 = output; ctx.r6.u64 = 0x123456789abcdef0ull;
            PPCContext expected = ctx;
            expected.r3.u64 = (!count || count == 3 || count > 4) ? ERROR_INVALID_PARAMETER : ERROR_NOT_SUPPORTED;
            const auto reservations = guest.reservationSnapshot();
            require(userServicesImport(ctx, guest.base(), "XamShowSigninUI") &&
                !memcmp(&ctx,&expected,sizeof(ctx)) && guest.reservationSnapshot() == reservations &&
                nativeSelectedLocalProfile(0) == 0 &&
                std::all_of(guest.base() + output,guest.base() + output + 128,[](uint8_t value) { return value == 0xa5; }),
                "unavailable online chooser returns an API error without UI, identity or unpassed output");
        }
        ctx.r3.u64 = 1; ctx.r4.u64 = 0x01000003;
        const PPCContext unknownChooser = ctx;
        require(!userServicesImport(ctx,guest.base(),"XamShowSigninUI") && !memcmp(&ctx,&unknownChooser,sizeof(ctx)),
            "unknown chooser flag combinations remain unhandled without mutation");
        ctx.lr = 0x821A32AC; ctx.r3.u64 = 1; ctx.r4.u64 = 0x01000000;
        require(call("XamShowSigninUI") == ERROR_INVALID_WINDOW_HANDLE && nativeSelectedLocalProfile(0) == 0 &&
            std::all_of(guest.base() + output, guest.base() + output + 128, [](uint8_t value) { return value == 0xa5; }),
            "absent exact-owned title window cannot manufacture UI completion, selection or guest-buffer output");
        require(WaitForSingleObject(dialogEvent->handle, 0) == WAIT_TIMEOUT,
            "failed or unsupported dialog creates no UI lifecycle or sign-in notification");
        require(xuid() == 0 && guest.read64(output) == id,
            "failed native chooser preserves the actual persisted offline account identity");
        require(closeObject(dialogListener), "native chooser listener closure");
        {
            TestDesktop desktop;
            for (const bool confirm : {false, true, false}) {
                const uint64_t before = nativeSelectedLocalProfile(0);
                ctx.r3.u64 = 1; ctx.r4.u64 = 6;
                const uint32_t listener = call("XamNotifyCreateListener");
                const uint32_t status = privateSigninDialog(desktop.handle, ctx, confirm);
                require(status == (confirm ? ERROR_SUCCESS : ERROR_CANCELLED), "actual modal Use/Cancel status");
                require(nativeSelectedLocalProfile(0) == (confirm ? id : before),
                    "only explicit real native confirmation selects a profile; cancellation preserves prior selection");
                auto notification = [&](uint32_t match, uint32_t expectedParameter) {
                    ctx.r3.u64 = listener; ctx.r4.u64 = match; ctx.r5.u64 = output + 128; ctx.r6.u64 = output + 132;
                    const uint32_t found = call("XNotifyGetNext");
                    if (found) require(guest.read32(output + 128) == match && guest.read32(output + 132) == expectedParameter,
                        "actual modal lifecycle notification payload");
                    return found;
                };
                require(notification(9, 1) && notification(9, 0) && !notification(9, 0),
                    "actual creation and destruction publish exactly one native UI lifecycle pair");
                require(notification(10, 1) == (confirm && !before),
                    "actual selection transition alone publishes sign-in; canceled dialogs never invent one");
                require(!notification(10, 1), "real selection emits no duplicate sign-in event");
                ctx.r3.u64 = 0;
                require(call("XamUserGetSigninState") == 1 && xuid() == 0 && guest.read64(output) == id,
                    "real confirmation preserves offline state1 and persisted local ID");
                require(closeObject(listener), "isolated modal listener closure");
            }
            shutdownUserServices();
            initializeUserServices(saves, false);
            require(nativeSelectedLocalProfile(0) == 0 && xuid() == 0 && guest.read64(output) == id,
                "service restart ends UI selection while preserving durable local identity");
        }
        ctx.r3.u64 = 0; ctx.r4.u64 = 0; ctx.r5.u64 = output;
        require(call("XamUserGetSigninInfo") == 0 && guest.read64(output) == id && guest.read32(output + 12) == 1,
            "native signin information has the proven BE40-byte layout");
        std::array<char, 32768> username{}; DWORD usernameLength = DWORD(username.size());
        require(GetUserNameA(username.data(), &usernameLength), "actual Windows account name");
        std::array<char, 16> expected{}; memcpy(expected.data(), username.data(), std::min<size_t>(strlen(username.data()), 15));
        require(memcmp(guest.base() + output + 24, expected.data(), 16) == 0, "signin name comes from actual Windows account");
        std::array<uint8_t, 40> signin{}; memcpy(signin.data(), guest.base() + output, signin.size());
        ctx.r3.u64 = 0; ctx.r4.u64 = 2; ctx.r5.u64 = output;
        require(call("XamUserGetSigninInfo") == 0 && !memcmp(signin.data(), guest.base() + output, signin.size()) &&
            guest.read32(output + 8) == 0 && guest.read32(output + 12) == 1,
            "actual title flags2 reports the same local nonguest profile without Live authorization");
        ctx.r3.u64 = 0; ctx.r4.u64 = 1; ctx.r5.u64 = output;
        bool unknownSignin = false;
        try { call("XamUserGetSigninInfo"); } catch (const NativeFailure&) { unknownSignin = true; }
        require(unknownSignin && !memcmp(signin.data(), guest.base() + output, signin.size()),
            "unverified signin query flags do not create identity output");
        memset(guest.base() + output, 0xa5, 128);
        for (const uint32_t category : {1u, 4u}) {
            ctx.r3.u64 = 0; ctx.r4.u64 = category; ctx.r5.u64 = output;
            require(call("XamUserGetDeviceContext") == uint32_t(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) &&
                int32_t(ctx.r3.u32) < 0 && ctx.r4.u32 == category && ctx.r5.u32 == output &&
                std::all_of(guest.base() + output, guest.base() + output + 128, [](uint8_t value) { return value == 0xa5; }),
                "unavailable native voice context returns negative HRESULT and leaves Xbox device output untouched");
        }
        ctx.r3.u64 = 0; ctx.r4.u64 = 1; ctx.r5.u64 = 0;
        require(call("XamUserGetDeviceContext") == uint32_t(E_INVALIDARG), "device context requires an output DWORD pointer");
        ctx.r3.u64 = 0; ctx.r4.u64 = 2; ctx.r5.u64 = output;
        require(!userServicesImport(ctx, guest.base(), "XamUserGetDeviceContext") && ctx.r3.u32 == 0,
            "unverified device categories remain explicit import boundaries");
        ctx.r3.u64 = 0; ctx.r4.u64 = output; ctx.r5.u64 = 16;
        require(call("XamUserGetName") == 0 && memcmp(guest.base() + output, expected.data(), 16) == 0, "local profile ANSI name ABI");
        ctx.r3.u64 = 0; ctx.r4.u64 = 0xfc; ctx.r5.u64 = output;
        require(call("XamUserCheckPrivilege") == ERROR_NOT_LOGGED_ON && guest.read32(output) == 0,
            "offline privilege checks do not grant Xbox Live authorization");
        const uint32_t ids = output + 256, size = output + 300, profileBuffer = output + 512, record = output + 2048;
        const uint32_t payload = output + 3000, overlap = output + 3040;
        auto readProfile = [&](uint32_t count, uint32_t title = 0xfffe07d1, uint32_t user = 0, uint32_t buffer = 0, uint32_t completion = 0) {
            ctx.r3.u64 = title; ctx.r4.u64 = user; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
            ctx.r7.u64 = count; ctx.r8.u64 = ids; ctx.r9.u64 = size; ctx.r10.u64 = buffer;
            guest.write32(ctx.r1.u32 + 84, completion);
            return call("XamUserReadProfileSettings");
        };
        guest.write32(ids, 0x1004000c); guest.write32(ids + 4, 0x1004000d); guest.write32(ids + 8, 0x1004000e);
        guest.write32(size, 0);
        require(readProfile(3, 0xfffe07d1, 255) == ERROR_INSUFFICIENT_BUFFER && guest.read32(size) == 128,
            "actual audio profile size query permits user255 and nine-argument ABI");
        require(readProfile(3, 0xfffe07d1, 0, profileBuffer) == 0 && guest.read32(profileBuffer) == 3 &&
            guest.read32(profileBuffer + 4) == profileBuffer + 8 && guest.read32(profileBuffer + 8 + 2 * 40 + 32) == 100,
            "persisted native port communication preferences preserve original title defaults");
        guest.write32(ids, 0x63e83fff); guest.write32(size, 0);
        require(readProfile(1, 0) == ERROR_INSUFFICIENT_BUFFER && guest.read32(size) == 1048, "native binary profile capacity layout");
        require(readProfile(1, 0, 0, profileBuffer) == 0 && guest.read32(profileBuffer + 8) == 0 && guest.base()[profileBuffer + 32] == 0xff,
            "new title settings remain genuinely unset");
        memset(guest.base() + record, 0, 40); guest.write32(record + 16, 0x63e83fff); guest.base()[record + 24] = 6;
        const std::array<uint8_t, 5> saved{{0x12, 0, 0x34, 0xff, 0x56}};
        memcpy(guest.base() + payload, saved.data(), saved.size());
        guest.write32(record + 32, uint32_t(saved.size())); guest.write32(record + 36, payload);
        ctx.r3.u64 = output + 3100; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
        require(synchronizationImport(ctx, guest.base(), "NtCreateEvent") && ctx.r3.u32 == 0, "actual profile completion event");
        const uint32_t completionEvent = guest.read32(output + 3100);
        memset(guest.base() + overlap, 0, 28); guest.write32(overlap + 12, completionEvent);
        auto writeProfile = [&](uint32_t completion) {
            ctx.r3.u64 = 0; ctx.r4.u64 = 0; ctx.r5.u64 = 1; ctx.r6.u64 = record; ctx.r7.u64 = completion;
            return call("XamUserWriteProfileSettings");
        };
        require(writeProfile(overlap) == ERROR_IO_PENDING && guest.read32(overlap) == 0 &&
            WaitForSingleObject(lookupObject(completionEvent)->handle, 0) == WAIT_OBJECT_0,
            "native durable profile write completes and signals its real event");
        guest.base()[payload] = 0x99; guest.write32(overlap + 16, 0x82001000);
        bool rejected = false;
        try { writeProfile(overlap); } catch (const NativeFailure&) { rejected = true; }
        require(rejected, "unimplemented profile completion APC is rejected before persistence");
        guest.write32(overlap + 16, 0);
        require(readProfile(1, 0, 0, profileBuffer) == 0 && guest.read32(profileBuffer + 8) == 2 &&
            guest.read32(profileBuffer + 40) == saved.size() && memcmp(guest.base() + guest.read32(profileBuffer + 44), saved.data(), saved.size()) == 0,
            "native title profile reads actual saved bytes and rejected callback does not alter them");
        require(guest.read32(0x829F67FC) == 0x010001f7 &&
            guest.read32(0x823E0CF8) == 0x48615b05 && guest.read32(0x823E0CFC) == 0x2c030000 &&
            guest.read32(0x823E0D00) == 0x4080000c && guest.read32(0x823E0D04) == 0x3860065b &&
            guest.read32(0x823E0DBC) == 0x48615a41 && guest.read32(0x823E0DC0) == 0x2c030000 &&
            guest.read32(0x823E0DC4) == 0x4080000c && guest.read32(0x823E0DC8) == 0x3860065b,
            "actual XGI context/property imports are ordinal503 and negative returns immediately become1627");
        const PPCContext beforeAppRequest = ctx;
        const uint32_t appStack = ctx.r1.u32, appRequest = appStack + 80;
        const uint32_t appRange = guest.allocate(4096), appData = appRange + 64, appOverlap = appRange + 256;
        const uint32_t guestLastError = guest.read32(ctx.r13.u32 + 256) + 352;
        const uint32_t previousGuestError = guest.read32(guestLastError);
        std::array<uint8_t, 128> previousStack{};
        memcpy(previousStack.data(), guest.base() + appStack + 64, previousStack.size());
        memset(guest.base() + appRange, 0x6d, 384);
        guest.write32(appOverlap, ERROR_IO_PENDING); guest.write32(appOverlap + 12, completionEvent);
        std::array<uint8_t, 384> appDataSentinel{};
        memcpy(appDataSentinel.data(), guest.base() + appRange, appDataSentinel.size());
        require(ResetEvent(lookupObject(completionEvent)->handle), "real event rearmed for rejected XGI requests");
        const auto appReservations = guest.reservationSnapshot();
        const uint64_t appSelection = nativeSelectedLocalProfile(0);
        auto appArguments = [&](bool context) {
            ctx = beforeAppRequest;
            memset(guest.base() + appStack + 64, 0xa5, 128);
            guest.write32(appRequest, 0); guest.write64(appRequest + 8, 0);
            guest.write32(appRequest + 16, context ? 0x8001 : 0x10040002);
            guest.write32(appRequest + 20, context ? 7 : 4);
            guest.write32(appRequest + 24, appData);
            ctx.lr = context ? 0x823E0CFC : 0x823E0DC0;
            ctx.r3.u64 = 0xfb; ctx.r4.u64 = context ? 0x000b0006 : 0x000b0007;
            ctx.r5.u64 = 0; ctx.r6.u64 = appRequest; ctx.r7.u64 = context ? 24 : 32;
            ctx.r8.u64 = 0xfeed1234; ctx.r9.u64 = appOverlap; ctx.r10.u64 = appData;
            guest.write32(guestLastError, 0x5a11);
        };
        for (const bool context : {false, true}) {
            appArguments(context);
            std::array<uint8_t, 128> input{}; memcpy(input.data(), guest.base() + appStack + 64, input.size());
            PPCContext expectedRequest = ctx;
            expectedRequest.r3.u64 = uint32_t(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
            SetLastError(0x5a11);
            const uint32_t status = call("XMsgStartIORequest");
            const DWORD hostError = GetLastError();
            require(status == 0x80070032 && int32_t(status) < 0 && status != ERROR_IO_PENDING &&
                !memcmp(&ctx, &expectedRequest, sizeof(ctx)) &&
                !memcmp(input.data(), guest.base() + appStack + 64, input.size()) &&
                !memcmp(appDataSentinel.data(), guest.base() + appRange, appDataSentinel.size()) &&
                guest.read32(guestLastError) == 0x5a11 && hostError == 0x5a11 &&
                WaitForSingleObject(lookupObject(completionEvent)->handle, 0) == WAIT_TIMEOUT,
                "absent XGI provider rejects the exact synchronous wrapper without payload/input/profile/error/event/completion mutation; poison padding is valid");
            for (uint32_t unknown = 0; unknown != 13; ++unknown) {
                appArguments(context);
                if (unknown == 0) ctx.lr = 0x82894A94;
                else if (unknown == 1) ctx.r3.u64 = 0xfe;
                else if (unknown == 2) ctx.r4.u64 = context ? 0x000b0007 : 0x000b0006;
                else if (unknown == 3) --ctx.r7.u64;
                else if (unknown == 4) ctx.r5.u64 = appOverlap;
                else if (unknown == 5) guest.write32(appRequest, 1);
                else if (unknown == 6) guest.write64(appRequest + 8, id);
                else if (unknown == 7) ctx.r6.u64 += 4;
                else if (unknown == 8) ctx.r1.u64 += 0x100000000ull;
                else if (unknown == 9) ctx.r6.u64 += 0x100000000ull;
                else if (unknown == 10) ctx.r5.u64 = 0x100000000ull;
                else if (unknown == 11) ctx.r7.u64 += 0x100000000ull;
                else ctx.r6.u64 = 0;
                const PPCContext incomingRequest = ctx;
                memcpy(input.data(), guest.base() + appStack + 64, input.size());
                SetLastError(0x5a11);
                const bool handled = userServicesImport(ctx, guest.base(), "XMsgStartIORequest");
                const DWORD untouchedError = GetLastError();
                require(!handled && !memcmp(&ctx, &incomingRequest, sizeof(ctx)) &&
                    !memcmp(input.data(), guest.base() + appStack + 64, input.size()) &&
                    !memcmp(appDataSentinel.data(), guest.base() + appRange, appDataSentinel.size()) &&
                    guest.read32(guestLastError) == 0x5a11 && untouchedError == 0x5a11 &&
                    WaitForSingleObject(lookupObject(completionEvent)->handle, 0) == WAIT_TIMEOUT,
                    "unknown XGI provenance, async overlap, local user, XUID or layout remains unhandled with full state preserved");
            }
            appArguments(context);
            const uint32_t partial = appRange + guest.allocationSize(appRange) - (context ? 23 : 31);
            ctx.r1.u64 = partial - 80; ctx.r6.u64 = partial;
            const PPCContext incompleteRequest = ctx;
            bool invalidRequest = false;
            try { call("XMsgStartIORequest"); } catch (const NativeFailure&) { invalidRequest = true; }
            require(invalidRequest && !memcmp(&ctx, &incompleteRequest, sizeof(ctx)) &&
                !memcmp(appDataSentinel.data(), guest.base() + appRange, appDataSentinel.size()),
                "exact XGI wrapper requires its complete owned payload before any request mutation");
        }
        memcpy(guest.base() + appStack + 64, previousStack.data(), previousStack.size());
        guest.write32(guestLastError, previousGuestError); ctx = beforeAppRequest;
        require(guest.reservationSnapshot() == appReservations && nativeSelectedLocalProfile(0) == appSelection &&
            readProfile(1, 0, 0, profileBuffer) == 0 &&
            !memcmp(guest.base() + guest.read32(profileBuffer + 44), saved.data(), saved.size()),
            "unavailable context/property registration preserves actual durable title settings and local selection");
        require(guest.read32(0x823E1050) == 0x7cc83378 && guest.read32(0x823E1054) == 0x64860001 &&
            guest.read32(0x823E1058) == 0x64850002 && guest.read32(0x823E105C) == 0x38e00001 &&
            guest.read32(0x823E1060) == 0x38800000 && guest.read32(0x823E1064) == 0x48615808 &&
            guest.read32(0x82232750) == 0x3963fc1b && guest.read32(0x8223275C) == 0x995e0020,
            "original gamer-tile wrapper maps overlap to r8 and the real updater distinguishes IO_PENDING from immediate error");
        require(guest.read32(0x823E13C0) == 0x486154dd && guest.read32(0x823E13C4) == 0x38600000,
            "original queue cleanup calls app-IO cancellation then discards its result");
        // Allocate the final test-owned region after real event storage. A
        // prior region may have a valid adjacent allocation, which does not
        // make a cross-boundary read an invalid guest span.
        const uint32_t tileRange = guest.allocate(4096);
        const auto tileReservations = guest.reservationSnapshot();
        const uint64_t tileSelection = nativeSelectedLocalProfile(0);
        const uint32_t tileOverlap = output + 64;
        require(ResetEvent(lookupObject(completionEvent)->handle), "real completed profile event can be rearmed for absent-tile contract");
        memset(guest.base() + output + 32, 0xa5, 96);
        guest.write32(tileOverlap, 0); guest.write32(tileOverlap + 12, completionEvent);
        std::array<uint8_t, 96> tileSentinel{};
        memcpy(tileSentinel.data(), guest.base() + output + 32, tileSentinel.size());
        auto tileArguments = [&] {
            ctx.lr = 0x82232750; ctx.r3.u64 = 0; ctx.r4.u64 = 0;
            ctx.r5.u64 = 0x0002000d; ctx.r6.u64 = 0x0001000d; ctx.r7.u64 = 1;
            ctx.r8.u64 = tileOverlap; ctx.r9.u64 = 0xffffffff; ctx.r10.u64 = output + 32;
        };
        tileArguments(); const PPCContext tileCaller = ctx;
        require(call("XamWriteGamerTile") == ERROR_NOT_SUPPORTED && ctx.r3.u64 != ERROR_IO_PENDING &&
            !memcmp(tileSentinel.data(), guest.base() + output + 32, tileSentinel.size()) &&
            WaitForSingleObject(lookupObject(completionEvent)->handle, 0) == WAIT_TIMEOUT && ctx.lr == tileCaller.lr &&
            ctx.r4.u64 == tileCaller.r4.u64 && ctx.r5.u64 == tileCaller.r5.u64 && ctx.r6.u64 == tileCaller.r6.u64 &&
            ctx.r7.u64 == tileCaller.r7.u64 && ctx.r8.u64 == tileCaller.r8.u64 &&
            ctx.r9.u64 == tileCaller.r9.u64 && ctx.r10.u64 == tileCaller.r10.u64,
            "missing dashboard provider rejects a tile immediately without upload, async completion, event signal or unpassed output mutation");
        for (const uint32_t unknown : {0u, 1u, 2u, 3u, 4u, 5u}) {
            tileArguments();
            if (unknown == 0) ctx.lr = 0;
            else if (unknown == 1) ctx.r3.u64 = 1;
            else if (unknown == 2) ctx.r4.u64 = 0xfffe07d1;
            else if (unknown == 3) ctx.r6.u64 = 0x0001000c;
            else if (unknown == 4) ctx.r7.u64 = 0;
            else ctx.r8.u64 = 0;
            const uint64_t incoming = ctx.r3.u64;
            require(!userServicesImport(ctx, guest.base(), "XamWriteGamerTile") && ctx.r3.u64 == incoming &&
                !memcmp(tileSentinel.data(), guest.base() + output + 32, tileSentinel.size()),
                "unverified gamer-tile caller, title, local user, ID shape or synchronous form remains an explicit boundary");
        }
        auto cancelTile = [&](uint32_t request) {
            ctx.lr = 0x823E13C4; ctx.r3.u64 = request; ctx.r4.u64 = 1;
            return call("XMsgCancelIORequest");
        };
        require(cancelTile(tileOverlap) == ERROR_NOT_SUPPORTED && ctx.r4.u64 == 1 &&
            !memcmp(tileSentinel.data(), guest.base() + output + 32, tileSentinel.size()) &&
            WaitForSingleObject(lookupObject(completionEvent)->handle, 0) == WAIT_TIMEOUT,
            "absent app-IO cleanup returns truthful failure without signaling or writing the rejected tile overlap");
        // The original error drain also visits unissued queued entries. No
        // pointer-tracking or manufactured pending job may reject that case.
        memset(guest.base() + tileOverlap, 0, 28);
        require(cancelTile(tileOverlap) == ERROR_NOT_SUPPORTED &&
            std::all_of(guest.base() + tileOverlap, guest.base() + tileOverlap + 28, [](uint8_t byte) { return byte == 0; }),
            "unissued queue entries have no native job to cancel and remain unchanged");
        ctx.lr = 0; ctx.r3.u64 = tileOverlap; ctx.r4.u64 = 1;
        require(!userServicesImport(ctx, guest.base(), "XMsgCancelIORequest") && ctx.r3.u64 == tileOverlap,
            "unproved cancellation caller remains unhandled");
        ctx.lr = 0x823E13C4; ctx.r3.u64 = tileOverlap; ctx.r4.u64 = 0;
        require(!userServicesImport(ctx, guest.base(), "XMsgCancelIORequest") && ctx.r3.u64 == tileOverlap,
            "unproved cancellation wait shape remains unhandled");
        require(guest.read32(0x82889818) == 0x38800001 && guest.read32(0x8288981C) == 0x386300a0 &&
            guest.read32(0x82889820) == 0x3bdf0054 && guest.read32(0x82889824) == 0x4816d079 &&
            guest.read32(0x82889828) == 0x817f0054,
            "original voice reset passes wait1/owner+160 and ignores result before loading its real worker state+84");
        const uint32_t voiceOwner = tileRange + 64;
        memset(guest.base() + voiceOwner - 16, 0xa5, 512);
        guest.write32(voiceOwner + 84, 0); guest.write32(voiceOwner + 200, 0);
        memset(guest.base() + voiceOwner + 160, 0, 28);
        std::array<uint8_t, 512> voiceSentinel{};
        memcpy(voiceSentinel.data(), guest.base() + voiceOwner - 16, voiceSentinel.size());
        auto voiceResetArguments = [&] {
            ctx.lr = 0x82889828; ctx.r31.u64 = voiceOwner; ctx.r30.u64 = voiceOwner + 84;
            ctx.r3.u64 = voiceOwner + 160; ctx.r4.u64 = 1;
        };
        voiceResetArguments(); const PPCContext voiceResetCaller = ctx;
        require(call("XMsgCancelIORequest") == ERROR_NOT_SUPPORTED &&
            !memcmp(voiceSentinel.data(), guest.base() + voiceOwner - 16, voiceSentinel.size()) &&
            ctx.r4.u64 == voiceResetCaller.r4.u64 && ctx.r30.u64 == voiceResetCaller.r30.u64 &&
            ctx.r31.u64 == voiceResetCaller.r31.u64 && ctx.lr == voiceResetCaller.lr &&
            WaitForSingleObject(lookupObject(completionEvent)->handle, 0) == WAIT_TIMEOUT,
            "genuine inactive voice reset reports unsupported without worker-state, owner, overlap, event or completion mutation");
        for (const uint32_t unknown : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u}) {
            memcpy(guest.base() + voiceOwner - 16, voiceSentinel.data(), voiceSentinel.size());
            voiceResetArguments();
            if (unknown < 3) guest.write32(voiceOwner + 84, unknown + 1);
            else if (unknown == 3) guest.write32(voiceOwner + 200, 1);
            else if (unknown == 4) guest.write32(voiceOwner + 160, ERROR_IO_PENDING);
            else if (unknown == 5) guest.write32(voiceOwner + 172, completionEvent);
            else if (unknown == 6) ctx.r3.u64 += 4;
            else if (unknown == 7) ctx.r30.u64 += 4;
            else if (unknown == 8) ctx.r4.u64 = 0;
            else ctx.lr = 0x82889fac;
            std::array<uint8_t, 512> before{}; memcpy(before.data(), guest.base() + voiceOwner - 16, before.size());
            const uint64_t pointer = ctx.r3.u64;
            require(!userServicesImport(ctx, guest.base(), "XMsgCancelIORequest") && ctx.r3.u64 == pointer &&
                !memcmp(before.data(), guest.base() + voiceOwner - 16, before.size()) &&
                WaitForSingleObject(lookupObject(completionEvent)->handle, 0) == WAIT_TIMEOUT,
                "real active/pending voice states, submitted events or unproved ownership/wait/caller stay explicit and unchanged");
        }
        const uint32_t partialVoiceOwner = tileRange + guest.allocationSize(tileRange) - 479;
        ctx.lr = 0x82889828; ctx.r31.u64 = partialVoiceOwner; ctx.r30.u64 = partialVoiceOwner + 84;
        ctx.r3.u64 = partialVoiceOwner + 160; ctx.r4.u64 = 1;
        bool invalidVoiceOwner = false;
        try { call("XMsgCancelIORequest"); } catch (const NativeFailure&) { invalidVoiceOwner = true; }
        require(invalidVoiceOwner && ctx.r3.u64 == partialVoiceOwner + 160,
            "pre-initialization reset requires the complete original 480-byte owned voice object");
        const uint32_t tileTail = tileRange + guest.allocationSize(tileRange) - 27;
        memset(guest.base() + tileTail, 0xa5, 27);
        for (const bool cancel : {false, true}) {
            bool invalid = false;
            try {
                if (cancel) cancelTile(tileTail);
                else { tileArguments(); ctx.r8.u64 = tileTail; call("XamWriteGamerTile"); }
            } catch (const NativeFailure&) { invalid = true; }
            require(invalid && std::all_of(guest.base() + tileTail, guest.base() + tileTail + 27, [](uint8_t byte) { return byte == 0xa5; }),
                "tile rejection and no-job cleanup require the complete owned 28-byte overlap before any mutation");
        }
        require(guest.reservationSnapshot() == tileReservations && nativeSelectedLocalProfile(0) == tileSelection &&
            xuid() == 0 && guest.read64(output) == id,
            "dashboard tile absence creates no storage or local identity/selection/entitlement transition");
        ctx.r3.u64 = 0;
        require(call("XamUserGetSigninState") == 1 && readProfile(1, 0, 0, profileBuffer) == 0 &&
            memcmp(guest.base() + guest.read32(profileBuffer + 44), saved.data(), saved.size()) == 0,
            "gamer-tile rejection preserves actual already saved title profile bytes and offline state");
        require(closeObject(completionEvent), "native profile completion event closure");
        ctx.r3.u64 = 1; ctx.r4.u64 = 6;
        const uint32_t listener = call("XamNotifyCreateListener");
        auto native = lookupObject(listener);
        require(native && native->kind == ObjectKind::Notification && WaitForSingleObject(native->handle, 0) == WAIT_TIMEOUT,
            "listener owns a genuine initially unsignaled native event");
        ctx.r3.u64 = uint64_t(1) << 32; ctx.r4.u64 = 6;
        const uint32_t filtered = call("XamNotifyCreateListener");
        auto filteredNative = lookupObject(filtered);
        auto next = [&](uint32_t handle, uint32_t match = 0x0a) {
            ctx.r3.u64 = handle; ctx.r4.u64 = match; ctx.r5.u64 = output; ctx.r6.u64 = output + 4;
            return call("XNotifyGetNext");
        };
        require(next(listener) == 0 && guest.read32(output) == 0 && guest.read32(output + 4) == 0,
            "creation does not inject sign-in or network notifications");
        const uint32_t duplicate = duplicateObject(listener);
        require(duplicate && closeObject(listener), "native listener registry duplication");
        shutdownUserServices();
        require(WaitForSingleObject(native->handle, 0) == WAIT_OBJECT_0, "real profile deactivation wakes native listener waiters");
        require(WaitForSingleObject(filteredNative->handle, 0) == WAIT_TIMEOUT, "64-bit listener masks exclude unmatched system events");
        require(next(duplicate, 0x09) == 0 && WaitForSingleObject(native->handle, 0) == WAIT_OBJECT_0,
            "matching query cannot consume a different queued notification");
        require(next(duplicate) == 1 && guest.read32(output) == 0x0a && guest.read32(output + 4) == 1,
            "profile lifecycle notification preserves the actual changed-user bitmask");
        require(WaitForSingleObject(native->handle, 0) == WAIT_TIMEOUT, "draining final native notification resets listener event");
        require(xuid() == 0x80070525 && guest.read64(output) == 0, "deactivated local profile is unavailable");
        require(nativeSelectedLocalProfile(0) == 0, "profile deactivation cannot retain a native selection identity");
        initializeUserServices(saves);
        require(xuid() == 0 && guest.read64(output) == id, "persisted native local ID survives profile restart");
        require(nativeSelectedLocalProfile(0) == id && nativeSelectedLocalProfile(1) == 0,
            "default native startup selects only the actual persisted Windows account without a chooser");
        ctx.r3.u64 = 0;
        require(call("XamUserGetSigninState") == 1,
            "automatic local selection retains actual offline SDK state1");
        ctx.r3.u64 = 0; ctx.r4.u64 = 0; ctx.r5.u64 = output;
        require(call("XamUserGetSigninInfo") == 0 && guest.read64(output) == id && guest.read32(output + 12) == 1 &&
            !(guest.read32(output + 8) & 2), "automatic selection reports the actual offline non-guest identity");
        require(readProfile(1, 0, 0, profileBuffer) == 0 && memcmp(guest.base() + guest.read32(profileBuffer + 44), saved.data(), saved.size()) == 0,
            "actual native profile bytes survive service restart");
        require(next(duplicate) == 1 && guest.read32(output) == 0x0a, "real profile reactivation reaches existing listeners");
        ctx.r3.u64 = 1; ctx.r4.u64 = 6;
        const uint32_t selectedListener = call("XamNotifyCreateListener");
        auto selectedEvent = lookupObject(selectedListener);
        require(selectedEvent && WaitForSingleObject(selectedEvent->handle, 0) == WAIT_TIMEOUT,
            "post-startup original system listener begins without an invented selection event");
        memset(guest.base() + output + 512, 0xa5, 128);
        auto splashRequest = [&] {
            ctx.lr = 0x821A32AC; ctx.r3.u64 = 1; ctx.r4.u64 = 0x01000000;
            ctx.r5.u64 = output + 512; ctx.r6.u64 = 0x123456789abcdef0ull;
            ctx.r7.u64 = 0x100000001ull; ctx.r8.u64 = 0xffffffffffffffffull;
        };
        for (unsigned request = 0; request < 2; ++request) {
            splashRequest(); const PPCContext before = ctx;
            const auto reservations = guest.reservationSnapshot();
            require(call("XamShowSigninUI") == ERROR_SUCCESS && ctx.lr == before.lr &&
                ctx.r4.u64 == before.r4.u64 && ctx.r5.u64 == before.r5.u64 && ctx.r6.u64 == before.r6.u64 &&
                ctx.r7.u64 == before.r7.u64 && ctx.r8.u64 == before.r8.u64 && ctx.r1.u64 == before.r1.u64 &&
                nativeSelectedLocalProfile(0) == id && nativeSelectedLocalProfile(1) == 0 &&
                guest.reservationSnapshot() == reservations &&
                std::all_of(guest.base() + output + 512, guest.base() + output + 640, [](uint8_t value) { return value == 0xa5; }),
                "exact splash request uses actual selected account without creating a modal, output or identity");
            require(next(selectedListener, 9) == 0 && next(selectedListener, 10) == 1 && guest.read32(output + 4) == 1 &&
                next(selectedListener, 10) == 0 && WaitForSingleObject(selectedEvent->handle, 0) == WAIT_TIMEOUT,
                "each real splash request queues one current-account refresh without UI lifecycle notifications");
            ctx.r3.u64 = 0;
            require(call("XamUserGetSigninState") == 1 && xuid() == 0 && guest.read64(output) == id,
                "splash completion preserves genuine offline state1 and the actual persisted identity");
        }
        splashRequest(); ctx.lr = 0x8222AAA4; ctx.r4.u64 = 0x01000002;
        PPCContext unavailableExpected = ctx; unavailableExpected.r3.u64 = ERROR_NOT_SUPPORTED;
        const auto unavailableReservations = guest.reservationSnapshot();
        require(userServicesImport(ctx,guest.base(),"XamShowSigninUI") &&
            !memcmp(&ctx,&unavailableExpected,sizeof(ctx)) && nativeSelectedLocalProfile(0) == id &&
            guest.reservationSnapshot() == unavailableReservations &&
            std::all_of(guest.base() + output + 512,guest.base() + output + 640,[](uint8_t value) { return value == 0xa5; }) &&
            next(selectedListener,9) == 0 && next(selectedListener,10) == 0 &&
            WaitForSingleObject(selectedEvent->handle,0) == WAIT_TIMEOUT,
            "online-only failure preserves actual selected local account without UI events or fake refresh");
        splashRequest(); ctx.lr += 4;
        require(call("XamShowSigninUI") == ERROR_INVALID_WINDOW_HANDLE && next(selectedListener, 10) == 0,
            "unobserved callers keep the owned chooser contract and do not refresh the account");
        splashRequest(); ctx.r3.u64 = 0x100000001ull;
        require(call("XamShowSigninUI") == ERROR_INVALID_WINDOW_HANDLE && next(selectedListener, 10) == 0,
            "high-bit user-count aliases cannot select the exact splash completion path");
        splashRequest(); ctx.r4.u64 = 0x100000001000000ull;
        require(call("XamShowSigninUI") == ERROR_INVALID_WINDOW_HANDLE && next(selectedListener, 10) == 0,
            "high-bit flag aliases cannot select the exact splash completion path");
        require(closeObject(selectedListener), "selected-account refresh listener closure");
        require(closeObject(duplicate) && closeObject(filtered), "native listener closure");
        shutdownUserServices();
        printf("Native local profile and notification contracts passed\n");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "Native user contract: %s\n", error.what());
        return 1;
    }
}
