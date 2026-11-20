#pragma once
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr { namespace previewcam {
using Read = bool (*)(void*, uintptr_t, void*, size_t) noexcept;
using Write = bool (*)(void*, uintptr_t, const void*, size_t) noexcept;
struct Memory { Read read; Write write; void* context; };
template<class T> bool get(const Memory& m, uintptr_t p, T& v) noexcept {
    return p && p <= UINTPTR_MAX-sizeof(v) && m.read(m.context,p,&v,sizeof(v));
}
struct Pose { float v[16]{}; };
inline bool finite(const Pose& p) noexcept {
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<3;++c)
        if(!std::isfinite(p.v[r*4+c])) return false;
    return true;
}
inline bool equal(const Pose& a,const Pose& b) noexcept {
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<3;++c)
        if(std::memcmp(&a.v[r*4+c],&b.v[r*4+c],sizeof(float))) return false;
    return true;
}
// CameraState has four Vec3 rows, with uninterpreted fourth lanes. The game
// multiplies the three basis rows, and adds base translation for row three.
inline Pose compose(const Pose& local,const Pose& base,const Pose& original) noexcept {
    Pose out=original;
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<3;++c) {
        out.v[r*4+c]=(local.v[r*4+1]*base.v[4+c]+local.v[r*4]*base.v[c])+
            local.v[r*4+2]*base.v[8+c];
        if(r==3) out.v[r*4+c]+=base.v[12+c];
    }
    return out;
}
enum Address { Child,Child10,Stack,Nested,HmdIface,HmdObject,PreviewIface,PreviewObject,
    Weight,ConstantWeight,State,AddressCount };
constexpr uint32_t kRvas[AddressCount]={0x5189070,0x5189078,0x5189370,0x5189A18,
    0x518C920,0x518C8B0,0x51AA500,0x51AA388,0x8BEA20,0xF81910,0x5294E0};
// A HeadlookCamera entry (the weapon screens' head-side producer) may be the roster's head entry once its producer hook has armed: headlookIface and headlookObject are its two vtables, set then, 0 until
// then (and for good when the hook stands down, so such an entry stays a foreign entry).
struct Game {
    uintptr_t at[AddressCount]{};
    std::atomic<uintptr_t> headlookIface{0},headlookObject{0};
    Game()=default;
    Game(const Game& o){*this=o;}
    Game& operator=(const Game& o){
        std::memcpy(at,o.at,sizeof(at));
        headlookIface.store(o.headlookIface.load(std::memory_order_relaxed),std::memory_order_relaxed);
        headlookObject.store(o.headlookObject.load(std::memory_order_relaxed),std::memory_order_relaxed);
        return *this;
    }
};
struct Identity {
    uintptr_t child=0,stack=0,predecessor=0,array=0,hmd=0,preview=0;
    uint8_t headClass=0;   // F233: 0 the HMD activity's camera, 1 a HeadlookCamera (the head entry `hmd` is of this class)
    bool operator==(const Identity& b) const noexcept {
        return child==b.child&&stack==b.stack&&predecessor==b.predecessor&&
            array==b.array&&hmd==b.hmd&&preview==b.preview&&headClass==b.headClass;
    }
};
// F232: the first check identity() failed, one value per check, in the order they run. kForeignEntry..kNoPreview belong to the roster scan (kIfaceVtable, kObjectVtable, kEnabled and kBackPointer are what the single-pair check said before the scan took every roster and are no longer produced); kOtherProducer is not an identity failure (the child
// qualified but the producer calling is not the one it names) and is only counted.
enum class Refuse : uint8_t { kNone = 0, kChildVtable, kChild10, kStack, kStackType, kPredecessor, kCount, kArray, kMode, kMembers, kRegistered, kName, kEntryPointers, kIfaceVtable, kObjectVtable,
    kEnabled, kBackPointer, kWeights, kForeignEntry, kHmdEntries, kNoPreview, kOtherProducer, kRefuseCount };
