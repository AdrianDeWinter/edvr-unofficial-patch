// The rig for the system d3d11.dll check (src\common\system_d3d11.h: chooseSystemD3D11 and the walk it falls back to;
// src\d3d11\d3d11_proxy.cpp: probeSystemModule, which the context probe and slot reclaim read).
//
// The d3d11 proxy takes its handle to Windows' d3d11.dll from a LoadLibraryW of <System32>\d3d11.dll inside DllMain. A
// hook on LoadLibraryExW (3Dmigoto and EDHM with load_library_redirect = 2) can answer that load with a different
// d3d11.dll, and on a Windows rig that was the whole of draws=0: the probe counted a context's entries against the
// wrong image, and reclaim, told the wrong module implements the context, never took back Windows' re-points.
//
// This rig stands in for that substitute with a COPY of a system DLL staged under the name d3d11.dll in a directory of
// its own and mapped first. It does not hook anything; it asserts the thing the fix rests on: given a handle that is
// not System32's d3d11.dll, whatever the loader would say about the name, the chooser finds the module whose FILE is
// System32's, and never answers with the substitute. Case ids, which tell a mutation that tripped the right check from
// one that tripped another:
//
//   S0  the process maps no d3d11.dll before the rig loads one (a rig whose process started with one cannot say
//       "not mapped yet", so it stops here instead of skipping S2)
//   S1  the decoy maps under the name d3d11.dll from a path that is not System32's
//   S2  System32's d3d11.dll NOT mapped: the walk finds nothing, and the chooser says "unresolved" for the decoy and
//       for no handle -- returning the handle it was given, unverified, never a guess
//   S3  System32's d3d11.dll maps by full path, beside the decoy: two modules of one name
//   S4  the handle held is System32's: chosen as it is, route "held"
//   S5  THE FIX: the handle held is the decoy: the chooser answers System32's module, verified, by-name or walked
//   S6  no handle held: System32's module again
//   S7  the walk tells the two same-named modules apart by path, and the module list shows both, in load order
//   S8  resolveSystemD3D11Exports: before System32 maps, preserves fallback procs; after it maps, re-resolves
//       D3D11CreateDevice and D3D11CreateDeviceAndSwapChain from System32 even when the held handle is the decoy
//   S9  resolveSystemD3D11Exports: chained-export fallback refuses selfModule and resolves missing exports from
//       System32 rather than the decoy
//   S10 resolveSystemD3D11ExportsFrom: missing exports on a verified handle preserve the existing fallback procs
//       rather than overwriting them with null
//
//   system_module_test.exe --dry-run             touches nothing
//   system_module_test.exe --self-test           stages the decoy under %TEMP% and removes it again
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "system_d3d11.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* id, const char* what) {
    std::printf("[system_module_test] %s %s: %s\n", ok ? "ok  " : "FAIL", id, what);
    if (!ok) ++g_failures;
}

