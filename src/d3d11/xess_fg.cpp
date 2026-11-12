#include "xess_fg.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <utility>

#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"

#ifndef EDVR_HAVE_XESS
#define EDVR_HAVE_XESS 0
#endif

#if EDVR_HAVE_XESS
#include <algorithm>
#include <array>
#include <cmath>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell_d3d12.h>
#include "load_proc.h"
#include "xess_engine.h"
using Microsoft::WRL::ComPtr;
#endif

namespace edvr {
namespace {

std::atomic<bool> g_bridged{false};
std::atomic<uint32_t> g_maxFrames{0};

// The first reason frame generation did not start or stopped, for the panel. Written once, then published.
std::atomic<int> g_problemState{0};   // 0 none, 1 being written, 2 readable
char g_problem[240] = "";

void noteProblem(const char* text) {
    int expected = 0;
    if (!g_problemState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
    std::snprintf(g_problem, sizeof(g_problem), "%s", text ? text : "unknown");
    g_problemState.store(2, std::memory_order_release);
}

bool wantedByConfig() { return Config::get().getBool("fix.xess_frame_generation", false); }

#if EDVR_HAVE_XESS

constexpr unsigned kRing = 3;
constexpr UINT kProxyBuffers = 3;
constexpr UINT kPlainBuffers = 2;
constexpr DWORD kGpuWaitMs = 5000;
constexpr DWORD kKeyWaitMs = 100;

// The two settings, re-read only when the configuration changes (Config::generation()), not on every Present.
// The render thread's: Present, the resolver's xessFgWanted.
struct FgSettings {
    uint32_t generation = ~0u;
    bool wanted = false;
    uint32_t frames = 1;
};
FgSettings g_settings;

const FgSettings& settings() {
    Config& cfg = Config::get();
    const uint32_t generation = cfg.generation();   // read before the values (config.h)
    if (generation != g_settings.generation) {
        g_settings.generation = generation;
        g_settings.wanted = cfg.getBool("fix.xess_frame_generation", false);
        g_settings.frames = static_cast<uint32_t>(cfg.getIntInRange("fix.xess_generated_frames", 1, 1, 3));
    }
    return g_settings;
}

struct FgApi {
    decltype(&xefgSwapChainGetVersion) getVersion = nullptr;
    decltype(&xefgSwapChainD3D12CreateContext) createContext = nullptr;
    decltype(&xefgSwapChainD3D12BuildPipelines) buildPipelines = nullptr;
    decltype(&xefgSwapChainD3D12GetProperties) getProperties = nullptr;
    decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) initFromDesc = nullptr;
    decltype(&xefgSwapChainD3D12GetSwapChainPtr) getSwapChainPtr = nullptr;
    decltype(&xefgSwapChainD3D12GetInitializationParameters) getInitParams = nullptr;
    decltype(&xefgSwapChainD3D12TagFrameResource) tagResource = nullptr;
    decltype(&xefgSwapChainTagFrameConstants) tagConstants = nullptr;
    decltype(&xefgSwapChainSetEnabled) setEnabled = nullptr;
    decltype(&xefgSwapChainSetPresentId) setPresentId = nullptr;
    decltype(&xefgSwapChainGetLastPresentStatus) lastStatus = nullptr;
    decltype(&xefgSwapChainSetLoggingCallback) setLogging = nullptr;
    decltype(&xefgSwapChainDestroy) destroy = nullptr;
    decltype(&xefgSwapChainSetLatencyReduction) setLatency = nullptr;
    decltype(&xefgSwapChainSetNumInterpolatedFrames) setFrames = nullptr;
};

struct LlApi {
    decltype(&xellD3D12CreateContext) createContext = nullptr;
    decltype(&xellDestroyContext) destroy = nullptr;
    decltype(&xellSetSleepMode) setSleepMode = nullptr;
    decltype(&xellSleep) sleep = nullptr;
    decltype(&xellAddMarkerData) marker = nullptr;
    decltype(&xellGetVersion) getVersion = nullptr;
    decltype(&xellSetLoggingCallback) setLogging = nullptr;
};

struct CommandSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t doneValue = 0;   // the out fence's value once this slot's last submission finished
};

struct Bridge {
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue;
    // The bridge's own fences, each signalled by one queue only, so neither value can go backwards: `in` by D3D11
    // (Elite's frame is in the shared texture), `out` by D3D12 (the copy into the proxy finished; D3D11 and the CPU
    // wait on it). XeSS-SR's fence and counter are never touched.
    ComPtr<ID3D12Fence> in12, out12;
    ComPtr<ID3D11Fence> in11, out11;
    uint64_t inValue = 0, outValue = 0;
    uint64_t lastOut = 0;            // the out value of the last copy that reads the shared texture
    HANDLE outEvent = nullptr;
    HMODULE fgModule = nullptr, llModule = nullptr, dxgiModule = nullptr;
    FgApi fg;
    LlApi ll;
    xefg_swapchain_handle_t fgContext = nullptr;
    xell_context_handle_t llContext = nullptr;
    uint32_t initFlags = 0;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain4> proxy;
    // After a fault: a plain D3D11 flip swap chain on the real window, which Elite's frame is copied into instead.
    ComPtr<IDXGISwapChain1> plain;
    bool fellBack = false;
    bool tearing = false;
    bool presenterFullscreen = false;
    HWND hidden = nullptr, real = nullptr;
    bool keepHiddenWindow = false;   // the old swap chain may still live on it (xessFgRelease)
    IDXGISwapChain* hiddenSwap = nullptr;   // Elite's; not owned
    // Elite's back buffer, copied each Present into this texture both APIs see.
    ComPtr<ID3D11Texture2D> shared11;
    ComPtr<IDXGIKeyedMutex> sharedKey;
    ComPtr<ID3D12Resource> shared12;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;   // the proxy's (an sRGB request mapped to its UNORM twin)
    std::array<CommandSlot, kRing> ring;
    unsigned nextSlot = 0;
    // XeLL frame ids and XeSS-FG present ids are the same counter.
    uint32_t frameId = 1;
    uint32_t startedId = 0;          // the id xellSleep last ran for
    uint32_t taggedId = 0;           // the id motion and depth were last tagged for
    unsigned taggedRun = 0;          // consecutive presents that had inputs
    int enabledState = -1;           // what xefgSwapChainSetEnabled was last told; -1 unknown
    uint32_t framesState = 0;        // what xefgSwapChainSetNumInterpolatedFrames was last told
    // The camera the resolver noted for this frame.
    float rows[4][4] = {};
    bool haveRows = false;
    float frameMs = 0.f;
    uint64_t presents = 0, statusFailures = 0, skipped = 0;
    bool flagsNoted = false, deviceNoted = false;
    bool failed = false;   // a fault: frame generation stops and the plain swap chain takes the window
    ~Bridge() {
        // Order matters: the proxy before XeSS-FG's context, XeSS-FG before XeLL (the guide's shutdown order).
        proxy.Reset();
        if (fgContext && fg.destroy) fg.destroy(fgContext);
        if (llContext && ll.destroy) ll.destroy(llContext);
        plain.Reset();
        for (CommandSlot& slot : ring) { slot.list.Reset(); slot.allocator.Reset(); }
        shared12.Reset(); sharedKey.Reset(); shared11.Reset();
        factory.Reset();
        in11.Reset(); out11.Reset(); in12.Reset(); out12.Reset();
        queue.Reset(); d12.Reset(); ctx4.Reset(); context.Reset(); d11.Reset();
        if (outEvent) CloseHandle(outEvent);
        if (hidden && !keepHiddenWindow) DestroyWindow(hidden);
        // The modules stay loaded: a late callback from either library must still find its code.
    }
};

// A raw owner, as xess_engine.cpp's State: no static destructor runs D3D12 teardown under the loader lock at exit.
Bridge* g_bridge = nullptr;

IDXGISwapChain* presenter(const Bridge& b) {
    if (b.plain) return b.plain.Get();
    if (b.proxy && !b.fellBack) return b.proxy.Get();
    return nullptr;
}

const char* fgResultName(xefg_swapchain_result_t r) {
    switch (r) {
    case XEFG_SWAPCHAIN_RESULT_SUCCESS: return "success";
    case XEFG_SWAPCHAIN_RESULT_WARNING_OLD_DRIVER: return "warning-old-driver";
    case XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES: return "warning-too-few-frames";
    case XEFG_SWAPCHAIN_RESULT_WARNING_FRAMES_ID_MISMATCH: return "warning-frame-id-mismatch";
    case XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS: return "warning-missing-present-status";
    case XEFG_SWAPCHAIN_RESULT_WARNING_RESOURCE_SIZES_MISMATCH: return "warning-resource-sizes-mismatch";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported-device";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported-driver";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT: return "invalid-argument";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "out-of-memory";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED: return "unsupported";
    case XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES: return "mismatch-input-resources";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES: return "incorrect-input-resources";
    case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED: return "latency-reduction-unsupported";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DXGI_INVALID_CALL: return "dxgi-invalid-call";
    case XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER: return "wrong-call-order";
    default: return "error";
    }
}

void fgLog(const char* message, xefg_swapchain_logging_level_t level, void*) {
    static std::atomic<unsigned> lines{0};
    if (lines.fetch_add(1, std::memory_order_relaxed) >= 400) return;   // a chatty failure must not fill the log
    Log::get().note("xess-fg: library %s: %s", level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR ? "error" :
                    level == XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING ? "warning" : "info", message ? message : "");
}

void llLog(const char* message, xell_logging_level_t level) {
    static std::atomic<unsigned> lines{0};
    if (lines.fetch_add(1, std::memory_order_relaxed) >= 200) return;
    Log::get().note("xess-fg: XeLL %s: %s", level >= XELL_LOGGING_LEVEL_ERROR ? "error" :
                    level == XELL_LOGGING_LEVEL_WARNING ? "warning" : level == XELL_LOGGING_LEVEL_INFO ? "info" : "debug",
                    message ? message : "");
}

// A fault while the bridge runs: logged once, and the next step falls back to the plain swap chain (fallBack).
void fault(Bridge& b, const char* format, ...) {
    if (b.failed) return;
    b.failed = true;
    char text[240];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Log::get().note("xess-fg: %s; frame generation stops for this session, and Elite's frame goes to the window "
                    "through a plain D3D11 swap chain", text);
    char problem[240];
    std::snprintf(problem, sizeof(problem), "stopped: %s", text);
    noteProblem(problem);
}

// A library beside this module, never one found on the search path or already loaded from elsewhere.
HMODULE loadBeside(const wchar_t* name, char* why, size_t whySize) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&xessFgBridged), &self) || !self) {
        std::snprintf(why, whySize, "EDVR module could not be located (Win32=%lu)", GetLastError());
        return nullptr;
    }
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* const slash = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (!slash || size_t(slash + 1 - path) + wcslen(name) + 1 > MAX_PATH) {
        std::snprintf(why, whySize, "EDVR module path could not be read");
        return nullptr;
    }
    wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - path), name);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        std::snprintf(why, whySize, "%ls is missing beside EDVR", name);
        return nullptr;
    }
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        std::snprintf(why, whySize, "%ls beside EDVR failed to load (Win32=%lu)", name, GetLastError());
        return nullptr;
    }
    wchar_t loaded[MAX_PATH]{};
    const DWORD loadedLength = GetModuleFileNameW(module, loaded, MAX_PATH);
    if (!loadedLength || loadedLength >= MAX_PATH || _wcsicmp(loaded, path) != 0) {
        std::snprintf(why, whySize, "%ls resolved to another module", name);
        return nullptr;   // left loaded: another copy owns it
    }
    return module;
}

