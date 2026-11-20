// gpu_engine_load_test -- src\d3d11\gpu_engine_load.h, the Monitor GPU tile's engine-counter fold. The instance strings are the ones
// Get-Counter printed for \GPU Engine(*)\Utilization Percentage. --dry-run writes nothing; --self-test runs the checks.
#include "../../src/d3d11/gpu_engine_load.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* name) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

using edvr::GpuEngineClass;
using edvr::GpuEngineInstance;
using edvr::GpuEngineRow;
using edvr::formatGpuLoadTile;
using edvr::gpuEngineLoadPercent;
using edvr::gpuLoadUseNv;
using edvr::kGpuVendorNvidia;
using edvr::parseGpuEngineInstance;

GpuEngineRow row(uint32_t low, uint32_t high, uint32_t phys, uint32_t eng, GpuEngineClass cls, double percent) {
    GpuEngineRow r;
    r.luidLow = low;
    r.luidHigh = high;
    r.phys = phys;
    r.eng = eng;
    r.cls = cls;
    r.percent = percent;
    return r;
}

bool parsed(const wchar_t* name, GpuEngineInstance& inst) { return parseGpuEngineInstance(name, inst); }

std::string readSource(const char* path) {
    std::string out;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return out;
    char buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, got);
    std::fclose(f);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    bool dryRun = false, selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    }
    if (dryRun) {
        std::printf("gpu_engine_load_test: --dry-run writes nothing; the checks run under --self-test\n");
        return 0;
    }
    if (!selfTest) {
        std::printf("usage: gpu_engine_load_test --self-test | --dry-run\n");
        return 1;
    }

    GpuEngineInstance inst{};
    check(parsed(L"pid_10244_luid_0x00000000_0x00013641_phys_0_eng_0_engtype_3d", inst) &&
              inst.luidLow == 0x13641 && inst.luidHigh == 0 && inst.phys == 0 && inst.eng == 0 &&
              inst.cls == GpuEngineClass::Graphics,
          "a 3d instance is the graphics engine on its LUID");
    check(parsed(L"pid_10244_luid_0x00000000_0x00013641_phys_0_eng_2_engtype_compute 0", inst) && inst.eng == 2 &&
              inst.cls == GpuEngineClass::Compute,
          "compute 0 is a compute engine");
    check(parsed(L"pid_10244_luid_0x00000000_0x00013641_phys_0_eng_1_engtype_copy", inst) && inst.cls == GpuEngineClass::Other,
          "copy is not a graphics engine");
    check(parsed(L"pid_10244_luid_0x00000000_0x00013641_phys_0_eng_7_engtype_video codec engine", inst) &&
              inst.cls == GpuEngineClass::Other,
          "a video codec engine is not a graphics engine");
    check(parsed(L"pid_9_luid_0x00000001_0x00015701_phys_0_eng_10_engtype_graphics_1", inst) && inst.luidHigh == 1 &&
              inst.luidLow == 0x15701 && inst.eng == 10 && inst.cls == GpuEngineClass::Graphics,
          "graphics_1 is a graphics engine, high LUID included");
    check(parsed(L"\\\\flow-z13\\gpu engine(pid_10244_luid_0x00000000_0x00013641_phys_0_eng_0_engtype_3d)\\utilization percentage",
                 inst) &&
              inst.cls == GpuEngineClass::Graphics && inst.luidLow == 0x13641,
          "a full counter path parses the instance inside it");
    check(!parsed(L"pid_12_luid_0x1", inst), "a truncated instance is refused");
    check(!parsed(L"", inst) && !parsed(nullptr, inst), "an empty name is refused");

    const uint32_t low = 0x13641;
    const GpuEngineRow sameEngine[] = {
        row(low, 0, 0, 0, GpuEngineClass::Graphics, 40),
        row(low, 0, 0, 0, GpuEngineClass::Graphics, 35),
    };
    check(gpuEngineLoadPercent(sameEngine, 2, low, 0) == 75, "two processes on one 3D engine add");

    const GpuEngineRow clamped[] = {
        row(low, 0, 0, 0, GpuEngineClass::Graphics, 80),
        row(low, 0, 0, 0, GpuEngineClass::Graphics, 80),
    };
    check(gpuEngineLoadPercent(clamped, 2, low, 0) == 100, "process shares clamp at 100");

    const GpuEngineRow engines[] = {
        row(low, 0, 0, 0, GpuEngineClass::Graphics, 30),
        row(low, 0, 0, 10, GpuEngineClass::Graphics, 55),
    };
    check(gpuEngineLoadPercent(engines, 2, low, 0) == 55, "two 3D engines stay apart; the busiest wins");

    const GpuEngineRow copyBeside[] = {
        row(low, 0, 0, 0, GpuEngineClass::Graphics, 10),
        row(low, 0, 0, 1, GpuEngineClass::Other, 99),
    };
    check(gpuEngineLoadPercent(copyBeside, 2, low, 0) == 10, "a busy copy engine does not raise a quiet 3D engine");

    const GpuEngineRow idle3d[] = {
        row(low, 0, 0, 0, GpuEngineClass::Graphics, 0),
        row(low, 0, 0, 1, GpuEngineClass::Other, 90),
    };
    check(gpuEngineLoadPercent(idle3d, 2, low, 0) == 0, "an idle 3D engine is 0, not the copy engine beside it");

    const GpuEngineRow computeBusy[] = {
        row(low, 0, 0, 0, GpuEngineClass::Graphics, 20),
        row(low, 0, 0, 2, GpuEngineClass::Compute, 70),
    };
    check(gpuEngineLoadPercent(computeBusy, 2, low, 0) == 70, "compute counts, and is not added to 3D");

    const GpuEngineRow onlyCopy[] = {row(low, 0, 0, 1, GpuEngineClass::Other, 80)};
    check(gpuEngineLoadPercent(onlyCopy, 1, low, 0) == 80, "with no 3D or compute engine, the busiest engine stands");

    const GpuEngineRow otherAdapter[] = {
        row(low, 0, 0, 0, GpuEngineClass::Graphics, 12),
        row(0x15701, 0, 0, 0, GpuEngineClass::Graphics, 99),
    };
    check(gpuEngineLoadPercent(otherAdapter, 2, low, 0) == 12, "the other adapter's engine is not this GPU");
    check(gpuEngineLoadPercent(otherAdapter, 2, 0x15701, 0) == 99, "the same rows answer for the other adapter");

    const GpuEngineRow junk[] = {
        row(low, 0, 0, 0, GpuEngineClass::Graphics, -5),
        row(low, 0, 0, 2, GpuEngineClass::Compute, std::nan("")),
    };
    check(gpuEngineLoadPercent(junk, 2, low, 0) == -1, "a negative share and a NaN are not a sample");
    check(gpuEngineLoadPercent(nullptr, 0, low, 0) == -1, "no rows is not a sample");

    char value[24], sub[40];
    formatGpuLoadTile(40, 82, true, 67, false, "n/a", value, sizeof(value), sub, sizeof(sub));
    check(std::strcmp(value, "82%") == 0 && std::strcmp(sub, "load, 67 C") == 0,
          "NVIDIA's higher figure wins, and its temperature is shown");
    formatGpuLoadTile(90, 40, true, 61, false, "n/a", value, sizeof(value), sub, sizeof(sub));
    check(std::strcmp(value, "90%") == 0 && std::strcmp(sub, "load, 61 C") == 0,
          "the engine counter wins when it reads higher, and the temperature stays");
    formatGpuLoadTile(33, -1, false, 0, false, "n/a", value, sizeof(value), sub, sizeof(sub));
    check(std::strcmp(value, "33%") == 0 && std::strcmp(sub, "load") == 0,
          "without NVIDIA there is a percentage and no temperature");
    formatGpuLoadTile(-1, -1, false, 0, true, "n/a (GPU counters refused)", value, sizeof(value), sub, sizeof(sub));
    check(std::strcmp(value, "--") == 0 && std::strcmp(sub, "sampling") == 0, "a source that has not answered yet is sampling");
    formatGpuLoadTile(-1, -1, false, 0, false, "n/a (GPU counters refused)", value, sizeof(value), sub, sizeof(sub));
    check(std::strcmp(value, "--") == 0 && std::strcmp(sub, "n/a (GPU counters refused)") == 0,
          "a source that answered with nothing names the reason");
    formatGpuLoadTile(150, -1, false, 0, false, "n/a", value, sizeof(value), sub, sizeof(sub));
    check(std::strcmp(value, "100%") == 0, "a share past 100 clamps");

    check(gpuLoadUseNv(true, false, 0x1002), "before the adapter is known, NVIDIA's figure is still a candidate");
    check(gpuLoadUseNv(true, true, kGpuVendorNvidia), "an NVIDIA adapter keeps NVIDIA's figure");
    check(!gpuLoadUseNv(true, true, 0x1002), "an AMD adapter does not borrow the NVIDIA GPU's percent");
    check(!gpuLoadUseNv(true, true, 0x8086), "an Intel adapter does not borrow the NVIDIA GPU's percent");
    check(!gpuLoadUseNv(false, true, kGpuVendorNvidia), "NVIDIA's figure is absent when NvAPI did not answer");

    const std::string source = readSource("src\\d3d11\\perf_monitor.cpp");
    check(!source.empty(), "perf_monitor.cpp is read from the repo root");
    check(source.find("\\\\GPU Engine(*)\\\\Utilization Percentage") != std::string::npos,
          "the monitor samples the GPU Engine utilization counter");
    check(source.find("formatGpuLoadTile") != std::string::npos, "the GPU tile uses the shared formatter");
    check(source.find("gpuEngineLoadPercent") != std::string::npos, "the monitor folds engines with the shared rule");
    check(source.find("s.gpuLoadPct") == std::string::npos, "the tile no longer prints the NVIDIA percent on its own");

    std::printf("gpu_engine_load_test: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
