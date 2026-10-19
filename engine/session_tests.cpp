#include "session.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace Doritos::Engine;
namespace {
void check(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
bool close(double a,double b) { return std::abs(a-b)<1e-6; }
Model testModel() {
    Model model;
    for(auto name:{"idle","run","run_fast","jump","fall","land","startup_usa","startup_europe","startup_japan"}) {
        Clip clip;clip.name=name;clip.duration=1;model.clips.push_back(clip);
    }
    return model;
}
MovementSettings testSettings() {
    MovementSettings settings;settings.courseParameters=true;settings.feetOffset={0,0,.5f};settings.feetRadii={.5f,.5f,.5f};return settings;
}
ScriptChunk source(std::string code,std::string actor="obstacles_functions",std::string field="") {
    ScriptChunk chunk;chunk.source=actor=="obstacles_functions"?"data/templates/scripts.wbr":"synthetic.wbm";
    chunk.type=actor=="obstacles_functions"?"SCRIPTSMASTER":"STARTPOS";
    chunk.field=field.empty()?(actor=="obstacles_functions"?"Script":"Command"):std::move(field);chunk.actor=std::move(actor);
    chunk.code.assign(code.begin(),code.end());return chunk;
}
SceneScriptReference reference(const std::vector<ScriptChunk>& scripts,size_t index) {
    auto& script=scripts[index];SceneScriptReference result;
    result.source=script.source;result.type=script.type;result.actor=script.actor;result.field=script.field;
    result.ordinal=script.ordinal;result.index=script.index;result.mode=2;result.catalogIndex=int32_t(index);return result;
}
SceneRecord actor(uint32_t id,float x,std::string type="STARTPOS") {
    SceneRecord record;record.id=id;record.ordinal=id;record.source="synthetic.wbm";record.type=std::move(type);
    record.name="actor-"+std::to_string(id);record.position={x,0,7};record.mode=1;record.maxCommands=1;record.commandCounter=1;
    record.resources={"FXFX:empty","RVMP:marker","CLP3:authored"};
    DirectX::XMStoreFloat4x4(&record.transform,DirectX::XMMatrixTranslation(x,0,7));
    SceneTriggerBox box;box.resource=2;box.name="CLP3:authored";box.collisionMask=1;box.minimum={-.5f,-.5f,-.5f};box.maximum={.5f,.5f,.5f};
    DirectX::XMStoreFloat4x4(&box.transform,DirectX::XMMatrixScaling(.2f,4,4)*DirectX::XMMatrixTranslation(x,0,1));
    record.boxes.push_back(box);return record;
}
CourseScene scene() {
    CourseScene course;course.id=0;course.name="synthetic";
    auto start=actor(0,-10);start.mode=0;start.position={-10,0,2};start.boxes.clear();start.resources.clear();
    course.records={start,actor(1,10),actor(2,20)};return course;
}
const char* checkpointLibrary=R"(
function NativeCheckpoint()
    me.Resources[2]='FXFX:native-flame'
    me.Resources[3]='FXFX:native-crowd'
    assert(game.GetPlayerCount()==1)
    local theme=game.GetCurrentTheme()
    gui.hud.AddControl('check_point','GGRP:'..theme,CreateV2(2,3),1)
    WaitFrames(1)
    me.collided.SetLastCheckPoint(false)
    WaitFrames(1)
end
)";
void contextCheckpointAndRespawn() {
    auto model=testModel();auto settings=testSettings();CoursePlayer player(model,settings);player.reset({-100,0,0});
    auto authored=scene();std::vector<ScriptChunk> scripts={source(checkpointLibrary),source("NativeCheckpoint()","first"),
        source("NativeCheckpoint()","second"),source("me.Resources[4]='FXFX:respawn'","first","OnRespawnScript")};
    authored.records[1].scripts={reference(scripts,1),reference(scripts,3)};
    authored.records[2].scripts={reference(scripts,2)};
    CourseSession session(authored,scripts,2,settings.feetOffset,settings.feetRadii);
    check(session.lastCheckpoint()==&authored.records[0]&&session.checkpointCount()==0,"Session defaulted to a Mode1 checkpoint");
    check(session.requestRespawn(),"Initial Mode0 STARTPOS could not respawn");session.tick(player,.1);
    check(player.position.x==-10&&player.position.z==2&&!player.waitingForDirection(),"Initial respawn did not use exact authored Ready position");
    player.position={10,0,0};session.tick(player,.1);
    check(session.lastCheckpoint()==&authored.records[0]&&session.tasks().size()==1,"Checkpoint ignored its authored first frame wait");
    player.position={20,0,0};session.tick(player,.1);
    check(session.lastCheckpoint()==&authored.records[1],"First yielded checkpoint lost its invoking me when second contact arrived");
    session.tick(player,.1);
    check(session.lastCheckpoint()==&authored.records[2]&&session.checkpointCount()==2,"Second yielded checkpoint used another task's context");
    session.tick(player,.1);
    check(session.hud().size()==1&&session.hud()[0].updates==2&&session.hud()[0].resource=="GGRP:JAPAN"&&
          session.hud()[0].position.x==2,"Checkpoint did not update real themed HUD state");
    check(session.triggers()[1].resources==std::vector<std::string>{"CLP3:authored","RVMP:marker","FXFX:native-flame","FXFX:native-crowd"},
          "Checkpoint resource mutation did not use zero-based reverse serialized order and append");
    // Reenter the first checkpoint. Its one-shot command stays consumed, and
    // respawn remains selected by the explicit second script callback.
    player.position={10,0,0};session.tick(player,.1);
    check(session.triggers()[1].commands==1&&session.lastCheckpoint()==&authored.records[2],"Checkpoint MaxCommands latch reset on another contact");
    double before=session.elapsed();player.velocity={10,2,-4};
    check(session.requestRespawn(),"Selected checkpoint respawn was rejected");session.tick(player,.25);
    check(player.position.x==20&&player.position.y==0&&player.position.z==7&&player.velocity.x==0&&player.velocity.z==0,
          "Respawn added a height offset or preserved velocity");
    check(close(session.elapsed(),before+.25)&&session.respawnCount()==2,"Respawn reset the session timer or counter");
    check(session.issues().size()==2&&std::all_of(session.issues().begin(),session.issues().end(),[](auto& issue) {
        return issue.field=="SetLastCheckPoint"&&!issue.task&&issue.error=="Behind-checkpoint actor deactivation is not implemented";
    }),"Checkpoint false path hid missing actor deactivation or produced an unexpected script failure");

    // A checkpoint OnRespawnScript is separate from its consumed contact Command.
    auto isolated=scene();isolated.records.resize(2);isolated.records[1].scripts={reference(scripts,1),reference(scripts,3)};
    CourseSession respawnSession(isolated,scripts,0);player.position={10,0,0};
    respawnSession.tick(player,0);respawnSession.tick(player,0);respawnSession.tick(player,0);
    check(respawnSession.requestRespawn(),"Checkpoint script did not select respawn STARTPOS");respawnSession.tick(player,.1);
    check(respawnSession.triggers()[1].resources.size()==5&&respawnSession.triggers()[1].resources[4]=="FXFX:respawn"&&
          respawnSession.triggers()[1].commands==1,"Independent OnRespawnScript did not run after Command was consumed");
}
void countersCooldownAndUnsupported() {
    auto model=testModel();CoursePlayer player(model,testSettings());auto authored=scene();authored.records.resize(2);
    auto& trigger=authored.records[1];trigger.type="TRIGGER";trigger.commandCounter=2;trigger.maxCommands=1;trigger.waitTime={.5f,0};
    std::vector<ScriptChunk> scripts={source(""),source("me.Resources[3]='FXFX:counted'","counter")};
    scripts[0]=source("function Unused() end");trigger.scripts={reference(scripts,1)};
    CourseSession session(authored,scripts,0);
    player.position={10,0,0};session.tick(player,.1);
    check(session.triggers()[1].contacts==1&&session.triggers()[1].commands==0&&close(session.triggers()[1].cooldownUntil,.6),
          "Modulo-suppressed trigger did not refresh its cooldown");
    player.position={-100,0,0};session.tick(player,.1);player.position={10,0,0};session.tick(player,.1);
    check(session.triggers()[1].contacts==1,"Cooldown-active entering contact incremented the command counter");
    session.tick(player,1);check(session.triggers()[1].commands==0,"Mode1 fired while staying inside after cooldown elapsed");
    player.position={-100,0,0};session.tick(player,0);player.position={10,0,0};session.tick(player,0);
    check(session.triggers()[1].contacts==2&&session.triggers()[1].commands==1,"CommandCounter did not fire on the eligible second contact");
    player.position={-100,0,0};session.tick(player,1);player.position={10,0,0};session.tick(player,0);
    check(session.triggers()[1].contacts==2&&session.triggers()[1].commands==1&&close(session.triggers()[1].cooldownUntil,2.8),
          "MaxCommands gate order differs from native cooldown refresh and counter behavior");
    trigger.mode=2;CourseSession unsupported(authored,scripts,0);unsupported.tick(player,.1);
    check(!unsupported.triggers()[1].supported&&!unsupported.issues().empty()&&unsupported.triggers()[1].commands==0,
          "Unsupported trigger Mode2 silently succeeded");
    authored.records.erase(authored.records.begin());CourseSession noStart(authored,scripts,0);
    check(!noStart.lastCheckpoint()&&!noStart.requestRespawn(),"Session guessed an initial Mode1 checkpoint");
}
void transformedContactAndFinishTimer() {
    auto model=testModel();CoursePlayer player(model,testSettings());auto authored=scene();authored.records.resize(2);
    auto& finish=authored.records[1];finish.boxes[0].collisionGroup=512;
    // Rotate and shear the box. Authored actor position alone is far away from
    // its box center and cannot be used as a checkpoint/finish plane.
    finish.position={999,0,7};auto& matrix=finish.boxes[0].transform;
    DirectX::XMStoreFloat4x4(&matrix,DirectX::XMMatrixScaling(.2f,4,4)*DirectX::XMMatrixRotationZ(.6f)*DirectX::XMMatrixTranslation(10,5,1));
    matrix._21+=.7f;
    std::vector<ScriptChunk> scripts={source("function Unused() end"),source("me.collided.SetFighterActionState('FINISHER')","finisher")};
    finish.scripts={reference(scripts,1)};
    CourseSession session(authored,scripts,0,{0,0,.75f},{.2f,.3f,.75f});
    player.position={999,0,0};session.tick(player,.25,false);
    check(!session.finished()&&session.elapsed()==0,"Session guessed finish from actor position or ignored timer gate");
    player.position={10.75f,5,0};session.tick(player,0);
    check(!session.finished(),"Ellipsoid contact accepted an empty corner of the transformed box AABB");
    player.position={-100,0,0};session.tick(player,.25);
    player.position={10,5,0};session.tick(player,.25);
    check(session.finished()&&close(session.elapsed(),.5)&&!session.finisher(),"Finish collider did not freeze time before unsupported finisher action");
    check(std::any_of(session.issues().begin(),session.issues().end(),[](auto& issue){return issue.task&&issue.error.find("finisher controller")!=std::string::npos;}),
          "Unsupported fighter action returned placeholder success");
    player.position={-100,0,0};session.tick(player,1);
    check(close(session.elapsed(),.5),"Finish time continued after leaving the authored finish box");
    check(session.requestRespawn(),"Finish session lost Mode0 respawn fallback");session.tick(player,.25);
    check(close(session.elapsed(),.5)&&session.finished(),"Respawn cleared the captured finish-line time");
}
void ownedAssets(const std::filesystem::path& directory) {
    auto model=loadModel(directory/"character.dne");auto settings=loadMovementSettings(directory/"movement.dnp");
    auto authored=loadScene(directory/"scene.dns");auto scripts=loadScripts(directory/"scripts.dnl");
    check(authored.courses.size()==15,"Owned session telemetry requires all 15 courses");
    for(auto& course:authored.courses) {
        auto target=std::find_if(course.records.begin(),course.records.end(),[](auto& record) {
            return record.type=="STARTPOS"&&record.mode==1&&!record.boxes.empty()&&
                   std::none_of(record.boxes.begin(),record.boxes.end(),[](auto& box){return bool(box.collisionGroup&512);});
        });
        check(target!=course.records.end(),"Owned course has no checkpoint contact box");
        auto theme=std::find_if(model.courses.begin(),model.courses.end(),[&](auto& value){return value.id==course.id;});
        check(theme!=model.courses.end(),"Owned session course has no theme");
        CourseSession session(course,scripts,theme->theme,settings.feetOffset,settings.feetRadii);
        CoursePlayer player(model,settings);auto& box=target->boxes[0];
        XMFLOAT3 center={(box.minimum.x+box.maximum.x)*.5f,(box.minimum.y+box.maximum.y)*.5f,(box.minimum.z+box.maximum.z)*.5f};
        DirectX::XMStoreFloat3(&center,DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&center),DirectX::XMLoadFloat4x4(&box.transform)));
        player.reset({center.x-settings.feetOffset.x,center.y-settings.feetOffset.y,center.z-settings.feetOffset.z},CourseStart::Ready,theme->theme);
        for(unsigned step=0;step<4;++step)session.tick(player,.01);
        check(session.lastCheckpoint()==&*target&&session.checkpointCount()>=1,"Actual owned CheckPoint did not select its invoking STARTPOS");
        check(!session.hud().empty()&&session.hud()[0].name=="check_point","Actual owned CheckPoint did not update HUD state");
        check(session.requestRespawn(),"Actual owned checkpoint could not request respawn");double before=session.elapsed();session.tick(player,.1);
        check(player.position.x==target->position.x&&player.position.y==target->position.y&&player.position.z==target->position.z,
              "Actual owned checkpoint respawn differs from exact authored STARTPOS");
        check(close(session.elapsed(),before+.1),"Actual owned checkpoint respawn reset elapsed time");
        std::cout<<"course="<<course.id<<" checkpoint_record="<<target->id<<" checkpoint_commands="<<session.checkpointCount()
                 <<" respawns="<<session.respawnCount()<<" hud_controls="<<session.hud().size()<<" explicit_issues="<<session.issues().size()<<'\n';
        for(auto& issue:session.issues())std::cout<<"  record="<<issue.record<<" field="<<issue.field<<" issue="<<issue.error<<'\n';
    }
}
}
int main(int argc,char** argv) {
    try {
        contextCheckpointAndRespawn();countersCooldownAndUnsupported();transformedContactAndFinishTimer();
        if(argc==2)ownedAssets(argv[1]);else check(argc==1,"Usage: NativeSessionTests [private-assets-directory]");
        std::cout<<"Native session checkpoint contexts, contacts, cooldown, exact respawn and finish-line timer checks passed\n";return 0;
    } catch(const std::exception& error) { std::cerr<<"Native session test: "<<error.what()<<'\n';return 1; }
}