// Loads both libraries and resolves every export used. Fills `why` and returns false on any miss.
bool loadLibraries(Bridge& b, char* why, size_t whySize) {
    if (!b.fgModule) b.fgModule = loadBeside(L"libxess_fg.dll", why, whySize);
    if (!b.fgModule) return false;
    if (!b.llModule) b.llModule = loadBeside(L"libxell.dll", why, whySize);
    if (!b.llModule) return false;
    FgApi& f = b.fg;
    if (!loadProc(b.fgModule, "xefgSwapChainGetVersion", f.getVersion) ||
        !loadProc(b.fgModule, "xefgSwapChainD3D12CreateContext", f.createContext) ||
        !loadProc(b.fgModule, "xefgSwapChainD3D12BuildPipelines", f.buildPipelines) ||
        !loadProc(b.fgModule, "xefgSwapChainD3D12GetProperties", f.getProperties) ||
        !loadProc(b.fgModule, "xefgSwapChainD3D12InitFromSwapChainDesc", f.initFromDesc) ||
        !loadProc(b.fgModule, "xefgSwapChainD3D12GetSwapChainPtr", f.getSwapChainPtr) ||
        !loadProc(b.fgModule, "xefgSwapChainD3D12GetInitializationParameters", f.getInitParams) ||
        !loadProc(b.fgModule, "xefgSwapChainD3D12TagFrameResource", f.tagResource) ||
        !loadProc(b.fgModule, "xefgSwapChainTagFrameConstants", f.tagConstants) ||
        !loadProc(b.fgModule, "xefgSwapChainSetEnabled", f.setEnabled) ||
        !loadProc(b.fgModule, "xefgSwapChainSetPresentId", f.setPresentId) ||
        !loadProc(b.fgModule, "xefgSwapChainGetLastPresentStatus", f.lastStatus) ||
        !loadProc(b.fgModule, "xefgSwapChainSetLoggingCallback", f.setLogging) ||
        !loadProc(b.fgModule, "xefgSwapChainDestroy", f.destroy) ||
        !loadProc(b.fgModule, "xefgSwapChainSetLatencyReduction", f.setLatency) ||
        !loadProc(b.fgModule, "xefgSwapChainSetNumInterpolatedFrames", f.setFrames)) {
        std::snprintf(why, whySize, "libxess_fg.dll lacks a required export");
        return false;
    }
    LlApi& l = b.ll;
    if (!loadProc(b.llModule, "xellD3D12CreateContext", l.createContext) ||
        !loadProc(b.llModule, "xellDestroyContext", l.destroy) ||
        !loadProc(b.llModule, "xellSetSleepMode", l.setSleepMode) ||
        !loadProc(b.llModule, "xellSleep", l.sleep) ||
        !loadProc(b.llModule, "xellAddMarkerData", l.marker) ||
        !loadProc(b.llModule, "xellGetVersion", l.getVersion) ||
        !loadProc(b.llModule, "xellSetLoggingCallback", l.setLogging)) {
        std::snprintf(why, whySize, "libxell.dll lacks a required export");
        return false;
    }
    xefg_swapchain_version_t fv{};
    xell_version_t lv{};
    if (f.getVersion(&fv) != XEFG_SWAPCHAIN_RESULT_SUCCESS || fv.major != 1 || fv.minor < 3) {
        std::snprintf(why, whySize, "libxess_fg.dll version %u.%u.%u is not 1.3 or later 1.x", fv.major, fv.minor, fv.patch);
        return false;
    }
    if (l.getVersion(&lv) != XELL_RESULT_SUCCESS || lv.major != 1 || lv.minor < 3) {
        std::snprintf(why, whySize, "libxell.dll version %u.%u.%u is not 1.3 or later 1.x", lv.major, lv.minor, lv.patch);
        return false;
    }
    Log::get().note("xess-fg: libraries loaded: XeSS-FG %u.%u.%u, XeLL %u.%u.%u", fv.major, fv.minor, fv.patch,
                    lv.major, lv.minor, lv.patch);
    return true;
}

