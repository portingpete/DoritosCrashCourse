#include "scene.h"
#include "asset_reader.h"
#include <algorithm>
#include <set>
#include <utility>

namespace Doritos::Engine {
namespace {
float sceneNumber(AssetReader& input) {
    auto value=input.number();
    need(std::abs(value)<=1e9f,"Native scene number outside budget");
    return value;
}
XMFLOAT3 sceneVector(AssetReader& input) {
    return {sceneNumber(input),sceneNumber(input),sceneNumber(input)};
}
XMFLOAT4X4 sceneMatrix(AssetReader& input) {
    XMFLOAT4X4 matrix;
    for(auto& row:matrix.m)for(auto& value:row)value=sceneNumber(input);
    need(matrix._14==0&&matrix._24==0&&matrix._34==0&&matrix._44==1,
         "Native scene matrix is not affine");
    return matrix;
}
void requireInvertible(const XMFLOAT4X4& matrix) {
    float determinant=DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(DirectX::XMLoadFloat4x4(&matrix)));
    need(std::isfinite(determinant)&&std::abs(determinant)>1e-12f,"Singular native scene transform");
}
bool scriptField(const std::string& field) {
    return field=="Command"||field=="OnRespawnScript"||field=="OnActivate"||
           field=="LeftTriggerCommand"||field=="AllTouchersLeftTriggerCommand";
}
}

Scene loadScene(const std::filesystem::path& path) {
    AssetReader input(path);Scene scene;
    need(input.read<uint32_t>()==0x31534e44&&input.read<uint32_t>()==1,"Unsupported independent scene asset");
    auto courseCount=input.count(15);need(courseCount>0,"Empty native scene course catalog");
    std::array<bool,15> used{};
    uint64_t totalRecords=0,totalResources=0,totalScripts=0,totalBoxes=0;
    for(uint32_t i=0;i<courseCount;++i) {
        CourseScene course;course.id=input.count(14);
        need(!used[course.id],"Duplicate native scene course ID");used[course.id]=true;
        course.name=input.string();auto chunks=input.count(128);
        need(chunks>=2,"Native scene course needs start and end chunks");
        for(uint32_t j=0;j<chunks;++j) {
            SceneChunk chunk;chunk.id=input.count(chunks-1);chunk.serializedOrdinal=input.count(chunks-1);
            need(chunk.id==j&&chunk.serializedOrdinal==chunks-1-j,"Invalid native scene chunk order");
            chunk.name=input.string();chunk.source=input.string();
            chunk.offset=sceneVector(input);chunk.start=sceneVector(input);chunk.end=sceneVector(input);
            course.chunks.push_back(std::move(chunk));
        }
        need(course.chunks.front().name=="common/start_chunk"&&course.chunks.back().name=="common/end_chunk",
             "Unexpected native scene course endpoints");
        auto records=input.count(4096);totalRecords+=records;
        need(totalRecords<=65536,"Native scene records exceed budget");
        std::set<std::pair<uint32_t,uint32_t>> recordLocations;
        for(uint32_t j=0;j<records;++j) {
            SceneRecord record;record.id=input.count(records-1);record.chunk=input.count(chunks-1);
            record.ordinal=input.count(1000000);
            need(record.id==j&&recordLocations.emplace(record.chunk,record.ordinal).second,
                 "Duplicate native scene record identity");
            record.source=input.string();record.type=input.string();record.name=input.string();
            need(record.source==course.chunks[record.chunk].source,"Native scene record source differs from chunk");
            need(record.type=="STARTPOS"||record.type=="TRIGGER","Unsupported native scene record type");
            record.position=sceneVector(input);record.transform=sceneMatrix(input);
            need(record.position.x==record.transform._41&&record.position.y==record.transform._42&&record.position.z==record.transform._43,
                 "Native scene position differs from transform");
            record.enabled=input.count(1)!=0;record.mode=input.read<int32_t>();
            record.maxCommands=input.read<int32_t>();record.commandCounter=input.read<int32_t>();
            record.waitTime={sceneNumber(input),sceneNumber(input)};
            record.useTimePerActor=input.count(1)!=0;record.sendToToucher=input.count(1)!=0;
            record.supported=input.count(1)!=0;record.exclusion=input.string();
            need(record.supported==record.exclusion.empty(),"Native scene exclusion status is inconsistent");
            if(record.supported)requireInvertible(record.transform);
            auto resources=input.count(512);totalResources+=resources;
            need(totalResources<=1048576,"Native scene resources exceed budget");
            for(uint32_t k=0;k<resources;++k)record.resources.push_back(input.string());
            auto scripts=input.count(128);totalScripts+=scripts;
            need(totalScripts<=262144,"Native scene script references exceed budget");
            for(uint32_t k=0;k<scripts;++k) {
                SceneScriptReference reference;
                reference.source=input.string();reference.type=input.string();reference.actor=input.string();reference.field=input.string();
                reference.ordinal=input.count(1000000);reference.index=input.count(65535);reference.mode=input.count(2);
                reference.flags=input.read<uint32_t>();reference.catalogIndex=input.read<int32_t>();
                need(scriptField(reference.field)&&reference.mode>=1,"Invalid native scene script reference");
                need((reference.mode==1&&reference.catalogIndex==-1)||
                     (reference.mode==2&&reference.catalogIndex>=0&&reference.catalogIndex<16384),
                     "Native scene script catalog index is invalid");
                need(reference.mode!=1||!record.supported,"Text script requires explicit native scene exclusion");
                record.scripts.push_back(std::move(reference));
            }
            auto boxes=input.count(512);totalBoxes+=boxes;
            need(totalBoxes<=262144,"Native scene collision boxes exceed budget");
            need(record.supported||boxes==0,"Excluded native scene record contains collision boxes");
            std::set<uint32_t> boxResources;
            for(uint32_t k=0;k<boxes;++k) {
                SceneTriggerBox box;box.resource=input.read<uint32_t>();box.primitiveType=input.read<uint32_t>();
                box.collisionMask=input.read<uint32_t>();box.collisionGroup=input.read<uint32_t>();box.name=input.string();
                need(box.resource<resources&&boxResources.insert(box.resource).second&&box.name==record.resources[box.resource]&&
                     box.name.starts_with("CLP3:")&&box.primitiveType==2,"Invalid native scene collision resource");
                box.minimum=sceneVector(input);box.maximum=sceneVector(input);box.transform=sceneMatrix(input);
                need(box.minimum.x<box.maximum.x&&box.minimum.y<box.maximum.y&&box.minimum.z<box.maximum.z,
                     "Invalid native scene collision bounds");
                requireInvertible(box.transform);record.boxes.push_back(std::move(box));
            }
            course.records.push_back(std::move(record));
        }
        scene.courses.push_back(std::move(course));
    }
    std::sort(scene.courses.begin(),scene.courses.end(),[](auto& a,auto& b){return a.id<b.id;});
    input.end();return scene;
}
}
