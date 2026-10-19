// Hidden D3D9Ex regression for in-place title buffer updates. A draw is queued
// from static buffers, both resources are changed while still bound, and a
// second draw must consume the replacement bytes without disturbing aliases.
#include "renderer/native/d3d9_backend.h"
#include "renderer/native/frame_pacer.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Doritos::Graphics;
namespace {
using Microsoft::WRL::ComPtr;
unsigned checks = 0;

void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(uint32_t(result)) + ")");
}

struct FixtureWindow {
    HWND window = nullptr;
    FixtureWindow() {
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"DoritosNativeStreamingBufferFixture";
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Cannot register hidden streaming-buffer fixture window");
        window = CreateWindowW(type.lpszClassName, L"Hidden streaming-buffer fixture", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, type.hInstance, nullptr);
        if (!window || IsWindowVisible(window))
            throw std::runtime_error("Streaming-buffer fixture window must remain hidden");
    }
    ~FixtureWindow() { if (window) DestroyWindow(window); }
};

std::vector<uint8_t> compile(const char* source, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), "native_streaming_buffers", nullptr, nullptr,
        "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY,
        0, &code, &errors);
    if (FAILED(result)) throw std::runtime_error(errors ?
        std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) :
        "Cannot compile streaming-buffer fixture shader");
    const auto* begin = static_cast<const uint8_t*>(code->GetBufferPointer());
    return {begin, begin + code->GetBufferSize()};
}

void appendWord(std::vector<uint8_t>& data, uint32_t value, bool bigEndian) {
    for (int byte = 0; byte < 4; ++byte)
        data.push_back(uint8_t(value >> ((bigEndian ? 3 - byte : byte) * 8)));
}

struct CacheShader { const char* name; bool vertex; std::vector<uint8_t> code; };

std::vector<uint8_t> shaderCache(const std::array<CacheShader, 2>& shaders, bool bigEndian) {
    std::vector<uint8_t> data;
    appendWord(data, 3, bigEndian);
    appendWord(data, 0, bigEndian);
    appendWord(data, uint32_t(shaders.size()), bigEndian);
    for (const auto& shader : shaders) {
        const size_t length = std::strlen(shader.name);
        appendWord(data, uint32_t(length + 1), bigEndian);
        data.insert(data.end(), shader.name, shader.name + length + 1);
        appendWord(data, 1, bigEndian);
        appendWord(data, 0, bigEndian);
        appendWord(data, 0, bigEndian);
        appendWord(data, 0, bigEndian);
        appendWord(data, 0, bigEndian);
        appendWord(data, uint32_t(shader.code.size()), bigEndian);
        data.insert(data.end(), shader.code.begin(), shader.code.end());
    }
    return data;
}

struct Vertex {
    float x, y, z;
    uint32_t color;
};
static_assert(sizeof(Vertex) == 16);

struct BoundStream {
    ComPtr<IDirect3DVertexBuffer9> buffer;
    UINT offset = 0, stride = 0;
};

BoundStream stream(IDirect3DDevice9* device, UINT index) {
    BoundStream result;
    check(device->GetStreamSource(index, &result.buffer, &result.offset, &result.stride),
        "Query actual native vertex stream");
    return result;
}

ComPtr<IDirect3DIndexBuffer9> indices(IDirect3DDevice9* device) {
    ComPtr<IDirect3DIndexBuffer9> result;
    check(device->GetIndices(&result), "Query actual native index binding");
    return result;
}

uint32_t pixel(const std::vector<uint32_t>& pixels, uint32_t x, uint32_t y) {
    return pixels[size_t(y) * 64 + x];
}

