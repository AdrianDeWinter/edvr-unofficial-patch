#include "../../src/d3d11/native_perf_history.h"
#include "../../src/d3d11/native_benchmark_collector.h"
#include "../../src/d3d11/frame_ticks.h"
#include "../../src/d3d11/xess_fg_presented.h"
#include "../../src/common/perf_presented.h"
#include "../../src/common/fps_overlay_mode.h"
#include "../../src/d3d11/menu_flat_layout.h"
#include "../../src/d3d11/menu_policy.h"
#include <vector>
#include <cstdio>
#include <cstring>
#include <string>
#include <cmath>
#include <cwchar>
#include <limits>

using namespace edvr;
namespace {
unsigned checks=0, failures=0;
void check(bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",name);}}
bool approx(double a,double b){return std::fabs(a-b)<1e-8;}
NativeTimingSnapshot cpu(uint64_t seq,uint64_t at) {
    NativeTimingSnapshot s{};s.active=true;s.generation=7;s.firstSequence=1;s.sequence=seq;
    s.capturedAtMs=at;s.waitMs=4;s.predictedPeriodMs=11;s.haveCpu=true;
    s.applicationMs=3;s.applicationValid=true;
    s.cpu={sizeof(s.cpu),EDVR_NATIVE_TIMING_VERSION_5,seq};s.cpu.submitMs[0]=1;s.cpu.submitMs[1]=2;s.cpu.baseDisplayHz=90;
    s.cpu.callerWorkMs=14.4;s.cpu.callerWorkValid=1;   // the caller work per cycle: the monitor's CPU on a version 5 frame
    return s;
}
GpuFrameSnapshot gpu(uint64_t seq,uint64_t at,uint64_t age=0,double ms=5) {
    GpuFrameSnapshot s{};s.enabled=s.haveResult=true;s.capturedAtMs=at;
    s.result.sequence=seq;s.result.ageMs=age;s.result.outerMs=ms;
    s.result.source=GpuSpanSource::ApplicationRender;s.result.reason=GpuSpanReason::Valid;return s;
}
void device(NativeTimingSnapshot& s,uint64_t seq,uint64_t at,uint32_t status=EdvrNativeGpuValid) {
    s.haveDeviceGpu=true;s.deviceGpu={sizeof(s.deviceGpu),EDVR_NATIVE_TIMING_VERSION_3,seq,at,status,{1,2},{3,4}};
}
void averages() {
    NativePerfHistory h;auto t=cpu(1,10000);auto g=gpu(1,10000);device(t,1,10000);
    h.observe(true,t,g,10000);
    for(int i=0;i<100;++i)h.observe(true,t,g,10001);
    check(h.submit(10001,200).count==1&&h.producer(10001,200).count==1&&h.transfer(10001,200).count==1,"duplicates never weight any stream");
    check(approx(h.submit(10001,200).meanMs,3)&&approx(h.wait(10001,200).meanMs,4),"CPU submit and wait remain distinct");
    check(approx(h.producer(10001,200).meanMs,5)&&approx(h.transfer(10001,200).meanMs,3)&&approx(h.compose(10001,200).meanMs,7),"producer, XR copy and XR composition remain independent");
    t=cpu(2,10100);t.cpu.submitMs[0]=7;device(t,2,10100);h.observe(true,t,g,10100);
    check(h.submit(10100,200).count==2&&approx(h.submit(10100,200).meanMs,6)&&h.producer(10100,200).count==1,"asynchronous sources keep separate sample counts");
    check(h.submit(10201,200).count==1&&approx(h.submit(10201,200).meanMs,9)&&!h.producer(10201,200).count,"200ms window ages each source without fallback");
    check(!h.submit(12101,10000).count&&!h.predictedPeriod(12101),"stale current source hides older averages and prediction");
    t=cpu(3,12200);t.cpu.submitMs[0]=t.cpu.submitMs[1]=t.waitMs=0;device(t,3,12200);for(double& x:t.deviceGpu.transferMs)x=0;for(double& x:t.deviceGpu.composeMs)x=0;
    h.observe(true,t,gpu(3,12200,0,0),12200);
    check(h.submit(12200,200).count==1&&h.submit(12200,200).meanMs==0&&h.wait(12200,200).count==1,"measured zero CPU durations count");
    check(h.producer(12200,200).count==1&&h.producer(12200,200).meanMs==0&&h.transfer(12200,200).count==1&&h.compose(12200,200).meanMs==0,"measured zero GPU durations count");
}
void invalidation() {
    NativePerfHistory h;auto t=cpu(10,10000);auto g=gpu(10,10000);device(t,10,10000);h.observe(true,t,g,10000);
    auto partial=t;partial.cpu.callerWorkValid=0;partial.cpu.callerWorkMs=0;h.observe(true,partial,g,10000);
    check(!h.applicationCpu(10000,200).count&&h.applicationGpu(10000,200).count==1&&
          h.submit(10000,200).count==1,"incomplete CPU figure (no caller work) does not substitute submit wall or erase GPU");
    h.clear();auto oldDefinition=g;oldDefinition.result.source=GpuSpanSource::RenderToSubmit;
    h.observe(true,t,oldDefinition,10000);
    check(h.applicationCpu(10000,200).count==1&&!h.applicationGpu(10000,200).count,
          "legacy GPU interval cannot become an application benchmark measurement");
    h.clear();h.observe(true,t,g,10000);
    auto bad=t;bad.invalid=true;bad.sequence=bad.cpu.sequence=0;bad.haveDeviceGpu=false;h.observe(true,bad,{},10000);
    check(!h.submit(10000,200).count&&!h.producer(10000,200).count&&!h.transfer(10000,200).count&&!h.predictedPeriod(10000),"recenter clears all histories and prediction");
    h.observe(true,t,g,10000);check(!h.submit(10000,200).count&&!h.producer(10000,200).count&&!h.transfer(10000,200).count,"cleared snapshots cannot replay even when invalidation lost their identity");
    t=cpu(11,10010);device(t,11,10010);g=gpu(11,10010);h.observe(true,t,g,10010);
    check(h.submit(10010,200).count==1&&h.producer(10010,200).count==1,"later completion recovers");
    g.enabled=false;device(t,12,10010,EdvrNativeGpuDisabled);h.observe(true,t,g,10010);
    check(!h.producer(10010,200).count&&!h.transfer(10010,200).count&&h.submit(10010,200).count==1,"disabled GPU streams do not erase CPU");
    g.enabled=true;device(t,12,10010);h.observe(true,t,g,10010);check(!h.producer(10010,200).count&&!h.transfer(10010,200).count,"re-enable cannot replay consumed disabled sequences");
    device(t,13,10020,EdvrNativeGpuPending);h.observe(true,t,gpu(12,10020),10020);device(t,13,10020);h.observe(true,t,gpu(12,10020),10020);
    check(h.transfer(10020,200).count==1,"unconsumed pending device result resolves on same sequence");
    t.haveCpu=t.haveDeviceGpu=false;g={};h.observe(true,t,g,10020);
    check(!h.submit(10020,200).count&&!h.producer(10020,200).count&&!h.transfer(10020,200).count,"missing snapshots clear affected streams");
    t=cpu(14,10030);t.generation=8;t.firstSequence=14;h.observe(true,t,gpu(13,10030),10030);
    check(h.submit(10030,200).count==1&&!h.producer(10030,200).count,"new session rejects prior-session GPU");
    h.observe(false,t,gpu(14,10030),10030);check(!h.submit(10030,200).count&&!h.predictedPeriod(10030),"native off retires everything");
    h.observe(true,t,gpu(14,10030),10030);t.active=false;h.observe(true,t,g,10030);check(!h.submit(10030,200).count,"provider close retires history");
}
void agesAndValues() {
    NativePerfHistory h;auto t=cpu(1,10000);auto g=gpu(1,10000,150);device(t,1,9900);h.observe(true,t,g,10000);
    check(!h.producer(10051,200).count&&h.submit(10051,200).count==1&&h.transfer(10051,200).count==1,"producer age accrued before publication is included");
    float graph[4]{};check(h.graph(true,graph,4,11851)==0,"GPU history vanishes when effective newest sample is stale");
    for(unsigned kind=0;kind<4;++kind){
        h.clear();t=cpu(1,10000);g=gpu(1,10000);
        if(kind==0)g.capturedAtMs=0;if(kind==1)g.capturedAtMs=10001;if(kind==2)g.result.ageMs=UINT64_MAX;if(kind==3)g.result.ageMs=2001;
        h.observe(true,t,g,10000);check(!h.producer(10000,10000).count,"invalid/missing/future/overflow producer timestamp rejected");
    }
    for(unsigned kind=0;kind<6;++kind){
        h.clear();t=cpu(1,10000);device(t,1,10000);
        if(kind==0)t.capturedAtMs=0;if(kind==1)t.capturedAtMs=10001;if(kind==2)t.waitMs=NAN;
        if(kind==3)t.cpu.submitMs[0]=-1;if(kind==4)t.cpu.submitMs[0]=INFINITY;if(kind==5)t.cpu.submitMs[0]=600001;
        h.observe(true,t,gpu(1,10000),10000);check(!h.submit(10000,200).count&&h.producer(10000,200).count==1,"invalid CPU state rejects only CPU history");
    }
    for(unsigned kind=0;kind<5;++kind){
        h.clear();t=cpu(1,10000);device(t,1,10000);
        if(kind==0)t.deviceGpu.completedAtMs=0;if(kind==1)t.deviceGpu.completedAtMs=10001;
        if(kind==2)t.deviceGpu.transferMs[0]=NAN;if(kind==3)t.deviceGpu.composeMs[1]=-1;if(kind==4)t.deviceGpu.status=EdvrNativeGpuQueryFailure;
        h.observe(true,t,gpu(1,10000),10000);check(!h.transfer(10000,200).count&&h.submit(10000,200).count==1,"invalid device result does not erase CPU");
    }
    h.clear();t=cpu(1,10000);h.observe(true,t,gpu(1,10000),10000);
    t=cpu(2,10010);t.predictedPeriodMs=NAN;h.observe(true,t,gpu(2,10010),10010);
    check(h.submit(10010,200).count==2&&!h.predictedPeriod(10010),"invalid new prediction replaces old prediction without losing CPU samples");
    t=cpu(3,10020);t.predictedPeriodMs=0;h.observe(true,t,gpu(3,10020),10020);check(!h.predictedPeriod(10020),"zero period is unavailable");
}
void displayBase() {
    NativePerfHistory h;
    // The spec cases: 22.222 ms predicted against a 90 Hz base is a 45 Hz
    // display rate and THROTTLED; 11.111 ms is 90 Hz, no flag.
    auto t=cpu(1,10000);t.predictedPeriodMs=22.222;auto g=gpu(1,10000);
    h.observe(true,t,g,10000);
    const double predicted=h.predictedPeriod(10000);
    const double base=h.basePeriodMs(10000);
    check(predicted>22.0&&predicted<22.3&&base>11.0&&base<11.2&&
          NativePerfHistory::displayThrottled(predicted,base),
          "predicted 22.222 ms with base 90 Hz reads a 45 Hz display and THROTTLED");
    t=cpu(2,10010);t.predictedPeriodMs=11.111;h.observe(true,t,gpu(2,10010),10010);
    check(!NativePerfHistory::displayThrottled(h.predictedPeriod(10010),h.basePeriodMs(10010)),
          "predicted 11.111 ms with base 90 Hz shows no flag");
    // A version 3 host never sends baseDisplayHz: the session's first
    // predicted period stands in as the base.
    h.clear();t=cpu(3,10020);t.cpu.version=EDVR_NATIVE_TIMING_VERSION_3;t.cpu.baseDisplayHz=0;t.predictedPeriodMs=11.111;
    h.observe(true,t,gpu(3,10020),10020);
    t=cpu(4,10030);t.cpu.version=EDVR_NATIVE_TIMING_VERSION_3;t.cpu.baseDisplayHz=0;t.predictedPeriodMs=22.222;
    h.observe(true,t,gpu(4,10030),10030);
    check(NativePerfHistory::displayThrottled(h.predictedPeriod(10030),h.basePeriodMs(10030)),
          "version 3 host falls back to the session's first predicted period");
    // The published base is ring-fresh like the prediction: past 2 s the
    // first-period fallback answers, and a new session resets both.
    h.clear();t=cpu(5,10040);t.predictedPeriodMs=12;h.observe(true,t,gpu(5,10040),10040);
    check(h.basePeriodMs(10040)>11.0&&h.basePeriodMs(10040)<11.2&&approx(h.basePeriodMs(12041),12),
          "stale published base falls back to the first predicted period");
    t=cpu(6,10050);t.generation=9;t.firstSequence=6;t.predictedPeriodMs=17;
    h.observe(true,t,gpu(6,10050),10050);
    check(NativePerfHistory::displayThrottled(h.predictedPeriod(10050),h.basePeriodMs(10050)),
          "new session resets the base to the new session's prediction");
}
// The monitor's CPU figure (NativePerfHistory::cpuFigure, the settlement
// governor's rule): on a version 5 frame the caller work per cycle -- the
// game's thread per frame, which on the 2026-09-23 flights read 11.7-12.7 ms
// while the pre-submit application time beside it read under 10 -- labelled
// as the plain CPU (cpuSource CallerWork); a version 5 frame without valid
// caller work has no figure, never the application time beside it. An older
// runtime (version 3 or 4: no caller work) falls back to the application
// time, labelled pre-submit, and no average ever holds both.
void callerWorkCrossing() {
    NativePerfHistory h;
    check(h.cpuSource()==NativeCpuSource::None,"no frame yet: the CPU figure has no source");
    auto t=cpu(1,10000);   // version 5: caller work 14.4 ms beside app 3 ms
    h.observe(true,t,gpu(1,10000),10000);
    check(t.cpu.version==EDVR_NATIVE_TIMING_VERSION_5&&t.cpu.callerWorkValid==1&&
          h.applicationCpu(10000,200).count==1&&approx(h.applicationCpu(10000,200).meanMs,14.4)&&
          h.cpuSource()==NativeCpuSource::CallerWork,
          "version 5: the monitor's CPU is the caller work per cycle (14.4), not the pre-submit app time (3)");
    float graph[2]{};
    check(h.graph(false,graph,2,10000)==1&&graph[0]==14.4f,"version 5: the CPU graph plots the caller work");
    check(h.basePeriodMs(10000)>11.0&&h.basePeriodMs(10000)<11.2,
          "version 5: the base display rate still reads (version 4 and later)");
    t=cpu(2,10010);t.cpu.callerWorkValid=0;t.cpu.callerWorkMs=0;
    h.observe(true,t,gpu(2,10010),10010);
    check(!h.applicationCpu(10010,200).count&&h.cpuSource()==NativeCpuSource::CallerWork,
          "version 5 without valid caller work: no CPU figure, never the app time beside it");
    t=cpu(3,10020);t.cpu.callerWorkMs=12.2;h.observe(true,t,gpu(3,10020),10020);
    check(h.applicationCpu(10020,200).count==1&&approx(h.applicationCpu(10020,200).meanMs,12.2),
          "version 5: the next frame with caller work is the figure again");
    // An older runtime (a new session, version 4, no caller work): the
    // pre-submit application time stands in, and says so.
    t=cpu(4,10030);t.generation=8;t.firstSequence=4;
    t.cpu.version=EDVR_NATIVE_TIMING_VERSION_4;t.cpu.size=EDVR_NATIVE_TIMING_FRAME_SIZE_4;
    t.cpu.callerWorkMs=0;t.cpu.callerWorkValid=0;
    h.observe(true,t,gpu(4,10030),10030);
    check(h.applicationCpu(10030,200).count==1&&approx(h.applicationCpu(10030,200).meanMs,3)&&
          h.cpuSource()==NativeCpuSource::PreSubmit&&h.basePeriodMs(10030)>11.0&&h.basePeriodMs(10030)<11.2,
          "version 4 (no caller work): the CPU falls back to applicationMs, labelled pre-submit; the base rate reads");
    t=cpu(5,10040);t.generation=8;t.firstSequence=4;t.cpu.version=EDVR_NATIVE_TIMING_VERSION_4;
    t.cpu.size=EDVR_NATIVE_TIMING_FRAME_SIZE_4;t.cpu.callerWorkMs=0;t.cpu.callerWorkValid=0;t.applicationValid=false;
    h.observe(true,t,gpu(5,10040),10040);
    check(!h.applicationCpu(10040,200).count,"version 4 without app time: no CPU figure");
    // Never one average of both: a version 5 frame after version 4 samples in
    // the same session starts the stream over.
    t=cpu(6,10050);t.generation=8;t.firstSequence=4;t.applicationMs=3;
    h.observe(true,t,gpu(6,10050),10050);
    auto older=cpu(7,10060);older.generation=8;older.firstSequence=4;older.cpu.version=EDVR_NATIVE_TIMING_VERSION_4;
    older.cpu.size=EDVR_NATIVE_TIMING_FRAME_SIZE_4;older.cpu.callerWorkMs=0;older.cpu.callerWorkValid=0;
    h.observe(true,older,gpu(7,10060),10060);
    check(h.applicationCpu(10060,200).count==1&&approx(h.applicationCpu(10060,200).meanMs,3)&&
          h.cpuSource()==NativeCpuSource::PreSubmit,"a change of figure starts the average over");
    // The rule itself, frame by frame.
    NativeTimingSnapshot f=cpu(8,10070);
    NativeCpuFigure x=NativePerfHistory::cpuFigure(f);
    check(x.valid&&x.source==NativeCpuSource::CallerWork&&approx(x.ms,14.4),"rule: version 5 with caller work");
    f.cpu.callerWorkValid=0;x=NativePerfHistory::cpuFigure(f);
    check(!x.valid&&x.source==NativeCpuSource::CallerWork,"rule: version 5 without it has no figure");
    f=cpu(9,10080);f.cpu.callerWorkMs=NAN;x=NativePerfHistory::cpuFigure(f);
    check(!x.valid,"rule: a NaN caller work is no figure");
    f=cpu(10,10090);f.cpu.version=EDVR_NATIVE_TIMING_VERSION_3;f.cpu.baseDisplayHz=0;x=NativePerfHistory::cpuFigure(f);
    check(x.valid&&x.source==NativeCpuSource::PreSubmit&&approx(x.ms,3),"rule: version 3 falls back to applicationMs");
    f.haveCpu=false;x=NativePerfHistory::cpuFigure(f);
    check(!x.valid&&x.source==NativeCpuSource::None,"rule: no CPU frame, no figure");
    h.clear();
    check(h.cpuSource()==NativeCpuSource::None,"clear forgets the figure's source");
}
void graphs() {
    NativePerfHistory h;for(uint64_t i=1;i<=950;++i){auto t=cpu(i,10000+i);t.cpu.submitMs[0]=double(i);t.cpu.submitMs[1]=0;t.cpu.callerWorkMs=double(i);h.observe(true,t,gpu(i,10000+i,0,double(i)+100),10000+i);}
    check(h.submit(10950,10000).count==900&&approx(h.submit(10950,10000).meanMs,500.5),"ring wraps without overweighting or losing ordering");
    float values[5]={-99,-99,-99,-99,-99};check(h.graph(false,values,3,10950)==3&&values[0]==948&&values[1]==949&&values[2]==950&&values[3]==-99,"CPU graph uses newest bounded tail oldest-first");
    check(h.graph(true,values,3,10950)==3&&values[0]==1048&&values[2]==1050,"producer graph uses its own observations");
    check(h.graph(true,nullptr,3,10950)==0&&h.graph(true,values,0,10950)==0&&h.graph(true,values,-1,10950)==0,"invalid graph buffers/capacities rejected");
    check(!h.graph(true,values,3,12951),"stale graph never continues to present old results as live");
    check(!h.graph(false,values,3,9000),"time reversal never exposes future observations");
}

NativeBenchmarkObservation benchmarkSample(uint64_t seq, uint64_t at,
                                           double cpuMs, double gpuMs) {
    NativeBenchmarkObservation s{};
    s.scope = 1;
    s.cpuSequence = seq; s.cpuAtMs = at; s.cpuMs = cpuMs; s.cpuValid = true;
    s.gpuSequence = seq; s.gpuAtMs = at; s.gpuMs = gpuMs; s.gpuValid = true;
    return s;
}

void benchmarkAdmission() {
    NativeBenchmarkCollector c;
    NativeBenchmarkObservation tick{};tick.scope=1;
    c.observe(tick,1000);c.observe(tick,3000);
    check(c.sampling()&&c.collecting(),"metadata-only monitor ticks start a usable sampling window");
    const auto onlyGpu=[](uint64_t seq,uint64_t at,double value) {
        auto s=benchmarkSample(seq,at,0,value);s.cpuSequence=s.cpuAtMs=0;return s;
    };
    const auto onlyCpu=[](uint64_t seq,uint64_t at,double value) {
        auto s=benchmarkSample(seq,at,value,0);s.gpuSequence=s.gpuAtMs=0;return s;
    };
    c.observe(onlyGpu(1,3010,10),3010);
    c.observe(onlyGpu(257,3011,20),3011); // Same hash bucket.
    c.observe(onlyCpu(1,3001,1),3012); // Removing it must not hide 257.
    c.observe(onlyCpu(257,3002,2),3013);
    c.observe(onlyGpu(3,3014,999),3014);
    c.observe(onlyCpu(3,2999,999),3015); // Pre-warmup, completed late.
    auto invalid=onlyCpu(4,3020,NAN);invalid.cpuValid=false;c.observe(invalid,3020);
    c.observe(onlyGpu(4,3021,0),3021); // Valid measured zero remains valid.
    c.observe({},33000);
    check(c.sampling()&&!c.collecting(),"drain accepts completions without claiming active collection");
    c.observe(onlyGpu(5,33500,30),33500); // Completion in drain before CPU arrival.
    c.observe(onlyCpu(5,32999,3),33501);
    c.observe(onlyCpu(6,33000,999),33502); // Half-open sample interval.
    c.observe(onlyCpu(7,32998,4),35000);
    c.observe(onlyGpu(7,34999,40),35000);
    check(!c.ready(),"deadline batch drains before the final monitor tick");
    c.observe({},35000);
    NativeBenchmarkReport r{};
    check(c.takeReport(&r)&&!r.aborted&&r.cpu.valid==4&&r.cpu.invalid==1&&
          r.gpu.valid==5&&r.gpu.missing==0,"colliding and drain orphans retain their own admitted frames");
    check(approx(r.cpu.p50,2)&&approx(r.cpu.p99,4)&&approx(r.gpu.p50,20)&&
          approx(r.gpu.p99,40),"warmup and boundary frames cannot bias either distribution");
    check(r.startedAtMs==3000&&r.sampleEndedAtMs==33000&&r.drainMs==2000,
          "sample and drain durations are distinct");
    c.observe(tick,40000);c.observe(tick,42000);
    c.observe(onlyCpu(100,42001,7),42001);tick.scope=2;c.observe(tick,42500);
    check(c.takeReport(&r)&&r.aborted&&r.sampleEndedAtMs-r.startedAtMs==500&&r.drainMs==0,
          "partial reports state actual duration, not planned thirty seconds");
}
void benchmarkWindows() {
    NativeBenchmarkCollector c;
    c.observe(benchmarkSample(1, 0, 1, 2), 0);
    check(c.warming() && !c.ready(), "benchmark starts in warmup");
    c.observe(benchmarkSample(1, 1999, 1, 2), 1999);
    check(c.warming(), "two-second warmup excludes early samples");
    c.observe(benchmarkSample(1, 2000, 1, 2), 2000);
    c.observe(benchmarkSample(2, 2010, 9, 6), 2010);
    c.observe(benchmarkSample(2, 2011, 9, 6), 2011);
    check(c.sampling(), "window samples after warmup");
    c.observe({}, 32000);
    check(!c.ready(), "window enters a bounded GPU completion drain");
    c.observe({}, 34000);
    check(c.ready(), "elapsed window and drain produce one report");
    NativeBenchmarkReport report{};
    check(c.takeReport(&report) && report.complete && !report.aborted,
          "completed benchmark report is consumable");
    check(report.cpu.valid == 2 && report.cpu.stored == 2 && report.cpu.missing == 0 &&
          approx(report.cpu.p50, 1) && approx(report.cpu.p95, 9) && approx(report.cpu.p99, 9),
          "CPU distribution uses individual values and nearest-rank percentiles");
    check(report.gpu.valid == 2 && report.gpu.stored == 2 && report.gpu.missing == 0 &&
          approx(report.gpu.p50, 2) && approx(report.gpu.p95, 6),
          "GPU distribution remains independent from CPU");
    check(!c.ready() && !c.takeReport(&report), "report is emitted once and next window is idle");

    c.observe(benchmarkSample(10, 40000, 3, 4), 40000);
    c.observe(benchmarkSample(11, 40001, 5, 6), 40001);
    c.observe(benchmarkSample(200, 40002, 7, 8), 40002);
    c.observe(benchmarkSample(12, 42000, 7, 8), 42000);
    c.observe({}, 74000);
    check(c.ready() && c.takeReport(&report), "second recurring window completes");
    check(report.cpu.valid == 1 && report.gpu.valid == 1 && report.cpu.missing == 0,
          "a recurring window resets values without replaying old samples");

    c.observe(benchmarkSample(300, 80000, 1, 1), 80000);
    c.observe(benchmarkSample(300, 82000, 1, 1), 82000);
    NativeBenchmarkObservation invalid = benchmarkSample(301, 82001, 0, 0);
    invalid.cpuValid = false;
    c.observe(invalid, 82001);
    NativeBenchmarkObservation changed = benchmarkSample(1, 82002, 2, 2);
    changed.scope = 2;
    c.observe(changed, 82002);
    check(c.ready(), "scope change emits an aborted partial report");
    check(c.takeReport(&report) && report.aborted &&
          report.abortReason == kNativeBenchmarkScopeChanged,
          "scope change identifies why a window was aborted");
    c.observe(changed, 82003);
    check(c.warming() && !c.ready(), "scope change starts a new warmup");

    c.reset();
    c.observe(benchmarkSample(1, 0, 1, 1), 0);
    c.observe(benchmarkSample(1, 2000, 1, 1), 2000);
    NativeBenchmarkObservation cpuOnly = benchmarkSample(200, 2001, 2, 0);
    cpuOnly.gpuSequence = cpuOnly.gpuAtMs = 0;
    cpuOnly.gpuValid = false;
    c.observe(cpuOnly, 2001);
    c.observe({}, 32000);
    c.observe({}, 34000);
    check(c.takeReport(&report) && report.cpu.missing == 0 && report.gpu.missing == 1,
          "retirement marks a missing asynchronous GPU counterpart");

    c.reset();
    c.observe(benchmarkSample(1, 0, 1, 1), 0);
    c.observe(benchmarkSample(1, 2000, 1, 3), 2000);
    NativeBenchmarkObservation cpuTwo = benchmarkSample(2, 2001, 2, 0);
    cpuTwo.gpuSequence = cpuTwo.gpuAtMs = 0;
    cpuTwo.gpuValid = false;
    c.observe(cpuTwo, 2001);
    c.observe({}, 32000);
    // GPU completion order and completion timestamps are independent from
    // CPU admission. Both belong to admitted window frames and are allowed
    // through the bounded drain even when they arrive out of order or late.
    NativeBenchmarkObservation lateGpu = benchmarkSample(2, 1000, 0, 4);
    lateGpu.cpuSequence = lateGpu.cpuAtMs = 0;
    lateGpu.cpuValid = false;
    c.observe(lateGpu, 33000);
    NativeBenchmarkObservation oldGpu = benchmarkSample(1, 1001, 0, 3);
    oldGpu.cpuSequence = oldGpu.cpuAtMs = 0;
    oldGpu.cpuValid = false;
    c.observe(oldGpu, 33001);
    c.observe({}, 34000);
    check(c.takeReport(&report) && report.cpu.valid == 2 && report.gpu.valid == 2 &&
          report.gpu.missing == 0 && approx(report.gpu.p50, 3) && approx(report.gpu.p95, 4),
          "late out-of-order GPU completions attach to their own CPU frame");

    c.reset();
    c.observe(benchmarkSample(1, 0, 1, 1), 0);
    c.observe(benchmarkSample(1, 2000, 1, 1), 2000);
    c.noteDropped(true, 2);
    c.noteDropped(false, 3);
    c.observe({}, 32000);
    c.observe({}, 34000);
    check(c.takeReport(&report) && report.aborted &&
          report.abortReason == kNativeBenchmarkTransportLoss &&
          report.cpu.invalid == 2 && report.gpu.invalid == 3,
          "completion queue overwrite is reported as invalid coverage");

    c.reset();
    c.observe(benchmarkSample(1, 0, 1, 1), 0);
    c.observe(benchmarkSample(1, 2000, 1, 1), 2000);
    c.observe({}, 1999);
    check(c.ready() && c.takeReport(&report) && report.aborted &&
          report.abortReason == kNativeBenchmarkClockReversed,
          "clock reversal aborts an active benchmark window");

    c.reset();
    c.observe(benchmarkSample(1, 0, 1, 1), 0);
    c.observe(benchmarkSample(1, 2000, 1, 1), 2000);
    for (unsigned i = 0; i <= NativeBenchmarkCollector::kCapacity; ++i) {
        c.observe(benchmarkSample(100 + i, 2001 + i, 1, 1), 2001 + i);
        if (c.ready()) break;
    }
    check(c.ready() && c.takeReport(&report) && report.aborted && report.overflow,
          "storage overflow aborts instead of silently truncating samples");
}

// ---- frame_ticks.h: where the Present hook's time went, per frame -------------------------------------
// The recorder is driven with explicit clock readings at one tick per microsecond, so every number below
// is derived by hand from the timeline, never measured.
constexpr int64_t kUs = 1000000;   // ticks a second: one tick a microsecond, so ticks/1000 is ms
// The summary's numbers are floats, so a tolerance a float's own rounding cannot break.
bool nearMs(double a, double b) { return std::fabs(a - b) < 1e-5; }

void frameTicksChain() {
    // Hook 1: entered at 1000; the game's own time before that is not a tick. 10 us of work before the real
    // Present (1000..1010), the real Present 1010..1060, then 30 + 10 us of work after it, then the
    // boundary: journal_watch 200 us (the slow one), fss_mode_latch 10 us, and the frame edge at 1500 inside the
    // menu, which has run 190 us since the last mark.
    FrameTicks t;
    t.enter(1000);
    t.markAt("present_pre", 1010);
    t.external(1060);
    t.markAt("gpu_frame_present", 1090);
    t.markAt("present_post", 1100);
    t.boundary(true);
    t.markAt("journal_watch", 1300);
    t.markAt("fss_mode_latch", 1310);
    FrameTickSummary a = t.cut("menu_tick", 1500, kUs);
    check(nearMs(a.hookMs, 0.45) && nearMs(a.boundaryMs, 0.40) && nearMs(a.realMs, 0.05) &&
          a.marks == 6 && a.hooks == 1,
          "frame ticks: the hook's ticks, the boundary's part of them and the real Present are kept apart");
    check(a.top[0].name && !std::strcmp(a.top[0].name, "journal_watch") && nearMs(a.top[0].ms, 0.20) &&
          a.top[1].name && !std::strcmp(a.top[1].name, "menu_tick") && nearMs(a.top[1].ms, 0.19) &&
          a.top[2].name && !std::strcmp(a.top[2].name, "gpu_frame_present") && nearMs(a.top[2].ms, 0.03),
          "frame ticks: the three slowest, slowest first, by name -- the real Present never among them");

    // The chain runs on past the cut: the rest of the menu (100 us), the rest of the boundary (100 us), then
    // out of the boundary for the render callback (20 us) and the trace note (10 us). Hook 1 returns at 1730.
    t.markAt("menu", 1600);
    t.markAt("boundary_rest", 1700);
    t.boundary(false);
    t.markAt("render_callback", 1720);
    t.markAt("timing_note", 1730);
    // The game runs 1730..5000. Hook 2, entered at 5000: 5 us before the real Present (5005), the real Present
    // (5005..5055), 25 + 20 us after it, 10 us of journal, and the frame edge at 5400 in the menu.
    t.enter(5000);
    t.markAt("present_pre", 5005);
    t.external(5055);
    t.markAt("gpu_frame_present", 5080);
    t.markAt("present_post", 5100);
    t.boundary(true);
    t.markAt("journal_watch", 5110);
    FrameTickSummary b = t.cut("menu_tick", 5400, kUs);
    // Ticks: hook 1's tail 100+100+20+10 = 230 us, hook 2's head 5+25+20+10+290 = 350 us; boundary: the
    // tail's menu and boundary_rest (200), the head's journal_watch and menu_tick (300).
    check(nearMs(b.hookMs, 0.58) && nearMs(b.boundaryMs, 0.50) && nearMs(b.realMs, 0.05) && b.hooks == 1 && b.marks == 9,
          "frame ticks: a frame holds the previous hook's tail and this hook's head, cut at the frame edge");
    // THE PARTITION: the frame is 1500..5400 = 3.90 ms and the game's own stretch between the two hooks
    // (1730..5000) is 3.27 ms, so the hook's ticks, the real Present and the game add up to the frame.
    check(nearMs(double(b.hookMs) + double(b.realMs) + 3.27, 3.90),
          "frame ticks: ticks + real Present + the game between hooks are the whole frame");
    check(b.top[0].name && !std::strcmp(b.top[0].name, "menu_tick") && nearMs(b.top[0].ms, 0.29) &&
          !std::strcmp(b.top[1].name, "menu") && nearMs(b.top[1].ms, 0.10),
          "frame ticks: the menu's tail after the edge lands in the next frame, its head before it in this one");
    // A frame's summary is consumed: the next starts empty.
    FrameTickSummary empty = t.cut("menu_tick", 5400, kUs);
    check(empty.marks == 0 && empty.hooks == 0 && empty.hookMs == 0.0f && !empty.top[0].name,
          "frame ticks: a cut leaves nothing behind for the next frame");
}

void frameTicksTopThree() {
    FrameTicks t;
    t.enter(100);
    // Durations 5, 9, 9, 3, 12, 1, 9: the top three are 12, then the FIRST 9, then the second 9 -- a tie keeps
    // the earlier tick ahead of the later, and 3 and 1 and the last 9 never enter.
    const char* names[] = {"a5", "b9", "c9", "d3", "e12", "f1", "g9"};
    const int64_t ms[] = {5, 9, 9, 3, 12, 1, 9};
    int64_t at = 100;
    for (int i = 0; i < 7; ++i) { at += ms[i] * 1000; t.markAt(names[i], at); }
    FrameTickSummary s = t.cut("end", at + 1, kUs);
    check(s.top[0].name && !std::strcmp(s.top[0].name, "e12") && s.top[1].name && !std::strcmp(s.top[1].name, "b9") &&
          s.top[2].name && !std::strcmp(s.top[2].name, "c9") && nearMs(s.top[0].ms, 12.0) && nearMs(s.top[2].ms, 9.0),
          "frame ticks: the slowest three of many, ties keeping the earlier");

    // Fewer than three ticks: the unused slots stay null, so a printer cannot invent a name.
    FrameTicks few;
    few.enter(0 + 10);
    few.markAt("only", 60);
    FrameTickSummary f = few.cut("edge", 60, kUs);
    check(f.marks == 1 && f.top[0].name && !std::strcmp(f.top[0].name, "only") && !f.top[1].name && !f.top[2].name,
          "frame ticks: unused slots are null");
}

void frameTicksOddClocks() {
    FrameTicks t;
    // A mark before any chain starts one and records nothing: the time since an unknown start is not a tick.
    t.markAt("before_enter", 500);
    t.enter(1000);
    t.markAt("late", 1000);   // no time passed: nothing recorded
    t.markAt("back", 900);    // the clock stepped back: nothing recorded, and the chain does not move back
    t.markAt("ok", 1040);
    t.external(1030);         // the real Present that ends before the chain's last mark: ignored
    FrameTickSummary s = t.cut("edge", 1040, kUs);
    check(s.marks == 1 && nearMs(s.hookMs, 0.04) && nearMs(s.realMs, 0.0) && s.top[0].name && !std::strcmp(s.top[0].name, "ok"),
          "frame ticks: a repeated, reversed or pre-chain reading records nothing");
    // No clock frequency: the numbers read 0, the names stay.
    FrameTicks noRate;
    noRate.enter(10);
    noRate.markAt("x", 20);
    FrameTickSummary z = noRate.cut("edge", 30, 0);
    check(z.hookMs == 0.0f && z.marks == 2 && z.top[0].name,
          "frame ticks: an unknown clock rate reports zero milliseconds, not garbage");
}

void frameTicksUncut() {
    // A profile that never cuts (the flat one) must not let the sums grow for the whole session: after eight
    // hooks without a cut the pile is dropped, so a later cut holds only what came since.
    FrameTicks t;
    for (int i = 1; i <= 9; ++i) {
        t.enter(i * 1000);
        t.markAt("x", i * 1000 + i * 10);
    }
    FrameTickSummary s = t.cut("edge", 9 * 1000 + 100, kUs);
    check(s.hooks == 1 && s.marks == 2 && nearMs(s.hookMs, 0.10),
          "frame ticks: eight hooks without a cut are dropped, so nothing piles up unbounded");
    // With cuts in between, nothing is ever dropped.
    FrameTicks steady;
    bool kept = true;
    for (int i = 1; i <= 20; ++i) {
        steady.enter(i * 1000);
        steady.markAt("x", i * 1000 + 100);
        const FrameTickSummary r = steady.cut("edge", i * 1000 + 100, kUs);
        kept = kept && r.hooks == 1 && r.marks == 1 && nearMs(r.hookMs, 0.10);
    }
    check(kept, "frame ticks: a cut every hook loses nothing");
}

// A frame's LONG FRAME clause, as the log carries it.
void frameShareText() {
    FrameTickSummary s;
    s.hookMs = 0.45f; s.boundaryMs = 0.40f; s.realMs = 0.05f; s.marks = 7; s.hooks = 1;
    s.top[0] = {"journal_watch", 0.20f}; s.top[1] = {"menu_tick", 0.19f}; s.top[2] = {"gpu_frame_present", 0.02f};
    char text[512];
    size_t n = formatEdvrShare(text, sizeof(text), 26.4, s, 0.10, true);
    check(std::string(text) ==
          "ED-UP in this frame: 0.45 ms in the Present hook (frame boundary 0.40 ms), 0.05 ms in the real Present, "
          "draw hooks ~0.10 ms (sampled this frame), 25.90 ms outside the hook; "
          "slowest ED-UP ticks: journal_watch=0.20 ms, menu_tick=0.19 ms, gpu_frame_present=0.02 ms;" &&
          n == std::strlen(text),
          "LONG FRAME share: the hook's ms, the boundary's, the real Present's, the draw hooks', the rest, and three named ticks");
    formatEdvrShare(text, sizeof(text), 26.4, s, 0.10, false);
    check(std::strstr(text, "draw hooks ~0.10 ms (held over)") != nullptr,
          "LONG FRAME share: a draw-hook figure not measured this frame says it is held over");

    // Engine motion's clause (engine_motion_cpu.h): exact for the frame, at the end of the share, the
    // rest of the text unchanged. No calls says so, never 0.00 ms; an unmeasured frame has no clause.
    const std::string plain = [&] {
        formatEdvrShare(text, sizeof(text), 26.4, s, 0.10, true);
        return std::string(text);
    }();
    EngineMotionFrame em;
    em.measured = true;
    em.renderMs = 0.31;
    em.calls = 118;
    n = formatEdvrShare(text, sizeof(text), 26.4, s, 0.10, true, em);
    check(std::string(text) == plain + " engine motion 0.31 ms;" && n == std::strlen(text),
          "LONG FRAME share: engine motion's clause is this frame's render-thread ms, after the ticks");
    em.calls = 0;
    em.renderMs = 0.0;
    formatEdvrShare(text, sizeof(text), 26.4, s, 0.10, true, em);
    check(std::string(text) == plain + " engine motion none this frame;" && std::strstr(text, "0.00 ms") == nullptr,
          "LONG FRAME share: engine motion's code that never ran on this thread says none, not 0.00 ms");
    em.calls = 1;
    formatEdvrShare(text, sizeof(text), 26.4, s, 0.10, true, em);
    check(std::string(text) == plain + " engine motion 0.00 ms;",
          "LONG FRAME share: engine motion that ran once and rounds to nothing is 0.00 ms, not none");
    EngineMotionFrame off;
    formatEdvrShare(text, sizeof(text), 26.4, s, 0.10, true, off);
    check(std::string(text) == plain, "LONG FRAME share: an unmeasured frame (the priming one) has no engine motion clause");
    FrameTickSummary none;
    formatEdvrShare(text, sizeof(text), 12.0, none, 0.0, false);
    check(std::strstr(text, "slowest ED-UP ticks: none recorded;") != nullptr &&
          std::strstr(text, "12.00 ms outside the hook") != nullptr,
          "LONG FRAME share: a frame the chain never ran in says so instead of naming nothing");
    FrameTickSummary over;
    over.hookMs = 3.0f; over.realMs = 2.0f;
    formatEdvrShare(text, sizeof(text), 4.0, over, 0.0, true);
    check(std::strstr(text, "0.00 ms outside the hook") != nullptr,
          "LONG FRAME share: the rest of the frame never reads negative");
    char tiny[24];
    n = formatEdvrShare(tiny, sizeof(tiny), 26.4, s, 0.10, true);
    check(n == std::strlen(tiny) && n < sizeof(tiny), "LONG FRAME share: a short buffer is cut, never overrun");
}

// The whole native line, at its worst, must fit what Log::note keeps (about 1166 characters of message): the
// tail -- runtime sequence and game work -- is the key that lays this line against native_long_cycle.
void nativeLongFrameFits() {
    FrameTickSummary worst;
    worst.hookMs = worst.boundaryMs = worst.realMs = 4999.99f;
    worst.marks = 60; worst.hooks = 2;
    worst.top[0] = {"exposure_reclaim_tick", 4999.99f};
    worst.top[1] = {"engine_velocity_clock", 4999.99f};
    worst.top[2] = {"draw_census_boundary", 4999.99f};
    EngineMotionFrame motion;   // the widest clause: every digit at its largest
    motion.measured = true;
    motion.renderMs = 4999.99;
    motion.calls = 18446744073709551615ull;
    // Both draw-hook wordings, each with the widest engine motion clause: whichever is longer must fit.
    for (int fresh = 0; fresh < 2; ++fresh) {
        char share[400];
        formatEdvrShare(share, sizeof(share), 4999.9, worst, 4999.99, fresh != 0, motion);
        NativeLongFrame line;
        line.frameMs = 4999.9;
        const std::string reference(79, 'r'), events(199, 'e'), stamp(219, 's'), gameWork(47, 'g');
        line.reference = reference.c_str();
        line.textures = line.buffers = line.shaders = 4294967295u;
        line.creationMb = 123456.7;
        line.share = share;
        line.events = events.c_str();
        line.stamp = stamp.c_str();
        line.sequence = 18446744073709551615ull;
        line.gameWork = gameWork.c_str();
        char text[1400];
        const size_t n = formatNativeLongFrame(text, sizeof(text), line);
        std::printf("native_perf_history_test: the LONG FRAME line at its worst, draw hooks %s, is %zu characters "
                    "(the log keeps about 1166; the gate is 1160)\n", fresh ? "sampled" : "held over", n);
        check(std::strstr(share, " engine motion 4999.99 ms;") != nullptr,
              "LONG FRAME line at its worst: the engine motion clause is in it, whole");
        const std::string tail = std::string("runtime sequence 18446744073709551615, game work ") + gameWork + ".";
        check(n == std::strlen(text) && n < 1160 && std::string(text).size() >= tail.size() &&
              std::string(text).compare(std::string(text).size() - tail.size(), tail.size(), tail) == 0,
              "LONG FRAME line at its worst fits the log's line, sequence and game work intact");
    }
    char text[1400];

    // And the ordinary line reads as the old one did, the share added before the events.
    NativeLongFrame plain;
    plain.frameMs = 26.2;
    plain.reference = "runtime predicted period 11.1 ms";
    plain.textures = 1; plain.buffers = 22; plain.shaders = 0; plain.creationMb = 64.6;
    plain.share = "ED-UP in this frame: X;";
    plain.events = "none";
    plain.stamp = " This is frame 34811; the flip timeline is not armed.";
    plain.sequence = 31151;
    plain.gameWork = "6.58 ms";
    formatNativeLongFrame(text, sizeof(text), plain);
    check(std::string(text) ==
          "monitor: LONG FRAME -- 26.2 ms between Presents (runtime predicted period 11.1 ms), no WaitGetPoses, CPU busy, "
          "compositor, reprojection, or door samples; game creations: 1 textures, 22 buffers, 0 shaders (64.6 MB); "
          "ED-UP in this frame: X; ED-UP events: none. This is frame 34811; the flip timeline is not armed. "
          "runtime sequence 31151, game work 6.58 ms.",
          "LONG FRAME line: the old text intact, ED-UP's share between the creations and the events");
}

// What one mark costs on this machine, said out loud so a slow clock is visible in the build log. Not an
// assertion: a loaded build machine would make any threshold a flake.
void frameTickCostNote() {
    FrameTicks t;
    t.enter(FrameTicks::now());
    const int kMarks = 200000;
    LARGE_INTEGER a{}, b{}, f{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < kMarks; ++i) t.mark("cost");
    QueryPerformanceCounter(&b);
    const double ns = double(b.QuadPart - a.QuadPart) * 1e9 / double(f.QuadPart) / kMarks;
    std::printf("native_perf_history_test: one frame-tick mark costs %.1f ns here (about 60 a frame)\n", ns);
    const FrameTickSummary s = t.cut("edge", FrameTicks::now(), f.QuadPart);
    check(s.marks > 0, "frame ticks: the live clock records marks");
}
// ---- the presented frame rate (xess_fg_presented.h, perf_presented.h) -------------------------------------------------------------------------------
std::string noteOf(fgpresented::Tally& t) {
    char note[240]="";
    return t.takeNote(note,sizeof(note))?std::string(note):std::string("<none>");
}
void presentedTally() {
    using fgpresented::Source;using fgpresented::Tally;
    uint64_t total=0;int64_t qpc=0;Source src=Source::None;
    {   // nothing sampled: no count at all, which is not a count of zero
        Tally t;
        check(!t.get(&total,&qpc,&src)&&!t.unavailable()&&noteOf(t)=="<none>","presented: a tally nobody fed has no count, is not unavailable and says nothing");
    }
    {   // XeSS-FG's own count per Present, summed: never rendered * (1 + generated)
        Tally t;
        const uint32_t perPresent[]={2,2,1,3,2,3};uint64_t want=0;int64_t at=100;
        for(uint32_t f:perPresent){t.onStatus(true,f,at);want+=f;at+=17;}
        check(t.get(&total,&qpc,&src)&&total==want&&total==13,"presented: the status's frames per Present are summed (13 here, not 6 Presents times a factor)");
        check(src==Source::StatusFrames&&qpc==at-17,"presented: the status is the source and its time is the last Present's");
        const std::string first=noteOf(t),second=noteOf(t);
        check(first=="perf: presented frames from XeSS-FG present status (framesPresented, summed per Present)"&&second=="<none>","presented: the source is logged once, in the words the log tool can find");
        check(!t.wantsStats(),"presented: while the status works, frame statistics are not read");
    }
    {   // a status of 0 or above the bound is not a count of one Present's frames; one odd sample in a status that works is dropped
        Tally t;t.onStatus(true,2,1);t.onStatus(true,0,2);t.onStatus(true,fgpresented::kMaxFramesPerPresent+1,3);t.onStatus(false,0,4);
        check(t.get(&total,&qpc,&src)&&total==2&&!t.wantsStats(),"presented: an odd status (0, too big, failed) adds nothing and does not end a status that works");
    }
    {   // the status call fails: after kProbePresents the statistics take over
        Tally t;
        for(unsigned i=0;i<fgpresented::kProbePresents-1;++i)t.onStatus(false,0,i);
        check(!t.wantsStats()&&!t.get(&total,&qpc,&src),"presented: a status that has failed fewer times than the probe is still waited on");
        t.onStatus(false,0,99);
        check(t.wantsStats()&&t.statusWhy()&&std::strstr(t.statusWhy(),"fails"),"presented: a status that never works is given up on after the probe, with its reason");
        t.onStats(true,false,1000,0,10);   // the baseline
        check(!t.get(&total,&qpc,&src),"presented: the first frame statistics are a baseline, not frames");
        t.onStats(true,false,1002,500,20);t.onStats(true,false,1004,600,30);
        check(t.get(&total,&qpc,&src)&&total==4&&src==Source::FrameStatistics&&qpc==600,"presented: frame statistics add PresentCount's steps, timed by SyncQPCTime");
        check(noteOf(t)=="perf: presented frames from DXGI frame statistics (PresentCount)","presented: the statistics source is logged when it starts");
    }
    {   // a status that says 0 every time
        Tally t;
        for(unsigned i=0;i<fgpresented::kProbePresents;++i)t.onStatus(true,0,i);
        check(t.wantsStats()&&t.statusWhy()&&std::strstr(t.statusWhy(),"never reports"),"presented: a status that always says 0 is given up on, and the reason says so");
    }
    {   // wrap, disjoint, a restarted counter
        Tally t;t.onSwapChainChanged();
        t.onStats(true,false,0xFFFFFFFEu,0,1);t.onStats(true,false,2,10,2);
        check(t.get(&total,&qpc,&src)&&total==4,"presented: PresentCount wrapping past 2^32 is a small step (0xFFFFFFFE to 2 is 4)");
        t.onStats(false,true,0,0,3);t.onStats(true,false,500,20,4);
        check(t.get(&total,nullptr,nullptr)&&total==4,"presented: a DISJOINT result and the sample after it add nothing (a new baseline)");
        t.onStats(true,false,501,30,5);
        check(t.get(&total,nullptr,nullptr)&&total==5,"presented: counting resumes from that baseline");
        t.onStats(true,false,501+fgpresented::kMaxStatsStep+1,40,6);t.onStats(true,false,502+fgpresented::kMaxStatsStep+1,50,7);
        check(t.get(&total,nullptr,nullptr)&&total==6,"presented: a step beyond the bound is a restarted counter, not 1000 frames");
    }
    {   // the bridge falls back: the count carries on from the plain swap chain's statistics
        Tally t;
        for(int i=0;i<10;++i)t.onStatus(true,2,i);
        noteOf(t);
        t.onSwapChainChanged();
        check(t.wantsStats()&&t.get(&total,&qpc,&src)&&total==20&&src==Source::StatusFrames,"presented: after the fall back the count so far stands and statistics are wanted");
        t.onStats(true,false,50,1000,100);t.onStats(true,false,51,1010,110);t.onStats(true,false,52,1020,120);
        check(t.get(&total,&qpc,&src)&&total==22&&src==Source::FrameStatistics&&qpc==1020,"presented: the plain swap chain's presents are added from its own baseline (one frame each)");
        check(noteOf(t)=="perf: presented frames from DXGI frame statistics (PresentCount)","presented: the switch of source is logged");
    }
    {   // unavailable: said with the reason, never as 0
        Tally t;
        for(unsigned i=0;i<fgpresented::kProbePresents;++i)t.onStatus(false,0,i);
        for(unsigned i=0;i<fgpresented::kProbePresents;++i)t.onStats(false,false,0,0,i);
        check(t.unavailable()&&!t.get(&total,&qpc,&src)&&std::strlen(t.unavailableWhy())>0,"presented: neither source works: unavailable with a reason, and no count (not zero)");
        check(noteOf(t).rfind("perf: presented frames unavailable (",0)==0,"presented: unavailable is logged once with its reason");
        Tally flat;
        for(unsigned i=0;i<fgpresented::kProbePresents;++i)flat.onStatus(false,0,i);
        for(unsigned i=0;i<4*fgpresented::kProbePresents+2;++i)flat.onStats(true,false,7,i,i);
        check(flat.unavailable()&&std::strstr(flat.unavailableWhy(),"never advance"),"presented: statistics that never advance are unavailable too, not a rate of zero");
    }
}
void presentedRing() {
    {   // a session without a bridge: nothing observed, and every answer is "absent"
        PresentedRing r;
        float v[8];
        check(!r.any()&&r.count()==0&&r.copy(v,8)==0&&r.stats().count==0&&r.pushes()==0,"presented ring: never fed is empty, with no stats and no samples (absent, not 0)");
    }
    {   // 2x generation: the counter gains 2 per rendered frame (17 ms)
        PresentedRing r;
        r.observe(0,0,1000);
        check(!r.any(),"presented ring: the first observation is a baseline");
        for(int i=1;i<=10;++i)r.observe(uint64_t(2*i),int64_t(17*i),1000);
        const PerfStats s=r.stats();
        check(r.count()==10&&std::fabs(s.avgMs-8.5f)<1e-4f,"presented ring: two frames per 17 ms rendered frame is an 8.5 ms period");
        check(std::fabs(perfFpsOf(s.avgMs)-117.647f)<0.01f,"presented ring: ...which is 117.6 fps shown");
        float v[16];
        check(r.copy(v,16)==10&&std::fabs(v[0]-8.5f)<1e-4f&&r.copy(v,4)==4,"presented ring: the strip's samples are those periods, newest last");
    }
    {   // the counter lags a Present: no entry, and the time goes to the next one
        PresentedRing r;r.observe(10,0,1000);r.observe(10,17,1000);
        check(!r.any(),"presented ring: a Present that did not move the counter makes no entry");
        r.observe(14,34,1000);
        check(r.count()==1&&std::fabs(r.periodAt(0)-8.5f)<1e-4f,"presented ring: ...and the next entry spans both (34 ms, 4 frames, 8.5 ms)");
    }
    {   // a wrap or restart of the counter, a long gap, a rebase
        PresentedRing r;r.observe(100,0,1000);r.observe(102,17,1000);
        r.observe(5,34,1000);
        check(r.count()==1,"presented ring: a counter that went backwards is a new baseline, not an entry");
        r.observe(7,51,1000);
        check(r.count()==2,"presented ring: counting resumes from the new baseline");
        r.observe(9,51+6000,1000);
        check(r.count()==2,"presented ring: a 6 s gap (a load) is not an entry");
        r.rebase();r.observe(11,7000,1000);r.observe(13,7017,1000);
        check(r.count()==3,"presented ring: a rebase (a profile switch) drops the baseline and keeps the strip");
    }
    {   // the mean is every frame over the time it took, not the mean of the per-entry periods
        PresentedRing r;r.observe(0,0,1000);r.observe(1,10,1000);r.observe(3,50,1000);   // 10 ms for 1 frame, 40 ms for 2 (20 ms each)
        const PerfStats s=r.stats();
        check(std::fabs(s.avgMs-50.0f/3.0f)<1e-3f,"presented ring: the mean is 50 ms over 3 frames, not the 15 ms mean of the two periods");
    }
    {   // the window
        PresentedRing r;r.observe(0,0,1000);
        for(int i=1;i<=200;++i)r.observe(uint64_t(i),int64_t(i)*10,1000);   // 200 entries of 10 ms: 2 s
        for(int i=201;i<=300;++i)r.observe(uint64_t(i)+uint64_t(i-200),int64_t(i)*10,1000);   // 100 entries of 10 ms for 2 frames: 5 ms
        check(r.stats(1.0f).count==100&&std::fabs(r.stats(1.0f).avgMs-5.0f)<1e-3f,"presented ring: a one-second window covers the newest 100 entries");
        check(r.stats().count==300&&r.stats().avgMs<10.0f&&r.stats().avgMs>5.0f,"presented ring: the whole ring covers all of them");
    }
    {   // the capacity
        PresentedRing r;r.observe(0,0,1000);
        for(int i=1;i<=PresentedRing::kCapacity+50;++i)r.observe(uint64_t(i),int64_t(i)*10,1000);
        check(r.count()==PresentedRing::kCapacity&&r.pushes()==uint64_t(PresentedRing::kCapacity+50),"presented ring: it keeps the newest kCapacity and counts every push");
    }
}
void presentedBudgets() {
    check(std::fabs(displayBudgetMs(144.0f)-6.9444f)<1e-3f&&std::fabs(displayBudgetMs(60.0f)-16.6667f)<1e-3f,"budget: the presented budget is the refresh period");
    check(displayBudgetMs(0.0f)==kUnknownRefreshBudgetMs&&displayBudgetMs(1.0f)==kUnknownRefreshBudgetMs&&displayBudgetMs(5000.0f)==kUnknownRefreshBudgetMs,"budget: an unknown or absurd rate falls back to 11.1 ms");
    check(std::fabs(renderedBudgetMs(144.0f,0)-6.9444f)<1e-3f,"budget: without frame generation the rendered budget is the refresh period");
    check(std::fabs(renderedBudgetMs(144.0f,1)-13.8889f)<1e-3f&&std::fabs(renderedBudgetMs(60.0f,3)-66.6667f)<1e-3f,"budget: the rendered budget is that period times (1 + active generated frames)");
    check(std::fabs(renderedBudgetMs(0.0f,2)-33.3f)<1e-3f,"budget: an unknown rate still scales with the factor");
    check(presentedStateOf(false,true,false,true)==PresentedState::Absent&&presentedStateOf(false,false,false,false)==PresentedState::Absent,"state: no bridge is absent, whatever else says");
    check(presentedStateOf(true,true,false,true)==PresentedState::Counting&&presentedStateOf(true,true,false,false)==PresentedState::Measuring,"state: counting needs an entry; before it, measuring");
    check(presentedStateOf(true,false,true,false)==PresentedState::Unavailable&&presentedStateOf(true,false,false,false)==PresentedState::Measuring,"state: unavailable only when said; not sampled yet is measuring");
    check(presentedStateOf(true,true,true,true)==PresentedState::Counting,"state: a count that came back after an unavailable verdict is counting");
}
struct TileRow{std::string caption,value,sub;};
std::vector<TileRow> tilesOf(const PerfReadout& r) {
    std::vector<TileRow> v;
    emitRateTiles(r,[&](const char* c,const char* val,const char* sub){v.push_back({c,val,sub?sub:""});});
    return v;
}
PerfReadout bridged(PresentedState st,uint32_t active) {
    PerfReadout r;r.haveRendered=true;r.renderedFps=58.4f;r.renderedMs=17.1f;r.renderedLowFps=52.0f;r.renderedLowMs=19.2f;
    r.haveCpu=true;r.cpuMs=9.14f;r.presented=st;r.presentedFps=116.8f;r.presentedMs=8.56f;r.presentedLowFps=104.0f;r.presentedLowMs=9.6f;r.activeGenerated=active;
    return r;
}
void readoutText() {
    char line[160];
    {   // no bridge: the compact formatter declines, so today's line is printed, and there are three tiles and no note
        PerfReadout r=bridged(PresentedState::Absent,0);
        check(!formatCompactLine(r,line,sizeof(line)),"readout: without a bridge the compact formatter leaves the line to the one printed today");
        const std::vector<TileRow> t=tilesOf(r);
        char note[80]="x";formatBridgeNote(r,note,sizeof(note));
        check(t.size()==3&&t[0].caption=="RENDERED"&&t[1].caption=="FRAME TIME"&&t[2].caption=="1% LOW"&&note[0]==0,"readout: without a bridge only the rendered tiles show, and no note");
    }
    {   // frame generation 2x
        PerfReadout r=bridged(PresentedState::Counting,1);
        check(formatCompactLine(r,line,sizeof(line))&&std::string(line)=="render 58 / shown 117 fps   cpu 9.1 ms","readout: the compact line names both rates and the CPU");
        r.drops=2;
        check(formatCompactLine(r,line,sizeof(line))&&std::string(line)=="render 58 / shown 117 fps   cpu 9.1 ms   drops 2","readout: drops are appended only when there were some");
        const std::vector<TileRow> t=tilesOf(r);
        check(t.size()==6&&t[0].value=="58.4"&&t[1].value=="17.1"&&t[2].value=="52"&&t[3].caption=="PRESENTED"&&t[3].value=="116.8"&&t[4].caption=="PRESENTED 1% LOW"&&t[4].value=="104","readout: the rendered and presented tiles carry their own numbers");
        check(t[5].caption=="FRAME GENERATION"&&t[5].value=="x2","readout: the active factor is 1 + the generated frames");
        r.activeGenerated=3;check(tilesOf(r)[5].value=="x4","readout: three generated frames are x4");
    }
    {   // bridged with frame generation off: both shown, equal, x1
        PerfReadout r=bridged(PresentedState::Counting,0);
        r.presentedFps=r.renderedFps;
        const std::vector<TileRow> t=tilesOf(r);
        check(t.size()==6&&t[5].value=="x1"&&t[5].sub=="off: shown = rendered"&&t[3].value==t[0].value,"readout: with frame generation off the bridge still shows both, equal, at x1");
    }
    {   // unavailable and stopped and measuring: three different words
        PerfReadout r=bridged(PresentedState::Unavailable,0);
        std::snprintf(r.unavailableWhy,sizeof(r.unavailableWhy),"frame statistics never advance");
        char note[200];formatBridgeNote(r,note,sizeof(note));
        check(std::string(note)=="Presented: unavailable (frame statistics never advance)","readout: unavailable is said with its reason");
        check(formatCompactLine(r,line,sizeof(line))&&std::string(line)=="render 58 fps   shown n/a   cpu 9.1 ms","readout: ...and in the compact line as n/a, not a zero");
        check(tilesOf(r)[3].value=="--"&&tilesOf(r)[3].sub=="unavailable","readout: ...and in the tile");
        r=bridged(PresentedState::Counting,0);r.fgStopped=true;std::snprintf(r.stopped,sizeof(r.stopped),"a wait on the queue timed out");
        formatBridgeNote(r,note,sizeof(note));
        check(std::string(note)=="FG stopped: a wait on the queue timed out"&&tilesOf(r)[5].sub=="stopped","readout: a stopped bridge is said as FG stopped with the reason");
        r=bridged(PresentedState::Measuring,0);
        check(formatCompactLine(r,line,sizeof(line))&&std::string(line)=="render 58 / shown -- fps   cpu 9.1 ms"&&tilesOf(r)[3].sub=="measuring","readout: a count not measured yet is --, not 0");
        formatBridgeNote(r,note,sizeof(note));check(note[0]==0,"readout: measuring has no note");
        PerfReadout none;none.presented=PresentedState::Counting;
        check(formatCompactLine(none,line,sizeof(line))&&std::string(line)=="measuring","readout: no rendered frames yet is \"measuring\"");
    }
}
// ---- the frame-rate readout (fps_overlay_mode.h, menu_flat_layout.h, menu_policy.h) ------------------------------------------------------------------
void fpsOverlayModes() {
    check(parseFpsOverlay("off")==FpsOverlayMode::Off&&parseFpsOverlay("compact")==FpsOverlayMode::Compact&&parseFpsOverlay("detailed")==FpsOverlayMode::Detailed,"fps overlay: the three words");
    check(parseFpsOverlay("on")==FpsOverlayMode::Compact&&parseFpsOverlay("true")==FpsOverlayMode::Compact&&parseFpsOverlay("1")==FpsOverlayMode::Compact&&parseFpsOverlay("yes")==FpsOverlayMode::Compact,"fps overlay: the old on words read as compact");
    check(parseFpsOverlay("false")==FpsOverlayMode::Off&&parseFpsOverlay("0")==FpsOverlayMode::Off&&parseFpsOverlay("no")==FpsOverlayMode::Off,"fps overlay: the old off words read as off");
    check(parseFpsOverlay("  Detailed \r")==FpsOverlayMode::Detailed&&parseFpsOverlay("ON")==FpsOverlayMode::Compact,"fps overlay: case and surrounding blanks do not matter");
    check(parseFpsOverlay("")==FpsOverlayMode::Off&&parseFpsOverlay(nullptr)==FpsOverlayMode::Off&&parseFpsOverlay("loud")==FpsOverlayMode::Off,"fps overlay: empty, missing and unknown read as the default, off");
    check(!std::strcmp(fpsOverlayWord(FpsOverlayMode::Compact),"compact")&&parseFpsOverlay(fpsOverlayWord(FpsOverlayMode::Detailed))==FpsOverlayMode::Detailed,"fps overlay: the words the menu writes read back");
    check(parseMonitorPanelView("full")==MonitorPanelView::Full&&parseMonitorPanelView("detailed")==MonitorPanelView::Detailed&&parseMonitorPanelView("compact")==MonitorPanelView::Compact,"monitor panel view: the three words");
    check(parseMonitorPanelView("")==MonitorPanelView::Full&&parseMonitorPanelView(nullptr)==MonitorPanelView::Full&&parseMonitorPanelView("off")==MonitorPanelView::Full&&parseMonitorPanelView("loud")==MonitorPanelView::Full,"monitor panel view: empty, missing, off and unknown read as the default, full");
    check(parseMonitorPanelView("  Compact \r")==MonitorPanelView::Compact,"monitor panel view: case and surrounding blanks do not matter");
    check(parseMonitorPanelView(monitorPanelViewWord(MonitorPanelView::Detailed))==MonitorPanelView::Detailed&&parseMonitorPanelView(monitorPanelViewWord(MonitorPanelView::Full))==MonitorPanelView::Full,"monitor panel view: the words the menu writes read back");
    check(monitorPanelReadoutMode(MonitorPanelView::Full)==FpsOverlayMode::Off&&monitorPanelReadoutMode(MonitorPanelView::Detailed)==FpsOverlayMode::Detailed&&monitorPanelReadoutMode(MonitorPanelView::Compact)==FpsOverlayMode::Compact,"monitor panel view: detailed and compact draw the readout's shapes, full the Monitor page");
}
void detailedText() {
    char a[96],b[96];
    PerfReadout r=bridged(PresentedState::Counting,1);r.drops=3;
    formatDetailRates(r,a,sizeof(a));formatDetailTimes(r,b,sizeof(b));
    check(std::string(a)=="render 58.4 / shown 116.8 fps"&&std::string(b)=="17.1 ms   worst 52 fps   cpu 9.1 ms   drops 3","detailed: line 1 the two rates, line 2 frame time, worst, cpu and drops");
    r=bridged(PresentedState::Absent,0);r.haveCpu=false;
    formatDetailRates(r,a,sizeof(a));formatDetailTimes(r,b,sizeof(b));
    check(std::string(a)=="58.4 fps"&&std::string(b)=="17.1 ms   worst 52 fps   drops 0","detailed: without a bridge one rate, and no cpu when it is unknown");
    r=bridged(PresentedState::Unavailable,0);formatDetailRates(r,a,sizeof(a));
    check(std::string(a)=="render 58.4 fps   shown n/a","detailed: an unavailable count is n/a, not a number");
    PerfReadout none;formatDetailRates(none,a,sizeof(a));formatDetailTimes(none,b,sizeof(b));
    check(std::string(a)=="measuring"&&b[0]==0,"detailed: nothing rendered yet is measuring, with no second line");
}
void flatCorner() {
    for(int outH:{1080,1440,2160}){
        const int outW=outH*16/9;
        const FlatCardSize fs=flatCardSize(outW,outH,kFlatMenuCardHeightFrac);
        const int w=fs.capPx*28,h=fs.capPx*14;
        const FlatPlacement ul=flatPlace(outW,outH,w,h),ur=flatPlace(outW,outH,w,h,FlatCorner::UpperRight);
        const int margin=int(std::lround(kFlatMarginFrac*outW));
        check(ul.oneToOne&&ur.oneToOne&&ur.w==w&&ur.h==h,"flat corner: the readout is composited 1:1 at flat sizes");
        check(ul.x==margin&&ur.x+ur.w==outW-margin&&ur.y==ul.y,"flat corner: upper right is the same 2% margin from the right edge, at the same height");
        check(std::fabs((ur.shift-ur.halfW)-(-1.0f+2.0f*float(ur.x)/float(outW)))<1e-5f&&ur.shift>ul.shift,"flat corner: the geometry's left edge is the placed x, to the right of the upper left one");
    }
    const FlatPlacement def=flatPlace(1920,1080,300,200);
    check(def.x==flatPlace(1920,1080,300,200,FlatCorner::UpperLeft).x,"flat corner: the default is upper left, as the menu and notices were placed");
}
void readoutMachine() {
    ScreenReadoutState m;
    check(!m.tick(false,false,5)&&!m.compositing(),"readout machine: not wanted does nothing");
    check(m.tick(true,false,5),"readout machine: wanted, it asks for the first raster");
    m.didSubmit(5);
    check(!m.compositing()&&!m.tick(true,false,5)&&!m.compositing(),"readout machine: the raster is not shown until the panel has taken one");
    check(!m.tick(true,false,6)&&m.compositing(),"readout machine: shown once the serial moved");
    check(m.tick(true,true,6)&&m.compositing(),"readout machine: a refresh is asked for and the card stays up");
    m.didSubmit(6);
    check(m.compositing(),"readout machine: a refresh submit does not hide it");
    m.tick(false,false,7);
    check(!m.compositing()&&m.tick(true,false,7),"readout machine: the monitor panel (or the menu's locked line) hides it, and it starts again from nothing");
    m.didSubmit(7);check(!m.compositing(),"readout machine: and is not shown until its own raster is taken");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    if(!std::wcscmp(argv[1],L"--dry-run")){std::puts("native_perf_history_test: dry-run (no runtime, device or files)");return 0;}
    if(std::wcscmp(argv[1],L"--self-test"))return 2;
    averages();invalidation();agesAndValues();displayBase();callerWorkCrossing();graphs();benchmarkWindows();benchmarkAdmission();
    frameTicksChain();frameTicksTopThree();frameTicksOddClocks();frameTicksUncut();frameShareText();nativeLongFrameFits();frameTickCostNote();
    fpsOverlayModes();detailedText();flatCorner();readoutMachine();
    presentedTally();presentedRing();presentedBudgets();readoutText();
    std::printf("native_perf_history_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