inline const char* refuseName(Refuse r) noexcept {
    static const char* const names[] = {"none", "child vtable", "child+0x10", "stack", "stack type", "predecessor", "count", "array", "mode", "members", "registered", "name", "entry pointers",
        "iface vtable", "object vtable", "enabled byte", "back-pointer", "weights", "foreign entry", "HMD entry count", "no preview entry", "other producer"};
    static_assert(sizeof(names) / sizeof(names[0]) == size_t(Refuse::kRefuseCount), "one name per reason");
    return size_t(r) < size_t(Refuse::kRefuseCount) ? names[size_t(r)] : "?";
}
constexpr uint64_t kMaxRoster = 6;
// The roster's two kinds of entry: an HMD entry is the HMD activity's interface, enabled, pointing back at the child; a preview entry the CustomisationVisual's, pointing back.
inline bool hmdEntry(const Memory& m,const Game& g,uintptr_t e,uintptr_t child) noexcept {
    uintptr_t v=0,hc=0;uint8_t enabled=0;
    return e>=0x20&&get(m,e,v)&&v==g.at[HmdIface]&&get(m,e-0x20,v)&&v==g.at[HmdObject]&&get(m,e-4,enabled)&&enabled&&get(m,e+0x120,hc)&&hc==child;
}
inline bool headlookEntry(const Memory& m,const Game& g,uintptr_t e,uintptr_t child) noexcept {
    uintptr_t v=0,hc=0;uint8_t enabled=0;
    const uintptr_t iface=g.headlookIface.load(std::memory_order_relaxed),object=g.headlookObject.load(std::memory_order_relaxed);
    return iface&&object&&e>=0x20&&get(m,e,v)&&v==iface&&get(m,e-0x20,v)&&v==object&&get(m,e-4,enabled)&&enabled&&get(m,e+0x120,hc)&&hc==child;
}
inline bool previewEntry(const Memory& m,const Game& g,uintptr_t e,uintptr_t child) noexcept {
    uintptr_t v=0,pc=0;
    return e>=0x60&&get(m,e,v)&&v==g.at[PreviewIface]&&get(m,e-0x60,v)&&v==g.at[PreviewObject]&&get(m,e+0x120,pc)&&pc==child;
}
// The identity check, naming the first failing check in `why`. The Location child's roster holds two to six producers in any order: exactly one head entry (the HMD activity's camera or, once its hook has
// armed, a HeadlookCamera), at least one preview entry, nothing else; the preview is the last preview entry. (A suit screen opened from the loadout browser leaves [HMD, old preview, new preview] and then
// [new preview, HMD]; the weapon screens have [preview, HeadlookCamera].) The scan reads the same memory the finalizer reads twice, so the answer is the same for the same memory.
inline bool identityWhy(const Memory& m,const Game& g,uintptr_t child,Identity& out,Refuse& why) noexcept {
    Identity q; q.child=child;
    uintptr_t v=0,v10=0; uint64_t count=0; uint32_t mode=0;
    if(!get(m,child,v)||v!=g.at[Child]){why=Refuse::kChildVtable;return false;}
    if(!get(m,child+0x10,v10)||v10!=g.at[Child10]){why=Refuse::kChild10;return false;}
    if(!get(m,child+0x30,q.stack)||!get(m,q.stack,v)){why=Refuse::kStack;return false;}
    if(v!=g.at[Stack]&&v!=g.at[Nested]){why=Refuse::kStackType;return false;}
    if(!get(m,child+0x40,q.predecessor)||!q.predecessor){why=Refuse::kPredecessor;return false;}
    if(!get(m,child+0x58,count)||count<2||count>kMaxRoster){why=Refuse::kCount;return false;}
    if(!get(m,child+0x60,q.array)){why=Refuse::kArray;return false;}
    if(!get(m,child+0x68,mode)||mode!=0){why=Refuse::kMode;return false;}
    unsigned members=0;uint8_t registered=0;
    for(unsigned i=0;i<11;++i) {uintptr_t c=0;if(!get(m,q.stack+0x28+i*8,c)){why=Refuse::kMembers;return false;}members+=c==child?1u:0u;}
    if(members!=1){why=Refuse::kMembers;return false;}
    if(!get(m,child+0x38,registered)||registered!=1){why=Refuse::kRegistered;return false;}
    uintptr_t name=0;uint64_t length=0;char text[8]{};
    if(!get(m,q.stack+0x20,name)||!get(m,name,length)||length!=8||!m.read(m.context,name+0x14,text,8)||std::memcmp(text,"Location",8)){why=Refuse::kName;return false;}
    const bool headlookOn=g.headlookIface.load(std::memory_order_relaxed)!=0;
    unsigned heads=0,previews=0;uintptr_t head=0,preview=0;uint8_t headClass=0;
    for(uint64_t i=0;i<count;++i) {
        uintptr_t e=0;
        if(!get(m,q.array+i*8,e)||e<0x60){why=Refuse::kEntryPointers;return false;}
        if(hmdEntry(m,g,e,child)){++heads;head=e;headClass=0;}
        else if(headlookOn&&headlookEntry(m,g,e,child)){++heads;head=e;headClass=1;}
        else if(previewEntry(m,g,e,child)){++previews;preview=e;}
        else {why=Refuse::kForeignEntry;return false;}
    }
    if(heads!=1){why=Refuse::kHmdEntries;return false;}
    if(!previews){why=Refuse::kNoPreview;return false;}
    q.hmd=head;q.preview=preview;q.headClass=headClass;
    uintptr_t f=0;
    const uintptr_t headIface=q.headClass==1?g.headlookIface.load(std::memory_order_relaxed):g.at[HmdIface];   // the weights of the head class present
    if(!get(m,headIface+0x20,f)||f!=g.at[Weight]||
       !get(m,g.at[PreviewIface]+0x20,f)||f!=g.at[ConstantWeight]||
       !get(m,headIface+0x28,f)||f!=g.at[State]||
       !get(m,g.at[PreviewIface]+0x28,f)||f!=g.at[State]){why=Refuse::kWeights;return false;}
    out=q;why=Refuse::kNone;return true;
}
inline bool identity(const Memory& m,const Game& g,uintptr_t child,Identity& out) noexcept {
    Refuse why=Refuse::kNone;
    return identityWhy(m,g,child,out,why);
}

