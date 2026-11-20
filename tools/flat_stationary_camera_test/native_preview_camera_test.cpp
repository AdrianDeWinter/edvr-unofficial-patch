// Run the actual four production entry wrappers against owned synthetic
// CameraState memory. No game, resolver, installer or runtime is consulted.
#include "../../src/d3d11/native_preview_camera.cpp"
#include <array>
#include <memory>
#include <thread>
#include <limits>
#include <stdexcept>
#include <cstdio>
#include <cmath>

namespace edvr {
unsigned unexpected=0;
unsigned layoutQueries=0;
Log& Log::get(){static Log l;return l;} Log::~Log()=default;
void Log::note(const char*,...){}
void CodeHook::uninstall(){}
bool CodeHook::install(void*,void*,void**,const char*,Prepare,void*){++unexpected;return false;}
uintptr_t gameRva(uint32_t) noexcept{++unexpected;return 0;}
bool gameRvasFound(const uint32_t*,size_t,uint32_t*) noexcept{++unexpected;return false;}
bool gamePinMatches(uint32_t,const uint8_t*,const uint8_t*,size_t) noexcept{++unexpected;return false;}
const char* gameBuildText() noexcept{++unexpected;return "test";}
bool gameBuildKnown() noexcept{++layoutQueries;return false;}
}
using namespace edvr::previewcam;
unsigned checks=0,failures=0;
void check(bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",name);}}
float separation(const Pose& a,const Pose& b){
    const float dx=a.v[12]-b.v[12],dy=a.v[13]-b.v[13],dz=a.v[14]-b.v[14];
    return std::sqrt(dx*dx+dy*dy+dz*dz);
}
Pose identityPose(){Pose p;p.v[0]=p.v[5]=p.v[10]=1;return p;}
struct Fixture {
    std::array<uint8_t,65536> bytes{};
    Game game;Memory memory{read,write,this};Engine engine;
    unsigned hmdN=0,previewN=0,finalN=0,writes=0,managerN=0;
    bool hmdReady=true,overlapBeforeCommit=false,overlapAfterCommit=false;
    uintptr_t denied=0;
    Pose local,previewPose,world;
    uintptr_t at(size_t off) const{return reinterpret_cast<uintptr_t>(bytes.data()+off);}
    template<class T>void put(size_t off,const T& value){std::memcpy(bytes.data()+off,&value,sizeof(value));}
    template<class T>T load(size_t off)const{T v;std::memcpy(&v,bytes.data()+off,sizeof(v));return v;}
    static bool read(void* ctx,uintptr_t at,void* out,size_t n) noexcept {
        auto& f=*static_cast<Fixture*>(ctx);const uintptr_t start=f.at(0);
        if(at==f.denied||at<start||n>f.bytes.size()||at-start>f.bytes.size()-n)return false;
        std::memcpy(out,reinterpret_cast<const void*>(at),n);
        // Deterministic adversarial admission immediately before the core's
        // commit reservation: finalizer's last output read has completed.
        if(f.overlapBeforeCommit&&at==f.at(0x30B0)) {f.engine.enter();f.engine.leave();}
        return true;
    }
    static bool write(void* ctx,uintptr_t at,const void* in,size_t n) noexcept {
        auto& f=*static_cast<Fixture*>(ctx);
        if(f.overlapAfterCommit){f.engine.enter();f.engine.leave();}
        ++f.writes;
        return edvr::previewWrite(nullptr,at,in,n);
    }
    Fixture() {
        for(unsigned i=0;i<AddressCount;++i)game.at[i]=at(0x8000+i*0x80);
        put(0x3000,game.at[Child]);put(0x3010,game.at[Child10]);put(0x3030,at(0x4000));
        put<uint8_t>(0x3038,1);put(0x3040,at(0x5000));put<uint64_t>(0x3058,2);put(0x3060,at(0x6000));
        put<uint32_t>(0x3068,0);put(0x4000,game.at[Nested]);put(0x4020,at(0x7000));put(0x4028,at(0x3000));
        put<uint64_t>(0x7000,8);std::memcpy(bytes.data()+0x7014,"Location",8);
        put(0x6000,at(0x1020));put(0x6008,at(0x2060));
        put(0x1000,game.at[HmdObject]);put(0x1020,game.at[HmdIface]);put<uint8_t>(0x101C,1);put(0x1140,at(0x3000));
        put(0x2000,game.at[PreviewObject]);put(0x2060,game.at[PreviewIface]);put(0x2180,at(0x3000));
        put<uint32_t>(0x239C,0);
        put(0x8000+HmdIface*0x80+0x20,game.at[Weight]);put(0x8000+HmdIface*0x80+0x28,game.at[State]);
        put(0x8000+PreviewIface*0x80+0x20,game.at[ConstantWeight]);put(0x8000+PreviewIface*0x80+0x28,game.at[State]);
        const Pose base=identityPose();put(0x5040,base);
        local.v[1]=1;local.v[4]=-1;local.v[10]=1;local.v[12]=1;local.v[13]=2;local.v[14]=3;
        previewPose.v[2]=-1;previewPose.v[5]=1;previewPose.v[8]=1;
        previewPose.v[12]=100;previewPose.v[13]=200;previewPose.v[14]=300;
        world=identityPose();world.v[12]=-621;world.v[13]=-27;world.v[14]=-79;
        // Padding is deliberately not homogeneous and includes a NaN payload.
        uint32_t pad=0x7FC12345;std::memcpy(&world.v[15],&pad,4);
        local.v[3]=std::numeric_limits<float>::quiet_NaN();previewPose.v[7]=local.v[3];
        put(0x24D0,previewPose);put<float>(0x1030,0.875f);put<uint32_t>(0x1040,0x12345678);
        engine.enable(true);
    }
    uintptr_t hmdOriginal(){++hmdN;put(0x1070,hmdReady?world:load<Pose>(0x5040));put(0x10F0,local);put(0x9000,load<Pose>(0x1070));return 0x11111111;}
    uintptr_t previewOriginal(){++previewN;put(0x20B0,load<uint32_t>(0x239C)==2?load<Pose>(0x5040):load<Pose>(0x24D0));return 0x22222222;}
    uintptr_t finalOriginal(){++finalN;std::memcpy(bytes.data()+0x3070,bytes.data()+0x1030,0x110);return 0x33333333;}
    void h(){check(hmd(engine,memory,game,at(0x1000),[&]{return hmdOriginal();})==0x11111111,"HMD return preserved");}
    void p(){check(preview(engine,memory,game,at(0x21B0),[&]{return previewOriginal();})==0x22222222,"preview return preserved");}
    void f(){check(finalizer(engine,memory,game,at(0x3000),[&]{return finalOriginal();})==0x33333333,"finalizer return preserved");}
    bool eyeComposed(const Pose& eye) const {
        const Pose composed=load<Pose>(0x30B0);
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c)
            if(eye.v[r*4+c]!=composed.v[r*4+c])return false;
        return true;
    }
    static bool farFrom(const Pose& a,const Pose& b) {
        const float dx=a.v[12]-b.v[12],dy=a.v[13]-b.v[13],dz=a.v[14]-b.v[14];
        return dx*dx+dy*dy+dz*dz>50.f*50.f;
    }
    bool expected()const {
        const Pose p=load<Pose>(0x30B0),primary=load<Pose>(0x1070);
        const float want[12]={0,0,-1,0,1,0,1,0,0,100,200,300};
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c) {
            if(p.v[r*4+c]!=want[r*3+c])return false;
            if(primary.v[r*4+c]!=want[r*3+c])return false;
        }
        return true;
    }
    bool untouchedOutsideXYZ()const {
        for(unsigned i=0;i<0x110;++i) {
            const bool xyz=i>=0x40&&i<0x80&&((i-0x40)%16)<12;
            if(!xyz&&bytes[0x3070+i]!=bytes[0x1030+i])return false;
        }
        return true;
    }
};
Fixture* live=nullptr;
uintptr_t actualHmd(uintptr_t x){check(x==live->at(0x1000),"actual HMD object ABI");return live->hmdOriginal();}
uintptr_t actualPreview(uintptr_t c,uintptr_t s){check(c==0xABCD&&s==live->at(0x21B0),"actual callback context/subscriber ABI");return live->previewOriginal();}
uintptr_t actualFinal(uintptr_t c){check(c==live->at(0x3000),"actual child ABI");return live->finalOriginal();}
uintptr_t actualManager(uintptr_t object) {
    check(object==0xDCBA,"actual manager ABI");++live->managerN;
    edvr::previewSourceHook(0xABCD,live->at(0x21B0));edvr::previewHmdHook(live->at(0x1000));edvr::previewFinalizerHook(live->at(0x3000));
    return 0x44444444;
}
static int testMain(int argc,char** argv) {
    if(argc!=2||std::strcmp(argv[1],"--self-test"))return 2;
    for(unsigned order=0;order<2;++order) {
        Fixture a;const auto result=manager(a.engine,[&]{if(order){a.p();a.h();}else{a.h();a.p();}a.f();return uintptr_t(99);});
        check(result==99&&a.hmdN==1&&a.previewN==1&&a.finalN==1,"both producer orders forward each original once");
        check(a.writes==2&&a.expected(),"preview pose without the local headset is the published eye");
        const Pose rawEye=a.load<Pose>(0x9000);
        check(applyEyeHead(a.engine,a.memory,a.at(0x9000)),"published head replaces the stale HMD copy the eyes multiply");
        const Pose eye=a.load<Pose>(0x9000);
        check(a.eyeComposed(eye)&&a.farFrom(rawEye,eye),"scene eye head is the preview pose, far from the raw HMD");
        if(order==0){
            std::printf("stable preview eye (%.6f, %.6f, %.6f) raw HMD (%.6f, %.6f, %.6f) distance %.3f m\n",
                eye.v[12],eye.v[13],eye.v[14],rawEye.v[12],rawEye.v[13],rawEye.v[14],separation(eye,rawEye));
            std::printf("original-once: hmd %u preview %u finalizer %u\n",a.hmdN,a.previewN,a.finalN);
            Pose menu{};menu.v[0]=menu.v[5]=menu.v[10]=1;menu.v[12]=0.012f;menu.v[13]=0.002f;menu.v[14]=0.005f;
            a.put(0x9200,menu);
            check(applyEyeHead(a.engine,a.memory,a.at(0x9200))&&equal(a.load<Pose>(0x9200),eye),
                "head-locked menu camera near the origin takes the preview pose");
            std::printf("menu camera: origin replaced with preview (%.6f, %.6f, %.6f)\n",
                a.load<Pose>(0x9200).v[12],a.load<Pose>(0x9200).v[13],a.load<Pose>(0x9200).v[14]);
        }
        check(a.untouchedOutsideXYZ(),"all padding FOV secondary and non-primary state bytes unchanged");
        check(!a.engine.current(),"manager generation closes after dispatch");
        Pose elsewhere=rawEye;elsewhere.v[12]+=80.f;a.put(0x9100,elsewhere);
        check(!applyEyeHead(a.engine,a.memory,a.at(0x9100))&&equal(a.load<Pose>(0x9100),elsewhere),
            "a head tens of metres from the HMD primary is not the eye copy");
    }
    {
        Fixture moved;moved.local.v[12]=9.f;
        manager(moved.engine,[&]{moved.h();moved.p();moved.f();return uintptr_t(0);});
        check(applyEyeHead(moved.engine,moved.memory,moved.at(0x9000)),"a different local headset pose still publishes");
        Fixture still;
        manager(still.engine,[&]{still.h();still.p();still.f();return uintptr_t(0);});
        check(applyEyeHead(still.engine,still.memory,still.at(0x9000)),"the original local headset pose publishes too");
        const Pose movedEye=moved.load<Pose>(0x9000),stillEye=still.load<Pose>(0x9000);
        check(equal(movedEye,stillEye)&&movedEye.v[12]==100.f&&movedEye.v[13]==200.f&&movedEye.v[14]==300.f,
            "local headset motion leaves the published eye on the preview pose");
        std::printf("local headset motion: published eye (%.6f, %.6f, %.6f) vs still (%.6f, %.6f, %.6f)\n",
            movedEye.v[12],movedEye.v[13],movedEye.v[14],stillEye.v[12],stillEye.v[13],stillEye.v[14]);
    }
    {
        // The 10:19 flight wrote the composed pose to the Location stage only.
        // That store is child+0xB0. It does not publish a head, so the eye
        // record copied from the HMD stays there and the eye replacement refuses.
        Fixture only;only.hmdOriginal();
        const Pose raw=only.load<Pose>(0x9000);
        const Pose changed=compose(only.local,only.previewPose,only.load<Pose>(0x30B0));
        check(only.memory.write(only.memory.context,only.at(0x30B0),&changed,sizeof(changed)),
            "Location stage output is the only pose store");
        const Pose stage=only.load<Pose>(0x30B0);
        const bool refused=!applyEyeHead(only.engine,only.memory,only.at(0x9000));
        const Pose left=only.load<Pose>(0x9000);
        check(only.writes==1&&only.farFrom(left,stage)&&refused&&equal(left,raw)&&stage.v[12]==changed.v[12],
            "changing only the Location stage output leaves the eye-origin record on the HMD");
        std::printf("Location-only miss: stage (%.6f, %.6f, %.6f) eye-origin left on HMD (%.6f, %.6f, %.6f) distance %.3f m refused %d\n",
            stage.v[12],stage.v[13],stage.v[14],left.v[12],left.v[13],left.v[14],separation(stage,left),refused?1:0);
    }
    {
        Fixture a;a.hmdOriginal();
        const Pose rawEye=a.load<Pose>(0x9000);
        const Pose composed=compose(a.local,a.previewPose,Pose{});
        uint8_t* body=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        check(body!=nullptr,"eye original body allocated");
        if(body){
            body[0]=0xB8;const uint32_t marker=0x51;std::memcpy(body+1,&marker,4);body[5]=0xC3;
            DWORD old=0;VirtualProtect(body,4096,PAGE_EXECUTE_READ,&old);
            FlushInstructionCache(GetCurrentProcess(),body,4096);
        }
        uint8_t* stub=body?edvr::eyeStub(reinterpret_cast<uintptr_t>(body)):nullptr;
        const bool sealed=stub&&edvr::eyePrepare(body,nullptr);
        check(sealed,"shipped eye stub sealed with its trampoline");
        uintptr_t ran=0;
        if(sealed){
            edvr::g_previewEngine.enable(true);
            edvr::g_previewEngine.enter();
            edvr::g_previewEngine.publishEye(edvr::g_previewEngine.current(),composed,rawEye);
            edvr::g_previewEngine.leave();
            using EyeFn=uintptr_t(__fastcall*)(uintptr_t,uintptr_t);
            ran=reinterpret_cast<EyeFn>(stub)(0,a.at(0x9000));
        }
        const Pose eye=a.load<Pose>(0x9000);
        check(ran==0x51&&equal(eye,composed)&&a.farFrom(rawEye,eye),
            "shipped eye stub stores the composed pose and the original body still runs");
        std::printf("eye stub: composed (%.6f, %.6f, %.6f) raw HMD (%.6f, %.6f, %.6f) distance %.3f m original %llu\n",
            eye.v[12],eye.v[13],eye.v[14],rawEye.v[12],rawEye.v[13],rawEye.v[14],separation(eye,rawEye),
            static_cast<unsigned long long>(ran));
        edvr::g_previewEngine.enter();edvr::g_previewEngine.leave();
        edvr::g_previewOriginal[4].store(nullptr,std::memory_order_release);
        if(stub)VirtualFree(stub,0,MEM_RELEASE);
        if(body)VirtualFree(body,0,MEM_RELEASE);
    }
    {
        Fixture a;manager(a.engine,[&]{std::thread h([&]{hmd(a.engine,a.memory,a.game,a.at(0x1000),[&]{return a.hmdOriginal();});});
            std::thread p([&]{preview(a.engine,a.memory,a.game,a.at(0x21B0),[&]{return a.previewOriginal();});});h.join();p.join();a.f();return uintptr_t(0);});
        check(a.expected()&&a.writes==2,"parallel producer publications join without task serialization");
    }
    unsigned refusalUnchanged=0;
    for(unsigned mode=0;mode<19;++mode) {
        Fixture a;
        manager(a.engine,[&]{
            if(mode==2)a.hmdReady=false;
            if(mode==3)a.put<uint32_t>(0x239C,2);
            if(mode==4)a.local.v[5]=std::numeric_limits<float>::quiet_NaN();
            if(mode==5){auto p=a.previewPose;p.v[12]=std::numeric_limits<float>::infinity();a.put(0x24D0,p);}
            if(mode==6)a.put(0x6008,a.at(0x1020));
            if(mode==7)a.put<uint8_t>(0x3038,0);
            if(mode==8)a.put(0x4030,a.at(0x3000));
            if(mode==9)a.denied=a.at(0x1140);
            if(mode==15)a.world.v[12]=std::numeric_limits<float>::quiet_NaN();
            if(mode!=0)a.h();if(mode!=1)a.p();
            if(mode==10){a.put<uint32_t>(0x3068,1);a.h();a.put<uint32_t>(0x3068,0);}
            if(mode==11){a.put<uint32_t>(0x3068,1);a.p();a.put<uint32_t>(0x3068,0);}
            if(mode==12){a.engine.enable(false);a.engine.enable(true);}
            if(mode==13)a.overlapBeforeCommit=true;
            if(mode==14){a.engine.enter();a.engine.leave();}
            if(mode==16){a.put(0x1140,a.at(0x3200));a.h();a.put(0x1140,a.at(0x3000));}
            if(mode==17){a.denied=a.at(0x2180);a.p();a.denied=0;}
            if(mode==18)a.engine.enable(false);
            a.f();return uintptr_t(0);
        });
        const Pose eyeBefore=a.load<Pose>(0x9000);
        check(a.writes==0&&equal(a.load<Pose>(0x30B0),a.load<Pose>(0x1070)),"skipped/failed/invalid/stale/overlapped evaluation leaves original pose");
        const bool refused=!applyEyeHead(a.engine,a.memory,a.at(0x9000));
        const Pose eyeAfter=a.load<Pose>(0x9000);
        check(refused&&equal(eyeAfter,eyeBefore),"refusal leaves the eye-origin record unchanged");
        if(refused&&equal(eyeAfter,eyeBefore)&&a.writes==0)++refusalUnchanged;
    }
    std::printf("refusal: %u modes left the eye-origin record unchanged\n",refusalUnchanged);
    {
        Fixture a;manager(a.engine,[&]{a.p();return uintptr_t(0);});
        manager(a.engine,[&]{a.h();a.f();return uintptr_t(0);});
        check(!a.writes,"old unconsumed preview cannot pair with next evaluation HMD");
        manager(a.engine,[&]{a.h();a.p();a.f();a.f();return uintptr_t(0);});
        check(a.writes==2,"duplicate finalizer consumes a completion pair only once");
        manager(a.engine,[&]{a.h();a.p();a.put<uint32_t>(0x3068,1);a.f();a.put<uint32_t>(0x3068,0);a.f();return uintptr_t(0);});
        check(a.writes==2,"unmatched finalizer consumes completion before roster re-entry");
    }
    {
        Fixture a;manager(a.engine,[&]{a.h();a.p();std::thread other([&]{manager(a.engine,[]{return uintptr_t(0);});});other.join();a.f();return uintptr_t(0);});
        check(!a.writes,"concurrent manager poisons active evaluation");
        manager(a.engine,[&]{a.h();a.p();a.f();return uintptr_t(0);});
        check(a.writes==2,"new exclusive evaluation recovers after overlap");
    }
    {
        Fixture a;try{manager(a.engine,[&]()->uintptr_t{a.h();throw std::runtime_error("fixture");});}catch(const std::runtime_error&){}
        check(!a.engine.current(),"manager RAII closes generation on exception");
        manager(a.engine,[&]{a.h();a.p();a.f();return uintptr_t(0);});check(a.writes==2,"exception cannot leak old completion into recovery");
    }
    {
        Fixture a;Engine::Completion late;
        manager(a.engine,[&]{late=a.engine.begin(a.at(0x3000),0,a.at(0x1000));a.f();return uintptr_t(0);});
        manager(a.engine,[&]{a.engine.finish(late,nullptr);a.h();a.p();a.f();return uintptr_t(0);});
        check(a.writes==2,"unfinished producer refuses; old completion cannot damage a new evaluation");
    }
    {
        Fixture a;a.overlapAfterCommit=true;
        manager(a.engine,[&]{a.h();a.p();a.f();check(!a.engine.current(),"manager admission after reservation poisons subsequent work");return uintptr_t(0);});
        check(a.writes==2&&a.expected(),"reservation commits owned finalizer write before later overlap; not an atomic-pose claim");
    }
    {
        Engine e;e.enable(true);Evaluation scope(e);
        for(uintptr_t i=1;i<=16;++i)check(e.begin(i,0).epoch!=0,"bounded child slot admitted");
        check(!e.begin(17,0).epoch&&!e.current(),"slot overflow refuses whole evaluation without eviction");
    }
    {
        Engine e;e.enable(true);Evaluation scope(e);Engine::Completion other;
        const auto first=e.beginWithClaim(1,0,101,[&]{
            e.invalidateProducer(202,0);other=e.begin(2,0,202);
        });
        check(first.epoch&&other.epoch&&e.current(),"different child producer between owner claim and phase publication does not poison");
        e.finish(first,nullptr);e.finish(other,nullptr);
        check(e.current()!=0,"parallel independent owner claims complete normally");
    }
    {
        Engine e;e.enable(true);Evaluation scope(e);
        e.beginWithClaim(1,0,101,[&]{e.invalidateProducer(101,0);});
        check(!e.current(),"same-owner duplicate is detected even before phase publication");
    }
    {
        Engine e;e.enable(true);Evaluation scope(e);Sample h,p;
        e.beginWithClaim(1,0,101,[&]{check(!e.take(e.current(),1,h,p),"finalizer refuses partially published owner claim");});
        check(!e.current(),"partial owner publication poisons an out-of-order finalizer");
    }
    {
        Engine e;e.enable(true);Pose raw=identityPose(),preview=raw,loaded;float origin[3]{};
        preview.v[12]=100.f;
        e.enter();const uint32_t first=e.current();e.publishEye(first,preview,raw);e.leave();
        e.enable(true);e.enable(true);
        check(e.loadEye(loaded,origin)&&equal(loaded,preview),
              "repeated active boundary preserves the completed eye publication");
        e.enter();const uint32_t next=e.current();
        e.publishEye(first,preview,raw);
        check(!e.loadEye(loaded,origin),"late eye publication from the previous evaluation cannot enter the next one");
        e.publishEye(next,preview,raw);e.leave();
        check(e.loadEye(loaded,origin)&&equal(loaded,preview),"the new evaluation can publish its own eye head");
        e.poison();
        check(!e.loadEye(loaded,origin),"an overlapped evaluation cannot supply a later eye builder");
        e.enable(false);
        check(!e.loadEye(loaded,origin),"disable invalidates the completed eye head");
        e.enable(true);
        check(!e.loadEye(loaded,origin),"re-enable does not revive the disabled eye head");
    }
    {
        Engine e;e.enable(true);Pose raw=identityPose(),preview=raw,loaded;float origin[3]{};
        preview.v[12]=100.f;
        e.enter();const uint32_t epoch=e.current();e.enable(false);e.enable(true);
        e.publishEye(epoch,preview,raw);
        check(!e.loadEye(loaded,origin),"late finalizer cannot publish through disable and re-enable");
        e.leave();
    }
    {
        Fixture a;live=&a;edvr::g_previewGame=a.game;edvr::g_previewEngine.enable(true);
        edvr::g_previewOriginal[0].store(reinterpret_cast<void*>(&actualHmd));edvr::g_previewOriginal[1].store(reinterpret_cast<void*>(&actualPreview));
        edvr::g_previewOriginal[2].store(reinterpret_cast<void*>(&actualFinal));edvr::g_previewOriginal[3].store(reinterpret_cast<void*>(&actualManager));
        check(edvr::previewManagerHook(0xDCBA)==0x44444444,"actual production manager return preserved");
        const Pose rawEye=a.load<Pose>(0x9000);
        check(applyEyeHead(edvr::g_previewEngine,a.memory,a.at(0x9000))&&a.eyeComposed(a.load<Pose>(0x9000))&&a.farFrom(rawEye,a.load<Pose>(0x9000)),
            "production finalizer publishes the eye head the builder multiplies");
        check(a.managerN==1&&a.hmdN==1&&a.previewN==1&&a.finalN==1&&a.expected()&&a.untouchedOutsideXYZ(),
            "actual four hook wrappers forward exactly once and change only XYZ");
        check(edvr::unexpected==0,"actual wrappers make no resolver runtime or installation calls");
        edvr::g_previewEngine.enter();edvr::nativePreviewCameraStop();
        check(!edvr::g_previewEngine.current(),"runtime shutdown poisons the live preview evaluation");edvr::g_previewEngine.leave();
    }
    {   // The roster check (F232, F232b, F233): the reasons, the scan, HeadlookCamera as a head entry, the finalizer accepting the preview's pose, the snapshots and their lines
        auto entry=[&](Fixture& f,size_t obj,bool hmdKind,uint8_t enabled=1)->uintptr_t {
            if(hmdKind){f.put(obj,f.game.at[HmdObject]);f.put(obj+0x20,f.game.at[HmdIface]);f.put<uint8_t>(obj+0x1C,enabled);f.put(obj+0x20+0x120,f.at(0x3000));return f.at(obj+0x20);}
            f.put(obj,f.game.at[PreviewObject]);f.put(obj+0x60,f.game.at[PreviewIface]);f.put(obj+0x60+0x120,f.at(0x3000));return f.at(obj+0x60);
        };
        // slot 0 and 1 are the first two entries; slot 2 the third. The Fixture's own roster is [HMD, preview], count 2.
        auto roster=[&](Fixture& f,unsigned count,uintptr_t slot0,uintptr_t slot1,uintptr_t slot2=0){f.put<uint64_t>(0x3058,count);f.put(0x6000,slot0);f.put(0x6008,slot1);if(slot2)f.put(0x6010,slot2);};
        auto run=[&](Fixture& f,Identity& id,Refuse& why){return identityWhy(f.memory,f.game,f.at(0x3000),id,why);};
        // the game's own order, [HMD, preview]
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;Identity id,plain;Refuse why=Refuse::kCount;
            check(run(f,id,why)&&why==Refuse::kNone&&id.hmd==f.at(0x1020)&&id.preview==f.at(0x2060)&&id.headClass==0&&identity(f.memory,f.game,f.at(0x3000),plain)&&plain==id,
                "[HMD, preview] is accepted: the head and the preview entries are array[0] and array[1], class 0, and identity() is identityWhy() with the reason dropped");
        }
        // after a handover the game leaves [preview, HMD]
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;roster(f,2,f.at(0x2060),f.at(0x1020));Identity id,again;Refuse why=Refuse::kNone;
            check(run(f,id,why)&&why==Refuse::kNone&&id.hmd==f.at(0x1020)&&id.preview==f.at(0x2060)&&run(f,again,why)&&id==again,"[preview, HMD] is accepted with the right entries, the same for the same memory");
        }
        // a roster of three: [HMD, the loadout's old preview, the new preview]: the last preview is used
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;const uintptr_t fresh=entry(f,0xA000,false);roster(f,3,f.at(0x1020),f.at(0x2060),fresh);Identity id,again;Refuse why=Refuse::kNone;
            check(run(f,id,why)&&id.hmd==f.at(0x1020)&&id.preview==fresh&&run(f,again,why)&&id==again,"[HMD, old preview, new preview] picks the last preview (slot 2), the same for the same memory");
        }
        // the refusals of the scan
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;Identity id;Refuse why=Refuse::kNone;
            const uintptr_t hmd2=entry(f,0xB000,true);roster(f,3,f.at(0x1020),f.at(0x2060),hmd2);
            check(!run(f,id,why)&&why==Refuse::kHmdEntries,"two HMD entries are refused");
            roster(f,2,f.at(0x1020),hmd2);
            check(!run(f,id,why)&&why==Refuse::kHmdEntries,"[HMD, HMD] is refused");
            roster(f,2,f.at(0x2060),f.at(0x2060));
            check(!run(f,id,why)&&why==Refuse::kHmdEntries,"[preview, preview] is refused (no head entry)");
            auto gp=std::make_unique<Fixture>();Fixture& g=*gp;g.put(0xC060,g.game.at[HmdObject]);g.put(0xC180,g.at(0x3000));roster(g,3,g.at(0x1020),g.at(0x2060),g.at(0xC060));
            check(!run(g,id,why)&&why==Refuse::kForeignEntry,"a foreign entry (an interface that is neither the HMD's nor the preview's) is refused");
            auto hp=std::make_unique<Fixture>();Fixture& h=*hp;const uintptr_t disabled=entry(h,0xB000,true,0);roster(h,3,h.at(0x1020),h.at(0x2060),disabled);
            check(!run(h,id,why)&&why==Refuse::kForeignEntry,"a disabled HMD entry is not an HMD entry: refused as foreign");
            auto kp=std::make_unique<Fixture>();Fixture& k=*kp;const uintptr_t stale=entry(k,0xA000,false);k.put(0xA060+0x120,uintptr_t(0x1234));roster(k,3,k.at(0x1020),k.at(0x2060),stale);
            check(!run(k,id,why)&&why==Refuse::kForeignEntry,"a preview entry that does not point back at the child is not a preview entry");
            auto mp=std::make_unique<Fixture>();Fixture& m7=*mp;roster(m7,7,m7.at(0x1020),m7.at(0x2060));
            check(!run(m7,id,why)&&why==Refuse::kCount,"a roster of seven is refused");
            auto np=std::make_unique<Fixture>();Fixture& n1=*np;roster(n1,1,n1.at(0x1020),n1.at(0x2060));
            check(!run(n1,id,why)&&why==Refuse::kCount,"a roster of one is refused");
            auto op=std::make_unique<Fixture>();Fixture& o=*op;roster(o,3,o.at(0x1020),o.at(0x2060),0x10);
            check(!run(o,id,why)&&why==Refuse::kEntryPointers,"an entry pointer too small to be an interface is refused");
        }
        // every check names itself
        struct Case {const char* name;Refuse want;void (*mutate)(Fixture&);};
        const Case cases[]={
            {"child vtable",Refuse::kChildVtable,[](Fixture& f){f.put<uintptr_t>(0x3000,0);}},
            {"child+0x10",Refuse::kChild10,[](Fixture& f){f.put<uintptr_t>(0x3010,0);}},
            {"stack",Refuse::kStack,[](Fixture& f){f.put<uintptr_t>(0x3030,0);}},
            {"stack type",Refuse::kStackType,[](Fixture& f){f.put<uintptr_t>(0x4000,0);}},
            {"predecessor",Refuse::kPredecessor,[](Fixture& f){f.put<uintptr_t>(0x3040,0);}},
            {"count",Refuse::kCount,[](Fixture& f){f.put<uint64_t>(0x3058,7);}},
            {"array",Refuse::kArray,[](Fixture& f){f.denied=f.at(0x3060);}},
            {"mode",Refuse::kMode,[](Fixture& f){f.put<uint32_t>(0x3068,1);}},
            {"members",Refuse::kMembers,[](Fixture& f){f.put<uintptr_t>(0x4028,0);}},
            {"registered",Refuse::kRegistered,[](Fixture& f){f.put<uint8_t>(0x3038,0);}},
            {"name",Refuse::kName,[](Fixture& f){std::memcpy(f.bytes.data()+0x7014,"Locatiom",8);}},
            {"entry pointers",Refuse::kEntryPointers,[](Fixture& f){f.put<uintptr_t>(0x6000,0x10);}},
            {"foreign entry (iface vtable)",Refuse::kForeignEntry,[](Fixture& f){f.put<uintptr_t>(0x1020,0);}},
            {"foreign entry (object vtable)",Refuse::kForeignEntry,[](Fixture& f){f.put<uintptr_t>(0x1000,0);}},
            {"foreign entry (enabled byte)",Refuse::kForeignEntry,[](Fixture& f){f.put<uint8_t>(0x101C,0);}},
            {"foreign entry (back-pointer)",Refuse::kForeignEntry,[](Fixture& f){f.put<uintptr_t>(0x1140,0);}},
            {"weights",Refuse::kWeights,[](Fixture& f){f.put<uintptr_t>(0x8000+HmdIface*0x80+0x20,0);}},
        };
        for(const Case& c:cases) {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;c.mutate(f);Identity id;Refuse why=Refuse::kNone;
            check(!run(f,id,why)&&why==c.want,c.name);
        }
        check(std::strcmp(refuseName(Refuse::kCount),"count")==0&&std::strcmp(refuseName(Refuse::kForeignEntry),"foreign entry")==0&&std::strcmp(refuseName(Refuse::kHmdEntries),"HMD entry count")==0&&
                  std::strcmp(refuseName(Refuse::kWeights),"weights")==0&&std::strcmp(refuseName(Refuse::kOtherProducer),"other producer")==0,"the reasons have their names");
        // the producer hooks count and snapshot a refused child: a foreign entry in the roster
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;f.put(0xC060,f.game.at[HmdObject]);f.put(0xC180,f.at(0x3000));roster(f,2,f.at(0x1020),f.at(0xC060));
            uint64_t counts[size_t(Refuse::kRefuseCount)]{};Refusal r;
            manager(f.engine,[&]{f.h();f.p();return uintptr_t(1);});
            check(f.engine.reasons[0][size_t(Refuse::kForeignEntry)].load()==1&&f.engine.reasons[1][size_t(Refuse::kForeignEntry)].load()==1&&f.hmdN==1&&f.previewN==1&&f.writes==0,
                "both producers' refused children are counted by reason, the originals still run once and nothing is written");
            check(!f.engine.takeRefusal(0,r,counts)&&counts[size_t(Refuse::kForeignEntry)]==1,"no snapshot is kept while the frame thread does not want one, the counts are still read");
            f.engine.diagWanted.store(true);
            manager(f.engine,[&]{f.h();f.p();return uintptr_t(1);});
            check(f.engine.takeRefusal(0,r,counts)&&r.role==0&&r.why==Refuse::kForeignEntry&&r.count==2&&r.mode==0&&r.registered==1&&r.members==1&&r.nameRead&&std::strcmp(r.name,"Location")==0&&r.entries==2&&
                      r.entry[0].ptr==f.at(0x1020)&&r.entry[0].iface==f.game.at[HmdIface]&&r.entry[0].obj20==f.game.at[HmdObject]&&r.entry[0].back==f.at(0x3000)&&r.entry[0].b4==1&&
                      r.entry[1].ptr==f.at(0xC060)&&r.entry[1].iface==f.game.at[HmdObject]&&r.entry[1].back==f.at(0x3000)&&r.entry[1].b4==0&&counts[size_t(Refuse::kForeignEntry)]==2,
                "the snapshot holds the child's count, mode, registered, stack vtable, members, name and each entry's pointer, vtables, back-pointer and byte at entry-4");
            check(!f.engine.takeRefusal(0,r,counts)&&f.engine.takeRefusal(1,r,counts)&&r.role==1&&!f.engine.takeRefusal(1,r,counts),"a snapshot is taken once, per role");
            char line[1100];formatRefusal(line,sizeof(line),r,counts,f.at(0));
            const std::string text=line;
            check(text.rfind("native preview camera (F232): role preview refused: foreign entry; roster: child 0x",0)==0&&text.find(", count 2, mode 0, registered 1, stack vtable +0x8180, members 1, name \"Location\"")!=std::string::npos&&
                      text.find("; entry 0: ptr 0x")!=std::string::npos&&text.find(" iface +0x8200 obj-0x20 +0x8280 obj-0x60 0x0 back yes b-4 01")!=std::string::npos&&
                      text.find("; entry 1: ptr 0x")!=std::string::npos&&text.find(" iface +0x8280 obj-0x20 0x0 obj-0x60 0x0 back yes b-4 00")!=std::string::npos&&text.find("; refusals by reason: foreign entry 2,")!=std::string::npos,
                "the line names the role, the reason, the roster and each entry (RVAs against the image base) and the role's per-reason counts");
            Refusal blank;blank.role=0;blank.why=Refuse::kChildVtable;uint64_t none[size_t(Refuse::kRefuseCount)]{};
            formatRefusal(line,sizeof(line),blank,none,0x1000);
            check(std::string(line)=="native preview camera (F232): role HMD refused: child vtable; roster: child 0x0, count unreadable, mode unreadable, registered unreadable, stack vtable unreadable, members unreadable, name \"unreadable\"; refusals by reason: none",
                "an unreadable roster says so, field by field");
        }
        // three entries, the earlier preview's producer is not the one the child names: counted as another producer, not refused as a roster
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;const uintptr_t fresh=entry(f,0xA000,false);roster(f,3,f.at(0x1020),f.at(0x2060),fresh);
            manager(f.engine,[&]{f.h();return uintptr_t(1);});
            check(f.engine.reasons[0][size_t(Refuse::kCount)].load()==0&&f.hmdN==1&&f.engine.hmdQualified.load()==1,"with a roster of three the HMD hook's child is not refused");
            auto gp=std::make_unique<Fixture>();Fixture& g=*gp;const uintptr_t fresh2=entry(g,0xA000,false);roster(g,3,g.at(0x1020),g.at(0x2060),fresh2);
            manager(g.engine,[&]{g.p();return uintptr_t(1);});
            check(g.engine.reasons[1][size_t(Refuse::kOtherProducer)].load()==1,"the earlier preview's producer is not the one the child names: counted as another producer");
        }
        // the cap and the game's copy
        {
            DiagCap c2;unsigned w2=0;for(unsigned i=0;i<100;++i)w2+=c2.allow(5000+i)?1u:0u;
            check(w2==40&&!c2.allow(5200)&&c2.allow(15000),"at most 40 lines in any ten seconds, a new window after ten");
            Game a,b;a.at[0]=7;a.headlookIface.store(5);a.headlookObject.store(6);b=a;Game c(a);
            check(b.at[0]==7&&b.headlookIface.load()==5&&b.headlookObject.load()==6&&c.at[0]==7&&c.headlookIface.load()==5&&Game().headlookIface.load()==0,"the game's headlook vtables copy with it and start at 0");
        }
        // wait-free: several threads refusing while the reader takes snapshots: nothing waits, nothing is lost from the counters
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;roster(f,7,f.at(0x1020),f.at(0x2060));f.engine.diagWanted.store(true);
            std::atomic<bool> go{false};std::atomic<unsigned> taken{0};
            std::thread t[3];
            for(auto& th:t)th=std::thread([&]{while(!go.load());for(unsigned i=0;i<2000;++i)f.engine.refusedChild(f.memory,f.game,f.at(0x3000),i&1,Refuse::kCount);});
            std::thread reader([&]{while(!go.load());Refusal r;uint64_t c[size_t(Refuse::kRefuseCount)]{};for(unsigned i=0;i<2000;++i)if(f.engine.takeRefusal(i&1,r,c)&&r.why==Refuse::kCount&&r.count==7)taken.fetch_add(1);});
            go.store(true);for(auto& th:t)th.join();reader.join();
            {Refusal r;uint64_t c[size_t(Refuse::kRefuseCount)]{};for(unsigned role=0;role<2;++role)if(f.engine.takeRefusal(role,r,c)&&r.why==Refuse::kCount&&r.count==7)taken.fetch_add(1);}   // what the reader missed is still there
            check(f.engine.reasons[0][size_t(Refuse::kCount)].load()+f.engine.reasons[1][size_t(Refuse::kCount)].load()==6000&&taken.load()>0,"concurrent refusals are all counted and snapshots are taken without waiting");
        }
        // ---- HeadlookCamera as the head entry
        auto headlook=[&](Fixture& f,bool previewFirst){
            const uintptr_t hi=f.at(0x8800),ho=f.at(0x8880);
            f.game.headlookIface.store(hi);f.game.headlookObject.store(ho);
            f.put(0x8820,f.game.at[Weight]);f.put(0x8828,f.game.at[State]);
            f.put(0xD000,ho);f.put(0xD020,hi);f.put<uint8_t>(0xD01C,1);f.put(0xD140,f.at(0x3000));
            f.put(0x6000,previewFirst?f.at(0x2060):f.at(0xD020));f.put(0x6008,previewFirst?f.at(0xD020):f.at(0x2060));
        };
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;headlook(f,true);Identity id;Refuse why=Refuse::kNone;
            check(run(f,id,why)&&why==Refuse::kNone&&id.hmd==f.at(0xD020)&&id.preview==f.at(0x2060)&&id.headClass==1,"[preview, Headlook] is accepted: the head entry is the Headlook one, class 1, the preview the other");
            Identity again;
            check(run(f,again,why)&&id==again,"the scan with a Headlook entry is the same for the same memory");
            Identity as=id;as.headClass=0;
            check(!(id==as),"the head class is part of the identity's equality");
            f.game.headlookIface.store(0);
            check(!run(f,id,why)&&why==Refuse::kForeignEntry,"a Headlook entry is foreign until its producer hook has published the vtables (and for good when the hook stood down)");
        }
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;headlook(f,true);Identity id;Refuse why=Refuse::kNone;
            f.put<uintptr_t>(0x8820,0);
            check(!run(f,id,why)&&why==Refuse::kWeights,"a Headlook interface whose weight is not the game's is refused");
            f.put(0x8820,f.game.at[Weight]);f.put<uintptr_t>(0x8828,0);
            check(!run(f,id,why)&&why==Refuse::kWeights,"a Headlook interface whose state is not the game's is refused");
            f.put(0x8828,f.game.at[State]);
            check(run(f,id,why),"... and accepted with both right");
        }
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;headlook(f,true);Identity id;Refuse why=Refuse::kNone;
            f.put<uint64_t>(0x3058,3);f.put(0x6000,f.at(0x1020));f.put(0x6008,f.at(0x2060));f.put(0x6010,f.at(0xD020));
            check(!run(f,id,why)&&why==Refuse::kHmdEntries,"an HMD entry and a Headlook entry are two head entries: refused");
            f.put<uint64_t>(0x3058,2);f.put(0x6008,f.at(0xD020));
            check(!run(f,id,why)&&why==Refuse::kHmdEntries,"[HMD, Headlook] is refused too");
        }
        // the flow: [preview, Headlook] with idle look; the preview wins the game's finalizer (child+0xB0 is its pose); EDVR applies
        auto flow=[&](Fixture& f,bool diag){
            f.engine.diagWanted.store(diag);
            manager(f.engine,[&]{
                hmd(f.engine,f.memory,f.game,f.at(0xD000),[&]{Pose base=f.load<Pose>(0x5040);f.put(0xD070,base);f.put(0xD0F0,f.local);return uintptr_t(0x55555555);},true);
                f.p();
                finalizer(f.engine,f.memory,f.game,f.at(0x3000),[&]{f.put(0x30B0,f.load<Pose>(0x20B0));return uintptr_t(0x33333333);});
                return uintptr_t(1);
            });
        };
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;headlook(f,true);
            f.put(0xD270,F3{{1,0,0}});f.put(0xD280,F3{{0,1,0}});f.put(0xD290,F3{{0,0,1}});f.put(0xD2A0,F3{{0.1f,0.2f,0.3f}});
            flow(f,true);
            check(f.engine.hmdQualified.load()==1&&f.engine.previewQualified.load()==1,"a Headlook entry with idle look (primary == base) qualifies as the head role");
            check(f.engine.applied.load()==1&&f.writes==2,"the preview-first frame is applied: the output equals the preview's primary");
            const Pose after=f.load<Pose>(0xD070),out=f.load<Pose>(0x30B0);
            check(after.v[12]==100&&after.v[13]==200&&after.v[14]==300&&out.v[12]==100&&out.v[14]==300,"... the Headlook entry's primary and child+0xB0 carry the preview's pose");
            FinalSnap fin;
            check(f.engine.takeFinal(fin)&&fin.why==FinalWhy::kApplied&&fin.headClass==1&&fin.out.read&&fin.out.v[0]==100&&fin.head.v[0]==0&&fin.preview.v[2]==300&&fin.base.read&&fin.extraRead&&
                      fin.extra[3][0]==0.1f&&fin.extra[3][2]==0.3f&&fin.extra[0][0]==1.f&&!f.engine.takeFinal(fin),
                "the finalization snapshot holds the output, the head's, the preview's and the base's translation and the look rotation and displacement, once");
            char line[1100];formatFinal(line,sizeof(line),fin);const std::string text=line;
            check(text.rfind("native preview camera (F233): final child 0x",0)==0&&text.find(" head class Headlook: applied; output (100.000, 200.000, 300.000), head primary (0.000, 0.000, 0.000), preview primary (100.000, 200.000, 300.000), base (0.000, 0.000, 0.000); output to head 374.166 m, output to preview 0.000 m")!=std::string::npos&&
                      text.find("; headlook R rows (1.000, 0.000, 0.000) (0.000, 1.000, 0.000) (0.000, 0.000, 1.000), d (0.100, 0.200, 0.300)")!=std::string::npos,
                "the final line names the class, applied or refused, the four translations, the distances and the look values");
            EyeSnapshot published;published.composed=f.previewPose;published.raw[0]=1;published.raw[1]=2;published.raw[2]=3;
            Pose carried=identityPose();carried.v[12]=3;carried.v[13]=4;carried.v[14]=0;
            f.engine.eyeSeen(&carried,&published,true);
            EyeSeen eye;
            check(f.engine.takeEyeSeen(eye)&&eye.replaced&&eye.published&&eye.head.v[0]==3&&eye.composed.v[0]==100&&!f.engine.takeEyeSeen(eye),"the eye snapshot holds the carried head, the published head and whether it was replaced, once");
            f.engine.eyeSeen(&carried,&published,true);f.engine.takeEyeSeen(eye);
            formatEye(line,sizeof(line),eye,&fin);const std::string eyeText=line;
            check(eyeText.rfind("native preview camera (F233): eye head (3.000, 4.000, 0.000), replaced; published eye head (100.000, 200.000, 300.000), HMD primary (1.000, 2.000, 3.000); distance to published ",0)==0&&
                      eyeText.find(", to origin 5.000 m")!=std::string::npos&&eyeText.find(", to base 5.000 m")!=std::string::npos&&eyeText.find(", to head primary 5.000 m")!=std::string::npos,
                "the eye line names the carried head, whether it was replaced, the published head and the distances in metres to the published head, output, head, preview, base and origin");
            f.engine.eyeSeen(&carried,nullptr,false);f.engine.takeEyeSeen(eye);
            formatEye(line,sizeof(line),eye,nullptr);
            check(std::string(line)=="native preview camera (F233): eye head (3.000, 4.000, 0.000), not replaced; published eye head unreadable, HMD primary unreadable; distance to published n/a m, to output n/a m, to head primary n/a m, to preview primary n/a m, to base n/a m, to origin 5.000 m",
                "an eye with no published head and no finalization says n/a for what it has not got");
        }
        // with the hook not armed the same frame does nothing: the Headlook entry is foreign
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;headlook(f,true);f.game.headlookIface.store(0);
            flow(f,false);
            check(f.engine.applied.load()==0&&f.engine.hmdQualified.load()==0&&f.writes==0,"with the Headlook hook not armed a [preview, Headlook] frame is left to the game");
        }
        // the finalizer on the standard roster: the output equal to the preview's primary, to the head's, or to neither
        auto standard=[&](int outputIs,Fixture& f,bool diag){   // 0 the head's (the game's own copy), 1 the preview's, 2 neither
            f.engine.diagWanted.store(diag);
            manager(f.engine,[&]{
                f.h();f.p();
                finalizer(f.engine,f.memory,f.game,f.at(0x3000),[&]{
                    if(outputIs==1)f.put(0x30B0,f.load<Pose>(0x20B0));
                    else if(outputIs==2){Pose other=identityPose();other.v[12]=7;f.put(0x30B0,other);}
                    else f.finalOriginal();
                    return uintptr_t(0x33333333);});
                return uintptr_t(1);
            });
        };
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;standard(1,f,true);FinalSnap fin;
            check(f.engine.applied.load()==1&&f.writes==2&&f.engine.takeFinal(fin)&&fin.why==FinalWhy::kApplied&&fin.headClass==0,"the output equal to the preview's primary is applied");
            auto hp=std::make_unique<Fixture>();Fixture& h=*hp;standard(0,h,false);
            check(h.engine.applied.load()==1&&h.expected(),"the output equal to the head's primary is applied, as it always was");
            auto kp=std::make_unique<Fixture>();Fixture& k=*kp;standard(2,k,true);
            check(k.engine.applied.load()==0&&k.engine.refused.load()==1&&k.writes==0&&k.engine.takeFinal(fin)&&fin.why==FinalWhy::kOutputMismatch,"an output that is neither is refused, and the snapshot says why");
            auto qp=std::make_unique<Fixture>();Fixture& q=*qp;q.put<uint64_t>(0x3058,7);standard(0,q,true);
            check(q.engine.applied.load()==0&&q.engine.refused.load()==0,"a child that is not eligible is neither applied nor refused, and no snapshot is kept for it");
        }
        // wait-free: a finalizer's and an eye stub's snapshots taken while the reader takes them
        {
            auto fp=std::make_unique<Fixture>();Fixture& f=*fp;headlook(f,true);f.engine.diagWanted.store(true);
            Identity id;Refuse why=Refuse::kNone;run(f,id,why);
            std::atomic<bool> go{false};std::atomic<unsigned> takenFinal{0},takenEye{0};
            std::thread writers[2];
            writers[0]=std::thread([&]{while(!go.load());for(unsigned i=0;i<2000;++i)f.engine.finalSeen(f.memory,id,FinalWhy::kApplied,nullptr,nullptr,nullptr);});
            writers[1]=std::thread([&]{while(!go.load());Pose c=identityPose();for(unsigned i=0;i<2000;++i)f.engine.eyeSeen(&c,nullptr,false);});
            std::thread reader([&]{while(!go.load());FinalSnap a;EyeSeen b;for(unsigned i=0;i<4000;++i){if(f.engine.takeFinal(a)&&a.headClass==1)takenFinal.fetch_add(1);if(f.engine.takeEyeSeen(b)&&b.head.read)takenEye.fetch_add(1);}});
            go.store(true);for(auto& w:writers)w.join();reader.join();
            {FinalSnap a;EyeSeen b;if(f.engine.takeFinal(a)&&a.headClass==1)takenFinal.fetch_add(1);if(f.engine.takeEyeSeen(b)&&b.head.read)takenEye.fetch_add(1);}   // what the reader missed is still there
            check(takenFinal.load()>0&&takenEye.load()>0,"snapshots are taken without waiting while the reader takes them, and are consistent");
        }
    }
    check(!edvr::previewInstall()&&edvr::layoutQueries==1&&edvr::unexpected==1,
        "unknown layout refuses before module lookup, resolver or hook installation");
    std::printf("native_preview_camera_test: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
// The fixtures are 64 KB each and the checks keep many alive in one frame: run them on a thread with a stack that holds them (the default 1 MB does not).
namespace { struct MainArgs {int argc;char** argv;int result;}; DWORD WINAPI mainThread(void* p){auto* a=static_cast<MainArgs*>(p);a->result=testMain(a->argc,a->argv);return 0;} }
int main(int argc,char** argv) {
    MainArgs args{argc,argv,1};
    HANDLE t=CreateThread(nullptr,32u<<20,mainThread,&args,0,nullptr);
    if(!t)return 1;
    WaitForSingleObject(t,INFINITE);CloseHandle(t);
    return args.result;
}
