#include "model.h"
#include "collision.h"
#include "locomotion.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Doritos::Engine;
void expect(bool condition,const char* reason) { if(!condition)throw std::runtime_error(reason); }
bool near(float a,float b) { return std::abs(a-b)<.0002f; }
template<class Function> void rejects(Function action) {
    bool rejected=false; try { action(); }catch(const std::runtime_error&){rejected=true;}
    expect(rejected,"Invalid native data was accepted");
}
int main() {
    try {
        Model model; model.bones.resize(2); model.bones[1].parent=0;
        model.bones[1].rest.position={0,2,0}; model.bones[1].inverseBind.position={0,-2,0};
        Mesh mesh; Vertex vertex; vertex.position={1,2,0}; vertex.normal={0,0,1}; vertex.bones[0]=1;
        mesh.vertices.push_back(vertex);
        auto rest=skin(mesh,samplePose(model,nullptr,0));
        expect(near(rest[0].position.x,1)&&near(rest[0].position.y,2),"Native inverse bind does not preserve rest geometry");
        Clip clip; clip.duration=2;
        Track track; track.bone=1;
        Key first; first.transform=model.bones[1].rest;
        Key last=first; last.time=1; last.transform.position={0,4,0}; last.transform.rotation={0,0,1,0};
        track.keys={first,last}; clip.tracks={track};
        auto halfway=skin(mesh,samplePose(model,&clip,.5f));
        expect(near(halfway[0].position.x,0)&&near(halfway[0].position.y,2),"Native quaternion interpolation or parent transform is wrong");
        auto looped=skin(mesh,samplePose(model,&clip,2.5f));
        expect(near(looped[0].position.x,halfway[0].position.x)&&near(looped[0].position.y,halfway[0].position.y),"Native animation looping differs");
        auto blended=skin(mesh,blendPose(model,&clip,0,&clip,1,.5f,false));
        expect(near(blended[0].position.x,halfway[0].position.x)&&near(blended[0].position.y,halfway[0].position.y),"Native local-space pose blending differs from analytic transform");
        Clip motion;motion.duration=2;motion.motion={{0,{0,2,0}},{1,{0,3,0}},{2,{0,6,0}}};
        auto delta=rootMotionDelta(motion,.5,1.5);expect(near(delta.y,2),"Nonuniform authored root motion was flattened");
        delta=rootMotionDelta(motion,1.5,4.5);expect(near(delta.y,6),"Root motion lost displacement across loop boundaries");
        delta=rootMotionDelta(motion,4.5,1.5);expect(near(delta.y,-6),"Reverse root-motion interval differs");
        delta=rootMotionDelta(motion,1,9,false);expect(near(delta.y,3),"One-shot root motion wrapped");
        rejects([&]{rootMotionDelta(motion,0,std::numeric_limits<double>::infinity());});
        Clip slow;slow.duration=1;slow.motion={{0,{}},{1,{0,-2,0}}};
        Clip fast=slow;fast.motion.back().position.y=-4;
        MovementSettings tuning;tuning.inertiaTime=1;tuning.speedChange=.4f;tuning.minimumSpeed=.7f;tuning.dragNoInput=10;
        GroundRun run;
        auto sample=run.advance(slow,fast,tuning,0,.01f);expect(!sample.active&&sample.distance==0,"Idle ground runner produced motion");
        sample=run.advance(slow,fast,tuning,1,.01f);
        expect(near(run.amount(),.01f)&&near(sample.playbackSpeed,.7075f)&&near(sample.distance,.01415f),"Native run acceleration/playback differs from authored curve");
        for(int i=1;i<70;++i)sample=run.advance(slow,fast,tuning,1,.01f);
        expect(near(run.amount(),.7f)&&near(sample.fastBlend,.5f)&&near(sample.distance,.03f),"Native slow/fast root-motion blending differs from analytic displacement");
        float previous=run.amount();sample=run.advance(slow,fast,tuning,0,.01f);
        expect(near(run.amount(),previous*.9f)&&sample.active,"Native no-input inertia decay differs");
        for(int i=0;i<100;++i)sample=run.advance(slow,fast,tuning,0,.01f);
        expect(!sample.active&&run.amount()==0,"Native run failed to return to idle");
        rejects([&]{run.advance(slow,fast,tuning,-1,.01f);});
        model.bones[0].parent=1; rejects([&]{samplePose(model,nullptr,0);}); model.bones[0].parent=-1;
        rejects([&]{samplePose(model,nullptr,std::numeric_limits<float>::quiet_NaN());});
        mesh.vertices[0].bones[0]=9; rejects([&]{skin(mesh,samplePose(model,nullptr,0));});
        for(float facing:{-1.f,1.f}) {
            using namespace DirectX;
            auto world=characterWorld({2,3,4},facing);auto matrix=XMLoadFloat4x4(&world);XMFLOAT3 forward,origin;
            XMStoreFloat3(&forward,XMVector3TransformNormal(XMVectorSet(0,-1,0,0),matrix));
            XMStoreFloat3(&origin,XMVector3TransformCoord(XMVectorZero(),matrix));
            expect(near(forward.x,facing)&&near(forward.y,0)&&near(forward.z,0)&&near(origin.x,2)&&near(origin.y,3)&&near(origin.z,4),
                   "Cal3D forward heading differs from course travel direction or actor position");
        }
        rejects([&]{characterWorld({},0);});
        Player facingPlayer;facingPlayer.tick({-1,false},.01f);facingPlayer.tick({},.01f);
        expect(facingPlayer.facing==-1,"Native arena lost its last heading after releasing input");
        Player player; float maximum=0;
        for(int i=0;i<1200;++i) { player.tick({i<600?1.f:0.f,i==120},1.f/120); maximum=std::max(maximum,player.position.z); }
        expect(near(player.position.x,15)&&player.grounded&&player.animation=="idle"&&maximum>.9f,"Native fixed-step movement/jump arena failed");
        rejects([&]{player.tick({},1);});
        std::vector<Triangle> triangles;
        for(int x=0;x<20;++x)for(int y=0;y<20;++y) {
            triangles.push_back({{float(x),float(y),0},{float(x+1),float(y),0},{float(x),float(y+1),0}});
            triangles.push_back({{float(x+1),float(y),0},{float(x+1),float(y+1),0},{float(x),float(y+1),0}});
        }
        CollisionScene collision(std::move(triangles));
        for(int x=0;x<20;++x)for(int y=0;y<20;++y) {
            auto hit=collision.ray({x+.25f,y+.25f,5},{0,0,-2},10);
            expect(hit&&near(hit->distance,5)&&near(hit->normal.z,1)&&near(hit->position.z,0),"Native spatial tree missed an analytic floor ray");
        }
        expect(!collision.ray({-1,-1,5},{0,0,-1},10)&&!collision.ray({1,1,5},{0,0,-1},4),"Native collision accepted out-of-bounds/short ray");
        auto reverse=collision.ray({1.25f,1.25f,-3},{0,0,1},5);
        expect(reverse&&near(reverse->distance,3)&&near(reverse->normal.z,-1),"Native back-face query failed");
        rejects([&]{collision.ray({},{},1);});
        auto sweep=collision.sweepEllipsoid({5,5,2},{.5f,.5f,.8f},{0,0,-100});
        expect(sweep&&near(sweep->fraction,.012f)&&near(sweep->normal.z,1),"Native ellipsoid tunneled through thin floor");
        auto moved=collision.moveEllipsoid({5,5,2},{.5f,.5f,.8f},{2,0,-100});
        expect(near(moved.position.x,7)&&std::abs(moved.position.z-.8f)<.001f,"Native slide lost floor-tangent displacement");
        auto overlap=collision.moveEllipsoid({5,5,.25f},{.5f,.5f,.5f},{});
        expect(overlap.position.z>=.5f&&overlap.position.z<.501f,"Native initial overlap was not resolved");
        CollisionScene edge({{{0,0,0},{10,0,0},{0,10,0}}});
        sweep=edge.sweepEllipsoid({-2,0,0},{.5f,.5f,.5f},{4,0,0});
        expect(sweep&&near(sweep->fraction,.375f)&&near(sweep->normal.x,-1),"Native swept vertex contact differs from analytic sphere");
        CollisionScene wall({{{3,-10,-10},{3,10,-10},{3,10,10}},{{3,-10,-10},{3,10,10},{3,-10,10}}});
        moved=wall.moveEllipsoid({0,0,1},{.25f,.5f,.8f},{10,1,0});
        expect(std::abs(moved.position.x-2.75f)<.001f&&near(moved.position.y,1),"Native wall sweep/slide failed");
        rejects([&]{collision.sweepEllipsoid({},{0,1,1},{});});
        std::cout<<"Independent native skinning, hierarchy, animation and fixed-step checks passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
