#include "environment.h"
#include "runtime.h"
#include <bcrypt.h>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Doritos::Native {
namespace {
std::vector<XexImport> exports;
std::map<std::string, uint32_t> modules;
std::mutex environmentMutex;
std::string text(uint32_t address) {
    std::string value;
    for (uint32_t i = 0; i < 65536; ++i) {
        if (uint64_t(address) + i > UINT32_MAX || !memory->valid(address + i, 1))
            fail("Inaccessible environment string");
        char c = char(memory->base()[address + i]);
        if (!c) return value;
        value += c;
    }
    fail("Unterminated environment string");
}
uint32_t headerField(uint32_t key) {
    constexpr uint32_t header = 0x81000000;
    uint32_t count = memory->read32(header + 20);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t record = header + 24 + 8 * i;
        if (memory->read32(record) == key)
            return (key & 255) <= 1 ? record + 4 : header + memory->read32(record + 4);
    }
    return 0;
}
uint16_t read16(uint32_t address) {
    if (!memory->valid(address, 2)) fail("Inaccessible environment input");
    uint16_t value; memcpy(&value, memory->base() + address, 2); return _byteswap_ushort(value);
}
void write16(uint32_t address, uint16_t value) {
    if (!memory->valid(address, 2)) fail("Inaccessible environment output");
    value = _byteswap_ushort(value);
    memcpy(memory->base() + address, &value, 2);
}
}

void initializeEnvironment(const LoadedXex& image) {
    std::lock_guard guard(environmentMutex);
    exports = image.imports;
    modules.clear();
    for (const char* name : {"xam.xex", "xboxkrnl.exe", "default.xex"}) {
        uint32_t module = memory->allocate(256);
        if (std::string(name) == "default.xex") memory->write32(module + 88, 0x81000000);
        modules[name] = module;
    }
}

