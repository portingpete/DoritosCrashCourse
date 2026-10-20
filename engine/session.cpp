#include "session.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Doritos::Engine {
namespace {
void requireSession(bool condition,const char* text) { if(!condition)throw std::runtime_error(text); }
double number(const ScriptValue& value) {
    auto result=std::get_if<double>(&value);requireSession(result&&std::isfinite(*result),"Session method requires a finite number");return *result;
}
const std::string& text(const ScriptValue& value) {
    auto result=std::get_if<std::string>(&value);requireSession(result,"Session method requires text");return *result;
}
bool boolean(const ScriptValue& value) {
    auto result=std::get_if<bool>(&value);requireSession(result,"Session method requires a boolean");return *result;
}
std::optional<size_t> resourceIndex(std::string_view value) {
    size_t index=0;auto parsed=std::from_chars(value.data(),value.data()+value.size(),index);
    if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||index>=24)return {};
    return index;
}
struct Vector2Object final:ScriptObject {
    XMFLOAT2 value;
    explicit Vector2Object(XMFLOAT2 point):value(point) {}
    ScriptValue get(std::string_view name) const override {
        if(name=="x")return double(value.x);if(name=="y")return double(value.y);return ScriptObject::get(name);
    }
};
struct FunctionObject final:ScriptObject {
    std::map<std::string,ScriptFunction,std::less<>> methods;
    std::map<std::string,ScriptValue,std::less<>> fields;
    ScriptValue get(std::string_view name) const override {
        auto found=fields.find(name);return found==fields.end()?ScriptObject::get(name):found->second;
    }
    bool hasMethod(std::string_view name) const override { return methods.contains(name); }
    ScriptValue call(std::string_view name,std::span<const ScriptValue> arguments) override {
        auto found=methods.find(name);return found==methods.end()?ScriptObject::call(name,arguments):found->second(arguments);
    }
};
// Exact closest point to the affine box, including shear from composed authored
// scales/rotations. Enumerate the 27 face/edge/vertex/free active sets.
bool ellipsoidTouchesBox(XMFLOAT3 base,XMFLOAT3 feetOffset,XMFLOAT3 feetRadii,const SceneTriggerBox& box) {
    constexpr double radius=1;
    double radii[3]={feetRadii.x,feetRadii.y,feetRadii.z};
    double minimum[3]={box.minimum.x,box.minimum.y,box.minimum.z};
    double maximum[3]={box.maximum.x,box.maximum.y,box.maximum.z};
    double center[3]={box.transform._41,box.transform._42,box.transform._43};
    double axes[3][3]{};
    for(unsigned axis=0;axis<3;++axis)for(unsigned component=0;component<3;++component) {
        center[component]+=(minimum[axis]+maximum[axis])*.5*box.transform.m[axis][component];
        axes[axis][component]=(maximum[axis]-minimum[axis])*.5*box.transform.m[axis][component];
    }
    double delta[3]={(base.x+feetOffset.x-center[0])/radii[0],(base.y+feetOffset.y-center[1])/radii[1],
                     (base.z+feetOffset.z-center[2])/radii[2]};
    for(auto& axis:axes)for(unsigned component=0;component<3;++component)axis[component]/=radii[component];
    for(unsigned component=0;component<3;++component) {
        double reach=radius;for(auto& axis:axes)reach+=std::abs(axis[component]);
        if(std::abs(delta[component])>reach)return false;
    }
    for(unsigned active=0;active<27;++active) {
        double coefficients[3]{},residual[3]={delta[0],delta[1],delta[2]};
        unsigned free[3]{},freeCount=0,code=active;
        for(unsigned axis=0;axis<3;++axis,code/=3) {
            unsigned state=code%3;
            if(!state)free[freeCount++]=axis;
            else {
                coefficients[axis]=state==1?-1:1;
                for(unsigned component=0;component<3;++component)residual[component]-=coefficients[axis]*axes[axis][component];
            }
        }
        double augmented[3][4]{};
        for(unsigned row=0;row<freeCount;++row) {
            for(unsigned col=0;col<freeCount;++col)for(unsigned component=0;component<3;++component)
                augmented[row][col]+=axes[free[row]][component]*axes[free[col]][component];
            for(unsigned component=0;component<3;++component)augmented[row][freeCount]+=axes[free[row]][component]*residual[component];
        }
        bool valid=true;
        for(unsigned col=0;col<freeCount;++col) {
            unsigned pivot=col;for(unsigned row=col+1;row<freeCount;++row)
                if(std::abs(augmented[row][col])>std::abs(augmented[pivot][col]))pivot=row;
            if(std::abs(augmented[pivot][col])<1e-20) { valid=false;break; }
            for(unsigned k=col;k<=freeCount;++k)std::swap(augmented[col][k],augmented[pivot][k]);
            double divisor=augmented[col][col];for(unsigned k=col;k<=freeCount;++k)augmented[col][k]/=divisor;
            for(unsigned row=0;row<freeCount;++row)if(row!=col) {
                double factor=augmented[row][col];for(unsigned k=col;k<=freeCount;++k)augmented[row][k]-=factor*augmented[col][k];
            }
        }
        if(!valid)continue;
        for(unsigned axis=0;axis<freeCount;++axis) {
            auto value=augmented[axis][freeCount];
            if(value < -1-1e-9||value > 1+1e-9) { valid=false;break; }
            coefficients[free[axis]]=value;
        }
        if(!valid)continue;
        double distance=0;
        for(unsigned component=0;component<3;++component) {
            double value=delta[component];for(unsigned axis=0;axis<3;++axis)value-=coefficients[axis]*axes[axis][component];
            distance+=value*value;
        }
        if(distance<=radius*radius+1e-10)return true;
    }
    return false;
}
bool sameLocation(const SceneScriptReference& reference,const ScriptChunk& chunk) {
    return reference.source==chunk.source&&reference.type==chunk.type&&reference.actor==chunk.actor&&reference.field==chunk.field&&
           reference.ordinal==chunk.ordinal&&reference.index==chunk.index;
}
}

