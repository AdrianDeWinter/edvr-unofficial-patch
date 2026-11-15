#pragma once
// Parallel projection left on for a canted Pimax (docs/canted-projection.md, "PP left on"; src/common/parallel_projection_notice.h): which headsets are
// canted models, whether two located eye orientations are parallel (and what is not an orientation at all), which runtimes are proven to apply Pimax
// Play's toggle, the verdict those make, the words, the Status page's shared hint slot, the host's once-a-session decision on a fixture host, and the
// wiring as source pins. The angle maths is checked against an independent definition written out here (2 * acos |a . b|), on orientations built by
// quaternion products rather than by the header's own formulas.
#include "launch_centre_cases.h"
#include "../../src/common/parallel_projection_notice.h"
#include "../../src/common/terrain_checkerboard_notice.h"
#include "../../src/common/vr_supersample_notice.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace edvr::openxr::test {
namespace parallel_fixture {
using Quat=XrQuaternionf;
constexpr double kPi=3.14159265358979323846;
inline Quat axisAngle(double ax,double ay,double az,double degrees){
  const double n=std::sqrt(ax*ax+ay*ay+az*az),h=degrees*kPi/360.0,s=std::sin(h)/n;
  return Quat{float(ax*s),float(ay*s),float(az*s),float(std::cos(h))};
}
inline Quat yaw(double degrees){return axisAngle(0,1,0,degrees);}
// The Hamilton product a*b, in doubles: b first, then a.
inline Quat mul(const Quat& a,const Quat& b){
  const double x=double(a.w)*b.x+double(a.x)*b.w+double(a.y)*b.z-double(a.z)*b.y;
  const double y=double(a.w)*b.y-double(a.x)*b.z+double(a.y)*b.w+double(a.z)*b.x;
  const double z=double(a.w)*b.z+double(a.x)*b.y-double(a.y)*b.x+double(a.z)*b.w;
  const double w=double(a.w)*b.w-double(a.x)*b.x-double(a.y)*b.y-double(a.z)*b.z;
  return Quat{float(x),float(y),float(z),float(w)};
}
// The definition of the angle between two orientations: 2 * acos |a . b|. Good to ~1e-3 degrees at the angles it is used for (large ones).
inline double referenceAngle(const Quat& a,const Quat& b){
  const double dot=std::fabs(double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z+double(a.w)*b.w);
  return 2.0*std::acos((std::min)(1.0,dot))*180.0/kPi;
}
// A row-major 3x4 pose from an orientation and an x translation: the shape the menu client publishes the eye poses in.
inline void pose3x4(const Quat& q,float tx,float out[12]){
  const double x=q.x,y=q.y,z=q.z,w=q.w;
  const double r[3][3]={{1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)},{2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)},{2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)}};
  for(int i=0;i<3;++i){for(int j=0;j<3;++j)out[i*4+j]=float(r[i][j]);out[i*4+3]=0;}
  out[3]=tx;
}
inline std::string readSource(const char* path){
  std::ifstream in(path,std::ios::binary);std::ostringstream all;all<<in.rdbuf();
  std::string text=all.str();text.erase(std::remove(text.begin(),text.end(),'\r'),text.end());return text;
}
inline bool has(const std::string& text,const char* needle){return text.find(needle)!=std::string::npos;}
inline size_t count(const std::string& text,const char* needle){size_t n=0;for(size_t at=text.find(needle);at!=std::string::npos;at=text.find(needle,at+1))++n;return n;}
}
template<class Check> void runParallelProjectionNoticeCases(Check&& check) {
  using namespace parallel_fixture;
  namespace ppn=edvr::ppn;
  // ---- which headsets are canted models ---------------------------------------------------------------------------------------------------------------
  {
    // The name PimaxXR reported on the 8K X this was built from, and every model of the family, spelled the ways a runtime might.
    const char* const canted[]={"Pimax Vision 8K X","Pimax Vision 8K Plus","Pimax Vision 8K","Pimax 8K","Pimax 8K Plus","Pimax 8K X","Pimax 8K+","Pimax Vision 8K+",
      "Pimax 5K Super","Pimax Vision 5K Super","Pimax 5K Plus","Pimax 5K+","Pimax Vision 5K Plus","Pimax 5K XR","Pimax Vision 5K XR","Pimax Artisan","Pimax Vision Artisan"};
    bool all=true;for(const char* name:canted)if(!ppn::cantedPimaxModel(name)){all=false;std::printf("  not recognised as a canted model: \"%s\"\n",name);}
    check(all,"canted models: the observed \"Pimax Vision 8K X\" and every model of the family (8K X, 8K Plus, 8K, 5K Super, 5K Plus, 5K XR, Artisan) are recognised");
    const char* const spellings[]={"PIMAX VISION 8K X","pimax vision 8k x","Pimax-Vision-8K-X","Pimax_Vision_8KX","  Pimax   Vision   8K   X  ","Pimax (Vision) 8K, X","pimaxvision8kx",
      "PIMAX_5K_PLUS","pimax.5k.xr","Pimax\tArtisan"};
    all=true;for(const char* name:spellings)if(!ppn::cantedPimaxModel(name)){all=false;std::printf("  not recognised as a canted model: \"%s\"\n",name);}
    check(all,"canted models: case, spaces and punctuation are ignored");
    const char* const notCanted[]={"Pimax Crystal","Pimax Crystal Super","Pimax Crystal Light","Pimax Crystal Pro","Pimax 12K","Pimax Dream Air","Pimax Dream Air SE","Pimax Portal",
      "Pimax","Pimax Vision","Pimax OpenXR","PimaxXR","Valve Index","Meta Quest 3","Oculus Quest 2","Oculus Rift S","HP Reverb G2","HTC Vive Pro 2","Acme 8K X","Acme Vision 8K Plus",
      "Varjo Aero","Pico 4","","   ","++","8K X"};
    bool none=true;for(const char* name:notCanted)if(ppn::cantedPimaxModel(name)){none=false;std::printf("  wrongly recognised as a canted model: \"%s\"\n",name);}
    check(none,"canted models: Crystal, Crystal Super, Crystal Light, 12K, Dream Air, Portal, a bare Pimax and every other maker (including one that says 8K X) are not");
    check(!ppn::cantedPimaxModel(nullptr),"canted models: no name is not a canted model");
    std::string longName="Pimax Vision 8K X";longName.append(400,'a');
    std::string longPrefix(300,'x');longPrefix+=" Pimax Vision 8K X";
    check(ppn::cantedPimaxModel(longName.c_str())&&!ppn::cantedPimaxModel(longPrefix.c_str()),
          "canted models: a name past the comparison buffer is cut, not overrun: the model before the cut counts, one after it does not");
    // The runtime that is proven to apply the toggle.
    check(ppn::honoursToggle("PimaxXR (Unofficial)")&&ppn::honoursToggle("PimaxXR")&&ppn::honoursToggle("pimax xr")&&ppn::honoursToggle("PIMAXXR"),
          "toggle: PimaxXR, however it is spelled, applies Pimax Play's toggle");
    check(!ppn::honoursToggle("Pimax OpenXR")&&!ppn::honoursToggle("SteamVR/OpenXR")&&!ppn::honoursToggle("Oculus")&&!ppn::honoursToggle("Monado")&&
              !ppn::honoursToggle("Pimax")&&!ppn::honoursToggle("")&&!ppn::honoursToggle(nullptr),
          "toggle: Pimax Play's own runtime (\"Pimax OpenXR\"), SteamVR, any other runtime, a bare Pimax and no name are not known to");
  }
  // ---- two located eye orientations: parallel or not ---------------------------------------------------------------------------------------------------
  {
    // A canted P2 pair as xrLocateViews gives it with parallel projection off, and the same pair with it on.
    const Quat left=yaw(-10),right=yaw(10);
    check(std::fabs(ppn::viewAngleDegrees(left,right)-20.0)<1e-3&&std::fabs(ppn::viewAngleDegrees(right,left)-20.0)<1e-3,"views: -10 and +10 degrees of yaw are 20 degrees apart, either way round");
    check(!ppn::parallelViews(left,right)&&ppn::classifyViews(left,right)==ppn::Views::Canted,"views: a canted pair (20 degrees apart) is not parallel");
    const Quat flat{0,0,0,1};
    check(ppn::parallelViews(flat,flat)&&ppn::classifyViews(flat,flat)==ppn::Views::Parallel&&ppn::viewAngleDegrees(flat,flat)==0.0,"views: 0 and 0 degrees (parallel projection on) are parallel, exactly 0 apart");
    // The boundary: under 0.5 degrees is parallel.
    const double steps[]={0.0,0.1,0.4,0.49,0.51,0.6,1.0,5.0,20.0};
    const bool expectParallel[]={true,true,true,true,false,false,false,false,false};
    bool boundary=true;
    for(unsigned i=0;i<sizeof(steps)/sizeof(steps[0]);++i){
      const Quat a=flat,b=yaw(steps[i]);
      boundary=boundary&&ppn::parallelViews(a,b)==expectParallel[i]&&ppn::parallelViews(b,a)==expectParallel[i]&&std::fabs(ppn::viewAngleDegrees(a,b)-steps[i])<2e-3;
      const Quat l=yaw(-steps[i]/2),r=yaw(steps[i]/2);   // the same angle split across the two eyes, as a runtime places them
      boundary=boundary&&ppn::parallelViews(l,r)==expectParallel[i];
    }
    check(boundary,"views: 0, 0.1, 0.4, 0.49 degrees apart are parallel and 0.51, 0.6, 1, 5, 20 are not, each way round and split across the eyes");
    check(ppn::viewsFromAngle(0.4999999)==ppn::Views::Parallel&&ppn::viewsFromAngle(ppn::kParallelUnderDegrees)==ppn::Views::Canted&&ppn::viewsFromAngle(0.0)==ppn::Views::Parallel&&
              ppn::kParallelUnderDegrees==0.5,
          "views: the limit is under 0.5 degrees, strictly: exactly 0.5 is canted");
    // Pitch and roll count as well as yaw (the angle is the whole rotation between the eyes), and a head turn shared by both eyes changes nothing.
    const Quat pitched=axisAngle(1,0,0,20),rolled=axisAngle(0,0,1,20),tilt=axisAngle(1,2,3,20);
    check(!ppn::parallelViews(flat,pitched)&&!ppn::parallelViews(flat,rolled)&&!ppn::parallelViews(flat,tilt)&&std::fabs(ppn::viewAngleDegrees(flat,tilt)-20.0)<1e-3&&
              std::fabs(ppn::viewAngleDegrees(flat,pitched)-20.0)<1e-3&&std::fabs(ppn::viewAngleDegrees(flat,rolled)-20.0)<1e-3,
          "views: 20 degrees of pitch, of roll and about a skew axis are 20 degrees apart");
    const Quat head=mul(mul(yaw(37),axisAngle(1,0,0,11)),axisAngle(0,0,1,-5));
    const Quat hl=mul(head,left),hr=mul(head,right),pl=mul(head,flat),pr=mul(head,flat);
    check(std::fabs(ppn::viewAngleDegrees(hl,hr)-20.0)<1e-3&&std::fabs(ppn::viewAngleDegrees(hl,hr)-referenceAngle(hl,hr))<1e-3&&!ppn::parallelViews(hl,hr)&&ppn::parallelViews(pl,pr),
          "views: a head turned 37 yaw, 11 pitch, -5 roll carries both eyes with it: the canted pair stays 20 apart and the parallel pair stays parallel");
    const Quat negated{-right.x,-right.y,-right.z,-right.w};
    check(ppn::parallelViews(right,negated)&&ppn::viewAngleDegrees(right,negated)<1e-3,"views: q and -q are the same orientation");
    // Invalid input is Unknown, never parallel and never canted.
    const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
    const Quat bad[]={Quat{nan,0,0,1},Quat{0,inf,0,1},Quat{0,0,0,0},Quat{0,0,0,2},Quat{0.1f,0,0,1},Quat{0,0,0,-inf},Quat{0.5f,0.5f,0.5f,0.0f},Quat{0,0,0,1.01f}};
    bool unknown=true;
    for(const Quat& b:bad){
      unknown=unknown&&ppn::classifyViews(flat,b)==ppn::Views::Unknown&&ppn::classifyViews(b,flat)==ppn::Views::Unknown&&!ppn::parallelViews(flat,b)&&!ppn::parallelViews(b,flat)&&
              !ppn::parallelViews(b,b)&&ppn::viewAngleDegrees(flat,b)<0.0;
    }
    check(unknown,"views: NaN, infinite, zero-length, non-unit (squared length 4, 1.01, 1.0201) and half-formed quaternions are Unknown on either side, never parallel, never canted");
    check(ppn::parallelViews(Quat{0,0,0,1.0004f},flat)&&ppn::classifyViews(Quat{0,0,0,1.0004f},flat)==ppn::Views::Parallel,
          "views: a quaternion a rounding error off unit length is still an orientation");
    check(ppn::viewsFromAngle(-1.0)==ppn::Views::Unknown&&ppn::viewsFromAngle(std::numeric_limits<double>::quiet_NaN())==ppn::Views::Unknown,"views: an angle of -1 or NaN is Unknown");
    // The same decision from the 3x4 poses the graphics half holds: translation does not matter, the rotation does, and a bad matrix is Unknown.
    float pl3[12],pr3[12],canted3[12],tilt3[12];
    pose3x4(flat,-0.032f,pl3);pose3x4(flat,0.032f,pr3);pose3x4(left,-0.032f,canted3);pose3x4(right,0.032f,tilt3);
    check(ppn::classifyRotations(pl3,pr3)==ppn::Views::Parallel&&ppn::classifyRotations(canted3,tilt3)==ppn::Views::Canted&&std::fabs(ppn::rotationAngleDegrees(canted3,tilt3)-20.0)<1e-3,
          "poses: the 3x4 form agrees: parallel rotations a head width apart are parallel, a -10/+10 degree pair is 20 apart");
    bool agree=true;
    const double offsets[]={0.0,0.3,0.4,0.6,2.0,20.0,90.0,179.0};
    for(double d:offsets){
      float ma[12],mb[12];pose3x4(mul(head,flat),0.0f,ma);pose3x4(mul(head,yaw(d)),0.0f,mb);
      agree=agree&&std::fabs(ppn::rotationAngleDegrees(ma,mb)-d)<2e-3&&ppn::classifyRotations(ma,mb)==(d<0.5?ppn::Views::Parallel:ppn::Views::Canted);
      agree=agree&&std::fabs(ppn::rotationAngleDegrees(ma,mb)-ppn::viewAngleDegrees(mul(head,flat),mul(head,yaw(d))))<2e-3;
    }
    check(agree,"poses: the matrix angle matches the quaternion angle and the construction from 0 to 179 degrees, under a shared head turn, with the same 0.5 degree limit");
    float scaled[12],reflected[12],nanned[12],skew[12];
    std::memcpy(scaled,pr3,sizeof(scaled));std::memcpy(reflected,pr3,sizeof(reflected));std::memcpy(nanned,pr3,sizeof(nanned));std::memcpy(skew,pr3,sizeof(skew));
    scaled[0]=1.1f;reflected[10]=-1.0f;nanned[5]=nan;skew[1]=0.05f;
    check(ppn::classifyRotations(pl3,scaled)==ppn::Views::Unknown&&ppn::classifyRotations(reflected,pl3)==ppn::Views::Unknown&&ppn::classifyRotations(pl3,nanned)==ppn::Views::Unknown&&
              ppn::classifyRotations(skew,pl3)==ppn::Views::Unknown&&ppn::classifyRotations(nullptr,pl3)==ppn::Views::Unknown&&ppn::classifyRotations(pl3,nullptr)==ppn::Views::Unknown,
          "poses: a scaled, mirrored, NaN or skewed rotation, or no pose at all, is Unknown");
  }
  // ---- the verdict ------------------------------------------------------------------------------------------------------------------------------------
  {
    using V=ppn::Verdict;using W=ppn::Views;
    const char* const xr="PimaxXR (Unofficial)";const char* const play="Pimax OpenXR";
    check(ppn::decide("Pimax Vision 8K X",xr,W::Parallel)==V::Notice,"verdict: the observed rig with parallel projection on (Pimax Vision 8K X, PimaxXR, parallel views) gets the notice");
    check(ppn::decide("Pimax Vision 8K X",xr,W::Canted)==V::ProjectionOff,"verdict: the same rig with it off (canted views) is the good state");
    check(ppn::decide("Pimax Vision 8K X",play,W::Parallel)==V::LogOnly&&ppn::decide("Pimax Vision 8K X","SteamVR/OpenXR",W::Parallel)==V::LogOnly&&ppn::decide("Pimax Vision 8K X","",W::Parallel)==V::LogOnly,
          "verdict: a runtime not known to apply the toggle (Pimax Play's own, SteamVR, no name) gets the log line only");
    check(ppn::decide("Pimax Vision 8K X",play,W::Canted)==V::ProjectionOff,"verdict: ...and canted views are the good state whatever the runtime");
    check(ppn::decide("Pimax Vision 8K X",xr,W::Unknown)==V::NotApplicable&&ppn::decide("Pimax Vision 8K X",play,W::Unknown)==V::NotApplicable,
          "verdict: views that are not known yet decide nothing, on any runtime");
    const char* const notCanted[]={"Pimax Crystal","Pimax Crystal Super","Pimax Crystal Light","Pimax 12K","Pimax Dream Air","Pimax Portal","Meta Quest 3","Valve Index","",nullptr};
    bool quiet=true;
    for(const char* name:notCanted)for(const char* runtime:{xr,play,static_cast<const char*>("")})for(W views:{W::Unknown,W::Parallel,W::Canted})quiet=quiet&&ppn::decide(name,runtime,views)==V::NotApplicable;
    check(quiet,"verdict: a headset that is not a canted model is never anything, parallel views being what it is supposed to report");
    const char* const family[]={"Pimax Vision 8K X","Pimax Vision 8K Plus","Pimax Vision 8K","Pimax 5K Super","Pimax 5K Plus","Pimax 5K XR","Pimax Artisan"};
    bool every=true;
    for(const char* name:family)every=every&&ppn::decide(name,xr,W::Parallel)==V::Notice&&ppn::decide(name,play,W::Parallel)==V::LogOnly&&ppn::decide(name,xr,W::Canted)==V::ProjectionOff;
    check(every,"verdict: every model of the family gets the same three answers");
  }
  // ---- the words ----------------------------------------------------------------------------------------------------------------------------------------
  {
    char line[1024];
    const char* const xr="PimaxXR (Unofficial)";
    const int n=ppn::formatLog(line,sizeof(line),ppn::Verdict::Notice,"Pimax Vision 8K X",xr,8268,3948,0.0);
    const std::string expected="canted eyes: parallel projection is on: this Pimax Vision 8K X is canted, but PimaxXR (Unofficial) reports parallel views (8268x3948 per eye). EDVR renders canted "
        "displays without it; turning \"Enable Parallel Projection\" off in Pimax Play and restarting the game cuts GPU time by up to about 30%.";
    check(n==int(expected.size())&&expected==line,"words: the notice's log line names the headset, the runtime and the recommended size, and says what to turn off in Pimax Play");
    ppn::formatLog(line,sizeof(line),ppn::Verdict::LogOnly,"Pimax Vision 8K X","Pimax OpenXR",8268,3948,0.0);
    const std::string unverified=line;
    check(unverified.find("canted eyes: parallel projection is on: this Pimax Vision 8K X is canted, but Pimax OpenXR reports parallel views (8268x3948 per eye).")==0&&
              has(unverified,"whether this runtime honours Pimax Play's toggle is unverified.")&&!has(unverified,"turning")&&!has(unverified,"30%"),
          "words: a runtime not known to apply the toggle gets the same first clause, names itself, says its toggle is unverified and sends the user nowhere");
    ppn::formatLog(line,sizeof(line),ppn::Verdict::ProjectionOff,"Pimax Vision 8K X",xr,5016,3160,20.0);
    const std::string off=line;
    check(off.find("canted eyes: parallel projection is off: this Pimax Vision 8K X is canted and PimaxXR (Unofficial) reports canted views, 20.00 degrees apart (5016x3160 per eye);")==0&&
              !has(off,"is on")&&!has(off,"unverified"),
          "words: the good state is logged as its own line, so a quiet log means the feature did not run and this line means it did and found nothing to say");
    std::memset(line,'x',sizeof(line));
    check(ppn::formatLog(line,sizeof(line),ppn::Verdict::NotApplicable,"Pimax Vision 8K X",xr,8268,3948,0.0)==0&&line[0]==0,"words: not applicable writes nothing");
    ppn::formatLog(line,sizeof(line),ppn::Verdict::Notice,"",nullptr,1,2,0.0);
    check(has(line,"this Pimax headset is canted, but the runtime reports")&&has(line,"(1x2 per eye)"),"words: a missing name reads as a placeholder, not as an empty gap");
    char tiny[40];std::memset(tiny,'x',sizeof(tiny));
    ppn::formatLog(tiny,sizeof(tiny),ppn::Verdict::Notice,"Pimax Vision 8K X",xr,8268,3948,0.0);
    check(std::strlen(tiny)==sizeof(tiny)-1,"words: a buffer too small gets a cut, terminated line, not an overrun");
    check(std::strlen(expected.c_str())<600&&unverified.size()<600&&off.size()<600,"words: each log line is well inside the trace line buffer (4096 bytes) and the host's 1024-byte local");
    char toast[96],hint[96];
    ppn::formatToast(toast,sizeof(toast));ppn::formatStatusHint(hint,sizeof(hint));
    check(std::strlen(toast)>0&&std::strlen(toast)<=ppn::kToastMax&&std::strlen(hint)>0&&std::strlen(hint)<=ppn::kHintMax&&ppn::kToastMax==tcn::kToastMax&&ppn::kHintMax==tcn::kHintMax,
          "words: the toast fits one line of the toast card and the hint two lines of the Status page's box (the same limits the other notices keep)");
    check(has(toast,"Parallel projection is on")&&has(toast,"Pimax Play")&&has(hint,"Parallel projection is on")&&has(hint,"Pimax Play")&&has(hint,"restart"),
          "words: both short forms say what is on, where to turn it off, and (the hint) that the game restarts");
    char other[200];
    vrss::formatToast(other,sizeof(other));const std::string ss=other;vrss::formatStatusHint(other,sizeof(other));const std::string ssHint=other;
    tcn::formatToast(other,sizeof(other));const std::string cb=other;tcn::formatStatusHint(other,sizeof(other));const std::string cbHint=other;
    check(ss!=toast&&cb!=toast&&ssHint!=hint&&cbHint!=hint,"words: the three notices' short forms are three different texts");
  }
  // ---- the Status page's one hint slot ----------------------------------------------------------------------------------------------------------------------
  {
    bool alone=true;for(uint64_t t:{uint64_t(0),uint64_t(5999),uint64_t(6000),uint64_t(12000),uint64_t(123456789)})alone=alone&&ppn::statusHintTurn(false,t,6000);
    check(alone,"hint slot: with neither of the other two hints applying this one shows, at every time");
    check(!ppn::statusHintTurn(true,0,6000)&&!ppn::statusHintTurn(true,5999,6000)&&!ppn::statusHintTurn(true,6000,6000)&&!ppn::statusHintTurn(true,11999,6000)&&
              ppn::statusHintTurn(true,12000,6000)&&ppn::statusHintTurn(true,17999,6000)&&!ppn::statusHintTurn(true,18000,6000)&&!ppn::statusHintTurn(true,24000,6000)&&!ppn::statusHintTurn(true,29999,6000)&&ppn::statusHintTurn(true,30000,6000)&&ppn::statusHintTurn(true,35999,6000),
          "hint slot: with another hint applying it takes the third 6 s turn: 12.0-17.9 s, then 30.0-35.9 s, and not before");
    check(!ppn::statusHintTurn(true,12000,0),"hint slot: a zero turn length never divides by zero and never wins");
    // None of the three is ever hidden: over thirty turns, with both others applying, each of the three shows in some turn, and in a turn exactly one does.
    bool shows[3]={false,false,false},oneEach=true;
    for(uint64_t turn=0;turn<30;++turn){
      const uint64_t now=turn*tcn::kHintAlternateMs+17;
      const auto picked=tcn::pickStatusHint(true,true,now);
      const bool parallelNow=ppn::statusHintTurn(true,now,tcn::kHintAlternateMs);
      const int which=parallelNow?2:(picked==tcn::Hint::Supersampling?0:1);
      shows[which]=true;
    }
    for(bool s:shows)oneEach=oneEach&&s;
    check(oneEach,"hint slot: with supersampling, checkerboard and parallel projection all applying, each of the three shows in some turn");
    bool turns[2]={false,false};   // with one other hint applying it shows for itself in some turns and yields in others
    for(uint64_t turn=0;turn<12;++turn)turns[ppn::statusHintTurn(true,turn*tcn::kHintAlternateMs,tcn::kHintAlternateMs)?1:0]=true;
    check(turns[0]&&turns[1],"hint slot: with one other hint applying the two share the slot, neither is hidden");
  }
  // ---- the host decides once a session, from real located views ----------------------------------------------------------------------------------------------
  {
    using namespace launch_fixture;
    const Quat left=yaw(-10),right=yaw(10),flat{0,0,0,1};
    auto geometry=[&](const Quat& l,const Quat& r){GeometryInput g{};g.views[0].pose.orientation=l;g.views[1].pose.orientation=r;return g;};
    auto name=[&](NativeRuntimeHost& h,const char* system,const char* runtime){
      NativeRuntimeHost::copyLabel(h.systemLabel,system);NativeRuntimeHost::copyLabel(h.runtimeLabel,runtime);
      h.sizes[0].recommendedImageRectWidth=h.sizes[1].recommendedImageRectWidth=8268;h.sizes[0].recommendedImageRectHeight=h.sizes[1].recommendedImageRectHeight=3948;
    };
    {
      Fixture f;auto& h=f.host;name(h,"Pimax Vision 8K X","PimaxXR (Unofficial)");
      check(!h.projectionModeNoted&&h.projectionVerdict==ppn::Verdict::NotApplicable,"host: before any locate nothing is decided");
      Quat unset{0,0,0,0};
      h.noteProjectionMode(geometry(unset,unset));
      h.noteProjectionMode(geometry(flat,Quat{std::numeric_limits<float>::quiet_NaN(),0,0,1}));
      check(!h.projectionModeNoted&&h.projectionVerdict==ppn::Verdict::NotApplicable,"host: a locate whose views carry no usable orientation decides nothing and latches nothing");
      h.noteProjectionMode(geometry(flat,flat));
      check(h.projectionModeNoted&&h.projectionVerdict==ppn::Verdict::Notice,"host: the first real pair (parallel) on the observed rig decides Notice");
      h.noteProjectionMode(geometry(left,right));
      check(h.projectionVerdict==ppn::Verdict::Notice,"host: once a session: a later pair does not decide again");
    }
    {
      Fixture f;auto& h=f.host;name(h,"Pimax Vision 8K X","PimaxXR (Unofficial)");
      h.noteProjectionMode(geometry(left,right));
      check(h.projectionModeNoted&&h.projectionVerdict==ppn::Verdict::ProjectionOff,"host: canted views on the same rig decide ProjectionOff");
    }
    {
      Fixture f;auto& h=f.host;name(h,"Pimax Vision 8K X","Pimax OpenXR");
      h.noteProjectionMode(geometry(flat,flat));
      check(h.projectionModeNoted&&h.projectionVerdict==ppn::Verdict::LogOnly,"host: Pimax Play's own runtime on a parallel pair decides LogOnly");
    }
    {
      Fixture f;auto& h=f.host;name(h,"Pimax Crystal Super","Pimax OpenXR");
      h.noteProjectionMode(geometry(flat,flat));
      check(h.projectionModeNoted&&h.projectionVerdict==ppn::Verdict::NotApplicable,"host: a Crystal Super's parallel views are what it is meant to report: decided, and nothing to say");
    }
    {
      Fixture f;auto& h=f.host;   // a rig with no names (a fixture, or a runtime that reported none)
      h.noteProjectionMode(geometry(flat,flat));
      check(h.projectionModeNoted&&h.projectionVerdict==ppn::Verdict::NotApplicable,"host: no names, no verdict");
    }
  }
  // ---- the wiring, as source pins ------------------------------------------------------------------------------------------------------------------------
  {
    const std::string host=readSource("src\\openxr\\native_runtime_host.h");
    const std::string menu=readSource("src\\d3d11\\menu.cpp");
    const std::string flat=readSource("src\\d3d11\\flat_runtime.cpp");
    const std::string header=readSource("src\\common\\parallel_projection_notice.h");
    const std::string menuHeader=readSource("src\\common\\native_menu.h");
    check(!host.empty()&&!menu.empty()&&!flat.empty()&&!header.empty()&&!menuHeader.empty(),"wiring: the sources the pins read are readable from the repo root");
    if(host.empty()||menu.empty()||flat.empty()||header.empty()||menuHeader.empty())return;
    // The host decides from a LOCATED, VALID pair: only after makeGeometrySnapshot accepted it, on the loading frames and on the game's.
    check(has(host,"const bool valid=makeGeometrySnapshot(located,snapshot);\n    if(valid)noteProjectionMode(located);")&&
              has(host,"const bool locatedValid=makeGeometrySnapshot(located,temporalGeometry);\n    if(locatedValid)noteProjectionMode(located);")&&count(host,"noteProjectionMode(located)")==2,
          "wiring: the host decides from the first pair makeGeometrySnapshot accepted, on a loading frame or a game frame, never before a locate");
    check(has(host,"projectionModeNoted=false;projectionVerdict=edvr::ppn::Verdict::NotApplicable;\n    const auto menuClosed=menu.close();")&&
              count(host,"nativeTracePuts(line);")>=1&&has(host,"edvr::ppn::formatLog(line,sizeof(line),projectionVerdict,systemLabel,runtimeLabel,"),
          "wiring: a closed session forgets the decision with the labels it was made from, and the line goes to the openxr log through the one trace");
    // The menu: the world-toast placement only (the flat profile has the screen corner); one check, decided once; only Notice raises anything.
    const size_t vrBranch=menu.find("if (pol.noticePlacement == MenuNoticePlacement::WorldToast) {");
    const size_t block=menu.find("const uint64_t parallelSession = nativeMenuSessionId();");
    const size_t eyes=menu.find("const bool named = nativeMenuEyePoses(eyes) && nativeRenderLabels(&labels) && labels.valid;");
    const size_t latch=menu.find("ppn::latchTick(s.parallelProjection, parallelSession, named ? eyes : nullptr, labels.systemName, labels.runtimeName)");
    const size_t toasts=menu.find("if (s.toasts) {\n                            s.toastQueue.push_back(parallelToast);");
    check(vrBranch!=std::string::npos&&block!=std::string::npos&&eyes!=std::string::npos&&latch!=std::string::npos&&toasts!=std::string::npos&&
              vrBranch<block&&block<eyes&&eyes<latch&&latch<toasts&&count(menu,"ppn::latchTick(")==1&&count(menu,"ppn::formatToast(")==1&&
              count(menu,"ppn::decide(")==0&&count(menu,"ppn::classifyRotations(")==0&&count(menu,"nativeMenuSessionId()")==1,
          "wiring: the menu tick reads the session identity before the held poses, decides through the latch in the VR branch, and toasts only the Notice, gated on menu.toasts");
    check(has(menu,"if (parallelSession != s.parallelProjection.session || !s.parallelProjection.checked) {")&&
              has(menu,"const bool named = nativeMenuEyePoses(eyes) && nativeRenderLabels(&labels) && labels.valid;"),
          "wiring: once a session is decided the tick fetches no poses and no names; the fetch sits behind the session and checked test");
    check(has(header,"if (views == Views::Unknown) return false;")&&has(header,"latch = SessionLatch{};")&&
              has(menu,"parallel projection: the headset notice is queued as a toast")&&has(menu,"parallel projection: menu.toasts is off, so no toast"),
          "wiring: views that are not a pair of orientations wait for the next tick (the latch), a new session starts the latch again, and the toast is logged either way");
    check(count(menu,"ppn::formatStatusHint(c.hint, sizeof(c.hint));")==1&&has(menu,"if (s.parallelProjection.notice &&\n                ppn::statusHintTurn(vrss::below(rw, rh, ew, eh) || terrainCheckerboardOn(), s.tickMs, tcn::kHintAlternateMs))")&&
              !has(menu,"statusLine(c, \"Parallel projection")&&!has(menu,"statusLine(c, \"Parallel Projection"),
          "wiring: the Status page's hint says it in the page's one slot, in its turn, and no page gains a line or a note");
    check(!has(flat,"ppn::")&&!has(flat,"parallel_projection_notice")&&!has(flat,"nativeMenuEyePoses"),"wiring: the flat runtime never touches it");
    check(has(menuHeader,"bool nativeMenuEyePoses(float out[2][12]);"),"wiring: the accessor is declared once, graphics-local, in the menu's own header");
    check(has(menuHeader,"uint64_t nativeMenuSessionId();")&&has(menuHeader,"#define EDVR_NATIVE_MENU_VERSION_1 1u")&&
              has(menuHeader,"struct EdvrNativeMenuTable {\n  uint32_t size, version;"),
          "wiring: the session identity is declared graphics-local beside the accessors, and the ABI version and table layout are unchanged");
    // The header stays dependency-free (the two halves and this rig include it): no OpenXR, no Windows, no logging.
    check(!has(header,"#include <openxr")&&!has(header,"#include <windows.h>")&&!has(header,"#include \"")&&!has(header,"Log::"),"wiring: the header includes nothing of the project");
  }
}
}
