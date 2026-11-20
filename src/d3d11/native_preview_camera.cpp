#include "native_preview_camera.h"
#include "native_preview_camera_core.h"
#include "preview_headlook_core.h"   // F233: HeadlookCamera's producer, vtables and pin (exact-build constants)
#include "concourse_vr_bake_core.h"   // baseMatches: the bake's write fence (pure)
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <iterator>
#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"

namespace edvr { namespace {
// Build334925: the manager's synchronous priority-group dispatch joins camera
// producers before each stage finalizer. HMD update stores its exact service
// pose at state+C0. CustomisationVisual's task copies its selected preview
// pose unless mode==2. The strict first-maximum finalizer otherwise selects
// the earlier HMD activity when both weights are one, discarding the preview.
// These ordinary entry hooks observe producer completion; only finalization
// changes the primary XYZ rows. No getter, task, or projection API is called.
constexpr uint32_t kManagerRva=0x106D8E0,kHmdRva=0x1074140,kPreviewRva=0x1261680,kFinalizerRva=0x1070980,kEyeRva=0x283FA20;
constexpr uint32_t kHooks[]={kHmdRva,kPreviewRva,kFinalizerRva,kManagerRva,kEyeRva};
constexpr uint8_t kHmdPin[]={0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x55,0x57,0x41,0x56};
constexpr uint8_t kPreviewPin[]={0x48,0x89,0x5C,0x24,0x18,0x55,0x48,0x83,0xEC,0x20};
constexpr uint8_t kFinalizerPin[]={0x48,0x8B,0xC4,0x55,0x53,0x57,0x41,0x54,0x48,0x8D,0xA8,0xC8,0xFE,0xFF,0xFF,0x48,0x81,0xEC,0x18,0x02,0x00,0x00};
constexpr uint8_t kManagerPin[]={0x48,0x83,0xEC,0x48,0x48,0x89,0x5C,0x24,0x58};
constexpr uint8_t kEyePin[]={0x48,0x8B,0xC4,0x48,0x89,0x58,0x10};
const uint8_t* const kPins[]={kHmdPin,kPreviewPin,kFinalizerPin,kManagerPin,kEyePin};
constexpr size_t kLengths[]={sizeof(kHmdPin),sizeof(kPreviewPin),sizeof(kFinalizerPin),sizeof(kManagerPin),sizeof(kEyePin)};
using One=uintptr_t (*)(uintptr_t);
using Two=uintptr_t (*)(uintptr_t,uintptr_t);
previewcam::Engine g_previewEngine;
previewcam::Game g_previewGame;
CodeHook g_previewHooks[5];
std::atomic<void*> g_previewOriginal[5]{};
uint8_t* g_previewRelays[5]{};
void* g_eyeTrampolineSlot=nullptr;
bool g_previewAttempted=false,g_previewInstalled=false,g_previewLastActive=false;
previewcam::DiagCap g_diagCap,g_finalCap,g_eyeCap;   // F232/F233: the frame thread's
uint64_t g_diagAt=0;
previewcam::FinalSnap g_lastFinal;bool g_haveFinal=false;   // F233: the newest finalization snapshot, the eye line's distances are to it
CodeHook g_headlookHook;                      // F233: HeadlookCamera's producer (optional: a stand-down loses only the weapon screens)
std::atomic<void*> g_headlookOriginal{nullptr};
uint8_t* g_headlookRelay=nullptr;
bool g_headlookArmed=false;
uint64_t g_previewReportAt=0,g_previewLastApplied=0;
#if defined(EDVR_PREVIEW_TEST_READ_PROBE)
void (*g_previewReadProbe)(uintptr_t,size_t) noexcept=nullptr;
#endif

bool previewRead(void*,uintptr_t at,void* out,size_t n) noexcept {
    __try {std::memcpy(out,reinterpret_cast<const void*>(at),n);}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
#if defined(EDVR_PREVIEW_TEST_READ_PROBE)
    if(g_previewReadProbe)g_previewReadProbe(at,n);
#endif
    return true;
}
bool previewWrite(void*,uintptr_t at,const void* in,size_t n) noexcept {
    if(n!=sizeof(previewcam::Pose))return false;
    __try {
        for(unsigned row=0;row<4;++row)
            std::memcpy(reinterpret_cast<void*>(at+row*16),static_cast<const uint8_t*>(in)+row*16,12);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
const previewcam::Memory g_previewMemory{previewRead,previewWrite,nullptr};
uintptr_t previewManagerHook(uintptr_t object) {
    const auto original=reinterpret_cast<One>(g_previewOriginal[3].load(std::memory_order_acquire));
    return previewcam::manager(g_previewEngine,[&]{return original(object);});
}
uintptr_t previewHmdHook(uintptr_t object) {
    const auto original=reinterpret_cast<One>(g_previewOriginal[0].load(std::memory_order_acquire));
    return previewcam::hmd(g_previewEngine,g_previewMemory,g_previewGame,object,[&]{return original(object);});
}
uintptr_t previewHeadlookHook(uintptr_t object) {
    const auto original=reinterpret_cast<One>(g_headlookOriginal.load(std::memory_order_acquire));
    return previewcam::hmd(g_previewEngine,g_previewMemory,g_previewGame,object,[&]{return original(object);},true);
}
uintptr_t previewSourceHook(uintptr_t context,uintptr_t subscriber) {
    const auto original=reinterpret_cast<Two>(g_previewOriginal[1].load(std::memory_order_acquire));
    return previewcam::preview(g_previewEngine,g_previewMemory,g_previewGame,subscriber,[&]{return original(context,subscriber);});
}
uintptr_t previewFinalizerHook(uintptr_t child) {
    const auto original=reinterpret_cast<One>(g_previewOriginal[2].load(std::memory_order_acquire));
    return previewcam::finalizer(g_previewEngine,g_previewMemory,g_previewGame,child,[&]{return original(child);});
}
bool previewPrepare(void* trampoline,void* context) noexcept {
    static_cast<std::atomic<void*>*>(context)->store(trampoline,std::memory_order_release);return true;
}
uint8_t* previewRelay(uintptr_t target,void* callback) noexcept {
    // Game entries and this DLL can be farther apart than rel32. Keep the
    // atomic five-byte entry patch; an adjacent, immutable indirect jump
    // reaches the ordinary callback without changing registers or the stack.
    constexpr uintptr_t granularity=64*1024,span=0x7FFE0000;
    const uintptr_t aligned=target&~(granularity-1);
    uint8_t* relay=nullptr;
    for(uintptr_t distance=granularity;distance<=span&&!relay;distance+=granularity) {
        const uintptr_t candidates[]={aligned>=distance?aligned-distance:0,
            aligned<=UINTPTR_MAX-distance?aligned+distance:0};
        for(auto candidate:candidates)if(candidate) {
            relay=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate),4096,
                MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
            if(relay)break;
        }
    }
    if(!relay)return nullptr;
    const uint8_t jump[]={0xFF,0x25,0,0,0,0};
    const uintptr_t destination=reinterpret_cast<uintptr_t>(callback);
    std::memcpy(relay,jump,sizeof(jump));std::memcpy(relay+6,&destination,sizeof(destination));
    DWORD oldProtect=0;
    if(!VirtualProtect(relay,4096,PAGE_EXECUTE_READ,&oldProtect)||
       !FlushInstructionCache(GetCurrentProcess(),relay,14)) {
        VirtualFree(relay,0,MEM_RELEASE);return nullptr;
    }
    return relay;
}
// TEMPORARY (the concourse cull's VR proving build, docs\concourse-cull-2026-11-05.md): a second, separate client of the eye stub. The ship preview above is untouched: with the
// probe idle (the default) eyeApply reads one atomic and goes on as before. While the probe is armed, a window of frames each lets the FIRST call of 0x283FA20 in that
// window raise the head pose's translation by g_probe.offset (visible on purpose); every other call is only counted and described. A "window" is the span between two
// present-thread ticks (nativeCameraProbeTick), so a frame's calls are the ones the game's threads made since the last tick.
// The first-call rule: the call that takes index 0 of the window's atomic counter, with a readable, finite pose that is not the identity. Flight 50 (7bc497b5): every frame's
// first call carried the head pose (rows rotated, the translation a few metres) at an address on 0x2845730's stack, which is its working copy of W+0x3330 (the RE pass of
// 2026-11-07), and the next call an identity pose on the heap. The first build refused a stack address and so wrote nothing. The ship preview's own guard is different
// (eyeCommit refuses a pose more than 0.5 m from the published head), which the probe cannot use, having no published head. Each of the window's first kProbeRecords calls is logged.
constexpr unsigned kProbeRecords = 6;   // Flight 50: 6 calls a frame
struct ProbeRecord {
    std::atomic<uint32_t> ready{0};
    uintptr_t at=0;
    bool stack=false,readable=false,wrote=false;
    float before[12]{},after[12]{};
};
struct Probe {
    std::atomic<bool> armed{false};      // the stub's one test while the probe is idle
    std::atomic<bool> writing{false};    // this window may write its first call
    std::atomic<uint32_t> calls{0};      // calls since the last tick (the index of the next call)
    std::atomic<uint32_t> stackCalls{0};
    ProbeRecord rec[kProbeRecords];      // the window's first calls, in order
    float offset[3]{};
    uint32_t frames=0,left=0,wroteWindows=0,startFrame=0,lastWindowFrame=0;
    std::atomic<uint32_t> wroteMask{0};  // bit i: the window at startFrame+i wrote
    uint64_t totalCalls=0;
} g_probe;
void probeFloats(const float* v,char (&out)[160]) noexcept {
    std::snprintf(out,sizeof(out),"x(%.3f %.3f %.3f) y(%.3f %.3f %.3f) z(%.3f %.3f %.3f) t(%.3f %.3f %.3f)",
        double(v[0]),double(v[1]),double(v[2]),double(v[3]),double(v[4]),double(v[5]),
        double(v[6]),double(v[7]),double(v[8]),double(v[9]),double(v[10]),double(v[11]));
}
// The pose's twelve floats (three per row, four rows; the lanes the game does not interpret are left out). False when the memory cannot be read.
bool probeReadPose(uintptr_t at,float (&out)[12]) noexcept {
    previewcam::Pose p;
    if(!previewcam::get(g_previewMemory,at,p))return false;
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c)out[r*3+c]=p.v[r*4+c];
    return true;
}
// The identity pose (Flight 50's second call each frame): not a head, so never written.
bool probeIdentity(const previewcam::Pose& p) noexcept {
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c){
        const float want=(r<3&&r==c)?1.0f:0.0f;
        const float d=p.v[r*4+c]-want;
        if(d>1e-4f||d<-1e-4f)return false;
    }
    return true;
}
// Called on the game's threads from eyeApply, after the null test. Wait-free: no log, no allocation. True when it wrote.
__declspec(noinline) bool probeEye(uintptr_t pose) noexcept {
    const uint32_t n=g_probe.calls.fetch_add(1,std::memory_order_acq_rel);
    const auto* tib=reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const bool stack=pose>=reinterpret_cast<uintptr_t>(tib->StackLimit)&&pose<reinterpret_cast<uintptr_t>(tib->StackBase);
    if(stack)g_probe.stackCalls.fetch_add(1,std::memory_order_relaxed);
    ProbeRecord* r=n<kProbeRecords?&g_probe.rec[n]:nullptr;
    if(r){r->at=pose;r->stack=stack;r->wrote=false;r->readable=probeReadPose(pose,r->before);for(float& f:r->after)f=0.0f;}
    bool wrote=false;
    if(n==0&&r&&r->readable&&g_probe.writing.load(std::memory_order_acquire)) {
        previewcam::Pose p;
        if(previewcam::get(g_previewMemory,pose,p)&&previewcam::finite(p)&&!probeIdentity(p)) {
            for(unsigned c=0;c<3;++c)p.v[12+c]+=g_probe.offset[c];
            wrote=g_previewMemory.write(g_previewMemory.context,pose,&p,sizeof(p));
        }
    }
    if(r){
        r->wrote=wrote;
        if(!probeReadPose(pose,r->after))for(float& f:r->after)f=0.0f;
        r->ready.store(1,std::memory_order_release);
    }
    return wrote;
}
// TEMPORARY (the concourse cull's VR bake, docs\concourse-cull-2026-11-05.md "Flight 51 results"): a third client of the eye stub. The stub's pose is the base B that 0x283FA20
// multiplies Elite's eye matrices with (eye_i = E_i * H * B, concourse_vr_bake_core.h), so the bake publishes the B that puts the head at the room's centre looking along a face,
// and every call of a window that carries Elite's base is written with it: a call at a stack address (0x2845730's copy of W+0x3330) whose pose is not the identity (Flight 51:
// calls 0, 2 and 4 of each frame carried the base, the heap calls 1, 3 and 5 the identity). Observing, it writes nothing and records what Elite left there. Wait-free in the
// stub: a seqlock guards the published pose, and a torn read is not written.
// Two fences keep a write to Elite's own base calls (the branch review, 2026-11-08). Match: a call is written only when its own pose, read before the write, is the base the tick
// expects (the latest window's first call, Elite's own; concourse_vr_bake_core.h baseMatches); the rest count as `foreign`. Lease: an aim is valid for one window:
// nativeCameraBakeTick (the window's close) ends it and the next aim renews it, so a tick that stalls cannot leave writing on; kBakeLeaseMs bounds even a window that never closes.
constexpr unsigned kBakeCalls = 8;
constexpr uint64_t kBakeLeaseMs = 500;
// Two windows, not one: the tick closes a window (nativeCameraBakeTick) a little before the same tick's aim renews it (nativeCameraBakeAim), and a stub call
// from another game thread in between must still be written. An aim not renewed lapses at the second window's close.
constexpr uint32_t kBakeLeaseWindows = 2;
struct BakeAim {
    std::atomic<bool> armed{false};      // the stub's one test while the bake is idle
    std::atomic<bool> writing{false};    // a pose is published: the window's base calls are written with it
    std::atomic<uint64_t> leaseUntilMs{0};   // GetTickCount64 past which the aim is no longer valid
    std::atomic<uint32_t> windowsLeft{0};    // windows the current aim may still write: an aim grants kBakeLeaseWindows, each window's close takes one
    std::atomic<uint32_t> seq{0};        // even: the pose is whole
    previewcam::Pose pose;
    previewcam::Pose expected;           // the base Elite is expected to hold in a call this aim may write (published with pose, under seq)
    bool expectedValid=false;
    std::atomic<uint32_t> foreignCalls{0};   // calls refused: their own pose is not the expected base, or none was published
    std::atomic<uint32_t> calls{0}, stackCalls{0}, wroteMask{0}, tornCalls{0};
    std::atomic<uint32_t> firstReady{0};
    bool firstStack=false,firstReadable=false;
    float firstBefore[16]{};
    float callT[kBakeCalls][3]{};        // each call's translation before (for the log)
    std::atomic<uint32_t> callStackMask{0};
} g_bakeAim;
bool bakeReadPublished(previewcam::Pose& out,previewcam::Pose& expected,bool& expectedValid) noexcept {
    const uint32_t s1=g_bakeAim.seq.load(std::memory_order_acquire);
    if(s1&1u)return false;
    std::memcpy(&out,&g_bakeAim.pose,sizeof(out));
    std::memcpy(&expected,&g_bakeAim.expected,sizeof(expected));
    expectedValid=g_bakeAim.expectedValid;
    std::atomic_thread_fence(std::memory_order_acquire);
    return g_bakeAim.seq.load(std::memory_order_relaxed)==s1;
}
// Called on the game's threads from eyeApply, after the null test. True when it wrote.
__declspec(noinline) bool bakeEye(uintptr_t pose) noexcept {
    const uint32_t n=g_bakeAim.calls.fetch_add(1,std::memory_order_acq_rel);
    const auto* tib=reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const bool stack=pose>=reinterpret_cast<uintptr_t>(tib->StackLimit)&&pose<reinterpret_cast<uintptr_t>(tib->StackBase);
    if(stack)g_bakeAim.stackCalls.fetch_add(1,std::memory_order_relaxed);
    previewcam::Pose before;
    const bool readable=previewcam::get(g_previewMemory,pose,before)&&previewcam::finite(before);
    if(n<kBakeCalls&&readable){
        for(unsigned c=0;c<3;++c)g_bakeAim.callT[n][c]=before.v[12+c];
        if(stack)g_bakeAim.callStackMask.fetch_or(1u<<n,std::memory_order_relaxed);
    }
    if(n==0){
        g_bakeAim.firstStack=stack;g_bakeAim.firstReadable=readable;
        if(readable)std::memcpy(g_bakeAim.firstBefore,before.v,sizeof(g_bakeAim.firstBefore));
        g_bakeAim.firstReady.store(1,std::memory_order_release);
    }
    if(!g_bakeAim.writing.load(std::memory_order_acquire)||!stack||!readable||probeIdentity(before))return false;
    if(GetTickCount64()>g_bakeAim.leaseUntilMs.load(std::memory_order_acquire))return false;   // the lease ran out: no aim is current
    previewcam::Pose p,expected;
    bool expectedValid=false;
    if(!bakeReadPublished(p,expected,expectedValid)||!previewcam::finite(p)){g_bakeAim.tornCalls.fetch_add(1,std::memory_order_relaxed);return false;}
    // `before` is Elite's own pose: nothing has been written into this call yet. Only the base the tick expects is overwritten.
    if(!expectedValid||!concourse_vr::baseMatches(before.v,expected.v)){g_bakeAim.foreignCalls.fetch_add(1,std::memory_order_relaxed);return false;}
    const bool wrote=g_previewMemory.write(g_previewMemory.context,pose,&p,sizeof(p));
    if(wrote&&n<32)g_bakeAim.wroteMask.fetch_or(1u<<n,std::memory_order_relaxed);
    return wrote;
}
__declspec(noinline) bool eyeCommit(uintptr_t pose,const previewcam::EyeSnapshot& snapshot) noexcept {
    previewcam::Pose current;
    if(!previewcam::get(g_previewMemory,pose,current)||!previewcam::finite(current))return false;
    const float dx=current.v[12]-snapshot.raw[0],dy=current.v[13]-snapshot.raw[1],dz=current.v[14]-snapshot.raw[2];
    if(dx*dx+dy*dy+dz*dz>0.25f)return false;
    if(!g_previewEngine.eyeSnapshotValid(snapshot))return false;
    return g_previewMemory.write(g_previewMemory.context,pose,&snapshot.composed,sizeof(snapshot.composed));
}
void eyeApply(uintptr_t pose) noexcept {
    // The builder is entered with whatever rdx the caller left. Do not read
    // that pointer unless this evaluation published a head.
    if(!pose)return;
    // TEMPORARY (the concourse VR probe): while it is armed it takes the call first; a pose it wrote is not the ship preview's to overwrite.
    if(g_probe.armed.load(std::memory_order_acquire)&&probeEye(pose))return;
    // TEMPORARY (the concourse VR bake): while it is armed it sees every call; a pose it wrote is not the ship preview's to overwrite.
    if(g_bakeAim.armed.load(std::memory_order_acquire)&&bakeEye(pose))return;
    // F233: while a watched preview screen is open the carried head translation, the published head and whether it was replaced are kept for the frame thread's line.
    const bool seen=g_previewEngine.diagWanted.load(std::memory_order_relaxed);
    previewcam::Pose carried;const bool carriedRead=seen&&previewcam::get(g_previewMemory,pose,carried);
    previewcam::EyeSnapshot snapshot;
    const bool published=g_previewEngine.loadEye(snapshot);
    const bool replaced=published&&eyeCommit(pose,snapshot);
    if(seen)g_previewEngine.eyeSeen(carriedRead?&carried:nullptr,published?&snapshot:nullptr,replaced);
}
// Entered by a jump from the patched eye-producer entry, so the game's return
// address stays on the stack and RSP is 8 mod 16. Eight pushes keep that
// residue. sub rsp,0x88 then leaves RSP 0 mod 16, which is what a call
// requires, and [rsp, rsp+0x20) is eyeApply's shadow space. Another sub 8
// would enter eyeApply at 0 mod 16; its /O2 movaps spill faults once a head
// is published. Volatile registers are restored and the original body is
// jumped to; only the pose memory rdx points at may change.
constexpr unsigned kEyeApplyImm=59,kEyeTrampImm=120,kEyeStubBytes=130;
uint8_t* eyeStub(uintptr_t target) noexcept {
    uint8_t* page=nullptr;
    constexpr uintptr_t granularity=64*1024,span=0x7FFE0000;
    const uintptr_t aligned=target&~(granularity-1);
    for(uintptr_t distance=granularity;distance<=span&&!page;distance+=granularity) {
        const uintptr_t candidates[]={aligned>=distance?aligned-distance:0,
            aligned<=UINTPTR_MAX-distance?aligned+distance:0};
        for(auto candidate:candidates)if(candidate) {
            page=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate),4096,
                MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
            if(page)break;
        }
    }
    if(!page)return nullptr;
    const uint8_t code[]={
        0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x9C,
        0x48,0x81,0xEC,0x88,0x00,0x00,0x00,
        0x0F,0x29,0x44,0x24,0x20,0x0F,0x29,0x4C,0x24,0x30,0x0F,0x29,0x54,0x24,0x40,
        0x0F,0x29,0x5C,0x24,0x50,0x0F,0x29,0x64,0x24,0x60,0x0F,0x29,0x6C,0x24,0x70,
        0x48,0x8B,0x8C,0x24,0xB0,0x00,0x00,0x00,
        0x48,0xB8,0,0,0,0,0,0,0,0,
        0xFF,0xD0,
        0x0F,0x28,0x44,0x24,0x20,0x0F,0x28,0x4C,0x24,0x30,0x0F,0x28,0x54,0x24,0x40,
        0x0F,0x28,0x5C,0x24,0x50,0x0F,0x28,0x64,0x24,0x60,0x0F,0x28,0x6C,0x24,0x70,
        0x48,0x81,0xC4,0x88,0x00,0x00,0x00,
        0x9D,0x41,0x5B,0x41,0x5A,0x41,0x59,0x41,0x58,0x5A,0x59,0x58,
        0x48,0xB8,0,0,0,0,0,0,0,0,
        0xFF,0xE0};
    static_assert(sizeof(code)==kEyeStubBytes,"eye stub size");
    std::memcpy(page,code,sizeof(code));
    const auto apply=reinterpret_cast<uintptr_t>(&eyeApply);
    std::memcpy(page+kEyeApplyImm,&apply,sizeof(apply));
    g_eyeTrampolineSlot=page+kEyeTrampImm;
    return page;
}
bool eyePrepare(void* trampoline,void*) noexcept {
    if(!g_eyeTrampolineSlot||!trampoline)return false;
    std::memcpy(g_eyeTrampolineSlot,&trampoline,sizeof(trampoline));
    DWORD oldProtect=0;
    auto* page=static_cast<uint8_t*>(g_eyeTrampolineSlot)-kEyeTrampImm;
    if(!VirtualProtect(page,4096,PAGE_EXECUTE_READ,&oldProtect)||
       !FlushInstructionCache(GetCurrentProcess(),page,kEyeStubBytes)) return false;
    g_previewOriginal[4].store(trampoline,std::memory_order_release);
    return true;
}
// F233: HeadlookCamera's producer, hooked like the HMD one (the pin is checked first, a relay within two gigabytes) and routed to hmd() as the head role. Optional: any refusal says so in one line and
// leaves the other five hooks as they are; without it a HeadlookCamera entry stays a foreign entry. The two vtables are published to the roster check only when the hook is armed.
void installHeadlook(uintptr_t base) {
    namespace hl=previewcam::headlook;
    uint8_t bytes[32]{};
    const uint32_t rvas[]={hl::kProducerRva,hl::kIfaceRva,hl::kObjectRva};
    if(!gameRvasFound(rvas,std::size(rvas),nullptr)) {
        Log::get().note("native preview camera (F233): HeadlookCamera producer hook stood down: binding not found on %s; weapon previews (purchase, loadout) are not handled",gameBuildText());return;
    }
    const uintptr_t target=base+gameRva(hl::kProducerRva);
    if(!previewRead(nullptr,target,bytes,hl::kProducerPinBytes)||!gamePinMatches(hl::kProducerRva,bytes,hl::kProducerPin,hl::kProducerPinBytes)) {
        Log::get().note("native preview camera (F233): HeadlookCamera producer hook stood down: entry pin +0x%X failed on %s; weapon previews (purchase, loadout) are not handled",hl::kProducerRva,gameBuildText());return;
    }
    g_headlookRelay=previewRelay(target,reinterpret_cast<void*>(&previewHeadlookHook));
    if(!g_headlookRelay) {
        Log::get().note("native preview camera (F233): HeadlookCamera producer hook stood down: near relay allocation/seal failed; weapon previews (purchase, loadout) are not handled");return;
    }
    if(!g_headlookHook.install(reinterpret_cast<void*>(target),g_headlookRelay,nullptr,"native-preview-headlook",previewPrepare,&g_headlookOriginal)) {
        VirtualFree(g_headlookRelay,0,MEM_RELEASE);g_headlookRelay=nullptr;g_headlookOriginal.store(nullptr,std::memory_order_release);
        Log::get().note("native preview camera (F233): HeadlookCamera producer hook stood down: install refused (%s); weapon previews (purchase, loadout) are not handled",g_headlookHook.refusal());return;
    }
    g_previewGame.headlookIface.store(base+gameRva(hl::kIfaceRva),std::memory_order_release);
    g_previewGame.headlookObject.store(base+gameRva(hl::kObjectRva),std::memory_order_release);
    g_headlookArmed=true;
    Log::get().note("native preview camera (F233): HeadlookCamera producer hook armed at +0x%X (pin %u bytes); a HeadlookCamera entry is a head entry of the Location camera list",
        hl::kProducerRva,unsigned(hl::kProducerPinBytes));
}
bool previewInstall() {
    // The resolver carries addresses across builds; it does not establish the
    // CameraState offsets or scheduler ownership contract on a new layout.
    if(!gameBuildKnown()) {
        Log::get().note("native preview camera: unavailable; CameraState/task layout is verified only on build 334925 (%s)",gameBuildText());
        return false;
    }
    const auto module=GetModuleHandleW(L"EliteDangerous64.exe");
    if(!module){Log::get().note("native preview camera: unavailable; game module not loaded");return false;}
    uint32_t missing=0;
    if(!gameRvasFound(kHooks,std::size(kHooks),&missing)||
       !gameRvasFound(previewcam::kRvas,previewcam::AddressCount,&missing)) {
        Log::get().note("native preview camera: unavailable; binding +0x%X not found on %s",missing,gameBuildText());return false;
    }
    const uintptr_t base=reinterpret_cast<uintptr_t>(module);uintptr_t targets[5]{};
    for(unsigned i=0;i<5;++i) {
        targets[i]=base+gameRva(kHooks[i]);uint8_t bytes[32]{};
        if(!previewRead(nullptr,targets[i],bytes,kLengths[i])||!gamePinMatches(kHooks[i],bytes,kPins[i],kLengths[i])) {
            Log::get().note("native preview camera: unavailable; entry pin +0x%X failed on %s",kHooks[i],gameBuildText());return false;
        }
    }
    for(unsigned i=0;i<previewcam::AddressCount;++i)g_previewGame.at[i]=base+gameRva(previewcam::kRvas[i]);
    void* replacements[]={reinterpret_cast<void*>(&previewHmdHook),reinterpret_cast<void*>(&previewSourceHook),
        reinterpret_cast<void*>(&previewFinalizerHook),reinterpret_cast<void*>(&previewManagerHook),nullptr};
    const char* names[]={"native-preview-hmd","native-preview-source","native-preview-finalizer","native-preview-manager","native-preview-eye"};
    for(unsigned i=0;i<5;++i) {
        const bool eye=i==4;
        g_previewRelays[i]=eye?eyeStub(targets[i]):previewRelay(targets[i],replacements[i]);
        if(!g_previewRelays[i]) {
            Log::get().note("native preview camera: unavailable; %s near relay allocation/seal failed; no camera writes enabled",names[i]);return false;
        }
        if(!g_previewHooks[i].install(reinterpret_cast<void*>(targets[i]),g_previewRelays[i],nullptr,names[i],
                                     eye?eyePrepare:previewPrepare,eye?nullptr:&g_previewOriginal[i])) {
            // This entry was not published. Previous successful entries and
            // their relays remain process-lifetime passive forwarders.
            VirtualFree(g_previewRelays[i],0,MEM_RELEASE);g_previewRelays[i]=nullptr;
            g_previewOriginal[i].store(nullptr,std::memory_order_release);
            // Already-installed forwarders remain passive. Never tear down a
            // trampoline while a scheduler thread may still be inside it.
            Log::get().note("native preview camera: unavailable; %s install refused; no camera writes enabled; %s",names[i],g_previewHooks[i].refusal());return false;
        }
    }
    installHeadlook(base);
    Log::get().note("native preview camera: armed on %s; five pinned callbacks, 16 child slots per synchronous evaluation; "
        "exact HMD/CustomisationVisual Location pair only, exact game head pose, primary XYZ only; overflow or observed overlap before commit refuses",gameBuildText());
    return true;
}
} // namespace