// The XeSS-SR D3D12 device and queue, and the bridge's own fences and CPU event on them.
bool openD3D12(Bridge& b, IUnknown* device, char* why, size_t whySize) {
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(b.d11.GetAddressOf())))) {
        std::snprintf(why, whySize, "the swap chain's device is not a D3D11 device");
        return false;
    }
    b.d11->GetImmediateContext(b.context.GetAddressOf());
    if (!b.context || FAILED(b.context.As(&b.ctx4))) {
        std::snprintf(why, whySize, "the D3D11 context has no ID3D11DeviceContext4");
        return false;
    }
    XessD3D12Bridge shared;
    const char* reason = nullptr;
    if (!xessBridgeOpen(b.d11.Get(), &shared, &reason)) {
        std::snprintf(why, whySize, "XeSS D3D12 bridge unavailable: %s", reason ? reason : "unknown");
        return false;
    }
    b.d12.Attach(shared.d12);
    b.queue.Attach(shared.queue);
    if (!xessBridgeMakeFence(b.d11.Get(), b.d12.Get(), b.in12.GetAddressOf(), b.in11.GetAddressOf(), &reason) ||
        !xessBridgeMakeFence(b.d11.Get(), b.d12.Get(), b.out12.GetAddressOf(), b.out11.GetAddressOf(), &reason)) {
        std::snprintf(why, whySize, "bridge fence: %s", reason ? reason : "unknown");
        return false;
    }
    b.outEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!b.outEvent) {
        std::snprintf(why, whySize, "bridge fence event failed (Win32=%lu)", GetLastError());
        return false;
    }
    return true;
}

// Blocks (bounded) until the out fence reaches `value`. Fills `why` on a timeout or a removed device.
bool waitOut(Bridge& b, uint64_t value, char* why, size_t whySize) {
    if (!value) return true;
    uint64_t completed = b.out12->GetCompletedValue();
    if (completed == UINT64_MAX) {
        std::snprintf(why, whySize, "D3D12 device removed (reason 0x%08X)", unsigned(b.d12->GetDeviceRemovedReason()));
        return false;
    }
    if (completed >= value) return true;
    ResetEvent(b.outEvent);
    const HRESULT hr = b.out12->SetEventOnCompletion(value, b.outEvent);
    if (FAILED(hr)) {
        std::snprintf(why, whySize, "SetEventOnCompletion failed 0x%08X", unsigned(hr));
        return false;
    }
    const DWORD result = WaitForSingleObject(b.outEvent, kGpuWaitMs);
    completed = b.out12->GetCompletedValue();
    if (result != WAIT_OBJECT_0 || completed < value || completed == UINT64_MAX) {
        std::snprintf(why, whySize, "the D3D12 queue did not reach %llu within %lu ms (completed %llu)",
                      static_cast<unsigned long long>(value), static_cast<unsigned long>(kGpuWaitMs),
                      static_cast<unsigned long long>(completed));
        return false;
    }
    return true;
}

// Waits until the queue has finished everything submitted so far (resize, fallback, teardown).
bool drainQueue(Bridge& b, char* why, size_t whySize) {
    if (!b.queue || !b.out12) return true;
    const uint64_t value = ++b.outValue;
    const HRESULT hr = b.queue->Signal(b.out12.Get(), value);
    if (FAILED(hr)) {
        std::snprintf(why, whySize, "queue Signal failed 0x%08X", unsigned(hr));
        return false;
    }
    return waitOut(b, value, why, whySize);
}

bool drainQueue(Bridge& b) {
    char why[200] = "";
    if (drainQueue(b, why, sizeof(why))) return true;
    Log::get().note("xess-fg: queue drain failed: %s", why);
    return false;
}

uint32_t clampFrames(uint32_t wanted) {
    const uint32_t max = g_maxFrames.load(std::memory_order_relaxed);
    if (wanted < 1) wanted = 1;
    if (max && wanted > max) wanted = max;
    return wanted;
}

DXGI_FORMAT proxyFormatFor(UINT format) {
    switch (static_cast<DXGI_FORMAT>(format)) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;   // FP16 scRGB and anything else: XeSS-FG takes none of them
    }
}

// The plain fallback swap chain's format for Elite's back buffer: what a flip-model swap chain accepts, in the
// same copy family. FP16 is fine here; it is only XeSS-FG that refuses it.
DXGI_FORMAT plainFormatFor(UINT format) {
    switch (static_cast<DXGI_FORMAT>(format)) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return proxyFormatFor(format);
    }
}

// Elite's flags for a flip-model swap chain: DO_NOT_SEQUENCE is the blt model's alone (a flip-model Present with it
// returns DXGI_ERROR_INVALID_CALL), so it is dropped; tearing is allowed for an unsynchronised windowed present.
UINT presentFlagsFor(Bridge& b, UINT syncInterval, UINT flags) {
    if (flags & DXGI_PRESENT_DO_NOT_SEQUENCE) {
        flags &= ~static_cast<UINT>(DXGI_PRESENT_DO_NOT_SEQUENCE);
        if (!b.flagsNoted) {
            b.flagsNoted = true;
            Log::get().note("xess-fg: Elite's Present asked for DXGI_PRESENT_DO_NOT_SEQUENCE, which a flip-model swap "
                            "chain refuses; dropped");
        }
    }
    if (syncInterval == 0 && b.tearing && !b.presenterFullscreen) flags |= DXGI_PRESENT_ALLOW_TEARING;
    return flags;
}

