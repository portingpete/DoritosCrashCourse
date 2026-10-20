#include "player.h"
#include "world.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Doritos::Engine;
namespace {
constexpr float tickTime=1.f/60;
void expect(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
void near(float actual,float expected,const char* message,float tolerance=.0002f) {
    expect(std::isfinite(actual)&&std::abs(actual-expected)<=tolerance,message);
}
Model model(float rootSpeed=2) {
    Model result;
    for(auto name:{"idle","run","run_fast","jump","fall","land","startup_usa","startup_europe","startup_japan"}) {
        Clip clip;clip.name=name;clip.duration=1;
        if(clip.name=="run"||clip.name=="run_fast")clip.motion={{0,{}},{1,{0,-rootSpeed,0}}};
        result.clips.push_back(clip);
    }
    return result;
}
MovementSettings settings() {
    MovementSettings result;result.courseParameters=true;result.feetOffset={0,0,.8f};result.feetRadii={.3f,.3f,.3f};
    result.gravity=-10;result.jumpGravityFactor=-.5f;result.jumpTimeFactor=.5f;
    result.inertiaTime=.1f;result.dragNoInput=100;result.jumpQueue=.2f;result.landRunFactor=.2f;
    result.minimumSpeed=1;result.maximumBoostSpeed=1;result.jumpFactors={0,0};result.airFactors={0,0};
    result.jumpSpeeds={0,6};result.airSpeeds={0,6};result.airFriction=0;return result;
}
PlatformSurface platform(float motor,bool conveyor=true,float friction=.3f,uint32_t ordinal=0) {
    PlatformSurface result;result.actor="Synthetic authored belt";result.ordinal=ordinal;
    result.slideVelocity={motor,0,0};result.friction=friction;result.useForFloor=true;result.slidable=true;
    result.conveyor=conveyor;return result;
}
std::vector<Triangle> rectangle(float low,float high,float z=0) {
    return {{{low,-8,z},{high,-8,z},{high,8,z}},{{low,-8,z},{high,8,z},{low,8,z}}};
}
CollisionScene belt(float motor,bool conveyor=true,float friction=.3f,float low=-100,float high=100) {
    return CollisionScene(rectangle(low,high),{platform(motor,conveyor,friction)},{0,0});
}
void settle(CoursePlayer& player,const CollisionScene& scene,float dt=tickTime) {
    for(unsigned i=0;i<240&&!player.grounded;++i)player.tick({},dt,scene);
    expect(player.grounded,"Synthetic conveyor player did not find its authored floor");
    for(unsigned i=0;i<4;++i)player.tick({},dt,scene);
    expect(player.grounded,"Synthetic conveyor player lost its initial support");
}
void sum(const CoursePlayer& player,const char* message) {
    auto relative=player.relativeVelocity();
    near(player.velocity.x,relative.x+player.surfaceVelocity.x+player.externalVelocity.x,message);
}
void noMotorOrCarry(const CoursePlayer& player,const char* message) {
    near(player.surfaceVelocity.x,0,message);near(player.externalVelocity.x,0,message);
}
void walkOff(CoursePlayer& player,const CollisionScene& scene,bool held=false,float dt=tickTime) {
    for(unsigned i=0;i<240&&player.grounded;++i)player.tick({0,false,held},dt,scene);
    expect(!player.grounded,"Synthetic conveyor player did not cross its finite platform edge");
}
void stationaryAndRunning() {
    auto source=model();auto tuning=settings();
    for(float motor:{3.f,-3.f}) {
        auto scene=belt(motor);CoursePlayer player(source,tuning);player.reset({0,0,.5f},CourseStart::Ready);settle(player,scene);
        expect(player.onConveyor&&player.supportTriangle!=std::numeric_limits<uint32_t>::max(),"Native player did not resolve its conveyor support");
        near(player.relativeVelocity().x,0,"Idle belt introduced root/action velocity");near(player.surfaceVelocity.x,motor,"Idle belt resolved the wrong authored motor");
        auto start=player.position.x;
        for(unsigned i=0;i<6;++i) { player.tick({},tickTime,scene);near(player.velocity.x,motor,"Idle belt did not drive at authored speed");sum(player,"Idle motor was duplicated or omitted"); }
        near(player.position.x-start,motor*6*tickTime,"Idle conveyor displacement does not match its motor");
        float against=motor>0?-1.f:1.f;
        for(unsigned i=0;i<6;++i)player.tick({against,false,false},tickTime,scene);
        expect(player.relativeVelocity().x*against>1.9f,"Against-belt input did not produce independent root motion");
        near(player.relativeVelocity().x,against*2,"Root motion changed with belt direction");
        sum(player,"Running velocity did not equal root motion plus belt motor");
        near(player.velocity.x,against*2+motor,"Against-belt actual velocity was not additive");
    }
    float displacements[2]{};
    for(unsigned i=0;i<2;++i) {
        auto scene=belt(3,true,i==0?0.f:50.f);CoursePlayer player(source,tuning);player.reset({0,0,.5f},CourseStart::Ready);settle(player,scene);
        auto start=player.position.x;for(unsigned step=0;step<6;++step)player.tick({},tickTime,scene);
        displacements[i]=player.position.x-start;near(player.velocity.x,3,"Flat conveyor motor incorrectly depends on friction");
    }
    near(displacements[0],displacements[1],"Flat conveyor travel changed with authored friction");
    auto unflagged=belt(3,false);CoursePlayer player(source,tuning);player.reset({0,0,.5f},CourseStart::Ready);settle(player,unflagged);
    expect(!player.onConveyor,"Unflagged moving surface was labeled IsConveyor");
    near(player.surfaceVelocity.x,3,"IsConveyor=false incorrectly disabled authored slide velocity");
    near(player.velocity.x,3,"Unflagged authored motor did not move an idle player");
}
void supportLossAndReset() {
    auto source=model();auto tuning=settings();auto edge=belt(3,true,.3f,-20,0);
    CoursePlayer player(source,tuning);player.reset({-2,0,.5f},CourseStart::Ready);settle(player,edge);walkOff(player,edge);
    near(player.surfaceVelocity.x,0,"Walking off retained a live belt motor");
    near(player.externalVelocity.x,3,"Walking off did not transfer the prior slide velocity once");
    auto start=player.position.x;
    for(unsigned i=0;i<8;++i) {
        player.tick({},tickTime,edge);near(player.externalVelocity.x,3,"Walking-off carry accumulated on successive air spans");
        near(player.velocity.x,3,"Walking-off carry changed with zero air friction and steering");
    }
    near(player.position.x-start,3*8*tickTime,"Walking-off displacement duplicated or lost external carry");
    auto lower=rectangle(-20,0);auto landing=rectangle(0,30,-2);lower.insert(lower.end(),landing.begin(),landing.end());
    CollisionScene twoFloors(lower,{platform(3),platform(0,false,.3f,1)},{0,0,1,1});
    CoursePlayer lander(source,tuning);lander.reset({-2,0,.5f},CourseStart::Ready);settle(lander,twoFloors);walkOff(lander,twoFloors);
    for(unsigned i=0;i<180&&!lander.grounded;++i)lander.tick({},tickTime,twoFloors);
    expect(lander.grounded&&lander.position.z< -1.9f,"Conveyor carry did not reach the lower ordinary floor");
    for(unsigned i=0;i<4;++i)lander.tick({},tickTime,twoFloors);
    noMotorOrCarry(lander,"Landing retained an old conveyor motor or external carry");near(lander.velocity.x,0,"Idle landing retained belt momentum");
    player.reset({-2,0,.5f},CourseStart::Ready);
    noMotorOrCarry(player,"Ready reset retained old motor/external carry");
    near(player.relativeVelocity().x,0,"Ready reset retained old action velocity");
    expect(!player.onConveyor&&player.supportTriangle==std::numeric_limits<uint32_t>::max(),"Ready reset retained stale conveyor support");
    settle(player,edge);near(player.velocity.x,3,"Ready reset failed to reacquire the authored belt");
}
void jumpDispatch() {
    auto source=model();auto tuning=settings();
    for(float motor:{3.f,-3.f}) {
        auto scene=belt(motor);CoursePlayer player(source,tuning);player.reset({0,0,.5f},CourseStart::Ready);settle(player,scene);
        float against=motor>0?-1.f:1.f;
        for(unsigned i=0;i<4;++i)player.tick({against,false,false},tickTime,scene);
        float oldActual=player.velocity.x,priorSlide=player.surfaceVelocity.x;
        float scaled=oldActual*.9f;
        expect(std::abs(scaled)<1||scaled*priorSlide<0,"Synthetic fresh-jump setup did not meet the authored extra-slide condition");
        player.tick({against,true,true},tickTime,scene);
        expect(player.jumps==1&&!player.grounded,"Fresh against-belt jump did not dispatch");
        near(player.velocity.x,scaled+.2f*priorSlide,"Fresh conveyor jump did not use actual prior speed and conditional .2 slide");
        noMotorOrCarry(player,"Dispatched jump incorrectly transferred belt speed into external carry");
        float dispatched=player.velocity.x;
        for(unsigned i=0;i<5;++i) {
            player.tick({0,false,true},tickTime,scene);
            near(player.velocity.x,dispatched,"Held jump duplicated prior conveyor slide");
            near(player.externalVelocity.x,0,"Held jump acquired a second prior-slide carry");
        }
        expect(player.jumps==1,"Holding conveyor jump dispatched twice");
    }
    {
        auto fastSource=model(6);auto scene=belt(3);CoursePlayer opposed(fastSource,tuning);
        opposed.reset({0,0,.5f},CourseStart::Ready);settle(opposed,scene);
        for(unsigned i=0;i<4;++i)opposed.tick({-1,false,false},tickTime,scene);
        float scaled=opposed.velocity.x*.9f;
        expect(std::abs(scaled)>1&&scaled*opposed.surfaceVelocity.x<0,"Fast opposing jump did not isolate the dot-product condition");
        opposed.tick({0,true,true},tickTime,scene);
        near(opposed.velocity.x,scaled+.6f,"Opposing conveyor jump omitted extra slide when horizontal speed exceeded one");
        CoursePlayer aligned(source,tuning);aligned.reset({0,0,.5f},CourseStart::Ready);settle(aligned,scene);
        for(unsigned i=0;i<4;++i)aligned.tick({1,false,false},tickTime,scene);
        scaled=aligned.velocity.x*.9f;
        expect(scaled>1,"Aligned jump did not isolate the no-extra-slide condition");
        aligned.tick({0,true,true},tickTime,scene);
        near(aligned.velocity.x,scaled,"Aligned high-speed conveyor jump incorrectly added conditional slide");
    }
    auto ordinary=belt(0,false);CoursePlayer normal(source,tuning);normal.reset({0,0,.5f},CourseStart::Ready);settle(normal,ordinary);
    for(unsigned i=0;i<4;++i)normal.tick({1,false,false},tickTime,ordinary);
    float oldActual=normal.velocity.x;normal.tick({0,true,true},tickTime,ordinary);
    near(normal.velocity.x,oldActual*.9f,"Ordinary jump lost its original scalar .9 horizontal adjustment");
    auto scene=belt(3);CoursePlayer buffered(source,tuning);buffered.reset({0,0,.5f},CourseStart::Startup);settle(buffered,scene);
    expect(buffered.waitingForDirection(),"Startup conveyor was released without directional input");
    buffered.tick({-1,true,true},tickTime,scene);
    expect(buffered.jumps==0&&!buffered.waitingForDirection(),"Startup direction press dispatched its jump too early");
    oldActual=buffered.velocity.x;float priorSlide=buffered.surfaceVelocity.x;
    near(priorSlide,3,"Pending startup queue suppressed authored world-slide resolution");
    buffered.tick({-1,false,true},tickTime,scene);
    expect(buffered.jumps==1&&!buffered.grounded,"Eligible startup jump did not dispatch on its later action tick");
    near(buffered.velocity.x,oldActual*.9f+1.6f*priorSlide,"Buffered startup conveyor jump did not apply the authored later-dispatch factor");
    noMotorOrCarry(buffered,"Buffered dispatched jump duplicated its slide into external carry");
}
void strictExpiredQueueDoesNotLatchHeld() {
    // Exact binary fractions distinguish age==T from age<T without tolerance.
    constexpr float dt=1.f/32;
    auto source=model();auto tuning=settings();tuning.jumpQueue=dt;
    auto scene=belt(3,true,.3f,-20,0);CoursePlayer player(source,tuning);
    player.reset({-3,0,.5f},CourseStart::Startup);settle(player,scene,dt);
    player.tick({-1,true,true},dt,scene);
    expect(player.jumps==0&&!player.waitingForDirection(),"Strict queue fixture failed its deferred startup setup");
    player.tick({-1,false,true},dt,scene);
    expect(player.jumps==0&&player.grounded,"Jump dispatched at pressAge==queueWindow instead of strict age<window");
    walkOff(player,scene,true,dt);
    expect(player.jumps==0,"An expired startup press dispatched later while held");
    near(player.externalVelocity.x,3,"Ignored expired startup press latched held and suppressed walking-off carry");
    for(unsigned i=0;i<4;++i)player.tick({0,false,true},dt,scene);
    near(player.externalVelocity.x,3,"Expired startup held input duplicated the one-shot walk-off carry");
}
void ordinaryLandingBuffer() {
    auto source=model();auto tuning=settings();
    for(bool fromJump:{false,true})for(float motor:{0.f,3.f,-3.f}) {
        auto scene=belt(motor,motor!=0);CoursePlayer player(source,tuning);
        player.reset({0,0,1.2f},CourseStart::Ready);uint64_t priorJumps=0;
        if(fromJump) {
            settle(player,scene);player.tick({0,true,true},tickTime,scene);priorJumps=1;
            expect(player.jumps==1&&!player.grounded,"Landing-buffer setup jump failed");
        }
        bool queued=false;
        for(unsigned i=0;i<180&&!player.grounded;++i) {
            bool press=!queued&&player.velocity.z<0&&player.position.z<.12f;
            player.tick({0,press,press},tickTime,scene);queued|=press;
        }
        expect(queued&&player.grounded&&player.jumps==priorJumps,"Landing-buffer setup did not preserve a pre-contact press");
        player.tick({},tickTime,scene);
        expect(player.grounded&&player.jumps==priorJumps,"Buffered jump dispatched before the post-contact action transition");
        near(player.surfaceVelocity.x,motor,"Pending landing buffer discarded the resolved surface motor");
        float expected=player.velocity.x*.9f;
        if(motor!=0&&(std::abs(expected)<1||expected*motor<0))expected+=1.6f*motor;
        player.tick({},tickTime,scene);
        expect(!player.grounded&&player.jumps==priorJumps+1,"Released landing buffer failed after the ordinary action transition");
        near(player.velocity.x,expected,"Released landing buffer lost its buffered conveyor adjustment");
        noMotorOrCarry(player,"Released landing buffer inherited the motor twice");
    }
}
void ownedProbe(const std::filesystem::path& assets) {
    auto source=loadModel(assets/"character.dne");auto tuning=loadMovementSettings(assets/"movement.dnp");
    auto courses=loadCollision(assets/"static-collision.dnc");
    for(const auto& catalog:source.courses) {
        if(catalog.theme!=0)continue;
        const auto course=std::find_if(courses.begin(),courses.end(),[&](const auto& value){return value.id==catalog.id;});
        expect(course!=courses.end(),"Owned USA course collision is missing");
        for(uint32_t index=0;index<course->scene.platformCount();++index) {
            const auto& surface=course->scene.platform(index);auto authored=surfaceVelocity(surface);
            if(!surface.conveyor||!surface.useForFloor||std::abs(std::abs(authored.x)-3)>.01f)continue;
            auto bounds=course->scene.platformBounds(index);if(!bounds)continue;
            XMFLOAT3 center{(bounds->first.x+bounds->second.x)*.5f,(bounds->first.y+bounds->second.y)*.5f,bounds->second.z+.5f};
            auto hit=course->scene.ray(center,{0,0,-1},bounds->second.z-bounds->first.z+1,true);
            if(!hit||hit->normal.z<.6f||course->scene.platformForTriangle(hit->triangle)!=&surface)continue;
            CoursePlayer player(source,tuning);player.reset({hit->position.x,hit->position.y,hit->position.z+.05f},CourseStart::Ready);settle(player,course->scene);
            expect(course->scene.platformForTriangle(player.supportTriangle)==&surface,"Owned conveyor center probe acquired another platform");
            auto start=player.position.x;
            for(unsigned i=0;i<6;++i)player.tick({},tickTime,course->scene);
            expect(player.grounded&&player.onConveyor,"Owned conveyor probe lost eligible support");
            near(player.velocity.x,authored.x,"Owned no-input conveyor speed differs from its authored motor",.03f);
            near((player.position.x-start)/(6*tickTime),authored.x,"Owned no-input conveyor displacement differs from its authored motor",.04f);
            float against=authored.x>0?-1.f:1.f;
            for(unsigned i=0;i<6;++i)player.tick({against,false,false},tickTime,course->scene);
            expect(player.relativeVelocity().x*against>0,"Owned against-belt pulse did not produce opposing action motion");
            sum(player,"Owned against-belt velocity was not action plus motor plus external carry");
            auto jumps=player.jumps;player.tick({0,true,true},tickTime,course->scene);
            expect(player.jumps==jumps+1&&!player.grounded,"Owned short conveyor jump failed to dispatch");
            player.reset({hit->position.x,hit->position.y,hit->position.z+.05f},CourseStart::Ready);
            noMotorOrCarry(player,"Owned conveyor Ready reset retained carry");settle(player,course->scene);
            near(player.velocity.x,authored.x,"Owned conveyor reset failed to reacquire its motor",.03f);
            std::cout<<"Owned bounded conveyor probe: course="<<catalog.id<<" chunk="<<surface.chunk<<" ordinal="<<surface.ordinal
                     <<" motor_x="<<authored.x<<" no_input_ticks=6 against_ticks=6 jumps="<<jumps+1<<" reset=1\n";
            return;
        }
    }
    throw std::runtime_error("Owned assets contain no owner-verified USA conveyor probe surface");
}
}
int main(int argc,char** argv) {
    try {
        stationaryAndRunning();supportLossAndReset();jumpDispatch();strictExpiredQueueDoesNotLatchHeld();ordinaryLandingBuffer();
        if(argc==2)ownedProbe(argv[1]);else expect(argc==1,"Use NativeConveyorTests [asset-directory]");
        std::cout<<"Native conveyor authored motors, relative/action decomposition, one-shot carry, jump dispatch, strict queue eligibility and reset contracts passed\n";
        return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
