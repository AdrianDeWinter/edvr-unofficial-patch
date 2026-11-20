// The concourse camera test (TEMPORARY): see concourse_camera_test.h. This file is exact-build code (hook_signatures.py AUDIT_EXEMPT): it installs only when gameBuildKnown(),
// so its RVAs are build 334925's and go through no resolver. Its relay is a copy of kinematic_eval_hook.cpp's (that file stays untouched).
#include "concourse_camera_test.h"
#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace edvr {
namespace {
using EvalFn = uintptr_t (__fastcall*)(uintptr_t,uintptr_t,uintptr_t);
constexpr size_t kRelayBytes=44,kOriginalLiteral=36;
std::mutex g_installMutex;

struct HookEntry {
    const char* name;
    uintptr_t rva;
    void* callback;
    const void* gate=nullptr;
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay=nullptr;
    std::atomic<bool> ready{false};
};

uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);
    const uintptr_t granularity=info.dwAllocationGranularity;
    const uintptr_t floor=reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling=reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance=uintptr_t(INT32_MAX)-0x10000u;
    uintptr_t at=target>distance?target-distance:floor;
    if(at<floor)at=floor;
    const uintptr_t limit=target>ceiling-distance?ceiling:target+distance;
    while(at<limit) {
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&region,sizeof(region)))break;
        const uintptr_t start=reinterpret_cast<uintptr_t>(region.BaseAddress);
        if(region.RegionSize>UINTPTR_MAX-start)break;
        const uintptr_t end=start+region.RegionSize;
        if(region.State==MEM_FREE) {
            uintptr_t candidate=at>start?at:start;
            if(candidate>UINTPTR_MAX-(granularity-1))break;
            candidate=(candidate+granularity-1)&~(granularity-1);
            if(candidate<limit && candidate<end && end-candidate>=4096) {
                auto* p=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate),4096,
                                                         MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
                if(p)return p;
            }
        }
        if(end<=at)break;at=end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code,const void* gate,void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback];
    // original: jmp [trampoline]. RAX/flags are volatile, and neither the
    // evaluator nor the job bodies consume RAX on entry. No stack adjustment
    // or nonvolatile modification occurs.
    const uint8_t body[kRelayBytes]={
        0x48,0xB8,0,0,0,0,0,0,0,0, 0x48,0x83,0x38,0,
        0x74,0x0E, 0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0,
        0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
    std::memcpy(code,body,sizeof(body));
    const uintptr_t gateAddress=reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress=reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code+2,&gateAddress,8);std::memcpy(code+22,&callbackAddress,8);
}

bool prepareRelay(void* trampoline,void* context) noexcept {
    auto& entry=*static_cast<HookEntry*>(context);
    const uintptr_t address=reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(entry.relay+kOriginalLiteral,&address,8);
    DWORD oldProtect=0;
    if(!VirtualProtect(entry.relay,4096,PAGE_EXECUTE_READ,&oldProtect) ||
       !FlushInstructionCache(GetCurrentProcess(),entry.relay,kRelayBytes))return false;
    entry.forward.store(address,std::memory_order_release);
    return true;
}
bool installOne(HookEntry& entry,uintptr_t base) noexcept {
    entry.relay=allocateRelay(base+entry.rva);
    if(!entry.relay){Log::get().note("concourse camera test: no relay page could be reserved near 0x%llX", static_cast<unsigned long long>(entry.rva));return false;}
    buildRelay(entry.relay,entry.gate,entry.callback);
    if(!entry.hook.install(reinterpret_cast<void*>(base+entry.rva),entry.relay,nullptr,entry.name,&prepareRelay,&entry)) {
        VirtualFree(entry.relay,0,MEM_RELEASE);
        entry.relay=nullptr;
        return false;
    }
    entry.ready.store(true,std::memory_order_release);
    return true;   // process-lifetime storage: an in-flight call may be executing the relay
}