void clientSize(HWND window, UINT* width, UINT* height) {
    RECT rect{};
    if (window && GetClientRect(window, &rect)) {
        if (width && !*width) *width = static_cast<UINT>(rect.right - rect.left);
        if (height && !*height) *height = static_cast<UINT>(rect.bottom - rect.top);
    }
    if (width && !*width) *width = 8;
    if (height && !*height) *height = 8;
}

HWND makeHiddenWindow() {
    static ATOM atom = 0;
    HINSTANCE instance = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&xessFgBridged), &instance);
    if (!atom) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = L"EDVR.XeSSFG.Hidden";
        atom = RegisterClassExW(&wc);
        if (!atom) return nullptr;
    }
    // Never shown: Elite's D3D11 swap chain lives on it and is never presented.
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, MAKEINTATOM(atom), L"EDVR XeSS-FG", WS_POPUP,
                           0, 0, 8, 8, nullptr, nullptr, instance, nullptr);
}

bool makeRing(Bridge& b, char* why, size_t whySize) {
    for (CommandSlot& slot : b.ring) {
        HRESULT hr = b.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.allocator.GetAddressOf()));
        if (SUCCEEDED(hr))
            hr = b.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                          IID_PPV_ARGS(slot.list.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = slot.list->Close();
        if (FAILED(hr)) {
            std::snprintf(why, whySize, "D3D12 command list failed 0x%08X", unsigned(hr));
            return false;
        }
    }
    return true;
}

bool makeShared(Bridge& b, char* why, size_t whySize) {
    b.shared12.Reset(); b.sharedKey.Reset(); b.shared11.Reset();
    const char* reason = nullptr;
    if (!xessBridgeShareTexture(b.d11.Get(), b.d12.Get(), b.width, b.height, static_cast<unsigned>(b.format),
                                b.shared11.GetAddressOf(), b.sharedKey.GetAddressOf(), b.shared12.GetAddressOf(), &reason)) {
        std::snprintf(why, whySize, "shared back buffer: %s", reason ? reason : "unknown");
        return false;
    }
    return true;
}

// Elite's back buffer and its description. The hidden swap chain is never presented, so buffer 0 is always the one
// Elite drew into, whatever its swap effect.
bool sourceBuffer(IDXGISwapChain* hidden, ComPtr<ID3D11Texture2D>& back, D3D11_TEXTURE2D_DESC* desc) {
    if (!hidden || FAILED(hidden->GetBuffer(0, IID_PPV_ARGS(back.ReleaseAndGetAddressOf()))) || !back) return false;
    back->GetDesc(desc);
    return true;
}

// The proxy and the shared texture follow Elite's back buffer: after each resize, and at Present if a resize was
// missed. A format XeSS-FG cannot present is a fault (the plain swap chain takes FP16).
void syncProxy(Bridge& b, UINT width, UINT height, UINT sourceFormat) {
    if (b.failed || !b.proxy) return;
    const DXGI_FORMAT format = proxyFormatFor(sourceFormat);
    if (format == DXGI_FORMAT_UNKNOWN) {
        fault(b, "Elite's back buffer is now format %u, which XeSS-FG does not present (RGBA8, BGRA8, RGB10A2)", sourceFormat);
        return;
    }
    if (width == b.width && height == b.height && format == b.format && b.shared11) return;
    char why[240] = "";
    // The proxy's back buffers are referenced by submitted copies; XeSS-FG asks for those to finish first.
    if (!drainQueue(b, why, sizeof(why))) {
        fault(b, "queue drain before the resize to %ux%u failed: %s", width, height, why);
        return;
    }
    b.shared12.Reset(); b.sharedKey.Reset(); b.shared11.Reset();
    const HRESULT hr = b.proxy->ResizeBuffers(0, width, height, format, b.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        fault(b, "the proxy refused the resize to %ux%u format=%u (0x%08X)", width, height, unsigned(format), unsigned(hr));
        return;
    }
    b.width = width;
    b.height = height;
    b.format = format;
    if (!makeShared(b, why, sizeof(why))) {
        fault(b, "resize to %ux%u: %s", width, height, why);
        return;
    }
    b.enabledState = -1;   // re-assert after the size change; XeSS-FG resets its history across it
    Log::get().note("xess-fg: resized to %ux%u format=%u", width, height, unsigned(format));
}

// The plain swap chain follows Elite's back buffer the same way.
bool syncPlain(Bridge& b, UINT width, UINT height, UINT sourceFormat) {
    if (!b.plain) return false;
    const DXGI_FORMAT format = plainFormatFor(sourceFormat);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(b.plain->GetDesc1(&desc)) || format == DXGI_FORMAT_UNKNOWN) return false;
    if (desc.Width == width && desc.Height == height && desc.Format == format) return true;
    const HRESULT hr = b.plain->ResizeBuffers(0, width, height, format, desc.Flags);
    static unsigned lines = 0;
    if (lines++ < 8)
        Log::get().note("xess-fg: plain swap chain resized to %ux%u format=%u (0x%08X)", width, height, unsigned(format), unsigned(hr));
    return SUCCEEDED(hr);
}

// After a fault: frame generation, XeLL and the proxy go; a plain D3D11 flip swap chain on the real window takes
// Elite's frame from here. Runs once.
void fallBack(Bridge& b) {
    if (b.fellBack) return;
    b.fellBack = true;
    xessSetInputTagger(nullptr);
    if (b.proxy && b.presenterFullscreen) b.proxy->SetFullscreenState(FALSE, nullptr);
    b.presenterFullscreen = false;
    drainQueue(b);
    for (CommandSlot& slot : b.ring) { slot.list.Reset(); slot.allocator.Reset(); slot.doneValue = 0; }
    b.shared12.Reset(); b.sharedKey.Reset(); b.shared11.Reset();
    b.proxy.Reset();
    if (b.fgContext) { b.fg.destroy(b.fgContext); b.fgContext = nullptr; }
    if (b.llContext) { b.ll.destroy(b.llContext); b.llContext = nullptr; }
    ComPtr<ID3D11Texture2D> back;
    D3D11_TEXTURE2D_DESC source{};
    const bool haveSource = sourceBuffer(b.hiddenSwap, back, &source);
    back.Reset();
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = haveSource ? source.Width : b.width;
    desc.Height = haveSource ? source.Height : b.height;
    desc.Format = plainFormatFor(haveSource ? static_cast<UINT>(source.Format) : static_cast<UINT>(b.format));
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kPlainBuffers;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = b.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    // The bridge's own factory, not the game's: device_hook.cpp's creation hook forwards it untouched.
    const HRESULT hr = b.factory && desc.Format != DXGI_FORMAT_UNKNOWN
        ? b.factory->CreateSwapChainForHwnd(b.d11.Get(), b.real, &desc, nullptr, nullptr, b.plain.GetAddressOf())
        : E_FAIL;
    if (FAILED(hr)) b.plain.Reset();
    Log::get().note("xess-fg: plain D3D11 swap chain on window %p %ux%u format=%u: 0x%08X%s", static_cast<void*>(b.real),
                    desc.Width, desc.Height, unsigned(desc.Format), unsigned(hr),
                    FAILED(hr) ? "; nothing can reach the window now" : "");
}

