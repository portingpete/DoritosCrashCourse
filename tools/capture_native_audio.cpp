// Diagnostic capture of one process tree's rendered audio. No endpoint or
// microphone capture. API contract: Microsoft's ApplicationLoopback sample:
// https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/ApplicationLoopback
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <audioclientactivationparams.h>
#include <wrl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
void checked(HRESULT status, const char* operation) {
    if (FAILED(status)) {
        char message[256];
        std::snprintf(message, sizeof(message), "%s failed: 0x%08lX (process loopback requires Windows build 20348 or newer)",
                      operation, static_cast<unsigned long>(status));
        throw std::runtime_error(message);
    }
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct ComScope {
    ComScope() { checked(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx"); }
    ~ComScope() { CoUninitialize(); }
};
class Activation final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    Handle done;
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    PROPVARIANT variant{};
    HRESULT status = E_PENDING;
    ComPtr<IAudioClient> client;
    HRESULT initialize(DWORD pid) {
        done.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!done.value) return HRESULT_FROM_WIN32(GetLastError());
        parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        parameters.ProcessLoopbackParams.TargetProcessId = pid;
        parameters.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        variant.vt = VT_BLOB;
        variant.blob.cbSize = sizeof(parameters);
        variant.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT result = E_UNEXPECTED;
        ComPtr<IUnknown> audio;
        status = operation->GetActivateResult(&result, &audio);
        if (SUCCEEDED(status)) status = result;
        if (SUCCEEDED(status)) status = audio.As(&client);
        SetEvent(done.value);
        return S_OK;
    }
};
struct RunningCapture {
    ComPtr<IAudioClient> client;
    bool started = false;
    ~RunningCapture() { if (started) client->Stop(); }
};
void putU16(uint8_t* bytes, uint16_t value) {
    bytes[0] = uint8_t(value); bytes[1] = uint8_t(value >> 8);
}
void putU32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (i * 8));
}
void writeAll(HANDLE file, const void* data, DWORD bytes) {
    auto cursor = static_cast<const uint8_t*>(data);
    while (bytes) {
        DWORD written = 0;
        if (!WriteFile(file, cursor, bytes, &written, nullptr) || !written)
            throw std::runtime_error("Output WAV write failed");
        cursor += written; bytes -= written;
    }
}
void writeWav(const std::filesystem::path& path, const std::vector<int16_t>& samples) {
    const uint32_t bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    // CREATE_NEW makes the no-overwrite guarantee atomic, including races
    // with another recorder that creates the same evidence path.
    Handle out{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (out.value == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            throw std::runtime_error("Output WAV already exists");
        throw std::runtime_error("Could not create output WAV");
    }
    std::array<uint8_t, 44> header{};
    std::memcpy(header.data(), "RIFF", 4); putU32(header.data() + 4, 36 + bytes);
    std::memcpy(header.data() + 8, "WAVEfmt ", 8); putU32(header.data() + 16, 16);
    putU16(header.data() + 20, 1); putU16(header.data() + 22, 2); putU32(header.data() + 24, 48000);
    putU32(header.data() + 28, 48000 * 4); putU16(header.data() + 32, 4); putU16(header.data() + 34, 16);
    std::memcpy(header.data() + 36, "data", 4); putU32(header.data() + 40, bytes);
    writeAll(out.value, header.data(), DWORD(header.size()));
    writeAll(out.value, samples.data(), bytes);
    if (!FlushFileBuffers(out.value)) throw std::runtime_error("Output WAV flush failed");
}
uint64_t creationIdentity(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        throw std::runtime_error("Could not read target process creation identity");
    return (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
std::string jsonString(const std::wstring& value) {
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    if (!bytes && !value.empty()) throw std::runtime_error("Could not encode target executable path");
    std::string utf8(size_t(bytes), '\0');
    if (bytes && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), utf8.data(), bytes, nullptr, nullptr))
        throw std::runtime_error("Could not encode target executable path");
    std::string result = "\"";
    for (unsigned char character : utf8) {
        if (character == '"' || character == '\\') { result += '\\'; result += char(character); }
        else if (character < 32) {
            char escaped[7]; std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(character)); result += escaped;
        } else result += char(character);
    }
    return result + '"';
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "Usage: NativeAudioCapture <target-pid> <seconds:0.1..600> <new-output.wav>\n");
        return 2;
    }
    try {
        wchar_t* end = nullptr;
        const unsigned long pidValue = std::wcstoul(argv[1], &end, 10);
        if (!*argv[1] || *end || !pidValue || pidValue == GetCurrentProcessId() || *argv[1] == L'-')
            throw std::runtime_error("A different, live target PID is required");
        const double seconds = std::wcstod(argv[2], &end);
        if (!*argv[2] || *end || !std::isfinite(seconds) || seconds < 0.1 || seconds > 600)
            throw std::runtime_error("Duration must be between 0.1 and 600 seconds");
        Handle target{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pidValue))};
        if (!target.value || WaitForSingleObject(target.value, 0) != WAIT_TIMEOUT)
            throw std::runtime_error("Target PID does not identify an accessible live process");
        const uint64_t created = creationIdentity(target.value);
        std::wstring executable(32768, L'\0');
        DWORD pathLength = DWORD(executable.size());
        if (!QueryFullProcessImageNameW(target.value, 0, executable.data(), &pathLength))
            throw std::runtime_error("Could not read retained target executable identity");
        executable.resize(pathLength);
        const std::string executableJson = jsonString(executable);
        const std::filesystem::path output(argv[3]);
        if (std::filesystem::exists(output)) throw std::runtime_error("Output WAV already exists");
        ComScope com;
        auto activation = Microsoft::WRL::Make<Activation>();
        if (!activation) throw std::bad_alloc();
        checked(activation->initialize(DWORD(pidValue)), "Activation event");
        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        // The retained process handle anchors identity; also reject an exit
        // during setup immediately before the PID-based activation call.
        if (WaitForSingleObject(target.value, 0) != WAIT_TIMEOUT || creationIdentity(target.value) != created)
            throw std::runtime_error("Retained target exited or changed identity before audio activation");
        checked(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                &activation->variant, activation.Get(), &operation), "Process loopback activation");
        if (WaitForSingleObject(activation->done.value, 15000) != WAIT_OBJECT_0)
            throw std::runtime_error("Process loopback activation timed out");
        checked(activation->status, "Process loopback completion");
        // Keep the capture event alive until a running client has stopped,
        // including exception unwinding.
        Handle ready{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        if (!ready.value) throw std::runtime_error("Could not create capture event");
        RunningCapture session{activation->client};
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; format.nChannels = 2;
        format.nSamplesPerSec = 48000; format.wBitsPerSample = 32;
        format.nBlockAlign = 8; format.nAvgBytesPerSec = 48000 * 8;
        checked(session.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
            0, 0, &format, nullptr), "Process capture Initialize");
        checked(session.client->SetEventHandle(ready.value), "SetEventHandle");
        ComPtr<IAudioCaptureClient> capture;
        checked(session.client->GetService(IID_PPV_ARGS(&capture)), "Capture service");
        std::vector<int16_t> samples;
        const uint64_t maximumFrames = uint64_t(std::ceil(seconds * 48000));
        samples.reserve(size_t(maximumFrames * 2));
        uint64_t frames = 0, packets = 0, nonzeroFrames = 0, finiteSamples = 0, nonfiniteSamples = 0, discontinuities = 0;
        long double energy = 0;
        double peak = 0;
        checked(session.client->Start(), "Capture Start"); session.started = true;
        const uint64_t began = GetTickCount64(), deadline = began + uint64_t(std::ceil(seconds * 1000));
        bool exited = false;
        std::fprintf(stderr, "[AUDIO CAPTURE] scope=target_process_tree pid=%lu rate=48000 channels=2 seconds=%.3f\n", pidValue, seconds);
        while (GetTickCount64() < deadline && frames < maximumFrames) {
            HANDLE events[]{ready.value, target.value};
            const DWORD remaining = DWORD(std::min<uint64_t>(100, deadline - std::min(deadline, GetTickCount64())));
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, remaining);
            if (wait == WAIT_FAILED) throw std::runtime_error("Capture event wait failed");
            if (wait == WAIT_OBJECT_0 + 1) { exited = true; break; }
            UINT32 available = 0;
            checked(capture->GetNextPacketSize(&available), "GetNextPacketSize");
            while (available && frames < maximumFrames) {
                BYTE* data = nullptr; UINT32 count = 0; DWORD flags = 0;
                checked(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr), "Capture GetBuffer");
                const UINT32 take = UINT32(std::min<uint64_t>(count, maximumFrames - frames));
                ++packets;
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities;
                for (UINT32 frame = 0; frame < take; ++frame) {
                    bool nonzero = false;
                    for (UINT32 channel = 0; channel < 2; ++channel) {
                        float value = 0;
                        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                            std::memcpy(&value, data + (frame * 2 + channel) * sizeof(float), sizeof(value));
                        if (std::isfinite(value)) {
                            ++finiteSamples; energy += static_cast<long double>(value) * value;
                            peak = std::max(peak, std::fabs(double(value)));
                            nonzero |= std::fabs(value) > 1e-7f;
                        } else { ++nonfiniteSamples; value = 0; }
                        const double bounded = std::clamp(double(value), -1.0, 1.0);
                        samples.push_back(int16_t(std::lround(bounded * (bounded < 0 ? 32768 : 32767))));
                    }
                    nonzeroFrames += nonzero;
                }
                frames += take;
                checked(capture->ReleaseBuffer(count), "Capture ReleaseBuffer");
                checked(capture->GetNextPacketSize(&available), "GetNextPacketSize");
            }
        }
        checked(session.client->Stop(), "Capture Stop"); session.started = false;
        writeWav(output, samples);
        const double rms = finiteSamples ? std::sqrt(double(energy / finiteSamples)) : 0;
        std::printf("{\"scope\":\"target_process_tree\",\"pid\":%lu,\"process_creation_filetime\":\"0x%016llX\",\"executable\":%s,\"rate\":48000,\"channels\":2,"
            "\"frames\":%llu,\"packets\":%llu,\"nonzero_frames\":%llu,\"finite_samples\":%llu,"
            "\"nonfinite_samples\":%llu,\"peak\":%.9g,\"rms\":%.9g,\"discontinuities\":%llu,"
            "\"target_exited\":%s,\"captured_seconds\":%.6f,\"wall_seconds\":%.6f}\n",
            pidValue, created, executableJson.c_str(), frames, packets, nonzeroFrames, finiteSamples, nonfiniteSamples, peak, rms, discontinuities,
            exited ? "true" : "false", double(frames) / 48000, double(GetTickCount64() - began) / 1000);
        return frames && !nonfiniteSamples ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native process audio capture: %s\n", error.what()); return 1;
    }
}