std::pair<uint32_t, uint32_t> createFixtureShaders(D3D9Backend& backend, const char* prefix) {
    const std::string vertexName = std::string(prefix) + "/vs";
    const std::string pixelName = std::string(prefix) + "/ps";
    const std::array<CacheShader, 2> shaders{{
        {vertexName.c_str(), true, compile(
            "struct O{float4 p:POSITION0;float4 c:COLOR0;}; O main(float3 p:POSITION0,float4 c:COLOR0){O o;o.p=float4(p,1);o.c=c;return o;}", "vs_3_0")},
        {pixelName.c_str(), false, compile(
            "float4 main(float4 c:COLOR0):COLOR0{return c;}", "ps_3_0")},
    }};
    const auto windowsCache = shaderCache(shaders, false);
    const auto xenonCache = shaderCache(shaders, true);
    backend.loadCaches(windowsCache, xenonCache);
    const ShaderKey key{0, 0, 0};
    return {backend.createShader(vertexName, key), backend.createShader(pixelName, key)};
}

double percentileValue(std::vector<double> values, unsigned percentile) {
    std::sort(values.begin(), values.end());
    const size_t rank = std::max<size_t>(1, (size_t(percentile) * values.size() + 99) / 100);
    return values[std::min(rank - 1, values.size() - 1)];
}

struct WaitMetrics {
    std::vector<double> overshootMicroseconds;
    double wallMilliseconds = 0;
    double threadCpuMilliseconds = 0;
};

uint64_t threadCpu100ns();

struct TimerResolutionScope {
    using Period = UINT(WINAPI*)(UINT);
    HMODULE library = nullptr;
    Period end = nullptr;
    bool active = false;

    TimerResolutionScope() {
        library = LoadLibraryExW(L"winmm.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        UINT status = library ? ERROR_PROC_NOT_FOUND : GetLastError();
        if (library) {
            const auto begin = reinterpret_cast<Period>(GetProcAddress(library, "timeBeginPeriod"));
            end = reinterpret_cast<Period>(GetProcAddress(library, "timeEndPeriod"));
            if (begin && end) { status = begin(1); active = status == 0; }
        }
        std::printf("[FRAME TIMER RESOLUTION] requested_ms=1 active=%s status=%u\n",
            active ? "true" : "false", status);
        if (!active) { if (library) FreeLibrary(library); library = nullptr; end = nullptr; }
    }

    ~TimerResolutionScope() {
        if (active) { end(1); active = false; }
        if (library) { FreeLibrary(library); library = nullptr; }
        end = nullptr;
    }
};

template<class Wait>
WaitMetrics measureWaits(Wait wait) {
    constexpr uint32_t samples = 300;
    WaitMetrics result;
    result.overshootMicroseconds.reserve(samples);
    const uint64_t cpuBefore = threadCpu100ns();
    const auto wallStart = std::chrono::steady_clock::now();
    for (uint32_t sample = 0; sample < samples; ++sample) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2 + sample % 3);
        wait(deadline);
        const double overshoot = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - deadline).count();
        result.overshootMicroseconds.push_back(std::max(0.0, overshoot));
    }
    const auto wallFinish = std::chrono::steady_clock::now();
    const uint64_t cpuAfter = threadCpu100ns();
    result.wallMilliseconds = std::chrono::duration<double, std::milli>(wallFinish - wallStart).count();
    result.threadCpuMilliseconds = double(cpuAfter - cpuBefore) / 10000.0;
    return result;
}

void printWaitMetrics(const char* method, const WaitMetrics& metrics) {
    std::printf("[FRAME WAIT BENCH] method=%s samples=%zu period_ms=2..4 overshoot_p50_us=%.2f overshoot_p95_us=%.2f overshoot_p99_us=%.2f overshoot_max_us=%.2f wall_ms=%.2f thread_cpu_ms=%.2f\n",
        method, metrics.overshootMicroseconds.size(),
        percentileValue(metrics.overshootMicroseconds, 50),
        percentileValue(metrics.overshootMicroseconds, 95),
        percentileValue(metrics.overshootMicroseconds, 99),
        *std::max_element(metrics.overshootMicroseconds.begin(), metrics.overshootMicroseconds.end()),
        metrics.wallMilliseconds, metrics.threadCpuMilliseconds);
}