HRESULT plainPresent(Bridge& b, IDXGISwapChain* hidden, UINT syncInterval, UINT flags) {
    if (!b.plain) return S_OK;   // fallBack logged that nothing can reach the window
    ComPtr<ID3D11Texture2D> back, target;
    D3D11_TEXTURE2D_DESC source{};
    if (sourceBuffer(hidden, back, &source) && syncPlain(b, source.Width, source.Height, source.Format) &&
        SUCCEEDED(b.plain->GetBuffer(0, IID_PPV_ARGS(target.GetAddressOf()))))
        b.context->CopyResource(target.Get(), back.Get());
    target.Reset();
    back.Reset();
    return b.plain->Present(syncInterval, presentFlagsFor(b, syncInterval, flags));
}

void llMarker(Bridge& b, uint32_t id, xell_latency_marker_type_t marker) {
    if (b.llContext) b.ll.marker(b.llContext, id, marker);
}

// XeLL wants, per frame id: sleep, then the simulation, render submit and present markers in order. Elite's
// simulation and render submission interleave on one thread, so the whole CPU frame is reported as both.
void startFrame(Bridge& b) {
    if (!b.llContext || b.startedId == b.frameId) return;
    b.startedId = b.frameId;
    b.ll.sleep(b.llContext, b.frameId);
    llMarker(b, b.frameId, XELL_SIMULATION_START);
    llMarker(b, b.frameId, XELL_SIMULATION_END);
    llMarker(b, b.frameId, XELL_RENDERSUBMIT_START);
}

// The camera, split into the view and projection XeSS-FG asks for (row-vector, as DirectXMath stores them).
// The resolver's rows are clip = [p,1] * M with a symmetric reversed infinite projection: columns 0 and 1 are
// the right and up axes scaled by the projection's x and y scales, column 3 is the forward axis (w = view z),
// and column 2 is the near plane alone. So M = V * P with V = [right | up | forward] and P that diagonal.
void cameraMatrices(const Bridge& b, float view[16], float projection[16]) {
    std::memset(view, 0, 16 * sizeof(float));
    std::memset(projection, 0, 16 * sizeof(float));
    const float (*m)[4] = b.rows;
    const float sx = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0]);
    const float sy = std::sqrt(m[0][1] * m[0][1] + m[1][1] * m[1][1] + m[2][1] * m[2][1]);
    if (!b.haveRows || !(sx > 0.f) || !(sy > 0.f) || !std::isfinite(sx) || !std::isfinite(sy)) {
        view[0] = view[5] = view[10] = view[15] = 1.f;
        projection[0] = projection[5] = projection[10] = projection[15] = 1.f;
        return;
    }
    for (int k = 0; k < 4; ++k) {
        view[k * 4 + 0] = m[k][0] / sx;
        view[k * 4 + 1] = m[k][1] / sy;
        view[k * 4 + 2] = m[k][3];
        view[k * 4 + 3] = k == 3 ? 1.f : 0.f;
    }
    projection[0] = sx;
    projection[5] = sy;
    projection[11] = 1.f;          // w = view z
    projection[14] = m[3][2];      // z = near: reversed infinite depth near / z
}

void tagInputs(ID3D12GraphicsCommandList* list, ID3D12Resource* motion, ID3D12Resource* depth,
               uint32_t width, uint32_t height, float jitterX, float jitterY, bool reset) {
    Bridge* b = g_bridge;
    if (!b || !b->proxy || b->failed || b->fellBack || !runtimeFlatProfile() || !list || !motion || !depth) return;
    // XeSS-SR may have been restarted on another D3D12 device (xessShutdown on a device change); XeSS-FG's context
    // belongs to the one the bridge opened, so a list from any other device is not tagged.
    ComPtr<ID3D12Device> listDevice;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(listDevice.GetAddressOf()))) || listDevice.Get() != b->d12.Get()) {
        if (!b->deviceNoted) {
            b->deviceNoted = true;
            Log::get().note("xess-fg: XeSS-SR now runs on another D3D12 device; frame generation gets no inputs from it");
        }
        return;
    }
    const uint32_t id = b->frameId;
    xefg_swapchain_d3d12_resource_data_t data{};
    data.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;   // XeSS-SR's next evaluate overwrites both; XeSS-FG copies them on this list
    data.resourceBase = {0, 0};
    data.resourceSize = {width, height};
    data.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    data.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    data.pResource = motion;
    const xefg_swapchain_result_t rm = b->fg.tagResource(b->fgContext, list, id, &data);
    data.type = XEFG_SWAPCHAIN_RES_DEPTH;
    data.pResource = depth;
    const xefg_swapchain_result_t rd = b->fg.tagResource(b->fgContext, list, id, &data);
    xefg_swapchain_frame_constant_data_t constants{};
    cameraMatrices(*b, constants.viewMatrix, constants.projectionMatrix);
    constants.jitterOffsetX = jitterX;   // render pixels, positive right/down, as XeSS-SR takes them
    constants.jitterOffsetY = jitterY;
    constants.motionVectorScaleX = 1.f;  // current -> previous, in render pixels
    constants.motionVectorScaleY = 1.f;
    constants.resetHistory = reset || !b->haveRows ? 1u : 0u;
    constants.frameRenderTime = b->frameMs;
    const xefg_swapchain_result_t rc = b->fg.tagConstants(b->fgContext, id, &constants);
    if (rm < 0 || rd < 0 || rc < 0) {
        static unsigned lines = 0;
        if (lines++ < 8)
            Log::get().note("xess-fg: tagging frame %u failed: motion=%s depth=%s constants=%s", id, fgResultName(rm),
                            fgResultName(rd), fgResultName(rc));
        return;
    }
    b->taggedId = id;
    b->haveRows = false;   // a frame whose resolver noted no camera resets rather than reuse this one
}

// Drops the bridge. `keepWindow`: the hidden window stays, because the swap chain made on it may still be alive.
void teardown(bool keepWindow) {
    xessSetInputTagger(nullptr);
    Bridge* b = g_bridge;
    g_bridge = nullptr;
    g_bridged.store(false, std::memory_order_relaxed);
    if (!b) return;
    if (IDXGISwapChain* shown = presenter(*b)) {
        if (b->presenterFullscreen) shown->SetFullscreenState(FALSE, nullptr);
    }
    drainQueue(*b);
    b->keepHiddenWindow = keepWindow;
    delete b;
}

