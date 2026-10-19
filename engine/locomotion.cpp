#include "locomotion.h"
#include "asset_reader.h"
#include <algorithm>

namespace Doritos::Engine {
MovementSettings loadMovementSettings(const std::filesystem::path& path) {
    AssetReader input(path);
    need(input.read<uint32_t>()==0x31504e44,"Unsupported native movement settings");
    auto version=input.read<uint32_t>(),count=input.read<uint32_t>();
    need((version==1&&count==17)||(version==2&&count==34),"Unsupported native movement settings version");
    MovementSettings s;
    s.inertiaTime=input.number();s.dragNoInput=input.number();s.dragOverRun=input.number();
    s.slope=input.number();s.slopeNoInput=input.number();s.slopeBoost=input.number();
    s.boostRange=input.number();s.boostFactor=input.number();s.gravity=input.number();
    s.airFriction=input.number();s.jumpGravityFactor=input.number();s.jumpTimeFactor=input.number();
    s.speedChange=input.number();s.minimumSpeed=input.number();s.maximumBoostSpeed=input.number();
    s.easeIn=input.number();s.easeOut=input.number();
    if(version>=2) {
        s.jumpQueue=input.number();s.landRunFactor=input.number();s.landResumeTime=input.number();
        s.jumpFactors={input.number(),input.number()};s.jumpSpeeds={input.number(),input.number()};
        s.airFactors={input.number(),input.number()};s.airSpeeds={input.number(),input.number()};
        s.feetOffset=input.vector();s.feetRadii=input.vector();s.courseParameters=true;
        need(s.jumpQueue>=0&&s.landRunFactor>=0&&s.landResumeTime>=0&&s.jumpSpeeds.x>=0&&s.jumpSpeeds.y>s.jumpSpeeds.x
             &&s.airSpeeds.x>=0&&s.airSpeeds.y>s.airSpeeds.x&&s.feetRadii.x>0&&s.feetRadii.y>0&&s.feetRadii.z>0
             &&s.jumpTimeFactor>=0&&s.jumpTimeFactor<1&&s.jumpGravityFactor<0,"Invalid native player settings");
    }
    input.end();
    need(s.inertiaTime>0&&s.dragNoInput>=0&&s.dragOverRun>=0&&s.boostRange>0&&s.gravity<0&&s.airFriction>=0
         &&s.speedChange>0&&s.speedChange<1&&s.minimumSpeed>0&&s.minimumSpeed<=1
         &&s.maximumBoostSpeed>=1&&s.easeIn>=0&&s.easeOut>=0,"Invalid native movement settings");
    return s;
}
void GroundRun::reset() { phase=0;inertia=0;active=false; }
void GroundRun::resume(float amount) {
    need(std::isfinite(amount)&&amount>=0,"Invalid native landing inertia");
    phase=0;inertia=amount;active=amount>0;
}
RunSample GroundRun::advance(const Clip& slow,const Clip& fast,const MovementSettings& settings,
                            float input,float seconds,float forwardSlope) {
    need(std::isfinite(input)&&input>=0&&input<=1&&std::isfinite(seconds)&&seconds>0&&seconds<=.05f
         &&std::isfinite(forwardSlope)&&std::abs(forwardSlope)<=1,"Invalid native ground-run step");
    need(!slow.motion.empty()&&!fast.motion.empty()&&slow.duration>0&&std::abs(slow.duration-fast.duration)<.001f,
         "Ground locomotion requires synchronized authored motion clips");
    need(settings.inertiaTime>0&&settings.speedChange>0&&settings.speedChange<1,"Invalid ground-run tuning");
    if(!active&&input<.001f)return {};
    active=true;
    float drive=input+forwardSlope*(input<.001f?settings.slopeNoInput:settings.slope);
    float next=inertia+drive/settings.inertiaTime*seconds;
    if(drive<.001f)next-=settings.dragNoInput*inertia*seconds;
    else if(inertia>1)next-=settings.dragOverRun*inertia*seconds;
    inertia=std::max(next,0.f);
    if(input<.001f&&inertia<=.19f) { reset();return {}; }
    float speed=1,blend=0;
    if(inertia<=settings.speedChange)speed=settings.minimumSpeed+(1-settings.minimumSpeed)*inertia/settings.speedChange;
    else blend=std::clamp((inertia-settings.speedChange)/(1-settings.speedChange),0.f,1.f);
    double old=phase;phase+=seconds*speed;
    auto a=rootMotionDelta(slow,old,phase),b=rootMotionDelta(fast,old,phase);
    XMFLOAT3 delta{a.x+(b.x-a.x)*blend,a.y+(b.y-a.y)*blend,a.z+(b.z-a.z)*blend};
    return {std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z),blend,speed,phase,true};
}
}
