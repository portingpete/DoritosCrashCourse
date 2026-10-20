#include "actors.h"
#include "asset_reader.h"
#include <algorithm>
#include <set>
#include <windows.h>

namespace Doritos::Engine {
namespace {
XMFLOAT4X4 affine(AssetReader& input) {
    XMFLOAT4X4 m;for(auto& row:m.m)for(float& v:row)v=input.number();
    need(m._14==0&&m._24==0&&m._34==0&&m._44==1,"Actor matrix is not affine");
    const float det=DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(DirectX::XMLoadFloat4x4(&m)));
    need(std::isfinite(det)&&std::abs(det)>1e-12f,"Singular actor transform");return m;
}
bool relativeAsset(const std::string& name,const char* prefix,const char* extension=nullptr) {
    const std::filesystem::path path(name);
    return !name.empty()&&!path.is_absolute()&&!path.has_root_name()&&name.starts_with(prefix)&&
        name.find("..") == std::string::npos&&name.find('\\')==std::string::npos&&
        (!extension||path.extension()==extension);
}
void localFile(const std::filesystem::path& path) {
    // A relative name alone cannot constrain directory junctions or symlinks.
    // Validate every ancestor as well as the leaf before opening model data.
    auto part=std::filesystem::absolute(path);
    while(!part.empty()) {
        const auto flags=GetFileAttributesW(part.c_str());
        need(flags!=INVALID_FILE_ATTRIBUTES&&!(flags&FILE_ATTRIBUTE_REPARSE_POINT),"Actor asset contains a link or missing component");
        auto parent=part.parent_path();if(parent==part)break;part=std::move(parent);
    }
}
}
Actors loadActors(const std::filesystem::path& path) {
    localFile(path);AssetReader input(path);Actors result;
    need(input.read<uint32_t>()==0x31414e44&&input.read<uint32_t>()==1,"Unsupported native actor asset");
    const auto materials=input.count(4096);
    for(uint32_t i=0;i<materials;++i) {
        Material m;m.name=input.string();m.texture=input.string();
        need(m.texture.empty()||relativeAsset(m.texture,"textures/"),"Actor texture outside asset root");
        m.color={input.number(),input.number(),input.number(),input.number()};
        for(float v:{m.color.x,m.color.y,m.color.z,m.color.w})need(v>=0&&v<=1,"Actor material color outside range");
        m.lit=input.count(1)!=0;m.transparent=input.count(1)!=0;m.uvSpeed={input.number(),input.number()};
        result.materials.push_back(std::move(m));
    }
    const auto models=input.count(256);
    for(uint32_t i=0;i<models;++i) {
        ActorModel m;m.name=input.string();m.path=input.string();
        // Theme variants may share geometry while mapping different materials.
        need(relativeAsset(m.path,"actors/",".dne"),"Invalid actor model path");
        const auto submeshes=input.count(4);need(submeshes>0,"Actor model has no submesh");
        for(uint32_t j=0;j<submeshes;++j) {
            auto material=input.read<uint32_t>();need(material<materials,"Actor submesh material absent");m.materials.push_back(material);
        }
        const auto events=input.count(1024);
        for(uint32_t j=0;j<events;++j) {
            ActorTimedEvent event;event.animation=input.string();event.time=input.number();event.command=input.string();
            need(event.time>=0,"Actor timed event has negative time");m.events.push_back(std::move(event));
        }
        auto modelPath=path.parent_path()/std::filesystem::path(m.path);localFile(modelPath);m.model=loadModel(modelPath);
        need(m.model.courses.empty()&&m.model.meshes.size()==submeshes,"Actor model structure differs from catalog");
        for(size_t j=0;j<m.model.meshes.size();++j)need(m.model.meshes[j].texture==j,"Actor submesh slot differs from catalog");
        for(const auto& event:m.events) {
            auto clip=std::find_if(m.model.clips.begin(),m.model.clips.end(),[&](const Clip& c){return c.name==event.animation;});
            need(clip!=m.model.clips.end()&&event.time<=clip->duration,"Actor event outside its animation");
        }
        result.models.push_back(std::move(m));
    }
    const auto courses=input.count(15);need(courses==15,"Native actors require all 15 courses");
    std::array<bool,15> used{};uint64_t total=0;
    for(uint32_t i=0;i<courses;++i) {
        CourseActors course;course.id=input.count(14);need(!used[course.id],"Duplicate actor course");used[course.id]=true;
        const auto instances=input.count(4096);total+=instances;need(total<=16384,"Too many native actors");
        std::set<std::pair<uint32_t,uint32_t>> identities;
        for(uint32_t j=0;j<instances;++j) {
            HammerInstance h;h.chunk=input.count(4095);h.ordinal=input.count(65535);h.name=input.string();h.model=input.read<uint32_t>();
            need(h.model<models&&!h.name.empty()&&identities.emplace(h.chunk,h.ordinal).second,"Invalid or duplicate native actor identity");
            h.actorWorld=affine(input);h.modelLocal=affine(input);h.animation=input.string();h.animationResource=input.count(23);
            h.loop=input.count(1)!=0;h.startMoving=input.count(1)!=0;h.wait=input.number();h.initialWait=input.number();
            need(h.wait>=0&&h.initialWait>=0,"Negative actor wait time");
            const auto& model=result.models[h.model].model;
            need(std::any_of(model.clips.begin(),model.clips.end(),[&](const Clip& c){return c.name==h.animation;}),"Actor animation absent");
            const auto scripts=input.count(8);std::set<std::string> fields;
            for(uint32_t k=0;k<scripts;++k) {
                SceneScriptReference reference;reference.field=input.string();auto catalog=input.read<uint32_t>();
                need(catalog==UINT32_MAX||catalog<=INT32_MAX,"Actor script catalog index outside budget");
                reference.catalogIndex=catalog==UINT32_MAX?-1:int32_t(catalog);reference.source=input.string();reference.type=input.string();reference.actor=input.string();
                reference.ordinal=input.count(65535);reference.index=input.count(65535);
                need(fields.insert(reference.field).second,"Duplicate native actor script field");h.scripts.push_back(std::move(reference));
            }
            const auto boxes=input.count(24);std::set<uint32_t> resources;
            for(uint32_t k=0;k<boxes;++k) {
                ActorBox b;b.resource=input.count(23);b.bone=input.read<uint32_t>();b.local=affine(input);b.minimum=input.vector();b.maximum=input.vector();
                b.group=input.read<uint32_t>();b.mask=input.read<uint32_t>();
                need(b.bone<model.bones.size()&&resources.insert(b.resource).second&&b.resource!=h.animationResource,"Invalid actor box resource or bone");
                need(b.minimum.x<b.maximum.x&&b.minimum.y<b.maximum.y&&b.minimum.z<b.maximum.z,"Empty actor box");h.boxes.push_back(b);
            }
            course.hammers.push_back(std::move(h));
        }
        result.courses.push_back(std::move(course));
    }
    std::sort(result.courses.begin(),result.courses.end(),[](const auto& a,const auto& b){return a.id<b.id;});input.end();return result;
}
XMFLOAT4X4 actorModelWorld(const HammerInstance& instance) {
    XMFLOAT4X4 result;DirectX::XMStoreFloat4x4(&result,DirectX::XMLoadFloat4x4(&instance.modelLocal)*DirectX::XMLoadFloat4x4(&instance.actorWorld));return result;
}
XMFLOAT4X4 actorBoxWorld(const HammerInstance& instance,const ActorBox& box,const std::vector<XMFLOAT4X4>& absoluteBones) {
    need(box.bone<absoluteBones.size(),"Actor attachment bone absent");
    auto world=actorModelWorld(instance);XMFLOAT4X4 result;
    DirectX::XMStoreFloat4x4(&result,DirectX::XMLoadFloat4x4(&box.local)*DirectX::XMLoadFloat4x4(&absoluteBones[box.bone])*DirectX::XMLoadFloat4x4(&world));return result;
}
HammerController::HammerController(const HammerInstance& instance,float animationDuration):
    startsMoving(instance.startMoving),loops(instance.loop),initial(instance.initialWait),wait(instance.wait),duration(animationDuration) {
    need(!instance.animation.empty()&&std::isfinite(duration)&&duration>0&&std::isfinite(initial)&&initial>=0&&std::isfinite(wait)&&wait>=0&&std::isfinite(duration+wait),"Invalid hammer controller parameters");reset();
}
void HammerController::reset() {
    current.firstPending=true;current.countdown=std::max(initial,.001f);current.running=startsMoving;
}
void HammerController::pause() { current.running=false; }
void HammerController::resume() { current.running=true; }
bool HammerController::tick(float seconds) {
    need(std::isfinite(seconds)&&seconds>=0&&seconds<=1,"Invalid hammer update delta");bool played=false;
    if(current.running&&current.countdown>0) {
        current.countdown-=seconds;
        if((current.countdown<=0&&loops)||current.firstPending) {
            current.playing=true;current.animationTime=0;current.firstPending=false;
            current.countdown=duration+wait;++current.plays;played=true;
        }
    }
    // The original action is auto-locked at its final key. Controller Pause
    // only gates the restart timer; it does not pause the skeletal resource.
    if(current.playing)current.animationTime=std::min(duration,current.animationTime+seconds);
    return played;
}
struct CourseActorSystem::Impl {
    const Actors& assets;const CourseActors& course;
    std::vector<HammerController> controllers;
    std::vector<ActorNotice> notices;
    std::set<std::pair<uint32_t,std::string>> noticed;
    Impl(const Actors& data,uint32_t id):assets(data),course(data.courses.at(id)) {
        need(course.id==id,"Actor course order differs from identity");
        for(const auto& actor:course.hammers)controllers.emplace_back(actor,clip(actor).duration);
    }
    const Clip& clip(const HammerInstance& actor) const {
        const auto& model=assets.models.at(actor.model).model;
        auto found=std::find_if(model.clips.begin(),model.clips.end(),[&](const Clip& c){return c.name==actor.animation;});
        need(found!=model.clips.end(),"Hammer animation missing");return *found;
    }
    void notice(uint32_t actor,const std::string& field,const std::string& detail) {
        if(noticed.emplace(actor,field+":"+detail).second)notices.push_back({actor,field,detail});
    }
    struct Object final:ScriptObject {
        std::weak_ptr<Impl> owner;size_t actor;
        Object(std::shared_ptr<Impl> value,size_t index):owner(value),actor(index) {}
        std::shared_ptr<Impl> live() const { auto value=owner.lock();need(bool(value),"Expired native actor object");return value; }
        ScriptValue get(std::string_view name) const override {
            if(name=="Name")return live()->course.hammers.at(actor).name;return ScriptObject::get(name);
        }
        bool hasMethod(std::string_view name) const override { return name=="Reset"||name=="Pause"||name=="Resume"; }
        ScriptValue call(std::string_view name,std::span<const ScriptValue> arguments) override {
            if(!hasMethod(name))return ScriptObject::call(name,arguments);
            need(arguments.empty(),"Hammer command requires no arguments");auto value=live();auto& controller=value->controllers.at(actor);
            if(name=="Reset")controller.reset();else if(name=="Pause")controller.pause();else controller.resume();
            return name!="Pause";
        }
    };
};
CourseActorSystem::CourseActorSystem(const Actors& assets,uint32_t course):impl(std::make_shared<Impl>(assets,course)) {}
void CourseActorSystem::tick(float seconds) {
    need(std::isfinite(seconds)&&seconds>=0&&seconds<=1,"Invalid actor system delta");
    for(uint32_t i=0;i<impl->controllers.size();++i) {
        auto& controller=impl->controllers[i];const auto before=controller.state();
        const bool played=controller.tick(seconds);const auto& actor=impl->course.hammers[i];
        if(played)for(const auto& script:actor.scripts)if(script.field=="OnPlayAnimationScript")
            impl->notice(i,script.field,"Authored actor script execution is pending");
        const auto& after=controller.state();
        if(after.playing)for(const auto& event:impl->assets.models[actor.model].events) {
            if(event.animation==actor.animation&&(played?event.time<=after.animationTime:event.time>before.animationTime&&event.time<=after.animationTime))
                impl->notice(i,"TimedEvent",event.command);
        }
    }
}
ScriptObjectPtr CourseActorSystem::lookup(uint32_t chunk,std::string_view name) const {
    size_t found=SIZE_MAX,preferred=SIZE_MAX;bool ambiguous=false,preferredAmbiguous=false;
    for(size_t i=0;i<impl->course.hammers.size();++i) {
        const auto& actor=impl->course.hammers[i];if(actor.name!=name)continue;
        if(actor.chunk==chunk) { if(preferred!=SIZE_MAX)preferredAmbiguous=true;else preferred=i; }
        if(found!=SIZE_MAX)ambiguous=true;else found=i;
    }
    if(preferredAmbiguous)return {};
    if(preferred!=SIZE_MAX)return std::make_shared<Impl::Object>(impl,preferred);
    if(found==SIZE_MAX||ambiguous)return {};return std::make_shared<Impl::Object>(impl,found);
}
std::vector<HammerState> CourseActorSystem::states() const {
    std::vector<HammerState> result;for(const auto& controller:impl->controllers)result.push_back(controller.state());return result;
}
std::vector<XMFLOAT4X4> CourseActorSystem::pose(size_t actor) const {
    const auto& instance=impl->course.hammers.at(actor);const auto& state=impl->controllers.at(actor).state();
    return samplePose(impl->assets.models.at(instance.model).model,state.playing?&impl->clip(instance):nullptr,state.animationTime,false);
}
std::vector<XMFLOAT4X4> CourseActorSystem::boxTransforms(size_t actor) const {
    const auto& instance=impl->course.hammers.at(actor);const auto& state=impl->controllers.at(actor).state();
    auto bones=sampleBoneTransforms(impl->assets.models.at(instance.model).model,state.playing?&impl->clip(instance):nullptr,state.animationTime,false);
    std::vector<XMFLOAT4X4> result;for(const auto& box:instance.boxes)result.push_back(actorBoxWorld(instance,box,bones));return result;
}
const std::vector<ActorNotice>& CourseActorSystem::notices() const { return impl->notices; }
}