// The copy of Elite's back buffer into the shared texture, on the D3D11 queue. False when this frame has no copy:
// a fault (b.failed set) or a key not taken in time (the frame is then not presented, so the window never shows a
// proxy buffer from an earlier frame).
bool copyIn(Bridge& b, ID3D11Texture2D* back, const char** skipReason) {
    if (!b.shared11) { *skipReason = "no shared texture"; return false; }
    if (b.sharedKey) {
        const HRESULT key = b.sharedKey->AcquireSync(0, kKeyWaitMs);
        // AcquireSync reports WAIT_TIMEOUT and WAIT_ABANDONED as success HRESULTs: only S_OK means the key is held. An
        // abandoned key is held too and is given back, as xess_engine.cpp's takeKeys does.
        if (key == static_cast<HRESULT>(WAIT_ABANDONED)) b.sharedKey->ReleaseSync(0);
        if (key != S_OK) {
            *skipReason = key == static_cast<HRESULT>(WAIT_TIMEOUT) ? "keyed mutex timeout" :
                          key == static_cast<HRESULT>(WAIT_ABANDONED) ? "keyed mutex abandoned" : "keyed mutex refused";
            return false;
        }
    }
    // The last copy's D3D12 read of the shared texture must finish before this write. A GPU wait here, not after
    // that read was submitted, so Elite's own rendering of this frame never waited on the D3D12 queue.
    HRESULT hr = b.lastOut ? b.ctx4->Wait(b.out11.Get(), b.lastOut) : S_OK;
    if (SUCCEEDED(hr)) b.context->CopyResource(b.shared11.Get(), back);
    if (b.sharedKey) b.sharedKey->ReleaseSync(0);
    if (SUCCEEDED(hr)) hr = b.ctx4->Signal(b.in11.Get(), ++b.inValue);
    if (FAILED(hr)) {
        fault(b, "the D3D11 side of the handoff failed 0x%08X", unsigned(hr));
        return false;
    }
    b.context->Flush();
    return true;
}

// The copy from the shared texture into the proxy's current back buffer, on the D3D12 queue after the D3D11 copy.
void submitToProxy(Bridge& b) {
    CommandSlot& slot = b.ring[b.nextSlot++ % kRing];
    char why[240] = "";
    if (!waitOut(b, slot.doneValue, why, sizeof(why))) {
        fault(b, "the copy slot did not come free: %s", why);
        return;
    }
    ComPtr<ID3D12Resource> target;
    HRESULT hr = slot.allocator->Reset();
    if (SUCCEEDED(hr)) hr = slot.list->Reset(slot.allocator.Get(), nullptr);
    if (FAILED(hr)) {
        fault(b, "the copy list could not be reset 0x%08X", unsigned(hr));
        return;
    }
    hr = b.proxy->GetBuffer(b.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(target.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = b.queue->Wait(b.in12.Get(), b.inValue);
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = b.shared12.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.pResource = target.Get();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[0].Transition.Subresource = barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        slot.list->ResourceBarrier(2, barriers);
        slot.list->CopyResource(target.Get(), b.shared12.Get());
        std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
        std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
        slot.list->ResourceBarrier(2, barriers);
        hr = slot.list->Close();
    } else {
        slot.list->Close();
    }
    if (FAILED(hr)) {
        fault(b, "the copy into the proxy failed 0x%08X", unsigned(hr));
        return;
    }
    ID3D12CommandList* lists[] = {slot.list.Get()};
    b.queue->ExecuteCommandLists(1, lists);
    const uint64_t done = ++b.outValue;
    hr = b.queue->Signal(b.out12.Get(), done);
    if (FAILED(hr)) {
        fault(b, "the copy's queue Signal failed 0x%08X", unsigned(hr));
        return;
    }
    slot.doneValue = done;
    b.lastOut = done;   // the next frame's D3D11 copy into the shared texture waits for this
}

#endif  // EDVR_HAVE_XESS

}  // namespace

bool xessFgBridged() { return g_bridged.load(std::memory_order_relaxed); }
uint32_t xessFgMaxGeneratedFrames() { return g_maxFrames.load(std::memory_order_relaxed); }
const char* xessFgProblem() { return g_problemState.load(std::memory_order_acquire) == 2 ? g_problem : nullptr; }

#if !EDVR_HAVE_XESS

bool xessFgWanted() { return false; }
HWND xessFgBeginCreate(IUnknown*, HWND, UINT*, UINT*, UINT) {
    if (runtimeLaunchProfile() == RuntimeProfile::Flat && wantedByConfig()) {
        Log::get().note("xess-fg: not started: this build has no XeSS SDK (EDVR_HAVE_XESS=0)");
        noteProblem("this build has no XeSS SDK");
    }
    return nullptr;
}
bool xessFgFinishCreate(IDXGISwapChain*, HWND) { return false; }
void xessFgAbandon() {}
void xessFgRelease() {}
bool xessFgOwns(const void*) { return false; }
HRESULT xessFgPresent(IDXGISwapChain*, UINT, UINT) { return E_FAIL; }
void xessFgBeforeResize(UINT*, UINT*) {}
HRESULT xessFgAfterResize(HRESULT hr) { return hr; }
HWND xessFgWindow() { return nullptr; }
HRESULT xessFgSetFullscreenState(BOOL, IDXGIOutput*) { return E_FAIL; }
HRESULT xessFgGetFullscreenState(BOOL*, IDXGIOutput**) { return E_FAIL; }
HRESULT xessFgResizeTarget(const DXGI_MODE_DESC*) { return E_FAIL; }
HRESULT xessFgGetContainingOutput(IDXGIOutput**) { return E_FAIL; }
HRESULT xessFgGetFrameStatistics(DXGI_FRAME_STATISTICS*) { return E_FAIL; }
HRESULT xessFgGetLastPresentCount(UINT*) { return E_FAIL; }
void xessFgFrameStart() {}
void xessFgNoteCamera(const float[4][4], float) {}

#else

bool xessFgWanted() {
    const Bridge* b = g_bridge;
    return b && !b->failed && !b->fellBack && settings().wanted;
}

