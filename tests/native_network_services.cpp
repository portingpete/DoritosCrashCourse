#include <winsock2.h>
#include "runtime/native/runtime.h"
#include "runtime/native/network_services.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
using namespace Doritos::Native;
namespace {
unsigned checks = 0;
void require(bool value, const char* detail) {
    ++checks;
    if (!value) throw std::runtime_error(detail);
}
struct StopNetwork { ~StopNetwork() { shutdownNetworkServices(); } };
bool unchanged(const uint8_t* begin, size_t bytes) {
    return std::all_of(begin, begin + bytes, [](uint8_t value) { return value == 0xa5; });
}
uint16_t read16(const uint8_t* base, uint32_t address) {
    return uint16_t(uint16_t(base[address]) << 8 | base[address + 1]);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        const auto image = loadOriginalXex(argv[1]);
        Memory guest(image); memory = &guest;
        StopNetwork stop;
        PPCContext ctx{};
        auto call = [&](const char* name) {
            require(networkServicesImport(ctx, guest.base(), name), "recognized native network service");
            return ctx.r3.u32;
        };
        const uint32_t storage = guest.allocate(4096), output = storage + 32;
        auto poison = [&] { memset(guest.base() + storage, 0xa5, 464); };
        auto startup = [&](uint32_t caller, WORD version, uint32_t destination) {
            ctx.r3.u64 = caller; ctx.r4.u64 = version; ctx.r5.u64 = destination;
            return call("NetDll_WSAStartup");
        };
        auto cleanup = [&](uint32_t caller = 1) {
            ctx.r3.u64 = caller;
            return call("NetDll_WSACleanup");
        };
        auto lastError = [&] {
            ctx.r3.u64 = 1;
            return call("NetDll_WSAGetLastError");
        };

        poison();
        require(startup(0, MAKEWORD(2, 2), output) == WSAEINVAL, "incorrect XNet caller fails before native startup");
        require(unchanged(guest.base() + storage, 464), "caller failure preserves every output byte");
        require(startup(1, 0, output) == WSAVERNOTSUPPORTED, "native Winsock rejects unsupported version");
        require(unchanged(guest.base() + storage, 464), "version failure preserves every output byte");
        require(startup(1, MAKEWORD(2, 2), 0) == WSAEFAULT && unchanged(guest.base() + storage, 464),
            "null WSADATA fails without output or native startup reference");
        const uint32_t tail = storage + guest.allocationSize(storage) - 399;
        memset(guest.base() + tail, 0xa5, 399);
        require(startup(1, MAKEWORD(2, 2), tail) == WSAEFAULT, "400-byte result cannot cross guest allocation boundary");
        require(unchanged(guest.base() + tail, 399), "invalid result range is not partly overwritten");
        require(cleanup() == uint32_t(SOCKET_ERROR) && ctx.r3.s64 == SOCKET_ERROR,
            "failed startup creates no cleanup reference and returns signed SOCKET_ERROR");
        require(lastError() == WSANOTINITIALISED, "uninitialized cleanup exposes real thread-local error");

        // Compare with real host negotiation, rather than hard-coding the OS's
        // description, highest version or legacy socket limits.
        for (const WORD version : {MAKEWORD(2, 2), MAKEWORD(2, 0), MAKEWORD(1, 1)}) {
            WSADATA actual{};
            require(WSAStartup(version, &actual) == 0, "host Winsock version negotiation");
            require(actual.lpVendorInfo == nullptr, "host result has no unconvertible vendor pointer");
            require(WSACleanup() == 0, "host comparison reference is balanced");
            poison();
            require(startup(1, version, output) == 0, "native bridge negotiates real Windows Winsock");
            std::array<uint8_t, 400> expected{};
            auto be16 = [&](size_t offset, WORD value) {
                expected[offset] = uint8_t(value >> 8); expected[offset + 1] = uint8_t(value);
            };
            be16(0, actual.wVersion); be16(2, actual.wHighVersion);
            memcpy(expected.data() + 4, actual.szDescription, sizeof(actual.szDescription));
            memcpy(expected.data() + 261, actual.szSystemStatus, sizeof(actual.szSystemStatus));
            be16(390, actual.iMaxSockets); be16(392, actual.iMaxUdpDg);
            require(memcmp(guest.base() + output, expected.data(), expected.size()) == 0,
                "every byte matches legacy 32-bit WSADATA layout and big-endian WORD encoding");
            require(read16(guest.base(), output) == actual.wVersion && guest.read32(output + 396) == 0,
                "negotiated version is readable and no x64 host pointer enters guest result");
            require(unchanged(guest.base() + storage, 32) && unchanged(guest.base() + output + 400, 32),
                "WSADATA write preserves both sentinel boundaries");
        }
        require(cleanup(0) == uint32_t(SOCKET_ERROR), "incorrect cleanup caller cannot consume native references");
        require(cleanup() == 0 && cleanup() == 0 && cleanup() == 0,
            "three successful startups require three successful cleanups");
        require(cleanup() == uint32_t(SOCKET_ERROR) && lastError() == WSANOTINITIALISED,
            "cleanup after last reference reports genuine not-initialized error");
        require(startup(1, MAKEWORD(2, 2), output) == 0 && startup(1, MAKEWORD(2, 2), output) == 0,
            "native startup can restart after balanced cleanup");
        shutdownNetworkServices();
        shutdownNetworkServices();
        require(cleanup() == uint32_t(SOCKET_ERROR) && lastError() == WSANOTINITIALISED,
            "shutdown drains native startup references and is repeatable");
        require(startup(1, MAKEWORD(2, 2), output) == 0 && cleanup() == 0,
            "shutdown leaves later native service startup usable");

        ctx.r3.u64 = 1; ctx.r4.s64 = WSAECONNREFUSED;
        call("NetDll_WSASetLastError");
        require(WSAGetLastError() == WSAECONNREFUSED, "guest setter writes actual native thread-local Winsock error");
        WSASetLastError(WSAEADDRINUSE);
        require(lastError() == WSAEADDRINUSE, "guest getter reads actual native Winsock error");
        // Win32 synchronization and thread creation may change the native last
        // error themselves. Between setting and reading each error use only
        // atomics, so this verifies Winsock's thread isolation, not preservation
        // across unrelated host APIs that make no such promise.
        std::atomic<bool> workerSet{false}, parentChecked{false};
        bool workerHandled = false; uint32_t workerError = 0;
        std::thread worker([&] {
            PPCContext other{};
            other.r3.u64 = 1; other.r4.s64 = WSAENETUNREACH;
            workerHandled = networkServicesImport(other, guest.base(), "NetDll_WSASetLastError");
            workerSet.store(true, std::memory_order_release);
            while (!parentChecked.load(std::memory_order_acquire)) {}
            other.r3.u64 = 1;
            workerHandled = networkServicesImport(other, guest.base(), "NetDll_WSAGetLastError") && workerHandled;
            workerError = other.r3.u32;
        });
        ctx.r3.u64 = 1; ctx.r4.s64 = WSAEADDRINUSE;
        call("NetDll_WSASetLastError");
        while (!workerSet.load(std::memory_order_acquire)) {}
        const uint32_t parentError = lastError();
        parentChecked.store(true, std::memory_order_release); worker.join();
        require(parentError == WSAEADDRINUSE && workerHandled && workerError == WSAENETUNREACH,
            "simultaneous guest workers preserve independent native thread errors");

        auto random = [&](uint32_t caller, uint32_t destination, uint32_t bytes) {
            ctx.r3.u64 = caller; ctx.r4.u64 = destination; ctx.r5.u64 = bytes;
            return call("NetDll_XNetRandom");
        };
        poison();
        require(random(0, output, 128) == WSAEINVAL && unchanged(guest.base() + storage, 464),
            "incorrect random caller fails without touching output");
        require(random(1, output, 16 * 1024 * 1024 + 1) == WSAEINVAL && unchanged(guest.base() + storage, 464),
            "bounded random request rejects excessive length before mutation");
        require(random(1, tail, 400) == WSAEFAULT && unchanged(guest.base() + tail, 399),
            "random output must fit entirely in committed guest memory");
        require(random(1, 0, 0) == 0, "zero-byte random request needs no output pointer");
        require(random(1, output, 128) == 0, "native BCrypt supplies actual random bytes");
        std::array<uint8_t, 128> first{};
        memcpy(first.data(), guest.base() + output, first.size());
        require(!unchanged(first.data(), first.size()) &&
            std::any_of(first.begin(), first.end(), [](uint8_t value) { return value != 0; }),
            "successful random operation emits bytes rather than fabricated empty completion");
        require(unchanged(guest.base() + storage, 32) && unchanged(guest.base() + output + 128, 304),
            "random generator writes only the requested byte range");
        require(random(1, output, 128) == 0 && memcmp(first.data(), guest.base() + output, first.size()) != 0,
            "independent native random requests produce different 1024-bit outputs");

        poison();
        ctx.r3.u64 = 1; ctx.r4.u64 = 0;
        require(call("NetDll_XNetStartup") == WSAEOPNOTSUPP,
            "absent Xbox secure transport reports unsupported and creates no session");
        guest.base()[output] = 13;
        ctx.r3.u64 = 1; ctx.r4.u64 = output;
        require(call("NetDll_XNetStartup") == WSAEOPNOTSUPP, "valid original XNet parameters still cannot invent secure transport");
        require(guest.base()[output] == 13 && unchanged(guest.base() + output + 1, 399),
            "unsupported secure startup preserves caller parameters");
        guest.base()[output] = 12;
        ctx.r3.u64 = 1; ctx.r4.u64 = output;
        require(call("NetDll_XNetStartup") == WSAEINVAL, "incorrect secure startup parameter size fails");
        ctx.r3.u64 = 0; ctx.r4.u64 = 0;
        require(call("NetDll_XNetStartup") == WSAEINVAL, "incorrect secure startup caller fails");
        ctx.r3.u64 = 1;
        require(call("NetDll_XNetCleanup") == WSANOTINITIALISED && unchanged(guest.base() + output + 1, 399),
            "inactive Xbox transport cleanup reports no acquired service and writes no output");
        ctx.r3.u64 = 0;
        require(call("NetDll_XNetCleanup") == WSAEINVAL, "incorrect secure cleanup caller fails");

        require(guest.read32(0x824D23C0) == 0x7c641b78 &&
            guest.read32(0x824D23C4) == 0x38600001 && guest.read32(0x824D23C8) == 0x48524c14,
            "original title-address wrapper moves pointer to r4, passes caller1 and tail branches");
        require(guest.read32(0x82846718) == 0x4bc8bca9 && guest.read32(0x8284671C) == 0x38a000ff &&
            guest.read32(0x82848EE0) == 0x4bc894e1 && guest.read32(0x82848EE4) == 0x816100bc,
            "both original LOCAL transition calls resume without testing returned address status");
        require(guest.read32(0x8284FE00) == 0x38a00024 && guest.read32(0x8284FE04) == 0x38800000 &&
            guest.read32(0x8284FE0C) == 0x387f002c && guest.read32(0x8284FE14) == 0x4bd85dbd,
            "actual peer constructor memset-clears exactly36 bytes at peer+44");
        // Reserve a fresh final range for the truncated-span fixture, after
        // all other guest allocations. A neighbor could make an old allocation
        // boundary readable, so prove invalidity independently before testing.
        const uint32_t addressRange = guest.allocate(4096);
        const uint32_t addressTail = addressRange + guest.allocationSize(addressRange) - 35;
        memset(guest.base() + addressTail, 0xa5, 35);
        require(guest.valid(addressTail, 35) && !guest.valid(addressTail, 36),
            "fresh terminal fixture owns35 readable bytes and no adjacent byte");
        auto addressQuery = [&](uint64_t lr, uint64_t caller, uint64_t destination) {
            ctx.lr = lr; ctx.r3.u64 = caller; ctx.r4.u64 = destination;
            return networkServicesImport(ctx, guest.base(), "NetDll_XNetGetTitleXnAddr");
        };
        const auto addressReservations = guest.reservationSnapshot();
        for (const uint64_t lr : {0x8284671Cull, 0x82848EE4ull}) {
            poison(); memset(guest.base() + output, 0, 36);
            ctx.r5.u64 = 0xabcdef0123456789ull; ctx.r3.u64 = 1; ctx.r4.u64 = output; ctx.lr = lr;
            const PPCContext expectedQuery = ctx;
            WSASetLastError(WSAENETUNREACH);
            const bool handled = networkServicesImport(ctx, guest.base(), "NetDll_XNetGetTitleXnAddr");
            const int socketError = WSAGetLastError();
            require(handled && ctx.r3.u64 == 1 && !memcmp(&ctx, &expectedQuery, sizeof(ctx)) &&
                socketError == WSAENETUNREACH,
                "proven LOCAL peer receives genuine inactive NONE1 without input-register or socket-error changes");
            require(std::all_of(guest.base() + output, guest.base() + output + 36,
                [](uint8_t value) { return value == 0; }) && unchanged(guest.base() + storage, 32) &&
                unchanged(guest.base() + output + 36, 396),
                "inactive title-address query preserves constructor-cleared address and all neighbor bytes");
            poison();
            require(addressQuery(lr, 1, output) && ctx.r3.u64 == 1 && unchanged(guest.base() + storage, 464),
                "inactive query never fabricates an address or overwrites prior caller storage");
        }
        for (const uint64_t lr : {0ull, 0x82851674ull, 0x82855340ull}) {
            poison();
            require(!addressQuery(lr, 1, output) && ctx.r3.u64 == 1 && ctx.r4.u64 == output &&
                unchanged(guest.base() + storage, 464),
                "unknown and stack-output callers remain explicitly unhandled with untouched bytes");
        }
        for (const uint64_t caller : {0ull, 2ull, 3ull, 4ull, 0x100000001ull, 0xffffffff00000001ull}) {
            poison();
            require(!addressQuery(0x8284671C, caller, output) && ctx.r3.u64 == caller &&
                ctx.r4.u64 == output && unchanged(guest.base() + storage, 464),
                "unknown SDK callers and full-width aliases stay excluded rather than being truncated to title1");
        }
        for (const uint64_t pointer : {uint64_t(output) + 0x100000000ull,
            uint64_t(output) | 0xffffffff00000000ull}) {
            poison();
            require(!addressQuery(0x82848EE4, 1, pointer) && ctx.r3.u64 == 1 && ctx.r4.u64 == pointer &&
                unchanged(guest.base() + storage, 464),
                "out-of-range high pointer bits cannot alias valid CPU output storage");
        }
        auto invalidAddress = [&](uint64_t destination) {
            bool stopped = false;
            try { addressQuery(0x8284671C, 1, destination); }
            catch (const NativeFailure&) { stopped = true; }
            return stopped;
        };
        poison();
        require(invalidAddress(0) && invalidAddress(0xfffffff0ull) && ctx.r3.u64 == 1 &&
            unchanged(guest.base() + storage, 464),
            "null and overflowing36-byte output ranges stop explicitly before mutation");
        require(invalidAddress(addressTail) && ctx.r3.u64 == 1 && unchanged(guest.base() + addressTail, 35),
            "entire36-byte original peer address must remain in a valid owned range");
        require(startup(1, MAKEWORD(2, 2), output) == 0,
            "real Winsock remains independent of Xbox title addressing");
        poison();
        require(addressQuery(0x82848EE4, 1, output) && ctx.r3.u64 == 1 && unchanged(guest.base() + storage, 464),
            "successful native Winsock startup does not invent a title-address service");
        require(cleanup() == 0 && guest.reservationSnapshot() == addressReservations,
            "address query acquires no extra native startup reference, peer, handle or output allocation");
        const auto saved3 = ctx.r3.u64, saved4 = ctx.r4.u64, saved5 = ctx.r5.u64;
        require(!networkServicesImport(ctx, guest.base(), "UnknownNetworkOperation") &&
            ctx.r3.u64 == saved3 && ctx.r4.u64 == saved4 && ctx.r5.u64 == saved5,
            "unimplemented network imports remain unhandled and preserve caller registers");
        printf("Native network contracts passed (%u checks, no outbound connections)\n", checks);
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "Native network contract failed: %s\n", error.what());
        return 1;
    }
}