void runWaiterBenchmark() {
    const auto defaultSleep = measureWaits([](FramePacer::Time deadline) {
        std::this_thread::sleep_until(deadline);
    });
    FrameWaiter waiter;
    const auto perRendererTimer = measureWaits([&](FramePacer::Time deadline) {
        waiter.waitUntil(deadline);
    });
    printWaitMetrics("std::this_thread::sleep_until", defaultSleep);
    printWaitMetrics("FrameWaiter", perRendererTimer);

    TimerResolutionScope timerResolution;
    const auto requestedResolutionSleep = measureWaits([](FramePacer::Time deadline) {
        std::this_thread::sleep_until(deadline);
    });
    FrameWaiter requestedResolutionWaiter;
    const auto requestedResolutionFrameWaiter = measureWaits([&](FramePacer::Time deadline) {
        requestedResolutionWaiter.waitUntil(deadline);
    });
    printWaitMetrics("std::this_thread::sleep_until + 1ms request", requestedResolutionSleep);
    printWaitMetrics("FrameWaiter + 1ms request", requestedResolutionFrameWaiter);
}

uint64_t fileTimeValue(FILETIME value) {
    ULARGE_INTEGER converted{};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

uint64_t threadCpu100ns() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        throw std::runtime_error("Cannot read benchmark thread CPU time");
    return fileTimeValue(kernel) + fileTimeValue(user);
}