HWND xessFgBeginCreate(IUnknown* device, HWND realWindow, UINT* width, UINT* height, UINT format) {
    if (runtimeLaunchProfile() != RuntimeProfile::Flat || !wantedByConfig() || g_bridge || !realWindow) return nullptr;
    char why[320] = "";
    auto b = new Bridge();
    b->real = realWindow;
    b->format = proxyFormatFor(format);
    bool ok = true;
    if (b->format == DXGI_FORMAT_UNKNOWN) {
        std::snprintf(why, sizeof(why), "swap chain format %u is not one XeSS-FG presents (RGBA8, BGRA8, RGB10A2)", format);
        ok = false;
    }
    ok = ok && openD3D12(*b, device, why, sizeof(why)) && loadLibraries(*b, why, sizeof(why));
    if (ok) {
        const xell_result_t lr = b->ll.createContext(b->d12.Get(), &b->llContext);
        if (lr != XELL_RESULT_SUCCESS) {
            std::snprintf(why, sizeof(why), "xellD3D12CreateContext returned %d", int(lr));
            b->llContext = nullptr;
            ok = false;
        } else {
            b->ll.setLogging(b->llContext, XELL_LOGGING_LEVEL_WARNING, &llLog);
        }
    }
    if (ok) {
        const xefg_swapchain_result_t fr = b->fg.createContext(b->d12.Get(), &b->fgContext);
        if (fr != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            std::snprintf(why, sizeof(why), "xefgSwapChainD3D12CreateContext returned %s (%d)", fgResultName(fr), int(fr));
            b->fgContext = nullptr;
            ok = false;
        }
    }
    if (ok) {
        b->fg.setLogging(b->fgContext, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, &fgLog, nullptr);
        b->initFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;
        xefg_swapchain_d3d12_init_params_t params{};
        params.initFlags = b->initFlags;
        params.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
        xefg_swapchain_properties_t props{};
        if (b->fg.getProperties(b->fgContext, &params, 0, 0, DXGI_FORMAT_UNKNOWN, &props) >= 0 && props.maxSupportedInterpolations)
            g_maxFrames.store(props.maxSupportedInterpolations, std::memory_order_relaxed);
        const xefg_swapchain_result_t lr = b->fg.setLatency(b->fgContext, b->llContext);
        if (lr < 0) {
            std::snprintf(why, sizeof(why), "xefgSwapChainSetLatencyReduction returned %s", fgResultName(lr));
            ok = false;
        }
    }
    if (ok) {
        // Compiles in the background while Elite finishes starting; initialization waits for it.
        const xefg_swapchain_result_t pr = b->fg.buildPipelines(b->fgContext, nullptr, 0, b->initFlags);
        if (pr < 0) Log::get().note("xess-fg: pipeline build did not start: %s", fgResultName(pr));
        b->hidden = makeHiddenWindow();
        if (!b->hidden) {
            std::snprintf(why, sizeof(why), "hidden window could not be made (Win32=%lu)", GetLastError());
            ok = false;
        }
    }
    if (!ok) {
        Log::get().note("xess-fg: not started: %s; the swap chain is made the usual way", why);
        noteProblem(why);
        drainQueue(*b);
        delete b;
        return nullptr;
    }
    clientSize(realWindow, width, height);
    b->width = *width;
    b->height = *height;
    g_bridge = b;
    Log::get().note("xess-fg: Elite's swap chain goes on hidden window %p; the proxy will own window %p (%ux%u format=%u, "
                    "GPU maximum %u generated frames)", static_cast<void*>(b->hidden), static_cast<void*>(realWindow),
                    *width, *height, format, g_maxFrames.load(std::memory_order_relaxed));
    return b->hidden;
}