bool samePath(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring modulePath(HMODULE m) {
    wchar_t actual[MAX_PATH]{};
    const DWORD length = m ? GetModuleFileNameW(m, actual, MAX_PATH) : 0;
    return length && length < MAX_PATH ? std::wstring(actual, length) : std::wstring();
}

// A copy of a system DLL under the name d3d11.dll, in a directory of the rig's own. Removed when it goes out of
// scope; the module must be unloaded first, which main does.
struct Decoy {
    std::wstring dir;
    std::wstring file;
    bool staged = false;

    bool stage() {
        wchar_t temp[MAX_PATH]{};
        const DWORD n = GetTempPathW(MAX_PATH, temp);
        if (!n || n >= MAX_PATH) return false;
        wchar_t sys[MAX_PATH]{};
        const UINT sn = GetSystemDirectoryW(sys, MAX_PATH);
        if (!sn || sn >= MAX_PATH) return false;
        dir = std::wstring(temp) + L"edvr_system_module_test_" + std::to_wstring(GetCurrentProcessId());
        file = dir + L"\\d3d11.dll";
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        // version.dll: small, and its imports resolve from anywhere, so the copy loads from this directory.
        staged = CopyFileW((std::wstring(sys) + L"\\version.dll").c_str(), file.c_str(), FALSE) != FALSE;
        return staged;
    }
    ~Decoy() {
        if (staged) DeleteFileW(file.c_str());
        if (!dir.empty()) RemoveDirectoryW(dir.c_str());
    }
};

int selfTest() {
    const std::wstring system = edvr::systemD3D11Path();
    check(!system.empty(), "S0", "the system directory is readable");
    if (system.empty()) return 1;
    if (!edvr::reportNoD3D11Mapped("system_module_test")) {
        check(false, "S0", "a d3d11.dll was already mapped when the rig started, so 'not mapped yet' cannot be shown");
        return 1;
    }
    check(true, "S0", "no d3d11.dll is mapped before the rig loads one");

    Decoy decoy;
    if (!decoy.stage()) {
        check(false, "S1", "could not stage the decoy d3d11.dll");
        return 1;
    }
    HMODULE decoyModule = LoadLibraryW(decoy.file.c_str());
    check(decoyModule != nullptr, "S1", "the decoy maps");
    if (!decoyModule) return 1;
    const std::wstring decoyPath = modulePath(decoyModule);
    check(samePath(decoyPath, decoy.file) && !samePath(decoyPath, system), "S1",
          "the decoy is a module named d3d11.dll whose file is not System32's");

    // S2: before System32's d3d11.dll is mapped there is nothing to find, and the chooser must say so.
    check(edvr::walkMappedD3D11AtPath(system) == nullptr, "S2", "the walk finds no System32 d3d11.dll while none is mapped");
    const edvr::SystemD3D11Choice early = edvr::chooseSystemD3D11(decoyModule);
    check(!early.verified && std::strcmp(early.route, "unresolved") == 0 && early.module == decoyModule, "S2",
          "with the decoy held and System32's unmapped: unresolved, the held handle returned as it is");
    const edvr::SystemD3D11Choice none = edvr::chooseSystemD3D11(nullptr);
    check(!none.verified && std::strcmp(none.route, "unresolved") == 0 && none.module == nullptr, "S2",
          "with no handle held and System32's unmapped: unresolved, no module");

    auto sentinelCreate = reinterpret_cast<FARPROC>(&samePath);
    auto sentinelSwap = reinterpret_cast<FARPROC>(&modulePath);
    const edvr::SystemD3D11Exports earlyExports =
        edvr::resolveSystemD3D11Exports(decoyModule, nullptr, sentinelCreate, sentinelSwap);
    check(!earlyExports.choice.verified && earlyExports.createDevice == sentinelCreate &&
              earlyExports.createDeviceAndSwapChain == sentinelSwap &&
              !earlyExports.createDeviceFromVerified && !earlyExports.createDeviceAndSwapChainFromVerified,
          "S8", "before System32 maps, resolveSystemD3D11Exports preserves fallback device-creation procs");
    check(earlyExports.fallbackModule == nullptr, "S9",
          "before System32 maps, a foreign loader handle is unavailable as chained fallback");

    const edvr::SystemD3D11Exports earlySelf =
        edvr::resolveSystemD3D11Exports(decoyModule, decoyModule, sentinelCreate, sentinelSwap);
    check(earlySelf.fallbackModule == nullptr, "S9",
          "when held equals selfModule and System32 is unmapped, chained fallback refuses selfModule");

    edvr::SystemD3D11Choice staleChoice{};
    staleChoice.module = decoyModule;
    staleChoice.route = "walked";
    staleChoice.verified = true;
    staleChoice.wantedPath = system;
    const bool pinned = edvr::pinVerifiedSystemD3D11(staleChoice, decoyModule);
    check(!pinned && !staleChoice.verified && staleChoice.module == decoyModule &&
              std::strcmp(staleChoice.route, "unresolved") == 0,
          "S9", "pinning rejects a discovered module whose current path differs from System32");

    // S3: System32's by full path, beside the decoy.
    HMODULE real = LoadLibraryW(system.c_str());
    check(real != nullptr && real != decoyModule && samePath(modulePath(real), system), "S3",
          "System32's d3d11.dll maps by full path as a second module of the name");
    if (!real || real == decoyModule) {
        FreeLibrary(decoyModule);
        return 1;
    }
    // Information, not an assertion: what a lookup of System32's full path answers with the decoy mapped first.
    HMODULE byName = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, system.c_str(), &byName) && byName) {
        std::printf("[system_module_test] info: a lookup of System32's full path answers with the %s\n",
                    byName == real ? "System32 module" : byName == decoyModule ? "DECOY" : "another module");
    } else {
        std::printf("[system_module_test] info: a lookup of System32's full path finds nothing\n");
    }

    const edvr::SystemD3D11Choice held = edvr::chooseSystemD3D11(real);
    check(held.verified && std::strcmp(held.route, "held") == 0 && held.module == real, "S4",
          "System32's module held: chosen as it is, route held");

    const edvr::SystemD3D11Choice fixed = edvr::chooseSystemD3D11(decoyModule);
    check(fixed.verified && fixed.module == real &&
              (std::strcmp(fixed.route, "by-name") == 0 || std::strcmp(fixed.route, "walked") == 0),
          "S5", "the decoy held: the chooser answers System32's module, verified, by-name or walked");
    check(samePath(fixed.heldPath, decoyPath) && samePath(fixed.wantedPath, system), "S5",
          "the choice names where the held handle came from and where System32's is");

    const edvr::SystemD3D11Choice fromNothing = edvr::chooseSystemD3D11(nullptr);
    check(fromNothing.verified && fromNothing.module == real, "S6", "no handle held: System32's module, verified");

    check(edvr::walkMappedD3D11AtPath(system) == real && edvr::walkMappedD3D11AtPath(decoyPath) == decoyModule, "S7",
          "the walk tells the two same-named modules apart by path");
    const std::vector<std::wstring> paths = edvr::mappedD3D11Paths();
    check(paths.size() == 2 && samePath(paths[0], decoyPath) && samePath(paths[1], system), "S7",
          "the module list shows the decoy then System32's, in load order");

    // S8: with System32 mapped beside the decoy, re-resolving from the decoy handle finds System32's real exports.
    const FARPROC realCreate = GetProcAddress(real, "D3D11CreateDevice");
    const FARPROC realSwap = GetProcAddress(real, "D3D11CreateDeviceAndSwapChain");
    check(realCreate != nullptr && realSwap != nullptr, "S8",
          "System32's d3d11.dll exports D3D11CreateDevice and D3D11CreateDeviceAndSwapChain");
    const edvr::SystemD3D11Exports fixedExports =
        edvr::resolveSystemD3D11Exports(decoyModule, decoyModule, sentinelCreate, sentinelSwap);
    check(fixedExports.choice.verified && fixedExports.choice.module == real &&
              fixedExports.createDevice == realCreate && fixedExports.createDeviceAndSwapChain == realSwap &&
              fixedExports.createDeviceFromVerified && fixedExports.createDeviceAndSwapChainFromVerified &&
              !fixedExports.missingCreateDevice && !fixedExports.missingCreateDeviceAndSwapChain,
          "S8", "with decoy held and System32 mapped, device-creation procs re-resolve to System32");

    // S9: chained fallback uses verified System32 (never decoy or self), so missing chained exports resolve.
    check(fixedExports.fallbackModule == real && fixedExports.fallbackModule != decoyModule, "S9",
          "with decoy held as selfModule and System32 mapped, chained fallback switches to verified System32");
    check(GetProcAddress(decoyModule, "D3D11CreateDevice") == nullptr &&
              GetProcAddress(fixedExports.fallbackModule, "D3D11CreateDevice") == realCreate &&
              GetProcAddress(fixedExports.fallbackModule, "D3D11CreateDeviceAndSwapChain") == realSwap,
          "S9", "exports absent from a partial chained module resolve through the verified System32 fallback");

    // S10: a verified module missing the D3D11CreateDevice* exports preserves fallback procs instead of nulling them.
    edvr::SystemD3D11Choice syntheticMissing;
    syntheticMissing.module = decoyModule;
    syntheticMissing.route = "walked";
    syntheticMissing.verified = true;
    const edvr::SystemD3D11Exports missingExports =
        edvr::resolveSystemD3D11ExportsFrom(syntheticMissing, nullptr, sentinelCreate, sentinelSwap);
    check(missingExports.missingCreateDevice && missingExports.missingCreateDeviceAndSwapChain &&
              missingExports.createDevice == sentinelCreate &&
              missingExports.createDeviceAndSwapChain == sentinelSwap &&
              !missingExports.createDeviceFromVerified && !missingExports.createDeviceAndSwapChainFromVerified,
          "S10", "missing exports on a verified handle preserve the existing fallback procs");

    FreeLibrary(real);
    FreeLibrary(decoyModule);
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) {
            std::printf("[system_module_test] dry run: would stage a copy of version.dll as d3d11.dll under %%TEMP%%, "
                        "map it and System32's d3d11.dll, and remove the copy. Nothing written.\n");
            return 0;
        }
        if (std::strcmp(argv[i], "--self-test") == 0) {
            const int rc = selfTest();
            std::printf("[system_module_test] %s (%d failure%s)\n", rc == 0 ? "PASS" : "FAIL", g_failures,
                        g_failures == 1 ? "" : "s");
            return rc;
        }
    }
    std::printf("usage: system_module_test.exe --dry-run | --self-test\n");
    return 2;
}
