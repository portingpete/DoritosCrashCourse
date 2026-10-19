#include "user_services.h"
#include "runtime.h"
#include "kernel_objects.h"
#include "input_services.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace Doritos::Native {
struct NativeNotificationState {
    std::mutex mutex;
    std::deque<std::pair<uint32_t, uint32_t>> queue;
    uint64_t mask = 0;
    uint32_t maxVersion = 0;
};
namespace {
constexpr uint32_t signinChanged = 0x0a, inputChanged = 0x12;
constexpr uint32_t invalidArgument = 0x80070057, noSuchUser = 0x80070525;
struct StoredSetting {
    uint8_t type = 0xff;
    std::vector<uint8_t> bytes;
    bool operator==(const StoredSetting&) const = default;
};
using Settings = std::map<uint32_t, StoredSetting>;
struct Profile {
    std::mutex mutex;
    std::condition_variable wake;
    std::jthread monitor;
    std::vector<std::weak_ptr<KernelObject>> listeners;
    std::array<bool, 4> connected{};
    std::filesystem::path directory;
    std::string name;
    uint64_t id = 0;
    bool active = false;
    bool selected = false, signinPending = false;
    HWND signinWindow = nullptr;
    Settings systemSettings, titleSettings;
} profile;
struct NativeHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~NativeHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void requireSpan(uint32_t address, uint32_t size) {
    if (!memory || !address || !memory->valid(address, size))
        throw NativeFailure("Local profile operation has an invalid guest buffer");
}
uint32_t decode32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}
void append32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(uint8_t(value >> shift));
}
uint32_t scalarSize(uint8_t type) {
    if (type == 1 || type == 5) return 4;
    if (type == 2 || type == 3 || type == 7) return 8;
    return 0;
}
bool titleSpecific(uint32_t id) { return (id & 0x3f00) == 0x3f00; }
bool systemPreference(uint32_t id) { return id >= 0x1004000c && id <= 0x1004000e; }
bool validSetting(uint32_t id, const StoredSetting& setting) {
    const uint32_t capacity = (id >> 16) & 0xfff;
    if (setting.type != (id >> 28) || (!titleSpecific(id) && !systemPreference(id))) return false;
    const uint32_t scalar = scalarSize(setting.type);
    if (scalar) return capacity == scalar && setting.bytes.size() == scalar;
    if (setting.type != 4 && setting.type != 6) return false;
    return setting.bytes.size() <= capacity && (setting.type != 4 || !(setting.bytes.size() & 1));
}
Settings loadSettings(const std::filesystem::path& path, bool& existed) {
    NativeHandle file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) { existed = false; return {}; }
        throw NativeFailure("Cannot open native profile settings");
    }
    existed = true;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 12 || size.QuadPart > 0x100000)
        throw NativeFailure("Native profile settings have an invalid size");
    std::vector<uint8_t> bytes(size_t(size.QuadPart)); DWORD read = 0;
    if (!ReadFile(file.value, bytes.data(), DWORD(bytes.size()), &read, nullptr) || read != bytes.size() || memcmp(bytes.data(), "DSET0001", 8))
        throw NativeFailure("Cannot read native profile settings record");
    Settings result;
    const uint32_t count = decode32(bytes.data() + 8);
    if (count > 1024) throw NativeFailure("Native profile setting count is invalid");
    size_t offset = 12;
    for (uint32_t index = 0; index < count; ++index) {
        if (offset + 12 > bytes.size()) throw NativeFailure("Native profile setting header is truncated");
        const uint32_t id = decode32(bytes.data() + offset), type = decode32(bytes.data() + offset + 4), length = decode32(bytes.data() + offset + 8);
        offset += 12;
        if (type > 7 || length > 4095 || offset + length > bytes.size()) throw NativeFailure("Native profile setting payload is invalid");
        StoredSetting value{uint8_t(type), std::vector<uint8_t>(bytes.begin() + offset, bytes.begin() + offset + length)};
        if (!validSetting(id, value) || !result.emplace(id, std::move(value)).second)
            throw NativeFailure("Native profile setting identity is invalid");
        offset += length;
    }
    if (offset != bytes.size()) throw NativeFailure("Native profile settings contain an invalid trailing record");
    return result;
}
uint32_t persistSettings(const std::filesystem::path& path, const Settings& settings) {
    std::vector<uint8_t> bytes{'D','S','E','T','0','0','0','1'};
    append32(bytes, uint32_t(settings.size()));
    for (const auto& [id, value] : settings) {
        append32(bytes, id); append32(bytes, value.type); append32(bytes, uint32_t(value.bytes.size()));
        bytes.insert(bytes.end(), value.bytes.begin(), value.bytes.end());
    }
    if (settings.size() > 1024 || bytes.size() > 0x100000) return ERROR_FILE_TOO_LARGE;
    const auto pending = std::filesystem::path(path.wstring() + L".pending-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    NativeHandle file;
    file.value = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written = 0; uint32_t result = ERROR_SUCCESS;
    if (!WriteFile(file.value, bytes.data(), DWORD(bytes.size()), &written, nullptr)) result = GetLastError();
    else if (written != bytes.size()) result = ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(file.value)) result = GetLastError();
    if (!CloseHandle(file.value) && !result) result = GetLastError();
    file.value = INVALID_HANDLE_VALUE;
    if (!result && !MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) result = GetLastError();
    if (result) DeleteFileW(pending.c_str());
    return result;
}
void initializeSettings() {
    bool existed = false;
    profile.systemSettings = loadSettings(profile.directory / "preferences.bin", existed);
    if (!existed) {
        // These are this native port's communication preferences. The initial
        // values match the original title's packed 0x26404 defaults at
        // sub_8288A0E0; they are persisted settings, not claimed host Live data.
        for (const auto& [id, value] : std::array<std::pair<uint32_t, uint32_t>, 3>{{{0x1004000c, 0}, {0x1004000d, 0}, {0x1004000e, 100}}}) {
            StoredSetting setting{1, {}}; append32(setting.bytes, value); profile.systemSettings.emplace(id, std::move(setting));
        }
        if (persistSettings(profile.directory / "preferences.bin", profile.systemSettings)) throw NativeFailure("Cannot persist native port preferences");
    }
    profile.titleSettings = loadSettings(profile.directory / "title-settings.bin", existed);
}
struct Completion {
    uint32_t address = 0, handle = 0;
    std::shared_ptr<KernelObject> event;
    explicit Completion(uint32_t overlapped) : address(overlapped) {
        if (!address) return;
        requireSpan(address, 28);
        // Completion routines require a guest APC queue. Reject before any
        // write or disk transaction; never silently drop a requested callback.
        if (memory->read32(address + 16)) throw NativeFailure("Native profile completion APC is not implemented");
        handle = memory->read32(address + 12);
        if (handle) {
            event = lookupObject(handle);
            if (!event || event->kind != ObjectKind::Event) throw NativeFailure("Native profile completion event is invalid");
        }
    }
    uint32_t finish(uint32_t status) const {
        if (!address) return status;
        memory->write32(address + 4, 0); memory->write32(address + 24, status);
        memory->write32(address, status);
        if (event) {
            // Keep the actual event alive through completion and refresh any
            // original embedded dispatcher header through the kernel bridge.
            std::lock_guard lock(event->dispatcherMutex);
            if (!SetEvent(event->handle)) throw NativeFailure("Cannot signal native profile completion");
            if (event->guestAddress) {
                struct EventInfo { LONG type, state; } information{};
                using QueryEvent = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
                static const auto query = reinterpret_cast<QueryEvent>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryEvent"));
                if (!query || query(event->handle, 0, &information, sizeof(information), nullptr) < 0)
                    throw NativeFailure("Cannot query native profile completion event");
                memory->write32(event->guestAddress + 4, uint32_t(information.state));
            }
        }
        // The native disk work really completed above. Immediate completion of
        // an overlapped request remains a pending-return call in the Xbox ABI.
        return ERROR_IO_PENDING;
    }
};
// Notification IDs encode version in bits 16..24 and mask index in 25..30.
// Original sub_825FEEA8 requests maximum version six; sub_82251160 consumes
// actual system IDs 9..18 using output ID and parameter at r5 and r6.
void publishLocked(uint32_t id, uint32_t parameter) {
    const uint32_t maskIndex = (id >> 25) & 63, version = (id >> 16) & 511;
    for (auto it = profile.listeners.begin(); it != profile.listeners.end();) {
        auto object = it->lock();
        if (!object) { it = profile.listeners.erase(it); continue; }
        ++it;
        auto state = object->notificationState;
        if (!state || !(state->mask & (uint64_t(1) << maskIndex)) || version > state->maxVersion) continue;
        std::lock_guard lock(state->mutex);
        state->queue.emplace_back(id, parameter);
        if (!SetEvent(object->handle)) throw NativeFailure("Cannot signal native platform listener");
    }
}
constexpr wchar_t signinClass[] = L"DoritosNativeSigninDialog";
constexpr wchar_t titleClass[] = L"DoritosNativeTitleWindow";
bool titleOwner(HWND window) {
    DWORD process = 0;
    wchar_t type[64]{};
    return window && IsWindow(window) && IsWindowVisible(window) &&
        GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId() &&
        GetClassNameW(window, type, 64) && !std::wcscmp(type, titleClass);
}
HWND findTitleOwner() {
    HWND result = nullptr;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        if (!titleOwner(window)) return TRUE;
        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}