// ---- TEMPORARY concourse camera test (docs\concourse-cull-2026-11-05.md "Flight 22 results"). Flight 22 showed that moving flat's camera where it is made moves the whole picture. This is
// the look test: the camera goes to the room's centre and steps through six directions. Offsets READ in analysis\decomp\verify_20261107_flat_camera_build.txt:
//   0x2874290 (H, pose record*, camera*): the single flat producer of the main camera. Integer arguments only (READ in the disassembly, and it reads nothing else on entry). One direct
//            caller, 0x281D190, call at 0x281D254, return 0x281D259. THE WRITE POINT, after it returns: the camera is a stack local of 0x281D190, which then copies it to the camera set,
//            and from there to the view node (+0x20) and the RenderView (+0x310). Moved here, every copy moves together, and the pose record, which game logic may read, is not touched.
//            The local is rebuilt each frame, so nothing is put back. Layout, READ as this function's stores: rows +0x00 +0x10 +0x20, position +0x30, +0x230 flags (left dirty by the
//            producer, so the derived rows and the view-projection rebuild on the copy; not touched here).
//   0x1EA9C0 (this, RenderView*[], count): read only, after the forward: which entry is view 0 and what its slot holds (offsets as in Flight 20, READ; the slot index is INFERRED).
// Nothing in a hook takes a lock or logs: the tick reads the snapshot. A fault in a read or write is caught.
constexpr uintptr_t kHkViews = 0x1EA9C0u, kHkProducer = 0x2874290u, kCtProducerRet = 0x281D259u;
constexpr uintptr_t kCtFlagsOff = 0x18, kCtCamOff = 0x310, kCtPosOff = 0x30, kCtCtxOff = 0xF4Bu * 8u, kCtCountOff = 0x1A940, kCtSlotsOff = 0x40, kCtSlotStride = 0x6A0,
                    kCtSlotPosOff = 0x540, kCtSlotFlagsOff = 0x688;
constexpr uint32_t kCtDropFlags = (1u << 24) | (1u << 18) | (1u << 17) | (1u << 15), kCtBit16 = 1u << 16;   // 0x1068000, 0x10000: RenderView+0x18 bits that leave the first context (READ in 0x1EA9C0)
enum : int { kIdxViews, kIdxProducer, kIdxCount };
alignas(8) std::atomic<uintptr_t> camTestGate{0};
std::atomic<uint32_t> g_ctMode{0};
std::atomic<bool> g_ctActing{false};
std::atomic<uint64_t> g_ctCalls[kIdxCount];
std::atomic<uint64_t> g_ctFaults{0}, g_ctNoView0{0}, g_ctMulti{0}, g_ctWritten{0}, g_ctRefused{0}, g_ctOtherSite{0};
std::atomic<uint32_t> g_ctRvCount{0}, g_ctMain{0}, g_ctFlags{0}, g_ctSlotIdx{0}, g_ctSlotFlags{0};
std::atomic<bool> g_ctSlotKnown{false};
std::atomic<float> g_ctRvPos[3], g_ctSlot[3], g_ctCamBefore[3], g_ctCamAfter[3], g_ctFwdBefore[3], g_ctFwdAfter[3], g_ctLookCentre[3], g_ctLookUp[3];
std::atomic<uint32_t> g_ctLookDir{0};
std::atomic<bool> g_ctAimOn{false};          // the bake: the rows are aimed outright, not the player's turned
std::atomic<float> g_ctAimFwd[3], g_ctAimUp[3], g_ctAimFov{0.0f}, g_ctAimNear{0.0f};
uintptr_t g_ctBase=0;

__declspec(noinline) uintptr_t __fastcall viewsObserved(uintptr_t,uintptr_t,uintptr_t) noexcept;
__declspec(noinline) uintptr_t __fastcall producerObserved(uintptr_t,uintptr_t,uintptr_t) noexcept;
HookEntry g_ctEntries[kIdxCount]={
    {"concourse-camera-test-views",kHkViews,reinterpret_cast<void*>(&viewsObserved),&camTestGate},
    {"concourse-camera-test-producer",kHkProducer,reinterpret_cast<void*>(&producerObserved),&camTestGate},
};
// The prologues, READ from the exe. A mismatch refuses that hook (an E9 first byte means another EDVR module already patched it).
struct Pin {int n;uint8_t b[32];};
const Pin kPins[kIdxCount]={
    {17,{0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0xB8,0x40,0xB9,0x02,0x00}},
    {26,{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0xF3,0x0F,0x10,0x0A,0x48,0x8B,0xD9,0x48,0x8B,0x49,0x68}},
};