// ---- F232: why a linked child was refused, and a snapshot of its roster -----------------------------------------------------------------------------------------------------------------
constexpr uint64_t kUnread=~uint64_t(0);
struct RosterEntry {
    uint64_t ptr=kUnread,iface=kUnread,obj20=kUnread,obj60=kUnread,back=kUnread;
    uint32_t b4=0x100;   // the byte at entry-4; 0x100: unread
};
struct Refusal {
    uint8_t role=0;                      // 0 the HMD activity's producer, 1 the preview's
    Refuse why=Refuse::kNone;
    uint64_t child=0,count=kUnread,mode=kUnread,registered=kUnread,stackVtable=kUnread,members=kUnread;
    bool nameRead=false;
    char name[9]{};
    unsigned entries=0;                  // min(count, kMaxRoster) entries were read
    RosterEntry entry[kMaxRoster];
};
// Best-effort reads of the child's roster. Fixed size, no allocation; an unreadable value stays kUnread.
inline void snapshotRoster(const Memory& m,const Game& g,uintptr_t child,unsigned role,Refuse why,Refusal& r) noexcept {
    (void)g;
    r=Refusal();r.role=uint8_t(role);r.why=why;r.child=child;
    uint64_t count=0,mode64=0;uint32_t mode=0;uint8_t registered=0;uintptr_t stack=0,array=0,v=0;
    if(get(m,child+0x58,count))r.count=count;
    if(get(m,child+0x68,mode))r.mode=mode;
    if(get(m,child+0x38,registered))r.registered=registered;
    (void)mode64;
    if(get(m,child+0x30,stack)&&get(m,stack,v)) {
        r.stackVtable=v;
        unsigned members=0;bool ok=true;
        for(unsigned i=0;i<11;++i) {uintptr_t c=0;if(!get(m,stack+0x28+i*8,c)){ok=false;break;}members+=c==child?1u:0u;}
        if(ok)r.members=members;
        uintptr_t name=0;
        if(get(m,stack+0x20,name)&&m.read(m.context,name+0x14,r.name,8)) {r.nameRead=true;r.name[8]=0;}
    }
    if(r.count!=kUnread&&get(m,child+0x60,array)) {
        r.entries=unsigned(r.count<kMaxRoster?r.count:kMaxRoster);
        for(unsigned i=0;i<r.entries;++i) {
            RosterEntry& e=r.entry[i];uintptr_t p=0,x=0,back=0;uint8_t b=0;
            if(!get(m,array+i*8,p))continue;
            e.ptr=p;
            if(get(m,p,x))e.iface=x;
            if(p>=0x20&&get(m,p-0x20,x))e.obj20=x;
            if(p>=0x60&&get(m,p-0x60,x))e.obj60=x;
            if(get(m,p+0x120,back))e.back=back;
            if(p>=4&&get(m,p-4,b))e.b4=b;
        }
    }
}
// A value as an RVA when it lies in the image ("+0x1234"), else as raw hex; "unreadable" for kUnread.
inline void rvaText(char* out,size_t cap,uint64_t value,uintptr_t base) noexcept {
    if(value==kUnread)std::snprintf(out,cap,"unreadable");
    else if(base&&value>=base&&value-base<0x10000000ull)std::snprintf(out,cap,"+0x%llX",static_cast<unsigned long long>(value-base));
    else std::snprintf(out,cap,"0x%llX",static_cast<unsigned long long>(value));
}
inline void numText(char* out,size_t cap,uint64_t value) noexcept {
    if(value==kUnread)std::snprintf(out,cap,"unreadable");
    else std::snprintf(out,cap,"%llu",static_cast<unsigned long long>(value));
}
inline const char* roleName(unsigned role) noexcept {return role==0?"HMD":"preview";}
inline const char* diagTag() noexcept {return "native preview camera (F232):";}
inline const char* eyeTag() noexcept {return "native preview camera (F233):";}
// "role R refused: <reason>; roster: ..." and the role's per-reason counts. `counts` is indexed by Refuse.
inline int formatRefusal(char* buf,size_t cap,const Refusal& r,const uint64_t counts[size_t(Refuse::kRefuseCount)],uintptr_t base) noexcept {
    char count[24],mode[24],reg[24],stackVt[28],members[24];
    numText(count,sizeof(count),r.count);numText(mode,sizeof(mode),r.mode);numText(reg,sizeof(reg),r.registered);
    rvaText(stackVt,sizeof(stackVt),r.stackVtable,base);numText(members,sizeof(members),r.members);
    size_t at=size_t(std::snprintf(buf,cap,"%s role %s refused: %s; roster: child 0x%llX, count %s, mode %s, registered %s, stack vtable %s, members %s, name \"%s\"",diagTag(),roleName(r.role),
        refuseName(r.why),static_cast<unsigned long long>(r.child),count,mode,reg,stackVt,members,r.nameRead?r.name:"unreadable"));
    for(unsigned i=0;i<r.entries&&at<cap;++i) {
        const RosterEntry& e=r.entry[i];char iface[28],o20[28],o60[28],b4[8];
        rvaText(iface,sizeof(iface),e.iface,base);rvaText(o20,sizeof(o20),e.obj20,base);rvaText(o60,sizeof(o60),e.obj60,base);
        if(e.b4>0xFF)std::snprintf(b4,sizeof(b4),"?");else std::snprintf(b4,sizeof(b4),"%02X",e.b4);
        at+=size_t(std::snprintf(buf+at,cap-at,"; entry %u: ptr 0x%llX iface %s obj-0x20 %s obj-0x60 %s back %s b-4 %s",i,static_cast<unsigned long long>(e.ptr==kUnread?0:e.ptr),iface,o20,o60,
            e.back==kUnread?"unreadable":e.back==r.child?"yes":"no",b4));
    }
    if(at<cap)at+=size_t(std::snprintf(buf+at,cap-at,"; refusals by reason:"));
    bool any=false;
    for(size_t i=1;i<size_t(Refuse::kRefuseCount)&&at<cap;++i)
        if(counts[i]){any=true;at+=size_t(std::snprintf(buf+at,cap-at," %s %llu,",refuseName(Refuse(i)),static_cast<unsigned long long>(counts[i])));}
    if(!any&&at<cap)at+=size_t(std::snprintf(buf+at,cap-at," none"));
    return int(at);
}
// At most kDiagLines lines in any ten seconds (the frame thread's own, no shared state).
constexpr unsigned kDiagLines=40;
struct DiagCap {
    uint64_t windowMs=0;unsigned written=0;
    bool allow(uint64_t nowMs) noexcept {
        if(!windowMs||nowMs-windowMs>=10000){windowMs=nowMs;written=0;}
        if(written>=kDiagLines)return false;
        ++written;return true;
    }
};

struct Sample { Identity id; Pose primary,local,base; };
struct EyeSnapshot { Pose composed{}; float raw[3]{}; uint32_t epoch=0; uint64_t generation=0; };
inline bool hmdSample(const Memory& m,const Identity& id,Sample& s) noexcept {
    s.id=id;
    return get(m,id.hmd+0x50,s.primary)&&get(m,id.hmd+0xD0,s.local)&&
        get(m,id.predecessor+0x40,s.base)&&finite(s.primary)&&finite(s.local)&&finite(s.base);
}
inline bool previewSample(const Memory& m,const Identity& id,Sample& s) noexcept {
    s.id=id;uint32_t mode=2;
    return get(m,id.preview+0x33C,mode)&&mode!=2&&get(m,id.preview+0x50,s.primary)&&
        get(m,id.preview+0x470,s.base)&&finite(s.primary)&&finite(s.base)&&equal(s.primary,s.base);
}