struct CourseSession::Impl {
    const CourseScene& scene;
    const std::vector<ScriptChunk>& scripts;
    unsigned theme=0;
    XMFLOAT3 feetOffset{},feetRadii{};
    uint32_t randomState=0;
    ScriptVm vm;
    ScriptActorLookup actorLookup;
    static constexpr uint64_t fighterOwner=UINT64_MAX;
    std::vector<SessionTriggerState> triggerState;
    std::vector<SessionIssue> issueState;
    std::vector<SessionHudControl> hudState;
    std::map<uint64_t,std::pair<size_t,std::string>> taskOwners;
    std::vector<std::pair<size_t,std::string>> pendingScripts;
    std::optional<size_t> checkpoint;
    double elapsedTime=0,clock=0;
    uint64_t respawns=0,checkpoints=0;
    bool crossed=false,cinematic=false,respawnRequested=false;
    ScriptObjectPtr fighter;

    struct ResourcesObject final:ScriptObject {
        Impl& owner;size_t record;
        ResourcesObject(Impl& session,size_t index):owner(session),record(index) {}
        ScriptValue get(std::string_view name) const override {
            auto index=resourceIndex(name);auto& resources=owner.triggerState[record].resources;
            if(index&&*index<resources.size())return resources[*index];
            return ScriptObject::get(name);
        }
        void set(std::string_view name,const ScriptValue& value) override {
            auto index=resourceIndex(name);auto& resources=owner.triggerState[record].resources;
            requireSession(index.has_value(),"Native resource write is outside the 24-resource array");
            auto replacement=text(value);
            requireSession(!replacement.starts_with("CLP3:")&&(*index>=resources.size()||!resources[*index].starts_with("CLP3:")),
                           "Native collision resource replacement is not implemented");
            if(*index>=resources.size())resources.resize(*index+1);
            resources[*index]=replacement;
        }
    };
    struct ActorObject final:ScriptObject {
        Impl& owner;size_t record;ScriptObjectPtr collided,resources;
        ActorObject(Impl& session,size_t index,ScriptObjectPtr contact):owner(session),record(index),collided(std::move(contact)),
            resources(std::make_shared<ResourcesObject>(session,index)) {}
        ScriptValue get(std::string_view name) const override {
            if(name=="Name")return owner.scene.records[record].name;
            if(name=="Enabled")return owner.triggerState[record].enabled;
            if(name=="Resources")return resources;
            if(name=="collided")return collided;
            return ScriptObject::get(name);
        }
        void set(std::string_view name,const ScriptValue& value) override {
            if(name=="Enabled") { owner.triggerState[record].enabled=boolean(value);return; }
            ScriptObject::set(name,value);
        }
        bool hasMethod(std::string_view name) const override { return name=="Activate"||name=="DeActivate"||name=="CleanMyDelays"; }
        ScriptValue call(std::string_view name,std::span<const ScriptValue> arguments) override {
            if(name=="CleanMyDelays") { owner.vm.cancelDelays(record+1);return true; }
            if(name=="Activate"||name=="DeActivate") {
                requireSession(arguments.empty(),"Trigger activation does not accept arguments");
                owner.triggerState[record].enabled=name=="Activate";
                if(name=="Activate")owner.pendingScripts.emplace_back(record,"OnActivate");
                return {};
            }
            return ScriptObject::call(name,arguments);
        }
    };
    struct FighterObject final:ScriptObject {
        Impl& owner;
        explicit FighterObject(Impl& session):owner(session) {}
        ScriptValue get(std::string_view name) const override {
            if(name=="Name")return std::string("player/gamepad0");return ScriptObject::get(name);
        }
        bool hasMethod(std::string_view name) const override {
            return name=="SetLastCheckPoint"||name=="Respawn"||name=="SetFighterActionState"||name=="CleanMyDelays";
        }
        ScriptValue call(std::string_view name,std::span<const ScriptValue> arguments) override {
            if(name=="CleanMyDelays") { owner.vm.cancelDelays(fighterOwner);return true; }
            if(name=="SetLastCheckPoint") {
                requireSession(arguments.size()<=1,"SetLastCheckPoint accepts one optional boolean");
                bool switchGroups=arguments.empty()?true:boolean(arguments[0]);
                auto invocation=owner.currentActor();
                if(owner.scene.records[invocation->record].type!="STARTPOS")return {};
                owner.checkpoint=invocation->record;++owner.checkpoints;
                owner.issue(invocation->record,"SetLastCheckPoint",switchGroups?
                    "Checkpoint actor-group activation/deactivation is not implemented":
                    "Behind-checkpoint actor deactivation is not implemented");
                return {};
            }
            if(name=="Respawn") {
                requireSession(arguments.empty(),"Respawn does not accept arguments");
                requireSession(owner.checkpoint.has_value(),"No authored STARTPOS is available for respawn");
                owner.respawnRequested=true;return {};
            }
            if(name=="SetFighterActionState") {
                throw std::runtime_error("Native fighter action/finisher controller and result flow are not implemented");
            }
            return ScriptObject::call(name,arguments);
        }
    };
    struct SceneObject final:ScriptObject {
        Impl& owner;
        explicit SceneObject(Impl& session):owner(session) {}
        ScriptValue get(std::string_view name) const override {
            if(name=="player/gamepad0")return owner.fighter;
            auto context=owner.currentActor();auto chunk=owner.scene.records[context->record].chunk;
            std::optional<size_t> candidate;
            for(size_t i=0;i<owner.scene.records.size();++i)if(owner.scene.records[i].name==name&&owner.scene.records[i].chunk==chunk) {
                requireSession(!candidate,"Authored actor name is ambiguous within a chunk");candidate=i;
            }
            if(!candidate)for(size_t i=0;i<owner.scene.records.size();++i)if(owner.scene.records[i].name==name) {
                requireSession(!candidate,"Authored actor name is ambiguous across chunks");candidate=i;
            }
            if(!candidate&&owner.actorLookup)if(auto actor=owner.actorLookup(chunk,name))return actor;
            requireSession(candidate.has_value(),"Referenced scene actor is outside the implemented native trigger scene");
            return ScriptObjectPtr(std::make_shared<ActorObject>(owner,*candidate,owner.fighter));
        }
    };