// Build only a Windows dialog description. These little-endian words are host
// UI data, not an Xbox structure or a replacement title resource.
void dialogWord(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(uint8_t(value)); bytes.push_back(uint8_t(value >> 8));
}
void dialogDword(std::vector<uint8_t>& bytes, uint32_t value) {
    dialogWord(bytes, uint16_t(value)); dialogWord(bytes, uint16_t(value >> 16));
}
void dialogString(std::vector<uint8_t>& bytes, const wchar_t* value) {
    while (*value) dialogWord(bytes, uint16_t(*value++));
    dialogWord(bytes, 0);
}
std::vector<uint8_t> signinTemplate() {
    std::vector<uint8_t> bytes;
    dialogDword(bytes, WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | DS_CENTER);
    dialogDword(bytes, 0); dialogWord(bytes, 3);
    for (const auto value : {0, 0, 270, 100}) dialogWord(bytes, uint16_t(value));
    dialogWord(bytes, 0); dialogString(bytes, signinClass);
    dialogString(bytes, L"Doritos Crash Course - Local Profile");
    dialogWord(bytes, 9); dialogString(bytes, L"Segoe UI");
    auto item = [&](DWORD style, WORD id, WORD type, short x, short y, short width, short height, const wchar_t* text) {
        while (bytes.size() & 3) bytes.push_back(0);
        dialogDword(bytes, style | WS_CHILD | WS_VISIBLE); dialogDword(bytes, 0);
        for (const auto value : {x, y, width, height}) dialogWord(bytes, uint16_t(value));
        dialogWord(bytes, id); dialogWord(bytes, 0xffff); dialogWord(bytes, type);
        dialogString(bytes, text); dialogWord(bytes, 0);
    };
    item(SS_LEFT, 100, 0x82, 12, 10, 246, 48, L"");
    item(WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 0x80, 72, 72, 108, 16, L"Use local profile");
    item(WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, 0x80, 186, 72, 72, 16, L"Cancel");
    return bytes;
}
struct SigninDialog {
    HWND owner = nullptr;
    uint64_t identity = 0;
    std::wstring description;
    std::exception_ptr failure;
    bool announced = false;
};
INT_PTR CALLBACK signinProcedure(HWND window, UINT message, WPARAM wparam, LPARAM parameter) {
    auto session = reinterpret_cast<SigninDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            session = reinterpret_cast<SigninDialog*>(parameter);
            SetWindowLongPtrW(window, DWLP_USER, parameter);
            if (!session || !titleOwner(session->owner) || GetWindow(window, GW_OWNER) != session->owner) {
                EndDialog(window, ERROR_INVALID_WINDOW_HANDLE); return TRUE;
            }
            if (!SetDlgItemTextW(window, 100, session->description.c_str()))
                throw NativeFailure("Cannot display actual Windows local-profile identity");
            {
                std::lock_guard lock(profile.mutex);
                if (!profile.active || profile.id != session->identity) { EndDialog(window, ERROR_NOT_LOGGED_ON); return TRUE; }
                profile.signinWindow = window;
                session->announced = true; publishLocked(0x09, 1);
            }
            SetFocus(GetDlgItem(window, IDOK));
            printf("[native-profile] Actual owned local-profile chooser opened; owner=%p dialog=%p\n", session->owner, window);
            fflush(stdout);
            return FALSE;
        }
        if (!session) return FALSE;
        if (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
            uint32_t status = ERROR_CANCELLED;
            if (LOWORD(wparam) == IDOK) {
                std::lock_guard lock(profile.mutex);
                if (!profile.active || profile.id != session->identity) status = ERROR_NOT_LOGGED_ON;
                else {
                    // This is a real native selection transition. It does not
                    // alter the SDK state 1 or any original Live predicate.
                    if (!profile.selected) { profile.selected = true; publishLocked(signinChanged, 1); }
                    status = ERROR_SUCCESS;
                }
            }
            EndDialog(window, status); return TRUE;
        }
        if (message == WM_CLOSE) { EndDialog(window, ERROR_CANCELLED); return TRUE; }
        if (message == WM_DESTROY) {
            std::lock_guard lock(profile.mutex);
            if (profile.signinWindow == window) profile.signinWindow = nullptr;
            if (session->announced) { session->announced = false; publishLocked(0x09, 0); }
        }
    } catch (...) {
        if (session) session->failure = std::current_exception();
        if (message != WM_DESTROY) EndDialog(window, ERROR_FUNCTION_FAILED);
        return TRUE;
    }
    return FALSE;
}
uint32_t showSignin(uint32_t usersNeeded) {
    // The observed SDK call has two arguments only. No guest output, event,
    // completion callback or XOVERLAPPED is consumed from later registers.
    if (!usersNeeded || usersNeeded > 4 || usersNeeded == 3) return ERROR_INVALID_PARAMETER;
    if (usersNeeded != 1) return ERROR_NOT_SUPPORTED;
    SigninDialog session;
    {
        std::lock_guard lock(profile.mutex);
        if (!profile.active) return ERROR_NOT_LOGGED_ON;
        if (profile.signinPending) return ERROR_BUSY;
        session.identity = profile.id;
        const int length = MultiByteToWideChar(CP_ACP, 0, profile.name.c_str(), -1, nullptr, 0);
        if (!length) throw NativeFailure("Cannot convert actual Windows profile name");
        std::wstring account(size_t(length), L'\0');
        if (!MultiByteToWideChar(CP_ACP, 0, profile.name.c_str(), -1, account.data(), length))
            throw NativeFailure("Cannot convert actual Windows profile name");
        account.resize(size_t(length - 1));
        session.description = L"Windows account: " + account + L"\r\n\r\nUse this account for local play and saves.";
        profile.signinPending = true;
    }
    struct Pending { ~Pending() { std::lock_guard lock(profile.mutex); profile.signinPending = false; } } pending;
    session.owner = findTitleOwner();
    if (!session.owner) return ERROR_INVALID_WINDOW_HANDLE;
    static std::once_flag registered;
    std::call_once(registered, [] {
        WNDCLASSW type{}; type.lpfnWndProc = DefDlgProcW; type.cbWndExtra = DLGWINDOWEXTRA;
        type.hInstance = GetModuleHandleW(nullptr); type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1); type.lpszClassName = signinClass;
        if (!RegisterClassW(&type)) throw NativeFailure("Cannot register native local-profile dialog");
    });
    const auto description = signinTemplate();
    const auto choice = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<const DLGTEMPLATE*>(description.data()),
        session.owner, signinProcedure, reinterpret_cast<LPARAM>(&session));
    if (session.failure) std::rethrow_exception(session.failure);
    const uint32_t status = choice == -1 ? (GetLastError() ? GetLastError() : ERROR_FUNCTION_FAILED) : uint32_t(choice);
    printf("[native-profile] Actual local-profile chooser closed; status=%u selected=%u\n", status, nativeSelectedLocalProfile(0) != 0);
    fflush(stdout);
    return status;
}
bool useSelectedAccountForSplash(const PPCContext& ctx) {
    // Shipped splash Lua requests ShowSignInUI(1). GUI821A3278 supplies
    // DisallowGuest and its SDK tail call preserves the exact LR821A32AC.
    if (ctx.lr != 0x821a32ac || ctx.r3.u64 != 1 || ctx.r4.u64 != 0x01000000) return false;
    std::lock_guard lock(profile.mutex);
    if (!profile.active || !profile.selected || !profile.id || profile.signinPending || profile.signinWindow) return false;
    // Startup precedes the title's listener creation. Refresh the actual
    // selected account after this real request so original event0A handling
    // 823A9894 -> 823A79D0 re-queries SDK state1 and refreshes its settings.
    // No identity transition or UI lifecycle occurs on this path.
    publishLocked(signinChanged, 1);
    return true;
}
void signinDiagnostic(const PPCContext& ctx, uint32_t user, uint32_t flags, uint32_t status, uint32_t state) {
    static const bool enabled = [] { char value[2]{}; return GetEnvironmentVariableA("DORITOS_USER_DIAGNOSTICS", value, 2) == 1 && value[0] == '1'; }();
    static std::atomic<uint32_t> count{0};
    if (!enabled || count.fetch_add(1, std::memory_order_relaxed) >= 48) return;
    printf("[native-profile-query] user=%u flags=%08X status=%08X state=%u lr=%08X selected=%u\n",
        user, flags, status, state, uint32_t(ctx.lr), profile.selected && user == 0 && profile.active);
    fflush(stdout);
}
std::array<bool, 4> inputSnapshot() {
    std::array<bool, 4> result{};
    for (DWORD index = 0; index < 4; ++index) {
        XINPUT_STATE state{};
        result[index] = nativeInputState(index, &state) == ERROR_SUCCESS;
    }
    return result;
}
void monitorInput(std::stop_token stop) {
    std::unique_lock lock(profile.mutex);
    while (!stop.stop_requested()) {
        profile.wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return stop.stop_requested(); });
        if (stop.stop_requested()) return;
        const auto now = inputSnapshot();
        if (now != profile.connected) {
            profile.connected = now;
            // This system event tells the original title to query input again.
            // sub_82251160 does not consume its parameter.
            publishLocked(inputChanged, 0);
        }
    }
}
std::string accountKey() {
    NativeHandle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
        throw NativeFailure("Cannot open Windows account identity");
    DWORD bytes = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
    if (!bytes) throw NativeFailure("Cannot query Windows account identity size");
    std::vector<uint8_t> data(bytes);
    if (!GetTokenInformation(token.value, TokenUser, data.data(), bytes, &bytes))
        throw NativeFailure("Cannot read Windows account identity");
    const auto sid = reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
    if (!IsValidSid(sid)) throw NativeFailure("Windows account identity is not a valid SID");
    const auto sidBytes = GetLengthSid(sid);
    std::array<uint8_t, 32> digest{};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw NativeFailure("Cannot initialize native account hash");
    const auto status = BCryptHash(algorithm, nullptr, 0, static_cast<PUCHAR>(sid), sidBytes, digest.data(), DWORD(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw NativeFailure("Cannot hash Windows account identity");
    std::string key;
    constexpr char hex[] = "0123456789abcdef";
    for (auto value : digest) { key += hex[value >> 4]; key += hex[value & 15]; }
    return key;
}
uint64_t loadLocalId(const std::filesystem::path& path) {
    NativeHandle file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) throw NativeFailure("Cannot open persisted native local profile");
    // A sharing violation prevents two processes from racing the first ID write.
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size)) throw NativeFailure("Cannot read native local profile size");
    std::array<uint8_t, 16> record{};
    DWORD transferred = 0;
    if (size.QuadPart == 16) {
        if (!ReadFile(file.value, record.data(), DWORD(record.size()), &transferred, nullptr) || transferred != record.size() ||
            memcmp(record.data(), "DLOCAL01", 8) != 0)
            throw NativeFailure("Persisted native local profile is invalid");
    } else if (size.QuadPart == 0) {
        memcpy(record.data(), "DLOCAL01", 8);
        if (BCryptGenRandom(nullptr, record.data() + 8, 8, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw NativeFailure("Cannot create native local profile identity");
        record[8] = 0xe0; // Offline local ID namespace, never an Xbox Live identity.
        if (!WriteFile(file.value, record.data(), DWORD(record.size()), &transferred, nullptr) || transferred != record.size() ||
            !FlushFileBuffers(file.value)) throw NativeFailure("Cannot persist native local profile identity");
    } else throw NativeFailure("Persisted native local profile has an invalid length");
    uint64_t result = 0;
    for (unsigned i = 8; i < 16; ++i) result = (result << 8) | record[i];
    if ((result >> 56) != 0xe0) throw NativeFailure("Persisted identity is not a native local profile");
    return result;
}
uint32_t createListener(uint64_t mask, uint32_t maxVersion) {
    auto state = std::make_shared<NativeNotificationState>();
    state->mask = mask; state->maxVersion = maxVersion;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return 0;
    const uint32_t handle = registerObject(event, ObjectKind::Notification);
    if (!handle) { CloseHandle(event); return 0; }
    auto object = lookupObject(handle);
    object->manualReset = true; object->notificationState = std::move(state);
    std::lock_guard lock(profile.mutex);
    profile.listeners.emplace_back(object);
    // Listener creation itself produces no sign-in, UI, or network event.
    return handle;
}
bool getNotification(uint32_t handle, uint32_t match, uint32_t idOut, uint32_t parameterOut) {
    requireSpan(idOut, 4); if (parameterOut) requireSpan(parameterOut, 4);
    memory->write32(idOut, 0); if (parameterOut) memory->write32(parameterOut, 0);
    auto object = lookupObject(handle);
    if (!object || object->kind != ObjectKind::Notification || !object->notificationState) return false;
    auto state = object->notificationState;
    std::lock_guard lock(state->mutex);
    auto entry = std::find_if(state->queue.begin(), state->queue.end(), [&](const auto& value) { return !match || value.first == match; });
    if (entry == state->queue.end()) return false;
    memory->write32(idOut, entry->first); if (parameterOut) memory->write32(parameterOut, entry->second);
    state->queue.erase(entry);
    if (state->queue.empty() && !ResetEvent(object->handle)) throw NativeFailure("Cannot reset native platform listener");
    return true;
}
uint32_t readProfile(PPCContext& ctx) {
    const uint32_t title = ctx.r3.u32, user = ctx.r4.u32, xuidCount = ctx.r5.u32, xuids = ctx.r6.u32;
    const uint32_t count = ctx.r7.u32, ids = ctx.r8.u32, sizePointer = ctx.r9.u32, output = ctx.r10.u32;
    const uint64_t overlapSlot = uint64_t(ctx.r1.u32) + 84;
    if (overlapSlot + 4 > 0x100000000ull) return ERROR_INVALID_PARAMETER;
    requireSpan(uint32_t(overlapSlot), 4);
    const uint32_t overlap = memory->read32(uint32_t(overlapSlot));
    if (xuidCount || xuids) throw NativeFailure("XUID-selected profile settings require an original callsite contract");
    if (!count || count > 32 || !sizePointer || !ids || (title && title != 0xfffe07d1)) return ERROR_INVALID_PARAMETER;
    requireSpan(sizePointer, 4); requireSpan(ids, count * 4);
    std::vector<uint32_t> settingIds;
    uint32_t required = 8 + count * 40;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = memory->read32(ids + i * 4), type = id >> 28, capacity = (id >> 16) & 0xfff;
        if (!titleSpecific(id) && !systemPreference(id)) throw NativeFailure("Unimplemented native system profile preference");
        const uint32_t scalar = scalarSize(uint8_t(type));
        if ((scalar && scalar != capacity) || (!scalar && type != 4 && type != 6)) return ERROR_INVALID_PARAMETER;
        settingIds.push_back(id);
        if (type == 4 || type == 6) required += capacity;
    }
    const uint32_t available = memory->read32(sizePointer);
    memory->write32(sizePointer, required);
    if (!output || available < required) return ERROR_INSUFFICIENT_BUFFER;
    requireSpan(output, required);
    Completion completion(overlap);
    if (user >= 4) return completion.finish(ERROR_NO_SUCH_USER);
    std::lock_guard lock(profile.mutex);
    if (user || !profile.active) return completion.finish(ERROR_NO_SUCH_USER);
    memset(memory->base() + output, 0, required);
    memory->write32(output, count); memory->write32(output + 4, output + 8);
    uint32_t trailing = output + 8 + 40 * count;
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t id = settingIds[index], record = output + 8 + index * 40;
        const uint32_t type = id >> 28, capacity = (id >> 16) & 0xfff;
        const auto& settings = titleSpecific(id) ? profile.titleSettings : profile.systemSettings;
        const auto value = settings.find(id);
        memory->write32(record + 8, user); memory->write32(record + 16, id);
        if (value == settings.end()) {
            // A new title profile has no stored bytes. The original title can
            // detect source NOT_SET and apply its own defaults.
            memory->base()[record + 24] = 0xff;
        } else {
            memory->write32(record, titleSpecific(id) ? 2 : 1);
            memory->base()[record + 24] = value->second.type;
            if (type == 4 || type == 6) {
                memory->write32(record + 32, uint32_t(value->second.bytes.size()));
                memory->write32(record + 36, trailing);
                if (!value->second.bytes.empty()) memcpy(memory->base() + trailing, value->second.bytes.data(), value->second.bytes.size());
            } else memcpy(memory->base() + record + 32, value->second.bytes.data(), value->second.bytes.size());
        }
        if (type == 4 || type == 6) trailing += capacity;
    }
    return completion.finish(ERROR_SUCCESS);
}
uint32_t writeProfile(PPCContext& ctx) {
    const uint32_t title = ctx.r3.u32, user = ctx.r4.u32, count = ctx.r5.u32, records = ctx.r6.u32, overlap = ctx.r7.u32;
    if (!count || count > 32 || !records || (title && title != 0xfffe07d1)) return ERROR_INVALID_PARAMETER;
    requireSpan(records, count * 40);
    Completion completion(overlap);
    if (user >= 4) return completion.finish(ERROR_INVALID_PARAMETER);
    std::vector<std::pair<uint32_t, StoredSetting>> changes;
    bool titleRecords = false, systemRecords = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t record = records + i * 40, id = memory->read32(record + 16);
        const uint8_t type = memory->base()[record + 24];
        if (!titleSpecific(id) && !systemPreference(id)) throw NativeFailure("Unimplemented native system profile preference write");
        titleRecords |= titleSpecific(id); systemRecords |= !titleSpecific(id);
        StoredSetting value{type, {}};
        if (type != 0xff) {
            const uint32_t scalar = scalarSize(type);
            if (scalar) value.bytes.assign(memory->base() + record + 32, memory->base() + record + 32 + scalar);
            else if (type == 4 || type == 6) {
                const uint32_t bytes = memory->read32(record + 32), pointer = memory->read32(record + 36);
                if (bytes > ((id >> 16) & 0xfff) || (bytes && !pointer)) return completion.finish(ERROR_INVALID_PARAMETER);
                if (bytes) { requireSpan(pointer, bytes); value.bytes.assign(memory->base() + pointer, memory->base() + pointer + bytes); }
            } else return completion.finish(ERROR_INVALID_PARAMETER);
            if (!validSetting(id, value)) return completion.finish(ERROR_INVALID_PARAMETER);
        }
        changes.emplace_back(id, std::move(value));
    }
    // The actual title writes one namespace per call. Multi-file atomic commit
    // is not silently approximated; reject that unproven mixed operation.
    if (titleRecords && systemRecords) throw NativeFailure("Mixed system/title profile transaction is not implemented");
    std::lock_guard lock(profile.mutex);
    if (user || !profile.active) return completion.finish(ERROR_NO_SUCH_USER);
    auto& existing = titleRecords ? profile.titleSettings : profile.systemSettings;
    auto updated = existing;
    for (auto& [id, value] : changes) {
        if (value.type == 0xff) updated.erase(id);
        else updated[id] = std::move(value);
    }
    const uint32_t status = persistSettings(profile.directory / (titleRecords ? "title-settings.bin" : "preferences.bin"), updated);
    if (!status) {
        const bool changed = updated != existing;
        existing = std::move(updated);
        if (changed) publishLocked(0x0e, user);
    }
    return completion.finish(status);
}
}
void initializeUserServices(const std::filesystem::path& saveRoot, bool selectAccount) {
    std::lock_guard lock(profile.mutex);
    if (profile.active || profile.monitor.joinable() || profile.signinPending) throw NativeFailure("Native local profile was already activated");
    std::array<char, 32768> username{};
    DWORD length = DWORD(username.size());
    if (!GetUserNameA(username.data(), &length) || !username[0]) throw NativeFailure("Cannot read Windows account name");
    profile.name = username.data();
    profile.directory = std::filesystem::absolute(saveRoot) / "profiles" / accountKey();
    std::filesystem::create_directories(profile.directory);
    profile.id = loadLocalId(profile.directory / "local-id.bin");
    initializeSettings();
    profile.connected = inputSnapshot();
    profile.selected = selectAccount;
    profile.active = true;
    publishLocked(signinChanged, 1);
    profile.monitor = std::jthread(monitorInput);
    printf("[native-profile] Windows account local profile activated; offline services; selected=%u\n", unsigned(selectAccount));
}
void shutdownUserServices() {
    // No guest memory is accessed by this monitor; join before registry teardown.
    if (profile.monitor.joinable()) {
        profile.monitor.request_stop(); profile.wake.notify_all(); profile.monitor.join();
    }
    std::lock_guard lock(profile.mutex);
    if (profile.active) { profile.active = false; profile.selected = false; publishLocked(signinChanged, 1); }
    if (profile.signinWindow) PostMessageW(profile.signinWindow, WM_CLOSE, 0, 0);
}
uint64_t nativeSelectedLocalProfile(uint32_t user) {
    std::lock_guard lock(profile.mutex);
    return user == 0 && profile.active && profile.selected ? profile.id : 0;
}
bool userServicesImport(PPCContext& ctx, uint8_t*, const char* name) {
    const uint32_t user = ctx.r3.u32;
    if (!strcmp(name, "XMsgStartIORequest")) {
        // Original synchronous XUser setters send input-only XGI requests.
        // Their exact negative-return branches return 1627 before checking
        // last error or consuming an overlap. Positive Win32 errors would
        // enter the accepted-request branch, so unavailable is an HRESULT.
        const bool context = ctx.lr == 0x823E0CFC && ctx.r4.u64 == 0x000B0006 && ctx.r7.u64 == 24;
        const bool property = ctx.lr == 0x823E0DC0 && ctx.r4.u64 == 0x000B0007 && ctx.r7.u64 == 32;
        if ((!context && !property) || ctx.r3.u64 != 0xFB || ctx.r5.u64 ||
            ctx.r1.u64 > uint64_t(UINT32_MAX) - 80 || ctx.r6.u64 != ctx.r1.u64 + 80) return false;
        const uint32_t request = ctx.r6.u32;
        requireSpan(request, context ? 24 : 32);
        if (memory->read32(request) || memory->read64(request + 8)) return false;
        // Padding +4 (and property +28) is unwritten by the original wrapper.
        // No Xbox context/property provider accepts a native job here. Leave
        // the full payload and pointed input bytes, error state, profile and
        // any event untouched; no registration or completion is claimed.
        ctx.r3.u64 = uint32_t(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        return true;
    }
    if (!strcmp(name, "XamWriteGamerTile")) {
        // Original 823E1050 expands a tile ID into the two SDK tile IDs and
        // forwards the original overlap in r8. r9/r10 are not API arguments.
        // The observed title updater treats any immediate result other than
        // IO_PENDING as an error, shows CANT_SAVE_GAMER_PICT and drains its
        // own queue. The already durable profile settings are independent.
        if (ctx.lr != 0x82232750 || user || ctx.r4.u32 || ctx.r7.u32 != 1 || !ctx.r8.u32 ||
            (ctx.r5.u32 & 0xffff0000) != 0x00020000 || (ctx.r6.u32 & 0xffff0000) != 0x00010000 ||
            (ctx.r5.u32 & 0xffff) != (ctx.r6.u32 & 0xffff)) return false;
        requireSpan(ctx.r8.u32, 28);
        // No Xbox dashboard tile provider accepts a native upload here. Reject
        // before accepting any asynchronous job, signaling an event, writing
        // an overlapped result or manufacturing a saved/uploaded gamer tile.
        static std::once_flag diagnostic;
        std::call_once(diagnostic, [] {
            printf("[native-profile] Xbox gamer-tile write unavailable; rejecting before asynchronous submission\n");
            fflush(stdout);
        });
        ctx.r3.u64 = ERROR_NOT_SUPPORTED;
        return true;
    }
    if (!strcmp(name, "XMsgCancelIORequest")) {
        if (ctx.r4.u32 != 1) return false;
        if (ctx.lr == 0x82889828) {
            // Original voice pre-initialization reset82889808 uses its real
            // 480-byte owner, overlap+160 and CPU-state pointer+84. The zeroed
            // initial state has no pending profile read or submitted overlap.
            // Keep active worker/pending states at their explicit boundary;
            // never clear the original CAS/drain state or invent completion.
            const uint32_t owner = ctx.r31.u32;
            if (uint64_t(owner) + 160 != ctx.r3.u32 || uint64_t(owner) + 84 != ctx.r30.u32) return false;
            requireSpan(owner, 480);
            if (memory->read32(owner + 84) || memory->read32(owner + 200) ||
                !std::all_of(memory->base() + owner + 160, memory->base() + owner + 188,
                    [](uint8_t byte) { return byte == 0; })) return false;
        } else if (ctx.lr != 0x823E13C4) return false;
        // Original 823E13B0 error-queue cleanup discards this result. It also
        // visits entries never submitted. The initial voice reset likewise
        // discards it before running its own genuine CPU-state drain loop.
        // No Xbox app-IO provider or StartIORequest accepted a native job;
        // preserve the entire overlap and its event, without claiming cancel
        // success, waiting for a nonexistent job or completing an upload.
        requireSpan(ctx.r3.u32, 28);
        ctx.r3.u64 = ERROR_NOT_SUPPORTED;
        return true;
    }
    if (!strcmp(name, "XMsgInProcessCall")) {
        if (user == 0xfa && ctx.r4.u32 == 0x0007001b) {
            // The native title owns its own music playback; no dashboard
            // music controller can acquire or lock these Windows voices.
            // Original 824CF700/824CF768 pass this exact stack packet and
            // two DWORD outputs. {controller=0, locked=0} lets the original
            // wrapper report title ownership. Returning unsupported instead
            // leaves BOOL=0, which 8257A700 inverts into FMOD's music mute.
            if (ctx.lr != 0x824CF730 || ctx.r3.u64 != 0xfa || ctx.r4.u64 != 0x0007001b || ctx.r6.u64 ||
                ctx.r1.u64 > uint64_t(UINT32_MAX) - 200 || (ctx.r1.u32 & 15) ||
                ctx.r5.u64 != ctx.r1.u64 + 80) return false;
            const uint32_t packet = ctx.r5.u32, controller = ctx.r1.u32 + 192, locked = ctx.r1.u32 + 196;
            requireSpan(packet, 12);
            if (memory->read32(packet) != 2 || memory->read32(packet + 4) != controller ||
                memory->read32(packet + 8) != locked) return false;
            const auto writable = [&](uint32_t address) {
                MEMORY_BASIC_INFORMATION info{};
                if (!memory->valid(address, 4) || !VirtualQuery(memory->base() + address, &info, sizeof(info)) ||
                    info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
                const DWORD protection = info.Protect & 255;
                return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                    protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
            };
            if (!writable(controller) || !writable(locked)) { ctx.r3.u64 = invalidArgument; return true; }
            memory->write32(controller, 0); memory->write32(locked, 0);
            ctx.r3.u64 = 0;
            return true;
        }
        // Original 823E0A88 sends XAM app FE/message 21012 with a pointer
        // to one input DWORD and arg2 zero. It consumes only the HRESULT
        // sign, mapping failure to ERROR_FUNCTION_FAILED; its title callers
        // then continue without consuming or reading an output buffer.
        // This Windows port has no matching Xbox dashboard message service.
        // Keep unknown messages/layouts at the explicit import boundary.
        if (user != 0xfe || ctx.r4.u32 != 0x00021012 || ctx.r6.u32 != 0) return false;
        requireSpan(ctx.r5.u32, 4);
        ctx.r3.u64 = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return true;
    }
    if (!strcmp(name, "XamAvatarInitialize")) {
        // Original sub_825EEE50 supplies six arguments: kind, render mode,
        // processor, callbacks, work buffer, work-buffer bytes. Its negative
        // return branch calls sub_825EEDD0 to clean up and propagates failure.
        // sub_82395970 then invokes its authored manager failure handler and
        // continues. Windows has no Xbox OS avatar asset service: no callback,
        // work-buffer, metadata or geometry output may be invented here.
        ctx.r3.u64 = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return true;
    }
    if (!strcmp(name, "XamAvatarShutdown")) {
        // Initialization above never opens a service or acquires avatar data.
        // The original failure cleanup may safely shut down this inactive port.
        return true;
    }
    if (!strcmp(name, "XamShowSigninUI")) {
        if (ctx.r4.u32 == 0x01000002) {
            // Original8222AA70 requests one non-guest online account at
            // LR8222AAA4. This port has no Xbox Live account/chooser service.
            // Return the API's unavailable result, preserving actual local
            // selection and all UI/notification state. Do not turn an online
            // request into successful offline sign-in or a fatal import miss.
            ctx.r3.u64 = (!user || user > 4 || user == 3) ? ERROR_INVALID_PARAMETER : ERROR_NOT_SUPPORTED;
            return true;
        }
        // Other requests retain the real owned Windows profile chooser.
        if (ctx.r4.u32 != 0x01000000) return false;
        if (useSelectedAccountForSplash(ctx)) {
            static std::once_flag diagnostic;
            std::call_once(diagnostic, [] {
                printf("[native-profile] Splash request uses actual selected offline account; current-profile refresh queued.\n");
                fflush(stdout);
            });
            ctx.r3.u64 = ERROR_SUCCESS;
            return true;
        }
        ctx.r3.u64 = showSignin(user);
        return true;
    }
    if (!strcmp(name, "XamContentGetLicenseMask")) {
        // Ordinal 614 has two arguments: DWORD* mask, optional overlapped.
        // Original 82327CD8 initializes its own mask to zero, then a nonzero
        // query result at 82327D24 returns the previous licensed byte without
        // consuming that mask or setting either cached license/query flag.
        // Only the proven synchronous caller is supported; no completion or
        // entitlement provider exists for the native local-profile service.
        if (ctx.lr != 0x82327D24 || ctx.r4.u32) return false;
        if (!ctx.r3.u32) { ctx.r3.u64 = ERROR_INVALID_PARAMETER; return true; }
        requireSpan(ctx.r3.u32, 4);
        static std::once_flag diagnostic;
        std::call_once(diagnostic, [] {
            printf("[native-profile] Xbox content-license query unavailable; returning unsupported without license output\n");
            fflush(stdout);
        });
        ctx.r3.u64 = ERROR_NOT_SUPPORTED;
        return true;
    }
    if (!strcmp(name, "XamUserCreateAchievementEnumerator")) {
        // SDK ABI: title, user, uint64 XUID, flags, offset, count, sizeOut,
        // handleOut in r3..r10. Original 82255B90 requests this exact local
        // shape; its nonzero branch at 82255C8C returns before consuming
        // either output, allocating storage or beginning enumeration.
        const uint32_t userIndex = ctx.r4.u32, count = ctx.r8.u32;
        if (userIndex >= 4 || !count || !ctx.r9.u32 || !ctx.r10.u32) {
            ctx.r3.u64 = ERROR_INVALID_PARAMETER; return true;
        }
        if (ctx.r3.u32 != 0 || userIndex != 0 || ctx.r5.u64 != 0 ||
            ctx.r6.u32 != 0xffffffff || ctx.r7.u32 != 0 || count != 12) return false;
        requireSpan(ctx.r9.u32, 4); requireSpan(ctx.r10.u32, 4);
        // No Xbox achievement provider exists for this Windows local account.
        // A real unavailable error is not an empty or completed enumerator.
        // Never manufacture metadata, unlocked records, handles or callbacks.
        static std::once_flag diagnostic;
        std::call_once(diagnostic, [] {
            printf("[native-profile] Xbox achievement enumeration unavailable; local profile preserved\n");
            fflush(stdout);
        });
        ctx.r3.u64 = ERROR_NOT_SUPPORTED;
        return true;
    }
    if (!strcmp(name, "XamNotifyCreateListener")) { ctx.r3.u64 = createListener(ctx.r3.u64, ctx.r4.u32); return true; }
    if (!strcmp(name, "XNotifyGetNext")) {
        ctx.r3.u64 = getNotification(user, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); return true;
    }
    if (!strcmp(name, "XamUserReadProfileSettings")) { ctx.r3.u64 = readProfile(ctx); return true; }
    if (!strcmp(name, "XamUserWriteProfileSettings")) { ctx.r3.u64 = writeProfile(ctx); return true; }
    if (!strcmp(name, "XamUserGetSigninState")) {
        std::lock_guard lock(profile.mutex); ctx.r3.u64 = user == 0 && profile.active ? 1 : 0; return true;
    }
    if (!strcmp(name, "XamUserGetDeviceContext")) {
        const uint32_t category = ctx.r4.u32, output = ctx.r5.u32;
        if (category != 1 && category != 4) return false;
        if (!output || user >= 4) { ctx.r3.u64 = invalidArgument; return true; }
        requireSpan(output, 4);
        std::lock_guard lock(profile.mutex);
        if (user || !profile.active) ctx.r3.u64 = noSuchUser;
        else {
            // Original voice helper 82889130 (category 1) handles a negative
            // HRESULT without consuming this output. Headset helper82644920
            // (category 4) maps it to DEVICE_NOT_CONNECTED before any read.
            // PCM playback does not provide an Xbox voice-capture context;
            // no encoded headset/device token exists in this native port.
            ctx.r3.u64 = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        return true;
    }
    if (!strcmp(name, "XamUserGetXUID")) {
        const uint32_t mask = ctx.r4.u32, output = ctx.r5.u32;
        if (!output) { ctx.r3.u64 = invalidArgument; return true; }
        requireSpan(output, 8); memory->write64(output, 0);
        std::lock_guard lock(profile.mutex);
        if (user >= 4 || !(mask & 7) || (mask & ~7)) ctx.r3.u64 = invalidArgument;
        else if (user || !profile.active || !(mask & 1)) ctx.r3.u64 = noSuchUser;
        else { memory->write64(output, profile.id); ctx.r3.u64 = 0; }
        return true;
    }
    if (!strcmp(name, "XamUserGetName")) {
        const uint32_t output = ctx.r4.u32, length = std::min(ctx.r5.u32, uint32_t(16));
        if (!output || !length || user >= 4) { ctx.r3.u64 = invalidArgument; return true; }
        requireSpan(output, length); memset(memory->base() + output, 0, length);
        std::lock_guard lock(profile.mutex);
        if (user || !profile.active) ctx.r3.u64 = noSuchUser;
        else { memcpy(memory->base() + output, profile.name.data(), std::min<size_t>(profile.name.size(), length - 1)); ctx.r3.u64 = 0; }
        return true;
    }
    if (!strcmp(name, "XamUserGetSigninInfo")) {
        const uint32_t flags = ctx.r4.u32, output = ctx.r5.u32;
        if (!output || user >= 4) { ctx.r3.u64 = invalidArgument; return true; }
        // Original 82255B90 queries flags 2 before distinguishing local state 1
        // from Live state 2. It uses the returned guest bit to decide whether
        // to load this user's local title profile. Flags 0 is the default SDK
        // and GUI query; both can report this actual persisted local account.
        if (flags != 0 && flags != 2) throw NativeFailure("XamUserGetSigninInfo flags require an original callsite contract");
        requireSpan(output, 40); memset(memory->base() + output, 0, 40);
        std::lock_guard lock(profile.mutex);
        if (user || !profile.active) ctx.r3.u64 = noSuchUser;
        else {
            memory->write64(output, profile.id); memory->write32(output + 12, 1);
            memcpy(memory->base() + output + 24, profile.name.data(), std::min<size_t>(profile.name.size(), 15));
            ctx.r3.u64 = 0;
        }
        signinDiagnostic(ctx, user, flags, ctx.r3.u32, !user && profile.active ? 1 : 0);
        return true;
    }
    if (!strcmp(name, "XamUserCheckPrivilege")) {
        const uint32_t output = ctx.r5.u32;
        if (!output || (user >= 4 && user != 0xff)) { ctx.r3.u64 = ERROR_INVALID_PARAMETER; return true; }
        requireSpan(output, 4); memory->write32(output, 0);
        std::lock_guard lock(profile.mutex);
        // Original sub_823E0E28 returns NOT_LOGGED_ON for every privilege when
        // the sign-in state is local (one). There is no native Live session.
        ctx.r3.u64 = user && user != 0xff ? ERROR_NO_SUCH_USER : ERROR_NOT_LOGGED_ON;
        return true;
    }
    return false;
}
}
