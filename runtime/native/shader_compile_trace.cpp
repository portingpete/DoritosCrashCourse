#include "runtime.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_82387140);
extern "C" PPC_FUNC(__imp__sub_82388328);

namespace {
using namespace Doritos::Native;
struct LastErrorRestore { DWORD value = GetLastError(); ~LastErrorRestore() { SetLastError(value); } };
bool selected() noexcept {
    static const bool enabled = [] {
        LastErrorRestore restore;
        wchar_t flag[2]{};
        return GetEnvironmentVariableW(L"DORITOS_AVATAR_TRACE",flag,2) == 1 && flag[0] == L'1';
    }();
    return enabled;
}
uint32_t word(const uint8_t* bytes, unsigned offset) noexcept {
    uint32_t value; std::memcpy(&value,bytes + offset,4); return _byteswap_ulong(value);
}
const uint8_t* readable(uint32_t address, uint32_t bytes) noexcept {
    return memory && memory->valid(address,bytes) ? memory->base() + address : nullptr;
}
struct VariantSnapshot {
    uint32_t address = 0, owner = 0, vertex = 0, pixel = 0, parameters = 0;
    std::array<uint32_t,3> key{};
    unsigned ready = 0, byte5 = 0;
    bool readable = false;
};
VariantSnapshot snapshot(uint32_t address) noexcept {
    VariantSnapshot value; value.address = address;
    // Lookup82388328 allocates2568B and returnsnode+24. The original
    // compile descriptor's owner2536 therefore fits in this exact span.
    if (const auto* bytes = readable(address,2540)) {
        value.readable = true; value.ready = bytes[4]; value.byte5 = bytes[5];
        for (unsigned index = 0; index < 3; ++index) value.key[index] = word(bytes,8 + index * 4);
        value.vertex = word(bytes,36); value.pixel = word(bytes,40);
        value.parameters = word(bytes,52); value.owner = word(bytes,2536);
    }
    return value;
}
struct CompileAttempt {
    uint32_t master = 0, caller = 0, stage = 0;
    std::array<uint32_t,3> key{};
    std::array<char,128> name{};
    bool masterReadable = false, keyReadable = false, nameTerminated = false;
    VariantSnapshot before;
    CompileAttempt* previous = nullptr;
};
thread_local CompileAttempt* activeAttempt = nullptr;
struct CompileSite {
    uint32_t master = 0, stage = 0;
    std::array<uint32_t,3> key{};
    std::array<char,128> name{};
    bool masterReadable = false, keyReadable = false, nameTerminated = false;
    uint64_t calls = 0, nullResults = 0, readyResults = 0, unreadyBefore = 0, unreadableBefore = 0;
    uint32_t firstCaller = 0, lastCaller = 0;
    VariantSnapshot firstBefore, lastBefore, lastAfter;
    uint32_t lastResult = 0;
};
struct CompileSites {
    std::array<CompileSite,256> sites{};
    CompileSite overflow{};
    unsigned used = 0;
    uint64_t attempts = 0, reports = 0;
};
thread_local CompileSites compileSites;
void printAttempt(const CompileAttempt& attempt, uint32_t result, const VariantSnapshot& after, uint64_t number) noexcept {
    const auto& before = attempt.before;
    std::fprintf(stderr,"[ORIGINAL SHADER COMPILE] thread=%lu attempt=%llu caller=%08X master=%08X master_readable=%u "
        "name=\"%s\" name_terminated=%u stage_word120=%u stage=%s key_readable=%u key=[%u,%u,%u] "
        "variant_before=%08X before_readable=%u before_ready4=%u before_byte5=%u before_key=[%u,%u,%u] "
        "result=%08X variant_after=%08X after_readable=%u after_owner2536=%08X after_ready4=%u after_byte5=%u "
        "after_vs36=%08X after_ps40=%08X after_parameters52=%u scope=normal_original_compile_call "
        "cpu_wall_measured=false cache_bypass=false\n",
        GetCurrentThreadId(),number,attempt.caller,attempt.master,unsigned(attempt.masterReadable),attempt.name.data(),
        unsigned(attempt.nameTerminated),attempt.stage,attempt.masterReadable ? (attempt.stage ? "vertex" : "pixel") : "unproven",
        unsigned(attempt.keyReadable),attempt.key[0],attempt.key[1],attempt.key[2],before.address,unsigned(before.readable),
        before.ready,before.byte5,before.key[0],before.key[1],before.key[2],result,after.address,unsigned(after.readable),
        after.owner,after.ready,after.byte5,after.vertex,after.pixel,after.parameters);
}
void record(const CompileAttempt& attempt, uint32_t result, const VariantSnapshot& after) noexcept {
    auto& batch = compileSites;
    CompileSite* site = nullptr;
    for (unsigned index = 0; index < batch.used; ++index) {
        auto& candidate = batch.sites[index];
        if (candidate.master == attempt.master && candidate.stage == attempt.stage && candidate.key == attempt.key &&
            candidate.name == attempt.name && candidate.masterReadable == attempt.masterReadable &&
            candidate.keyReadable == attempt.keyReadable && candidate.nameTerminated == attempt.nameTerminated) {
            site = &candidate; break;
        }
    }
    if (!site && batch.used < batch.sites.size()) {
        site = &batch.sites[batch.used++];
        site->master = attempt.master; site->stage = attempt.stage; site->key = attempt.key; site->name = attempt.name;
        site->masterReadable = attempt.masterReadable; site->keyReadable = attempt.keyReadable;
        site->nameTerminated = attempt.nameTerminated;
    }
    if (!site) site = &batch.overflow;
    if (!site->calls) { site->firstCaller = attempt.caller; site->firstBefore = attempt.before; }
    ++site->calls;
    site->nullResults += result == 0;
    site->readyResults += result != 0 && after.readable && after.ready != 0;
    site->unreadyBefore += attempt.before.readable && !attempt.before.ready;
    site->unreadableBefore += !attempt.before.readable;
    site->lastCaller = attempt.caller; site->lastBefore = attempt.before; site->lastAfter = after; site->lastResult = result;
    const uint64_t number = ++batch.attempts;
    if (number <= 16) { printAttempt(attempt,result,after,number); std::fflush(stderr); }
    if (number % 1024) return;
    std::array<const CompileSite*,256> ranked{};
    for (unsigned index = 0; index < batch.used; ++index) ranked[index] = &batch.sites[index];
    std::sort(ranked.begin(),ranked.begin() + batch.used,[](const auto* left, const auto* right) { return left->calls > right->calls; });
    const DWORD thread = GetCurrentThreadId();
    const auto report = ++batch.reports;
    std::fprintf(stderr,"[ORIGINAL SHADER COMPILE SITES] thread=%lu report=%llu attempts=%llu sites=%u overflow_calls=%llu "
        "top_limit=8 sort=cumulative_calls scope=current_thread_cumulative_original_compile_calls cpu_wall_measured=false cache_bypass=false\n",
        thread,report,number,batch.used,batch.overflow.calls);
    const auto print = [&](const CompileSite& entry, bool overflow) {
        const auto& before = entry.lastBefore; const auto& final = entry.lastAfter;
        std::fprintf(stderr,"[ORIGINAL SHADER COMPILE SITE] thread=%lu report=%llu master=%08X name=\"%s\" name_terminated=%u "
            "stage_word120=%u stage=%s key_readable=%u key=[%u,%u,%u] calls=%llu null_results=%llu ready_results=%llu "
            "unready_before=%llu unreadable_before=%llu first_caller=%08X last_caller=%08X first_variant=%08X last_variant=%08X "
            "last_before_readable=%u last_before_ready4=%u last_before_byte5=%u last_result=%08X last_after_readable=%u "
            "last_after_owner2536=%08X last_after_ready4=%u last_after_byte5=%u last_vs36=%08X last_ps40=%08X "
            "last_parameters52=%u overflow_bucket=%u scope=current_thread_cumulative_original_compile_calls cpu_wall_measured=false cache_bypass=false\n",
            thread,report,overflow ? 0u : entry.master,overflow ? "all_untracked" : entry.name.data(),unsigned(entry.nameTerminated),
            entry.stage,overflow ? "all_untracked" : entry.masterReadable ? (entry.stage ? "vertex" : "pixel") : "unproven",
            unsigned(entry.keyReadable),entry.key[0],entry.key[1],entry.key[2],entry.calls,entry.nullResults,entry.readyResults,
            entry.unreadyBefore,entry.unreadableBefore,entry.firstCaller,entry.lastCaller,entry.firstBefore.address,before.address,
            unsigned(before.readable),before.ready,before.byte5,entry.lastResult,unsigned(final.readable),final.owner,final.ready,
            final.byte5,final.vertex,final.pixel,final.parameters,unsigned(overflow));
    };
    for (unsigned index = 0; index < std::min(batch.used,8u); ++index) print(*ranked[index],false);
    if (batch.overflow.calls) print(batch.overflow,true);
    std::fflush(stderr);
}
void begin(CompileAttempt& attempt, const PPCContext& ctx) noexcept {
    attempt.master = ctx.r3.u32; attempt.caller = uint32_t(ctx.lr);
    if (const auto* master = readable(attempt.master,124)) {
        attempt.masterReadable = true; attempt.stage = word(master,120);
        const uint32_t name = word(master,8);
        for (unsigned index = 0; index + 1 < attempt.name.size(); ++index) {
            if (uint64_t(name) + index > UINT32_MAX) break;
            const auto* bytes = readable(name + index,1);
            if (!bytes) break;
            const uint8_t character = bytes[0];
            if (!character) { attempt.nameTerminated = true; break; }
            attempt.name[index] = character >= 32 && character < 127 ? char(character) : '?';
        }
    }
    if (const auto* key = readable(ctx.r4.u32,12)) {
        attempt.keyReadable = true;
        for (unsigned index = 0; index < 3; ++index) attempt.key[index] = word(key,index * 4);
    }
}
}

