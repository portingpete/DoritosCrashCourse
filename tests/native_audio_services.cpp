#include "runtime/native/runtime.h"
#include "runtime/native/audio_services.h"
#include "runtime/native/kernel_objects.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace Doritos::Native {
bool queryAudioPlayback(uint32_t token, uint64_t& played, uint32_t& queued, uint64_t& submitted,
    uint64_t& underruns, uint32_t& engineGlitches);
}
PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
using namespace Doritos::Native;
namespace {
std::array<std::atomic<unsigned>, 2> callbackCount{};
std::atomic<bool> waiting{false};
std::atomic<bool> stallRequested{false}, stallWaiting{false};
std::array<uint32_t, 2> expectedArguments{};
void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
void testCallback(PPCContext& ctx, uint8_t* base) {
    const uint32_t argument = memory->read32(ctx.r3.u32);
    const unsigned client = argument == expectedArguments[0] ? 0 : 1;
    require(argument == expectedArguments[client], "render callback receives its own distinct guest argument cell");
    const auto thread = lookupThreadId(GetCurrentThreadId());
    require(thread && thread->guestAddress == memory->read32(ctx.r13.u32 + 0x100) &&
        memory->read32(thread->guestAddress + 0x14c) == GetCurrentThreadId(), "audio worker owns registered native KTHREAD and PCR");
    ctx.r3.u64 = memory->read32(argument);
    ctx.r4.u64 = memory->read32(argument + 4);
    require(audioServicesImport(ctx, base, "XAudioSubmitRenderDriverFrame") && ctx.r3.u32 == 0,
        "native callback submits real PCM source buffer");
    const unsigned count = callbackCount[client].fetch_add(1) + 1;
    if (client == 0 && count == 28) {
        ctx.r3.u64 = memory->read32(argument + 8);
        ctx.r4.u64 = 3; ctx.r5.u64 = 1; ctx.r6.u64 = 0; ctx.r7.u64 = 0;
        waiting.store(true, std::memory_order_release);
        synchronizationImport(ctx, base, "KeWaitForSingleObject");
        throw std::runtime_error("unsignaled guest audio event returned without cancellation");
    }
    if (client == 1 && stallRequested.exchange(false)) {
        ctx.r3.u64 = memory->read32(argument + 8);
        ctx.r4.u64 = 3; ctx.r5.u64 = 1; ctx.r6.u64 = 0; ctx.r7.u64 = 0;
        stallWaiting.store(true, std::memory_order_release);
        synchronizationImport(ctx, base, "KeWaitForSingleObject");
        require(ctx.r3.u32 == 0, "stalled PCM producer resumes only after its guest event is signaled");
        stallWaiting.store(false, std::memory_order_release);
    }
}
struct AudioScope {
    Memory* owner;
    ~AudioScope() { shutdownAudioServices(owner); }
};
struct Playback {
    uint64_t played = 0, submitted = 0, underruns = 0;
    uint32_t queued = 0, engineGlitches = 0;
};
Playback playback(uint32_t token) {
    Playback value;
    require(queryAudioPlayback(token, value.played, value.queued, value.submitted, value.underruns, value.engineGlitches),
        "real registered voice exposes actual device progress");
    require(value.queued <= 16, "PCM buffering remains bounded by the owned source slots");
    return value;
}
void playbackRate(uint32_t token, const char* phase) {
    const auto before = playback(token);
    const uint64_t began = GetTickCount64();
    Sleep(1000);
    const auto after = playback(token);
    const uint64_t elapsed = GetTickCount64() - began;
    const double rate = double(after.played - before.played) * 1000.0 / double(elapsed);
    fprintf(stderr, "Native audio %s: device_samples=%llu wall_ms=%llu measured_hz=%.3f queue=%u underruns=%llu engine_glitches=%u\n",
        phase, after.played - before.played, elapsed, rate, after.queued, after.underruns, after.engineGlitches);
    require(rate >= audioRate * 0.94 && rate <= audioRate * 1.06,
        "actual source samples play at the original 48000 Hz rather than callback or wall-clock guesses");
    require(after.underruns == before.underruns, "steady playback preserves source headroom without new underruns");
    require(after.submitted > before.submitted, "steady real playback is maintained by new original callback frames");
}
void shortGuestSleepContract(PPCContext& ctx, Memory& guest, uint32_t timeout) {
    // Exercise the real import used by the original FMOD workers,
    // rather than Windows Sleep or an invented audio clock. The median rejects
    // the default ~15.6 ms rounding while tolerating occasional scheduler load.
    guest.write64(timeout, uint64_t(int64_t(-10000)));
    std::array<double, 31> milliseconds{};
    for (double& elapsed : milliseconds) {
        ctx.r3.u64 = 1; ctx.r4.u64 = 0; ctx.r5.u64 = timeout;
        const auto began = std::chrono::steady_clock::now();
        require(synchronizationImport(ctx, guest.base(), "KeDelayExecutionThread") && ctx.r3.u32 == 0,
            "original one-millisecond FMOD delay succeeds through the actual native import");
        elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    }
    std::sort(milliseconds.begin(), milliseconds.end());
    const double median = milliseconds[milliseconds.size() / 2];
    fprintf(stderr, "Native audio original 1 ms guest sleep: median_ms=%.3f worst_ms=%.3f\n", median, milliseconds.back());
    require(median >= 0.5 && median < 8.0,
        "active native audio engine preserves short original FMOD sleeps without default 15.6 ms rounding");
}
void conversionContract() {
    std::array<uint8_t, audioFrameBytes> source{};
    std::array<float, audioChannels * audioFrames> output{};
    // Include zeros, subnormals, infinities and NaN payloads. Conversion moves
    // float bits without arithmetic, so every source bit must be preserved.
    constexpr uint32_t special[]{0,0x80000000,1,0x7f800000,0xff800000,0x7fc12345};
    for (uint32_t channel = 0; channel < audioChannels; ++channel) {
        for (uint32_t frame = 0; frame < audioFrames; ++frame) {
            const uint32_t value = frame == 0 ? special[channel] : 0x3e000000 + (channel << 18) + frame;
            uint8_t* entry = source.data() + (channel * audioFrames + frame) * 4;
            entry[0] = uint8_t(value >> 24); entry[1] = uint8_t(value >> 16); entry[2] = uint8_t(value >> 8); entry[3] = uint8_t(value);
        }
    }
    convertAudioPlanarBE(source.data(), output.data());
    for (uint32_t frame = 0; frame < audioFrames; ++frame)
        for (uint32_t channel = 0; channel < audioChannels; ++channel)
            require(std::bit_cast<uint32_t>(output[frame * audioChannels + channel]) ==
                (frame == 0 ? special[channel] : 0x3e000000 + (channel << 18) + frame), "six-channel planar endian conversion preserves bits and order");
}
void quietPcm(Memory& guest, uint32_t address, unsigned cycles) {
    // Integer cycles per frame keep successive buffers continuous. Only the
    // front channels carry this quiet waveform; total source gain is 0.01.
    constexpr double tau = 6.283185307179586476925286766559;
    for (uint32_t channel = 0; channel < audioChannels; ++channel)
        for (uint32_t frame = 0; frame < audioFrames; ++frame) {
            const float value = channel < 2 ? float(0.01 * std::sin(tau * cycles * frame / audioFrames)) : 0;
            guest.write32(address + (channel * audioFrames + frame) * 4, std::bit_cast<uint32_t>(value));
        }
    require(std::bit_cast<float>(guest.read32(address + 4)) != 0, "device contract uses actual nonzero quiet PCM");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        conversionContract();
        const auto image = loadOriginalXex(argv[1]);
        Memory guest(image); memory = &guest;
        AudioScope audio{&guest};
        PPCContext ctx{}; guest.initializeThread(ctx, 0x100000); currentContext = &ctx;
        const uint32_t state = guest.allocate(4096), samples = guest.allocate(audioFrameBytes), event = state + 256;
        const uint32_t secondState = guest.allocate(4096), secondSamples = guest.allocate(audioFrameBytes), secondEvent = secondState + 256;
        expectedArguments = {state, secondState};
        PPC_LOOKUP_FUNC(guest.base(), PPC_CODE_BASE) = testCallback;
        auto prepare = [&](uint32_t argument, uint32_t pcm, uint32_t dispatcherEvent, unsigned cycles) {
            guest.write32(argument + 16, PPC_CODE_BASE); guest.write32(argument + 20, argument);
            guest.write32(argument + 4, pcm); guest.write32(argument + 8, dispatcherEvent);
            // Match the original driver's PCM-ready synchronization event:
            // each producer stall must consume its own wake, without leaving
            // a manual-reset signal for the following stall to reuse.
            guest.write32(dispatcherEvent, 0x01000400); guest.write32(dispatcherEvent + 4, 0);
            guest.write32(dispatcherEvent + 8, dispatcherEvent + 8); guest.write32(dispatcherEvent + 12, dispatcherEvent + 8);
            quietPcm(guest, pcm, cycles);
        };
        prepare(state, samples, event, 2);
        prepare(secondState, secondSamples, secondEvent, 3);
        auto call = [&](const char* name, uint32_t first, uint32_t second = 0) {
            ctx.r3.u64 = first; ctx.r4.u64 = second;
            require(audioServicesImport(ctx, guest.base(), name), "unrecognized audio operation");
            return ctx.r3.u32;
        };
        require(call("XAudioGetSpeakerConfig", 0x70000000) == 0x80070057, "speaker query validates writable guest output");
        const uint32_t deviceStatus = call("XAudioGetSpeakerConfig", state + 24);
        if (deviceStatus) {
            fprintf(stderr, "PCM conversion passed; native audio device unavailable: 0x%08X\n", deviceStatus);
            currentContext = nullptr;
            return 77;
        }
        require(guest.read32(state + 24) == 0 || guest.read32(state + 24) == 0x80000000, "speaker mode derives from actual native device channels");
        shortGuestSleepContract(ctx, guest, state + 64);
        require(call("XAudioGetVoiceCategoryVolume", 1, state + 28) == 0, "native game audio bus gain query");
        const float gain = std::bit_cast<float>(guest.read32(state + 28));
        require(std::isfinite(gain) && gain >= 0, "guest gain output is finite big-endian float");
        require(call("XAudioRegisterRenderDriverClient", state + 16, 0x70000000) == 0x80070057,
            "render registration validates output before native client creation");
        require(call("XAudioRegisterRenderDriverClient", state + 16, state) == 0, "native render callback registration");
        const uint32_t token = guest.read32(state);
        require(token != 0, "first render client publishes a real token");
        require(call("XAudioRegisterRenderDriverClient", secondState + 16, secondState) == 0,
            "second original render client with distinct argument can coexist");
        const uint32_t secondToken = guest.read32(secondState);
        require(secondToken && secondToken != token, "coexisting original render clients own distinct tokens");
        guest.write32(state + 32, 0x12345678);
        require(call("XAudioRegisterRenderDriverClient", state + 16, state + 32) == 0x800700aa && guest.read32(state + 32) == 0x12345678,
            "same callback and argument duplicate is rejected without publishing a token");
        require(call("XAudioSubmitRenderDriverFrame", token ^ 0x10000, samples) == 0x80070057 &&
            guest.read32(state) == token && guest.read32(secondState) == secondToken, "invalid token cannot alter either real client");
        require(call("XAudioSubmitRenderDriverFrame", token, 0x70000000) == 0x80070057, "inaccessible PCM frame rejected");
        const uint64_t deadline = GetTickCount64() + 5000;
        while ((!waiting.load(std::memory_order_acquire) || callbackCount[1].load() < 28) && GetTickCount64() < deadline) Sleep(1);
        require(waiting.load() && callbackCount[0].load() == 28 && callbackCount[1].load() >= 28,
            "both nonzero PCM voices produce beyond the total source slot capacity");
        require(playback(token).played >= 12 * audioFrames && playback(secondToken).played >= 12 * audioFrames,
            "two distinct source voices have actually played PCM and completed device buffers");
        playbackRate(secondToken, "steady");
        auto resumeSecondProducer = [&] {
            ctx.r3.u64 = secondEvent; ctx.r4.u64 = 1; ctx.r5.u64 = 0;
            require(synchronizationImport(ctx, guest.base(), "KeSetEvent"), "second producer wake uses its actual guest event");
        };
        stallRequested.store(true);
        const uint64_t shortDeadline = GetTickCount64() + 2000;
        while (!stallWaiting.load(std::memory_order_acquire) && GetTickCount64() < shortDeadline) Sleep(1);
        require(stallWaiting.load(), "short producer stall starts in the actual guest mixer wait");
        const auto shortBefore = playback(secondToken);
        require(shortBefore.queued >= 10, "short-stall regression starts with at least 53 ms of actual PCM headroom");
        Sleep(40);
        const auto shortAfter = playback(secondToken);
        require(shortAfter.played > shortBefore.played && shortAfter.queued > 0 && shortAfter.underruns == shortBefore.underruns,
            "queued PCM covers a 40 ms producer stall without starving or stopping device playback");
        resumeSecondProducer();
        const uint64_t shortRecoveryDeadline = GetTickCount64() + 2000;
        Playback shortRecovered = playback(secondToken);
        while ((stallWaiting.load() || shortRecovered.queued < 10) && GetTickCount64() < shortRecoveryDeadline) {
            Sleep(1); shortRecovered = playback(secondToken);
        }
        require(!stallWaiting.load() && shortRecovered.queued >= 10,
            "short producer stall restores the original bounded queue headroom");
        const uint64_t underrunsBefore = playback(secondToken).underruns;
        stallRequested.store(true);
        const uint64_t stallDeadline = GetTickCount64() + 2000;
        while (!stallWaiting.load(std::memory_order_acquire) && GetTickCount64() < stallDeadline) Sleep(1);
        require(stallWaiting.load(), "second PCM producer deliberately blocks in its own guest dispatcher wait");
        const uint64_t drainDeadline = GetTickCount64() + 2000;
        Playback drained = playback(secondToken);
        while ((drained.queued || drained.underruns <= underrunsBefore) && GetTickCount64() < drainDeadline) {
            Sleep(1); drained = playback(secondToken);
        }
        require(drained.queued == 0 && drained.underruns > underrunsBefore,
            "a real producer stall drains queued PCM and is detected as source starvation");
        resumeSecondProducer();
        const uint64_t recoveryDeadline = GetTickCount64() + 2000;
        Playback recovered = playback(secondToken);
        while ((stallWaiting.load() || recovered.queued < 8 || recovered.played <= drained.played) && GetTickCount64() < recoveryDeadline) {
            Sleep(1); recovered = playback(secondToken);
        }
        require(!stallWaiting.load() && recovered.queued >= 8 && recovered.played > drained.played,
            "producer wake refills usable queue headroom and resumes actual device playback");
        playbackRate(secondToken, "recovered");
        const uint64_t teardownStart = GetTickCount64();
        require(call("XAudioUnregisterRenderDriverClient", token) == 0, "native teardown cancels callback blocked in guest dispatcher wait");
        require(GetTickCount64() - teardownStart < 2000, "cancellable callback joins promptly before guest memory is freed");
        require(call("XAudioUnregisterRenderDriverClient", token) == 0x80070057, "stale render token rejected after teardown");
        const unsigned secondBefore = callbackCount[1].load();
        const uint64_t secondDeadline = GetTickCount64() + 2000;
        while (callbackCount[1].load() <= secondBefore && GetTickCount64() < secondDeadline) Sleep(1);
        require(callbackCount[1].load() > secondBefore, "first callback teardown preserves second client playback and device credits");
        require(call("XAudioUnregisterRenderDriverClient", secondToken) == 0, "second client independently unregisters");
        require(call("XAudioUnregisterRenderDriverClient", secondToken) == 0x80070057, "second stale token is independently rejected");
        shutdownAudioServices(&guest); currentContext = nullptr;
        fprintf(stderr, "Native PCM endian/channel, two nonzero voices, actual 48000 Hz playback, starvation recovery, distinct callback ABI and cancellable teardown passed.\n");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "Native audio contract: %s\n", error.what()); return 1;
    }
}