void nativePreviewCameraBoundary(bool wanted,bool active) {
    if(wanted&&!g_previewAttempted){g_previewAttempted=true;g_previewInstalled=previewInstall();}
    const bool enabled=wanted&&active&&g_previewInstalled;
    g_previewEngine.enable(enabled);
    const uint64_t now=GetTickCount64(),applied=g_previewEngine.applied.load();
    const bool firstApply=applied&&!g_previewLastApplied;
    if(g_previewAttempted&&(enabled!=g_previewLastActive||firstApply||!g_previewReportAt||now-g_previewReportAt>=10000)) {
        Log::get().note("native preview camera: active=%u installed=%u evaluations=%llu HMD-calls=%llu preview-calls=%llu finalizer-calls=%llu "
            "HMD-qualified=%llu preview-qualified=%llu applied=%llu refused=%llu; totals, zero callbacks means never ran",
            unsigned(enabled),unsigned(g_previewInstalled),g_previewEngine.evaluations.load(),g_previewEngine.hmdCalls.load(),
            g_previewEngine.previewCalls.load(),g_previewEngine.finalizerCalls.load(),g_previewEngine.hmdQualified.load(),
            g_previewEngine.previewQualified.load(),applied,g_previewEngine.refused.load());
        g_previewReportAt=now;
    }
    g_previewLastActive=enabled;g_previewLastApplied=applied;
}
void nativePreviewCameraDiagTick(bool screenOpen) {
    const bool wanted=screenOpen&&g_previewInstalled;
    g_previewEngine.diagWanted.store(wanted,std::memory_order_relaxed);
    if(!wanted)return;
    const uint64_t now=GetTickCount64();
    if(g_diagAt&&now-g_diagAt<1000)return;
    g_diagAt=now;
    const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"EliteDangerous64.exe"));
    for(unsigned role=0;role<2;++role) {
        previewcam::Refusal r;uint64_t counts[size_t(previewcam::Refuse::kRefuseCount)]{};
        if(!g_previewEngine.takeRefusal(role,r,counts)||!g_diagCap.allow(now))continue;
        char line[1100];
        previewcam::formatRefusal(line,sizeof(line),r,counts,base);
        Log::get().note("%s",line);
    }
    // F233: the finalizer's and the eye stub's newest snapshots (the eye line's distances are to the finalization's poses)
    previewcam::FinalSnap fin;
    if(g_previewEngine.takeFinal(fin)) {
        g_lastFinal=fin;g_haveFinal=true;
        if(g_finalCap.allow(now)) {char line[1100];previewcam::formatFinal(line,sizeof(line),fin);Log::get().note("%s",line);}
    }
    previewcam::EyeSeen eye;
    if(g_previewEngine.takeEyeSeen(eye)&&g_eyeCap.allow(now)) {
        char line[1100];previewcam::formatEye(line,sizeof(line),eye,g_haveFinal?&g_lastFinal:nullptr);Log::get().note("%s",line);
    }
}
void nativePreviewCameraStop() noexcept {
    g_previewEngine.diagWanted.store(false,std::memory_order_relaxed);
    g_previewEngine.enable(false);
    g_probe.writing.store(false,std::memory_order_release);
    g_probe.armed.store(false,std::memory_order_release);
    g_bakeAim.writing.store(false,std::memory_order_release);
    g_bakeAim.armed.store(false,std::memory_order_release);
}