    Impl(const CourseScene& authored,const std::vector<ScriptChunk>& catalog,unsigned country,XMFLOAT3 offset,XMFLOAT3 radii,ScriptActorLookup lookup):
        scene(authored),scripts(catalog),theme(country),feetOffset(offset),feetRadii(radii),randomState(authored.id+1),actorLookup(std::move(lookup)) {
        requireSession(theme<3&&scene.records.size()<=4096,"Native session catalog outside budget");
        for(float value:{feetOffset.x,feetOffset.y,feetOffset.z})requireSession(std::isfinite(value),"Invalid native session feet offset");
        for(float value:{feetRadii.x,feetRadii.y,feetRadii.z})requireSession(std::isfinite(value)&&value>0,"Invalid native session feet radii");
        triggerState.reserve(scene.records.size());
        for(size_t i=0;i<scene.records.size();++i) {
            auto& record=scene.records[i];SessionTriggerState state;
            state.enabled=record.enabled;state.supported=record.supported;state.error=record.exclusion;
            state.resources.assign(record.resources.rbegin(),record.resources.rend());
            if(record.type=="STARTPOS"&&record.mode==0&&record.boxes.empty()) {
                requireSession(!checkpoint,"Native session has multiple Mode0 STARTPOS actors");checkpoint=i;
            } else if(record.mode!=1) {
                state.supported=false;state.error="Native trigger Mode0/Mode2 execution is not implemented";
            }
            if(!std::isfinite(record.waitTime.x)||!std::isfinite(record.waitTime.y)) {
                state.supported=false;state.error="Native trigger cooldown is nonfinite";
            }
            triggerState.push_back(std::move(state));
            if(!triggerState.back().supported)issue(i,"trigger",triggerState.back().error);
        }
        fighter=std::make_shared<FighterObject>(*this);
        auto game=std::make_shared<FunctionObject>();
        game->methods["GetPlayerCount"]=[](auto arguments)->ScriptValue { requireSession(arguments.empty(),"GetPlayerCount does not accept arguments");return 1.; };
        game->methods["GetCurrentTheme"]=[this](auto arguments)->ScriptValue {
            requireSession(arguments.empty(),"GetCurrentTheme does not accept arguments");return std::string(theme==0?"USA":theme==1?"EU":"JAPAN");
        };
        auto hud=std::make_shared<FunctionObject>();
        hud->methods["AddControl"]=[this](std::span<const ScriptValue> arguments)->ScriptValue {
            requireSession(arguments.size()==4,"AddControl requires name, resource, position and scale");
            auto pointer=std::get_if<ScriptObjectPtr>(&arguments[2]);
            auto position=pointer?std::dynamic_pointer_cast<Vector2Object>(*pointer):nullptr;
            requireSession(bool(position),"HUD control position requires CreateV2");
            double scale=number(arguments[3]);requireSession(scale>0&&scale<=100,"HUD scale outside budget");
            SessionHudControl control;control.name=text(arguments[0]);control.resource=text(arguments[1]);
            control.position=position->value;control.scale=float(scale);control.createdAt=clock;
            control.record=scene.records[currentActor()->record].id;
            auto found=std::find_if(hudState.begin(),hudState.end(),[&](auto& item){return item.name==control.name;});
            if(found==hudState.end()) { requireSession(hudState.size()<128,"Native HUD control budget exceeded");control.updates=1;hudState.push_back(std::move(control)); }
            else { control.updates=found->updates+1;*found=std::move(control); }
            return {};
        };
        auto gui=std::make_shared<FunctionObject>();gui->fields["hud"]=ScriptObjectPtr(hud);
        vm.setGlobal("game",ScriptObjectPtr(game));vm.setGlobal("gui",ScriptObjectPtr(gui));
        vm.setGlobal("scene",ScriptObjectPtr(std::make_shared<SceneObject>(*this)));
        vm.bind("CreateV2",[](std::span<const ScriptValue> arguments)->ScriptValue {
            requireSession(arguments.size()==2,"CreateV2 requires two coordinates");
            auto x=number(arguments[0]),y=number(arguments[1]);
            requireSession(std::abs(x)<=1e9&&std::abs(y)<=1e9,"CreateV2 coordinate outside budget");
            return ScriptObjectPtr(std::make_shared<Vector2Object>(XMFLOAT2{float(x),float(y)}));
        });
        const ScriptChunk* library=nullptr;
        for(auto& chunk:scripts)if(chunk.source=="data/templates/scripts.wbr"&&chunk.type=="SCRIPTSMASTER"&&
                                  chunk.actor=="obstacles_functions"&&chunk.field=="Script"&&chunk.index==0) {
            requireSession(!library,"Ambiguous native obstacles_functions library");library=&chunk;
        }
        if(library) {
            auto id=vm.start(library->code,"obstacles_functions");taskOwners.emplace(id,std::pair{SIZE_MAX,std::string("library")});
            vm.tick(0);harvest();
        } else issue(SIZE_MAX,"library","Authored obstacles_functions library is missing");
    }
    std::shared_ptr<ActorObject> currentActor() const {
        auto context=vm.currentContext();requireSession(context,"Native scene method requires an invoking trigger task");
        auto found=context->find("me");
        if(found!=context->end())if(auto pointer=std::get_if<ScriptObjectPtr>(&found->second))
            if(auto actor=std::dynamic_pointer_cast<ActorObject>(*pointer))return actor;
        found=context->find("owner");
        if(found!=context->end())if(auto pointer=std::get_if<ScriptObjectPtr>(&found->second))
            if(auto actor=std::dynamic_pointer_cast<ActorObject>(*pointer))return actor;
        throw std::runtime_error("Current native task has no invoking trigger owner");
    }
    void issue(size_t record,std::string field,std::string error,uint64_t task=0) {
        uint32_t id=record<scene.records.size()?scene.records[record].id:UINT32_MAX;
        if(issueState.size()>=4096)return;
        if(!task&&std::any_of(issueState.begin(),issueState.end(),[&](auto& previous) {
            return !previous.task&&previous.record==id&&previous.field==field&&previous.error==error;
        }))return;
        issueState.push_back({id,task,std::move(field),std::move(error)});
    }
    void harvest() {
        for(auto& task:vm.tasks())if(task.status==ScriptStatus::Complete||task.status==ScriptStatus::Failed||task.status==ScriptStatus::Cancelled) {
            auto found=taskOwners.find(task.id);
            if(task.status==ScriptStatus::Failed) {
                if(found!=taskOwners.end())issue(found->second.first,found->second.second,task.error,task.id);
                else issue(SIZE_MAX,"script",task.error,task.id);
            }
            if(found!=taskOwners.end())taskOwners.erase(found);
        }
        vm.clearFinished();
    }
    void launch(size_t index,std::string_view field) {
        auto& record=scene.records[index];
        for(auto& reference:record.scripts)if(reference.field==field) {
            try {
                requireSession(reference.mode==2&&reference.catalogIndex>=0&&size_t(reference.catalogIndex)<scripts.size(),
                               "Native session script reference is unavailable");
                auto& chunk=scripts[size_t(reference.catalogIndex)];
                requireSession(sameLocation(reference,chunk),"Native session script location differs from catalog");
                auto invocation=ScriptObjectPtr(std::make_shared<ActorObject>(*this,index,fighter));
                bool sendToToucher=field=="Command"&&record.sendToToucher;
                ScriptContext context{{"me",sendToToucher?fighter:invocation},{"owner",invocation},{"collided",fighter}};
                auto id=vm.start(chunk.code,record.name+":"+reference.field,std::move(context),sendToToucher?fighterOwner:index+1);
                taskOwners.emplace(id,std::pair{index,reference.field});
            } catch(const std::exception& error) { issue(index,std::string(field),error.what()); }
        }
    }
    void tick(CoursePlayer& player,double seconds,bool timerRunning) {
        requireSession(std::isfinite(seconds)&&seconds>=0&&seconds<=1,"Invalid native session time step");
        clock+=seconds;if(timerRunning&&!crossed)elapsedTime+=seconds;
        if(respawnRequested) {
            respawnRequested=false;
            if(checkpoint) {
                player.reset(scene.records[*checkpoint].position,CourseStart::Ready,theme);++respawns;
                launch(*checkpoint,"OnRespawnScript");
            }
        }
        auto queued=std::move(pendingScripts);pendingScripts.clear();
        for(auto& [record,field]:queued)launch(record,field);
        for(size_t i=0;i<scene.records.size();++i) {
            auto& record=scene.records[i];auto& state=triggerState[i];bool touching=false;
            for(auto& box:record.boxes)if((box.collisionMask&1)&&ellipsoidTouchesBox(player.position,feetOffset,feetRadii,box)) {
                touching=true;
                if(box.collisionGroup&0x200)crossed=true;
            }
            bool entered=touching&&!state.inside,left=!touching&&state.inside;state.inside=touching;
            if(!state.supported||!state.enabled)continue;
            if(left) { launch(i,"LeftTriggerCommand");launch(i,"AllTouchersLeftTriggerCommand"); }
            if(record.mode!=1||!entered||clock+1e-12<state.cooldownUntil)continue;
            double cooldown=record.waitTime.x;
            if(record.waitTime.y>record.waitTime.x) {
                // The native contract uses an inclusive 15-bit uniform draw.
                // A session-local seed makes synthetic replay deterministic.
                randomState=randomState*214013u+2531011u;
                cooldown+=(double((randomState>>16)&32767u)/32767.)*(record.waitTime.y-record.waitTime.x);
            }
            state.cooldownUntil=clock+cooldown;
            if(record.maxCommands>=0&&state.commands>=uint64_t(record.maxCommands))continue;
            ++state.contacts;
            if(record.commandCounter>1&&state.contacts%uint64_t(record.commandCounter))continue;
            ++state.commands;
            launch(i,"Command");
        }
        vm.tick(seconds);harvest();
    }
};