void runUploadBenchmark(uint32_t sampleCount) {
    FixtureWindow window;
    D3D9Backend backend;
    constexpr uint32_t width = 1280, height = 720;
    backend.initialize(window.window, width, height);

    const uint32_t probeTexture = backend.createTexture(1, 1, 1, D3DFMT_A8R8G8B8, false, false);
    backend.bindTexture(15, probeTexture);
    ComPtr<IDirect3DBaseTexture9> probe;
    backend.getSampledTexture(15, &probe);
    ComPtr<IDirect3DDevice9> device;
    check(probe->GetDevice(&device), "Get benchmark D3D9Ex device");

    constexpr uint32_t vertexCount = 16384; // 256 KiB per complete vertex upload.
    constexpr uint32_t indexCount = 32768;  // 64 KiB per complete index upload.
    std::vector<Vertex> vertices(vertexCount, Vertex{2.0f, 2.0f, 0.5f, 0xff000000u});
    vertices[0] = {-1.0f, -1.0f, 0.5f, 0xff40c0ffu};
    vertices[1] = {3.0f, -1.0f, 0.5f, 0xff40c0ffu};
    vertices[2] = {-1.0f, 3.0f, 0.5f, 0xff40c0ffu};
    std::vector<uint16_t> indexData(indexCount, 0);
    indexData[0] = 0; indexData[1] = 1; indexData[2] = 2;
    const uint32_t vertexBuffer = backend.createVertexBuffer(
        {reinterpret_cast<const uint8_t*>(vertices.data()), vertices.size() * sizeof(Vertex)}, sizeof(Vertex));
    const uint32_t indexBuffer = backend.createIndexBuffer(indexData);
    const D3DVERTEXELEMENT9 elements[]{
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END(),
    };
    const uint32_t declaration = backend.createDeclaration(elements);
    const auto [vertexShader, pixelShader] = createFixtureShaders(backend, "native_stream_benchmark");
    backend.bindVertexBuffer(vertexBuffer);
    backend.bindIndexBuffer(indexBuffer);
    backend.bindDeclaration(declaration);
    backend.bindShader(ShaderStage::Vertex, vertexShader);
    backend.bindShader(ShaderStage::Pixel, pixelShader);
    backend.setViewport({0, 0, width, height, 0, 1});
    backend.setRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    backend.setRenderState(D3DRS_ZWRITEENABLE, FALSE);
    backend.setRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    backend.setRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    backend.beginFrame();
    backend.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff102030u, 1.0f, 0);
    for (uint32_t iteration = 0; iteration < 8; ++iteration) {
        vertices.back().color = (iteration & 1) ? 0xff112233u : 0xff445566u;
        indexData[1] = (iteration & 1) ? 1 : 2;
        indexData[2] = (iteration & 1) ? 2 : 1;
        backend.updateVertexBuffer(vertexBuffer,
            {reinterpret_cast<const uint8_t*>(vertices.data()), vertices.size() * sizeof(Vertex)});
        backend.updateIndexBuffer(indexBuffer, indexData);
        backend.drawIndexedTriangles(0, vertexCount, 0, 1);
    }
    constexpr uint32_t queuedDraws = 96;
    for (uint32_t draw = 0; draw < queuedDraws; ++draw)
        backend.drawIndexedTriangles(0, vertexCount, 0, 1);

    std::vector<double> vertexMilliseconds, indexMilliseconds;
    vertexMilliseconds.reserve(sampleCount);
    indexMilliseconds.reserve(sampleCount);
    const auto cpuBefore = threadCpu100ns();
    const auto wallStart = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0; iteration < sampleCount; ++iteration) {
        vertices.back().color = (iteration & 1) ? 0xff112233u : 0xff445566u;
        indexData[1] = (iteration & 1) ? 2 : 1;
        indexData[2] = (iteration & 1) ? 1 : 2;
        auto start = std::chrono::steady_clock::now();
        backend.updateVertexBuffer(vertexBuffer,
            {reinterpret_cast<const uint8_t*>(vertices.data()), vertices.size() * sizeof(Vertex)});
        auto split = std::chrono::steady_clock::now();
        backend.updateIndexBuffer(indexBuffer, indexData);
        auto finish = std::chrono::steady_clock::now();
        vertexMilliseconds.push_back(std::chrono::duration<double, std::milli>(split - start).count());
        indexMilliseconds.push_back(std::chrono::duration<double, std::milli>(finish - split).count());
        backend.drawIndexedTriangles(0, vertexCount, 0, 1);
    }
    const auto wallFinish = std::chrono::steady_clock::now();
    const auto cpuAfter = threadCpu100ns();
    backend.endFrame();

    ComPtr<IDirect3DQuery9> completion;
    check(device->CreateQuery(D3DQUERYTYPE_EVENT, &completion), "Create benchmark GPU completion query");
    check(completion->Issue(D3DISSUE_END), "Issue benchmark GPU completion query");
    const auto drainStart = std::chrono::steady_clock::now();
    HRESULT queryResult = S_FALSE;
    const auto queryDeadline = drainStart + std::chrono::seconds(10);
    while (queryResult == S_FALSE) {
        queryResult = completion->GetData(nullptr, 0, D3DGETDATA_FLUSH);
        if (queryResult == S_FALSE) {
            if (std::chrono::steady_clock::now() >= queryDeadline)
                throw std::runtime_error("Timed out waiting for the benchmark GPU completion query");
            SwitchToThread();
        }
    }
    check(queryResult, "Wait for benchmark GPU workload completion");
    const auto drainFinish = std::chrono::steady_clock::now();

    const BoundStream currentStream = stream(device.Get(), 0);
    D3DVERTEXBUFFER_DESC currentVertex{};
    D3DINDEXBUFFER_DESC currentIndex{};
    check(currentStream.buffer->GetDesc(&currentVertex), "Describe benchmark vertex resource");
    ComPtr<IDirect3DIndexBuffer9> currentIndices = indices(device.Get());
    check(currentIndices->GetDesc(&currentIndex), "Describe benchmark index resource");
    const double vertexMean = std::accumulate(vertexMilliseconds.begin(), vertexMilliseconds.end(), 0.0) / sampleCount;
    const double indexMean = std::accumulate(indexMilliseconds.begin(), indexMilliseconds.end(), 0.0) / sampleCount;
    std::printf("[BUFFER UPLOAD BENCH] samples=%u vertex_bytes=%u index_bytes=%u queued_draws=%u vertex_resource=%s index_resource=%s vertex_mean_ms=%.4f vertex_p50_ms=%.4f vertex_p95_ms=%.4f vertex_p99_ms=%.4f vertex_max_ms=%.4f index_mean_ms=%.4f index_p50_ms=%.4f index_p95_ms=%.4f index_p99_ms=%.4f index_max_ms=%.4f sample_wall_ms=%.3f sample_thread_cpu_ms=%.3f gpu_drain_ms=%.3f\n",
        sampleCount, vertexCount * uint32_t(sizeof(Vertex)), indexCount * uint32_t(sizeof(uint16_t)), queuedDraws,
        (currentVertex.Usage & D3DUSAGE_DYNAMIC) ? "dynamic" : "static",
        (currentIndex.Usage & D3DUSAGE_DYNAMIC) ? "dynamic" : "static",
        vertexMean, percentileValue(vertexMilliseconds, 50), percentileValue(vertexMilliseconds, 95),
        percentileValue(vertexMilliseconds, 99), *std::max_element(vertexMilliseconds.begin(), vertexMilliseconds.end()),
        indexMean, percentileValue(indexMilliseconds, 50), percentileValue(indexMilliseconds, 95),
        percentileValue(indexMilliseconds, 99), *std::max_element(indexMilliseconds.begin(), indexMilliseconds.end()),
        std::chrono::duration<double, std::milli>(wallFinish - wallStart).count(),
        double(cpuAfter - cpuBefore) / 10000.0,
        std::chrono::duration<double, std::milli>(drainFinish - drainStart).count());
}

