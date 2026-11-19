#pragma once
// The Monitor's GPU load tile (perf_monitor.cpp). Pure, so tools\gpu_engine_load_test runs the decision the tile makes.
//
// Windows publishes one "\GPU Engine(*)\Utilization Percentage" instance per process per engine. The instance name
// observed with Get-Counter on this machine (2026-11-19) is:
//   pid_<pid>_luid_0x<high>_0x<low>_phys_<n>_eng_<n>_engtype_<type>
// and the type is lowercase, with a space when the driver numbers it ("3d", "compute 0", "video codec engine").
// Each instance is that process's share of that engine, so shares of one engine add. Different engines are not added:
// a copy engine busy beside an idle 3D engine is not a graphics load. The tile's number is the busiest 3D, graphics,
// or compute engine on the game's adapter (the LUID of the device's DXGI adapter). With no such engine, the busiest
// engine of any type on that adapter stands, so an unfamiliar driver name still shows. NVIDIA's graphics-domain figure
// is a different instrument; the tile shows it only when it reads higher. Temperature is not in these counters.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>

namespace edvr {

enum class GpuEngineClass { Graphics, Compute, Other };

struct GpuEngineInstance {
    uint32_t luidLow = 0;
    uint32_t luidHigh = 0;
    uint32_t phys = 0;
    uint32_t eng = 0;
    GpuEngineClass cls = GpuEngineClass::Other;
};

// One counter row already parsed. `percent` is that process's share of `eng` on `phys`.
struct GpuEngineRow {
    uint32_t luidLow = 0;
    uint32_t luidHigh = 0;
    uint32_t phys = 0;
    uint32_t eng = 0;
    GpuEngineClass cls = GpuEngineClass::Other;
    double percent = 0.0;
};

inline GpuEngineClass classifyGpuEngine(const wchar_t* type) {
    if (!type || !type[0]) return GpuEngineClass::Other;
    if (_wcsicmp(type, L"3d") == 0) return GpuEngineClass::Graphics;
    if (_wcsnicmp(type, L"graphics", 8) == 0 && (type[8] == 0 || type[8] == L' ' || type[8] == L'_'))
        return GpuEngineClass::Graphics;
    if (_wcsnicmp(type, L"compute", 7) == 0 && (type[7] == 0 || type[7] == L' ' || type[7] == L'_'))
        return GpuEngineClass::Compute;
    return GpuEngineClass::Other;
}

// Accepts the instance name, or a full counter path that contains it. False when the shape is not there.
inline bool parseGpuEngineInstance(const wchar_t* name, GpuEngineInstance& out) {
    if (!name) return false;
    const wchar_t* p = name;
    if (wcsncmp(p, L"pid_", 4) != 0) {
        p = wcsstr(name, L"pid_");
        if (!p) return false;
    }
    p += 4;
    if (!iswdigit(*p)) return false;
    while (iswdigit(*p)) ++p;
    if (wcsncmp(p, L"_luid_0x", 8) != 0) return false;
    p += 8;
    wchar_t* end = nullptr;
    const unsigned long high = wcstoul(p, &end, 16);
    if (end == p || wcsncmp(end, L"_0x", 3) != 0) return false;
    p = end + 3;
    const unsigned long low = wcstoul(p, &end, 16);
    if (end == p || wcsncmp(end, L"_phys_", 6) != 0) return false;
    p = end + 6;
    if (!iswdigit(*p)) return false;
    const unsigned long phys = wcstoul(p, &end, 10);
    if (end == p || wcsncmp(end, L"_eng_", 5) != 0) return false;
    p = end + 5;
    if (!iswdigit(*p)) return false;
    const unsigned long eng = wcstoul(p, &end, 10);
    if (end == p || wcsncmp(end, L"_engtype_", 9) != 0) return false;
    p = end + 9;
    // The instance name ends at the type. A full counter path keeps going: "3d)\utilization percentage".
    wchar_t type[64];
    size_t n = 0;
    while (p[n] && p[n] != L')' && n + 1 < sizeof(type) / sizeof(type[0])) {
        type[n] = p[n];
        ++n;
    }
    if (!n) return false;
    type[n] = 0;
    out.luidHigh = static_cast<uint32_t>(high);
    out.luidLow = static_cast<uint32_t>(low);
    out.phys = static_cast<uint32_t>(phys);
    out.eng = static_cast<uint32_t>(eng);
    out.cls = classifyGpuEngine(type);
    return true;
}

// Distinct engines remembered while folding process shares. One adapter's nodes fit; a node past the table is left out
// rather than summed into a neighbour.
constexpr int kGpuEngineFold = 96;

// 0..100, or -1 when no row matches the adapter. Process shares of one (phys, eng) add and clamp at 100. The result is
// the busiest graphics or compute engine; failing those, the busiest engine of any class.
inline int gpuEngineLoadPercent(const GpuEngineRow* rows, int count, uint32_t luidLow, uint32_t luidHigh) {
    if (!rows || count <= 0) return -1;
    struct Fold {
        uint32_t phys;
        uint32_t eng;
        GpuEngineClass cls;
        double sum;
    };
    Fold fold[kGpuEngineFold];
    int n = 0;
    for (int i = 0; i < count; ++i) {
        const GpuEngineRow& row = rows[i];
        if (row.luidLow != luidLow || row.luidHigh != luidHigh) continue;
        if (!std::isfinite(row.percent) || row.percent < 0.0) continue;
        int slot = -1;
        for (int k = 0; k < n; ++k) {
            if (fold[k].phys == row.phys && fold[k].eng == row.eng) {
                slot = k;
                break;
            }
        }
        if (slot < 0) {
            if (n >= kGpuEngineFold) continue;
            slot = n++;
            fold[slot].phys = row.phys;
            fold[slot].eng = row.eng;
            fold[slot].cls = row.cls;
            fold[slot].sum = 0.0;
        }
        fold[slot].sum += row.percent;
    }
    bool sawWork = false;
    bool sawAny = false;
    double bestWork = 0.0;
    double bestAny = 0.0;
    for (int k = 0; k < n; ++k) {
        const double v = fold[k].sum > 100.0 ? 100.0 : fold[k].sum;
        sawAny = true;
        if (v > bestAny) bestAny = v;
        if (fold[k].cls == GpuEngineClass::Graphics || fold[k].cls == GpuEngineClass::Compute) {
            sawWork = true;
            if (v > bestWork) bestWork = v;
        }
    }
    const double chosen = sawWork ? bestWork : (sawAny ? bestAny : -1.0);
    if (chosen < 0.0) return -1;
    return static_cast<int>(chosen + 0.5);
}

// NVIDIA's percent and temperature describe NVIDIA's own GPU. Use them when the game's adapter is NVIDIA, and also while
// the adapter is not known yet (they are then the only figure). A known non-NVIDIA adapter does not borrow them: on a
// hybrid machine NvAPI's first GPU is often the other one.
constexpr uint32_t kGpuVendorNvidia = 0x10DEu;
inline bool gpuLoadUseNv(bool nvOk, bool haveAdapter, uint32_t vendorId) {
    return nvOk && (!haveAdapter || vendorId == kGpuVendorNvidia);
}

// The tile. `enginePercent` and `nvPercent` are -1 when that source has no sample. A waiting source prints "sampling";
// a source that has answered and has nothing prints `unavailable`.
inline void formatGpuLoadTile(int enginePercent, int nvPercent, bool haveTemp, int tempC, bool sampling,
                              const char* unavailable, char* value, size_t valueLen, char* sub, size_t subLen) {
    if (!value || !sub || !valueLen || !subLen) return;
    auto clamp = [](int p) { return p > 100 ? 100 : p; };
    int chosen = -1;
    if (enginePercent >= 0) chosen = clamp(enginePercent);
    if (nvPercent >= 0) {
        const int nv = clamp(nvPercent);
        if (nv > chosen) chosen = nv;
    }
    if (chosen >= 0) {
        std::snprintf(value, valueLen, "%d%%", chosen);
        if (haveTemp) std::snprintf(sub, subLen, "load, %d C", tempC);
        else std::snprintf(sub, subLen, "load");
        return;
    }
    std::snprintf(value, valueLen, "--");
    if (sampling) std::snprintf(sub, subLen, "sampling");
    else std::snprintf(sub, subLen, "%s", unavailable && unavailable[0] ? unavailable : "n/a");
}

}  // namespace edvr