// ---- F233: the eye path's evidence (log only) -----------------------------------------------------------------------------------------------------------------------------------------
// At finalization: the translations of child+0xB0 (the output), the head entry's primary, the preview's primary and the base, and for a HeadlookCamera its look rotation and displacement; whether the
// frame was applied or refused and why. In the eye stub: the head translation the eye builder carries, whether it was replaced, the published head. The producers' and finalizer's hooks take a
// try-claim on a fixed slot with an atomic_flag, only while the frame thread wants it (Engine::diagWanted), and skip when it is busy; nothing is logged or allocated there.
enum class FinalWhy : uint8_t { kApplied = 0, kIncomplete, kIdentity, kSamples, kOutputRead, kOutputMismatch, kNotCommitted, kFinalWhyCount };
inline const char* finalWhyName(FinalWhy w) noexcept {
    static const char* const names[] = {"applied", "a producer did not finish", "the roster changed", "a sample changed", "the output could not be read",
        "the output is neither the head's nor the preview's pose", "not committed"};
    static_assert(sizeof(names) / sizeof(names[0]) == size_t(FinalWhy::kFinalWhyCount), "one name per reason");
    return size_t(w) < size_t(FinalWhy::kFinalWhyCount) ? names[size_t(w)] : "?";
}
struct Vec3 { float v[3]{}; bool read=false; };
struct F3 { float v[3]; };
inline void readTranslation(const Memory& m,uintptr_t pose,Vec3& out) noexcept {
    Pose p;
    if(pose&&get(m,pose,p)){out.v[0]=p.v[12];out.v[1]=p.v[13];out.v[2]=p.v[14];out.read=true;}
}
inline void fromPose(const Pose* p,Vec3& out) noexcept {
    if(p){out.v[0]=p->v[12];out.v[1]=p->v[13];out.v[2]=p->v[14];out.read=true;}
}
struct FinalSnap {
    FinalWhy why=FinalWhy::kApplied;
    uint8_t headClass=0;
    uint64_t child=0;
    Vec3 out,head,preview,base;
    bool extraRead=false;
    float extra[4][3]{};   // a HeadlookCamera's look rotation rows (+0x250, +0x260, +0x270) and displacement (+0x280)
};
// The poses already sampled (the finalizer has them when the frame got that far) are used as they are; anything else is read from the game now.
inline void snapshotFinal(const Memory& m,const Identity& id,FinalWhy why,const Pose* output,const Pose* head,const Pose* preview,FinalSnap& s) noexcept {
    s=FinalSnap();s.why=why;s.headClass=id.headClass;s.child=id.child;
    if(output)fromPose(output,s.out);else readTranslation(m,id.child+0xB0,s.out);
    if(head)fromPose(head,s.head);else readTranslation(m,id.hmd+0x50,s.head);
    if(preview)fromPose(preview,s.preview);else readTranslation(m,id.preview+0x50,s.preview);
    readTranslation(m,id.predecessor+0x40,s.base);
    if(id.headClass==1) {
        const uint32_t offs[4]={0x250,0x260,0x270,0x280};bool ok=true;
        for(unsigned i=0;i<4;++i) {F3 f;if(get(m,id.hmd+offs[i],f)){std::memcpy(s.extra[i],f.v,sizeof(f.v));}else ok=false;}
        s.extraRead=ok;
    }
}
struct EyeSeen {
    Vec3 head;               // the translation the eye builder carries before EDVR touches it
    bool replaced=false;
    bool published=false;    // a head was published for this evaluation
    Vec3 composed,raw;       // the published eye head and the HMD primary it was built beside
};
inline float distance3(const Vec3& a,const Vec3& b) noexcept {
    const float dx=a.v[0]-b.v[0],dy=a.v[1]-b.v[1],dz=a.v[2]-b.v[2];
    return std::sqrt(dx*dx+dy*dy+dz*dz);
}
inline void vecText(char* out,size_t cap,const Vec3& v) noexcept {
    if(!v.read)std::snprintf(out,cap,"unreadable");
    else std::snprintf(out,cap,"(%.3f, %.3f, %.3f)",static_cast<double>(v.v[0]),static_cast<double>(v.v[1]),static_cast<double>(v.v[2]));
}
inline void distText(char* out,size_t cap,const Vec3& a,const Vec3& b) noexcept {
    if(!a.read||!b.read)std::snprintf(out,cap,"n/a");
    else std::snprintf(out,cap,"%.3f",static_cast<double>(distance3(a,b)));
}
inline int formatFinal(char* buf,size_t cap,const FinalSnap& s) noexcept {
    char o[64],h[64],p[64],b[64],oh[24],op[24];
    vecText(o,sizeof(o),s.out);vecText(h,sizeof(h),s.head);vecText(p,sizeof(p),s.preview);vecText(b,sizeof(b),s.base);
    distText(oh,sizeof(oh),s.out,s.head);distText(op,sizeof(op),s.out,s.preview);
    size_t at=size_t(std::snprintf(buf,cap,"%s final child 0x%llX head class %s: %s%s%s%s; output %s, head primary %s, preview primary %s, base %s; output to head %s m, output to preview %s m",
        eyeTag(),static_cast<unsigned long long>(s.child),s.headClass==1?"Headlook":"HMD",s.why==FinalWhy::kApplied?"applied":"refused",s.why==FinalWhy::kApplied?"":" (",
        s.why==FinalWhy::kApplied?"":finalWhyName(s.why),s.why==FinalWhy::kApplied?"":")",o,h,p,b,oh,op));
    if(s.headClass==1&&at<cap) {
        if(!s.extraRead)at+=size_t(std::snprintf(buf+at,cap-at,"; headlook rotation and displacement unreadable"));
        else at+=size_t(std::snprintf(buf+at,cap-at,"; headlook R rows (%.3f, %.3f, %.3f) (%.3f, %.3f, %.3f) (%.3f, %.3f, %.3f), d (%.3f, %.3f, %.3f)",
            double(s.extra[0][0]),double(s.extra[0][1]),double(s.extra[0][2]),double(s.extra[1][0]),double(s.extra[1][1]),double(s.extra[1][2]),
            double(s.extra[2][0]),double(s.extra[2][1]),double(s.extra[2][2]),double(s.extra[3][0]),double(s.extra[3][1]),double(s.extra[3][2])));
    }
    return int(at);
}
// The eye stub's line: the head it carried, whether it was replaced, and its distances in metres to the published eye head and (from the latest finalization, `last`, null when none) to the output, the head
// primary, the preview primary, the base, and to the tracking origin.
inline int formatEye(char* buf,size_t cap,const EyeSeen& e,const FinalSnap* last) noexcept {
    char head[64],pub[64],raw[64],dPub[24],dOut[24],dHead[24],dPrev[24],dBase[24],dOrigin[24];
    Vec3 origin;origin.read=true;
    vecText(head,sizeof(head),e.head);vecText(pub,sizeof(pub),e.published?e.composed:Vec3());vecText(raw,sizeof(raw),e.published?e.raw:Vec3());
    distText(dPub,sizeof(dPub),e.head,e.published?e.composed:Vec3());
    const Vec3 none;
    distText(dOut,sizeof(dOut),e.head,last?last->out:none);distText(dHead,sizeof(dHead),e.head,last?last->head:none);
    distText(dPrev,sizeof(dPrev),e.head,last?last->preview:none);distText(dBase,sizeof(dBase),e.head,last?last->base:none);
    distText(dOrigin,sizeof(dOrigin),e.head,origin);
    return std::snprintf(buf,cap,"%s eye head %s, %s; published eye head %s, HMD primary %s; distance to published %s m, to output %s m, to head primary %s m, to preview primary %s m, to base %s m, to origin %s m",
        eyeTag(),head,e.replaced?"replaced":"not replaced",pub,raw,dPub,dOut,dHead,dPrev,dBase,dOrigin);
}