bool xessFgFinishCreate(IDXGISwapChain* hidden, HWND realWindow) {
    Bridge* b = g_bridge;
    if (!b || !hidden || realWindow != b->real) return false;
    char why[320] = "";
    bool ok = true;
    DXGI_SWAP_CHAIN_DESC hiddenDesc{};
    if (FAILED(hidden->GetDesc(&hiddenDesc))) {
        std::snprintf(why, sizeof(why), "hidden swap chain description unreadable");
        ok = false;
    }
    if (ok) {
        b->width = hiddenDesc.BufferDesc.Width;
        b->height = hiddenDesc.BufferDesc.Height;
        b->dxgiModule = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        using CreateFactory2Fn = HRESULT (WINAPI*)(UINT, REFIID, void**);
        CreateFactory2Fn createFactory = nullptr;
        if (!b->dxgiModule || !loadProc(b->dxgiModule, "CreateDXGIFactory2", createFactory) ||
            FAILED(createFactory(0, IID_PPV_ARGS(b->factory.GetAddressOf())))) {
            std::snprintf(why, sizeof(why), "a DXGI factory for the proxy could not be made");
            ok = false;
        }
    }
    if (ok) {
        ComPtr<IDXGIFactory5> factory5;
        BOOL allow = FALSE;
        if (SUCCEEDED(b->factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            b->tearing = allow != FALSE;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = b->width;
        desc.Height = b->height;
        desc.Format = b->format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kProxyBuffers;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        desc.Flags = b->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        xefg_swapchain_d3d12_init_params_t params{};
        params.initFlags = b->initFlags;
        params.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
        params.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;   // the HUD is interpolated with the scene: no HUD-less image exists
        const xefg_swapchain_result_t ir =
            b->fg.initFromDesc(b->fgContext, realWindow, &desc, nullptr, b->queue.Get(), b->factory.Get(), &params);
        if (ir < 0) {
            std::snprintf(why, sizeof(why), "xefgSwapChainD3D12InitFromSwapChainDesc returned %s (%d)", fgResultName(ir), int(ir));
            ok = false;
        } else if (b->fg.getSwapChainPtr(b->fgContext, IID_PPV_ARGS(b->proxy.GetAddressOf())) < 0 || !b->proxy) {
            std::snprintf(why, sizeof(why), "the proxy swap chain pointer was refused");
            ok = false;
        }
        if (ok) {
            xefg_swapchain_d3d12_init_params_t applied{};
            if (b->fg.getInitParams(b->fgContext, &applied) >= 0 && applied.maxInterpolatedFrames)
                g_maxFrames.store(applied.maxInterpolatedFrames, std::memory_order_relaxed);
        }
    }
    ok = ok && makeShared(*b, why, sizeof(why)) && makeRing(*b, why, sizeof(why));
    if (ok) {
        xell_sleep_params_t sleep{};
        sleep.bLowLatencyMode = 1;
        const xell_result_t sr = b->ll.setSleepMode(b->llContext, &sleep);
        if (sr != XELL_RESULT_SUCCESS) {
            std::snprintf(why, sizeof(why), "xellSetSleepMode returned %d", int(sr));
            ok = false;
        }
    }
    if (!ok) {
        // The caller releases the hidden swap chain, then calls xessFgAbandon to take the window down after it.
        Log::get().note("xess-fg: not started: %s; the swap chain is remade on the game window", why);
        noteProblem(why);
        return false;
    }
    b->hiddenSwap = hidden;
    xessSetInputTagger(&tagInputs);
    g_bridged.store(true, std::memory_order_relaxed);
    Log::get().note("xess-fg: bridge ready: proxy %ux%u format=%u buffers=%u tearing=%u; XeLL low latency on; GPU maximum "
                    "%u generated frames; frame generation runs while fix.xess_frame_generation is on and the flat XeSS-SR "
                    "resolve runs", b->width, b->height, unsigned(b->format), kProxyBuffers, b->tearing ? 1u : 0u,
                    g_maxFrames.load(std::memory_order_relaxed));
    return true;
}

void xessFgAbandon() { teardown(false); }

void xessFgRelease() {
    if (!g_bridge) return;
    Log::get().note("xess-fg: Elite is making another swap chain on window %p; the bridge lets go of it",
                    static_cast<void*>(g_bridge->real));
    teardown(true);
}

bool xessFgOwns(const void* swapChain) {
    const Bridge* b = g_bridge;
    return b && b->hiddenSwap && b->hiddenSwap == swapChain;
}

HRESULT xessFgPresent(IDXGISwapChain* hidden, UINT syncInterval, UINT flags) {
    Bridge* b = g_bridge;
    if (!b || hidden != b->hiddenSwap) return DXGI_ERROR_INVALID_CALL;
    if (b->failed) fallBack(*b);
    if (flags & DXGI_PRESENT_TEST) {
        IDXGISwapChain* shown = presenter(*b);
        return shown ? shown->Present(syncInterval, presentFlagsFor(*b, syncInterval, flags)) : S_OK;
    }
    if (b->fellBack) return plainPresent(*b, hidden, syncInterval, flags);

    startFrame(*b);   // the first frame only: every later one started at the end of the last Present hook
    const uint32_t id = b->frameId;
    llMarker(*b, id, XELL_RENDERSUBMIT_END);

    // Elite's back buffer: the proxy follows its size and format (a missed resize is caught here), then it goes into
    // the shared texture on the D3D11 queue and, after a GPU-side handoff, into the proxy on the D3D12 queue.
    ComPtr<ID3D11Texture2D> back;
    D3D11_TEXTURE2D_DESC source{};
    const char* skipReason = "Elite's back buffer unreadable";
    bool copied = false;
    if (sourceBuffer(hidden, back, &source)) {
        syncProxy(*b, source.Width, source.Height, source.Format);
        copied = !b->failed && copyIn(*b, back.Get(), &skipReason);
    }
    back.Reset();
    if (copied) submitToProxy(*b);
    if (b->failed) {
        fallBack(*b);
        return plainPresent(*b, hidden, syncInterval, flags);
    }
    if (!copied) {
        // Presenting now would show a proxy buffer last written frames ago (and generate frames from it): the frame
        // is dropped instead, and the window keeps the last good image.
        if (b->skipped++ < 16)
            Log::get().note("xess-fg: frame %u not presented: %s", id, skipReason);
        llMarker(*b, id, XELL_PRESENT_START);
        llMarker(*b, id, XELL_PRESENT_END);
        ++b->frameId;
        if (!b->frameId) b->frameId = 1;
        return S_OK;
    }

    // Frame generation runs while it is wanted and the flat XeSS-SR resolve tagged this frame's inputs. Two frames
    // with inputs before it starts, so one stray resolve in a menu does not flap it (each disable drops its history).
    const FgSettings& s = settings();
    const bool inputs = b->taggedId == id;
    b->taggedRun = inputs ? b->taggedRun + 1 : 0;
    const bool want = s.wanted && runtimeFlatProfile() && !b->presenterFullscreen && inputs &&
                      (b->enabledState == 1 || b->taggedRun >= 2);
    if (int(want) != b->enabledState) {
        const xefg_swapchain_result_t er = b->fg.setEnabled(b->fgContext, want ? 1u : 0u);
        Log::get().note("xess-fg: frame generation %s at frame %u (%s)", want ? "enabled" : "disabled", id,
                        er < 0 ? fgResultName(er) : "ok");
        b->enabledState = want ? 1 : 0;
    }
    if (want) {
        const UINT frames = clampFrames(s.frames);
        if (frames != b->framesState) {
            const xefg_swapchain_result_t nr = b->fg.setFrames(b->fgContext, frames);
            Log::get().note("xess-fg: generated frames per rendered frame = %u (%s)", frames, nr < 0 ? fgResultName(nr) : "ok");
            if (nr >= 0) b->framesState = frames;
        }
    }

    const UINT presentFlags = presentFlagsFor(*b, syncInterval, flags);
    llMarker(*b, id, XELL_PRESENT_START);
    b->fg.setPresentId(b->fgContext, id);
    const HRESULT hr = b->proxy->Present(syncInterval, presentFlags);
    llMarker(*b, id, XELL_PRESENT_END);

    xefg_swapchain_present_status_t status{};
    const xefg_swapchain_result_t st = b->fg.lastStatus(b->fgContext, &status);
    if (st >= 0 && status.frameGenResult < 0 && b->statusFailures++ < 16)
        Log::get().note("xess-fg: frame %u: interpolation result %s (%d)", id, fgResultName(status.frameGenResult),
                        int(status.frameGenResult));
    if (++b->presents == 1)
        Log::get().note("xess-fg: first present hr=0x%08X", unsigned(hr));
    ++b->frameId;
    if (!b->frameId) b->frameId = 1;
    return hr;
}

void xessFgBeforeResize(UINT* width, UINT* height) {
    if (Bridge* b = g_bridge) clientSize(b->real, width, height);
}

HRESULT xessFgAfterResize(HRESULT hiddenResult) {
    Bridge* b = g_bridge;
    if (!b) return hiddenResult;
    ComPtr<ID3D11Texture2D> back;
    D3D11_TEXTURE2D_DESC source{};
    if (sourceBuffer(b->hiddenSwap, back, &source)) {
        back.Reset();
        if (b->fellBack) syncPlain(*b, source.Width, source.Height, source.Format);
        else syncProxy(*b, source.Width, source.Height, source.Format);
    }
    return hiddenResult;
}

HWND xessFgWindow() { return g_bridge ? g_bridge->real : nullptr; }

HRESULT xessFgSetFullscreenState(BOOL fullscreen, IDXGIOutput* target) {
    Bridge* b = g_bridge;
    IDXGISwapChain* shown = b ? presenter(*b) : nullptr;
    if (!shown) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = shown->SetFullscreenState(fullscreen, target);
    if (SUCCEEDED(hr)) b->presenterFullscreen = fullscreen != FALSE;
    b->enabledState = -1;   // XeSS-FG needs enabling again after exclusive fullscreen
    Log::get().note("xess-fg: exclusive fullscreen %s (0x%08X)%s", fullscreen ? "on" : "off", unsigned(hr),
                    fullscreen && !b->fellBack ? "; XeSS-FG presents without generating frames in exclusive fullscreen" : "");
    return hr;
}

HRESULT xessFgGetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) {
    Bridge* b = g_bridge;
    IDXGISwapChain* shown = b ? presenter(*b) : nullptr;
    return shown ? shown->GetFullscreenState(fullscreen, target) : DXGI_ERROR_INVALID_CALL;
}

HRESULT xessFgResizeTarget(const DXGI_MODE_DESC* mode) {
    Bridge* b = g_bridge;
    IDXGISwapChain* shown = b ? presenter(*b) : nullptr;
    return shown ? shown->ResizeTarget(mode) : DXGI_ERROR_INVALID_CALL;
}

HRESULT xessFgGetContainingOutput(IDXGIOutput** output) {
    Bridge* b = g_bridge;
    IDXGISwapChain* shown = b ? presenter(*b) : nullptr;
    return shown ? shown->GetContainingOutput(output) : DXGI_ERROR_INVALID_CALL;
}

HRESULT xessFgGetFrameStatistics(DXGI_FRAME_STATISTICS* stats) {
    Bridge* b = g_bridge;
    IDXGISwapChain* shown = b ? presenter(*b) : nullptr;
    return shown ? shown->GetFrameStatistics(stats) : DXGI_ERROR_INVALID_CALL;
}

HRESULT xessFgGetLastPresentCount(UINT* count) {
    Bridge* b = g_bridge;
    IDXGISwapChain* shown = b ? presenter(*b) : nullptr;
    return shown ? shown->GetLastPresentCount(count) : DXGI_ERROR_INVALID_CALL;
}

void xessFgFrameStart() {
    if (Bridge* b = g_bridge) startFrame(*b);
}

void xessFgNoteCamera(const float rows[4][4], float frameMs) {
    Bridge* b = g_bridge;
    if (!b || !rows) return;
    std::memcpy(b->rows, rows, sizeof(b->rows));
    b->haveRows = true;
    b->frameMs = frameMs > 0.f && std::isfinite(frameMs) ? frameMs : 0.f;
}

#endif  // EDVR_HAVE_XESS

}  // namespace edvr