bool nativeCameraBakeAvailable(const char** why) {
    const char* ignored=nullptr;
    if(!why)why=&ignored;
    *why=nullptr;
    if(!g_previewAttempted){g_previewAttempted=true;g_previewInstalled=previewInstall();}
    if(!g_previewInstalled){*why="the eye stub is not installed (the \"native preview camera:\" line says why)";return false;}
    return true;
}
void nativeCameraBakeObserve(bool on) noexcept {
    if(!on){g_bakeAim.writing.store(false,std::memory_order_release);g_bakeAim.armed.store(false,std::memory_order_release);return;}
    if(!g_previewInstalled)return;
    g_bakeAim.armed.store(true,std::memory_order_release);
}
void nativeCameraBakeAim(const float pose[16],const float expected[16]) noexcept {
    if(!g_previewInstalled||!pose)return;
    g_bakeAim.seq.fetch_add(1,std::memory_order_acq_rel);   // odd: a reader in the stub refuses the pose until it is whole again
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(g_bakeAim.pose.v,pose,sizeof(g_bakeAim.pose.v));
    g_bakeAim.expectedValid=expected!=nullptr;
    if(expected)std::memcpy(g_bakeAim.expected.v,expected,sizeof(g_bakeAim.expected.v));
    g_bakeAim.seq.fetch_add(1,std::memory_order_release);
    g_bakeAim.leaseUntilMs.store(GetTickCount64()+kBakeLeaseMs,std::memory_order_release);
    g_bakeAim.windowsLeft.store(kBakeLeaseWindows,std::memory_order_release);
    g_bakeAim.armed.store(true,std::memory_order_release);
    g_bakeAim.writing.store(true,std::memory_order_release);   // the lease: kBakeLeaseWindows windows, each closed by nativeCameraBakeTick
}
void nativeCameraBakeStopWriting() noexcept {g_bakeAim.writing.store(false,std::memory_order_release);}
bool nativeCameraBakeWriting() noexcept {return g_bakeAim.writing.load(std::memory_order_acquire);}
// Present thread, once a frame: the window since the last call, then a fresh one.
void nativeCameraBakeTick(NativeCameraBakeWindow* out) noexcept {
    NativeCameraBakeWindow w;
    // The window closes and takes one window of the aim's lease; at none left, writing stops until the tick aims again (nativeCameraBakeAim).
    {
        uint32_t left=g_bakeAim.windowsLeft.load(std::memory_order_acquire);
        if(left>0)--left;
        g_bakeAim.windowsLeft.store(left,std::memory_order_release);
        if(left==0)g_bakeAim.writing.store(false,std::memory_order_release);
    }
    w.armed=g_bakeAim.armed.load(std::memory_order_acquire);
    w.foreign=g_bakeAim.foreignCalls.exchange(0,std::memory_order_relaxed);
    w.calls=g_bakeAim.calls.exchange(0,std::memory_order_acq_rel);
    w.stackCalls=g_bakeAim.stackCalls.exchange(0,std::memory_order_relaxed);
    w.wroteMask=g_bakeAim.wroteMask.exchange(0,std::memory_order_relaxed);
    w.torn=g_bakeAim.tornCalls.exchange(0,std::memory_order_relaxed);
    w.stackMask=g_bakeAim.callStackMask.exchange(0,std::memory_order_relaxed);
    if(g_bakeAim.firstReady.exchange(0,std::memory_order_acq_rel)){
        w.firstSeen=true;w.firstStack=g_bakeAim.firstStack;w.firstReadable=g_bakeAim.firstReadable;
        if(w.firstReadable)std::memcpy(w.firstBefore,g_bakeAim.firstBefore,sizeof(w.firstBefore));
    }
    const unsigned n=w.calls<kBakeCalls?w.calls:kBakeCalls;
    for(unsigned k=0;k<n&&k<NativeCameraBakeWindow::kCalls;++k)for(unsigned c=0;c<3;++c)w.callT[k][c]=g_bakeAim.callT[k][c];
    if(out)*out=w;
}

