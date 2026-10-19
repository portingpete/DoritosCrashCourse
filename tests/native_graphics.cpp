#include "renderer/native/shader_cache.h"
#include "renderer/native/title_vertex.h"
#include "renderer/native/indexed_draw.h"
#include "renderer/native/geometry_validation_cache.h"
#include "renderer/native/recent_frame_times.h"
#include "renderer/native/capture_request.h"
#include "renderer/native/inline_buffer.h"
#include "renderer/native/bone_palette.h"
#include "renderer/native/batch_palette.h"
#include "renderer/native/phase_times.h"
#include "renderer/native/title_constants.h"
#include "renderer/native/depth_target.h"
#include "renderer/native/shadow_target.h"
#include "renderer/native/guest_span.h"
#include "renderer/native/title_renderer_span.h"
#include "renderer/native/source_shader.h"
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <limits>
#include <stdexcept>
#include <tuple>

using namespace Doritos::Graphics;
namespace {
size_t checks = 0;
void require(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
void rejects(const std::function<void()>& action, const char* label) {
    ++checks;
    try { action(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error(label);
}
void append(std::vector<uint8_t>& bytes, uint32_t value, bool bigEndian = false) {
    for (int index = 0; index < 4; ++index) bytes.push_back(static_cast<uint8_t>(value >> ((bigEndian ? 3 - index : index) * 8)));
}
std::vector<uint8_t> cache(bool bigEndian = false, bool duplicateKey = false, std::string name = "fixture/vs") {
    std::vector<uint8_t> bytes;
    append(bytes, 3, bigEndian); append(bytes, 0, bigEndian); append(bytes, 1, bigEndian);
    append(bytes, static_cast<uint32_t>(name.size()+1), bigEndian);
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.push_back(0);
    append(bytes, duplicateKey ? 2 : 1, bigEndian);
    for (int index = 0; index < (duplicateKey ? 2 : 1); ++index) {
        append(bytes, 0, bigEndian); append(bytes, 0, bigEndian); append(bytes, 4, bigEndian);
        append(bytes, 1, bigEndian); append(bytes, 7, bigEndian); append(bytes, 0, bigEndian);
        append(bytes, 4, bigEndian); append(bytes, 0, bigEndian);
        append(bytes, 8, bigEndian);
        append(bytes, 0xfffe0300, false); append(bytes, 0x0000ffff, false);
    }
    return bytes;
}
struct DepthEntry {
    uint32_t native, parent = 0;
    TitleSurfaceParent parentKind = TitleSurfaceParent::None;
    TitleBufferStorage storage;
};
struct DepthRegistry {
    std::map<uint32_t,DepthEntry> entries;
    uint32_t insertions = 0, failInsertion = 0;
    auto emplace(uint32_t address, DepthEntry entry) {
        if (++insertions == failInsertion) throw std::bad_alloc();
        return entries.emplace(address,std::move(entry));
    }
    void erase(uint32_t address) { entries.erase(address); }
};
void verifyDepthPairTransactions() {
    // Exercise the actual publication/rollback templates used by the bridge,
    // including failure after its first registry insertion has succeeded.
    for (uint32_t failure = 0; failure <= 9; ++failure) {
        DepthRegistry registry;
        if (failure == 8 || failure == 9) registry.failInsertion = failure - 7;
        uint32_t nativeCreates = 0, tokenAllocations = 0, writes = 0;
        std::vector<uint32_t> nativeFreed, tokenFreed;
        bool threw = false;
        TitleDepthTargetResources pair;
        try {
            pair = allocateTitleDepthTargetPair(
                [&]() -> uint32_t {
                    ++nativeCreates;
                    if (failure == 1) return 0; // Actual nullable CreateTexture.
                    if (failure == 2) throw std::runtime_error("device failure");
                    return 10;
                },
                [&](uint32_t texture) -> uint32_t {
                    require(texture == 10,"Depth view creation lost the actual texture handle");
                    ++nativeCreates;
                    if (failure == 3) return 0; // Actual nullable GetSurfaceLevel.
                    if (failure == 4) throw std::runtime_error("view failure");
                    return 20;
                },
                [&](uint32_t texture, uint32_t view) {
                    return publishTitleDepthTargetPair(registry,DepthEntry{texture},
                        [&](uint32_t parent) { return DepthEntry{view,parent,TitleSurfaceParent::Texture}; },
                        [&]() -> uint32_t {
                            ++tokenAllocations;
                            if (failure == 5 && tokenAllocations == 2) throw std::runtime_error("token allocation failure");
                            return 100 + tokenAllocations;
                        },
                        [&](uint32_t, const DepthEntry&) {
                            ++writes;
                            if ((failure == 6 && writes == 1) || (failure == 7 && writes == 2))
                                throw std::runtime_error("token initialization failure");
                        },
                        [&](uint32_t token) { tokenFreed.push_back(token); });
                },
                [&](uint32_t native) { nativeFreed.push_back(native); });
        } catch (const std::runtime_error&) { threw = true; }
        if (!failure) {
            require(!threw && pair.texture == 101 && pair.depthSurface == 102 && registry.entries.size() == 2,
                "Depth resource pair was not atomically published");
            require(nativeFreed.empty() && tokenFreed.empty(),"Successful depth publication released its owned resources");
            require(registry.entries.at(102).parent == 101 && registry.entries.at(102).parentKind == TitleSurfaceParent::Texture &&
                registry.entries.at(101).storage.ownedToken && registry.entries.at(102).storage.ownedToken,
                "Depth view lost immutable parent-kind or independent token ownership");
            registry.erase(101);
            require(registry.entries.at(102).parentKind == TitleSurfaceParent::Texture,
                "Surviving view description depended on a released parent token");
            continue;
        }
        require(registry.entries.empty(),"Partial depth publication stranded a registry entry");
        require(!pair.texture && !pair.depthSurface,"Failed depth pair exposed a partial native resource");
        require(threw == (failure == 2 || failure == 4 || failure == 5 || failure == 6 || failure == 7),
            "Depth allocation confused nullable exhaustion with invalid contract/device errors");
        const auto expectedNative = failure <= 2 ? std::vector<uint32_t>{} : failure <= 4 ?
            std::vector<uint32_t>{10} : std::vector<uint32_t>{20,10};
        require(nativeFreed == expectedNative,"Failed depth allocation did not release view before texture exactly once");
        const auto expectedTokens = failure <= 4 ? std::vector<uint32_t>{} : failure <= 6 ?
            std::vector<uint32_t>{101} : std::vector<uint32_t>{102,101};
        require(tokenFreed == expectedTokens,"Partial token publication leaked or double-freed owned token storage");
        require(nativeCreates == (failure <= 2 ? 1u : 2u),"Nullable depth creation continued to an invalid later stage");
    }
}
}
void verifyRendererOwnedSpans() {
    constexpr uint32_t address = 0x12000001; // Both guest owner and host bytes may be unaligned.
    require(titleRendererFieldAddress(address,titleRendererCullCameraOffset,4) == address + 0x11950 &&
        titleRendererFieldAddress(address,titleRendererCullModeOffset,4) == address + 0x5654c,
        "Sparse cull fields retain their exact original addresses");
    require(titleRendererFieldAddress(0x10000,0,4) == 0x10000 &&
        titleRendererFieldAddress(uint32_t((uint64_t(1) << 32) - titleRendererOwnedBytes),titleRendererOwnedBytes - 4,4) == 0xfffffffc,
        "Sparse renderer fields accept exact minimum and four-GiB endpoint");
    for (const auto [owner,offset,count] : std::array<std::array<uint32_t,3>,7>{{
        {0,0,4},{0xffff,0,4},{0xffffffff,0,4},{0xfffffffc,0,4},
        {address,0,0},{address,titleRendererOwnedBytes - 3,4},{address,UINT32_MAX,4}}})
        rejects([&] { titleRendererFieldAddress(owner,offset,count); },
            "Sparse renderer field escaped full logical owner bounds");
    std::vector<uint8_t> storage(titleRendererOwnedBytes + 2,0x5a);
    const auto owner = std::span(storage).subspan(1,titleRendererOwnedBytes);
    const uint64_t handle = 0x123456780000005aull | (uint64_t(4999) << 16);
    for (uint32_t registry : {0x11b90u,0x31778u}) {
        const uint32_t entryAddress = titleBatchRegistryEntry(address,registry,handle);
        const uint32_t offset = entryAddress - address;
        titleGuestWriteWord(owner,offset + 8,0x13000001);
        titleGuestWriteWord(owner,offset + 16,uint32_t(handle >> 32));
        titleGuestWriteWord(owner,offset + 20,uint32_t(handle));
        const auto entry = titleRendererOwnedRange(owner,address,entryAddress,24);
        require(entry.data() == owner.data() + registry + 4999 * 24 && entry.size() == 24 &&
            titleBatchRegisteredObject(entry,handle) == 0x13000001,
            "Highest actual inline renderer registry slot lost full handle/pointee identity");
        rejects([&] { titleBatchRegisteredObject(entry,handle ^ (uint64_t(1) << 32)); },
            "Renderer subspan authorized a different full64 handle generation");
    }
    titleGuestWriteWord(owner,titleRendererPaletteOffset,0x80000000);
    titleGuestWriteWord(owner,titleRendererPaletteOffset + 48 * 64 - 4,0x7fc12345);
    const auto before = storage;
    const auto batch = titleRendererPalette(owner,address,32,32);
    const auto bone = titleRendererPalette(owner,address,48,48);
    require(batch.size() == 32 * 64 && bone.size() == 48 * 64 &&
        batch.data() == owner.data() + titleRendererPaletteOffset && batch.data() == bone.data() &&
        titleGuestWord(bone,0) == 0x80000000 && titleGuestWord(bone,bone.size() - 4) == 0x7fc12345,
        "Owned32/48-matrix renderer palette changed exact bytes, offsets or float bits");
    for (const auto [matrices,limit] : std::array<std::pair<uint32_t,uint32_t>,6>{
        {{0,32},{33,32},{0,48},{49,48},{UINT32_MAX,48},{1,UINT32_MAX}}})
        rejects([&] { titleRendererPalette(owner,address,matrices,limit); },
            "Unbounded or wrong-producer renderer palette accepted");
    rejects([&] { titleRendererPalette(owner.first(owner.size() - 1),address,48,48); },
        "Truncated renderer owner authorized an inline palette");
    rejects([&] { titleRendererOwnedRange(owner,address,address - 1,1); },
        "Inline renderer range preceding its owner accepted");
    rejects([&] { titleRendererOwnedRange(owner,address,uint64_t(address) + owner.size() - 23,24); },
        "Inline registry range crossing renderer owner accepted");
    rejects([&] { titleRendererOwnedRange(owner,address,address,0); },
        "Empty renderer inline range accepted");
    rejects([&] { titleRendererOwnedRange(owner,0xfffffff0,0xfffffff0,4); },
        "Renderer owner wrapping4GB authorized a small inline read");
    rejects([&] { titleRendererOwnedRange(owner,address,uint64_t(1) << 32,4); },
        "Wrapped inline address accepted");
    require(titleGuestOwnedRange(owner,address,uint64_t(address) + owner.size() - 4,4).data() ==
        owner.data() + owner.size() - 4,"Exact renderer owner endpoint rejected");
    std::array<uint8_t,4> lastWord{0x80,0,0,0};
    require(titleGuestWord(titleGuestOwnedRange(lastWord,0xfffffffc,0xfffffffc,4),0) == 0x80000000,
        "Exact4GB guest owner endpoint rejected");
    rejects([&] { titleGuestOwnedRange(owner,address,address,SIZE_MAX); },
        "Overflow inline range length accepted");
    require(storage == before && storage.front() == 0x5a && storage.back() == 0x5a,
        "Owned renderer reads or rejected ranges mutated guest bytes/sentinels");
    titleGuestWriteWord(owner,titleRendererCullCameraOffset,0x81234567);
    const auto beforeCull = storage;
    const auto cullPrefix = owner.first(titleRendererCullPrefixBytes);
    require(titleGuestWord(cullPrefix,titleRendererCullCameraOffset) == 0x81234567,
        "Cull prefix changed its unaligned original camera pointer bits");
    titleGuestWriteWord(cullPrefix,titleRendererCullModeOffset,2);
    bool onlyCullModeChanged = true;
    for (size_t index = 0; index < storage.size(); ++index)
        if (index < titleRendererCullModeOffset + 1 || index >= titleRendererCullModeOffset + 5)
            onlyCullModeChanged = onlyCullModeChanged && storage[index] == beforeCull[index];
    require(onlyCullModeChanged && titleGuestWord(cullPrefix,titleRendererCullModeOffset) == 2,
        "Cull prefix write changed camera, other renderer fields or neighboring sentinels");
    const auto beforeTruncatedCull = storage;
    rejects([&] { titleGuestWriteWord(cullPrefix.first(cullPrefix.size() - 1),titleRendererCullModeOffset,1); },
        "Truncated cull mode word accepted inside renderer prefix");
    require(storage == beforeTruncatedCull,"Rejected cull mode write partially mutated renderer bytes");
}
void verifyDrawLocalVertexProofs() {
    // Repeated indices do not authorize a later draw to reuse an earlier
    // vertex proof, and all four lanes matter even when their weight is zero.
    const TitleBatchGeometry batch{2,2,2,2};
    std::vector<uint8_t> batchVertices(4 * 84);
    std::vector<uint16_t> batchIndices{0,1,0,1,0,1,2,3,2,3,2,3};
    const auto batchFloat = [&](uint32_t vertex, uint32_t offset, float value) {
        std::memcpy(batchVertices.data() + vertex * 84 + offset,&value,4);
    };
    for (uint32_t vertex = 0; vertex < 4; ++vertex) {
        batchFloat(vertex,12,1);
        batchFloat(vertex,28,float(vertex / 2));
    }
    validateTitleBatchGeometry(batch,batchVertices,batchIndices);
    for (uint32_t vertex = 0; vertex < 4; ++vertex) {
        batchFloat(vertex,40,std::numeric_limits<float>::quiet_NaN());
        rejects([&] { validateTitleBatchGeometry(batch,batchVertices,batchIndices); },
            "Previous RVMBatch draw proof hid a changed repeated zero-weight lane");
        batchFloat(vertex,40,0);
        validateTitleBatchGeometry(batch,batchVertices,batchIndices);
    }
    batchIndices[9] = 1; // Already referenced, but wrong replicated instance.
    rejects([&] { validateTitleBatchGeometry(batch,batchVertices,batchIndices); },
        "Repeated already-checked RVMBatch vertex bypassed later index replication");
    batchIndices[9] = 3;
    validateTitleBatchGeometry(batch,batchVertices,batchIndices);

    // The full uint16 domain is relative to baseVertex, not an absolute
    // bitset offset. This draw reads vertex65536 through relative65535.
    const TitleBoneGeometry bone{65537,1,1,65536,0,3,2};
    std::vector<uint8_t> boneVertices(size_t(bone.totalVertices) * 80);
    const std::array<uint16_t,3> boneIndices{UINT16_MAX,UINT16_MAX,UINT16_MAX};
    const auto boneFloat = [&](float value) {
        std::memcpy(boneVertices.data() + size_t(65536) * 80 + 40,&value,4);
    };
    validateTitleBoneGeometry(bone,boneVertices,boneIndices);
    for (float bad : {2.0f,std::numeric_limits<float>::quiet_NaN()}) {
        boneFloat(bad);
        rejects([&] { validateTitleBoneGeometry(bone,boneVertices,boneIndices); },
            "Previous Cal3D draw proof hid a changed repeated relative65535 lane");
        boneFloat(0);
        validateTitleBoneGeometry(bone,boneVertices,boneIndices);
    }
}
void verifyRvmBatchContract() {
    const TitleBatchGeometry actual{5693,684,6,3};
    require(titleBatchVertexCount(actual) == 17079 && titleBatchIndexCount(actual) == 6156,"Actual RVMBatch arithmetic changed");
    std::vector<uint8_t> vertices(34158 * 84);
    std::vector<uint16_t> indices(4104 * 3);
    const auto setFloat = [&](uint32_t vertex, uint32_t offset, float value) { std::memcpy(vertices.data() + size_t(vertex) * 84 + offset,&value,4); };
    for (uint32_t ordinal = 0; ordinal < actual.capacity; ++ordinal) {
        for (uint32_t vertex = 0; vertex < actual.verticesPerInstance; ++vertex) {
            setFloat(ordinal * actual.verticesPerInstance + vertex,12,1);
            setFloat(ordinal * actual.verticesPerInstance + vertex,28,float(ordinal));
        }
        for (uint32_t index = 0; index < actual.trianglesPerInstance * 3; ++index)
            indices[ordinal * actual.trianglesPerInstance * 3 + index] = static_cast<uint16_t>(index + ordinal * actual.verticesPerInstance);
    }
    validateTitleBatchGeometry(actual,vertices,indices);
    require(vertices.size() == 2869272 && indices.size() * 2 == 24624,"Actual owned RVMBatch buffer sizes changed");
    // Capacity tail has no authority over an actual active-prefix draw.
    setFloat(3 * actual.verticesPerInstance,28,std::numeric_limits<float>::quiet_NaN());
    indices[actual.trianglesPerInstance * actual.activeInstances * 3] = 65535;
    validateTitleBatchGeometry(actual,vertices,indices); require(true,"Inactive RVMBatch capacity tail was read");
    for (auto bad : {TitleBatchGeometry{0,684,6,3},{5693,0,6,3},{5693,684,0,3},{1,1,33,3},{5693,684,6,0},{5693,684,6,7}})
        rejects([&] { titleBatchVertexCount(bad); },"Invalid RVMBatch capacity/active count accepted");
    require(titleBatchVertexCount({64000,1,1,1}) == 64000 && titleBatchIndexCount({1,21333,1,1}) == 63999,"Exact 16-bit RVMBatch bounds rejected");
    rejects([&] { titleBatchVertexCount({64001,1,1,1}); },"64001 RVMBatch vertices accepted");
    rejects([&] { titleBatchIndexCount({1,21334,1,1}); },"64002 RVMBatch indices accepted");
    const uint16_t savedIndex = indices[2052]; indices[2052] = 1;
    rejects([&] { validateTitleBatchGeometry(actual,vertices,indices); },"Changed instance index replication accepted"); indices[2052] = savedIndex;
    indices[0] = 17079; rejects([&] { validateTitleBatchGeometry(actual,vertices,indices); },"Inactive capacity vertex authorized a draw"); indices[0] = 0;
    for (uint32_t lane = 0; lane < 4; ++lane) {
        for (float value : {-1.0f,0.5f,1.0f,2.0f,3.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),-0.0f}) {
            setFloat(0,28 + lane * 4,value);
            rejects([&] { validateTitleBatchGeometry(actual,vertices,indices); },"Invalid RVMBatch four-lane index accepted");
        }
        setFloat(0,28 + lane * 4,0);
        setFloat(0,12 + lane * 4,lane ? 1.0f : 0.0f);
        rejects([&] { validateTitleBatchGeometry(actual,vertices,indices); },"Wrong one-hot RVMBatch weight accepted");
        setFloat(0,12 + lane * 4,lane ? 0.0f : 1.0f);
    }
    rejects([&] { validateTitleBatchGeometry(actual,std::span(vertices).first(vertices.size() - 1),indices); },"Truncated real RVMBatch vertices accepted");
    rejects([&] { validateTitleBatchGeometry(actual,vertices,std::span(indices).first(indices.size() - 1)); },"Truncated real RVMBatch indices accepted");
    const D3DVERTEXELEMENT9 declaration[] = {{0,0,2,0,0,0},{0,12,3,0,1,0},{0,28,3,0,2,0},{0,44,2,0,3,0},
        {0,56,3,0,6,0},{0,72,4,0,10,0},{0,76,1,0,5,0},D3DDECL_END()};
    require(isTitleBatchDeclaration(declaration),"Actual RVMBatch format16 declaration rejected");
    for (uint32_t item = 0; item < std::size(declaration); ++item) for (uint32_t field = 0; field < 6; ++field) {
        auto changed = std::vector<D3DVERTEXELEMENT9>(std::begin(declaration),std::end(declaration));
        auto& element = changed[item];
        if (field == 0) ++element.Stream; else if (field == 1) ++element.Offset; else if (field == 2) ++element.Type;
        else if (field == 3) ++element.Method; else if (field == 4) ++element.Usage; else ++element.UsageIndex;
        require(!isTitleBatchDeclaration(changed),"Unverified RVMBatch declaration field accepted");
    }
    std::vector<uint8_t> batch(248), material(24), parent(660), engineVertex(72), engineIndex(64), registry(24);
    const auto be = [](std::vector<uint8_t>& bytes, uint32_t offset, uint32_t value) { for (uint32_t b = 0; b < 4; ++b) bytes[offset + b] = uint8_t(value >> ((3 - b) * 8)); };
    for (const auto [offset,value] : std::vector<std::pair<uint32_t,uint32_t>>{{0,0x8215e29c},{192,0x12000000},{200,6},{204,5693},{208,34158},{224,0x12bfb8f8},{228,0x12bfb940},{240,3},{244,0x12bfb910}}) be(batch,offset,value);
    be(material,16,684);
    require(titleBatchVertexCount(titleBatchCPUDescription(batch,material)) == 17079,"Actual RVMBatch CPU description rejected");
    for (const auto [offset,value] : std::vector<std::pair<uint32_t,uint32_t>>{{0,0},{200,33},{208,34159},{224,0},{228,0x12bfb939},{244,0x12bfb911},{244,0x12bfb940}}) {
        auto bad = batch; be(bad,offset,value); rejects([&] { titleBatchCPUDescription(bad,material); },"Unowned or inconsistent RVMBatch CPU description accepted");
    }
    rejects([&] { titleBatchCPUDescription(std::span(batch).first(247),material); },"Truncated RVMBatch CPU owner accepted");
    rejects([&] { titleBatchCPUDescription(batch,std::span(material).first(23)); },"Truncated RVMBatch material accepted");
    be(parent,0,0x8215df5c); be(parent,272,0x8215e040); be(parent,648,0x12c5c4b0);
    validateTitleBatchParent(parent,0x12c5c4b0); require(true,"Actual RVMBatch master ownership rejected");
    for (const uint32_t offset : {0u,272u,648u}) { auto bad = parent; be(bad,offset,0); rejects([&] { validateTitleBatchParent(bad,0x12c5c4b0); },"Unrelated RVMBatch parent accepted"); }
    const uint64_t handle = 0x0000002100210005ull;
    require(titleBatchRegistryEntry(0x12000000,0x11b90,handle) == 0x12011b90 + 33 * 24,"Actual full64 handle registry index changed");
    be(registry,8,0x13000000); be(registry,16,uint32_t(handle >> 32)); be(registry,20,uint32_t(handle));
    require(titleBatchRegisteredObject(registry,handle) == 0x13000000,"Exact full64 RVMBatch resource ownership rejected");
    rejects([&] { titleBatchRegisteredObject(registry,handle + (uint64_t(1) << 32)); },"Reused RVMBatch handle generation accepted");
    for (const uint64_t bad : {uint64_t(0xffff0000),uint64_t(5000) << 16}) rejects([&] { titleBatchRegistryEntry(0x12000000,0x11b90,bad); },"Out-of-range signed RVMBatch handle accepted");
    rejects([&] { titleBatchRegistryEntry(0xfffffff0,0x11b90,handle); },"RVMBatch registry address wrapped");
    be(engineVertex,0,0x8216059c); be(engineVertex,20,34158); be(engineVertex,24,34158); be(engineVertex,56,16); be(engineVertex,60,84); be(engineVertex,64,11);
    be(engineIndex,0,0x821605b8); be(engineIndex,20,4104); be(engineIndex,24,4104); be(engineIndex,56,12);
    // Original822A8DA8 asks both Engine allocators for direct-native mode1.
    // Flush retains that storage mode, clearing only dirty byte49.
    engineVertex[50] = engineIndex[50] = 1;
    const auto engineCheck = [&] { validateTitleBatchEngineBuffers(engineVertex,engineIndex,actual,11,12,2869272,24624); };
    engineCheck(); require(true,"Actual RVMBatch Engine/native token association rejected");
    for (const auto [offset,value] : std::vector<std::pair<uint32_t,uint32_t>>{{0,0},{20,34157},{24,34157},{56,10},{60,80},{64,13}}) { auto saved = engineVertex; be(engineVertex,offset,value); rejects(engineCheck,"Unowned RVMBatch vertex metadata accepted"); engineVertex = saved; }
    for (const auto [offset,value] : std::vector<std::pair<uint32_t,uint32_t>>{{0,0},{20,4103},{24,4103},{56,13}}) { auto saved = engineIndex; be(engineIndex,offset,value); rejects(engineCheck,"Unowned RVMBatch index metadata accepted"); engineIndex = saved; }
    engineVertex[49] = 1; rejects(engineCheck,"Dirty direct-native RVMBatch vertex buffer accepted"); engineVertex[49] = 0;
    engineIndex[49] = 1; rejects(engineCheck,"Dirty direct-native RVMBatch index buffer accepted"); engineIndex[49] = 0;
    for (const uint8_t mode : {uint8_t(0),uint8_t(2),uint8_t(255)}) {
        engineVertex[50] = mode; rejects(engineCheck,"Unverified RVMBatch vertex storage mode accepted"); engineVertex[50] = 1;
        engineIndex[50] = mode; rejects(engineCheck,"Unverified RVMBatch index storage mode accepted"); engineIndex[50] = 1;
    }
    std::array<float,768> values{}; std::array<bool,192> written; written.fill(true); std::array<float,48> matrices;
    for (uint32_t index = 0; index < matrices.size(); ++index) values[index] = matrices[index] = float(index) + 0.25f;
    const TitleBatchDraw draw{actual,matrices}; const ShaderParameter binding{81,0,192,0};
    const auto palette = [&] { return titleBatchCurrentPalette(ShaderStage::Vertex,"composite/vs",{0,2,79},binding,binding,values,written,draw); };
    require(palette() == 12,"Actual three-instance composite RVMBatch upload has the wrong row count");
    const auto multilayerPalette = [&] { return titleBatchCurrentPalette(ShaderStage::Vertex,"multilayer/vs",{0,2,79},binding,binding,values,written,draw); };
    require(multilayerPalette() == 12,"Actual multilayer RVMBatch upload has the wrong row count");
    for (const uint32_t row : {0u,11u}) {
        written[row] = false;
        rejects(palette,"Missing current RVMBatch active row accepted");
        rejects(multilayerPalette,"Missing current multilayer RVMBatch active row accepted");
        written[row] = true;
    }
    std::fill(written.begin() + 12,written.end(),false);
    require(palette() == 12 && multilayerPalette() == 12,"Unreachable reflected RVMBatch tail was required");
    values[0] += 1; rejects(palette,"Stale actual RVMBatch matrix prefix accepted"); values[0] = matrices[0];
    values[0] += 1; rejects(multilayerPalette,"Stale actual multilayer RVMBatch matrix prefix accepted"); values[0] = matrices[0];
    rejects([&] { titleBatchCurrentPalette(ShaderStage::Vertex,"composite/vs",{0,2,79},binding,binding,values,std::span(written).first(191),draw); },"Independently truncated full written palette span accepted");
    rejects([&] { titleBatchCurrentPalette(ShaderStage::Vertex,"composite/vs",{0,2,79},binding,binding,std::span(values).first(767),written,draw); },"Independently truncated full value palette span accepted");
    for (const uint32_t active : {1u,3u,32u}) {
        std::array<float,4096> shiftedValues{};
        std::array<bool,1024> shiftedWritten{};
        std::vector<float> current(active * 16);
        for (uint32_t index = 0; index < current.size(); ++index)
            shiftedValues[13 * 4 + index] = current[index] = float(index) + .5f;
        std::fill_n(shiftedWritten.begin() + 13,active * 4,true);
        const TitleBatchDraw currentDraw{{77,100,32,active},current};
        for (const auto family : {"composite/vs","multilayer/vs"}) {
            const auto prefix = [&] { return titleBatchCurrentPalette(ShaderStage::Vertex,family,{0,2,79},
                {81,13,192,0},binding,shiftedValues,shiftedWritten,currentDraw); };
            require(prefix() == active * 4,"RVMBatch capacity32/current prefix row count or source offset changed");
            shiftedWritten[13 + active * 4 - 1] = false;
            rejects(prefix,"Last active RVMBatch transform row was not required");
            shiftedWritten[13 + active * 4 - 1] = true;
            shiftedValues[13 * 4 + current.size() - 1] += 1;
            rejects(prefix,"Last active RVMBatch matrix value did not match current renderer");
            shiftedValues[13 * 4 + current.size() - 1] -= 1;
        }
    }
    for (const auto& [stage,name,key] : std::vector<std::tuple<ShaderStage,std::string_view,ShaderKey>>{
        {ShaderStage::Pixel,"composite/vs",{0,2,79}}, {ShaderStage::Pixel,"multilayer/vs",{0,2,79}},
        {ShaderStage::Vertex,"multilayer/ps",{0,2,79}}, {ShaderStage::Vertex,"fixture/vs",{0,2,79}},
        {ShaderStage::Vertex,"composite/vs",{0,2,15}}, {ShaderStage::Vertex,"multilayer/vs",{0,2,15}}})
        rejects([&] { titleBatchCurrentPalette(stage,name,key,binding,binding,values,written,draw); },"Unverified RVMBatch shader family/stage/non-skin key accepted");
}
void verifyGeometryValidationCache() {
    // One slot deliberately collides every key: equality, eviction and failed
    // proofs must be correct independently of the production hash/slot count.
    GeometryValidationCache<1> cache;
    GeometryValidationKey key{11,12,1,1,D3DPT_TRIANGLELIST,0,3,0,1,1,{3,1,0,3,0,3,1}};
    unsigned validations = 0;
    const auto accept = [&] { ++validations; };
    cache.validate(key,accept); cache.validate(key,accept);
    require(validations == 1,"Unchanged immutable geometry did not reuse its successful proof");
    std::vector<GeometryValidationKey> changed;
    const auto change = [&](auto mutate) { auto copy = key; mutate(copy); changed.push_back(copy); };
    change([](auto& k) { ++k.vertexHandle; }); change([](auto& k) { ++k.indexHandle; });
    change([](auto& k) { ++k.vertexRevision; }); change([](auto& k) { ++k.indexRevision; });
    change([](auto& k) { k.vertexRevision += uint64_t(1)<<32; });
    change([](auto& k) { k.indexRevision += uint64_t(1)<<32; });
    change([](auto& k) { ++k.topology; }); change([](auto& k) { ++k.baseVertex; });
    change([](auto& k) { ++k.vertexCount; }); change([](auto& k) { ++k.startIndex; });
    change([](auto& k) { ++k.triangles; }); change([](auto& k) { ++k.producer; });
    for (size_t field = 0; field < key.geometry.size(); ++field) change([&](auto& k) { ++k.geometry[field]; });
    for (const auto& other : changed) {
        cache.validate(key,accept);
        const unsigned before = validations;
        cache.validate(other,accept);
        require(validations == before + 1,"Changed buffer identity/revision or draw/producer bounds reused an unrelated proof");
    }
    unsigned attempts = 0;
    const auto reject = [&] { ++attempts; throw std::runtime_error("rejected geometry"); };
    key.vertexRevision += 10;
    rejects([&] { cache.validate(key,reject); },"First rejected proof was accepted");
    rejects([&] { cache.validate(key,reject); },"A rejected proof was cached");
    require(attempts == 2,"A rejected colliding draw bypassed revalidation");

    std::vector<uint8_t> vertices(3*80);
    std::array<uint16_t,3> indices{0,1,2};
    const TitleBoneGeometry geometry{3,1,0,3,0,3,1};
    key.vertexRevision += 1;
    const auto validate = [&] { validateTitleBoneGeometry(geometry,vertices,indices); };
    cache.validate(key,validate); cache.validate(key,validate);
    const float invalidBone = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(vertices.data()+28,&invalidBone,4); ++key.vertexRevision;
    rejects([&] { cache.validate(key,validate); },"Changed vertex content reused a prior bone-index proof");
    std::memset(vertices.data()+28,0,4);
    cache.validate(key,validate); // A prior failure may be corrected and retried.
    indices[1] = 3; ++key.indexRevision;
    rejects([&] { cache.validate(key,validate); },"Changed index content reused a prior draw-range proof");
}
int main() {
    try {
        verifyGeometryValidationCache();
        verifyRendererOwnedSpans();
        verifyDrawLocalVertexProofs();
        verifyRvmBatchContract();
        std::vector<uint8_t> shadowOwner(100);
        const auto shadowWord = [&](uint32_t offset, uint32_t value) {
            for (uint32_t byte = 0; byte < 4; ++byte) shadowOwner[offset + byte] = static_cast<uint8_t>(value >> ((3 - byte) * 8));
        };
        for (const auto [offset,value] : std::vector<std::pair<uint32_t,uint32_t>>{{0,0x82161380},{4,1},{8,2048},{12,2048},
            {28,0x12000100},{44,0x12000200},{72,0x12000300},{88,999},{96,0x2d200196}}) shadowWord(offset,value);
        const auto validShadowOwner = shadowOwner;
        const auto shadow = titleShadowTarget(shadowOwner);
        require(shadow.width == 2048 && shadow.height == 2048 && shadow.texture == 0x12000100 &&
            shadow.color == 0x12000200 && shadow.depth == 0x12000300 && shadowOwner == validShadowOwner,
            "Actual nonfloating shadow owner identity changed during validation");
        for (const auto [offset,value] : std::vector<std::pair<uint32_t,uint32_t>>{{0,0},{4,0},{4,3},{8,0},{12,8193},
            {28,0},{44,0},{72,0},{44,0x12000100},{72,0x12000200},{84,1},{88,0},{96,0x1a220197}}) {
            shadowWord(offset,value);
            rejects([&] { titleShadowTarget(shadowOwner); },"Unverified shadow owner accepted");
            shadowOwner = validShadowOwner;
        }
        rejects([&] { titleShadowTarget(std::span(shadowOwner).first(99)); },"Truncated shadow owner accepted");
        TitleShadowEndArguments shadowEnd{0x823903dc,20,0,shadow.texture,0,0,0,0x10400080,0x10400000,0,0,1.0};
        validateTitleShadowEnd(shadow,shadowEnd);
        require(true,"Exact original no-clear shadow End rejected");
        const auto validEnd = shadowEnd;
        for (uint32_t index = 0; index < 12; ++index) {
            switch (index) {
            case 0: shadowEnd.caller = 0; break; case 1: shadowEnd.flags = 4; break;
            case 2: shadowEnd.flags = 20 | 0x200; break; case 3: shadowEnd.sourceRectangle = 1; break;
            case 4: shadowEnd.destination = shadow.color; break; case 5: shadowEnd.destinationPoint = 1; break;
            case 6: shadowEnd.slice = 1; break; case 7: shadowEnd.level = 1; break;
            case 8: shadowEnd.clearColor++; break; case 9: shadowEnd.extra92 = 1; break;
            case 10: shadowEnd.extra100 = 1; break; case 11: shadowEnd.clearDepth = 0; break;
            }
            rejects([&] { validateTitleShadowEnd(shadow,shadowEnd); },"Unverified original shadow End arguments accepted");
            shadowEnd = validEnd;
        }
        shadowEnd.stack = 0xfffffff0; shadowEnd.clearColor = 0x70;
        rejects([&] { validateTitleShadowEnd(shadow,shadowEnd); },"Wrapped shadow clear pointer accepted");
        D3DVIEWPORT9 shadowViewport{0,0,2048,2048,0,1};
        require(isOriginalShadowClear(0x8237d34c,{},D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,0xffffffff,1,0,
            shadowViewport,2048,2048),"Original full shadow clear not recognized");
        shadowViewport.Width = 2047;
        require(!isOriginalShadowClear(0x8237d34c,{},D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,0xffffffff,1,0,
            shadowViewport,2048,2048),"Partial viewport clear authorized full shadow completion");
        shadowViewport.Width = 2048;
        require(!isOriginalShadowClear(0x8234ad38,{},D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,0xffffffff,1,0,
            shadowViewport,2048,2048),"Unrelated color/depth clear authorized original shadow End");
        const D3DRECT shadowRect{0,0,2048,2048};
        require(!isOriginalShadowClear(0x8237d34c,{&shadowRect,1},D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,0xffffffff,1,0,
            shadowViewport,2048,2048),"Rectangle clear widened exact authored shadow clear contract");
        const ShaderKey shadowKey{1u << 21,0,0};
        require(validateTitleShadowSample(ShaderStage::Pixel,"multilayer/ps",shadowKey,1u << 9,9,true,true,false),
            "Verified completed detached native main shadow read rejected");
        require(validateTitleShadowSample(ShaderStage::Pixel,"Composite/PS",shadowKey,1u << 9,9,true,true,false),
            "Canonical original composite shadow family rejected");
        require(!validateTitleShadowSample(ShaderStage::Pixel,"unrelated/ps",{},0,9,false,false,true),
            "Unused retained depth sampler was mistaken for an actual shader read");
        rejects([&] { validateTitleShadowSample(ShaderStage::Vertex,"multilayer/ps",shadowKey,512,9,true,true,false); },"Wrong shadow consumer stage accepted");
        rejects([&] { validateTitleShadowSample(ShaderStage::Pixel,"ocean/ps",shadowKey,512,9,true,true,false); },"Unverified shadow consumer family accepted");
        rejects([&] { validateTitleShadowSample(ShaderStage::Pixel,"multilayer/ps",{},512,9,true,true,false); },"Non-shadow feature key accepted for depth read");
        rejects([&] { validateTitleShadowSample(ShaderStage::Pixel,"multilayer/ps",{3u << 21,0,0},512,9,true,true,false); },"Floating shadow feature key accepted for D24 comparison");
        rejects([&] { validateTitleShadowSample(ShaderStage::Pixel,"multilayer/ps",{(1u << 21) | (1u << 7),0,0},512,9,true,true,false); },"Point/cube source branch accepted as type1 depth comparison");
        rejects([&] { validateTitleShadowSample(ShaderStage::Pixel,"multilayer/ps",{(1u << 21) | (1u << 6),0,0},512,9,true,true,false); },"Second-spot source-disabled shadow branch accepted");
        rejects([&] { validateTitleShadowSample(ShaderStage::Pixel,"multilayer/ps",shadowKey,512,9,true,true,false,D3DSTT_CUBE); },"Actual cube sampler declaration accepted for 2D depth sampling");
        rejects([&] { validateTitleShadowSample(ShaderStage::Pixel,"multilayer/ps",shadowKey,512,9,true,true,false,D3DSTT_VOLUME); },"Actual volume sampler declaration accepted for 2D depth sampling");
        for (uint32_t index = 0; index < 4; ++index)
            rejects([&] { validateTitleShadowSample(ShaderStage::Pixel,"multilayer/ps",shadowKey,index ? 512 : 256,
                index ? 9 : 8,index != 1,index != 2,index == 3); },"Unsafe or unowned actual depth read accepted");
        rejects([&] { validateTitleShadowSample(ShaderStage::Pixel,"multilayer/ps",shadowKey,1u << 16,9,true,true,false); },"Native sampler extent overflow accepted");
        std::vector<uint8_t> nativeSamplerTokens;
        for (uint32_t token : {0xffff0300u,0x0002fffeu,0xdeadbeefu,0xcafebabeu,0x0200001fu,0x10000000u,0xa00f0809u,0x0000ffffu})
            append(nativeSamplerTokens,token);
        require(nativeShaderSamplerMask(nativeSamplerTokens,ShaderStage::Pixel) == 512,
            "Bounded native DCL sampler9 mask differs from SM3 declaration ABI");
        require(nativeShaderSamplers(nativeSamplerTokens,ShaderStage::Pixel).textureTypes[9] == D3DSTT_2D,"Actual DCL2D texture kind lost");
        nativeSamplerTokens[23] = 0x18;
        require(nativeShaderSamplers(nativeSamplerTokens,ShaderStage::Pixel).textureTypes[9] == D3DSTT_CUBE,"Actual DCLcube texture kind lost");
        nativeSamplerTokens[23] = 0x20;
        require(nativeShaderSamplers(nativeSamplerTokens,ShaderStage::Pixel).textureTypes[9] == D3DSTT_VOLUME,"Actual DCLvolume texture kind lost");
        nativeSamplerTokens[23] = 0;
        rejects([&] { nativeShaderSamplerMask(nativeSamplerTokens,ShaderStage::Pixel); },"Undefined native sampler texture kind accepted");
        nativeSamplerTokens[23] = 0x10;
        nativeSamplerTokens[1] = 2;
        require(nativeShaderSamplerMask(nativeSamplerTokens,ShaderStage::Pixel) == 512,"Verified SM2 instruction declaration grammar rejected");
        nativeSamplerTokens[0] = 1;
        require(nativeShaderSamplerMask(nativeSamplerTokens,ShaderStage::Pixel) == 512,"Original ocean ps_2_1 declaration grammar rejected");
        nativeSamplerTokens[1] = 1;
        rejects([&] { nativeShaderSamplerMask(nativeSamplerTokens,ShaderStage::Pixel); },"Unverified SM1 instruction grammar accepted");
        nativeSamplerTokens[0] = 0; nativeSamplerTokens[1] = 3;
        rejects([&] { nativeShaderSamplerMask(nativeSamplerTokens,ShaderStage::Vertex); },"Native sampler stage version mismatch accepted");
        rejects([&] { nativeShaderSamplerMask(std::span(nativeSamplerTokens).first(15),ShaderStage::Pixel); },"Misaligned/truncated native code accepted");
        nativeSamplerTokens[20] = 0x10; // Texture declaration payload is not the destination token.
        require(nativeShaderSamplerMask(nativeSamplerTokens,ShaderStage::Pixel) == 512,"Sampler mask decoded a declaration payload as a register");
        nativeSamplerTokens[24] = 16;
        rejects([&] { nativeShaderSamplerMask(nativeSamplerTokens,ShaderStage::Pixel); },"Native sampler16 accepted");
        nativeSamplerTokens[24] = 9; nativeSamplerTokens.resize(28);
        rejects([&] { nativeShaderSamplerMask(nativeSamplerTokens,ShaderStage::Pixel); },"Native code without END accepted");
        const auto bytes = cache();
        std::vector<uint8_t> depthOwner(100);
        const auto depthWord = [&](uint32_t offset, uint32_t value) {
            for (uint32_t byte = 0; byte < 4; ++byte) depthOwner[offset + byte] = static_cast<uint8_t>(value >> ((3 - byte) * 8));
        };
        depthWord(0,0x82161380); depthWord(4,1); depthWord(8,2560); depthWord(12,1440);
        depthWord(88,999); depthWord(96,0x2d200196);
        const auto depthOwnerBefore = depthOwner;
        const auto target = titleDepthTargetDescription(depthOwner);
        require(target.width == 2560 && target.height == 1440 && depthOwner == depthOwnerBefore,
            "Exact type1 owner allocation changed original CPU bytes");
        rejects([&] { titleDepthTargetDescription(std::span(depthOwner).first(99)); },"Truncated depth target owner accepted");
        for (const auto [offset,value] : std::vector<std::pair<uint32_t,uint32_t>>{{0,0x82161384},{4,0},{4,3},
            {8,0},{12,0},{8,8193},{12,8193},{28,0x12345000},{72,0x12346000},{84,1},{84,2},{88,0},{96,0x1a220197}}) {
            depthWord(offset,value);
            rejects([&] { titleDepthTargetDescription(depthOwner); },"Unverified type1 depth allocation owner field accepted");
            depthOwner = depthOwnerBefore;
        }
        verifyDepthPairTransactions();
        const std::array<uint32_t,16> floatBits{0x00000000,0x80000000,0x3f800000,0xbf800000,
            0x7f800000,0xff800000,0x7fc12345,0xffc54321,0x7f812345,0xff812345,
            0x00000001,0x80000001,0x007fffff,0x00800000,0x7f7fffff,0xff7fffff};
        std::vector<uint8_t> floatSource{0x5a}; // Original source may be unaligned.
        for (const auto value : floatBits) append(floatSource,value,true);
        const auto floatSourceBefore = floatSource;
        std::array<uint8_t,10> scalarBytes{};
        scalarBytes.front() = 0x5a; scalarBytes.back() = 0xa5;
        for (uint32_t bits : {0u,0x80000000u,0x7fc12345u,0xffffffffu,0x00000001u}) {
            titleGuestWriteWord(std::span(scalarBytes).subspan(1,8),1,bits);
            require(titleGuestWord(std::span(scalarBytes).subspan(1,8),1) == bits,
                "Unaligned validated guest scalar changed integer/float bits");
            require(scalarBytes.front() == 0x5a && scalarBytes.back() == 0xa5,
                "Validated guest scalar changed neighboring sentinels");
        }
        const auto scalarBeforeReject = scalarBytes;
        rejects([&] { titleGuestWriteWord(std::span(scalarBytes).first(3),0,0); },"Truncated guest scalar write accepted");
        rejects([&] { titleGuestWriteWord(scalarBytes,SIZE_MAX,0); },"Overflow guest scalar write accepted");
        rejects([&] { titleGuestWord(std::span(scalarBytes).first(3),0); },"Truncated guest scalar read accepted");
        rejects([&] { titleGuestWord(scalarBytes,SIZE_MAX); },"Overflow guest scalar read accepted");
        require(scalarBytes == scalarBeforeReject,"Rejected guest scalar write partially mutated bytes");
        const auto decodedFloats = decodeTitleFloatConstants(std::span(floatSource).subspan(1),4);
        bool floatBitsEqual = decodedFloats.size() == floatBits.size();
        for (size_t index = 0; index < floatBits.size(); ++index)
            floatBitsEqual = floatBitsEqual && std::bit_cast<uint32_t>(decodedFloats[index]) == floatBits[index];
        require(floatBitsEqual,"Once-validated BE constants altered NaN payloads, signed zeros, infinities or subnormals");
        require(floatSource == floatSourceBefore,"Constant decoding mutated original guest bytes");
        std::vector<uint32_t> fullConstantBits(4096);
        std::vector<uint8_t> fullConstantBytes;
        uint32_t randomBits = 0x12345678;
        for (auto& value : fullConstantBits) {
            randomBits = randomBits * 1664525u + 1013904223u;
            value = randomBits;
            append(fullConstantBytes,value,true);
        }
        const auto fullConstants = decodeTitleFloatConstants(fullConstantBytes,1024);
        bool fullConstantsEqual = fullConstants.size() == fullConstantBits.size();
        for (size_t index = 0; index < fullConstantBits.size(); ++index)
            fullConstantsEqual = fullConstantsEqual && std::bit_cast<uint32_t>(fullConstants[index]) == fullConstantBits[index];
        require(fullConstantsEqual,"Bounded full-register conversion differs from original BE32 bit conversion");
        require(titleFloatConstantSpanBytes(0x10000,0,1024) == 16384 &&
            titleFloatConstantSpanBytes(0xfffffff0,1023,1) == 16,"Valid constant request or exact4GB endpoint rejected");
        rejects([&] { decodeTitleFloatConstants({},0); },"Empty constant upload accepted");
        rejects([&] { decodeTitleFloatConstants(fullConstantBytes,1025); },"Unbounded register conversion accepted");
        rejects([&] { decodeTitleFloatConstants(std::span(fullConstantBytes).first(15),1); },"Truncated float4 constant span accepted");
        rejects([&] { decodeTitleFloatConstants(std::span(fullConstantBytes).first(17),1); },"Unclaimed trailing constant bytes accepted");
        rejects([&] { titleFloatConstantSpanBytes(0x10000,1024,1); },"Guest register range beyond1024 accepted");
        rejects([&] { titleFloatConstantSpanBytes(0x10000,1023,2); },"Guest register count crossed adapter bound");
        rejects([&] { titleFloatConstantSpanBytes(0xfffffff1,0,1); },"Guest constant source wrapped4GB address space");
        rejects([&] { titleFloatConstantSpanBytes(0xffff,0,1); },"Constant source below committed guest domain accepted");
        for (uint32_t address : {0x7fc00000u,0x7fc00001u,0x7fcfffffu,0x7fbfffffu,
            0x7fe00000u,0x7fe00001u,0x7fefffffu,0x7fdfffffu})
            rejects([&] { titleFloatConstantSpanBytes(address,0,1); },"MMIO or crossing MMIO constant source accepted");
        require(titleFloatConstantSpanBytes(0x7fbffff0,0,1) == 16 &&
            titleFloatConstantSpanBytes(0x7fd00000,0,1) == 16 &&
            titleFloatConstantSpanBytes(0x7fdffff0,0,1) == 16 &&
            titleFloatConstantSpanBytes(0x7ff00000,0,1) == 16,"CPU span bordering MMIO domain incorrectly rejected");
        RecentPhaseTimes phaseWindow;
        require(phaseWindow.count() == 0 && phaseWindow.summary().wallNanoseconds == 0,"Phase window included unmeasured startup cycles");
        PhaseFrame phaseFixture;
        phaseFixture.wallNanoseconds = 100;
        phaseFixture.bridgeNanoseconds = 40;
        phaseFixture.phases[0] = {1,10,500};
        for (uint32_t index = 0; index < 11; ++index) phaseWindow.record(phaseFixture);
        phaseFixture.phases[0].worstNanoseconds = 10;
        for (uint32_t index = 0; index < 300; ++index) phaseWindow.record(phaseFixture);
        const auto phaseSummary = phaseWindow.summary();
        require(phaseWindow.count() == 300 && phaseSummary.wallNanoseconds == 30000 && phaseSummary.bridgeNanoseconds == 12000 &&
            phaseSummary.phases[0].calls == 300 && phaseSummary.phases[0].nanoseconds == 3000 && phaseSummary.phases[0].worstNanoseconds == 10,
            "Phase window retained overwritten timing/count/worst samples outside its last300 cycles");
        phaseWindow.reset();
        phaseFixture.wallNanoseconds = 200;
        phaseWindow.record(phaseFixture);
        require(phaseWindow.count() == 1 && phaseWindow.summary().wallNanoseconds == 200,
            "Clear-only phase reset retained previous cycle coverage");
        PhaseFrame scopeFrame;
        { PhaseScope disabled(scopeFrame,RenderPhase::Draw,false); }
        require(scopeFrame.phases[static_cast<size_t>(RenderPhase::Draw)].calls == 0,"Disabled timing recorded an API sample");
        { PhaseScope measured(scopeFrame,RenderPhase::Draw,true); measured.stop(); measured.stop(); }
        require(scopeFrame.phases[static_cast<size_t>(RenderPhase::Draw)].calls == 1,"Explicit phase stop/destructor double-counted an API call");
        std::vector<uint8_t> boneExporter(224);
        const auto exporterWord = [&](uint32_t offset, uint32_t word) {
            for (uint32_t byte = 0; byte < 4; ++byte) boneExporter[offset + byte] = static_cast<uint8_t>(word >> ((3 - byte) * 8));
        };
        exporterWord(56,0x12000000); exporterWord(60,80); exporterWord(220,0x13000000);
        exporterWord(64,0xdead002c); exporterWord(72,0xdead000c); exporterWord(80,0xdead001c);
        validateTitleBoneExporterBinding(boneExporter,0x12000000,0x13000000);
        require(true,"Original freed secondary staging pointers incorrectly authorized or rejected native geometry ownership");
        rejects([&] { validateTitleBoneExporterBinding(boneExporter,0xdead0000,0x13000000); },"Freed staging base authorized actual native VB ownership");
        rejects([&] { validateTitleBoneExporterBinding(boneExporter,0x12000000,0xdead1000); },"Unrelated original index buffer authorized actual native IB ownership");
        exporterWord(60,84);
        rejects([&] { validateTitleBoneExporterBinding(boneExporter,0x12000000,0x13000000); },"Unverified84-byte exporter accepted as live format10 geometry");
        exporterWord(60,80);
        TitleBoneGeometry boneGeometry{4,2,1,2,3,3,2};
        std::vector<uint8_t> boneVertices(5 * 80,0); // Includes an unused allocation sentinel.
        const auto boneIndex = [&](uint32_t vertex, uint32_t lane, float value) {
            std::memcpy(boneVertices.data() + vertex * 80 + 28 + lane * 4,&value,4);
        };
        for (uint32_t vertex : {1u,2u}) for (uint32_t lane = 0; lane < 4; ++lane) boneIndex(vertex,lane,float(lane % 2));
        boneIndex(0,0,99); // Unreferenced data cannot enlarge or invalidate the actual draw.
        std::vector<uint16_t> boneIndices{0,0,0,0,1,0,0,0,0}; // Last triangle is also an allocation sentinel.
        validateTitleBoneGeometry(boneGeometry,boneVertices,boneIndices);
        require(true,"Verified original two-bone indexed group rejected");
        const auto badBoneGeometry = [&](TitleBoneGeometry bad) { validateTitleBoneGeometry(bad,boneVertices,boneIndices); };
        auto alteredBone = boneGeometry;
        alteredBone.boneCount = 0;
        rejects([&] { badBoneGeometry(alteredBone); },"Empty active bone palette accepted");
        alteredBone = boneGeometry; alteredBone.boneCount = 49;
        rejects([&] { badBoneGeometry(alteredBone); },"Cal3D 48-bone group maximum exceeded");
        alteredBone = boneGeometry; alteredBone.vertexCount = 4;
        rejects([&] { badBoneGeometry(alteredBone); },"Unused allocation vertex sentinel authorized palette access");
        alteredBone = boneGeometry; alteredBone.startIndex = 6;
        rejects([&] { badBoneGeometry(alteredBone); },"Unused allocation triangle sentinel authorized palette access");
        boneIndices[4] = 2;
        rejects([&] { badBoneGeometry(boneGeometry); },"Index outside actual selected group accepted");
        boneIndices[4] = 1;
        for (float invalid : {2.0f,-1.0f,0.5f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            boneIndex(1,2,invalid); // Weight is zero: original HLSL still fetches this lane.
            rejects([&] { badBoneGeometry(boneGeometry); },"Invalid zero-weight bone index accepted");
        }
        boneIndex(1,2,0);
        rejects([&] { validateTitleBoneGeometry(boneGeometry,std::span(boneVertices).first(3 * 80),boneIndices); },
            "Truncated actual geometry accepted");
        const ShaderParameter originalBoneParameter{81,13,192,0}, nativeBoneParameter{81,0,192,0};
        std::array<float,4096> originalBoneValues{};
        std::array<bool,1024> originalBoneWritten{};
        std::array<float,32> currentBoneMatrices;
        for (uint32_t index = 0; index < currentBoneMatrices.size(); ++index)
            originalBoneValues[13 * 4 + index] = currentBoneMatrices[index] = float(index + 1) / 4;
        std::fill(originalBoneWritten.begin() + 13,originalBoneWritten.begin() + 21,true);
        originalBoneValues[21 * 4] = 999; // Missing reflected tail must remain unmodified.
        const auto palettePrefix = [&] {
            return titleBonePalettePrefix(ShaderStage::Vertex,"multilayer/vs",{4102,0,203},originalBoneParameter,
                nativeBoneParameter,originalBoneValues,originalBoneWritten,currentBoneMatrices);
        };
        require(palettePrefix() == 8 && originalBoneValues[21 * 4] == 999 && !originalBoneWritten[21],
            "Active two-bone prefix fabricated or required the unused 184 rows");
        originalBoneWritten[20] = false;
        rejects([&] { palettePrefix(); },"Missing active palette row accepted");
        originalBoneWritten[20] = true;
        originalBoneValues[13 * 4] = 1;
        rejects([&] { palettePrefix(); },"Stale previous-group matrix upload accepted");
        originalBoneValues[13 * 4] = currentBoneMatrices[0];
        require(!isTitleBonePaletteShader(ShaderStage::Pixel,"multilayer/vs",{4102,0,203}) &&
            !isTitleBonePaletteShader(ShaderStage::Vertex,"billboard/avatarpose_xenon_vs",{4102,0,203}) &&
            !isTitleBonePaletteShader(ShaderStage::Vertex,"multilayer/vs",{4102,0,139}),
            "Unverified family/stage/non-skin shader authorized an active palette exception");
        auto wrongBoneParameter = nativeBoneParameter; wrongBoneParameter.firstRegister = 65;
        rejects([&] { titleBonePalettePrefix(ShaderStage::Vertex,"multilayer/vs",{4102,0,203},originalBoneParameter,
            wrongBoneParameter,originalBoneValues,originalBoneWritten,currentBoneMatrices); },"Unverified native palette register offset accepted");
        wrongBoneParameter = nativeBoneParameter; wrongBoneParameter.id = 80;
        rejects([&] { titleBonePalettePrefix(ShaderStage::Vertex,"multilayer/vs",{4102,0,203},originalBoneParameter,
            wrongBoneParameter,originalBoneValues,originalBoneWritten,currentBoneMatrices); },"Other missing parameter received palette exception");
        rejects([&] { titleBonePalettePrefix(ShaderStage::Vertex,"multilayer/vs",{4102,0,203},originalBoneParameter,
            nativeBoneParameter,originalBoneValues,originalBoneWritten,std::span(currentBoneMatrices).first(31)); },
            "Partial matrix palette accepted");
        std::array<float,192> twelveBoneMatrices;
        std::array<float,4096> twelveBoneValues{};
        std::array<bool,1024> twelveBoneWritten{};
        for (uint32_t index = 0; index < twelveBoneMatrices.size(); ++index)
            twelveBoneValues[10 * 4 + index] = twelveBoneMatrices[index] = float(index + 1) / 16;
        std::fill(twelveBoneWritten.begin() + 10,twelveBoneWritten.begin() + 58,true);
        require(titleBonePalettePrefix(ShaderStage::Vertex,"multilayer/vs",{6,0,203},{81,10,192,0},nativeBoneParameter,
            twelveBoneValues,twelveBoneWritten,twelveBoneMatrices) == 48 && !twelveBoneWritten[58],
            "Actual12-bone material prefix with different reflected offset required its unused144-row tail");
        std::copy(currentBoneMatrices.begin(),currentBoneMatrices.end(),originalBoneValues.begin());
        std::fill(originalBoneWritten.begin(),originalBoneWritten.begin() + 8,true);
        require(titleBonePalettePrefix(ShaderStage::Vertex,"composite/vs",{0,2,76},{81,0,192,0},nativeBoneParameter,
            originalBoneValues,originalBoneWritten,currentBoneMatrices) == 8,
            "Verified shared composite skin source did not accept its actual matrix prefix");
        wrongBoneParameter = nativeBoneParameter; wrongBoneParameter.registerCount = 216;
        rejects([&] { titleBonePalettePrefix(ShaderStage::Vertex,"multilayer/vs",{6,0,203},{81,10,192,0},wrongBoneParameter,
            twelveBoneValues,twelveBoneWritten,twelveBoneMatrices); },"Different reflected palette size authorized main-family tail exception");
        auto wrongOriginalBone = originalBoneParameter; wrongOriginalBone.firstRegister = 833;
        rejects([&] { titleBonePalettePrefix(ShaderStage::Vertex,"multilayer/vs",{4102,0,203},wrongOriginalBone,nativeBoneParameter,
            originalBoneValues,originalBoneWritten,currentBoneMatrices); },"Original reflected palette span exceeded guest constant array");
        rejects([&] { titleBonePalettePrefix(ShaderStage::Pixel,"multilayer/vs",{4102,0,203},originalBoneParameter,nativeBoneParameter,
            originalBoneValues,originalBoneWritten,currentBoneMatrices); },"Pixel shader received a vertex skin palette exception");
        rejects([&] { titleBonePalettePrefix(ShaderStage::Vertex,"billboard/avatarpose_xenon_vs",{4102,0,203},originalBoneParameter,nativeBoneParameter,
            originalBoneValues,originalBoneWritten,currentBoneMatrices); },"SDK AvatarPose received the ordinary Cal3D palette exception");
        const auto palette0 = titleAvatarBufferBinding(0x12000000,0x12000150,0,0xe400f000);
        const auto palette1 = titleAvatarBufferBinding(0x12000000,0x12000170,3456,0xe400f000);
        require(palette0.data == 0xe400f000 && palette1.data == 0xe400fd80 &&
            !palette0.lifetime.ownedToken && !palette0.lifetime.ownedData &&
            palette1.lifetime.backingBase == palette0.lifetime.backingBase,
            "Original inline palettes did not retain shared physical backing and borrowed CPU ownership");
        rejects([&] { titleAvatarBufferBinding(0x12000000,0x12000150,3456,0xe400f000); }, "Inline palette offset selected the wrong supplied slot");
        rejects([&] { titleAvatarBufferBinding(0x12000000,0x12000150,1,0xe400f000); }, "Unverified inline palette slice was accepted");
        rejects([&] { titleAvatarBufferBinding(0x12000000,0x12000151,0,0xe400f000); }, "Unverified inline palette storage was accepted");
        rejects([&] { titleAvatarBufferBinding(0,336,0,0xe400f000); }, "Null inline CPU owner was accepted");
        rejects([&] { titleAvatarBufferBinding(0x12000000,0x12000150,0,0); }, "Null inline physical backing was accepted");
        rejects([&] { titleAvatarBufferBinding(UINT32_MAX-400,UINT32_MAX-64,0,0xe400f000); }, "Inline CPU owner wrapped guest address space");
        rejects([&] { titleAvatarBufferBinding(0x12000000,0x12000150,0,UINT32_MAX-6910); }, "Shared inline physical backing wrapped guest address space");
        struct BufferFixture { TitleBufferStorage storage; uint32_t native = 0, data = 0; };
        std::map<uint32_t,BufferFixture> inlineRegistry{
            {palette0.storage,{palette0.lifetime,0,0}}, // pending address association
            {palette1.storage,{palette1.lifetime,7,palette1.data}},
            {0x14000000,{{true,true,0,0xe400f000,0},8,0xe400f000}}, // separately owned resource
            {0x14000020,{{false,false,0x13000000,0xe4011000,0},9,0xe4011000}}};
        uint32_t releasedNative = 0;
        auto nativeRelease = [&](uint32_t, const BufferFixture& entry) { releasedNative += entry.native; };
        require(detachTitleBorrowedBuffers(inlineRegistry,palette1.data,nativeRelease) == 0 && releasedNative == 0,
            "Freeing a palette slice detached a different actual physical allocation");
        require(detachTitleBorrowedBuffers(inlineRegistry,0xe400f000,nativeRelease) == 2 && releasedNative == 7 &&
            !inlineRegistry.contains(palette0.storage) && !inlineRegistry.contains(palette1.storage) &&
            inlineRegistry.contains(0x14000000) && inlineRegistry.contains(0x14000020),
            "Physical free failed to detach pending/attached aliases or detached unrelated owned storage");
        require(inlineRegistry.emplace(palette0.storage,BufferFixture{palette0.lifetime}).second,
            "Destroyed inline CPU storage could not be registered again after real physical free");
        rejects([&] { detachTitleBorrowedBuffers(inlineRegistry,0xe400f000,
            [](uint32_t,const BufferFixture&) { throw std::runtime_error("actual native release failure"); }); },
            "Failed native release was hidden before physical free");
        require(inlineRegistry.contains(palette0.storage), "Failed native release erased borrowed lifetime metadata");
        require(detachTitleBorrowedBuffers(inlineRegistry,0,nativeRelease) == 0,
            "Null physical free detached pending borrowed resources");
        const auto parsed = ShaderCache::parse(bytes, ShaderCachePlatform::Windows);
        require(parsed.variantCount() == 1 && parsed.shaders().front().name == "fixture/vs", "Valid Windows cache did not parse");
        require(parsed.find("FIXTURE/VS", {0, 0, 4}).variant != nullptr, "Case-insensitive exact key lookup failed");
        require(parsed.find("fixture/vs", {0, 0, 5}).variant == nullptr, "Missing key returned a variant");
        const auto& code = parsed.shaders().front().variants.front().bytecode;
        const auto codeBytes = std::span(reinterpret_cast<const uint8_t*>(code.data()), code.size() * sizeof(uint32_t));
        require(parsed.matchCode(codeBytes).size() == 1, "Original bytecode match failed");
        require(ShaderCache::parse(cache(true), ShaderCachePlatform::Xenon).variantCount() == 1, "BE Xenon cache did not parse");
        for (size_t length = 0; length < bytes.size(); ++length)
            rejects([&] { ShaderCache::parse(std::span(bytes).first(length), ShaderCachePlatform::Windows); }, "Truncated cache was accepted");
        auto changed = bytes;
        changed.push_back(0);
        rejects([&] { ShaderCache::parse(changed, ShaderCachePlatform::Windows); }, "Trailing cache data was accepted");
        rejects([&] { ShaderCache::parse(cache(false, true), ShaderCachePlatform::Windows); }, "Duplicate feature key was accepted");
        // Genuine source additions preserve original identities and reject
        // replacing a known key or crossing platform/family boundaries.
        const auto xenonOriginal = ShaderCache::parse(cache(true),ShaderCachePlatform::Xenon);
        const auto xenonSource = ShaderCache::parse(cache(true,false,"multilayer/vs"),ShaderCachePlatform::Xenon);
        const auto sourceMerged = xenonOriginal.withXenonSourceExtension(xenonSource);
        require(sourceMerged.variantCount() == 2 && xenonOriginal.variantCount() == 1,"Original source extension mutated/replaced baseline");
        const auto sourceCode = xenonSource.shaders().front().variants.front().bytecode;
        const auto sourceCodeBytes = std::span(reinterpret_cast<const uint8_t*>(sourceCode.data()),sourceCode.size()*4);
        require(sourceMerged.requireCodeVariant({"multilayer/vs",{0,0,4}},ShaderStage::Vertex,sourceCodeBytes).variant != nullptr,
            "Appended genuine source identity lost its exact bytecode contract");
        rejects([&] { sourceMerged.withXenonSourceExtension(xenonSource); },"Source extension replaced an original feature key");
        rejects([&] { xenonOriginal.withXenonSourceExtension(parsed); },"Source extension accepted wrong platform");
        rejects([&] { xenonOriginal.withXenonSourceExtension(xenonOriginal); },"Source extension accepted unverified family");
        std::vector<uint8_t> sourceVariant(2540), originalCode(16);
        const ShaderSelection sourceIdentity{"multilayer/vs",{8388614,0,75}};
        titleGuestWriteWord(sourceVariant,2536,0x1000);
        for (size_t word = 0; word < 3; ++word) titleGuestWriteWord(sourceVariant,8 + word*4,sourceIdentity.key[word]);
        titleGuestWriteWord(sourceVariant,52,1);
        titleGuestWriteWord(sourceVariant,56,81); titleGuestWriteWord(sourceVariant,60,8); titleGuestWriteWord(sourceVariant,64,192);
        titleGuestWriteWord(originalCode,4,12); titleGuestWriteWord(originalCode,8,4);
        const auto harvested = originalSourceShader(sourceIdentity,ShaderStage::Vertex,0x1000,sourceVariant,originalCode);
        require(harvested.parameters.size() == 1 && harvested.parameters[0].registerCount == 192 && harvested.bytecode.size() == 4,
            "Source shader capture lost exact original reflection/code bytes");
        rejects([&] { originalSourceShader(sourceIdentity,ShaderStage::Vertex,0x2000,sourceVariant,originalCode); },"Source capture accepted wrong owner");
        rejects([&] { originalSourceShader(sourceIdentity,ShaderStage::Pixel,0x1000,sourceVariant,originalCode); },"Source capture accepted wrong stage");
        rejects([&] { originalSourceShader(sourceIdentity,ShaderStage::Vertex,0x1000,std::span(sourceVariant).first(2539),originalCode); },"Source capture accepted truncated variant");
        auto badKey = sourceIdentity; badKey.key[2] = 203;
        rejects([&] { originalSourceShader(badKey,ShaderStage::Vertex,0x1000,sourceVariant,originalCode); },"Source capture accepted wrong full key");
        titleGuestWriteWord(sourceVariant,52,156);
        rejects([&] { originalSourceShader(sourceIdentity,ShaderStage::Vertex,0x1000,sourceVariant,originalCode); },"Source capture exceeded inline reflection bounds");
        titleGuestWriteWord(sourceVariant,52,1); titleGuestWriteWord(sourceVariant,60,65);
        rejects([&] { originalSourceShader(sourceIdentity,ShaderStage::Vertex,0x1000,sourceVariant,originalCode); },"Source capture accepted overflowing registers");
        titleGuestWriteWord(sourceVariant,60,8); titleGuestWriteWord(sourceVariant,56,154); titleGuestWriteWord(sourceVariant,68,7);
        require(originalSourceShader(sourceIdentity,ShaderStage::Vertex,0x1000,sourceVariant,originalCode).parameters[0].reserved == 7,
            "Source diagnostic changed original custom parameter index");
        titleGuestWriteWord(originalCode,8,8);
        rejects([&] { originalSourceShader(sourceIdentity,ShaderStage::Vertex,0x1000,sourceVariant,originalCode); },"Source capture accepted mismatching allocation header");
        // Same code with two distinct keys must be exposed as two matches: no
        // invented single-key resolution is allowed at the native shader bridge.
        auto ambiguous = cache(false, true);
        const size_t record = 12 + 4 + sizeof("fixture/vs") + 4;
        auto extensionBytes = bytes;
        extensionBytes[record + 8] = 5;
        const auto extension = ShaderCache::parse(extensionBytes, ShaderCachePlatform::Windows);
        const auto merged = parsed.withWindowsExtension(extension);
        require(merged.variantCount() == 2 && merged.find("fixture/vs", {0,0,4}).variant &&
            merged.find("fixture/vs", {0,0,5}).variant, "Missing-key native cache extension did not preserve original variants");
        require(parsed.variantCount() == 1, "Native cache extension mutated original cache");
        rejects([&] { parsed.withWindowsExtension(parsed); }, "Native extension replaced an existing original key");
        rejects([&] { parsed.withWindowsExtension(ShaderCache::parse(cache(true),ShaderCachePlatform::Xenon)); }, "Xenon code was accepted as a native PC extension");
        ambiguous[record + 44] = 1;
        const auto both = ShaderCache::parse(ambiguous, ShaderCachePlatform::Windows);
        require(both.matchCode(codeBytes).size() == 2, "Ambiguous bytecode keys were silently collapsed");
        auto originalAliases = cache(true, true);
        originalAliases[record + 44 + 3] = 1;
        const auto xenonAliases = ShaderCache::parse(originalAliases, ShaderCachePlatform::Xenon);
        require(xenonAliases.matchCode(codeBytes).size() == 2, "Original duplicate-code identities disappeared");
        const ShaderSelection firstIdentity{"fixture/vs", {0,0,4}}, secondIdentity{"FIXTURE/VS", {1,0,4}};
        require(xenonAliases.requireCodeVariant(firstIdentity, ShaderStage::Vertex, codeBytes).variant->key == firstIdentity.key,
            "Original shader CPU identity selected the wrong first alias");
        require(xenonAliases.requireCodeVariant(secondIdentity, ShaderStage::Vertex, codeBytes).variant->key == secondIdentity.key,
            "Original shader CPU identity selected the wrong second alias");
        rejects([&] { xenonAliases.requireCodeVariant(firstIdentity, ShaderStage::Pixel, codeBytes); }, "Shader identity with the wrong stage was accepted");
        rejects([&] { xenonAliases.requireCodeVariant({"unknown/vs",{0,0,4}}, ShaderStage::Vertex, codeBytes); }, "Unknown shader name was resolved by its code alias");
        rejects([&] { xenonAliases.requireCodeVariant({"fixture/vs",{2,0,4}}, ShaderStage::Vertex, codeBytes); }, "Unknown feature key was resolved by its code alias");
        rejects([&] { xenonAliases.requireCodeVariant(firstIdentity, ShaderStage::Vertex, codeBytes.first(4)); }, "Truncated code was accepted for an exact shader identity");
        auto differentCode = std::vector<uint8_t>(codeBytes.begin(), codeBytes.end());
        differentCode[0] ^= 1;
        rejects([&] { xenonAliases.requireCodeVariant(firstIdentity, ShaderStage::Vertex, differentCode); }, "Different code was accepted for an exact shader identity");
        rejects([&] { parsed.requireCodeVariant(firstIdentity, ShaderStage::Vertex, codeBytes); }, "Native PC code was treated as original Xenon identity");
        changed = bytes;
        changed[changed.size() - 4] = 0;
        rejects([&] { ShaderCache::parse(changed, ShaderCachePlatform::Windows); }, "Missing shader END was accepted");
        changed = bytes;
        changed[changed.size() - 8] = 1;
        changed[changed.size() - 5] = 0x2f;
        rejects([&] { ShaderCache::parse(changed, ShaderCachePlatform::Windows); }, "Malformed shader profile was accepted");
        changed = bytes;
        changed[12] = 255;
        rejects([&] { ShaderCache::parse(changed, ShaderCachePlatform::Windows); }, "Overlong/truncated shader name was accepted");
        std::vector<uint8_t> declarationBytes = {
            0,0, 0,0, 0,0x2a,0x23,0xb9, 0,0,0,0,
            0,0, 0,12, 0,0x18,0x28,0x86, 0,10,0,0,
            0,0, 0,16, 0,0x2c,0x23,0xa5, 0,5,0,0,
            0,255, 0,0, 255,255,255,255, 0,0,0,0};
        const auto declaration = decodeTitleDeclaration(declarationBytes);
        require(declaration.size() == 4 && declaration[0].Type == D3DDECLTYPE_FLOAT3 &&
            declaration[1].Type == D3DDECLTYPE_D3DCOLOR && declaration[2].Offset == 16,
            "Original title declaration did not preserve fields");
        const std::vector<uint8_t> avatarDeclaration = {
            0,0, 0,0, 0,0x2a,0x23,0xb9, 0,0,0,0,
            0,0, 0,12, 0,0x2a,0x21,0x90, 0,3,0,0,
            0,0, 0,16, 0,0x1a,0x20,0x86, 0,1,0,0,
            0,0, 0,20, 0,0x1a,0x22,0x86, 0,2,0,0,
            0,0, 0,24, 0,0x18,0x28,0x86, 0,10,0,0,
            0,0, 0,28, 0,0x2c,0x23,0x5f, 0,5,0,0,
            0,0, 0,32, 0,0x2c,0x23,0x5f, 0,5,1,0,
            0,0, 0,36, 0,0x2c,0x23,0x5f, 0,5,2,0,
            0,1, 0,0, 0,0x1a,0x23,0xa6, 0,5,6,0,
            0,1, 0,16, 0,0x1a,0x23,0xa6, 0,5,7,0,
            0,1, 0,32, 0,0x1a,0x23,0xa6, 0,5,8,0,
            0,255, 0,0, 255,255,255,255, 0,0,0,0};
        require(isTitleUnavailableAvatarDeclaration(avatarDeclaration), "Original first Avatar unavailable layout was not recognized");
        rejects([&] { decodeTitleDeclaration(avatarDeclaration); }, "Unavailable Avatar layout became a decoded native declaration");
        require(!isTitleUnavailableAvatarDeclaration(declarationBytes), "Ordinary GUI declaration was classified as unavailable Avatar");
        for (size_t index = 0; index < avatarDeclaration.size(); ++index) {
            auto mutated = avatarDeclaration;
            mutated[index] ^= 1;
            require(isTitleUnavailableAvatarDeclaration(mutated) == (index % 12 == 11),
                "Avatar signature ignored a semantic field or consumed undefined padding");
        }
        require(!isTitleUnavailableAvatarDeclaration(std::span(avatarDeclaration).first(avatarDeclaration.size() - 1)),
            "Truncated Avatar declaration was given optional failure semantics");
        auto extendedAvatar = avatarDeclaration;
        extendedAvatar.push_back(0);
        require(!isTitleUnavailableAvatarDeclaration(extendedAvatar), "Extended Avatar signature was accepted");
        RecentFrameTimes recent;
        require(captureRequestEventName(1234,132456789012345678ULL) == L"Local\\DoritosNativeCapture-1234-132456789012345678",
            "Capture event identity disagrees with exact-process helper naming");
        require(captureRequestEventName(1234,1) != captureRequestEventName(1234,2),
            "Capture event naming did not separate reused PID lifetimes");
        require(recent.summary().intervals == 0, "Recent timing reported an interval before real timestamps");
        recent.record(38000000000ULL); // A real loading stall should age out.
        for (uint32_t index = 0; index < 299; ++index) recent.record(16666667);
        require(recent.summary().intervals == 300 && recent.summary().worstNanoseconds == 38000000000ULL,
            "Recent timing omitted a stall still in its consecutive window");
        recent.record(16666667);
        require(recent.summary().intervals == 300 && recent.summary().nanoseconds == 300ULL * 16666667 &&
            recent.summary().worstNanoseconds == 16666667, "Recent timing retained an expired loading stall");
        recent.reset(); // The backend calls this on clear-only Present.
        require(recent.summary().intervals == 0 && recent.summary().nanoseconds == 0 && recent.summary().worstNanoseconds == 0,
            "Interrupted recent timing retained stale intervals");
        recent.record(20000000);
        require(recent.summary().intervals == 1 && recent.summary().nanoseconds == 20000000 &&
            recent.summary().worstNanoseconds == 20000000, "Recent timing mixed intervals across an interrupted sequence");
        const std::vector<uint8_t> guestVertex = {0x3f,0x80,0,0, 0xc0,0,0,0, 0,0,0,0,
            0xff,0x12,0x34,0x56, 0x3f,0,0,0, 0x3f,0x80,0,0};
        const auto native = convertTitleVertexStream(guestVertex, 24, declaration);
        float position[3]; uint32_t color;
        std::memcpy(position, native.data(), 12);
        std::memcpy(&color, native.data() + 12, 4);
        require(position[0] == 1 && position[1] == -2 && position[2] == 0 && color == 0xff123456,
            "Native vertex endian conversion corrupted position/color");
        // Original BoneModel declaration82385FD8, format16 and stride84:
        // ordinary float weights/indices; this is distinct from SDK Avatar's
        // unverified packed mesh and extra palette fetch stream.
        const std::vector<uint8_t> boneDeclarationBytes = {
            0,0, 0,0, 0,0x2a,0x23,0xb9, 0,0,0,0,
            0,0, 0,12, 0,0x1a,0x23,0xa6, 0,1,0,0,
            0,0, 0,28, 0,0x1a,0x23,0xa6, 0,2,0,0,
            0,0, 0,44, 0,0x2a,0x23,0xb9, 0,3,0,0,
            0,0, 0,56, 0,0x1a,0x23,0xa6, 0,6,0,0,
            0,0, 0,72, 0,0x18,0x28,0x86, 0,10,0,0,
            0,0, 0,76, 0,0x2c,0x23,0xa5, 0,5,0,0,
            0,255, 0,0, 255,255,255,255, 0,0,0,0};
        const auto boneDeclaration = decodeTitleDeclaration(boneDeclarationBytes);
        require(boneDeclaration.size() == 8 && boneDeclaration[1].Usage == D3DDECLUSAGE_BLENDWEIGHT &&
            boneDeclaration[2].Type == D3DDECLTYPE_FLOAT4 && boneDeclaration[2].Usage == D3DDECLUSAGE_BLENDINDICES &&
            boneDeclaration[5].Offset == 72 && boneDeclaration[6].Offset == 76,
            "Original BoneModel declaration did not preserve its seven fields");
        std::vector<uint8_t> boneGuest;
        for (uint32_t vertex = 0; vertex < 2; ++vertex)
            for (uint32_t component = 0; component < 21; ++component)
                append(boneGuest, component == 18 ? 0xff123456 : 0x3f000000 + vertex * 0x00800000 + component * 0x00010000, true);
        const auto boneNative = convertTitleVertexStream(boneGuest, 84, boneDeclaration);
        for (uint32_t vertex = 0; vertex < 2; ++vertex) for (uint32_t component = 0; component < 21; ++component) {
            uint32_t value = 0;
            std::memcpy(&value, boneNative.data() + vertex * 84 + component * 4, 4);
            require(value == (component == 18 ? 0xff123456 : 0x3f000000 + vertex * 0x00800000 + component * 0x00010000),
                "BoneModel stream conversion changed a real layout component");
        }
        const std::vector<uint8_t> indices = {0,1, 0,2, 0x12,0x34};
        require(convertTitleTriangleIndices(indices) == std::vector<uint16_t>({1, 2, 0x1234}), "Native index endian conversion failed");
        // Original GUI batch823818E0 uses four fields across a32-byte vertex:
        // position, packed color, primary UV and secondary float2 at+24.
        auto guiDeclarationBytes = declarationBytes;
        guiDeclarationBytes.insert(guiDeclarationBytes.end() - 12,
            {0,0, 0,24, 0,0x2c,0x23,0xa5, 0,5,1,0});
        const auto guiDeclaration = decodeTitleDeclaration(guiDeclarationBytes);
        auto guiVertex = guestVertex;
        guiVertex.insert(guiVertex.end(),{0x40,0,0,0, 0xc0,0x40,0,0});
        const auto guiNative = convertTitleVertexStream(guiVertex,32,guiDeclaration);
        float extra[2]{};
        std::memcpy(extra,guiNative.data() + 24,8);
        require(guiNative.size() == 32 && extra[0] == 2 && extra[1] == -3,
            "Original immediate GUI secondary float2 was not preserved");
        const std::vector<uint8_t> immediateIndices{0,10,0,11,0,12,0,12,0,11,0,13};
        require(convertTitleImmediateIndices(immediateIndices,10,4) == std::vector<uint16_t>({0,1,2,2,1,3}),
            "Original indexed-UP copied minVertex range lost its negative-base equivalent");
        require(convertTitleImmediateIndices(std::span(immediateIndices).first(6),0,14) == std::vector<uint16_t>({10,11,12}),
            "Original GUI minVertex0 indices were changed");
        rejects([&] { convertTitleImmediateIndices(immediateIndices,11,4); }, "Immediate index below minVertex was accepted");
        rejects([&] { convertTitleImmediateIndices(immediateIndices,10,3); }, "Immediate index beyond copied vertices was accepted");
        rejects([&] { convertTitleImmediateIndices(std::span(immediateIndices).first(5),10,4); }, "Truncated immediate BE16 index was accepted");
        rejects([&] { convertTitleImmediateIndices(immediateIndices,UINT32_MAX,4); }, "Immediate minVertex overflow was accepted");
        rejects([&] { convertTitleImmediateIndices(immediateIndices,65535,2); }, "Immediate vertex extent exceeded BE16 addressing");
        rejects([&] { convertTitleImmediateIndices(immediateIndices,0,0); }, "Empty immediate vertex extent was accepted");
        const auto strip = decodeTitleIndexedDraw(6, 5);
        require(strip.topology == D3DPT_TRIANGLESTRIP && strip.triangles == 3,
            "Original strip enum6 was cast to native triangle fan or lost itsN+2 extent");
        const auto list = decodeTitleIndexedDraw(4, 6);
        require(list.topology == D3DPT_TRIANGLELIST && list.triangles == 2,
            "Existing original triangle list mapping changed");
        const std::vector<uint8_t> stripGuest = {0,99, 0,0, 0,1, 0,2, 0,3, 0,4};
        const auto stripIndices = convertTitleTriangleIndices(stripGuest);
        require(stripIndices == std::vector<uint16_t>({99,0,1,2,3,4}),
            "Original strip index order was changed, invalidating native alternating winding");
        require(validateIndexedTriangleRange(strip.topology,2,5,7,stripIndices,1,strip.triangles) == 5,
            "Native strip extent/nonzero base vertex did not preserve the original draw range");
        const std::vector<uint16_t> listIndices{0,1,2,2,1,3};
        require(validateIndexedTriangleRange(list.topology,3,4,7,listIndices,0,list.triangles) == 6,
            "Existing indexed list extent/nonzero base vertex changed");
        const std::vector<uint16_t> degenerateStrip{0,1,2,2,3,4};
        require(validateIndexedTriangleRange(D3DPT_TRIANGLESTRIP,0,5,5,degenerateStrip,0,4) == 6,
            "Valid authored strip degenerates were rejected or removed");
        rejects([&] { decodeTitleIndexedDraw(5,5); }, "Unverified Xbox triangle fan was accepted");
        rejects([&] { decodeTitleIndexedDraw(6,2); }, "Strip without a complete triangle was accepted");
        rejects([&] { decodeTitleIndexedDraw(4,4); }, "Partial original triangle list was accepted");
        rejects([&] { validateIndexedTriangleRange(strip.topology,3,5,7,stripIndices,1,3); }, "Strip base vertex exceeded the buffer");
        rejects([&] { validateIndexedTriangleRange(strip.topology,-1,5,7,stripIndices,1,3); }, "Unverified negative title base vertex was accepted");
        rejects([&] { validateIndexedTriangleRange(strip.topology,0,4,7,stripIndices,1,3); }, "Strip index referenced beyond the vertex range");
        rejects([&] { validateIndexedTriangleRange(strip.topology,0,5,7,stripIndices,2,3); }, "Strip consumed beyond available indices");
        rejects([&] { validateIndexedTriangleRange(strip.topology,0,5,7,stripIndices,1,UINT32_MAX); }, "Strip extent overflow was accepted");
        rejects([&] { validateIndexedTriangleRange(list.topology,0,5,7,listIndices,0,UINT32_MAX); }, "List extent multiplication overflow was accepted");
        rejects([&] { validateIndexedTriangleRange(D3DPT_TRIANGLEFAN,0,5,7,stripIndices,1,3); }, "Unverified native fan was accepted");
        rejects([&] { decodeTitleDeclaration(std::span(declarationBytes).first(36)); }, "Missing declaration END was accepted");
        declarationBytes[6] = 0x22;
        rejects([&] { decodeTitleDeclaration(declarationBytes); }, "Unknown Xbox declaration type was accepted");
        rejects([&] { convertTitleVertexStream(guestVertex, 20, declaration); }, "Invalid title vertex stride was accepted");
        rejects([&] { convertTitleTriangleIndices(std::span(indices).first(4)); }, "Incomplete triangle indices were accepted");
        std::printf("Native graphics contracts passed: %zu checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native graphics contract failed: %s\n", error.what());
        return 1;
    }
}
