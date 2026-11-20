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