// One synchronous camera-manager evaluation encloses all parallel producers
// and the later-priority finalizers. Overlap poisons that entire evaluation.
// Independent producer slots publish immutable samples with release/acquire;
// there is no mutex, waiting, allocation or game task serialization.
class Engine {
    static constexpr uint64_t Commit=uint64_t(1)<<29,Initial=uint64_t(1)<<30,Poison=uint64_t(1)<<31,CountMask=Commit-1;
    static constexpr unsigned Slots=16;
    struct Row {
        std::atomic<uintptr_t> child{0};std::atomic<uint32_t> cycle{0};
        std::atomic<bool> closed{false};std::atomic<unsigned> phase[2]{};
        std::atomic<uintptr_t> owner[2]{};
        Sample sample[2];
        void reset() noexcept {child.store(0);cycle.store(0);closed.store(false);for(auto& p:phase)p.store(0);for(auto& p:owner)p.store(0);}
    } rows_[Slots];
    std::atomic<uint64_t> state_{0};std::atomic<bool> enabled_{false};
    // The kind-5 eye cameras are built after this evaluation, from a copy of
    // the HMD primary made before the finalizer. The copy is what becomes
    // camera+0x50 and then cb1 row 275. Location child+0xB0 is a different store.
    struct EyeHead {
        // A failed claim leaves that eye on the game's pose. Neither side waits
        // for another camera job, and the payload is never read while written.
        mutable std::atomic_flag busy=ATOMIC_FLAG_INIT;
        std::atomic<uint32_t> serial{0};
        std::atomic<uint64_t> generation{1};
        uint64_t publishedGeneration=0;
        Pose composed{};
        float raw[3]{};
    } eye_;
    struct FinalSlot {
        std::atomic_flag busy=ATOMIC_FLAG_INIT;
        uint64_t serial=0,taken=0;
        FinalSnap snap;
    } final_;
    struct EyeSlot {
        std::atomic_flag busy=ATOMIC_FLAG_INIT;
        uint64_t serial=0,taken=0;
        EyeSeen seen;
    } eyeSeen_;
    struct Diag {
        std::atomic_flag busy=ATOMIC_FLAG_INIT;
        uint64_t serial=0,taken=0;   // serial: written under the claim; taken: the reader's, under the claim
        Refusal refusal;
    } diag_[2];
    Row* row(uintptr_t child,bool create) noexcept {
        if(!child)return nullptr;
        for(unsigned i=0;i<Slots;++i) {
            auto& r=rows_[i];uintptr_t key=r.child.load(std::memory_order_acquire);
            if(!key&&create) {uintptr_t zero=0;r.child.compare_exchange_strong(zero,child);key=r.child.load();}
            if(key==child)return &r;
        }
        return nullptr;
    }
public:
    std::atomic<uint64_t> applied{0},refused{0},evaluations{0},hmdCalls{0},previewCalls{0},finalizerCalls{0},hmdQualified{0},previewQualified{0};
    // F232: per role and reason, how often a linked child was refused (the producer hooks); a roster snapshot of the latest refusal while the frame thread wants one. Wait-free: a refusal
    // takes a try-claim on the role's slot with an atomic_flag and is skipped, never waited for, when the slot is busy; the counters are plain atomics; nothing is logged or allocated here.
    std::atomic<uint64_t> reasons[2][size_t(Refuse::kRefuseCount)]{};
    std::atomic<bool> diagWanted{false};
    void refusedChild(const Memory& m,const Game& g,uintptr_t child,unsigned role,Refuse why) noexcept {
        if(role>1)return;
        reasons[role][size_t(why)].fetch_add(1,std::memory_order_relaxed);
        if(why==Refuse::kOtherProducer||!diagWanted.load(std::memory_order_relaxed))return;
        Diag& d=diag_[role];
        if(d.busy.test_and_set(std::memory_order_acquire))return;
        snapshotRoster(m,g,child,role,why,d.refusal);
        ++d.serial;
        d.busy.clear(std::memory_order_release);
    }
    // F233: the finalizer's snapshot of an eligible child (applied or refused, and why); the poses it already holds are passed so that the values are the ones it judged.
    void finalSeen(const Memory& m,const Identity& id,FinalWhy why,const Pose* output,const Pose* head,const Pose* preview) noexcept {
        if(!diagWanted.load(std::memory_order_relaxed))return;
        if(final_.busy.test_and_set(std::memory_order_acquire))return;
        snapshotFinal(m,id,why,output,head,preview,final_.snap);
        ++final_.serial;
        final_.busy.clear(std::memory_order_release);
    }
    bool takeFinal(FinalSnap& out) noexcept {
        if(final_.busy.test_and_set(std::memory_order_acquire))return false;
        const bool fresh=final_.serial!=final_.taken;
        if(fresh){out=final_.snap;final_.taken=final_.serial;}
        final_.busy.clear(std::memory_order_release);
        return fresh;
    }
    // F233: the eye stub's snapshot: the pose the builder carried (null if unreadable), the published head (null when none), whether it was replaced.
    void eyeSeen(const Pose* carried,const EyeSnapshot* published,bool replaced) noexcept {
        if(!diagWanted.load(std::memory_order_relaxed))return;
        if(eyeSeen_.busy.test_and_set(std::memory_order_acquire))return;
        EyeSeen& s=eyeSeen_.seen;s=EyeSeen();
        fromPose(carried,s.head);s.replaced=replaced;
        if(published) {
            s.published=true;fromPose(&published->composed,s.composed);
            s.raw.v[0]=published->raw[0];s.raw.v[1]=published->raw[1];s.raw.v[2]=published->raw[2];s.raw.read=true;
        }
        ++eyeSeen_.serial;
        eyeSeen_.busy.clear(std::memory_order_release);
    }
    bool takeEyeSeen(EyeSeen& out) noexcept {
        if(eyeSeen_.busy.test_and_set(std::memory_order_acquire))return false;
        const bool fresh=eyeSeen_.serial!=eyeSeen_.taken;
        if(fresh){out=eyeSeen_.seen;eyeSeen_.taken=eyeSeen_.serial;}
        eyeSeen_.busy.clear(std::memory_order_release);
        return fresh;
    }
    // The frame thread: the latest snapshot of a role if there is a new one and its slot is not busy, with the role's counts.
    bool takeRefusal(unsigned role,Refusal& out,uint64_t counts[size_t(Refuse::kRefuseCount)]) noexcept {
        if(role>1)return false;
        Diag& d=diag_[role];
        if(d.busy.test_and_set(std::memory_order_acquire))return false;
        const bool fresh=d.serial!=d.taken;
        if(fresh){out=d.refusal;d.taken=d.serial;}
        d.busy.clear(std::memory_order_release);
        for(size_t i=0;i<size_t(Refuse::kRefuseCount);++i)counts[i]=reasons[role][i].load(std::memory_order_relaxed);
        return fresh;
    }
    void poison() noexcept {state_.fetch_or(Poison,std::memory_order_acq_rel);}
    void enable(bool value) noexcept {
        enabled_.store(value,std::memory_order_release);
        if(!value) {
            eye_.generation.fetch_add(1,std::memory_order_acq_rel);
            eye_.serial.store(0,std::memory_order_release);
            poison();
        }
    }
    uint32_t current() const noexcept {
        const uint64_t s=state_.load(std::memory_order_acquire);
        return enabled_.load()&&(s&CountMask)==1&&!(s&(Commit|Initial|Poison))?uint32_t(s>>32):0;
    }
    bool valid(uint32_t epoch) const noexcept {return epoch&&current()==epoch;}
    bool eyeEpoch(uint32_t epoch) const noexcept {
        // The eye builder follows manager completion, when the count is zero.
        const uint64_t s=state_.load(std::memory_order_acquire);
        return epoch&&enabled_.load(std::memory_order_acquire)&&uint32_t(s>>32)==epoch&&
            !(s&(Commit|Initial|Poison));
    }
    void enter() noexcept {
        uint64_t old=state_.load(),next=0;
        for(;;) {
            const uint64_t n=old&CountMask;
            next=n?((old+1)|Poison):((((old>>32)+1)<<32)|Initial|1);
            if(state_.compare_exchange_weak(old,next))break;
        }
        if(!(old&CountMask)) {
            for(auto& r:rows_)r.reset();
            eye_.serial.store(0,std::memory_order_release);
            evaluations.fetch_add(1,std::memory_order_relaxed);
            if(enabled_.load()&&uint32_t(next>>32))state_.compare_exchange_strong(next,next&~Initial);
        }
    }
    void leave() noexcept {state_.fetch_sub(1,std::memory_order_release);}
    // The commit decision is atomic with manager admission. A later manager
    // poisons subsequent work, not a write already committed. Actual pose
    // stores remain inside the game's exclusive child-finalizer callback;
    // this is not an atomic multiword pose or a repair for invalid concurrent
    // dispatch of the same game subscriber collection.
    // Publish the head the later eye-camera builder multiplies. Cleared when
    // the next evaluation starts, so a missed builder cannot reuse a stale head.
    void publishEye(uint32_t epoch,const Pose& composed,const Pose& raw) noexcept {
        const uint64_t generation=eye_.generation.load(std::memory_order_acquire);
        if(!valid(epoch))return;
        if(eye_.busy.test_and_set(std::memory_order_acquire)) {
            // Another head cannot stand in for this one. Invalidate even a
            // writer holding the claim, rather than leave its pose available.
            eye_.generation.fetch_add(1,std::memory_order_acq_rel);
            eye_.serial.store(0,std::memory_order_release);
            return;
        }
        eye_.serial.store(0,std::memory_order_release);
        eye_.composed=composed;
        eye_.raw[0]=raw.v[12];eye_.raw[1]=raw.v[13];eye_.raw[2]=raw.v[14];
        eye_.publishedGeneration=generation;
        if(valid(epoch)&&eye_.generation.load(std::memory_order_acquire)==generation)
            eye_.serial.store(epoch,std::memory_order_release);
        eye_.busy.clear(std::memory_order_release);
    }
    bool eyeSnapshotValid(const EyeSnapshot& snapshot) const noexcept {
        return snapshot.epoch&&eye_.serial.load(std::memory_order_acquire)==snapshot.epoch&&
            eye_.generation.load(std::memory_order_acquire)==snapshot.generation&&eyeEpoch(snapshot.epoch);
    }
    bool loadEye(EyeSnapshot& snapshot) const noexcept {
        if(!enabled_.load(std::memory_order_acquire)||eye_.busy.test_and_set(std::memory_order_acquire))return false;
        const uint32_t serial=eye_.serial.load(std::memory_order_acquire);
        const uint64_t generation=eye_.generation.load(std::memory_order_acquire);
        const bool published=eye_.publishedGeneration==generation&&eyeEpoch(serial);
        if(published) {
            snapshot.composed=eye_.composed;
            snapshot.raw[0]=eye_.raw[0];snapshot.raw[1]=eye_.raw[1];snapshot.raw[2]=eye_.raw[2];
            snapshot.epoch=serial;snapshot.generation=generation;
        }
        const bool valid=published&&eye_.serial.load(std::memory_order_acquire)==serial&&
            eye_.generation.load(std::memory_order_acquire)==generation&&eyeEpoch(serial);
        eye_.busy.clear(std::memory_order_release);
        return valid;
    }
    bool loadEye(Pose& composed,float raw[3]) const noexcept {
        EyeSnapshot snapshot;
        if(!loadEye(snapshot))return false;
        composed=snapshot.composed;
        raw[0]=snapshot.raw[0];raw[1]=snapshot.raw[1];raw[2]=snapshot.raw[2];
        return true;
    }
    template<class Store> bool commit(uint32_t epoch,Store store) noexcept {
        if(!epoch||!enabled_.load())return false;
        uint64_t expected=(uint64_t(epoch)<<32)|1;
        if(!state_.compare_exchange_strong(expected,expected|Commit))return false;
        const bool result=store();state_.fetch_and(~Commit,std::memory_order_release);return result;
    }
    void invalidateRole(uintptr_t child,unsigned role) noexcept {
        if(!current())return;
        Row* r=row(child,false);if(!r)return;
        // A second invocation before finalization cannot reuse an older
        // successful result, even if this invocation has an unsupported roster.
        if(r->closed.load()||r->phase[role].load()!=0)poison();
    }
    void invalidateProducer(uintptr_t owner,unsigned role) noexcept {
        if(!current())return;
        for(auto& r:rows_) {
            const uintptr_t prior=r.owner[role].load(std::memory_order_acquire);
            if(prior&&prior==owner){poison();return;}
        }
    }
    struct Completion {uint32_t epoch=0,cycle=0;uintptr_t child=0;unsigned role=0;};
    // The no-op production specialization has no callback overhead. The seam
    // lets the rig enter another producer at the exact claim/publication gap.
    template<class AfterClaim> Completion beginWithClaim(uintptr_t child,unsigned role,uintptr_t owner,AfterClaim afterClaim) noexcept {
        Completion c;c.epoch=current();c.child=child;c.role=role;
        if(!c.epoch)return {};
        Row* r=row(child,true);if(!r||r->closed.load()){poison();return {};}
        c.cycle=r->cycle.load();uintptr_t unowned=0;
        if(!r->owner[role].compare_exchange_strong(unowned,owner?owner:child)){poison();return {};}
        afterClaim();
        unsigned empty=0;
        if(!r->phase[role].compare_exchange_strong(empty,1)){poison();return {};}
        return c;
    }
    Completion begin(uintptr_t child,unsigned role,uintptr_t owner=0) noexcept {
        return beginWithClaim(child,role,owner,[]{});
    }
    void finish(const Completion& c,const Sample* s) noexcept {
        if(!valid(c.epoch))return;
        Row* r=row(c.child,false);
        if(!r||r->closed.load()||r->cycle.load()!=c.cycle){poison();return;}
        if(s)r->sample[c.role]=*s;
        r->phase[c.role].store(s?2u:3u,std::memory_order_release);
    }
    bool take(uint32_t epoch,uintptr_t child,Sample& h,Sample& p) noexcept {
        if(!valid(epoch))return false;
        Row* r=row(child,false);if(!r)return false;
        if(r->closed.exchange(true)){poison();return false;}
        const unsigned a=r->phase[0].load(std::memory_order_acquire),b=r->phase[1].load(std::memory_order_acquire);
        if(a==1||b==1||(!a&&r->owner[0].load())||(!b&&r->owner[1].load())){poison();return false;}
        const bool ok=a==2&&b==2;
        if(ok){h=r->sample[0];p=r->sample[1];}
        r->phase[0].store(0);r->phase[1].store(0);r->owner[0].store(0);r->owner[1].store(0);
        r->cycle.fetch_add(1);r->closed.store(false);
        return ok&&valid(epoch);
    }
};
struct Evaluation {Engine& e;explicit Evaluation(Engine& a):e(a){e.enter();}~Evaluation(){e.leave();}};
template<class Original> uintptr_t manager(Engine& e,Original original) {Evaluation scope(e);return original();}
template<class Original> uintptr_t hmd(Engine& e,const Memory& m,const Game& g,uintptr_t object,Original original,bool headlook=false) {
    e.hmdCalls.fetch_add(1,std::memory_order_relaxed);
    e.invalidateProducer(object,0);
    uintptr_t child=0;Identity before;Pose baseBefore;
    const bool linked=e.current()&&get(m,object+0x140,child);
    if(linked)e.invalidateRole(child,0);
    Refuse why=Refuse::kNone;
    const bool known=linked&&identityWhy(m,g,child,before,why);
    if(linked&&!known)e.refusedChild(m,g,child,0,why);
    else if(known&&(before.hmd!=object+0x20||before.headClass!=(headlook?1u:0u)))e.refusedChild(m,g,child,0,Refuse::kOtherProducer);
    const bool eligible=known&&before.hmd==object+0x20&&before.headClass==(headlook?1u:0u)&&
        get(m,before.predecessor+0x40,baseBefore)&&finite(baseBefore);
    const auto c=eligible?e.begin(child,0,object):Engine::Completion{};const uintptr_t result=original();
    Identity after;Sample s;
    const bool good=eligible&&e.valid(c.epoch)&&identity(m,g,child,after)&&before==after&&
        hmdSample(m,after,s)&&equal(s.base,baseBefore)&&
        (headlook?true:!equal(s.primary,s.base));   // F233: HeadlookCamera's primary is the base with idle look input: only the base unchanged and a finite primary (hmdSample) are asked of it
    if(good)e.hmdQualified.fetch_add(1,std::memory_order_relaxed);
    e.finish(c,good?&s:nullptr);return result;
}
template<class Original> uintptr_t preview(Engine& e,const Memory& m,const Game& g,uintptr_t subscriber,Original original) {
    e.previewCalls.fetch_add(1,std::memory_order_relaxed);
    e.invalidateProducer(subscriber,1);
    uintptr_t child=0;Identity before;const bool linked=subscriber>=0x1B0&&e.current()&&get(m,subscriber-0x30,child);
    if(linked)e.invalidateRole(child,1);
    Refuse why=Refuse::kNone;
    const bool known=linked&&identityWhy(m,g,child,before,why);
    if(linked&&!known)e.refusedChild(m,g,child,1,why);
    else if(known&&before.preview!=subscriber-0x150)e.refusedChild(m,g,child,1,Refuse::kOtherProducer);
    const bool eligible=known&&before.preview==subscriber-0x150;
    const auto c=eligible?e.begin(child,1,subscriber):Engine::Completion{};const uintptr_t result=original();
    Identity after;Sample s;
    const bool good=eligible&&e.valid(c.epoch)&&identity(m,g,child,after)&&before==after&&previewSample(m,after,s);
    if(good)e.previewQualified.fetch_add(1,std::memory_order_relaxed);
    e.finish(c,good?&s:nullptr);return result;
}
template<class Original> uintptr_t finalizer(Engine& e,const Memory& m,const Game& g,uintptr_t child,Original original) {
    e.finalizerCalls.fetch_add(1,std::memory_order_relaxed);
    const uint32_t epoch=e.current();Identity before;const bool eligible=epoch&&identity(m,g,child,before);
    const uintptr_t result=original();Sample h,p;
    // Consume on every finalizer, including inactive/changed/unsupported rosters.
    const bool complete=e.take(epoch,child,h,p);
    Identity after;Sample hc,pc;Pose output;bool outputRead=false;
    // The game's finalizer is a strict first maximum in roster order, so child+0xB0 is the head's pose when the head entry comes first and the preview's when the preview does: either is accepted.
    FinalWhy why=FinalWhy::kApplied;bool good=false;
    if(!eligible){}
    else if(!complete)why=FinalWhy::kIncomplete;
    else if(!(identity(m,g,child,after)&&before==after&&after==h.id&&after==p.id))why=FinalWhy::kIdentity;
    else if(!(hmdSample(m,after,hc)&&previewSample(m,after,pc)&&equal(h.primary,hc.primary)&&equal(h.local,hc.local)&&
              equal(h.base,hc.base)&&equal(p.primary,pc.primary)))why=FinalWhy::kSamples;
    else if(!((outputRead=get(m,child+0xB0,output))&&finite(output)))why=FinalWhy::kOutputRead;
    else if(!(equal(output,h.primary)||equal(output,p.primary)))why=FinalWhy::kOutputMismatch;
    else good=true;
    if(good) {
        // The preview pose is the stable exterior camera. Composing the local
        // headset onto it makes the ship follow the head. The 13:48 flight's
        // scene eyes were this pose, and a second kind-5 pair stayed at the
        // origin with the headset's orientation: the head-locked menu.
        const Pose changed=p.primary;
        const bool finiteChanged=finite(changed);
        const bool wrote=finiteChanged&&e.commit(epoch,[&]{
            return m.write(m.context,after.hmd+0x50,&changed,sizeof(changed))&&
                m.write(m.context,child+0xB0,&changed,sizeof(changed));
        });
        if(wrote) {
            e.publishEye(epoch,changed,h.primary);
            e.applied.fetch_add(1,std::memory_order_relaxed);
            e.finalSeen(m,before,FinalWhy::kApplied,&output,&h.primary,&p.primary);
            return result;
        }
        why=FinalWhy::kNotCommitted;
    }
    if(eligible) {
        e.refused.fetch_add(1,std::memory_order_relaxed);
        e.finalSeen(m,before,why,outputRead?&output:nullptr,complete?&h.primary:nullptr,complete?&p.primary:nullptr);
    }
    return result;
}
// The eye-camera producer multiplies this head by the per-eye offset and
// stores the product at the kind-5 camera's +0x50, which the view-constant
// refresh loads into cb1 row 275. Replace a head that is still the raw HMD
// primary, and a head sitting on the tracking origin: the ship-menu cameras
// on the 13:48 frame were a stereo pair there, 36 m from the preview. A pose
// tens of metres from both is another camera and is left alone.
inline bool applyEyeHead(Engine& e,const Memory& m,uintptr_t pose) noexcept {
    EyeSnapshot snapshot;
    if(!e.loadEye(snapshot))return false;
    Pose current;
    if(!get(m,pose,current)||!finite(current))return false;
    const float dx=current.v[12]-snapshot.raw[0],dy=current.v[13]-snapshot.raw[1],dz=current.v[14]-snapshot.raw[2];
    const float ox=current.v[12],oy=current.v[13],oz=current.v[14];
    if(dx*dx+dy*dy+dz*dz>0.25f&&ox*ox+oy*oy+oz*oz>1.f)return false;
    return e.eyeSnapshotValid(snapshot)&&m.write(m.context,pose,&snapshot.composed,sizeof(snapshot.composed));
}
}} // namespace edvr::previewcam