void verifyQueuedMutation() {
    FixtureWindow window;
    D3D9Backend backend;
    backend.initialize(window.window, 64, 64);

    // The prompt texture exposes the backend's real device to this hidden test.
    const uint32_t probeTexture = backend.createTexture(1, 1, 1, D3DFMT_A8R8G8B8, false, false);
    backend.bindTexture(15, probeTexture);
    ComPtr<IDirect3DBaseTexture9> probe;
    backend.getSampledTexture(15, &probe);
    require(probe != nullptr, "Backend did not expose its native fixture texture");
    ComPtr<IDirect3DDevice9> device;
    check(probe->GetDevice(&device), "Get streaming-buffer fixture device");

    const uint32_t green = 0xff00ff00u, red = 0xffff0000u, black = 0xff000000u;
    const std::array<Vertex, 6> originalVertices{{
        {-1.0f, -1.0f, 0.5f, green}, {0.0f, -1.0f, 0.5f, green}, {-0.5f, 1.0f, 0.5f, green},
        {2.0f, 2.0f, 0.5f, black}, {2.0f, 2.0f, 0.5f, black}, {2.0f, 2.0f, 0.5f, black},
    }};
    const std::array<Vertex, 6> changedVertices{{
        {2.0f, 2.0f, 0.5f, black}, {2.0f, 2.0f, 0.5f, black}, {2.0f, 2.0f, 0.5f, black},
        {0.0f, -1.0f, 0.5f, red}, {1.0f, -1.0f, 0.5f, red}, {0.5f, 1.0f, 0.5f, red},
    }};
    const std::array<Vertex, 6> repeatedVertices{{
        {-0.5f, -1.0f, 0.5f, 0xff0000ffu}, {0.5f, -1.0f, 0.5f, 0xff0000ffu}, {0.0f, 1.0f, 0.5f, 0xff0000ffu},
        {2.0f, 2.0f, 0.5f, black}, {2.0f, 2.0f, 0.5f, black}, {2.0f, 2.0f, 0.5f, black},
    }};
    const std::array<uint16_t, 3> firstIndices{0, 1, 2};
    const std::array<uint16_t, 3> secondIndices{3, 4, 5};
    const std::array<uint16_t, 3> thirdIndices{0, 2, 1};
    const uint32_t vertexBuffer = backend.createVertexBuffer(
        {reinterpret_cast<const uint8_t*>(originalVertices.data()), sizeof(originalVertices)}, sizeof(Vertex));
    const uint32_t indexBuffer = backend.createIndexBuffer(firstIndices);
    const D3DVERTEXELEMENT9 elements[]{
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END(),
    };
    const uint32_t declaration = backend.createDeclaration(elements);
    const auto [vertexShader, pixelShader] = createFixtureShaders(backend, "native_streaming_fixture");

    backend.bindVertexBuffer(vertexBuffer);
    backend.bindIndexBuffer(indexBuffer);
    backend.bindDeclaration(declaration);
    backend.bindShader(ShaderStage::Vertex, vertexShader);
    backend.bindShader(ShaderStage::Pixel, pixelShader);
    backend.setViewport({0, 0, 64, 64, 0, 1});
    backend.setRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    backend.setRenderState(D3DRS_ZWRITEENABLE, FALSE);
    backend.setRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    backend.setRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    const BoundStream oldStream0 = stream(device.Get(), 0);
    require(oldStream0.buffer != nullptr && oldStream0.offset == 0 && oldStream0.stride == sizeof(Vertex),
        "Initial static vertex stream binding is incorrect");
    D3DVERTEXBUFFER_DESC oldVertexDescription{};
    check(oldStream0.buffer->GetDesc(&oldVertexDescription), "Describe initial static vertex buffer");
    require((oldVertexDescription.Usage & D3DUSAGE_WRITEONLY) != 0 &&
        (oldVertexDescription.Usage & D3DUSAGE_DYNAMIC) == 0 && oldVertexDescription.Pool == D3DPOOL_DEFAULT,
        "An unchanged title vertex buffer should start static and write-only");

    // A second stream aliases the same resource at a nonzero offset. Promotion
    // must preserve its identity and layout while rebinding every alias.
    check(device->SetStreamSource(1, oldStream0.buffer.Get(), sizeof(Vertex), sizeof(Vertex)),
        "Create nonzero-offset alias for the static vertex stream");
    const BoundStream oldStream1 = stream(device.Get(), 1);
    require(oldStream1.buffer.Get() == oldStream0.buffer.Get() && oldStream1.offset == sizeof(Vertex) &&
        oldStream1.stride == sizeof(Vertex), "Nonzero-offset vertex stream alias was not installed");

    ComPtr<IDirect3DIndexBuffer9> oldIndices = indices(device.Get());
    require(oldIndices != nullptr, "Initial static index buffer binding is empty");
    D3DINDEXBUFFER_DESC oldIndexDescription{};
    check(oldIndices->GetDesc(&oldIndexDescription), "Describe initial static index buffer");
    require((oldIndexDescription.Usage & D3DUSAGE_WRITEONLY) != 0 &&
        (oldIndexDescription.Usage & D3DUSAGE_DYNAMIC) == 0 && oldIndexDescription.Pool == D3DPOOL_DEFAULT,
        "An unchanged title index buffer should start static and write-only");

    backend.updateVertexBuffer(vertexBuffer,
        {reinterpret_cast<const uint8_t*>(originalVertices.data()), sizeof(originalVertices)});
    backend.updateIndexBuffer(indexBuffer, firstIndices);
    require(stream(device.Get(), 0).buffer.Get() == oldStream0.buffer.Get() &&
        stream(device.Get(), 1).buffer.Get() == oldStream1.buffer.Get() &&
        indices(device.Get()).Get() == oldIndices.Get(),
        "Byte-identical static dirty notifications changed active buffer identities");
    D3DVERTEXBUFFER_DESC unchangedVertexDescription{};
    D3DINDEXBUFFER_DESC unchangedIndexDescription{};
    check(oldStream0.buffer->GetDesc(&unchangedVertexDescription), "Describe unchanged static vertex buffer");
    check(oldIndices->GetDesc(&unchangedIndexDescription), "Describe unchanged static index buffer");
    require((unchangedVertexDescription.Usage & D3DUSAGE_DYNAMIC) == 0 &&
        (unchangedIndexDescription.Usage & D3DUSAGE_DYNAMIC) == 0,
        "Byte-identical static dirty notifications promoted an unchanged buffer");

    std::vector<uint32_t> framePixels;
    ComPtr<IDirect3DSurface9> readback;
    check(device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
        &readback, nullptr), "Create hidden streaming-buffer pixel readback");
    backend.setPresentationUi([&](IDirect3DDevice9Ex* callbackDevice, IDirect3DSurface9* output) {
        check(callbackDevice->GetRenderTargetData(output, readback.Get()), "Read queued streaming-buffer frame");
        D3DLOCKED_RECT locked{};
        check(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "Lock queued streaming-buffer pixels");
        if (!locked.pBits || locked.Pitch < 64 * 4) {
            readback->UnlockRect();
            throw std::runtime_error("Invalid streaming-buffer fixture readback pitch");
        }
        framePixels.resize(64 * 64);
        for (uint32_t row = 0; row < 64; ++row)
            std::memcpy(framePixels.data() + size_t(row) * 64,
                static_cast<const uint8_t*>(locked.pBits) + size_t(row) * locked.Pitch, 64 * 4);
        check(readback->UnlockRect(), "Unlock queued streaming-buffer pixels");
    });

    backend.beginFrame();
    backend.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff102030u, 1.0f, 0);
    backend.drawIndexedTriangles(0, 6, 0, 1);

    // Both uploads occur after the first DrawIndexedPrimitive has been queued,
    // and before the next draw. Draw B can only appear if both bytes and indices
    // switch to the new dynamic allocations while active bindings stay intact.
    backend.updateVertexBuffer(vertexBuffer,
        {reinterpret_cast<const uint8_t*>(changedVertices.data()), sizeof(changedVertices)});
    const BoundStream changedStream0 = stream(device.Get(), 0);
    const BoundStream changedStream1 = stream(device.Get(), 1);
    require(changedStream0.buffer && changedStream0.buffer.Get() != oldStream0.buffer.Get() &&
        changedStream0.offset == oldStream0.offset && changedStream0.stride == oldStream0.stride,
        "Vertex promotion did not replace stream 0 while preserving its offset and stride");
    require(changedStream1.buffer.Get() == changedStream0.buffer.Get() &&
        changedStream1.offset == oldStream1.offset && changedStream1.stride == oldStream1.stride,
        "Vertex promotion lost the aliased stream or its nonzero offset/stride");
    D3DVERTEXBUFFER_DESC changedVertexDescription{};
    check(changedStream0.buffer->GetDesc(&changedVertexDescription), "Describe promoted dynamic vertex buffer");
    require((changedVertexDescription.Usage & (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY)) ==
        (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY) && changedVertexDescription.Pool == D3DPOOL_DEFAULT,
        "First changed vertex upload did not promote to a default-pool dynamic write-only buffer");

    backend.updateIndexBuffer(indexBuffer, secondIndices);
    ComPtr<IDirect3DIndexBuffer9> changedIndices = indices(device.Get());
    require(changedIndices && changedIndices.Get() != oldIndices.Get(),
        "First changed index upload did not preserve the active binding through promotion");
    D3DINDEXBUFFER_DESC changedIndexDescription{};
    check(changedIndices->GetDesc(&changedIndexDescription), "Describe promoted dynamic index buffer");
    require((changedIndexDescription.Usage & (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY)) ==
        (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY) && changedIndexDescription.Pool == D3DPOOL_DEFAULT &&
        changedIndexDescription.Format == D3DFMT_INDEX16,
        "First changed index upload did not promote to a default-pool dynamic INDEX16 buffer");

    // Repeating byte-identical data must keep the promoted resource identities.
    backend.updateVertexBuffer(vertexBuffer,
        {reinterpret_cast<const uint8_t*>(changedVertices.data()), sizeof(changedVertices)});
    backend.updateIndexBuffer(indexBuffer, secondIndices);
    require(stream(device.Get(), 0).buffer.Get() == changedStream0.buffer.Get() &&
        stream(device.Get(), 1).buffer.Get() == changedStream1.buffer.Get() &&
        indices(device.Get()).Get() == changedIndices.Get(),
        "Identical reuploads changed the active vertex or index resources");

    backend.drawIndexedTriangles(0, 6, 0, 1);
    backend.updateVertexBuffer(vertexBuffer,
        {reinterpret_cast<const uint8_t*>(repeatedVertices.data()), sizeof(repeatedVertices)});
    backend.updateIndexBuffer(indexBuffer, thirdIndices);
    require(stream(device.Get(), 0).buffer.Get() == changedStream0.buffer.Get() &&
        stream(device.Get(), 1).buffer.Get() == changedStream1.buffer.Get() &&
        indices(device.Get()).Get() == changedIndices.Get(),
        "Repeated dynamic DISCARD upload replaced or lost a bound stream alias or index binding");
    backend.drawIndexedTriangles(0, 6, 0, 1);
    const BoundStream afterSecondDraw0 = stream(device.Get(), 0);
    const BoundStream afterSecondDraw1 = stream(device.Get(), 1);
    ComPtr<IDirect3DIndexBuffer9> afterSecondDrawIndices = indices(device.Get());
    require(afterSecondDraw0.buffer.Get() == changedStream0.buffer.Get() &&
        afterSecondDraw0.offset == changedStream0.offset && afterSecondDraw0.stride == changedStream0.stride &&
        afterSecondDraw1.buffer.Get() == changedStream1.buffer.Get() &&
        afterSecondDraw1.offset == changedStream1.offset && afterSecondDraw1.stride == changedStream1.stride &&
        afterSecondDrawIndices.Get() == changedIndices.Get(),
        "A later draw did not retain promoted stream aliases and index binding");
    backend.endFrame();
    backend.present();

    require(framePixels.size() == 64 * 64, "Hidden GPU readback did not capture the queued frame");
    require(pixel(framePixels, 16, 32) == green,
        "Queued draw A did not retain its original static vertex and index bytes after promotion");
    require(pixel(framePixels, 48, 32) == red,
        "Queued draw B did not consume both changed vertex and index bytes from promoted buffers");
    require(pixel(framePixels, 32, 32) == 0xff0000ffu,
        "Queued draw C did not retain its repeated dynamic DISCARD vertex/index snapshot");
    require(pixel(framePixels, 63, 10) == 0xff102030u,
        "Streaming-buffer fixture changed pixels outside its three queued triangles");
    require(backend.counters().draws == 3 && backend.counters().triangles == 3 && backend.counters().presents == 1,
        "Streaming-buffer fixture did not submit exactly three real draws and one hidden game frame");
    backend.beginFrame();
    backend.drawIndexedTriangles(0,6,0,1); // Reuse the successful current-content proof.
    const std::array<uint16_t,3> invalidIndices{0,6,1};
    backend.updateIndexBuffer(indexBuffer,invalidIndices);
    bool rejected = false;
    try { backend.drawIndexedTriangles(0,6,0,1); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected && backend.counters().draws == 4,
        "A changed native index buffer reused a stale proof or submitted an invalid GPU draw");
    backend.updateIndexBuffer(indexBuffer,thirdIndices);
    backend.drawIndexedTriangles(0,6,0,1);
    backend.endFrame();
    require(backend.counters().draws == 5 && backend.counters().presents == 1,
        "Corrected native index data could not be revalidated after a rejected cached range");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && (std::strcmp(argv[1], "--timer-benchmark") == 0 ||
            std::strcmp(argv[1], "--waiter-benchmark") == 0)) {
            runWaiterBenchmark();
            return 0;
        }
        if (argc == 3 && std::strcmp(argv[1], "--benchmark") == 0) {
            const unsigned long parsed = std::stoul(argv[2]);
            if (!parsed || parsed > 10000) throw std::runtime_error("Benchmark sample count must be 1 through 10000");
            runUploadBenchmark(static_cast<uint32_t>(parsed));
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Usage: NativeStreamingBufferTests [--benchmark sample-count|--waiter-benchmark]");
        verifyQueuedMutation();
        std::printf("Native streaming-buffer hardware contract: %u checks passed; hidden D3D9Ex queued A/B/C draw, static-to-dynamic promotion, stream alias offsets and readback only\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native streaming-buffer hardware contract failed after %u checks: %s\n", checks, error.what());
        return 1;
    }
}