bool environmentImport(PPCContext& ctx, uint8_t* base, const char* name) {
    const std::string_view function(name);
    auto result = [&](uint32_t value) { ctx.r3.u64 = value; };
    if (function == "XGetLanguage") { result(1); return true; } // English native profile.
    if (function == "XGetGameRegion") { result(0xff); return true; } // North American native profile.
    if (function == "XGetVideoMode" || function == "VdQueryVideoMode") {
        // X_VIDEO_MODE's 48-byte ABI is read by device setup 0x82347820.
        // This is the requested native render mode; it does not certify a frame.
        const uint32_t fields[]{2560, 1440, 0, 1, 1, std::bit_cast<uint32_t>(60.0f), 1, 0, 0, 0, 0, 0};
        if (!memory->valid(ctx.r3.u32, sizeof(fields))) fail("Inaccessible video mode output");
        for (uint32_t i = 0; i < 12; ++i) memory->write32(ctx.r3.u32 + 4*i, fields[i]);
        return true;
    }
    if (function == "XeCryptRandom") {
        if (ctx.r4.u32 && !memory->valid(ctx.r3.u32, ctx.r4.u32)) fail("Inaccessible random output");
        if (ctx.r4.u32 && BCryptGenRandom(nullptr, base + ctx.r3.u32, ctx.r4.u32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            fail("Native cryptographic random generation failed");
        return true;
    }
    if (function == "ExGetXConfigSetting") {
        uint16_t category = ctx.r3.u16, setting = ctx.r4.u16;
        uint32_t value = 0; uint16_t bytes = 4;
        if (category == 3 && setting == 9) value = 1;
        else if (category == 3 && setting == 14) { value = 103; bytes = 1; }
        else if (category == 3 && setting == 10) value = 0x40000; // widescreen desktop profile
        else if (category == 2 && setting == 2) value = 0x1000; // North American AV profile
        else if (category == 3 && setting == 12) {
            DYNAMIC_TIME_ZONE_INFORMATION zone{};
            if (GetDynamicTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID) { result(0xc0000001); return true; }
            value = zone.DynamicDaylightTimeDisabled ? 2 : 0;
        } else if (category == 3 && setting >= 1 && setting <= 7) {
            TIME_ZONE_INFORMATION zone{};
            if (GetTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID) { result(0xc0000001); return true; }
            if (setting == 1) value = uint32_t(zone.Bias);
            else if (setting == 6) value = uint32_t(zone.StandardBias);
            else if (setting == 7) value = uint32_t(zone.DaylightBias);
            else if (setting == 4 || setting == 5) {
                const auto& date = setting == 4 ? zone.StandardDate : zone.DaylightDate;
                value = (uint32_t(date.wMonth)<<24) | (uint32_t(date.wDay)<<16) | (uint32_t(date.wDayOfWeek)<<8) | date.wHour;
            } else {
                const wchar_t* zoneName = setting == 2 ? zone.StandardName : zone.DaylightName;
                unsigned count = 0;
                for (size_t i = 0; zoneName[i] && count < 4; ++i)
                    if ((!i || zoneName[i-1] == L' ') && zoneName[i] < 128) value |= uint32_t(zoneName[i]) << (24 - 8*count++);
            }
        } else { result(category == 2 || category == 3 ? 0xc00000f0 : 0xc00000ef); return true; }
        if (ctx.r7.u32) write16(ctx.r7.u32, bytes);
        if (!ctx.r5.u32) { result(ctx.r6.u16 ? 0xc00000f1 : 0); return true; }
        if (ctx.r6.u16 < bytes) { result(0xc0000023); return true; }
        if (bytes == 4) memory->write32(ctx.r5.u32, value);
        else { if (!memory->valid(ctx.r5.u32, 1)) fail("Inaccessible country setting"); base[ctx.r5.u32] = uint8_t(value); }
        result(0); return true;
    }
    if (function == "XamGetSystemVersion") {
        // Native compatibility profile follows the 2.0.11626.0 imported ABI.
        result((2u << 28) | (11626u << 8)); return true;
    }
    if (function == "XamGetExecutionId") {
        uint32_t address = headerField(0x40006);
        memory->write32(ctx.r3.u32, address);
        result(address ? 0 : 0xc0000225); return true;
    }
    if (function == "XexLoadImage" || function == "XexGetModuleHandle") {
        uint32_t out = function == "XexLoadImage" ? ctx.r6.u32 : ctx.r4.u32;
        std::string requested = ctx.r3.u32 ? text(ctx.r3.u32) : "default.xex";
        std::transform(requested.begin(), requested.end(), requested.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::lock_guard guard(environmentMutex);
        auto found = modules.find(requested);
        memory->write32(out, found == modules.end() ? 0 : found->second);
        fprintf(stderr, "[MODULE] %s status=%s\n", requested.c_str(), found == modules.end() ? "absent" : "native service");
        result(found == modules.end() ? (function == "XexLoadImage" ? 0xc000000f : 126) : 0);
        return true;
    }
    if (function == "XexUnloadImage") {
        std::lock_guard guard(environmentMutex);
        for (const auto& [module, handle] : modules) if (handle == ctx.r3.u32) { result(0); return true; }
        result(0xc0000008); return true;
    }
    if (function == "XexGetProcedureAddress") {
        std::lock_guard guard(environmentMutex);
        std::string library;
        for (const auto& [module, handle] : modules) if (handle == ctx.r3.u32) library = module;
        uint32_t target = 0;
        if (library.empty()) { memory->write32(ctx.r5.u32, 0); result(0xc0000008); return true; }
        std::string requested = ctx.r4.u32 > 0xffff ? text(ctx.r4.u32) : std::string();
        for (const auto& entry : exports) {
            if (entry.kind != 1 || entry.library != library) continue;
            if ((requested.empty() && entry.ordinal == ctx.r4.u32) || (!requested.empty() && requested == entry.name)) {
                target = entry.address; break;
            }
        }
        memory->write32(ctx.r5.u32, target);
        fprintf(stderr, "[EXPORT] %s ordinal/name=0x%08X target=0x%08X\n", library.c_str(), ctx.r4.u32, target);
        result(target ? 0 : 0xc0000263); return true;
    }
    if (function == "ExAllocatePoolWithTag" || function == "ExAllocatePoolTypeWithTag") {
        if (function == "ExAllocatePoolTypeWithTag" && ctx.r5.u32) fail("Unsupported pool type");
        result(ctx.r3.u32 ? memory->allocate(ctx.r3.u32) : 0); return true;
    }
    if (function == "ExFreePool") {
        if (ctx.r3.u32 && !memory->release(ctx.r3.u32)) fail("Pool free has no owned native allocation");
        return true;
    }
    if (function == "XamAlloc") {
        if (ctx.r3.u32 != 0) fail("Unsupported XAM allocation flags");
        memory->write32(ctx.r5.u32, ctx.r4.u32 ? memory->allocate(ctx.r4.u32) : 0);
        result(0); return true;
    }
    if (function == "XamFree") {
        if (ctx.r3.u32 && !memory->release(ctx.r3.u32)) fail("XAM free has no owned native allocation");
        result(0); return true;
    }
    if (function == "RtlCompareMemoryUlong") {
        uint32_t bytes = ctx.r4.u32 & ~3u, matched = 0;
        while (matched < bytes && memory->read32(ctx.r3.u32 + matched) == ctx.r5.u32) matched += 4;
        result(matched); return true;
    }
    if (function == "RtlInitUnicodeString") {
        uint32_t bytes = 0;
        if (ctx.r4.u32) while (bytes < 0xfffe && read16(ctx.r4.u32 + bytes)) bytes += 2;
        if (bytes == 0xfffe) fail("Unterminated Unicode string");
        write16(ctx.r3.u32, uint16_t(bytes)); write16(ctx.r3.u32+2, ctx.r4.u32 ? uint16_t(bytes+2) : 0);
        memory->write32(ctx.r3.u32+4, ctx.r4.u32); return true;
    }
    if (function == "RtlMultiByteToUnicodeN" || function == "RtlUnicodeToMultiByteN") {
        uint32_t out = ctx.r3.u32, capacity = ctx.r4.u32, written = ctx.r5.u32, input = ctx.r6.u32, bytes = ctx.r7.u32;
        if (bytes && !memory->valid(input, bytes)) fail("Inaccessible conversion input");
        if (capacity && !memory->valid(out, capacity)) fail("Inaccessible conversion output");
        uint32_t count = function == "RtlMultiByteToUnicodeN" ? std::min(capacity/2, bytes) : std::min(capacity, bytes/2);
        if (function == "RtlMultiByteToUnicodeN") {
            for (uint32_t i = 0; i < count; ++i) {
                if (base[input+i] >= 128) fail("Non-ASCII ANSI conversion requires a verified title code page");
                write16(out+2*i, base[input+i]);
            }
            if (written) memory->write32(written, count*2);
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                uint16_t c = read16(input+2*i);
                if (c >= 128) fail("Non-ASCII Unicode conversion requires a verified title code page");
                base[out+i] = uint8_t(c);
            }
            if (written) memory->write32(written, count);
        }
        result(0); return true;
    }
    if (function == "RtlUnicodeStringToAnsiString") {
        uint32_t out = ctx.r3.u32, source = ctx.r4.u32;
        uint16_t bytes = read16(source);
        if (bytes & 1) { result(0xc000000d); return true; }
        uint32_t input = memory->read32(source+4), count = bytes/2, target;
        if (ctx.r5.u32) {
            target = memory->allocate(count+1);
            memory->write32(out+4, target); write16(out+2, uint16_t(count+1));
        } else {
            target = memory->read32(out+4);
            if (read16(out+2) <= count) { result(0xc0000023); return true; }
        }
        if (!memory->valid(target, count+1)) fail("Inaccessible ANSI string output");
        for (uint32_t i = 0; i < count; ++i) {
            uint16_t c = read16(input+2*i);
            if (c >= 128) fail("Non-ASCII ANSI string requires a verified title code page");
            base[target+i] = uint8_t(c);
        }
        base[target+count] = 0; write16(out, uint16_t(count)); result(0); return true;
    }
    if (function == "RtlFreeAnsiString") {
        uint32_t buffer = memory->read32(ctx.r3.u32+4);
        if (buffer && !memory->release(buffer)) fail("ANSI string free has no owned native allocation");
        memory->write32(ctx.r3.u32, 0); memory->write32(ctx.r3.u32+4, 0); return true;
    }
    if (function == "RtlTimeFieldsToTime") {
        uint32_t input = ctx.r3.u32;
        if (!memory->valid(input, 16)) fail("Inaccessible time fields");
        uint16_t fields[8];
        for (unsigned i = 0; i < 8; ++i) { memcpy(&fields[i], base + input + 2*i, 2); fields[i] = _byteswap_ushort(fields[i]); }
        SYSTEMTIME time{fields[0],fields[1],fields[7],fields[2],fields[3],fields[4],fields[5],fields[6]};
        FILETIME native{};
        bool valid = SystemTimeToFileTime(&time, &native);
        if (valid) memory->write64(ctx.r4.u32, uint64_t(native.dwLowDateTime) | (uint64_t(native.dwHighDateTime)<<32));
        result(valid); return true;
    }
    if (function == "RtlTimeToTimeFields") {
        uint64_t input = memory->read64(ctx.r3.u32);
        FILETIME native{DWORD(input),DWORD(input>>32)};
        SYSTEMTIME time{};
        if (!FileTimeToSystemTime(&native, &time)) fail("Invalid native time conversion");
        uint16_t fields[]{time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,time.wDayOfWeek};
        for (unsigned i = 0; i < 8; ++i) write16(ctx.r4.u32 + 2*i, fields[i]);
        return true;
    }
    return false;
}
}
