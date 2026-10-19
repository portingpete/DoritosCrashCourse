#include "model.h"
#include "collision.h"
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
        model.bones[0].parent=1; rejects([&]{samplePose(model,nullptr,0);}); model.bones[0].parent=-1;
        rejects([&]{samplePose(model,nullptr,std::numeric_limits<float>::quiet_NaN());});
        mesh.vertices[0].bones[0]=9; rejects([&]{skin(mesh,samplePose(model,nullptr,0));});
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
        std::cout<<"Independent native skinning, hierarchy, animation and fixed-step checks passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
