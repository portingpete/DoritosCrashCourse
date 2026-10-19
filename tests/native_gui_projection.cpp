#include "renderer/native/gui_projection.h"
#include <cstdio>
#include <stdexcept>

using namespace Doritos::Graphics;
namespace {
void require(bool condition, const char* detail) { if (!condition) throw std::runtime_error(detail); }
void near(float actual, float expected, const char* detail) {
    require(std::isfinite(actual) && std::abs(actual - expected) < .000001f,detail);
}
using Triangle = std::array<GuiProjectionVertex,3>;
const Triangle flat{{{{-1,1,.5f,1},{0,0}},{{1,1,.5f,1},{1,0}},{{-1,-1,.5f,1},{0,1}}}};
void rejects(const Triangle& triangle, const std::array<float,2>& point, const char* detail) {
    GuiProjectionHit hit{{7,8},9};
    require(!projectedGuiTriangleHit(triangle,point,hit),detail);
    require(hit.uv == std::array<float,2>{7,8} && hit.depth == 9,"failed hit preserves its output");
}
}
int main() {
    try {
        GuiProjectionHit hit{};
        require(projectedGuiTriangleHit(flat,{.25f,.25f},hit),"flat triangle interior intersects");
        near(hit.uv[0],.25f,"flat U follows viewport X");
        near(hit.uv[1],.25f,"top-left screen origin reverses clip Y");
        near(hit.depth,.5f,"flat D3D depth");

        // Independent screen barycentrics are (.5,.25,.25). Unequal clip W
        // gives texture weights (.5,.125,.0625), whose sum is .6875.
        const Triangle perspective{{{{-1,1,.2f,1},{0,0}},{{2,2,1.2f,2},{1,0}},{{-4,-4,3.2f,4},{0,1}}}};
        require(projectedGuiTriangleHit(perspective,{.25f,.25f},hit),"perspective triangle intersects");
        near(hit.uv[0],2.f / 11,"U uses reciprocal W");
        near(hit.uv[1],1.f / 11,"V uses reciprocal W");
        near(hit.depth,.45f,"depth linearly interpolates Z divided by W");
        auto reversed = perspective;
        std::swap(reversed[1],reversed[2]);
        GuiProjectionHit reverseHit{};
        require(projectedGuiTriangleHit(reversed,{.25f,.25f},reverseHit),"both triangle windings work");
        near(reverseHit.uv[0],hit.uv[0],"winding preserves U");
        near(reverseHit.uv[1],hit.uv[1],"winding preserves V");

        require(projectedGuiTriangleHit(flat,{0,0},hit),"vertex boundary intersects");
        near(hit.uv[0],0,"vertex U"); near(hit.uv[1],0,"vertex V");
        require(projectedGuiTriangleHit(flat,{.5f,.5f},hit),"shared edge boundary intersects");
        near(hit.uv[0],.5f,"edge U"); near(hit.uv[1],.5f,"edge V");
        require(projectedGuiTriangleHit(flat,{.5f,std::nextafter(.5f,1.f)},hit),
            "float rounding at a tessellation seam does not lose hover");
        near(hit.uv[0],.5f,"rounded seam U"); near(hit.uv[1],.5f,"rounded seam V");
        rejects(flat,{.5f,.5001f},"real space beyond a triangle edge remains outside");
        rejects(flat,{.6f,.6f},"point outside triangle rejects");
        rejects(flat,{-0.01f,.2f},"point outside viewport rejects");
        rejects(flat,{.2f,1.01f},"point beyond viewport rejects");
        rejects(flat,{std::numeric_limits<float>::quiet_NaN(),.2f},"nonfinite pointer rejects");

        auto invalid = flat;
        invalid[2] = invalid[1];
        rejects(invalid,{.25f,.25f},"duplicate vertices reject");
        invalid = flat; invalid[2].clip = {0,1,.5f,1};
        rejects(invalid,{.25f,.25f},"collinear projected vertices reject");
        for (const float w : {0.f,-1.f}) {
            invalid = flat; invalid[1].clip[3] = w;
            rejects(invalid,{.25f,.25f},"nonpositive W rejects");
        }
        for (size_t vertex = 0; vertex < 3; ++vertex) {
            for (size_t axis = 0; axis < 4; ++axis) {
                invalid = flat; invalid[vertex].clip[axis] = std::numeric_limits<float>::infinity();
                rejects(invalid,{.25f,.25f},"nonfinite clip component rejects");
            }
            for (size_t axis = 0; axis < 2; ++axis) {
                invalid = flat; invalid[vertex].uv[axis] = std::numeric_limits<float>::quiet_NaN();
                rejects(invalid,{.25f,.25f},"nonfinite UV component rejects");
            }
        }
        for (const float depth : {-.1f,1.1f}) {
            invalid = flat;
            for (auto& vertex : invalid) vertex.clip[2] = depth;
            rejects(invalid,{.25f,.25f},"depth outside D3D clip range rejects");
        }
        auto cropped = flat;
        cropped[0].clip[0] = -2;
        require(projectedGuiTriangleHit(cropped,{.1f,.1f},hit),"partially clipped triangle retains its visible interior");
        auto unboundedUV = flat;
        for (auto& vertex : unboundedUV) vertex.uv = {-2,3};
        require(projectedGuiTriangleHit(unboundedUV,{.25f,.25f},hit),"legitimate material UV outside zero-to-one remains representable");
        near(hit.uv[0],-2,"unbounded texture U"); near(hit.uv[1],3,"unbounded texture V");

        std::optional<GuiProjectionHit> closest;
        require(updateClosestProjectedGuiHit(flat,{.25f,.25f},closest),"first valid surface wins");
        require(!updateClosestProjectedGuiHit(flat,{.25f,.25f},closest),"equal depth preserves first surface");
        auto nearer = flat;
        for (auto& vertex : nearer) { vertex.clip[2] = .1f; vertex.uv = {.7f,.8f}; }
        require(updateClosestProjectedGuiHit(nearer,{.25f,.25f},closest),"nearer overlapping surface replaces farther hit");
        require(!updateClosestProjectedGuiHit(flat,{.25f,.25f},closest),"farther surface preserves nearest hit");
        require(!updateClosestProjectedGuiHit(invalid,{.25f,.25f},closest),"invalid surface preserves nearest hit");
        near(closest->depth,.1f,"nearest surface depth");
        near(closest->uv[0],.7f,"nearest surface U"); near(closest->uv[1],.8f,"nearest surface V");
        std::puts("native GUI projection contract passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"native GUI projection contract failed: %s\n",error.what());
        return 1;
    }
}
