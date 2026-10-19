// Native XAudio2 PCM output; callback/planar contracts verified in Doritos
// sub_82620080, sub_8261FE68 and sub_8261FC98. Lifecycle design follows the
// GPLv3 native audio driver in The Darkness Recomp; no emulator runtime.
#include "runtime.h"
#include "audio_services.h"
#include "kernel_objects.h"
#include <objbase.h>
#include <xaudio2.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

namespace Doritos::Native {
void convertAudioPlanarBE(const uint8_t* source, float* output) {
    for (uint32_t frame = 0; frame < audioFrames; ++frame) {
        for (uint32_t channel = 0; channel < audioChannels; ++channel) {
            const uint8_t* input = source + (channel * audioFrames + frame) * 4;
            const uint32_t bits = (uint32_t(input[0]) << 24) | (uint32_t(input[1]) << 16) | (uint32_t(input[2]) << 8) | input[3];
            memcpy(output + frame * audioChannels + channel, &bits, 4);
        }
    }
}
namespace {
constexpr uint32_t badParameter = 0x80070057, busy = 0x800700aa;
constexpr uint32_t maxQueued = 16;
constexpr uint32_t targetQueued = 12, startQueued = 4;
struct Client;
struct PcmSlot {
    std::array<float, audioChannels * audioFrames> samples{};
    std::atomic<bool> busy{false};
    Client* owner = nullptr;
};
struct VoiceCallback : IXAudio2VoiceCallback {
    Client* owner = nullptr;
    void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override;
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT status) noexcept override;
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 bytesRequired) noexcept override;
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
};
struct Client {
    Memory* owner = nullptr;
    uint32_t token = 0, callback = 0, argument = 0, argumentCell = 0, threadToken = 0;
    HANDLE worker = nullptr, stop = nullptr, credit = nullptr;
    DWORD threadId = 0;
    PPCContext context{};
    IXAudio2SourceVoice* voice = nullptr;
    VoiceCallback voiceCallback;
    std::array<PcmSlot, maxQueued> slots;
    std::atomic<uint64_t> completed{0}, submitted{0};
    std::atomic<HRESULT> error{S_OK};
    std::atomic<bool> started{false}, terminalStop{false}, starving{false};
    std::atomic<uint64_t> underruns{0}, starvedPasses{0}, callbacks{0};
    uint64_t callbackNanoseconds = 0, worstCallbackNanoseconds = 0;
    bool closing = false;
    bool trace = false;
    uint64_t traceFrames = 0, traceNonzero = 0, traceInvalid = 0;
    double traceSquares = 0;
    float tracePeak = 0;
};
std::mutex audioMutex;
IXAudio2* engine = nullptr;
IXAudio2MasteringVoice* master = nullptr;
CO_MTA_USAGE_COOKIE mtaCookie = nullptr;
// Original FMOD workers sleep through 82524518/825223C8. The mixer setup
// 82537808 derives short millisecond periods, and other workers poll at 1 ms.
// A Windows process that has not requested timer resolution can turn each of
// those sleeps into about 15.6 ms, starving upstream PCM despite a full voice.
// Keep one balanced request for the native engine's lifetime, including both
// original render clients. No setting survives engine teardown.
struct AudioTimerPeriod {
    using Period = UINT (WINAPI*)(UINT);
    HMODULE library = nullptr;
    Period end = nullptr;
    bool active = false;
    void start() {
        if (active) return;
        library = LoadLibraryExW(L"winmm.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        UINT status = library ? ERROR_PROC_NOT_FOUND : GetLastError();
        if (library) {
            auto begin = reinterpret_cast<Period>(GetProcAddress(library, "timeBeginPeriod"));
            end = reinterpret_cast<Period>(GetProcAddress(library, "timeEndPeriod"));
            if (begin && end) { status = begin(1); active = status == 0; }
        }
        fprintf(stderr, "[AUDIO TIMER] requested_ms=1 active=%s status=%u\n", active ? "true" : "false", status);
        if (!active) { if (library) FreeLibrary(library); library = nullptr; end = nullptr; }
    }
    void stop() {
        if (active) { end(1); active = false; }
        if (library) { FreeLibrary(library); library = nullptr; }
        end = nullptr;
    }
    ~AudioTimerPeriod() { stop(); }
} audioTimer;
// The title retains its startup engine while constructing its own FMOD engine.
// Each original render driver needs its own voice, worker and callback credits.
std::map<uint32_t, std::shared_ptr<Client>> clients;
std::map<Memory*, uint32_t> shuttingDown;
std::condition_variable audioChanged;
uint32_t closingClients = 0;
uint32_t nextToken = 1;
uint64_t audioClock() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// MMCSS schedules this buffer-service worker without changing process priority.
// Dynamic loading keeps the native bridge self-contained.
struct AudioScheduling {
    using Set = HANDLE (WINAPI*)(LPCWSTR, LPDWORD);
    using Priority = BOOL (WINAPI*)(HANDLE, AVRT_PRIORITY);
    using Revert = BOOL (WINAPI*)(HANDLE);
    HMODULE library = nullptr;
    HANDLE task = nullptr;
    Revert revert = nullptr;
    explicit AudioScheduling(uint32_t token) {
        library = LoadLibraryExW(L"avrt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library) {
            auto set = reinterpret_cast<Set>(GetProcAddress(library, "AvSetMmThreadCharacteristicsW"));
            auto priority = reinterpret_cast<Priority>(GetProcAddress(library, "AvSetMmThreadPriority"));
            revert = reinterpret_cast<Revert>(GetProcAddress(library, "AvRevertMmThreadCharacteristics"));
            DWORD index = 0;
            if (set && revert) task = set(L"Audio", &index);
            if (task && priority) priority(task, AVRT_PRIORITY_HIGH);
        }
        const bool fallback = !task && SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        fprintf(stderr, "[AUDIO SCHEDULING] token=%08X mmcss_audio=%s above_normal_fallback=%s target_buffers=%u target_ms=%.3f\n",
            token, task ? "true" : "false", fallback ? "true" : "false", targetQueued, 1000.0 * targetQueued * audioFrames / audioRate);
    }
    ~AudioScheduling() { if (task) revert(task); if (library) FreeLibrary(library); }
};
// XAudio2 can release multiple 256-frame credits in one processing quantum.
// The original FMOD refill worker independently polls a four-block ring.
// Give it time to refill between callback consumptions, including startup
// and recovery bursts. Four ms still permits 250 callbacks/s, above the
// required 187.5, so the bounded native queue can recover its headroom.
struct CallbackCadence {
    HANDLE timer = nullptr;
    uint64_t next = 0;
    CallbackCadence() {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
        fprintf(stderr, "[AUDIO CADENCE] spacing_ms=4 high_resolution_timer=%s\n", timer ? "true" : "false");
    }
    bool wait(HANDLE stop) {
        for (;;) {
            const uint64_t now = audioClock();
            if (now >= next) {
                const DWORD status = WaitForSingleObject(stop, 0);
                if (status == WAIT_OBJECT_0) return false;
                if (status != WAIT_TIMEOUT) fail("Native audio cadence stop check failed");
                return true;
            }
            const uint64_t remaining = next - now;
            if (timer) {
                LARGE_INTEGER due{}; due.QuadPart = -int64_t((remaining + 99) / 100);
                if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) fail("Native audio cadence timer failed");
                HANDLE events[]{stop, timer};
                const DWORD status = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                if (status == WAIT_OBJECT_0) return false;
                if (status != WAIT_OBJECT_0 + 1) fail("Native audio cadence wait failed");
            } else {
                const DWORD status = WaitForSingleObject(stop, DWORD((remaining + 999999) / 1000000));
                if (status == WAIT_OBJECT_0) return false;
                if (status != WAIT_TIMEOUT) fail("Native audio cadence fallback wait failed");
            }
        }
    }
    void consumed() { next = audioClock() + 4000000; }
    ~CallbackCadence() { if (timer) CloseHandle(timer); }
    CallbackCadence(const CallbackCadence&) = delete;
    CallbackCadence& operator=(const CallbackCadence&) = delete;
};
bool span(Memory* guest, uint32_t address, uint32_t bytes, bool output = false) {
    if (!guest || !guest->valid(address, bytes)) return false;
    uint64_t cursor = address, end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(guest->base() + cursor, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        DWORD protection = info.Protect & 0xff;
        const bool write = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (output ? !write : !(write || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ)) return false;
        uint64_t next = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize - guest->base();
        if (next <= cursor) return false;
        cursor = std::min(next, end);
    }
    return true;
}
HRESULT ensureEngine() {
    if (master) return S_OK;
    HRESULT status = CoIncrementMTAUsage(&mtaCookie);
    if (FAILED(status)) return status;
    status = XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(status)) status = engine->CreateMasteringVoice(&master);
    if (FAILED(status)) {
        if (engine) { engine->Release(); engine = nullptr; }
        CoDecrementMTAUsage(mtaCookie); mtaCookie = nullptr;
    } else audioTimer.start();
    return status;
}
void releaseEngine() {
    if (master) { master->DestroyVoice(); master = nullptr; }
    if (engine) { engine->Release(); engine = nullptr; }
    if (mtaCookie) { CoDecrementMTAUsage(mtaCookie); mtaCookie = nullptr; }
    audioTimer.stop();
}
void invokeGuest(Client* client) {
    __try {
        client->context.r3.u64 = client->argumentCell;
        auto function = PPC_LOOKUP_FUNC(client->owner->base(), client->callback);
        function(client->context, client->owner->base());
    } __except (exceptionFilter(GetExceptionInformation())) { fflush(stderr); ExitProcess(3); }
}
DWORD WINAPI audioWorker(void* raw) {
    auto* client = static_cast<Client*>(raw);
    currentContext = &client->context;
    client->context.fpscr.loadFromHost();
    client->owner->write32(client->owner->read32(client->context.r13.u32 + 0x100) + 0x14c, GetCurrentThreadId());
    const HANDLE previous = setDispatcherCancellation(client->stop);
    AudioScheduling scheduling(client->token);
    CallbackCadence cadence;
    uint64_t reportAt = audioClock(), reportPlayed = 0, reportSubmitted = 0, reportCallbacks = 0;
    uint32_t minimumQueued = targetQueued;
    try {
        while (WaitForSingleObject(client->stop, 0) != WAIT_OBJECT_0) {
            if (FAILED(client->error.load())) fail("Native audio output device failed");
            XAUDIO2_VOICE_STATE state{};
            client->voice->GetState(&state);
            minimumQueued = std::min(minimumQueued, state.BuffersQueued);
            const uint64_t now = audioClock();
            if (client->trace && now - reportAt >= 1000000000ull) {
                XAUDIO2_PERFORMANCE_DATA performance{}; engine->GetPerformanceData(&performance);
                const uint64_t submitted = client->submitted.load(), callbacks = client->callbacks.load();
                const double interval = double(now - reportAt) / 1e9;
                fprintf(stderr, "[AUDIO RATE] token=%08X interval_s=%.3f played_samples=%llu played_hz=%.3f mixed_hz=%.3f callbacks_hz=%.3f queued=%u min_queued=%u underruns=%llu starved_passes=%llu engine_glitches=%u callback_avg_ms=%.3f callback_worst_ms=%.3f started=%s\n",
                    client->token, interval, state.SamplesPlayed, double(state.SamplesPlayed - reportPlayed) / interval,
                    double(submitted - reportSubmitted) * audioFrames / interval, double(callbacks - reportCallbacks) / interval,
                    state.BuffersQueued, minimumQueued, client->underruns.load(), client->starvedPasses.load(), performance.GlitchesSinceEngineStarted,
                    callbacks ? double(client->callbackNanoseconds) / double(callbacks) / 1e6 : 0,
                    double(client->worstCallbackNanoseconds) / 1e6, client->started.load() ? "true" : "false");
                fflush(stderr);
                reportAt = now; reportPlayed = state.SamplesPlayed; reportSubmitted = submitted; reportCallbacks = callbacks;
                minimumQueued = state.BuffersQueued;
            }
            const bool slotAvailable = std::any_of(client->slots.begin(), client->slots.end(),
                [](const PcmSlot& slot) { return !slot.busy.load(std::memory_order_acquire); });
            // GetState can reflect engine progress before the corresponding
            // completion releases a slot. Do not consume a guest mixer frame
            // until there is also owned storage for its submission.
            if (state.BuffersQueued >= targetQueued || !slotAvailable) {
                HANDLE events[]{client->stop, client->credit};
                if (WaitForMultipleObjects(2, events, FALSE, 100) == WAIT_FAILED) fail("Native audio queue wait failed");
                continue;
            }
            if (!cadence.wait(client->stop)) break;
            const uint64_t before = client->submitted.load(std::memory_order_relaxed);
            const uint64_t began = audioClock();
            invokeGuest(client);
            cadence.consumed();
            const uint64_t elapsed = audioClock() - began;
            client->callbacks.fetch_add(1);
            client->callbackNanoseconds += elapsed;
            client->worstCallbackNanoseconds = std::max(client->worstCallbackNanoseconds, elapsed);
            if (!client->started.load()) {
                // Serialize voice startup with the terminal close request so
                // a late callback cannot restart a voice that was just muted.
                std::lock_guard lock(audioMutex);
                client->voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                if (!client->closing && !client->terminalStop.load() && state.BuffersQueued >= startQueued) {
                    const HRESULT status = client->voice->Start();
                    if (FAILED(status)) fail("Native audio source could not start after PCM prefill");
                    client->started.store(true);
                }
            }
            if (client->submitted.load(std::memory_order_relaxed) == before) {
                WaitForSingleObject(client->stop, 6);
            }
        }
    } catch (const DispatcherWaitCancelled&) {}
    catch (const std::exception& error) {
        fprintf(stderr, "[AUDIO STOP] %s\n", error.what()); fflush(stderr); ExitProcess(1);
    }
    setDispatcherCancellation(previous);
    currentContext = nullptr;
    return 0;
}
void VoiceCallback::OnBufferEnd(void* context) noexcept {
    auto* slot = static_cast<PcmSlot*>(context);
    slot->busy.store(false, std::memory_order_release);
    owner->completed.fetch_add(1, std::memory_order_release);
    SetEvent(owner->credit);
}
void VoiceCallback::OnVoiceError(void*, HRESULT status) noexcept {
    owner->error.store(status, std::memory_order_release);
    SetEvent(owner->credit);
}
void VoiceCallback::OnVoiceProcessingPassStart(UINT32 bytesRequired) noexcept {
    if (!owner->started.load(std::memory_order_relaxed) || owner->terminalStop.load(std::memory_order_relaxed)) return;
    if (bytesRequired) {
        owner->starvedPasses.fetch_add(1, std::memory_order_relaxed);
        if (!owner->starving.exchange(true, std::memory_order_relaxed)) owner->underruns.fetch_add(1, std::memory_order_relaxed);
        SetEvent(owner->credit);
    } else owner->starving.store(false, std::memory_order_relaxed);
}
void destroyClient(const std::shared_ptr<Client>& client) {
    if (!client) return;
    if (client->worker) {
        if (GetCurrentThreadId() == client->threadId) { fprintf(stderr, "[AUDIO STOP] Cannot tear down the executing callback stack.\n"); fflush(stderr); ExitProcess(1); }
        SetEvent(client->stop);
        if (WaitForSingleObject(client->worker, 5000) != WAIT_OBJECT_0) {
            fprintf(stderr, "[AUDIO STOP] Callback worker still owns guest memory.\n"); fflush(stderr); ExitProcess(1);
        }
    }
    if (client->voice) { client->voice->Stop(); client->voice->DestroyVoice(); client->voice = nullptr; }
    if (client->worker) { CloseHandle(client->worker); client->worker = nullptr; }
    if (client->credit) { CloseHandle(client->credit); client->credit = nullptr; }
    if (client->stop) { CloseHandle(client->stop); client->stop = nullptr; }
    if (client->threadToken) { closeObject(client->threadToken); client->threadToken = 0; }
    else if (client->owner && client->context.r13.u32) {
        client->owner->release(client->owner->read32(client->context.r13.u32 + 0x74));
        client->owner->release(client->owner->read32(client->context.r13.u32));
        client->owner->release(client->context.r13.u32);
    }
    if (client->argumentCell) { client->owner->release(client->argumentCell); client->argumentCell = 0; }
}
uint32_t registerClient(uint32_t descriptor, uint32_t output) {
    if (!span(memory, descriptor, 8) || !span(memory, output, 4, true)) return badParameter;
    const uint32_t callback = memory->read32(descriptor), argument = memory->read32(descriptor + 4);
    if (callback < PPC_CODE_BASE || callback >= PPC_CODE_BASE + PPC_CODE_SIZE || (callback & 3) || !PPC_LOOKUP_FUNC(memory->base(), callback)) return badParameter;
    std::lock_guard lock(audioMutex);
    if (shuttingDown.contains(memory)) return busy;
    for (const auto& [token, client] : clients)
        if (client->owner == memory && client->callback == callback && client->argument == argument) return busy;
    if (nextToken > 0xffff) return 0x8007000e;
    HRESULT status = ensureEngine();
    if (FAILED(status)) return uint32_t(status);
    auto client = std::make_shared<Client>();
    client->owner = memory; client->callback = callback; client->argument = argument; client->voiceCallback.owner = client.get();
    char trace[2]{};
    client->trace = GetEnvironmentVariableA("DORITOS_AUDIO_TRACE", trace, sizeof(trace)) == 1 && trace[0] == '1';
    auto reject = [&](HRESULT error) { destroyClient(client); return uint32_t(error); };
    client->argumentCell = memory->allocate(4);
    memory->write32(client->argumentCell, argument);
    memory->initializeThread(client->context, 0x100000);
    for (auto& slot : client->slots) slot.owner = client.get();
    client->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client->credit = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!client->stop || !client->credit) return reject(HRESULT_FROM_WIN32(GetLastError()));
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = audioChannels; format.Format.nSamplesPerSec = audioRate;
    format.Format.wBitsPerSample = 32; format.Format.nBlockAlign = audioChannels * 4;
    format.Format.nAvgBytesPerSec = audioRate * audioChannels * 4; format.Format.cbSize = 22;
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
        SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    status = engine->CreateSourceVoice(&client->voice, &format.Format, 0, 1.0f, &client->voiceCallback);
    if (FAILED(status)) return reject(status);
    XAUDIO2_VOICE_DETAILS destination{};
    master->GetVoiceDetails(&destination);
    std::vector<float> matrix(audioChannels * destination.InputChannels);
    client->voice->GetOutputMatrix(master, audioChannels, destination.InputChannels, matrix.data());
    float largest = 1;
    for (uint32_t channel = 0; channel < destination.InputChannels; ++channel) {
        float sum = 0;
        for (uint32_t source = 0; source < audioChannels; ++source) sum += std::fabs(matrix[source * destination.InputChannels + channel]);
        largest = std::max(largest, sum);
    }
    status = client->voice->SetVolume(1.0f / largest);
    if (FAILED(status)) return reject(status);
    client->worker = CreateThread(nullptr, 64 * 1024 * 1024, audioWorker, client.get(),
        CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &client->threadId);
    if (!client->worker) return reject(HRESULT_FROM_WIN32(GetLastError()));
    HANDLE registered = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), client->worker, GetCurrentProcess(), &registered, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        // Suspended native worker has not acquired guest storage yet.
        fprintf(stderr, "[AUDIO STOP] Could not register owned callback thread.\n"); fflush(stderr); ExitProcess(1);
    }
    const uint32_t pcr = client->context.r13.u32, thread = memory->read32(pcr + 0x100);
    client->threadToken = registerObject(registered, ObjectKind::Thread, thread);
    auto object = lookupObject(client->threadToken);
    object->threadId = client->threadId; object->owner = memory;
    object->allocations = {memory->read32(pcr + 0x74), memory->read32(pcr), pcr};
    client->token = 0x41550000 | nextToken++;
    clients.emplace(client->token, client);
    memory->write32(output, client->token);
    if (ResumeThread(client->worker) == DWORD(-1)) { fprintf(stderr, "[AUDIO STOP] Callback worker could not start.\n"); fflush(stderr); ExitProcess(1); }
    fprintf(stderr, "[AUDIO] Native XAudio2 output registered callback=0x%08X token=0x%08X deviceChannels=%u\n", callback, client->token, destination.InputChannels);
    return 0;
}
uint32_t unregisterClient(uint32_t token) {
    std::shared_ptr<Client> client;
    { std::lock_guard lock(audioMutex);
      const auto found = clients.find(token);
      if (found == clients.end() || found->second->closing) return badParameter;
      client = found->second; client->closing = true; ++closingClients;
      // Close and startup share this lock. Silence the voice before joining a
      // callback that may be blocked in its original guest mixer wait.
      SetEvent(client->stop); SetEvent(client->credit);
      client->voice->SetVolume(0); client->voice->Stop(); }
    destroyClient(client);
    { std::lock_guard lock(audioMutex); clients.erase(token); --closingClients; }
    audioChanged.notify_all();
    return 0;
}
uint32_t submit(uint32_t token, uint32_t samples) {
    std::lock_guard lock(audioMutex);
    const auto found = clients.find(token);
    if (found == clients.end()) return badParameter;
    const auto& client = found->second;
    if (client->terminalStop.load()) return badParameter;
    if (client->closing && GetCurrentThreadId() != client->threadId) return badParameter;
    if (!span(client->owner, samples, audioFrameBytes)) return badParameter;
    if (FAILED(client->error.load())) return uint32_t(client->error.load());
    for (auto& slot : client->slots) {
        if (slot.busy.exchange(true, std::memory_order_acquire)) continue;
        convertAudioPlanarBE(client->owner->base() + samples, slot.samples.data());
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = audioFrameBytes; buffer.pAudioData = reinterpret_cast<const BYTE*>(slot.samples.data()); buffer.pContext = &slot;
        const HRESULT status = client->voice->SubmitSourceBuffer(&buffer);
        if (FAILED(status)) { slot.busy.store(false, std::memory_order_release); return uint32_t(status); }
        client->submitted.fetch_add(1, std::memory_order_relaxed);
        if (client->trace) {
            bool nonzero = false;
            for (float sample : slot.samples) {
                if (!std::isfinite(sample)) { ++client->traceInvalid; continue; }
                const float magnitude = std::fabs(sample);
                client->tracePeak = std::max(client->tracePeak, magnitude);
                client->traceSquares += double(sample) * sample;
                nonzero |= magnitude > 0;
            }
            client->traceNonzero += nonzero;
            if (++client->traceFrames % 1875 == 0) {
                fprintf(stderr, "[AUDIO PCM] token=%08X buffers=%llu completed=%llu nonzero=%llu invalid=%llu peak=%.6f rms=%.6f\n",
                    client->token, client->traceFrames, client->completed.load(), client->traceNonzero, client->traceInvalid,
                    client->tracePeak, std::sqrt(client->traceSquares / (double(client->traceFrames) * audioFrames * audioChannels)));
                fflush(stderr);
            }
        }
        return 0;
    }
    return busy;
}
}
// Diagnostic contract used by the real-device regression; no invented clocks
// or queue progress. A closing client is excluded before touching its voice.
bool queryAudioPlayback(uint32_t token, uint64_t& played, uint32_t& queued, uint64_t& submitted, uint64_t& underruns, uint32_t& engineGlitches) {
    std::lock_guard lock(audioMutex);
    const auto found = clients.find(token);
    if (found == clients.end() || found->second->closing) return false;
    XAUDIO2_VOICE_STATE state{}; found->second->voice->GetState(&state);
    played = state.SamplesPlayed; queued = state.BuffersQueued;
    submitted = found->second->submitted.load(); underruns = found->second->underruns.load();
    XAUDIO2_PERFORMANCE_DATA performance{}; engine->GetPerformanceData(&performance);
    engineGlitches = performance.GlitchesSinceEngineStarted;
    return true;
}
bool audioServicesImport(PPCContext& ctx, uint8_t*, const char* name) {
    const std::string_view function(name);
    auto result = [&](uint32_t status) { ctx.r3.u64 = status; return true; };
    if (function == "XAudioGetSpeakerConfig") {
        const uint32_t output = ctx.r3.u32;
        if (!span(memory, output, 4, true)) return result(badParameter);
        std::lock_guard lock(audioMutex);
        const HRESULT status = ensureEngine();
        if (FAILED(status)) return result(uint32_t(status));
        XAUDIO2_VOICE_DETAILS details{}; master->GetVoiceDetails(&details);
        // Doritos consumes only bit31: stereo uses its two-channel mode.
        // Other console output capabilities are not invented for a PC device.
        memory->write32(output, details.InputChannels <= 2 ? 0x80000000 : 0);
        fprintf(stderr, "[AUDIO] Native output channels=%u rate=%u\n", details.InputChannels, details.InputSampleRate);
        return result(0);
    }
    if (function == "XAudioGetVoiceCategoryVolume") {
        const uint32_t category = ctx.r3.u32, output = ctx.r4.u32;
        if (category != 1) fail("Audio category requires a verified native bus mapping");
        if (!span(memory, output, 4, true)) return result(badParameter);
        std::lock_guard lock(audioMutex);
        const HRESULT status = ensureEngine();
        if (FAILED(status)) return result(uint32_t(status));
        float volume; master->GetVolume(&volume);
        memory->write32(output, std::bit_cast<uint32_t>(volume)); return result(0);
    }
    if (function == "XAudioRegisterRenderDriverClient") return result(registerClient(ctx.r3.u32, ctx.r4.u32));
    if (function == "XAudioSubmitRenderDriverFrame") return result(submit(ctx.r3.u32, ctx.r4.u32));
    if (function == "XAudioUnregisterRenderDriverClient") return result(unregisterClient(ctx.r3.u32));
    return false;
}
void shutdownAudioServices(Memory* owner) {
    std::vector<uint32_t> owned;
    { std::lock_guard lock(audioMutex);
      ++shuttingDown[owner];
      for (const auto& [token, client] : clients) if (client->owner == owner && !client->closing) owned.push_back(token); }
    for (uint32_t token : owned) unregisterClient(token);
    std::unique_lock lock(audioMutex);
    audioChanged.wait(lock, [&] {
        return std::none_of(clients.begin(), clients.end(), [&](const auto& entry) { return entry.second->owner == owner; });
    });
    if (clients.empty() && !closingClients) releaseEngine();
    if (!--shuttingDown[owner]) shuttingDown.erase(owner);
}
void requestAudioServicesStop(Memory* owner) {
    std::lock_guard lock(audioMutex);
    // This is a terminal process-close request. Retain its registration barrier
    // and all guest allocations; the lifecycle owner separately ends workers.
    if (!shuttingDown.contains(owner)) shuttingDown.emplace(owner, 1);
    for (const auto& [token, client] : clients) {
        if (client->owner != owner || client->closing) continue;
        client->terminalStop.store(true);
        SetEvent(client->stop); SetEvent(client->credit);
        if (client->voice) { client->voice->SetVolume(0); client->voice->Stop(); }
    }
}
}
