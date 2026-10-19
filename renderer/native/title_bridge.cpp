#include "title_bridge.h"
#include "title_window.h"
#include "video_settings_ui.h"
#include "title_vertex.h"
#include "title_texture.h"
#include "indexed_draw.h"
#include "inline_buffer.h"
#include "title_constants.h"
#include "shadow_target.h"
#include "guest_span.h"
#include "title_renderer_span.h"
#include "source_shader_capture.h"
#include "title_input_prompts.h"
#include "runtime/native/runtime.h"
#include "runtime/native/input_services.h"
#include "runtime/native/mouse_services.h"
#include <future>
#include <algorithm>
#include <array>
#include <cstring>
#include <bit>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <chrono>
#include <source_location>

namespace Doritos::Graphics {
namespace {
using Doritos::Native::memory;
constexpr uint32_t kTokenMagic = 0x444e4731; // DNG1: native adapter token, never an Xbox GPU object.
enum class Kind : uint32_t { Device = 1, VertexBuffer, IndexBuffer, Declaration, VertexShader, PixelShader, Texture, CubeTexture, Surface };
struct Resource {
    Kind kind;
    uint32_t native = 0;
    uint32_t data = 0;
    uint32_t bytes = 0;
    uint32_t stride = 0;
    bool dirty = true;
    std::vector<D3DVERTEXELEMENT9> declaration;
    uint32_t xboxFormat = 0, parent = 0, face = 0, level = 0;
    TitleSurfaceParent parentKind = TitleSurfaceParent::None;
    TitleBufferStorage storage;
};
struct State {
    std::recursive_mutex mutex;
    std::vector<uint8_t> windowsCache, xenonCache, nativeExtension, xenonSourceExtension;
    std::unique_ptr<TitleWindow> window;
    HANDLE closeRequested = nullptr;
    std::unique_ptr<D3D9Backend> backend;
    std::map<uint32_t, Resource> resources;
    uint32_t device = 0, declaration = 0, vertexBuffer = 0, indexBuffer = 0;
    uint32_t sceneRenderer = 0;
    PPCContext* presentationContext = nullptr;
    uint32_t immediateDiagnostics = 0;
    uint32_t optionalShaderDiagnostics = 0;
    uint32_t optionalDeclarationDiagnostics = 0;
    uint32_t offscreenFrameDiagnostics = 0;
    uint32_t boneDrawDiagnostics = 0;
    uint32_t boneExporterDiagnostics = 0;
    uint32_t cjDrawDiagnostics = 0;
    bool reportPhases = false;
    bool traceAvatar = false;
} state;
using Doritos::Native::MemoryQueryCounters;
struct MemoryQueryFrame { MemoryQueryCounters whole, bridge, critical; uint64_t criticalCalls = 0; };
struct MemoryQuerySite {
    const char* function = nullptr;
    uint64_t calls = 0;
    uint64_t hookWallNanoseconds = 0, wallMeasuredCalls = 0;
    MemoryQueryCounters queries;
};
struct MemoryQueryReport {
    DWORD thread = 0;
    RenderCounters previous;
    MemoryQueryCounters previousQueries, bridgeCycle;
    Doritos::Native::CriticalMemoryQueryCounters previousCritical;
    bool previousDrawn = false;
    std::array<MemoryQueryFrame,300> recent{};
    uint32_t next = 0, count = 0;
    std::map<uint_least32_t,MemoryQuerySite> sites;
    uint32_t siteCycles = 0;
} memoryQueryReport;
thread_local uint32_t bridgePhaseDepth = 0;

std::span<const uint8_t> guestBytes(uint32_t address, uint32_t count) {
    if (!memory || !memory->valid(address, count)) throw std::runtime_error("Native graphics guest byte range is inaccessible");
    return {memory->base() + address, count};
}
std::span<uint8_t> guestMutableBytes(uint32_t address, uint32_t count) {
    guestBytes(address,count);
    return {memory->base() + address,count};
}
D3D9Backend& backend() {
    if (!state.backend) {
        if (state.windowsCache.empty() || state.xenonCache.empty())
            throw std::runtime_error("Native title graphics requires original verified Windows and Xenon shader caches");
        state.window = std::make_unique<TitleWindow>(state.closeRequested, [](HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
            if(handleNativeVideoSettingsMessage(window,message,wparam,lparam)) return true;
            return Doritos::Native::handleNativeMouseMessage(window,message,wparam,lparam);
        });
        registerNativeVideoSettingsOwner(state.window->handle(),Doritos::Native::setNativeInputSuspended);
        auto renderer = std::make_unique<D3D9Backend>();
        renderer->loadCaches(state.windowsCache, state.xenonCache, state.nativeExtension,state.xenonSourceExtension);
        renderer->initialize(state.window->handle(), 2560, 1440);
        const HWND owner=state.window->handle();
        renderer->setPresentationUi([owner](IDirect3DDevice9Ex* device,IDirect3DSurface9* output) {
            if(!nativeVideoSettingsOpen()) return;
            if(GetForegroundWindow()==owner) {
                // Read physical controllers directly: guest input is suspended
                // while the in-game settings menu owns navigation.
                uint32_t held=0;
                for(DWORD user=0;user<XUSER_MAX_COUNT;++user) {
                    XINPUT_STATE input{};
                    if(XInputGetState(user,&input)!=ERROR_SUCCESS) continue;
                    const auto buttons=input.Gamepad.wButtons;
                    const auto map=[&](WORD physical,uint32_t action) {if(buttons&physical) held|=action;};
                    map(XINPUT_GAMEPAD_DPAD_UP,VideoUiUp);map(XINPUT_GAMEPAD_DPAD_DOWN,VideoUiDown);
                    map(XINPUT_GAMEPAD_DPAD_LEFT,VideoUiLeft);map(XINPUT_GAMEPAD_DPAD_RIGHT,VideoUiRight);
                    map(XINPUT_GAMEPAD_A,VideoUiSelect);map(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_BACK,VideoUiBack);
                    map(XINPUT_GAMEPAD_LEFT_SHOULDER,VideoUiPreviousPage);
                    map(XINPUT_GAMEPAD_RIGHT_SHOULDER,VideoUiNextPage);
                }
                updateNativeVideoSettingsController(held);
            }
            renderNativeVideoSettings(device,output);
        });
        state.backend = std::move(renderer);
    }
    return *state.backend;
}
void addMemoryQueries(MemoryQueryCounters& destination, const MemoryQueryCounters& source) {
    destination.validations += source.validations;
    destination.queries += source.queries;
    destination.queryNanoseconds += source.queryNanoseconds;
    destination.validationNanoseconds += source.validationNanoseconds;
}
MemoryQueryCounters memoryQueryDifference(const MemoryQueryCounters& after, const MemoryQueryCounters& before) {
    return {after.validations - before.validations,after.queries - before.queries,
        after.queryNanoseconds - before.queryNanoseconds,after.validationNanoseconds - before.validationNanoseconds};
}
void recordMemoryQuerySite(const std::source_location& location, const MemoryQueryCounters& queries,
    uint64_t hookWallNanoseconds, bool hookWallSelected) {
    auto& sites = memoryQueryReport.sites;
    auto found = sites.find(location.line());
    if (found == sites.end()) {
        if (sites.size() >= 128) return;
        found = sites.emplace(location.line(),MemoryQuerySite{location.function_name()}).first;
    }
    ++found->second.calls;
    if (hookWallSelected) {
        ++found->second.wallMeasuredCalls;
        found->second.hookWallNanoseconds += hookWallNanoseconds;
    }
    addMemoryQueries(found->second.queries,queries);
}
void resetMemoryQuerySites() {
    memoryQueryReport.siteCycles = 0;
    for (auto& [line,site] : memoryQueryReport.sites) {
        site.calls = 0;
        site.hookWallNanoseconds = site.wallMeasuredCalls = 0;
        site.queries = {};
    }
}
void finishMemoryQueryFrame(const MemoryQueryCounters& queries) {
    auto& report = memoryQueryReport;
    const auto current = backend().counters();
    const auto critical = Doritos::Native::criticalMemoryQueryCounters();
    if (!report.thread) report.thread = GetCurrentThreadId();
    const bool drawn = current.presents == report.previous.presents + 1 &&
        current.clearPresents == report.previous.clearPresents &&
        current.initializationPresents == report.previous.initializationPresents;
    if (drawn && report.previousDrawn) {
        report.recent[report.next] = {memoryQueryDifference(queries,report.previousQueries),report.bridgeCycle,
            memoryQueryDifference(critical.queries,report.previousCritical.queries),critical.calls - report.previousCritical.calls};
        report.next = (report.next + 1) % report.recent.size();
        report.count = std::min<uint32_t>(report.count + 1,static_cast<uint32_t>(report.recent.size()));
        ++report.siteCycles;
    } else {
        report.next = report.count = 0;
        resetMemoryQuerySites();
    }
    report.previous = current;
    report.previousQueries = queries;
    report.previousCritical = critical;
    report.previousDrawn = drawn;
    report.bridgeCycle = {};
    if (!drawn || current.presents % 300 || !report.count) return;
    MemoryQueryFrame summary{};
    for (uint32_t index = 0; index < report.count; ++index) {
        addMemoryQueries(summary.whole,report.recent[index].whole);
        addMemoryQueries(summary.bridge,report.recent[index].bridge);
        addMemoryQueries(summary.critical,report.recent[index].critical);
        summary.criticalCalls += report.recent[index].criticalCalls;
    }
    const double divisor = double(report.count);
    std::fprintf(stderr,"[GUEST SPAN QUERIES] presents=%llu thread=%lu consecutive_cycles=%u complete=%u whole_validations_per_cycle=%.3f whole_queries_per_cycle=%.3f whole_query_ms=%.3f whole_validation_ms=%.3f bridge_validations_per_cycle=%.3f bridge_queries_per_cycle=%.3f bridge_query_ms=%.3f bridge_validation_ms=%.3f scope=current_present_thread_cpu_wall validation_includes_query=true not_additive_to_render_phases=true gpu_execution_measured=false\n",
        static_cast<unsigned long long>(current.presents),static_cast<unsigned long>(report.thread),report.count,
        static_cast<uint32_t>(report.count == report.recent.size()),summary.whole.validations / divisor,
        summary.whole.queries / divisor,summary.whole.queryNanoseconds / divisor / 1e6,
        summary.whole.validationNanoseconds / divisor / 1e6,summary.bridge.validations / divisor,
        summary.bridge.queries / divisor,summary.bridge.queryNanoseconds / divisor / 1e6,
        summary.bridge.validationNanoseconds / divisor / 1e6);
    std::fflush(stderr);
    std::fprintf(stderr,"[CRITICAL SPAN QUERIES] presents=%llu thread=%lu consecutive_cycles=%u calls_per_cycle=%.3f queries_per_cycle=%.3f query_ms_per_cycle=%.3f validation_ms_per_cycle=%.3f scope=current_present_thread_enter_try_leave subset_of_whole_queries=true lock_wait_measured=false\n",
        static_cast<unsigned long long>(current.presents),static_cast<unsigned long>(report.thread),report.count,
        summary.criticalCalls / divisor,summary.critical.queries / divisor,
        summary.critical.queryNanoseconds / divisor / 1e6,summary.critical.validationNanoseconds / divisor / 1e6);
    std::array<std::pair<uint_least32_t,MemoryQuerySite*>,128> sites{};
    size_t siteCount = 0;
    for (auto& [line,site] : report.sites)
        if (site.calls) sites[siteCount++] = {line,&site};
    std::sort(sites.begin(),sites.begin() + siteCount,[](const auto& left,const auto& right) {
        return left.second->queries.queryNanoseconds > right.second->queries.queryNanoseconds;
    });
    const double siteDivisor = report.siteCycles ? double(report.siteCycles) : 1;
    for (size_t index = 0; index < std::min<size_t>(8,siteCount); ++index) {
        const auto& [line,site] = sites[index];
        std::fprintf(stderr,"[GUEST SPAN QUERY SITE] presents=%llu line=%u hook=\"%s\" cycles_since_report=%u calls_per_cycle=%.3f queries_per_cycle=%.3f query_ms_per_cycle=%.3f hook_wall_ms_per_cycle=%.3f wall_measured_calls_per_cycle=%.3f hook_wall_timing_selected=%u scope=outermost_bridge_calls_cpu_wall query_subset_of_hook_wall_if_selected=true wall_overlaps_render_phases=true not_additive_to_render_phases=true bridge_mutex_wait_excluded=true\n",
            static_cast<unsigned long long>(current.presents),static_cast<unsigned>(line),site->function,
            report.siteCycles,site->calls / siteDivisor,site->queries.queries / siteDivisor,
            site->queries.queryNanoseconds / siteDivisor / 1e6,
            site->hookWallNanoseconds / siteDivisor / 1e6,site->wallMeasuredCalls / siteDivisor,
            static_cast<uint32_t>(state.reportPhases));
    }
    resetMemoryQuerySites();
    std::fflush(stderr);
}
void initializeResourceToken(uint32_t address, const Resource& resource) {
    const auto bytes = guestMutableBytes(address,16);
    titleGuestWriteWord(bytes,0,kTokenMagic);
    titleGuestWriteWord(bytes,4,static_cast<uint32_t>(resource.kind));
    titleGuestWriteWord(bytes,8,resource.native);
    titleGuestWriteWord(bytes,12,0);
}
uint32_t token(Resource resource) {
    if (!memory) throw std::runtime_error("Native graphics has no guest memory context");
    const uint32_t address = memory->allocate(16);
    initializeResourceToken(address,resource);
    state.resources.emplace(address, std::move(resource));
    return address;
}
Resource& resourceEntry(uint32_t address, Kind expected) {
    const auto found = state.resources.find(address);
    if (found == state.resources.end() || found->second.kind != expected)
        throw std::runtime_error("Invalid native title graphics adapter token/kind");
    return found->second;
}
Resource& resource(uint32_t address, Kind expected) {
    auto& entry = resourceEntry(address,expected);
    if (memory->read32(address) != kTokenMagic)
        throw std::runtime_error("Invalid native title graphics adapter token/kind");
    return entry;
}
void device(uint32_t address) { resource(address, Kind::Device); }
std::span<const uint8_t> deviceGuestBytes(uint32_t deviceAddress, uint32_t address, uint32_t bytes) {
    resourceEntry(deviceAddress,Kind::Device);
    if (!memory || !memory->validPair(deviceAddress,4,address,bytes))
        throw std::runtime_error("Native graphics guest byte range is inaccessible");
    if (titleGuestWord({memory->base() + deviceAddress,4},0) != kTokenMagic)
        throw std::runtime_error("Invalid native title graphics adapter token/kind");
    return {memory->base() + address,bytes};
}
Resource& deviceResource(uint32_t deviceAddress, uint32_t address, Kind expected) {
    auto& entry = resourceEntry(address,expected);
    const auto bytes = deviceGuestBytes(deviceAddress,address,4);
    if (titleGuestWord(bytes,0) != kTokenMagic)
        throw std::runtime_error("Invalid native title graphics adapter token/kind");
    return entry;
}
std::vector<float> originalFloatConstants(uint32_t deviceAddress, uint32_t address,
    uint32_t firstRegister, uint32_t registerCount) {
    std::chrono::steady_clock::time_point start;
    if (state.reportPhases) start = std::chrono::steady_clock::now();
    const auto bytes = deviceGuestBytes(deviceAddress,address,titleFloatConstantSpanBytes(address,firstRegister,registerCount));
    auto values = decodeTitleFloatConstants(bytes,registerCount);
    if (state.reportPhases) backend().recordConstantDecodePhaseTime(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));
    return values;
}
Resource& textureResource(uint32_t address) {
    const auto found = state.resources.find(address);
    if (found == state.resources.end() || (found->second.kind != Kind::Texture && found->second.kind != Kind::CubeTexture))
        throw std::runtime_error("Invalid native title texture adapter token");
    return resource(address, found->second.kind);
}
void writeDescription(uint32_t output, const Resource& entry, const D3DSURFACE_DESC& description, uint32_t type) {
    const auto bytes = guestMutableBytes(output,32);
    if (!entry.xboxFormat) throw std::runtime_error("Native title resource has no original format metadata");
    const uint32_t samples = description.MultiSampleType == D3DMULTISAMPLE_NONE ? 0 :
        description.MultiSampleType == D3DMULTISAMPLE_2_SAMPLES ? 1 : description.MultiSampleType == D3DMULTISAMPLE_4_SAMPLES ? 2 : UINT32_MAX;
    if (samples == UINT32_MAX) throw std::runtime_error("Unverified native title surface description sample type");
    const uint32_t fields[] = {entry.xboxFormat,type,description.Usage,0,samples,0,description.Width,description.Height};
    for (uint32_t index = 0; index < std::size(fields); ++index) titleGuestWriteWord(bytes,index * 4,fields[index]);
}
uint32_t createDDSResource(std::span<const uint8_t> source, bool cube, uint32_t requestedLevels) {
    const auto dds = parseTitleDDS(source);
    if (dds.faces != (cube ? 6u : 1u)) throw std::runtime_error("Original title DDS resource type does not match its header");
    uint32_t maximumLevels = 1;
    for (uint32_t extent = std::max(dds.width, dds.height); extent > 1; extent >>= 1) ++maximumLevels;
    const uint32_t levels = !requestedLevels || requestedLevels == UINT32_MAX ? maximumLevels : std::min(requestedLevels, maximumLevels);
    if (requestedLevels != UINT32_MAX && requestedLevels > 14)
        throw std::runtime_error("Original title DDS requested mip count exceeds native bounds");
    if (levels > dds.levels && dds.levels != 1)
        throw std::runtime_error("Unverified DDS generation from an authored nonzero source mip");
    const uint32_t handle = backend().createTexture(dds.width, dds.height, levels, dds.format, cube, false);
    try {
        for (const auto& image : dds.images) {
            if (image.level >= levels) continue;
            const uint32_t surface = backend().textureSurface(handle, image.face, image.level);
            try { backend().uploadSurface(surface, dds.format, image.rowPitch, source.subspan(image.offset, image.bytes)); }
            catch (...) { backend().release(surface); throw; }
            backend().release(surface);
        }
        if (levels > dds.levels) backend().generateMipmaps(handle);
    } catch (...) { backend().release(handle); throw; }
    Resource entry{cube ? Kind::CubeTexture : Kind::Texture, handle};
    entry.xboxFormat = dds.format == D3DFMT_DXT1 ? 0x1a200152 : 0x1a200154;
    return token(std::move(entry));
}
std::optional<ShaderSelection> originalShaderSelection(const PPCContext& ctx, ShaderStage stage,
    uint32_t codeAddress, uint32_t codeBytes) {
    // These are the four exact BL return addresses in the original source and
    // library loaders. Nonvolatile registers retain the title's CPU objects;
    // unrelated SDK callers must still use the bounded bytecode-only contract.
    const bool source = ctx.lr == 0x82387628 || ctx.lr == 0x82387638;
    const bool cached = ctx.lr == 0x82387d40 || ctx.lr == 0x82387d4c;
    if (!source && !cached) return std::nullopt;
    const bool vertexCall = ctx.lr == 0x82387638 || ctx.lr == 0x82387d4c;
    if (vertexCall != (stage == ShaderStage::Vertex))
        throw std::runtime_error("Original shader loader return address has the wrong native stage");
    const uint32_t master = source ? ctx.r16.u32 : ctx.r23.u32;
    const uint32_t variant = ctx.r30.u32;
    const auto masterBytes = guestBytes(master,124);
    const auto variantBytes = guestBytes(variant,source ? 2540 : 52);
    if ((titleGuestWord(masterBytes,120) != 0) != vertexCall)
        throw std::runtime_error("Original shader master stage disagrees with its creation call");
    if (source && titleGuestWord(variantBytes,2536) != master)
        throw std::runtime_error("Original source shader variant does not belong to its master");
    // The cached loader reads this exact allocation into the variant before
    // creation. Its +2536 field is not initialized by that path and is unused.
    if (cached && (titleGuestWord(variantBytes,48) != codeAddress || titleGuestWord(variantBytes,44) != codeBytes))
        throw std::runtime_error("Original cached shader variant bytecode allocation disagrees with its creation call");
    const uint32_t nameAddress = titleGuestWord(masterBytes,8);
    if (!nameAddress) return std::nullopt;
    ShaderSelection selection;
    for (uint32_t index = 0; index < 256; ++index) {
        if (nameAddress > UINT32_MAX - index)
            throw std::runtime_error("Original shader master name address wraps");
        const uint8_t value = guestBytes(nameAddress + index, 1)[0];
        if (!value) {
            if (selection.name.empty()) throw std::runtime_error("Original shader master name is empty");
            for (uint32_t word = 0; word < 3; ++word) selection.key[word] = titleGuestWord(variantBytes,8 + word * 4);
            return selection;
        }
        if (value < 33 || value > 126 || value == '\\' || value == ':')
            throw std::runtime_error("Original shader master name is not a bounded cache identifier");
        selection.name.push_back(static_cast<char>(value));
    }
    throw std::runtime_error("Original shader master name exceeds its cache bound");
}
void createShader(PPCContext& ctx, ShaderStage stage) {
    const uint32_t address = ctx.r3.u32;
    const auto headerBytes = guestBytes(address,12);
    const uint32_t header = titleGuestWord(headerBytes,4), instructions = titleGuestWord(headerBytes,8);
    if (header < 12 || header > 1024 * 1024 || instructions > 1024 * 1024 - header)
        throw std::runtime_error("Invalid original Xenon shader bytecode header");
    const auto code = guestBytes(address, header + instructions);
    const auto selection = originalShaderSelection(ctx, stage, address, header + instructions);
    if (selection && (ctx.lr == 0x82387628 || ctx.lr == 0x82387638) &&
        (selection->name == "multilayer/vs" || selection->name == "multilayer/ps" ||
         selection->name == "composite/vs" || selection->name == "composite/ps"))
        captureOriginalSourceShader(*selection,stage,ctx.r16.u32,ctx.r30.u32,guestBytes(ctx.r30.u32,2540),code);
    const auto* identity = selection ? &*selection : nullptr;
    if (backend().optionalAvatarShaderUnavailable(code, stage, identity)) {
        // Original LoadShaders82387AE0 handles genuine null creation by clearing
        // the variant parameter count and invoking its own reset callback.
        // No guest token or native shader object is created for unavailable SDK
        // avatar families; the original authored optional failure path remains.
        if (state.optionalShaderDiagnostics++ < 128) {
            if (selection) std::fprintf(stderr,
                "[NATIVE OPTIONAL SHADER] lr=%08X name=\"%s\" key=[%u,%u,%u] stage=%s creation=unavailable returned=null\n",
                static_cast<uint32_t>(ctx.lr),selection->name.c_str(),selection->key[0],selection->key[1],selection->key[2],
                stage == ShaderStage::Vertex ? "vertex" : "pixel");
            else std::fprintf(stderr,
                "[NATIVE OPTIONAL SHADER] lr=%08X identity=unavailable stage=%s creation=unavailable returned=null\n",
                static_cast<uint32_t>(ctx.lr),stage == ShaderStage::Vertex ? "vertex" : "pixel");
            std::fflush(stderr);
        }
        ctx.r3.u64 = 0;
        return;
    }
    const uint32_t handle = backend().createShaderForXenon(code, stage, identity);
    ctx.r3.u64 = token({stage == ShaderStage::Vertex ? Kind::VertexShader : Kind::PixelShader, handle});
}
void createBuffer(PPCContext& ctx, Kind kind) {
    const uint32_t count = ctx.r3.u32;
    if (!count || count > 128 * 1024 * 1024 ||
        (kind == Kind::IndexBuffer && (ctx.r5.u32 != 1 || count % 6)))
        throw std::runtime_error("Unsupported original title vertex/index buffer allocation contract");
    const uint32_t data = memory->allocate(count);
    ctx.r3.u64 = token({kind, 0, data, count});
}
void lockBuffer(PPCContext& ctx, Kind kind) {
    auto& entry = resource(ctx.r3.u32, kind);
    // Verified Xbox pointer-returning wrappers replace the incoming offset and
    // size registers before their shared helper and return the full backing
    // pointer. Preserve that observed behavior; only supported flags are zero.
    if (ctx.r6.u32) throw std::runtime_error("Native title buffer lock flags are not verified");
    if (!entry.data) throw std::runtime_error("Native title buffer backing has not been associated");
    guestBytes(entry.data, entry.bytes);
    entry.dirty = true;
    ctx.r3.u64 = entry.data;
}
TitleAvatarBufferBinding originalInlineBuffer(const PPCContext& ctx, uint32_t storage) {
    const uint32_t owner = ctx.r31.u32;
    guestBytes(owner, TitleAvatarBufferBinding::ownerBytes);
    if (memory->read32(owner) != 0x82161664 || memory->read32(owner + 80) != 0x82161804 || ctx.r29.u32 != storage)
        throw std::runtime_error("Original AvatarPose inline buffer owner/iterator does not match its constructor");
    const auto binding = titleAvatarBufferBinding(owner, storage, ctx.r30.u32, memory->read32(owner + 404));
    guestBytes(binding.lifetime.backingBase, TitleAvatarBufferBinding::backingBytes);
    guestBytes(storage, 32);
    return binding;
}
TitleShadowTarget originalShadowTarget(uint32_t owner) {
    const auto target = titleShadowTarget(guestBytes(owner,100));
    const auto& color = resource(target.color,Kind::Surface);
    const auto& depth = resource(target.depth,Kind::Surface);
    const auto& texture = resource(target.texture,Kind::Texture);
    const auto colorDescription = backend().surfaceDescription(color.native);
    const auto depthDescription = backend().surfaceDescription(depth.native);
    const auto textureDescription = backend().textureDescription(texture.native,0);
    if (color.xboxFormat != 0x28280186 || depth.xboxFormat != 0x2d200196 || texture.xboxFormat != 0x2d200196 ||
        depth.parent != target.texture || depth.parentKind != TitleSurfaceParent::Texture || depth.face || depth.level ||
        backend().textureLevelCount(texture.native) != 1 ||
        colorDescription.Width != target.width || colorDescription.Height != target.height ||
        depthDescription.Width != target.width || depthDescription.Height != target.height ||
        textureDescription.Width != target.width || textureDescription.Height != target.height ||
        colorDescription.Format != D3DFMT_X8R8G8B8 || depthDescription.Format != D3DFMT_D24S8 ||
        textureDescription.Format != D3DFMT_D24S8 ||
        colorDescription.MultiSampleType != D3DMULTISAMPLE_NONE || depthDescription.MultiSampleType != D3DMULTISAMPLE_NONE ||
        textureDescription.MultiSampleType != D3DMULTISAMPLE_NONE ||
        colorDescription.Usage != D3DUSAGE_RENDERTARGET || depthDescription.Usage != D3DUSAGE_DEPTHSTENCIL ||
        textureDescription.Usage != D3DUSAGE_DEPTHSTENCIL || colorDescription.Pool != D3DPOOL_DEFAULT ||
        depthDescription.Pool != D3DPOOL_DEFAULT || textureDescription.Pool != D3DPOOL_DEFAULT)
        throw std::runtime_error("Original type1 owner disagrees with actual native depth alias and color companion");
    backend().verifyDepthTextureSurfaceAlias(texture.native,depth.native);
    return target;
}
uint32_t originalOffscreenBegin(const PPCContext& ctx) {
    // Exact color-bind BL in original82390020 (RT vtable+4). Its r31 retains
    // the CPU owner, r27 the device, and r26 the original Begin0/1 preference.
    const uint32_t owner = ctx.r31.u32;
    const auto ownerBytes = guestBytes(owner,100);
    if (titleGuestWord(ownerBytes,4) == 1) {
        const auto target = originalShadowTarget(owner);
        if (ctx.lr != 0x8239009c || ctx.r4.u32 || ctx.r27.u32 != ctx.r3.u32 || ctx.r26.u32 > 1 || ctx.r5.u32 != target.color)
            throw std::runtime_error("Unverified original nonfloating type1 shadow Begin caller/arguments");
        return owner;
    }
    if (ctx.r4.u32 || ctx.r27.u32 != ctx.r3.u32 || ctx.r26.u32 > 1 ||
        titleGuestWord(ownerBytes,0) != 0x82161380 || titleGuestWord(ownerBytes,4) ||
        titleGuestWord(ownerBytes,44) != ctx.r5.u32 || titleGuestWord(ownerBytes,84) != 1 ||
        titleGuestWord(ownerBytes,88) != 999 || titleGuestWord(ownerBytes,96) != 0x28280186)
        throw std::runtime_error("Unverified original ordinary offscreen RT Begin owner/arguments");
    const auto& color = resource(ctx.r5.u32,Kind::Surface);
    const auto& depth = resource(titleGuestWord(ownerBytes,72),Kind::Surface);
    const auto& texture = resource(titleGuestWord(ownerBytes,28),Kind::Texture);
    const auto colorDescription = backend().surfaceDescription(color.native);
    const auto depthDescription = backend().surfaceDescription(depth.native);
    const auto textureDescription = backend().textureDescription(texture.native,0);
    const uint32_t width = titleGuestWord(ownerBytes,8), height = titleGuestWord(ownerBytes,12);
    if (!width || !height || width > 8192 || height > 8192 ||
        color.xboxFormat != 0x28280186 || depth.xboxFormat != 0x2d200196 || texture.xboxFormat != 0x28280186 ||
        colorDescription.Width != width || colorDescription.Height != height ||
        depthDescription.Width != width || depthDescription.Height != height ||
        textureDescription.Width != width || textureDescription.Height != height ||
        colorDescription.Format != D3DFMT_X8R8G8B8 || depthDescription.Format != D3DFMT_D24S8 ||
        textureDescription.Format != D3DFMT_X8R8G8B8 ||
        colorDescription.MultiSampleType != D3DMULTISAMPLE_2_SAMPLES || depthDescription.MultiSampleType != D3DMULTISAMPLE_2_SAMPLES ||
        textureDescription.MultiSampleType != D3DMULTISAMPLE_NONE ||
        !(colorDescription.Usage & D3DUSAGE_RENDERTARGET) || !(depthDescription.Usage & D3DUSAGE_DEPTHSTENCIL) ||
        !(textureDescription.Usage & D3DUSAGE_RENDERTARGET))
        throw std::runtime_error("Original offscreen RT Begin dimensions/formats/samples disagree with actual native resources");
    return owner;
}
template<class Geometry> struct OriginalPaletteGeometry {
    Geometry geometry;
    std::span<const uint8_t> palette;
};
OriginalPaletteGeometry<TitleBatchGeometry> originalBatchGeometry(const PPCContext& ctx, const Resource& vertices, const Resource& indices,
    std::span<const uint8_t> batch) {
    // Original RVMBatch draw8231BB28 is reached by material822D26B0 with
    // batch retained in both r16 and r31, then renderer448/lowLR8234D544.
    const uint32_t owner = ctx.r31.u32;
    if (ctx.lr != 0x8234d544 || ctx.r4.u32 != 4 || ctx.r5.u32 || ctx.r6.u32 || ctx.r16.u32 != owner ||
        batch.size() != 248 || titleBatchBEWord(batch,0) != 0x8215e29c)
        throw std::runtime_error("Unverified original RVMBatch indexed draw caller/base/start");
    const uint32_t descriptor = titleBatchBEWord(batch,244);
    const auto material = guestBytes(descriptor,24);
    const auto geometry = titleBatchCPUDescription(batch,material);
    const uint32_t parent = titleBatchBEWord(batch,192);
    const auto parentBytes = guestBytes(parent,660);
    validateTitleBatchParent(parentBytes,owner);
    const uint32_t renderer = memory->read32(0x82baa92c);
    const auto rendererBytes = guestBytes(renderer,titleRendererOwnedBytes);
    const uint64_t vertexHandle = titleBatchBEHandle(batch,216), indexHandle = titleBatchBEHandle(material,8);
    if (titleBatchBEHandle(rendererBytes,0x11b78) != vertexHandle ||
        titleBatchBEHandle(rendererBytes,0x11b80) != indexHandle ||
        titleBatchBEWord(rendererBytes,0x557e0) != geometry.activeInstances ||
        titleBatchBEWord(rendererBytes,0x56afc) || ctx.r7.u32 != titleBatchIndexCount(geometry))
        throw std::runtime_error("Original RVMBatch current handles/count/palette disagree with the real draw");
    const auto vertexEntry = titleRendererOwnedRange(rendererBytes,renderer,
        titleBatchRegistryEntry(renderer,0x11b90,vertexHandle),24);
    const auto indexEntry = titleRendererOwnedRange(rendererBytes,renderer,
        titleBatchRegistryEntry(renderer,0x31778,indexHandle),24);
    const auto engineVertex = guestBytes(titleBatchRegisteredObject(vertexEntry,vertexHandle),72);
    const auto engineIndex = guestBytes(titleBatchRegisteredObject(indexEntry,indexHandle),64);
    static uint32_t batchBufferDiagnostics = 0;
    if (batchBufferDiagnostics++ < 16) {
        std::fprintf(stderr,"[NATIVE BATCH ENGINE BUFFERS] owner=%08X capacity=%u active=%u vertices_per_instance=%u triangles_per_instance=%u vb_vtable=%08X vb_count20=%u vb_count24=%u vb_flag49=%u vb_flag50=%u vb_format56=%u vb_stride60=%u vb_token64=%08X vb_native_count68=%u expected_vb_token=%08X vb_bytes=%u ib_vtable=%08X ib_count20=%u ib_count24=%u ib_flag49=%u ib_flag50=%u ib_token56=%08X ib_native_count60=%u expected_ib_token=%08X ib_bytes=%u\n",
            owner,geometry.capacity,geometry.activeInstances,geometry.verticesPerInstance,geometry.trianglesPerInstance,
            titleBatchBEWord(engineVertex,0),titleBatchBEWord(engineVertex,20),titleBatchBEWord(engineVertex,24),engineVertex[49],engineVertex[50],
            titleBatchBEWord(engineVertex,56),titleBatchBEWord(engineVertex,60),titleBatchBEWord(engineVertex,64),titleBatchBEWord(engineVertex,68),state.vertexBuffer,vertices.bytes,
            titleBatchBEWord(engineIndex,0),titleBatchBEWord(engineIndex,20),titleBatchBEWord(engineIndex,24),engineIndex[49],engineIndex[50],
            titleBatchBEWord(engineIndex,56),titleBatchBEWord(engineIndex,60),state.indexBuffer,indices.bytes);
        std::fflush(stderr);
    }
    validateTitleBatchEngineBuffers(engineVertex,engineIndex,geometry,state.vertexBuffer,state.indexBuffer,vertices.bytes,indices.bytes);
    if (vertices.stride != 84 || vertices.dirty || indices.dirty ||
        !vertices.storage.ownedToken || !vertices.storage.ownedData ||
        !indices.storage.ownedToken || !indices.storage.ownedData ||
        !isTitleBatchDeclaration(resource(state.declaration,Kind::Declaration).declaration))
        throw std::runtime_error("Original RVMBatch does not own its actually bound format16 native buffers/declaration");
    // These inline palette bytes stay within the checked renderer. No guest
    // callback, write, allocation or free occurs before the caller copies them.
    return {geometry,titleRendererPalette(rendererBytes,renderer,geometry.activeInstances,32)};
}
// Diagnostics never call guest code or replace unreadable data with a default.
// This is deliberately bounded even when an original string is malformed.
std::string cjDiagnosticString(uint32_t address) {
    if (!address) return "<null>";
    std::string value;
    for (uint32_t offset = 0; offset < 128; ++offset) {
        if (uint64_t(address) + offset > UINT32_MAX || !memory->valid(address + offset,1)) return "<unreadable>";
        const uint8_t byte = memory->base()[address + offset];
        if (!byte) return value;
        value.push_back(byte >= 32 && byte < 127 ? char(byte) : '?');
    }
    return "<unbounded>";
}
uint32_t diagnoseCJDraw(const PPCContext& ctx, std::span<const uint8_t> instance,
    std::span<const uint8_t> master, std::span<const uint8_t> group, uint32_t selected, uint32_t ordinal) {
    if (!state.traceAvatar || state.cjDrawDiagnostics >= 128) return 0;
    struct LastErrorRestore { DWORD value = GetLastError(); ~LastErrorRestore() { SetLastError(value); } } lastErrorRestore;
    constexpr char name[] = "_avatar/cj_native.cfg";
    const uint32_t nameAddress = titleGuestWord(master,8);
    if (uint64_t(nameAddress) + sizeof(name) > UINT32_MAX || !memory->valid(nameAddress,sizeof(name)) ||
        std::memcmp(memory->base() + nameAddress,name,sizeof(name))) return 0;
    const uint32_t serial = ++state.cjDrawDiagnostics;
    const auto floatWord = [](std::span<const uint8_t> bytes, uint32_t offset) {
        return std::bit_cast<float>(titleGuestWord(bytes,offset));
    };
    std::fprintf(stderr,
        "[NATIVE CJ DRAW] serial=%u master=%08X instance=%08X name=%s geometry_loaded581=%u gpu656=%u material_loadnow668=%u group=%u group_ordinal=%u material_ordinal=%u local_opacity116=%.9g world_opacity200=%.9g quantized_opacity204=%u local_scale120=[%.9g,%.9g,%.9g] world_scale188=[%.9g,%.9g,%.9g] world_position172=[%.9g,%.9g,%.9g]\n",
        serial,ctx.r31.u32,ctx.r16.u32 - 408,name,master[581],master[656],master[668],selected,ordinal,
        titleGuestWord(group,40),floatWord(instance,116),floatWord(instance,200),titleGuestWord(instance,204),
        floatWord(instance,120),floatWord(instance,124),floatWord(instance,128),
        floatWord(instance,188),floatWord(instance,192),floatWord(instance,196),
        floatWord(instance,172),floatWord(instance,176),floatWord(instance,180));
    // Type1 822D05E8 keeps its actual selected ShaderMaster in r30. The
    // geometry VT12=8239B648 saves it at callerSP-24, then makes a128B
    // frame; renderer8234D4A8 adds112B. Only those exact three return routes
    // and both actual stack backchains prove the saved value at hookSP+216.
    uint32_t shader = 0, shaderCaller = 0;
    bool stackProven = false;
    if (uint64_t(ctx.r1.u32) + 240 <= UINT32_MAX && memory->valid(ctx.r1.u32,240)) {
        const std::span<const uint8_t> stack{memory->base() + ctx.r1.u32,240};
        shaderCaller = titleGuestWord(stack,232);
        stackProven = titleGuestWord(stack,0) == ctx.r1.u32 + 112 &&
            titleGuestWord(stack,112) == ctx.r1.u32 + 240 && titleGuestWord(stack,104) == 0x8239b704 &&
            (shaderCaller == 0x822d1504 || shaderCaller == 0x822d16a0 || shaderCaller == 0x822d16e8) &&
            titleGuestWord(stack,216) == 0;
        if (stackProven) shader = titleGuestWord(stack,220);
    }
    const bool shaderReadable = shader && memory->valid(shader,528);
    const std::span<const uint8_t> shaderBytes = shaderReadable ?
        std::span<const uint8_t>{memory->base() + shader,528} : std::span<const uint8_t>{};
    const uint32_t vtable = shaderReadable ? titleGuestWord(shaderBytes,0) : UINT32_MAX;
    const bool shaderProven = shaderReadable && (vtable == 0x82160cec || vtable == 0x82157b7c);
    std::fprintf(stderr,"[NATIVE CJ MATERIAL] serial=%u selected_shader_stack_proven=%u shader_caller=%08X shader=%08X readable=%u vtable=%08X original_shader_class=%u\n",
        serial,stackProven ? 1u : 0u,shaderCaller,shader,shaderReadable ? 1u : 0u,vtable,shaderProven ? 1u : 0u);
    if (shaderProven) {
        const uint64_t handle = uint64_t(titleGuestWord(shaderBytes,16)) << 32 | titleGuestWord(shaderBytes,20);
        const uint32_t type = titleGuestWord(shaderBytes,292), data = titleGuestWord(shaderBytes,296);
        const bool typeDataReadable = type == 1 && data && uint64_t(data) + 84 <= UINT32_MAX && memory->valid(data,84);
        const uint32_t diffuse = typeDataReadable ? memory->read32(data + 80) : 0;
        const uint32_t texture = titleGuestWord(shaderBytes,340);
        const bool textureReadable = texture && memory->valid(texture,172);
        const auto shaderName = cjDiagnosticString(titleGuestWord(shaderBytes,8));
        const auto diffuseName = typeDataReadable ? cjDiagnosticString(diffuse) : std::string("<unproven-type-data>");
        const auto textureName = textureReadable ? cjDiagnosticString(memory->read32(texture + 8)) : std::string("<unreadable>");
        std::fprintf(stderr,
            "[NATIVE CJ MATERIAL VALUES] serial=%u shader=%08X handle=%016llX name=%s refs96=%u loaded100=%u type292=%u data296=%08X type1_data_readable=%u diffuse80=%08X diffuse_name=%s texture340=%08X texture_readable=%u texture_name=%s texture_refs168=%u blend132=%u alpha136=%u flags148=%08X diffuse_rgb158=[%u,%u,%u] cached_opaque408=%u\n",
            serial,shader,static_cast<unsigned long long>(handle),shaderName.c_str(),titleGuestWord(shaderBytes,96),
            shaderBytes[100],type,data,typeDataReadable ? 1u : 0u,diffuse,diffuseName.c_str(),texture,
            textureReadable ? 1u : 0u,textureName.c_str(),textureReadable ? memory->read32(texture + 168) : UINT32_MAX,
            titleGuestWord(shaderBytes,132),titleGuestWord(shaderBytes,136),titleGuestWord(shaderBytes,148),
            shaderBytes[158],shaderBytes[159],shaderBytes[160],shaderBytes[408]);
        // Original Type1 BuildTextures reads this actual TextureManager global.
        // Its shipped DX group VT52 is a pure load of the runtime type index;
        // read that same word without calling the guest or creating a resource.
        constexpr uint32_t textureManagerGlobal = 0x82b5ab78, textureTypeIndex = 0x82a74b08;
        constexpr uint32_t typeFlags = 0x82baa938, textureManagerVtable = 0x82160c44;
        const bool managerGlobalReadable = memory->valid(textureManagerGlobal,4);
        const uint32_t manager = managerGlobalReadable ? memory->read32(textureManagerGlobal) : 0;
        const bool managerReadable = manager && uint64_t(manager) + 72 <= UINT32_MAX && memory->valid(manager,72);
        const uint32_t managerVtable = managerReadable ? memory->read32(manager) : UINT32_MAX;
        const bool managerVtableReadable = managerVtable != UINT32_MAX && memory->valid(managerVtable,56);
        const uint32_t indexGetter = managerVtableReadable ? memory->read32(managerVtable + 52) : UINT32_MAX;
        const bool originalTextureGroup = managerReadable && managerVtable == textureManagerVtable &&
            indexGetter == 0x822ac9c0;
        const bool indexReadable = originalTextureGroup && memory->valid(textureTypeIndex,4);
        const uint32_t typeIndex = indexReadable ? memory->read32(textureTypeIndex) : UINT32_MAX;
        const bool flagsReadable = indexReadable && typeIndex < 256 && memory->valid(typeFlags + typeIndex * 4,4);
        const uint32_t flags = flagsReadable ? memory->read32(typeFlags + typeIndex * 4) : UINT32_MAX;
        // Original8224E360 consults these flags only after its name lookup misses.
        // Reporting this predicate does not establish whether a named master exists.
        const uint32_t absentCreationAllowed = flagsReadable ? ((flags & 0x20) || !(flags & 4) ? 1u : 0u) : UINT32_MAX;
        std::fprintf(stderr,
            "[NATIVE CJ TEXTURE GROUP] serial=%u manager_global_readable=%u manager=%08X manager_readable=%u vtable=%08X index_getter=%08X original_texture_group=%u type_index_readable=%u type_index=%u flags_readable=%u flags=%08X absent_name_creation_allowed=%u\n",
            serial,managerGlobalReadable ? 1u : 0u,manager,managerReadable ? 1u : 0u,managerVtable,indexGetter,
            originalTextureGroup ? 1u : 0u,indexReadable ? 1u : 0u,typeIndex,flagsReadable ? 1u : 0u,flags,absentCreationAllowed);
    }
    std::fflush(stderr);
    return serial;
}
OriginalPaletteGeometry<TitleBoneGeometry> originalBoneGeometry(const PPCContext& ctx, const Resource& vertices, const Resource& indices,
    uint32_t* cjDiagnosticSerial = nullptr) {
    const uint32_t master = ctx.r31.u32;
    if (ctx.lr != 0x8234d544 || ctx.r4.u32 != 4 || ctx.r16.u32 < 408)
        throw std::runtime_error("Unverified original Cal3D indexed palette draw caller");
    const auto instance = guestBytes(ctx.r16.u32 - 408,712);
    const auto masterBytes = guestBytes(master,812);
    const auto word = [](std::span<const uint8_t> bytes, uint32_t offset) {
        return uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset + 1]) << 16 |
            uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
    };
    if (word(instance,0) != 0x82161c24 || word(instance,408) != 0x82161f20 || word(instance,64) != master ||
        word(masterBytes,0) != 0x82161af4 || !masterBytes[656] || instance[657] ||
        vertices.stride != 80 || vertices.dirty || indices.dirty ||
        !vertices.storage.ownedToken || !vertices.storage.ownedData ||
        !indices.storage.ownedToken || !indices.storage.ownedData ||
        !isTitleBoneDeclaration(resource(state.declaration,Kind::Declaration).declaration)) {
        // Diagnostic only: a native skin shader can be reached by another
        // original geometry interface. No Cal3D ownership/tail gate is relaxed.
        static uint32_t ownershipDiagnostics = 0;
        if (ownershipDiagnostics++ < 16) {
            const uint32_t renderer = memory->read32(0x82baa92c);
            const auto& declaration = resource(state.declaration,Kind::Declaration).declaration;
            const bool cachedHandlesValid = memory->valid(renderer,0x11b88);
            const uint64_t cachedVertex = cachedHandlesValid ? memory->read64(renderer + 0x11b78) : UINT64_MAX;
            const uint64_t cachedIndex = cachedHandlesValid ? memory->read64(renderer + 0x11b80) : UINT64_MAX;
            // Read-only equivalent of original8224DEE8: inline24B registry
            // entries require a signed16 index and exact full64 identity.
            const auto descriptor = [&](uint64_t handle, uint32_t registry, uint32_t bytes) {
                const int32_t index = static_cast<int16_t>((handle >> 16) & 0xffff);
                if (index < 0 || index >= 5000) return uint32_t(0);
                const uint64_t address = uint64_t(renderer) + registry + uint32_t(index) * 24;
                if (address > UINT32_MAX || !memory->valid(static_cast<uint32_t>(address),24) ||
                    memory->read64(static_cast<uint32_t>(address) + 16) != handle) return uint32_t(0);
                const uint32_t object = memory->read32(static_cast<uint32_t>(address) + 8);
                return memory->valid(object,bytes) ? object : uint32_t(0);
            };
            const uint32_t engineVertex = descriptor(cachedVertex,0x11b90,72);
            const uint32_t engineIndex = descriptor(cachedIndex,0x31778,64);
            const uint32_t indexInfo = word(masterBytes,244);
            std::fprintf(stderr,
                "[NATIVE PALETTE BATCH] owner=%08X vtable=%08X vertices204=%u capacity_vertices208=%u vertex_handle216=%08X%08X active_instances240=%u index_info244=%08X index_info_valid=%u index_handle8=%016llX source_triangles18=%u cached_vb_handle11B78=%016llX cached_ib_handle11B80=%016llX resolved_engine_vb=%08X resolved_vb_vtable=%08X resolved_vb_format56=%u resolved_vb_stride60=%u resolved_vb_token64=%08X resolved_engine_ib=%08X resolved_ib_vtable=%08X resolved_ib_token56=%08X\n",
                master,word(masterBytes,0),word(masterBytes,204),word(masterBytes,208),word(masterBytes,216),word(masterBytes,220),
                word(masterBytes,240),indexInfo,static_cast<uint32_t>(memory->valid(indexInfo,24)),
                static_cast<unsigned long long>(memory->valid(indexInfo,24) ? memory->read64(indexInfo + 8) : UINT64_MAX),
                memory->valid(indexInfo,24) ? memory->read32(indexInfo + 16) & 0xffffu : UINT32_MAX,
                static_cast<unsigned long long>(cachedVertex),static_cast<unsigned long long>(cachedIndex),
                engineVertex,engineVertex ? memory->read32(engineVertex) : UINT32_MAX,
                engineVertex ? memory->read32(engineVertex + 56) : UINT32_MAX,
                engineVertex ? memory->read32(engineVertex + 60) : UINT32_MAX,
                engineVertex ? memory->read32(engineVertex + 64) : UINT32_MAX,
                engineIndex,engineIndex ? memory->read32(engineIndex) : UINT32_MAX,
                engineIndex ? memory->read32(engineIndex + 56) : UINT32_MAX);
            std::fprintf(stderr,
                "[NATIVE PALETTE OWNER] lr=%08X interface_r16=%08X interface_vtable=%08X candidate_instance=%08X candidate_instance_vtable=%08X candidate_master_r31=%08X candidate_master_vtable=%08X instance_master64=%08X master_gpu656=%u instance_special657=%u renderer=%08X active_bones=%u generic_vertices204=%u generic_instances240=%u generic_index_info244=%08X vb_token=%08X vb_data=%08X vb_native=%u vb_bytes=%u vb_stride=%u vb_dirty=%u vb_owned_token=%u vb_owned_data=%u ib_token=%08X ib_data=%08X ib_native=%u ib_bytes=%u ib_dirty=%u ib_owned_token=%u ib_owned_data=%u declaration_token=%08X elements=%zu\n",
                static_cast<uint32_t>(ctx.lr),ctx.r16.u32,word(instance,408),ctx.r16.u32 - 408,word(instance,0),master,word(masterBytes,0),
                word(instance,64),masterBytes[656],instance[657],renderer,memory->valid(renderer,0x557e4) ? memory->read32(renderer + 0x557e0) : UINT32_MAX,
                word(masterBytes,204),word(masterBytes,240),word(masterBytes,244),state.vertexBuffer,vertices.data,vertices.native,
                vertices.bytes,vertices.stride,static_cast<uint32_t>(vertices.dirty),static_cast<uint32_t>(vertices.storage.ownedToken),
                static_cast<uint32_t>(vertices.storage.ownedData),state.indexBuffer,indices.data,indices.native,indices.bytes,
                static_cast<uint32_t>(indices.dirty),static_cast<uint32_t>(indices.storage.ownedToken),static_cast<uint32_t>(indices.storage.ownedData),
                state.declaration,declaration.size());
            for (const auto& element : declaration)
                std::fprintf(stderr,"[NATIVE PALETTE DECL] stream=%u offset=%u type=%u method=%u usage=%u index=%u\n",
                    element.Stream,element.Offset,element.Type,element.Method,element.Usage,element.UsageIndex);
            backend().reportTitlePaletteState();
            std::fflush(stderr);
        }
        throw std::runtime_error("Original Cal3D palette draw has unverified instance, GPU buffer or format10 ownership");
    }
    const auto exporter = guestBytes(word(masterBytes,756),224);
    const bool mismatched = word(exporter,56) != vertices.data || word(exporter,60) != 80 || word(exporter,220) != indices.data;
    if (state.boneExporterDiagnostics++ < 16 || mismatched) {
        std::fprintf(stderr,
            "[NATIVE BONE EXPORTER] master=%08X exporter=%08X vb_token=%08X vb_data=%08X vb_native=%u ib_token=%08X ib_data=%08X ib_native=%u positions=%08X stride=%u index_data=%08X unused_staging_normal=%08X unused_staging_weight=%08X unused_staging_blend_index=%08X\n",
            master,word(masterBytes,756),state.vertexBuffer,vertices.data,vertices.native,state.indexBuffer,indices.data,indices.native,
            word(exporter,56),word(exporter,60),word(exporter,220),word(exporter,64),word(exporter,72),word(exporter,80));
        std::fflush(stderr);
    }
    validateTitleBoneExporterBinding(exporter,vertices.data,indices.data);
    const auto vector = [&](uint32_t begin, uint32_t end, uint32_t stride, uint32_t limit) {
        if (!begin || end <= begin || end - begin > limit || (end - begin) % stride)
            throw std::runtime_error("Original Cal3D group vector exceeds verified CPU bounds");
        return guestBytes(begin,end - begin);
    };
    const auto groups = vector(word(exporter,0),word(exporter,4),44,44 * 65536);
    const auto groupIds = vector(word(masterBytes,804),word(masterBytes,808),4,4 * 65536);
    const uint32_t selected = word(exporter,32), ordinal = word(instance,660);
    if (selected >= groups.size() / 44 || ordinal >= groupIds.size() / 4 || word(groupIds,ordinal * 4) != selected)
        throw std::runtime_error("Original Cal3D selected group does not belong to the current Bone instance draw");
    const auto group = groups.subspan(selected * 44,44);
    const auto bones = vector(word(group,0),word(group,4),4,48 * 4);
    TitleBoneGeometry geometry{word(masterBytes,660),word(masterBytes,664),word(group,16),word(group,20),
        word(group,24),word(group,28) * 3,static_cast<uint32_t>(bones.size() / 4)};
    const uint32_t rendererAddress = memory->read32(0x82baa92c);
    const auto renderer = guestBytes(rendererAddress,titleRendererOwnedBytes);
    if (word(group,28) > UINT32_MAX / 3 || geometry.baseVertex != ctx.r5.u32 || geometry.startIndex != ctx.r6.u32 ||
        geometry.indexCount != ctx.r7.u32 || word(renderer,0x557e0) != geometry.boneCount ||
        word(renderer,0x56afc) != geometry.baseVertex || uint64_t(geometry.totalVertices) * 80 > vertices.bytes ||
        uint64_t(geometry.totalTriangles) * 6 > indices.bytes)
        throw std::runtime_error("Original Cal3D group geometry/palette disagrees with the actual indexed draw");
    const auto palette = titleRendererPalette(renderer,rendererAddress,geometry.boneCount,48);
    if (cjDiagnosticSerial) *cjDiagnosticSerial = diagnoseCJDraw(ctx,instance,masterBytes,group,selected,ordinal);
    return {geometry,palette};
}
template<class F> void call(F action, bool phaseFrameBoundary = false,
    const std::source_location& location = std::source_location::current()) {
    std::lock_guard lock(state.mutex);
    const bool queryTiming = Doritos::Native::memoryQueryTimingEnabled() &&
        (!memoryQueryReport.thread || memoryQueryReport.thread == GetCurrentThreadId());
    if (!state.reportPhases && !queryTiming) {
        try { action(); } catch (const std::exception& error) { Doritos::Native::fail(error.what()); }
        return;
    }
    const bool outermost = bridgePhaseDepth++ == 0;
    const auto queriesBefore = queryTiming && outermost ? Doritos::Native::memoryQueryCounters() : MemoryQueryCounters{};
    std::chrono::steady_clock::time_point start;
    if (state.reportPhases) start = std::chrono::steady_clock::now();
    try { action(); }
    catch (const std::exception& error) { --bridgePhaseDepth; Doritos::Native::fail(error.what()); }
    --bridgePhaseDepth;
    if (outermost && state.backend) {
        uint64_t hookWallNanoseconds = 0;
        if (state.reportPhases) {
            // Reuse the existing phase interval; query-only reporting takes no
            // additional clock readings and leaves hook wall time unselected.
            hookWallNanoseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
            state.backend->recordBridgePhaseTime(hookWallNanoseconds);
            if (phaseFrameBoundary) state.backend->finishPhaseFrame();
        }
        if (queryTiming) {
            const auto queriesAfter = Doritos::Native::memoryQueryCounters();
            const auto queries = memoryQueryDifference(queriesAfter,queriesBefore);
            addMemoryQueries(memoryQueryReport.bridgeCycle,queries);
            recordMemoryQuerySite(location,queries,hookWallNanoseconds,state.reportPhases);
            if (phaseFrameBoundary) finishMemoryQueryFrame(queriesAfter);
        }
    }
}
}
void prepareTitleGraphics(std::span<const uint8_t> windowsCache, std::span<const uint8_t> xenonCache, std::span<const uint8_t> nativeExtension, std::span<const uint8_t> xenonSourceExtension, HANDLE closeRequested) {
    std::lock_guard lock(state.mutex);
    if (state.backend || !state.resources.empty()) throw std::runtime_error("Native title graphics is already active");
    auto original = ShaderCache::parse(windowsCache, ShaderCachePlatform::Windows);
    if (!nativeExtension.empty()) original.withWindowsExtension(ShaderCache::parse(nativeExtension, ShaderCachePlatform::Windows));
    auto originalXenon = ShaderCache::parse(xenonCache, ShaderCachePlatform::Xenon);
    if (!xenonSourceExtension.empty()) originalXenon.withXenonSourceExtension(ShaderCache::parse(xenonSourceExtension,ShaderCachePlatform::Xenon));
    state.closeRequested = closeRequested;
    state.windowsCache.assign(windowsCache.begin(), windowsCache.end());
    state.xenonCache.assign(xenonCache.begin(), xenonCache.end());
    state.nativeExtension.assign(nativeExtension.begin(), nativeExtension.end());
    state.xenonSourceExtension.assign(xenonSourceExtension.begin(),xenonSourceExtension.end());
    wchar_t phaseReport[2]{};
    state.reportPhases = GetEnvironmentVariableW(L"DORITOS_PHASE_REPORT",phaseReport,2) == 1 && phaseReport[0] == L'1';
    wchar_t avatarTrace[2]{};
    state.traceAvatar = GetEnvironmentVariableW(L"DORITOS_AVATAR_TRACE",avatarTrace,2) == 1 && avatarTrace[0] == L'1';
}
void shutdownTitleGraphics() {
    std::lock_guard lock(state.mutex);
    state.backend.reset();
    state.window.reset();
    state.closeRequested = nullptr;
    if (memory) for (const auto& [address, entry] : state.resources) {
        if (entry.data && entry.storage.ownedData) memory->release(entry.data);
        if (entry.storage.ownedToken) memory->release(address);
    }
    state.resources.clear();
    state.device = state.declaration = state.vertexBuffer = state.indexBuffer = 0;
    state.sceneRenderer = 0;
    state.presentationContext = nullptr;
    state.immediateDiagnostics = 0;
    state.optionalShaderDiagnostics = 0;
    state.optionalDeclarationDiagnostics = 0;
    state.offscreenFrameDiagnostics = 0;
    state.boneDrawDiagnostics = 0;
    state.boneExporterDiagnostics = 0;
    state.cjDrawDiagnostics = 0;
    state.reportPhases = false;
    state.traceAvatar = false;
    memoryQueryReport = {};
}
RenderCounters titleGraphicsCounters() {
    std::lock_guard lock(state.mutex);
    return state.backend ? state.backend->counters() : RenderCounters{};
}
void prepareTitleFrame() {
    std::lock_guard lock(state.mutex);
    backend().setKeyboardMousePrompts(Doritos::Native::nativeInputDevice() == Doritos::Native::NativeInputDevice::KeyboardMouse);
    backend().prepareFrame();
}
void prepareTitleSceneResolution(uint32_t renderer) {
    std::lock_guard lock(state.mutex);
    if (state.sceneRenderer == renderer) return;
    if (state.sceneRenderer || !memory || !memory->valid(renderer,0x56bcc))
        throw std::runtime_error("Unverified original primary scene resource owner");
    const SceneResolutionResources resources{
        resource(memory->read32(renderer+0x5655c),Kind::Surface).native,
        resource(memory->read32(renderer+0x56560),Kind::Surface).native,
        {resource(memory->read32(renderer+0x56bb8),Kind::Texture).native,
         resource(memory->read32(renderer+0x56bbc),Kind::Texture).native}};
    backend().setSceneResolutionResources(resources,[renderer](uint32_t width,uint32_t height) {
        if (!state.presentationContext || !memory)
            throw std::runtime_error("Original CPU resolution update requires a current presentation call frame");
        updateTitleRenderResolution(*state.presentationContext,memory->base(),renderer,width,height);
    });
    state.sceneRenderer = renderer;
}
void setTitleGuiPointerContext(const GuiPointerContext& context) {
    std::lock_guard lock(state.mutex);
    if (!state.backend) return;
    // Original GUISCREEN owns a counted string at +0/+4. Only guiScreens is
    // rendered through rtt/guiScreens; dialogs, pause and HUD use flat input
    // even when that monitor is still visible behind them.
    bool monitorScreen = false;
    if (memory && context.screen && memory->valid(context.screen,104) && memory->read32(context.screen + 4) == 10) {
        const uint32_t name = memory->read32(context.screen);
        monitorScreen = name && memory->valid(name,11) && !std::memcmp(memory->base() + name,"guiScreens",11);
    }
    state.backend->setGuiPointerContext(context,monitorScreen);
}
GuiPointerResult mapTitleGuiPointer(const GuiPointerContext& context, float x, float y) {
    std::lock_guard lock(state.mutex);
    return state.backend ? state.backend->mapGuiPointer(context,x,y) : GuiPointerResult{};
}
GuiPointerResult mapTitlePresentationPointer(float x,float y) {
    std::lock_guard lock(state.mutex);
    return state.backend?state.backend->mapPresentationPointer(x,y):GuiPointerResult{GuiPointerMapping::Hit,x,y};
}
TitleDepthTargetResources createTitleDepthTargetResources(uint32_t owner) {
    std::lock_guard lock(state.mutex);
    const auto description = titleDepthTargetDescription(guestBytes(owner,100));
    auto& gpu = backend();
    return allocateTitleDepthTargetPair(
        [&] { return gpu.createDepthTexture(description.width,description.height); },
        [&](uint32_t texture) { return gpu.textureSurface(texture,0,0); },
        [&](uint32_t texture, uint32_t view) {
            gpu.verifyDepthTextureSurfaceAlias(texture,view);
            Resource textureEntry{Kind::Texture,texture};
            textureEntry.xboxFormat = 0x2d200196;
            return publishTitleDepthTargetPair(state.resources,std::move(textureEntry),
                [&](uint32_t parent) {
                    Resource viewEntry{Kind::Surface,view};
                    viewEntry.xboxFormat = 0x2d200196;
                    viewEntry.parent = parent;
                    viewEntry.parentKind = TitleSurfaceParent::Texture;
                    return viewEntry;
                },
                [&] {
                    // Retain the reservation address even if commit fails so
                    // this new pair cannot strand partially allocated storage.
                    const uint32_t address = memory->reserve(16);
                    try { memory->commit(address,memory->allocationSize(address)); }
                    catch (...) { memory->release(address); throw; }
                    return address;
                },
                [](uint32_t address, const Resource& entry) { initializeResourceToken(address,entry); },
                [](uint32_t address) { memory->release(address); });
        },
        [&](uint32_t handle) { gpu.release(handle); });
}
}