bool nativeCameraProbeArm(const float offset[3],uint32_t frames,uint32_t frame,const char** why) {
    const char* ignored=nullptr;
    if(!why)why=&ignored;
    *why=nullptr;
    if(g_probe.armed.load(std::memory_order_acquire)){*why="a probe is already running";return false;}
    if(!frames||frames>16){*why="a bad frame count";return false;}
    // On demand, as nativePreviewCameraBoundary does when the preview is wanted. The five hooks stay passive forwarders until the engine is enabled; the stub only calls eyeApply.
    if(!g_previewAttempted){g_previewAttempted=true;g_previewInstalled=previewInstall();}
    if(!g_previewInstalled){*why="the eye stub is not installed (the \"native preview camera:\" line says why)";return false;}
    g_probe.offset[0]=offset[0];g_probe.offset[1]=offset[1];g_probe.offset[2]=offset[2];
    g_probe.frames=frames;g_probe.left=frames;g_probe.wroteWindows=0;g_probe.totalCalls=0;
    g_probe.startFrame=frame;g_probe.lastWindowFrame=frame;
    g_probe.wroteMask.store(0,std::memory_order_relaxed);
    g_probe.calls.store(0,std::memory_order_relaxed);
    g_probe.stackCalls.store(0,std::memory_order_relaxed);
    for(auto& r:g_probe.rec)r.ready.store(0,std::memory_order_relaxed);
    g_probe.writing.store(false,std::memory_order_release);   // the press's own window, the frame before, is not written
    g_probe.armed.store(true,std::memory_order_release);
    return true;
}
bool nativeCameraProbeRunning() noexcept {return g_probe.armed.load(std::memory_order_acquire);}
uint32_t nativeCameraProbePhase(uint32_t frame) noexcept {
    if(!g_probe.armed.load(std::memory_order_acquire)||frame<g_probe.startFrame)return 0;
    const uint32_t i=frame-g_probe.startFrame;
    if(i==0)return 1;
    if(i<=g_probe.frames)return 2;
    return i==g_probe.frames+1?3:0;
}
bool nativeCameraProbeWrote(uint32_t frame) noexcept {
    if(frame<g_probe.startFrame)return false;
    const uint32_t i=frame-g_probe.startFrame;
    return i>=1&&i<=g_probe.frames&&(g_probe.wroteMask.load(std::memory_order_acquire)>>i&1u)!=0;
}
// Present thread, once a frame: logs the window that has just ended, then opens the next.
void nativeCameraProbeTick(uint32_t frame) {
    if(!g_probe.armed.load(std::memory_order_acquire))return;
    g_probe.writing.store(false,std::memory_order_release);
    const uint32_t wf=g_probe.lastWindowFrame,idx=wf-g_probe.startFrame;
    const char* phase=idx==0?"before: the press's own frame, nothing written":idx<=g_probe.frames?"probe: the first call is written unless it is the identity":"after: nothing written";
    bool wroteAny=false;
    char b[160],a[160];
    for(unsigned k=0;k<kProbeRecords;++k){
        ProbeRecord& r=g_probe.rec[k];
        if(!r.ready.load(std::memory_order_acquire))continue;
        wroteAny=wroteAny||r.wrote;
        if(!r.readable){
            Log::get().note("concourse vr probe: camera frame %u: call %u at 0x%llX: pose unreadable",wf,k,(unsigned long long)r.at);
        } else {
            probeFloats(r.before,b);probeFloats(r.after,a);
            Log::get().note("concourse vr probe: camera frame %u: call %u at 0x%llX%s, wrote %s; pose before %s; after %s",
                wf,k,(unsigned long long)r.at,r.stack?" (a stack address)":"",r.wrote?"yes":"no",b,a);
        }
        r.ready.store(0,std::memory_order_release);
    }
    const uint32_t calls=g_probe.calls.exchange(0,std::memory_order_acq_rel);
    const uint32_t stackCalls=g_probe.stackCalls.exchange(0,std::memory_order_relaxed);
    if(wroteAny){g_probe.wroteMask.fetch_or(1u<<idx,std::memory_order_acq_rel);++g_probe.wroteWindows;}
    g_probe.totalCalls+=calls;
    Log::get().note("concourse vr probe: camera frame %u [%s]: stub calls %u (first %u, other %u, %u of them at a stack address); wrote %s",
        wf,phase,calls,calls?1u:0u,calls?calls-1u:0u,stackCalls,wroteAny?"yes":"no");
    if(idx>g_probe.frames){
        Log::get().note("concourse vr probe: camera done: %u of %u frames wrote a pose; the stub ran %llu calls in %u frames (zero calls means the stub never ran, not that nothing moved)",
            g_probe.wroteWindows,g_probe.frames,(unsigned long long)g_probe.totalCalls,g_probe.frames+2);
        g_probe.armed.store(false,std::memory_order_release);
        return;
    }
    g_probe.lastWindowFrame=frame;
    const bool w=g_probe.left>0;
    if(w)--g_probe.left;
    g_probe.writing.store(w,std::memory_order_release);
}
} // namespace edvr
