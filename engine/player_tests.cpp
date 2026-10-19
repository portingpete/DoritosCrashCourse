#include "player.h"
#include "world.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <iomanip>

using namespace Doritos::Engine;
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
int main(int argc,char** argv) {
    try {
        Model model;
        for(auto name:{"idle","run","run_fast","jump","fall","land"}) {
            Clip clip;clip.name=name;clip.duration=1;
            if(clip.name=="run"||clip.name=="run_fast")clip.motion={{0,{}},{1,{0,-2,0}}};
            model.clips.push_back(clip);
        }
        MovementSettings settings;settings.courseParameters=true;settings.feetOffset={0,0,.8f};settings.feetRadii={.5f,.5f,.5f};
        settings.gravity=-10;settings.jumpGravityFactor=-.5f;settings.jumpTimeFactor=.5f;settings.jumpQueue=.2f;
        settings.dragNoInput=10;settings.landRunFactor=.2f;settings.jumpSpeeds={0,6};settings.airSpeeds={0,6};
        CollisionScene floor({{{-50,-50,0},{50,-50,0},{50,50,0}},{{-50,-50,0},{50,50,0},{-50,50,0}}});
        CoursePlayer player(model,settings);player.reset({0,0,1});
        for(int i=0;i<120;++i)player.tick({},1.f/60,floor);
        require(player.grounded&&std::abs(player.position.z)<.0001f,"Native player did not settle on its base-point floor");
        auto jump=[&](int held) {
            float maximum=0;uint64_t before=player.jumps;
            for(int i=0;i<120;++i) { player.tick({0,i==0,i<held},1.f/60,floor);maximum=std::max(maximum,player.position.z); }
            require(player.grounded&&player.jumps==before+1,"Native jump did not land exactly once");return maximum;
        };
        float shortHeight=jump(1),longHeight=jump(18);
        require(longHeight>shortHeight+.2f,"Native held jump did not extend the arc");
        auto jumps=player.jumps;
        for(int i=0;i<90;++i)player.tick({0,false,true},1.f/60,floor);
        require(player.jumps==jumps,"Holding jump retriggered without another press");
        CollisionScene wall({{{-50,-50,0},{50,-50,0},{50,50,0}},{{-50,-50,0},{50,50,0},{-50,50,0}},
            {{4,-5,0},{4,5,0},{4,5,5}},{{4,-5,0},{4,5,5},{4,-5,5}}});
        for(int i=0;i<300;++i)player.tick({1,false,false},1.f/60,wall);
        require(player.grounded&&std::abs(player.position.x-3.5f)<.002f,"Native course player crossed a swept wall");
        CollisionScene slope({{{-10,-5,-2.5f},{10,-5,2.5f},{10,5,2.5f}},{{-10,-5,-2.5f},{10,5,2.5f},{-10,5,-2.5f}}});
        CoursePlayer climber(model,settings);climber.reset({0,0,1});
        for(int i=0;i<120;++i)climber.tick({},1.f/60,slope);
        for(int i=0;i<60;++i)climber.tick({1,false,false},1.f/60,slope);
        require(climber.grounded&&climber.position.x>1.5f&&std::abs(climber.position.z-climber.position.x*.25f)<.0002f,
                "Native ground motion lost its slope-tangent displacement");
        if(argc==2||argc==4) {
            std::filesystem::path assets=argv[1];auto owned=loadModel(assets/"character.dne");auto tuning=loadMovementSettings(assets/"movement.dnp");
            auto courses=loadCollision(assets/"static-collision.dnc");auto world=loadWorld(assets/"world.dnw");
            if(argc==4) {
                require(!std::filesystem::exists(argv[3]),"Replay output already exists");
                std::ifstream input(argv[2]);std::string line;
                require(bool(std::getline(input,line))&&line=="dt,horizontal,pressed,held","Invalid native replay header");
                CoursePlayer actual(owned,tuning);actual.reset(world.courses[0].spawn);
                for(int step=0;step<120;++step)actual.tick({},1.f/60,courses[0].scene);
                std::ofstream output(argv[3]);output<<"step,x,y,z,vx,vz,grounded,jumps,animation\n"<<std::setprecision(10);
                size_t step=0;
                while(std::getline(input,line)) {
                    std::istringstream row(line);float dt=0,axis=0;int pressed=0,held=0;char a=0,b=0,c=0;
                    require(bool(row>>dt>>a>>axis>>b>>pressed>>c>>held)&&a==','&&b==','&&c==','
                            &&(pressed==0||pressed==1)&&(held==0||held==1)&&++step<=1000000,"Invalid native replay row");
                    actual.tick({axis,bool(pressed),bool(held)},dt,courses[0].scene);
                    output<<step<<','<<actual.position.x<<','<<actual.position.y<<','<<actual.position.z<<','
                          <<actual.velocity.x<<','<<actual.velocity.z<<','<<actual.grounded<<','<<actual.jumps<<','<<actual.animation<<'\n';
                }
                require(bool(output)&&step>0,"Native replay output failed");std::cout<<"Replayed "<<step<<" native course-player steps\n";return 0;
            }
            for(size_t i=0;i<world.courses.size();++i) {
                CoursePlayer actual(owned,tuning);actual.reset(world.courses[i].spawn);
                for(int step=0;step<120;++step)actual.tick({},1.f/60,courses[i].scene);
                require(actual.grounded,"Native player missed an authored starting platform");
                float base=actual.position.z,start=actual.position.x;
                for(int step=0;step<20;++step)actual.tick({1,false,false},1.f/60,courses[i].scene);
                for(int step=0;step<60;++step)actual.tick({},1.f/60,courses[i].scene);
                float distance=actual.position.x-start,height=0;
                for(int step=0;step<90;++step) {
                    actual.tick({0,step==0,step<8},1.f/60,courses[i].scene);height=std::max(height,actual.position.z-base);
                }
                require(actual.grounded&&actual.jumps==1,"Native authored starting-platform jump failed");
                std::cout<<"course="<<i<<" settled_z="<<base<<" pulse_distance="<<distance<<" rest_jump_height="<<height<<'\n';
            }
        }
        std::cout<<"Native base-point grounding, held-jump, landing and swept-wall checks passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