CourseSession::CourseSession(const CourseScene& scene,const std::vector<ScriptChunk>& scripts,unsigned theme,XMFLOAT3 feetOffset,XMFLOAT3 feetRadii,ScriptActorLookup actorLookup):
    impl(std::make_unique<Impl>(scene,scripts,theme,feetOffset,feetRadii,std::move(actorLookup))) {}
CourseSession::~CourseSession()=default;
void CourseSession::tick(CoursePlayer& player,double seconds,bool timerRunning) { impl->tick(player,seconds,timerRunning); }
bool CourseSession::requestRespawn() { if(!impl->checkpoint)return false;impl->respawnRequested=true;return true; }
const SceneRecord* CourseSession::lastCheckpoint() const { return impl->checkpoint?&impl->scene.records[*impl->checkpoint]:nullptr; }
double CourseSession::elapsed() const { return impl->elapsedTime; }
bool CourseSession::finished() const { return impl->crossed; }
bool CourseSession::finisher() const { return impl->cinematic; }
uint64_t CourseSession::respawnCount() const { return impl->respawns; }
uint64_t CourseSession::checkpointCount() const { return impl->checkpoints; }
const std::vector<SessionIssue>& CourseSession::issues() const { return impl->issueState; }
const std::vector<SessionHudControl>& CourseSession::hud() const { return impl->hudState; }
const std::vector<SessionTriggerState>& CourseSession::triggers() const { return impl->triggerState; }
const std::vector<ScriptLog>& CourseSession::logs() const { return impl->vm.logs(); }
uint64_t CourseSession::droppedLogs() const { return impl->vm.droppedLogs(); }
std::vector<ScriptTask> CourseSession::tasks() const { return impl->vm.tasks(); }
}
