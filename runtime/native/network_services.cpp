#include <winsock2.h>
#include "network_services.h"
#include "runtime.h"
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace Doritos::Native {
namespace {
std::mutex networkMutex;
uint32_t startupReferences = 0;
void write16(uint32_t address, WORD value) {
    value = _byteswap_ushort(value);
    memcpy(memory->base() + address, &value, 2);
}
}
bool networkServicesImport(PPCContext& ctx, uint8_t* base, const char* name) {
    if (!strcmp(name, "NetDll_XNetStartup")) {
        const uint32_t params = ctx.r4.u32;
        if (ctx.r3.u32 != 1) ctx.r3.u64 = WSAEINVAL;
        else if (params && (!memory->valid(params, 13) || base[params] != 13)) ctx.r3.u64 = WSAEINVAL;
        else {
            // The absent Xbox secure-transport service creates no session.
            // The original game handles this genuine unsupported-protocol
            // result, then requests ordinary native Winsock independently.
            fprintf(stderr, "[NETWORK] Xbox secure transport unavailable; returning WSAEOPNOTSUPP\n");
            ctx.r3.u64 = WSAEOPNOTSUPP;
        }
        return true;
    }
    if (!strcmp(name, "NetDll_XNetGetTitleXnAddr")) {
        // Original LOCAL peer transitions pass a constructor-cleared XNADDR
        // at peer+44, then ignore this status before real CPU registration.
        // Other original callers copy stack results and remain unsupported.
        const bool localPeerQuery = ctx.lr == 0x8284671Cull || ctx.lr == 0x82848EE4ull;
        if (!localPeerQuery || ctx.r3.u64 != 1 || ctx.r4.u64 > UINT32_MAX) return false;
        const uint32_t output = ctx.r4.u32;
        if (!memory || base != memory->base() || !output || !memory->valid(output, 36))
            fail("Native offline title-address query requires the original valid 36-byte peer address");
        // XNET_GET_XNADDR_NONE: this port never initialized an Xbox title-
        // address service. Windows Winsock does not acquire it or an Xbox MAC.
        // Preserve all output bytes and LastError; no address, connection,
        // session, online readiness or pending/completed work is invented.
        ctx.r3.u64 = 1;
        return true;
    }
    if (!strcmp(name, "NetDll_XNetCleanup")) {
        // Native offline networking does not open an Xbox secure transport.
        // The original SDK cleanup can release its real CPU pool regardless
        // of this inactive service's failure status.
        ctx.r3.u64 = ctx.r3.u32 == 1 ? WSANOTINITIALISED : WSAEINVAL;
        return true;
    }
    if (!strcmp(name, "NetDll_WSAStartup")) {
        const uint32_t output = ctx.r5.u32;
        if (ctx.r3.u32 != 1) { ctx.r3.u64 = WSAEINVAL; return true; }
        // Original 32-bit WSADATA layout: WORD versions, 257/129-byte strings,
        // two WORD limits and a 32-bit vendor pointer. Never copy the host's
        // different x64 field order or a native pointer into guest storage.
        if (!output || !memory->valid(output, 400)) { ctx.r3.u64 = WSAEFAULT; return true; }
        std::lock_guard lock(networkMutex);
        WSADATA data{};
        const int status = WSAStartup(ctx.r4.u16, &data);
        if (status) { ctx.r3.u64 = uint32_t(status); return true; }
        if (data.lpVendorInfo) {
            WSACleanup();
            fail("Native Winsock vendor-info conversion is not implemented");
        }
        ++startupReferences;
        if (output) {
            memset(base + output, 0, 400);
            write16(output, data.wVersion); write16(output + 2, data.wHighVersion);
            memcpy(base + output + 4, data.szDescription, sizeof(data.szDescription));
            memcpy(base + output + 261, data.szSystemStatus, sizeof(data.szSystemStatus));
            write16(output + 390, data.iMaxSockets); write16(output + 392, data.iMaxUdpDg);
            memory->write32(output + 396, 0);
        }
        fprintf(stderr, "[NETWORK] native Winsock initialized; negotiated version=%u.%u\n",
            LOBYTE(data.wVersion), HIBYTE(data.wVersion));
        ctx.r3.u64 = 0;
        return true;
    }
    if (!strcmp(name, "NetDll_WSACleanup")) {
        std::lock_guard lock(networkMutex);
        if (ctx.r3.u32 != 1 || !startupReferences) { WSASetLastError(WSANOTINITIALISED); ctx.r3.s64 = SOCKET_ERROR; return true; }
        const int status = WSACleanup();
        if (!status) --startupReferences;
        ctx.r3.s64 = status;
        return true;
    }
    if (!strcmp(name, "NetDll_WSAGetLastError")) { ctx.r3.u64 = uint32_t(WSAGetLastError()); return true; }
    if (!strcmp(name, "NetDll_WSASetLastError")) { WSASetLastError(ctx.r4.s32); return true; }
    if (!strcmp(name, "NetDll_XNetRandom")) {
        const uint32_t output = ctx.r4.u32, bytes = ctx.r5.u32;
        if (ctx.r3.u32 != 1 || bytes > 16*1024*1024) ctx.r3.u64 = WSAEINVAL;
        else if (bytes && !memory->valid(output, bytes)) ctx.r3.u64 = WSAEFAULT;
        else ctx.r3.u64 = bytes && BCryptGenRandom(nullptr, base + output, bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ? WSASYSCALLFAILURE : 0;
        return true;
    }
    return false;
}
void shutdownNetworkServices() {
    std::lock_guard lock(networkMutex);
    while (startupReferences) { WSACleanup(); --startupReferences; }
}
}