using namespace Doritos::Graphics;
PPC_FUNC(sub_822AC6F8);
extern "C" PPC_FUNC(__imp__sub_823E3498);
extern "C" PPC_FUNC(__imp__sub_8234FAB8);
// Strong AOT overrides. Original material/scene code stays compiled PPC; these
// boundaries consume verified arguments and operate native Windows resources.
PPC_FUNC(sub_823D25D8) {
    call([&] {
        guestBytes(ctx.r7.u32, 16);
        guestBytes(ctx.r8.u32, 4);
        if (state.device) throw std::runtime_error("Multiple title graphics devices are unsupported");
        backend();
        state.device = token({Kind::Device});
        Doritos::Native::memory->write32(ctx.r8.u32, state.device);
        state.window->revealStartup();
        std::fprintf(stderr, "[native renderer] original title requested hardware device: 2560x1440, target 60 FPS; draws/presents=0\n");
        ctx.r3.u64 = 0;
    });
}
PPC_FUNC(sub_823C0440) { call([&] { createShader(ctx, ShaderStage::Pixel); }); }
PPC_FUNC(sub_823C0628) { call([&] { createShader(ctx, ShaderStage::Vertex); }); }
PPC_FUNC(sub_823BEA90) { call([&] {
    device(ctx.r3.u32);
    backend().bindShader(ShaderStage::Pixel, ctx.r4.u32 ? resource(ctx.r4.u32, Kind::PixelShader).native : 0);
}); }
PPC_FUNC(sub_823BEC50) { call([&] {
    device(ctx.r3.u32);
    backend().bindShader(ShaderStage::Vertex, ctx.r4.u32 ? resource(ctx.r4.u32, Kind::VertexShader).native : 0);
}); }
PPC_FUNC(sub_823CE338) { call([&] { createBuffer(ctx, Kind::VertexBuffer); }); }
PPC_FUNC(sub_823CE410) { call([&] { createBuffer(ctx, Kind::IndexBuffer); }); }
PPC_FUNC(sub_824CF7D0) { call([&] {
    // Original82390E18 creates two supplied32-byte slots and later associates
    // its real6912-byte palette allocation. Native GPU upload remains lazy.
    if (ctx.lr != 0x82390e9c || ctx.r3.u32 != TitleAvatarBufferBinding::bytes ||
        ctx.r4.u32 || ctx.r5.u32 || ctx.r6.u32)
        throw std::runtime_error("Unverified original inline vertex buffer header creation caller/arguments");
    const auto binding = originalInlineBuffer(ctx, ctx.r7.u32);
    Resource entry{Kind::VertexBuffer, 0, 0, TitleAvatarBufferBinding::bytes};
    entry.storage = binding.lifetime;
    if (!state.resources.emplace(binding.storage, std::move(entry)).second)
        throw std::runtime_error("Original inline vertex buffer storage is already registered");
    // Mark native adapter identity only; no Xbox GPU header words/fetch address.
    memory->write32(binding.storage, kTokenMagic);
    memory->write32(binding.storage + 4, static_cast<uint32_t>(Kind::VertexBuffer));
    memory->write32(binding.storage + 8, 0);
    memory->write32(binding.storage + 12, 0);
    ctx.r3.u64 = binding.storage; // Original memset result, unused by the caller.
}); }
PPC_FUNC(sub_824CFBD8) { call([&] {
    if (ctx.lr != 0x82390eac)
        throw std::runtime_error("Unverified original inline vertex buffer address association caller");
    const auto binding = originalInlineBuffer(ctx, ctx.r3.u32);
    auto& entry = resource(binding.storage, Kind::VertexBuffer);
    if (ctx.r4.u32 != binding.data || entry.storage.ownedToken || entry.storage.ownedData ||
        entry.storage.owner != binding.lifetime.owner || entry.storage.backingBase != binding.lifetime.backingBase ||
        entry.storage.offset != binding.lifetime.offset || entry.bytes != TitleAvatarBufferBinding::bytes || entry.data || entry.native)
        throw std::runtime_error("Original inline vertex buffer association disagrees with its pending registration");
    entry.data = binding.data;
    entry.dirty = true;
    ctx.r3.u64 = 1; // Original type query result, unused by the caller.
}); }
PPC_FUNC(sub_823E3498) {
    call([&] {
        const uint32_t count = detachTitleBorrowedBuffers(state.resources, ctx.r3.u32,
            [](uint32_t address, const Resource& entry) {
                if (!entry.native) return;
                if (state.vertexBuffer == address) {
                    backend().unbindVertexBuffer();
                    state.vertexBuffer = 0;
                }
                backend().release(entry.native);
            });
        if (count) {
            std::fprintf(stderr, "[NATIVE INLINE BUFFER] physical_free=%08X borrowed_aliases_detached=%u\n", ctx.r3.u32, count);
            std::fflush(stderr);
        }
    });
    // Exact original wrapper is r4=r3; r3=0; tail MmFreePhysicalMemory.
    // Forward only after releasing the graphics mutex, preserving memory-service
    // lock order and leaving genuine physical/header frees with their CPU owner.
    __imp__sub_823E3498(ctx, base);
}
PPC_FUNC(sub_823CF300) { call([&] { lockBuffer(ctx, Kind::VertexBuffer); }); }
PPC_FUNC(sub_823CF350) { call([&] { lockBuffer(ctx, Kind::IndexBuffer); }); }
PPC_FUNC(sub_823CE400) { call([&] { resource(ctx.r3.u32, Kind::VertexBuffer).dirty = true; }); }
PPC_FUNC(sub_823CE4C0) { call([&] { resource(ctx.r3.u32, Kind::IndexBuffer).dirty = true; }); }
PPC_FUNC(sub_823BEF28) { call([&] {
    const uint32_t address = ctx.r3.u32;
    uint32_t bytes = 0;
    for (; bytes < 12 * (MAXD3DDECLLENGTH + 1); bytes += 12) {
        const auto record = guestBytes(address + bytes, 12);
        if (record[0] == 0 && record[1] == 255) { bytes += 12; break; }
    }
    const auto records = guestBytes(address, bytes);
    if (ctx.lr == 0x82396320 && uint64_t(ctx.r1.u32) + 240 == address &&
        isTitleUnavailableAvatarDeclaration(records)) {
        const uint32_t owner = ctx.r30.u32;
        guestBytes(owner, 136);
        const uint32_t vtable = memory->read32(owner);
        if ((vtable != 0x8213d634 && vtable != 0x8213d70c) || uint64_t(owner) + 56 != ctx.r16.u32)
            throw std::runtime_error("Original optional Avatar declaration owner does not match its initializer");
        for (uint32_t index = 0; index < 5; ++index)
            if (memory->read32(owner + 56 + index * 16))
                throw std::runtime_error("Original optional Avatar declaration ownership was not cleared");
        // Low creation originally returns null on allocation failure. This exact
        // caller converts null to its negative result and exits before stores or
        // AddRef. The original destructor only releases nonnull ownership slots.
        if (state.optionalDeclarationDiagnostics++ < 8) {
            std::fprintf(stderr, "[NATIVE OPTIONAL DECLARATION] lr=%08X owner=%08X layout=SDK-avatar-first creation=unavailable returned=null\n",
                static_cast<uint32_t>(ctx.lr), owner);
            std::fflush(stderr);
        }
        ctx.r3.u64 = 0;
        return;
    }
    auto declaration = decodeTitleDeclaration(records);
    Resource entry{Kind::Declaration, backend().createDeclaration(declaration)};
    entry.declaration = std::move(declaration);
    ctx.r3.u64 = token(std::move(entry));
}); }
PPC_FUNC(sub_823BEE20) { call([&] {
    device(ctx.r3.u32);
    backend().bindDeclaration(resource(ctx.r4.u32, Kind::Declaration).native);
    state.declaration = ctx.r4.u32;
}); }
PPC_FUNC(sub_823BEFA8) { call([&] {
    device(ctx.r3.u32);
    // Original code programs the Xenos shader GPR split, defaulting 0/0 to
    // 64/64. D3D9 owns register allocation for its compiled shader objects;
    // these observed title configurations need no guest GPU packet or data.
    if (ctx.r4.u32 || !((ctx.r5.u32 == 32 && ctx.r6.u32 == 96) ||
        (ctx.r5.u32 == 0 && ctx.r6.u32 == 0)))
        throw std::runtime_error("Unverified original title shader register allocation policy");
}); }
PPC_FUNC(sub_823C6040) { call([&] {
    Resource* selected = nullptr;
    if (ctx.r5.u32) selected = &deviceResource(ctx.r3.u32,ctx.r5.u32,Kind::VertexBuffer);
    else device(ctx.r3.u32);
    if (!ctx.r5.u32 && ctx.r4.u32 <= 1 && !ctx.r6.u32 && !ctx.r7.u32) {
        // Original Reset8234F3F8 unbinds streams0 and1 with null resources.
        backend().unbindVertexBuffer(ctx.r4.u32);
        if (!ctx.r4.u32) state.vertexBuffer = 0;
        return;
    }
    if (ctx.r4.u32 != 0 || ctx.r6.u32 != 0)
        throw std::runtime_error("Native title vertex stream offset/multiple streams are not verified");
    if (!ctx.r5.u32) { backend().unbindVertexBuffer(); state.vertexBuffer = 0; return; }
    auto& entry = *selected;
    if (!entry.storage.ownedToken || !entry.storage.ownedData)
        throw std::runtime_error("Original borrowed AvatarPose palette is not an ordinary native vertex stream");
    const auto& declaration = resource(state.declaration, Kind::Declaration).declaration;
    if (entry.native && entry.stride != ctx.r7.u32)
        throw std::runtime_error("Original title changed the stride of a live native vertex resource");
    if (!entry.native) {
        const auto native = convertTitleVertexStream(guestBytes(entry.data, entry.bytes), ctx.r7.u32, declaration);
        entry.native = backend().createVertexBuffer(native, ctx.r7.u32);
        entry.stride = ctx.r7.u32;
        entry.dirty = false;
    }
    if (entry.dirty) {
        const auto native = convertTitleVertexStream(guestBytes(entry.data, entry.bytes), entry.stride, declaration);
        backend().updateVertexBuffer(entry.native, native);
        entry.dirty = false;
    }
    backend().bindVertexBuffer(entry.native);
    state.vertexBuffer = ctx.r5.u32;
}); }
PPC_FUNC(sub_823C61E8) { call([&] {
    if (!ctx.r4.u32) { device(ctx.r3.u32); backend().unbindIndexBuffer(); state.indexBuffer = 0; return; }
    auto& entry = deviceResource(ctx.r3.u32,ctx.r4.u32,Kind::IndexBuffer);
    if (!entry.native) {
        const auto native = convertTitleTriangleIndices(guestBytes(entry.data, entry.bytes));
        entry.native = backend().createIndexBuffer(native);
        entry.dirty = false;
    }
    if (entry.dirty) {
        const auto native = convertTitleTriangleIndices(guestBytes(entry.data, entry.bytes));
        backend().updateIndexBuffer(entry.native, native);
        entry.dirty = false;
    }
    backend().bindIndexBuffer(entry.native);
    state.indexBuffer = ctx.r4.u32;
}); }
PPC_FUNC(sub_823C7E50) { call([&] {
    const auto& vertices = deviceResource(ctx.r3.u32,state.vertexBuffer,Kind::VertexBuffer);
    const auto draw = decodeTitleIndexedDraw(ctx.r4.u32, ctx.r7.u32);
    if (!vertices.stride || ctx.r5.u32 > vertices.bytes / vertices.stride)
        throw std::runtime_error("Native title indexed draw has an invalid base vertex");
    uint32_t vertexCount = vertices.bytes / vertices.stride - ctx.r5.u32;
    std::optional<TitleBoneDraw> boneDraw;
    std::optional<TitleBatchDraw> batchDraw;
    uint32_t cjDiagnosticSerial = 0;
    std::vector<float> matrices;
    if (backend().usesTitleBonePalette()) {
        const auto& indices = resource(state.indexBuffer,Kind::IndexBuffer);
        // Original8231AF74 allocates exactly248B. Validate only when the
        // two batch registers agree; a missing/wrong-class span still takes
        // the original Bone path. No callback, write, allocation or free
        // occurs before the batch description consumes this checked span.
        std::span<const uint8_t> batch;
        if (ctx.r16.u32 == ctx.r31.u32 && memory->valid(ctx.r31.u32,248))
            batch = {memory->base() + ctx.r31.u32,248};
        if (!batch.empty() && titleBatchBEWord(batch,0) == 0x8215e29c) {
            const auto [geometry,palette] = originalBatchGeometry(ctx,vertices,indices,batch);
            matrices.resize(geometry.activeInstances * 16);
            for (size_t index = 0; index < matrices.size(); ++index) {
                const auto* bytes = palette.data() + index * 4;
                matrices[index] = std::bit_cast<float>(uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
                    uint32_t(bytes[2]) << 8 | bytes[3]);
            }
            batchDraw = TitleBatchDraw{geometry,matrices};
            vertexCount = titleBatchVertexCount(geometry);
            static uint32_t batchDiagnostics = 0;
            if (batchDiagnostics++ < 16) {
                std::fprintf(stderr,"[NATIVE RVM BATCH] owner=%08X parent=%08X instances=%u capacity=%u vertices_per_instance=%u triangles_per_instance=%u vertex_count=%u index_count=%u stride=%u constants=verified_active_prefix\n",
                    ctx.r31.u32,titleBatchBEWord(batch,192),geometry.activeInstances,geometry.capacity,
                    geometry.verticesPerInstance,geometry.trianglesPerInstance,vertexCount,titleBatchIndexCount(geometry),vertices.stride);
                std::fflush(stderr);
            }
        } else {
        const auto [geometry,palette] = originalBoneGeometry(ctx,vertices,indices,&cjDiagnosticSerial);
        matrices.resize(geometry.boneCount * 16);
        for (size_t index = 0; index < matrices.size(); ++index) {
            const auto* bytes = palette.data() + index * 4;
            matrices[index] = std::bit_cast<float>(uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
                uint32_t(bytes[2]) << 8 | bytes[3]);
        }
        boneDraw = TitleBoneDraw{geometry,matrices};
        vertexCount = geometry.vertexCount;
        }
    }
    // The diagnostic belongs to the already validated Cal3D draw. Ordinary
    // indexed draws cannot exhaust its budget and must not probe that owner.
    if (boneDraw && state.boneDrawDiagnostics < 16 && ctx.lr == 0x8234d544 && memory->valid(ctx.r31.u32,780) && memory->read32(ctx.r31.u32) == 0x82161af4 &&
        state.boneDrawDiagnostics++ < 16) {
        const uint32_t renderer = memory->read32(0x82baa92c);
        std::fprintf(stderr,
            "[NATIVE BONE DRAW] master=%08X interface=%08X interface_vtable=%08X active_bones=%u stride=%u base=%u start=%u index_count=%u\n",
            ctx.r31.u32,ctx.r16.u32,memory->valid(ctx.r16.u32,4) ? memory->read32(ctx.r16.u32) : 0,
            memory->valid(renderer,0x557e4) ? memory->read32(renderer + 0x557e0) : UINT32_MAX,
            vertices.stride,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32);
        std::fflush(stderr);
    }
    if (draw.topology == D3DPT_TRIANGLELIST)
        backend().drawIndexedTriangles(ctx.r5.s32, vertexCount, ctx.r6.u32, draw.triangles,
            boneDraw ? &*boneDraw : nullptr,batchDraw ? &*batchDraw : nullptr,cjDiagnosticSerial);
    else backend().drawIndexedTriangleStrip(ctx.r5.s32, vertexCount, ctx.r6.u32, draw.triangles);
}); }
PPC_FUNC(sub_823C79C0) { call([&] {
    const bool stackValid = Doritos::Native::memory && ctx.r1.u32 <= UINT32_MAX - 84 &&
        Doritos::Native::memory->valid(ctx.r1.u32 + 84, 4);
    const uint32_t stride = stackValid ? titleGuestWord({memory->base() + ctx.r1.u32 + 84,4},0) : 0;
    if (state.immediateDiagnostics++ < 8) {
        std::fprintf(stderr,
            "[NATIVE IMMEDIATE DRAW] lr=%08X device=%08X primitive=%u min_vertex=%u vertices=%u indices=%u index_data=%08X index_format=%u vertex_data=%08X sp=%08X stride=%u stack_valid=%u\n",
            static_cast<uint32_t>(ctx.lr),ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,
            ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,ctx.r1.u32,stride,stackValid ? 1u : 0u);
        std::fflush(stderr);
    }
    device(ctx.r3.u32);
    if (!stackValid || ctx.r9.u32 != 1 || !stride || stride > 2048)
        throw std::runtime_error("Unverified original immediate title index format or vertex stride");
    const auto draw = decodeTitleIndexedDraw(ctx.r4.u32,ctx.r7.u32);
    const uint64_t offset = uint64_t(ctx.r5.u32) * stride;
    const uint64_t vertexBytes = uint64_t(ctx.r6.u32) * stride;
    const uint64_t indexBytes = uint64_t(ctx.r7.u32) * 2;
    if (!vertexBytes || vertexBytes > 128 * 1024 * 1024 || indexBytes > 128 * 1024 * 1024 ||
        offset > UINT32_MAX || uint64_t(ctx.r10.u32) + offset > UINT32_MAX)
        throw std::runtime_error("Original immediate title geometry exceeds bounded guest byte ranges");
    const auto& declaration = resource(state.declaration,Kind::Declaration).declaration;
    const auto indices = convertTitleImmediateIndices(guestBytes(ctx.r8.u32,static_cast<uint32_t>(indexBytes)),ctx.r5.u32,ctx.r6.u32);
    const auto vertices = convertTitleVertexStream(guestBytes(ctx.r10.u32 + static_cast<uint32_t>(offset),
        static_cast<uint32_t>(vertexBytes)),stride,declaration);
    backend().drawIndexedImmediate(draw.topology,vertices,stride,indices,draw.triangles);
    state.vertexBuffer = state.indexBuffer = 0;
    // All three verified title callers discard the low return register and
    // retain their own CPU counters/reset callbacks in the original AOT code.
    ctx.r3.u64 = 0;
}); }
PPC_FUNC(sub_823BE6E8) { call([&] {
    if (!ctx.r6.u32 || ctx.r6.u32 > 1024) throw std::runtime_error("Invalid original vertex constant register count");
    const auto values = originalFloatConstants(ctx.r3.u32,ctx.r5.u32,ctx.r4.u32,ctx.r6.u32);
    backend().setGuestFloatRegisters(ShaderStage::Vertex, ctx.r4.u32, values);
}); }
PPC_FUNC(sub_823BE7C0) { call([&] {
    if (!ctx.r6.u32 || ctx.r6.u32 > 1024) throw std::runtime_error("Invalid original pixel constant register count");
    const auto values = originalFloatConstants(ctx.r3.u32,ctx.r5.u32,ctx.r4.u32,ctx.r6.u32);
    backend().setGuestFloatRegisters(ShaderStage::Pixel, ctx.r4.u32, values);
}); }
PPC_FUNC(sub_823D2188) { call([&] { device(ctx.r3.u32); backend().beginFrame(); }); }
PPC_FUNC(sub_823D2220) { call([&] { device(ctx.r3.u32); backend().endFrame(); }); }
PPC_FUNC(sub_823D2EC0) { call([&] {
    device(ctx.r3.u32);
    if (ctx.lr == 0x8234737c) { backend().presentInitialization(); return; }
    const uint64_t gamePresents = backend().counters().presents;
    struct PresentationContext {
        PPCContext* previous;
        explicit PresentationContext(PPCContext& current) : previous(state.presentationContext) {state.presentationContext=&current;}
        ~PresentationContext() {state.presentationContext=previous;}
    } presentationContext(ctx);
    backend().present();
    const uint64_t presented = backend().counters().presents;
    if (presented != gamePresents) {
        state.window->reveal();
        if (presented % 300 == 0) Doritos::Native::reportOriginalGameState();
    }
},true); }
PPC_FUNC(sub_823D2EC8) { call([&] {
    device(ctx.r3.u32);
    if (ctx.r5.u32) throw std::runtime_error("Unverified original title presentation parameters");
    backend().selectPresentationTexture(ctx.r4.u32 ? resource(ctx.r4.u32, Kind::Texture).native : 0);
}); }
PPC_FUNC(sub_823CDF10) { call([&] {
    device(ctx.r3.u32);
    const auto stackBytes = guestBytes(ctx.r1.u32 + 92,12);
    if (ctx.lr == 0x823903dc) {
        // Original non-tile RT resolve82390178. Flags0 select RT0 and allow
        // automatic MSAA averaging. +4 instead selects the depth source;
        // actual clear-color/depth gates are0x100/0x200, neither requested here.
        const uint32_t owner = ctx.r26.u32;
        const auto ownerBytes = guestBytes(owner,100);
        if (titleGuestWord(ownerBytes,4) == 1) {
            const auto target = originalShadowTarget(owner);
            validateTitleShadowEnd(target,{static_cast<uint32_t>(ctx.lr),ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,
                ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,ctx.r1.u32,
                titleGuestWord(stackBytes,0),titleGuestWord(stackBytes,8),ctx.f1.f64});
            guestBytes(ctx.r10.u32,16); // Preserved unused no-postclear RGBA input.
            backend().finishDepthTargetAlias(resource(target.texture,Kind::Texture).native,
                resource(target.depth,Kind::Surface).native,resource(target.color,Kind::Surface).native);
            return;
        }
        if (ctx.r4.u32 || ctx.r5.u32 || ctx.r7.u32 || ctx.r8.u32 || ctx.r9.u32 ||
            uint64_t(ctx.r1.u32) + 128 != ctx.r10.u32 || ctx.f1.f64 != 1.0 ||
            titleGuestWord(stackBytes,0) || titleGuestWord(stackBytes,8) ||
            titleGuestWord(ownerBytes,0) != 0x82161380 || titleGuestWord(ownerBytes,4) != 0 ||
            titleGuestWord(ownerBytes,84) != 1 || titleGuestWord(ownerBytes,88) != 999 ||
            titleGuestWord(ownerBytes,28) != ctx.r6.u32)
            throw std::runtime_error("Unverified original non-tile color RT resolve contract");
        guestBytes(ctx.r10.u32,16); // Original unused clear vector remains intact.
        const auto& texture = resource(ctx.r6.u32,Kind::Texture);
        const auto& color = resource(titleGuestWord(ownerBytes,44),Kind::Surface);
        const auto& depth = resource(titleGuestWord(ownerBytes,72),Kind::Surface);
        const auto textureDescription = backend().textureDescription(texture.native,0);
        const auto colorDescription = backend().surfaceDescription(color.native);
        const auto depthDescription = backend().surfaceDescription(depth.native);
        const uint32_t width = titleGuestWord(ownerBytes,8), height = titleGuestWord(ownerBytes,12);
        if (textureDescription.Width != width || textureDescription.Height != height ||
            colorDescription.Width != width || colorDescription.Height != height ||
            depthDescription.Width != width || depthDescription.Height != height ||
            colorDescription.Format != decodeTitleTextureFormat(titleGuestWord(ownerBytes,96)).native ||
            colorDescription.Format != textureDescription.Format || depthDescription.Format != D3DFMT_D24S8 ||
            colorDescription.MultiSampleType != D3DMULTISAMPLE_2_SAMPLES || depthDescription.MultiSampleType != D3DMULTISAMPLE_2_SAMPLES)
            throw std::runtime_error("Original RT owner and actual native resolve resources disagree");
        backend().resolveRenderTarget(texture.native,color.native,D3DMULTISAMPLE_2_SAMPLES);
        return;
    }
    // Exact full color RT0 resolve from original non-EDRAM FrameEnd8234A7E0.
    // Flag16 selects the non-MSAA sample resolve; no source rect, destination
    // point, mip, slice or post-resolve clear is requested by this path.
    if (ctx.r4.u32 != 16 || ctx.r5.u32 || ctx.r7.u32 || ctx.r8.u32 || ctx.r9.u32 || ctx.r10.u32 ||
        ctx.f1.f64 != 0.0 || titleGuestWord(stackBytes,0) || titleGuestWord(stackBytes,8))
        throw std::runtime_error("Unverified original title resolve rectangle, sample or clear parameters");
    backend().resolveRenderTarget(resource(ctx.r6.u32, Kind::Texture).native);
}); }
PPC_FUNC(sub_823D75C0) { call([&] {
    device(ctx.r3.u32);
    if (ctx.r4.u32) throw std::runtime_error("Native title renderer cannot select Xbox EDRAM tile predication");
    // Verified92-instruction mask0 path only disables Xenos GPU-internal
    // predication and dirty words. Native D3D9 full targets are unpredicated;
    // no renderer CPU outputs or guest GPU resource words are fabricated.
}); }
PPC_FUNC(sub_823D6BB8) { call([&] {
    const bool cube = ctx.r10.u32 == 18;
    if ((!cube && ctx.r10.u32 != 3) || ctx.r5.u32 != (cube ? 6u : 1u) || ctx.r7.u32 > 1)
        throw std::runtime_error("Unverified original title texture resource type, usage or depth");
    const auto format = decodeTitleTextureFormat(ctx.r8.u32);
    if (format.depth) throw std::runtime_error("Unverified original title depth texture creation");
    const uint32_t handle = backend().createTexture(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, format.native, cube, ctx.r7.u32 == 1);
    Resource entry{cube ? Kind::CubeTexture : Kind::Texture, handle};
    entry.xboxFormat = ctx.r8.u32;
    ctx.r3.u64 = token(std::move(entry));
}); }
PPC_FUNC(sub_823CFE40) { call([&] {
    device(ctx.r3.u32);
    static_assert(sizeof(D3DCAPS9) == 304);
    const auto output = guestMutableBytes(ctx.r4.u32,sizeof(D3DCAPS9));
    const auto caps = backend().capabilities();
    // Original304-byte caps layout matches D3DCAPS9. Preserve float bit patterns
    // while converting every32-bit ABI field to guest big endian.
    const auto* bytes = reinterpret_cast<const uint8_t*>(&caps);
    for (uint32_t offset = 0; offset < sizeof(caps); offset += 4) {
        uint32_t word;
        std::memcpy(&word, bytes + offset, sizeof(word));
        titleGuestWriteWord(output,offset,word);
    }
}); }
PPC_FUNC(sub_823D6968) { call([&] {
    const auto& parent = resource(ctx.r3.u32, Kind::Texture);
    const uint32_t handle = backend().textureSurface(parent.native,0,ctx.r4.u32);
    if (!handle) { ctx.r3.u64 = 0; return; }
    Resource entry{Kind::Surface,handle};
    entry.xboxFormat = parent.xboxFormat; entry.parent = ctx.r3.u32; entry.level = ctx.r4.u32;
    entry.parentKind = TitleSurfaceParent::Texture;
    ctx.r3.u64 = token(std::move(entry));
}); }
PPC_FUNC(sub_823D6A48) { call([&] {
    const auto& parent = resource(ctx.r3.u32, Kind::CubeTexture);
    Resource entry{Kind::Surface, backend().textureSurface(parent.native, ctx.r4.u32, ctx.r5.u32)};
    entry.xboxFormat = parent.xboxFormat; entry.parent = ctx.r3.u32; entry.face = ctx.r4.u32; entry.level = ctx.r5.u32;
    entry.parentKind = TitleSurfaceParent::CubeTexture;
    ctx.r3.u64 = token(std::move(entry));
}); }
PPC_FUNC(sub_823D6CD8) { call([&] {
    if (ctx.r7.u32) throw std::runtime_error("Native title surface creation requires native allocation rather than Xbox EDRAM placement");
    const auto format = decodeTitleTextureFormat(ctx.r5.u32);
    const auto samples = ctx.r6.u32 == 0 ? D3DMULTISAMPLE_NONE : ctx.r6.u32 == 1 ? D3DMULTISAMPLE_2_SAMPLES : D3DMULTISAMPLE_4_SAMPLES;
    if (ctx.r6.u32 > 2) throw std::runtime_error("Unverified original title surface multisample count");
    const uint32_t handle = backend().createSurface(ctx.r3.u32, ctx.r4.u32, format.native, format.depth, samples);
    Resource entry{Kind::Surface, handle};
    entry.xboxFormat = ctx.r5.u32;
    ctx.r3.u64 = token(std::move(entry));
}); }
PPC_FUNC(sub_823CDF60) { call([&] {
    const auto found = state.resources.find(ctx.r3.u32);
    if (found == state.resources.end()) throw std::runtime_error("Invalid native resource type query");
    switch (resource(ctx.r3.u32, found->second.kind).kind) {
    case Kind::VertexBuffer: ctx.r3.u64 = 1; break;
    case Kind::IndexBuffer: ctx.r3.u64 = 2; break;
    case Kind::Texture: ctx.r3.u64 = 3; break;
    case Kind::CubeTexture: ctx.r3.u64 = 18; break;
    case Kind::Surface: ctx.r3.u64 = 4; break;
    default: throw std::runtime_error("Unverified original title resource type query");
    }
}); }
PPC_FUNC(sub_823D6958) { call([&] {
    ctx.r3.u64 = backend().textureLevelCount(textureResource(ctx.r3.u32).native);
}); }
PPC_FUNC(sub_823D6A40) { call([&] {
    const auto& entry = textureResource(ctx.r3.u32);
    writeDescription(ctx.r5.u32, entry, backend().textureDescription(entry.native, ctx.r4.u32), entry.kind == Kind::CubeTexture ? 18 : 3);
}); }
PPC_FUNC(sub_823D6E10) { call([&] {
    const auto& entry = resource(ctx.r3.u32, Kind::Surface);
    writeDescription(ctx.r4.u32, entry, backend().surfaceDescription(entry.native),static_cast<uint32_t>(entry.parentKind));
}); }
PPC_FUNC(sub_8264A4B8) { call([&] {
    if (ctx.r4.u32 || ctx.r5.u32 || ctx.r6.u32 != 5)
        throw std::runtime_error("Unverified original title mip palette, source level or filter");
    backend().generateMipmaps(textureResource(ctx.r3.u32).native);
    ctx.r3.u64 = 0;
}); }
PPC_FUNC(sub_8264B128) { call([&] {
    device(ctx.r3.u32);
    guestBytes(ctx.r6.u32, 4);
    const uint32_t texture = createDDSResource(guestBytes(ctx.r4.u32, ctx.r5.u32), true, UINT32_MAX);
    Doritos::Native::memory->write32(ctx.r6.u32, texture);
    ctx.r3.u64 = 0;
}); }
PPC_FUNC(sub_8264B0B8) { call([&] {
    device(ctx.r3.u32);
    guestBytes(ctx.r1.u32 + 84, 52);
    auto stack = [&](uint32_t offset) { return Doritos::Native::memory->read32(ctx.r1.u32 + offset); };
    // Exact original823906A8 ordinary DDS call: default dimensions/format,
    // managed usage semantics, default filter and mip filter, no color key,
    // optional image-info or palette. Authored engine metadata callbacks remain.
    if (ctx.r6.u32 != UINT32_MAX || ctx.r7.u32 != UINT32_MAX || ctx.r9.u32 || ctx.r10.u32 != UINT32_MAX ||
        stack(84) != 1 || stack(92) != UINT32_MAX || stack(100) != UINT32_MAX || stack(108) || stack(116) || stack(124))
        throw std::runtime_error("Unverified original title DDS resizing, format, usage, filtering or output contract");
    guestBytes(stack(132), 4);
    const uint32_t texture = createDDSResource(guestBytes(ctx.r4.u32, ctx.r5.u32), false, ctx.r8.u32);
    Doritos::Native::memory->write32(stack(132), texture);
    ctx.r3.u64 = 0;
}); }
PPC_FUNC(sub_823C6FC0) { call([&] {
    device(ctx.r3.u32);
    const uint32_t offscreenOwner = ctx.lr == 0x8239009c ? originalOffscreenBegin(ctx) : 0;
    backend().bindRenderTarget(ctx.r4.u32, ctx.r5.u32 ? resource(ctx.r5.u32, Kind::Surface).native : 0);
    if (offscreenOwner) {
        // This authentic separate offscreen camera boundary does not call the
        // onscreen startFrame hook. The original CPU method/depth bind continue.
        backend().prepareFrame();
        static uint32_t shadowFrameDiagnostics = 0;
        const bool shadow = memory->read32(offscreenOwner + 4) == 1;
        if ((shadow ? shadowFrameDiagnostics++ : state.offscreenFrameDiagnostics++) < 16) {
            std::fprintf(stderr,
                "[NATIVE OFFSCREEN FRAME] lr=%08X owner=%08X type=%u color=%08X depth=%08X size=%ux%u samples=%u prepared_after_actual_color_bind=1\n",
                static_cast<uint32_t>(ctx.lr),offscreenOwner,memory->read32(offscreenOwner + 4),ctx.r5.u32,memory->read32(offscreenOwner + 72),
                memory->read32(offscreenOwner + 8),memory->read32(offscreenOwner + 12),1u << memory->read32(offscreenOwner + 84));
            std::fflush(stderr);
        }
    }
}); }
PPC_FUNC(sub_823C6CE8) { call([&] {
    device(ctx.r3.u32);
    backend().bindDepthSurface(ctx.r4.u32 ? resource(ctx.r4.u32, Kind::Surface).native : 0);
}); }
PPC_FUNC(sub_823D6F38) { call([&] {
    uint32_t handle = 0;
    if (ctx.r5.u32) {
        const auto found = state.resources.find(ctx.r5.u32);
        if (found == state.resources.end() || (found->second.kind != Kind::Texture && found->second.kind != Kind::CubeTexture))
            throw std::runtime_error("Invalid original title native sampler texture");
        handle = deviceResource(ctx.r3.u32,ctx.r5.u32,found->second.kind).native;
    } else device(ctx.r3.u32);
    // r6 is the original fetch-state dirty mask. Native SetTexture performs
    // the corresponding resource state binding without guest fetch packets.
    backend().bindTexture(ctx.r4.u32, handle);
}); }
PPC_FUNC(sub_8234FAB8) {
    const uint32_t renderer = ctx.r3.u32, sampler = ctx.r4.u32, master = ctx.r5.u32;
    __imp__sub_8234FAB8(ctx, base);
    if (!ctx.r3.u32 || sampler >= 16 || !master) return;
    call([&] {
        // Tag after the original master binding, including its CPU texture
        // cache-hit path. That path may omit the low-level SetTexture hook.
        if (!memory || base != memory->base() || !memory->valid(master,176)) return;
        const std::span<const uint8_t> masterBytes{base + master,176};
        if (titleGuestWord(masterBytes,0) != 0x82160afc) return;
        const uint32_t name = titleGuestWord(masterBytes,8);
        if (!name || !memory->valid(name,256)) return;
        const char* text = reinterpret_cast<const char*>(base + name);
        const char* end = static_cast<const char*>(std::memchr(text,0,256));
        if (!end) return;
        const auto prompt = titleInputPrompt({text,static_cast<size_t>(end-text)});
        const bool guiScreen = std::string_view(text,static_cast<size_t>(end-text)) == "rtt/guiScreens";
        if (prompt.kind == InputPromptKind::None && !guiScreen) return;
        // Mirror original 822AF070's direct/array texture selection and
        // 823463C0's GPU token precedence, without changing either object.
        uint32_t texture = titleGuestWord(masterBytes,136);
        if (!texture) {
            const uint32_t array = titleGuestWord(masterBytes,140);
            const uint32_t index = titleGuestWord(masterBytes,144);
            const uint64_t element = uint64_t(array) + uint64_t(index)*4;
            if (!array || element > UINT32_MAX || !memory->valid(uint32_t(element),4)) return;
            texture = titleGuestWord({base + element,4},0);
        }
        const uint64_t cache = uint64_t(renderer) + 0x557f4 + uint64_t(sampler)*4;
        if (!texture || !memory->valid(texture,44) || cache > UINT32_MAX ||
            !memory->valid(uint32_t(cache),4) || titleGuestWord({base + cache,4},0) != texture) return;
        const std::span<const uint8_t> textureBytes{base + texture,44};
        uint32_t token = 0;
        for (const uint32_t offset : {32u,36u,40u,28u}) {
            token = titleGuestWord(textureBytes,offset);
            if (token) break;
        }
        const auto found = state.resources.find(token);
        if (found == state.resources.end() || found->second.kind != Kind::Texture) return;
        const auto native = resource(token,Kind::Texture).native;
        if (guiScreen) backend().setGuiScreenTexture(native);
        if (prompt.kind != InputPromptKind::None) backend().setInputPrompt(native,prompt.kind,prompt.alphaMask);
    });
}
PPC_FUNC(sub_82648F60) { call([&] {
    const auto surface = resource(ctx.r3.u32, Kind::Surface).native;
    const auto description = backend().surfaceDescription(surface);
    const auto stackBytes = guestBytes(ctx.r1.u32 + 84,36);
    if (ctx.r4.u32 || ctx.r5.u32 || ctx.r9.u32 || !ctx.r10.u32 ||
        titleGuestWord(stackBytes,0) || titleGuestWord(stackBytes,8) ||
        titleGuestWord(stackBytes,16) || titleGuestWord(stackBytes,24) != 1 ||
        titleGuestWord(stackBytes,32))
        throw std::runtime_error("Unverified original title texture upload palette, filtering, resampling or color key");
    const auto rectangle = decodeTitleTextureRect(guestBytes(ctx.r10.u32, 16));
    if (rectangle.left || rectangle.top || rectangle.right != description.Width || rectangle.bottom != description.Height)
        throw std::runtime_error("Unverified original title partial texture upload");
    const auto format = decodeTitleTextureFormat(ctx.r7.u32);
    const uint32_t rows = (description.Height + format.blockHeight - 1) / format.blockHeight;
    const uint64_t sourceLength = uint64_t(ctx.r8.u32) * rows;
    if (sourceLength > 128 * 1024 * 1024) throw std::runtime_error("Original texture upload exceeds native adapter limit");
    const auto native = convertTitleLinearTexture(guestBytes(ctx.r6.u32, static_cast<uint32_t>(sourceLength)),
        description.Width, description.Height, ctx.r8.u32, ctx.r7.u32);
    backend().uploadSurface(surface, native.format, native.rowPitch, native.bytes);
    ctx.r3.u64 = 0;
}); }
PPC_FUNC(sub_823C6868) { call([&] {
    device(ctx.r3.u32);
    const auto bytes = guestBytes(ctx.r4.u32,24);
    D3DVIEWPORT9 viewport{titleGuestWord(bytes,0),titleGuestWord(bytes,4),titleGuestWord(bytes,8),titleGuestWord(bytes,12),
        std::bit_cast<float>(titleGuestWord(bytes,16)),std::bit_cast<float>(titleGuestWord(bytes,20))};
    backend().setViewport(viewport);
}); }
PPC_FUNC(sub_823C63D0) { call([&] {
    device(ctx.r3.u32);
    guestBytes(ctx.r4.u32, sizeof(D3DGAMMARAMP));
    D3DGAMMARAMP ramp{};
    backend().getGammaRamp(ramp);
    const uint16_t* values = reinterpret_cast<const uint16_t*>(&ramp);
    auto* destination = Doritos::Native::memory->base() + ctx.r4.u32;
    for (uint32_t index = 0; index < sizeof(ramp) / 2; ++index) {
        destination[index * 2] = uint8_t(values[index] >> 8);
        destination[index * 2 + 1] = uint8_t(values[index]);
    }
}); }
PPC_FUNC(sub_823C6300) { call([&] {
    device(ctx.r3.u32);
    // Original setter immediately moves ramp r5 into r4 and ignores incoming
    // flags r4. The getter instead receives its output ramp in r4.
    const auto bytes = guestBytes(ctx.r5.u32, sizeof(D3DGAMMARAMP));
    D3DGAMMARAMP ramp{};
    auto* values = reinterpret_cast<uint16_t*>(&ramp);
    for (uint32_t index = 0; index < sizeof(ramp) / 2; ++index)
        values[index] = uint16_t(bytes[index * 2]) << 8 | bytes[index * 2 + 1];
    backend().setGammaRamp(ramp);
}); }
PPC_FUNC(sub_823CF280) { call([&] {
    const uint32_t address = ctx.r3.u32;
    const auto found = state.resources.find(address);
    if (found == state.resources.end() || found->second.kind == Kind::Device)
        throw std::runtime_error("Invalid or unverified original title native resource release");
    const auto entry = found->second;
    if (entry.native) backend().release(entry.native);
    if (entry.data && entry.storage.ownedData) Doritos::Native::memory->release(entry.data);
    if (entry.storage.ownedToken) Doritos::Native::memory->release(address);
    state.resources.erase(found);
}); }
PPC_FUNC(sub_823BCCF8) { call([&] {
    device(ctx.r3.u32);
    const uint32_t mask = ctx.r6.u32;
    if ((mask & ~63u) || ((mask & 15) && (mask & 15) != 15) || ctx.r10.u32)
        throw std::runtime_error("Unverified original title partial-channel clear or clear parameters");
    const uint32_t flags = (mask & 15 ? D3DCLEAR_TARGET : 0) | (mask & 16 ? D3DCLEAR_ZBUFFER : 0) | (mask & 32 ? D3DCLEAR_STENCIL : 0);
    if (!flags) return;
    std::vector<D3DRECT> rectangles;
    if (ctx.r5.u32) {
        if (ctx.r4.u32 > 4096) throw std::runtime_error("Original title clear rectangle count exceeds native bounds");
        if (!ctx.r4.u32) return; // Original nonnull rectangle pointer with count0 clears nothing.
        const auto rectangleBytes = guestBytes(ctx.r5.u32,ctx.r4.u32 * 16);
        for (uint32_t index = 0; index < ctx.r4.u32; ++index) {
            const auto rectangle = decodeTitleTextureRect(rectangleBytes.subspan(index * 16,16));
            rectangles.push_back({static_cast<LONG>(rectangle.left), static_cast<LONG>(rectangle.top), static_cast<LONG>(rectangle.right), static_cast<LONG>(rectangle.bottom)});
        }
    }
    backend().clearRectangles(rectangles, flags, ctx.r7.u32, static_cast<float>(ctx.f1.f64), ctx.r9.u32,
        static_cast<uint32_t>(ctx.lr));
}); }
PPC_FUNC(sub_8234AF50) { call([&] {
    const uint32_t renderer = ctx.r3.u32;
    const bool reverse = guestBytes(renderer + 0x56b94, 1)[0] != 0;
    backend().setRenderState(D3DRS_ZFUNC, reverse ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL);
    D3DVIEWPORT9 viewport{ctx.r6.u32, ctx.r7.u32, ctx.r4.u32, ctx.r5.u32,
        static_cast<float>(reverse ? ctx.f2.f64 : ctx.f1.f64), static_cast<float>(reverse ? ctx.f1.f64 : ctx.f2.f64)};
    backend().setViewport(viewport);
}); }
PPC_FUNC(sub_82377330) { call([&] {
    const uint32_t renderer = ctx.r3.u32;
    const auto bytes = guestMutableBytes(renderer + 0x56550,4);
    const uint32_t enabled = ctx.r4.u8 != 0;
    backend().setRenderState(D3DRS_ZENABLE, enabled ? D3DZB_TRUE : D3DZB_FALSE);
    titleGuestWriteWord(bytes,0,enabled);
}); }
PPC_FUNC(sub_82378B20) { call([&] {
    const uint32_t renderer = ctx.r3.u32;
    const auto bytes = guestMutableBytes(renderer + 0x56554,4);
    const uint32_t enabled = ctx.r4.u8 != 0;
    backend().setRenderState(D3DRS_ZWRITEENABLE, enabled);
    titleGuestWriteWord(bytes,0,enabled);
}); }
PPC_FUNC(sub_82374F00) { call([&] {
    const uint32_t renderer = ctx.r3.u32;
    const uint32_t cameraAddress = titleRendererFieldAddress(renderer,titleRendererCullCameraOffset,4);
    const uint32_t modeAddress = titleRendererFieldAddress(renderer,titleRendererCullModeOffset,4);
    if (!memory || !memory->validPair(cameraAddress,4,modeAddress,4))
        throw std::runtime_error("Native graphics guest byte range is inaccessible");
    const std::span<const uint8_t> cameraBytes{memory->base() + cameraAddress,4};
    const std::span<uint8_t> modeBytes{memory->base() + modeAddress,4};
    uint32_t mode = ctx.r4.u32;
    if (mode > 2) throw std::runtime_error("Unverified original title native culling mode");
    const uint32_t camera = titleGuestWord(cameraBytes,0);
    if (camera) {
        PPCContext reflection = ctx;
        reflection.r3.u64 = camera;
        // Original822AC6F8 only reads camera+120 and returns; no renderer
        // mutation, guest callback or allocation invalidates these fields.
        sub_822AC6F8(reflection, base);
        if (reflection.r3.u32 == 2 && mode) mode = 3 - mode;
    }
    backend().setRenderState(D3DRS_CULLMODE, mode == 0 ? D3DCULL_NONE : mode == 1 ? D3DCULL_CW : D3DCULL_CCW);
    titleGuestWriteWord(modeBytes,0,mode);
}); }
PPC_FUNC(sub_82359498) { call([&] {
    const uint32_t renderer = ctx.r3.u32, mode = ctx.r4.u32;
    const auto modeBytes = guestMutableBytes(renderer + 0x56548,4);
    if (!mode || mode > 6) throw std::runtime_error("Unverified original title native blend mode");
    // Exact selected SDK state values from the original inline switch tables:
    // modes1..6: (enable,src,dst) = (0,1,0),(1,6,7),(0,1,0),
    // (1,8,0),(1,1,1),(1,6,1). Native D3D9 uses different enum values.
    constexpr D3DBLEND source[] = {D3DBLEND_ONE,D3DBLEND_ONE,D3DBLEND_SRCALPHA,D3DBLEND_ONE,D3DBLEND_DESTCOLOR,D3DBLEND_ONE,D3DBLEND_SRCALPHA};
    constexpr D3DBLEND destination[] = {D3DBLEND_ZERO,D3DBLEND_ZERO,D3DBLEND_INVSRCALPHA,D3DBLEND_ZERO,D3DBLEND_ZERO,D3DBLEND_ONE,D3DBLEND_ONE};
    backend().setRenderState(D3DRS_ALPHABLENDENABLE, mode != 1 && mode != 3);
    backend().setRenderState(D3DRS_SRCBLEND, source[mode]);
    backend().setRenderState(D3DRS_DESTBLEND, destination[mode]);
    titleGuestWriteWord(modeBytes,0,mode);
}); }
PPC_FUNC(sub_8234FE28) { call([&] {
    guestBytes(ctx.r3.u32, 4);
    const uint32_t sampler = ctx.r4.u32, mode = ctx.r5.u32;
    if (sampler >= 16 || mode > 4) throw std::runtime_error("Unverified original title native sampler filter mode");
    constexpr D3DTEXTUREFILTERTYPE min[] = {D3DTEXF_POINT,D3DTEXF_POINT,D3DTEXF_LINEAR,D3DTEXF_LINEAR,D3DTEXF_ANISOTROPIC};
    constexpr D3DTEXTUREFILTERTYPE mag[] = {D3DTEXF_POINT,D3DTEXF_LINEAR,D3DTEXF_LINEAR,D3DTEXF_LINEAR,D3DTEXF_LINEAR};
    constexpr D3DTEXTUREFILTERTYPE mip[] = {D3DTEXF_NONE,D3DTEXF_POINT,D3DTEXF_POINT,D3DTEXF_LINEAR,D3DTEXF_LINEAR};
    backend().setSamplerState(sampler, D3DSAMP_MINFILTER, min[mode]);
    backend().setSamplerState(sampler, D3DSAMP_MAGFILTER, mag[mode]);
    backend().setSamplerState(sampler, D3DSAMP_MIPFILTER, mip[mode]);
    if (mode == 4) backend().setSamplerState(sampler, D3DSAMP_MAXANISOTROPY, 8);
}); }
PPC_FUNC(sub_82371F60) { call([&] {
    const uint32_t renderer = ctx.r3.u32;
    const auto referenceBytes = guestMutableBytes(renderer + 0x56b80,4);
    const int32_t reference = ctx.r4.s32;
    if (reference >= 0) {
        const uint32_t clamped = std::min<uint32_t>(static_cast<uint32_t>(reference), 255);
        backend().setRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        // Original selected Xbox SDK comparison4 is GREATER; native enum is5.
        backend().setRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        backend().setRenderState(D3DRS_ALPHAREF, clamped);
        titleGuestWriteWord(referenceBytes,0,clamped);
    } else {
        backend().setRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        titleGuestWriteWord(referenceBytes,0,static_cast<uint32_t>(reference));
    }
}); }
PPC_FUNC(sub_8237A5E8) { call([&] {
    guestBytes(ctx.r3.u32, 4);
    const uint32_t sampler = ctx.r4.u32, addressU = ctx.r5.u32, addressV = ctx.r6.u32;
    if (sampler >= 16 || addressU > 3 || addressV > 3)
        throw std::runtime_error("Unverified original title native sampler addressing mode");
    // Verified original table820F3C6C is0,1,2,6: wrap,mirror,clamp,border.
    constexpr D3DTEXTUREADDRESS address[] = {D3DTADDRESS_WRAP,D3DTADDRESS_MIRROR,D3DTADDRESS_CLAMP,D3DTADDRESS_BORDER};
    backend().setSamplerState(sampler, D3DSAMP_ADDRESSU, address[addressU]);
    backend().setSamplerState(sampler, D3DSAMP_ADDRESSV, address[addressV]);
    if (addressU == 3 || addressV == 3) {
        const auto rgb = guestBytes(ctx.r7.u32, 3);
        // The original API reduces its RGB input to transparent black or white.
        backend().setSamplerState(sampler, D3DSAMP_BORDERCOLOR, (rgb[0] | rgb[1] | rgb[2]) ? 0xffffffffu : 0u);
    }
}); }
PPC_FUNC(sub_823C4770) { call([&] {
    device(ctx.r3.u32);
    ctx.r3.u64 = backend().renderState(D3DRS_CLIPPLANEENABLE) & 63;
}); }
PPC_FUNC(sub_823C4728) { call([&] {
    device(ctx.r3.u32);
    if (ctx.r4.u32 & ~63u) throw std::runtime_error("Unverified original title clip-plane mask");
    backend().setRenderState(D3DRS_CLIPPLANEENABLE, ctx.r4.u32);
}); }
PPC_FUNC(sub_823C64B8) { call([&] {
    device(ctx.r3.u32);
    const auto bytes = guestBytes(ctx.r5.u32,16);
    std::array<float, 4> plane;
    for (uint32_t index = 0; index < plane.size(); ++index)
        plane[index] = std::bit_cast<float>(titleGuestWord(bytes,index * 4));
    backend().setClipPlane(ctx.r4.u32, plane);
}); }