bool ctFiniteSmall(float v) noexcept {return std::fabs(v)<1.0e9f;}   // false for NaN too

// 0x1EA9C0 after the forward: which entry is view 0 and what its slot holds. Read only.
void ctReadViews(uintptr_t self,uintptr_t views,uintptr_t count) noexcept {
    __try {
        g_ctRvCount.store(static_cast<uint32_t>(count),std::memory_order_relaxed);
        if(!views||count==0||count>64) {g_ctNoView0.fetch_add(1,std::memory_order_relaxed);return;}
        const uintptr_t* array=reinterpret_cast<const uintptr_t*>(views);
        uint32_t groupA=0,mainCount=0;
        uintptr_t chosen=0;
        int slotIdx=-1;
        uint32_t chosenFlags=0;
        for(uintptr_t i=0;i<count;++i) {
            const uintptr_t rv=array[i];
            if(!rv)continue;
            const uint32_t flags=*reinterpret_cast<const uint32_t*>(rv+kCtFlagsOff);
            if((flags&kCtDropFlags)||(flags&kCtBit16))continue;   // not in the first context
            if(flags&3u) {
                if(++mainCount==1) {chosen=rv;slotIdx=static_cast<int>(groupA);chosenFlags=flags;}
            }
            ++groupA;
        }
        g_ctMain.store(mainCount,std::memory_order_relaxed);
        if(mainCount!=1||!chosen) {(mainCount==0?g_ctNoView0:g_ctMulti).fetch_add(1,std::memory_order_relaxed);return;}
        g_ctFlags.store(chosenFlags,std::memory_order_relaxed);
        const float* pos=reinterpret_cast<const float*>(chosen+kCtCamOff+kCtPosOff);
        for(int i=0;i<3;++i)g_ctRvPos[i].store(pos[i],std::memory_order_relaxed);
        const uintptr_t ctx=self+kCtCtxOff;
        const uint64_t n=*reinterpret_cast<const uint64_t*>(ctx+kCtCountOff);
        if(n>0&&n<=64&&static_cast<uint64_t>(slotIdx)<n) {
            const uintptr_t slot=ctx+kCtSlotsOff+kCtSlotStride*static_cast<uintptr_t>(slotIdx);
            const float* p=reinterpret_cast<const float*>(slot+kCtSlotPosOff);
            for(int i=0;i<3;++i)g_ctSlot[i].store(p[i],std::memory_order_relaxed);
            g_ctSlotFlags.store(*reinterpret_cast<const uint32_t*>(slot+kCtSlotFlagsOff),std::memory_order_relaxed);
            g_ctSlotIdx.store(static_cast<uint32_t>(slotIdx),std::memory_order_relaxed);
            g_ctSlotKnown.store(true,std::memory_order_relaxed);
        } else g_ctSlotKnown.store(false,std::memory_order_relaxed);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ctFaults.fetch_add(1,std::memory_order_relaxed);
        g_ctSlotKnown.store(false,std::memory_order_relaxed);
    }
}

// A quarter turn of v about the unit axis a (right-handed): a x v + a (a . v). A rotation, so the rows stay orthonormal and keep their handedness.
void ctTurn90(float* v,const float* a) noexcept {
    const float d=a[0]*v[0]+a[1]*v[1]+a[2]*v[2];
    const float x=a[1]*v[2]-a[2]*v[1]+a[0]*d,y=a[2]*v[0]-a[0]*v[2]+a[1]*d,z=a[0]*v[1]-a[1]*v[0]+a[2]*d;
    v[0]=x;v[1]=y;v[2]=z;
}

// 0x2874290 after it returns: the read, and in the window the write. The camera is read in log only too: the RenderView's camera against it says, with no write at all, that this is view 0's.
// The value every main camera's near plane comes from: a float in .data holding 0.025, read at 15 places and written by none (a rip-relative scan of build 334925). 0x287AAE0 stores it over
// the four view owners' cameras right after the views take their slots (0x287AEC4 and on, camera object +0x254, dirty bits 0xD), which is why Flight 27's write to the camera's own near
// never reached cb1. Raised at the producer, so it goes with the camera it was written for; each tick while the bake is not acting puts it back.
constexpr uintptr_t kCtNearValueRva=0x5E85B8Cu;
constexpr float kCtNearValue=0.025f;
std::atomic<float*> g_ctNearAt{nullptr};
std::atomic<bool> g_ctNearRaised{false};