PPC_FUNC(sub_82387140) {
    if (!selected()) { __imp__sub_82387140(ctx,base); return; }
    CompileAttempt attempt;
    { LastErrorRestore restore; begin(attempt,ctx); }
    attempt.previous = activeAttempt;
    activeAttempt = &attempt;
    struct RestoreAttempt { CompileAttempt* previous; ~RestoreAttempt() { activeAttempt = previous; } } restoreAttempt{attempt.previous};
    __imp__sub_82387140(ctx,base);
    // Original guest calls can mutate/release memory. Requery the exact
    // invocation's returned or previously observed descriptor; retain no span.
    LastErrorRestore restore;
    const uint32_t result = ctx.r3.u32;
    const auto after = snapshot(result ? result : attempt.before.address);
    record(attempt,result,after);
}
PPC_FUNC(sub_82388328) {
    // Diagnostic context exists only during an opted-in compile call. This
    // exact return is its first lookup, before the compiler writes the variant.
    CompileAttempt* attempt = activeAttempt;
    const bool observe = attempt && ctx.lr == 0x8238715c &&
        uint64_t(attempt->master) + 136 <= UINT32_MAX && ctx.r3.u32 == attempt->master + 136;
    __imp__sub_82388328(ctx,base);
    if (observe) { LastErrorRestore restore; attempt->before = snapshot(ctx.r3.u32); }
}