// The value a raise replaced, remembered on the transition from not raised to raised (the producer raises it again every frame, and those calls must not remember the raised value);
// a restore puts exactly that back. kCtNearValue is only what resolve checks the address against.
std::atomic<float> g_ctNearReplaced{kCtNearValue};
std::atomic<float> g_ctNearWritten{0.0f};   // the last value a raise wrote: a read of it is not the game's value
void ctNearRaise(float nearM) noexcept {
    float* at=g_ctNearAt.load(std::memory_order_relaxed);
    if(!at)return;
    __try {
        const bool was=g_ctNearRaised.load(std::memory_order_relaxed);
        const float cur=*static_cast<volatile float*>(at);
        // A restore racing this raise may not have written its value back yet: what is read is then our own earlier write, and the value remembered stays.
        if(!was&&cur!=g_ctNearWritten.load(std::memory_order_relaxed))g_ctNearReplaced.store(cur,std::memory_order_relaxed);
        g_ctNearWritten.store(nearM,std::memory_order_relaxed);
        *static_cast<volatile float*>(at)=nearM;
        g_ctNearRaised.store(true,std::memory_order_release);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ctFaults.fetch_add(1,std::memory_order_relaxed);
    }
}
void ctNearRestore() noexcept {
    if(!g_ctNearRaised.exchange(false,std::memory_order_acq_rel))return;
    float* at=g_ctNearAt.load(std::memory_order_relaxed);
    if(!at)return;
    __try {
        *static_cast<volatile float*>(at)=g_ctNearReplaced.load(std::memory_order_relaxed);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ctFaults.fetch_add(1,std::memory_order_relaxed);
    }
}
bool ctNearValueOk(uintptr_t at) noexcept {
    __try {return *reinterpret_cast<const float*>(at)==kCtNearValue;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

void ctCameraEdit(uintptr_t cam,uint32_t mode) noexcept {
    __try {
        float* pos=reinterpret_cast<float*>(cam+kCtPosOff);
        const float* fwd=reinterpret_cast<const float*>(cam+0x20u);
        for(int i=0;i<3;++i)if(!ctFiniteSmall(pos[i])||!ctFiniteSmall(fwd[i])){g_ctRefused.fetch_add(1,std::memory_order_relaxed);return;}
        for(int i=0;i<3;++i) {
            g_ctCamBefore[i].store(pos[i],std::memory_order_relaxed);
            g_ctFwdBefore[i].store(fwd[i],std::memory_order_relaxed);
        }
        if(g_ctActing.load(std::memory_order_relaxed)&&(mode==2||mode==3)) {
            float* rows[3]={reinterpret_cast<float*>(cam),reinterpret_cast<float*>(cam+0x10u),reinterpret_cast<float*>(cam+0x20u)};
            if(g_ctAimOn.load(std::memory_order_relaxed)) {
                // The bake: the forward row is where the view looks (Flight 24: the row at the room's up looked up). The right row keeps the game's handedness, read off its own rows: with
                // h = (R x U) . F, R' = h (U' x F') gives (R' x U') . F' = h.
                const float* R=rows[0];const float* U=rows[1];const float* F=rows[2];
                const float h=(R[1]*U[2]-R[2]*U[1])*F[0]+(R[2]*U[0]-R[0]*U[2])*F[1]+(R[0]*U[1]-R[1]*U[0])*F[2]<0.0f?-1.0f:1.0f;
                float fw[3],up[3];
                for(int i=0;i<3;++i) {
                    fw[i]=g_ctAimFwd[i].load(std::memory_order_relaxed);
                    up[i]=g_ctAimUp[i].load(std::memory_order_relaxed);
                }
                rows[0][0]=h*(up[1]*fw[2]-up[2]*fw[1]);
                rows[0][1]=h*(up[2]*fw[0]-up[0]*fw[2]);
                rows[0][2]=h*(up[0]*fw[1]-up[1]*fw[0]);
                for(int i=0;i<3;++i) {
                    rows[1][i]=up[i];
                    rows[2][i]=fw[i];
                }
                const float fov=g_ctAimFov.load(std::memory_order_relaxed);
                if(fov>0.0f)*reinterpret_cast<float*>(cam+0x260u)=fov;   // the vertical field of view (READ, 0x4F30F0); the producer leaves the camera dirty, so the projection is built from it
                // The near plane: this camera's own (+0x234, READ in 0x4F30F0) and the value the game stores over it (above). Whatever is nearer is clipped by the hardware, whatever its name
                // and however large its record, so what is behind it is drawn and captured: Flight 26's Floor07, r 1018 m, with a surface 44 m from the centre, cannot be culled by record.
                const float nearM=g_ctAimNear.load(std::memory_order_relaxed);
                if(nearM>0.0f) {
                    *reinterpret_cast<float*>(cam+0x234u)=nearM;
                    ctNearRaise(nearM);
                }
            } else {
                ctNearRestore();
                // The look: the player's own axes turned: directions 0 to 3 a quarter turn at a time about the room's up, 4 and 5 straight up and down, about the camera's right row.
                float up[3];
                for(int i=0;i<3;++i)up[i]=g_ctLookUp[i].load(std::memory_order_relaxed);
                const uint32_t dir=g_ctLookDir.load(std::memory_order_relaxed);
                for(uint32_t k=0;k<(dir<4u?dir:0u);++k)for(int r=0;r<3;++r)ctTurn90(rows[r],up);
                if(dir==4u||dir==5u) {
                    const float sign=dir==4u?1.0f:-1.0f;
                    const float axis[3]={rows[0][0]*sign,rows[0][1]*sign,rows[0][2]*sign};
                    ctTurn90(rows[1],axis);
                    ctTurn90(rows[2],axis);
                }
            }
            for(int i=0;i<3;++i)pos[i]=g_ctLookCentre[i].load(std::memory_order_relaxed);
            g_ctWritten.fetch_add(1,std::memory_order_relaxed);
        } else {
            ctNearRestore();
        }
        for(int i=0;i<3;++i) {
            g_ctCamAfter[i].store(pos[i],std::memory_order_relaxed);
            g_ctFwdAfter[i].store(fwd[i],std::memory_order_relaxed);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ctFaults.fetch_add(1,std::memory_order_relaxed);
    }
}

template<int I> uintptr_t ctForward3(uintptr_t a,uintptr_t b,uintptr_t c) noexcept {
    const auto forward=reinterpret_cast<EvalFn>(g_ctEntries[I].forward.load(std::memory_order_acquire));
    if(!forward)return 0; // stood down at install; the relay is unreachable then
    return forward(a,b,c);
}

__declspec(noinline) uintptr_t __fastcall viewsObserved(uintptr_t self,uintptr_t views,uintptr_t count) noexcept {
    g_ctCalls[kIdxViews].fetch_add(1,std::memory_order_relaxed);
    const uintptr_t result=ctForward3<kIdxViews>(self,views,count);
    ctReadViews(self,views,count);
    return result;
}
__declspec(noinline) uintptr_t __fastcall producerObserved(uintptr_t h,uintptr_t pose,uintptr_t cam) noexcept {
    g_ctCalls[kIdxProducer].fetch_add(1,std::memory_order_relaxed);
    const uintptr_t site=reinterpret_cast<uintptr_t>(_ReturnAddress());   // the relay jumps, so this is the game's call site
    const uintptr_t result=ctForward3<kIdxProducer>(h,pose,cam);
    if(cam&&site==g_ctBase+kCtProducerRet)ctCameraEdit(cam,g_ctMode.load(std::memory_order_relaxed));
    else g_ctOtherSite.fetch_add(1,std::memory_order_relaxed);
    return result;
}

bool ctPinOk(int i,uintptr_t at) noexcept {
    __try {return std::memcmp(reinterpret_cast<const void*>(at),kPins[i].b,static_cast<size_t>(kPins[i].n))==0;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool ctAlreadyPatched(uintptr_t at) noexcept {
    __try {return *reinterpret_cast<const uint8_t*>(at)==0xE9;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
std::atomic<uint32_t> g_ctHooked{0};
} // namespace

// Installed once, only when hotkey.concourse_camera_test is bound (the caller decides), and only on build 334925. Each hook installs alone; true when at least one is in.
// A refusal is one log line naming the hook.
bool concourseCameraTestInstall(const char** why) noexcept {
    const char* sink=nullptr;
    if(!why)why=&sink;
    *why=nullptr;
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        if(g_ctHooked.load(std::memory_order_acquire)!=0)return true;
        if(!gameBuildKnown()){*why="the game build is not 334925, the build these offsets were read on";return false;}
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base){*why="the module base could not be read";return false;}
        g_ctBase=base;
        if(ctNearValueOk(base+kCtNearValueRva))g_ctNearAt.store(reinterpret_cast<float*>(base+kCtNearValueRva),std::memory_order_relaxed);
        else Log::get().note("concourse camera test: the near value at 0x%llX is not the 0.025 that was read: the bake frames keep the game's near plane",static_cast<unsigned long long>(kCtNearValueRva));
        uint32_t mask=0;
        for(int i=0;i<kIdxCount;++i) {
            const uintptr_t at=base+g_ctEntries[i].rva;
            if(ctAlreadyPatched(at)) {
                Log::get().note("concourse camera test: %s: 0x%llX is already patched by another EDVR module; skipped", g_ctEntries[i].name,static_cast<unsigned long long>(g_ctEntries[i].rva));
                continue;
            }
            if(!ctPinOk(i,at)) {
                Log::get().note("concourse camera test: %s: the prologue at 0x%llX is not the one that was read; skipped", g_ctEntries[i].name,static_cast<unsigned long long>(g_ctEntries[i].rva));
                continue;
            }
            if(!installOne(g_ctEntries[i],base)) {
                Log::get().note("concourse camera test: %s: CodeHook refused 0x%llX (its own line names why); skipped", g_ctEntries[i].name,static_cast<unsigned long long>(g_ctEntries[i].rva));
                continue;
            }
            mask|=1u<<i;
        }
        g_ctHooked.store(mask,std::memory_order_release);
        if(mask==0){*why="no hook could be installed";return false;}
        return true;
    } catch(...) {*why="an exception during the install";return false;}
}

// The mode (0 off, 1 log only, 2 the look, 3 the bake) and whether the camera may be written now. The relay gate is open while the mode is not 0.
void concourseCameraTestControl(uint32_t mode,bool acting,bool busy) noexcept {
    g_ctMode.store(mode,std::memory_order_relaxed);
    g_ctActing.store(acting&&(mode==2||mode==3),std::memory_order_relaxed);
    if(!acting)ctNearRestore();   // after the store: a producer that still saw acting can raise it once more, and the next tick's call puts that back too
    const bool open=mode==1||mode==2||(mode==3&&busy);
    camTestGate.store(open?uintptr_t(1):uintptr_t(0),std::memory_order_release);
}

// Where the look stands and which way it faces: set by the tick before it says acting.
void concourseCameraTestLook(const float centre[3],const float up[3],uint32_t dir) noexcept {
    for(int i=0;i<3;++i) {
        g_ctLookCentre[i].store(centre[i],std::memory_order_relaxed);
        g_ctLookUp[i].store(up[i],std::memory_order_relaxed);
    }
    g_ctLookDir.store(dir,std::memory_order_relaxed);
    g_ctAimOn.store(false,std::memory_order_relaxed);
}

// The bake's frame: where it stands, the unit axes it looks along, its vertical field of view in radians, and its near plane in metres.
void concourseCameraTestAim(const float centre[3],const float forward[3],const float up[3],float fovRad,float nearM) noexcept {
    for(int i=0;i<3;++i) {
        g_ctLookCentre[i].store(centre[i],std::memory_order_relaxed);
        g_ctAimFwd[i].store(forward[i],std::memory_order_relaxed);
        g_ctAimUp[i].store(up[i],std::memory_order_relaxed);
    }
    g_ctAimFov.store(fovRad,std::memory_order_relaxed);
    g_ctAimNear.store(nearM,std::memory_order_relaxed);
    g_ctAimOn.store(true,std::memory_order_relaxed);
}

bool concourseCameraTestHooked() noexcept {return g_ctHooked.load(std::memory_order_acquire)!=0;}
// The look and the bake move flat's camera only through the producer hook (producerObserved, 0x2874290). Flight 55: after a switch from VR to flat the automatic
// install found 0x2874290 "already patched by another EDVR module" and skipped it, yet the other hooks went in, so "hooked" was true and two bakes held the screen with
// the camera never moved. This is the test that matters.
bool concourseCameraTestCanMove() noexcept {return (g_ctHooked.load(std::memory_order_acquire)&(1u<<kIdxProducer))!=0;}

// TEMPORARY (the concourse cull's VR bake, docs\concourse-cull-2026-11-05.md): the same .data float, for VR, where the flat hooks are never installed. 0x2845730 passes it to
// 0x283FA20 as the eye cameras' near (analysis\decomp\decomp_2845730_334925.txt: DAT_145e85b8c, and 0x287B330 stores that argument at camera +0x234). Its address is checked
// exactly as flat's install checks it (the build, then the 0.025 that was read), and the write and the put-back are flat's own ctNearRaise and ctNearRestore.
bool concourseNearValueResolve(const char** why) noexcept {
    const char* sink=nullptr;
    if(!why)why=&sink;
    *why=nullptr;
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        if(g_ctNearAt.load(std::memory_order_relaxed))return true;
        if(!gameBuildKnown()){*why="the game build is not 334925, the build the near value's address was read on";return false;}
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base){*why="the module base could not be read";return false;}
        if(!ctNearValueOk(base+kCtNearValueRva)){*why="the value there is not the 0.025 that was read";return false;}
        g_ctNearAt.store(reinterpret_cast<float*>(base+kCtNearValueRva),std::memory_order_relaxed);
        return true;
    } catch(...) {*why="an exception while resolving it";return false;}
}
bool concourseNearValueRaise(float nearM,float* before) noexcept {
    float* at=g_ctNearAt.load(std::memory_order_relaxed);
    if(before)*before=0.0f;
    if(!at)return false;
    __try {
        if(before)*before=*static_cast<volatile float*>(at);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ctFaults.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    ctNearRaise(nearM);
    return g_ctNearRaised.load(std::memory_order_relaxed);
}
bool concourseNearValueRestore() noexcept {
    const bool was=g_ctNearRaised.load(std::memory_order_relaxed);
    ctNearRestore();
    return was;
}
float concourseNearValueRestoredTo() noexcept {return g_ctNearReplaced.load(std::memory_order_relaxed);}

void concourseCameraTestSnapshot(ConcourseCameraTestSnap* out) noexcept {
    if(!out)return;
    out->hookMask=g_ctHooked.load(std::memory_order_acquire);
    for(int i=0;i<kIdxCount;++i)out->calls[i]=g_ctCalls[i].load(std::memory_order_relaxed);
    out->written=g_ctWritten.load(std::memory_order_relaxed);
    out->refused=g_ctRefused.load(std::memory_order_relaxed);
    out->otherSite=g_ctOtherSite.load(std::memory_order_relaxed);
    out->noView0=g_ctNoView0.load(std::memory_order_relaxed);
    out->multiView=g_ctMulti.load(std::memory_order_relaxed);
    out->faults=g_ctFaults.load(std::memory_order_relaxed);
    out->rvCount=g_ctRvCount.load(std::memory_order_relaxed);
    out->mainCount=g_ctMain.load(std::memory_order_relaxed);
    out->flags=g_ctFlags.load(std::memory_order_relaxed);
    out->slotFlags=g_ctSlotFlags.load(std::memory_order_relaxed);
    out->slotIndex=g_ctSlotIdx.load(std::memory_order_relaxed);
    out->slotKnown=g_ctSlotKnown.load(std::memory_order_relaxed);
    for(int i=0;i<3;++i) {
        out->rvPos[i]=g_ctRvPos[i].load(std::memory_order_relaxed);
        out->slot[i]=g_ctSlot[i].load(std::memory_order_relaxed);
        out->camBefore[i]=g_ctCamBefore[i].load(std::memory_order_relaxed);
        out->camAfter[i]=g_ctCamAfter[i].load(std::memory_order_relaxed);
        out->fwdBefore[i]=g_ctFwdBefore[i].load(std::memory_order_relaxed);
        out->fwdAfter[i]=g_ctFwdAfter[i].load(std::memory_order_relaxed);
    }
}

} // namespace edvr
