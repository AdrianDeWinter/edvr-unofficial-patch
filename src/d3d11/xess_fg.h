#pragma once

#include <cstdint>

struct IUnknown;
struct IDXGIOutput;
struct IDXGISwapChain;
struct DXGI_MODE_DESC;
struct DXGI_FRAME_STATISTICS;
struct IDXGIFactory2;
struct IDXGISwapChain1;
struct DXGI_SWAP_CHAIN_DESC1;
typedef struct HWND__* HWND;
typedef long HRESULT;
typedef int BOOL;
typedef unsigned int UINT;

namespace edvr {

// XeSS Frame Generation and Xe Low Latency in the flat profile
// (docs/xess-frame-generation-2026-11-12.md).
//
// Intel's XeSS-FG and XeLL are D3D12 only, and XeSS-FG works by owning the
// window's swap chain. Elite is D3D11, so EDVR bridges: when the flat profile
// launches with temporal AA mode xess, Elite's D3D11 swap chain is made on a
// hidden window of EDVR's, and Elite's frame is copied each Present over the
// XeSS-SR D3D11/D3D12 device pair (xess_engine.cpp). Until frame generation can
// run, EDVR's plain D3D11 flip swap chain shows that frame on Elite's real
// window. Frame generation can run when fix.xess_frame_generation is on, the
// window is windowed and in the foreground, and XeSS-SR is resolving; then the
// XeSS-FG proxy swap chain takes the window and keeps it until exclusive
// fullscreen, when the plain swap chain takes it back. XeLL paces the proxy's
// frames, and the motion vectors and depth XeSS-SR already receives are tagged
// for frame generation on the same D3D12 command list.
//
// A fault in the bridge (a GPU wait that times out, a resize the proxy refuses,
// a format XeSS-FG cannot present) stops frame generation for the session and
// hands the real window to the plain D3D11 flip swap chain for good, so Elite's
// frame keeps reaching the screen.
//
// Settings, read live (on a configuration change) while the bridge exists:
//   fix.xess_frame_generation  on | off    frames are generated only on frames
//                                          the flat XeSS-SR resolve ran
//   fix.xess_generated_frames  1 | 2 | 3   generated frames per rendered frame,
//                                          clamped to the GPU's maximum
// The bridge is made when Elite creates its swap chain only if the flat profile
// launches with both fix.xess_frame_generation on and temporal AA mode xess;
// otherwise the swap chain is made the usual way. After a bridged start the key
// is live both ways (the proxy is made when the window is windowed and XeSS-SR
// resolves), and the bridge stays until exit, also if the AA mode is switched
// away from xess. Turning the key on, or choosing xess, after a start without
// both takes effect at the next game start.

// The settings panel's view.
// True when the bridge owns Elite's window this session (plain or proxy).
bool xessFgBridged();
// True while the bridge's window is in exclusive fullscreen (Elite's Fullscreen
// display mode). XeSS-FG makes no frames there, only in Borderless or Windowed.
bool xessFgExclusiveFullscreen();
// The GPU's maximum generated frames per rendered frame (Intel Arc: up to 3,
// other vendors: 1). A non-Intel GPU's 1 is known from Elite's swap chain
// creation on; an Intel GPU's stays 0 (unknown) until a bridge asks XeSS-FG.
uint32_t xessFgMaxGeneratedFrames();
// Why frame generation did not start this session, or stopped: null when it
// did not fail (never tried, or running). The text stays the same once set.
const char* xessFgProblem();
// True while the bridge generates (not fallen back, not in exclusive fullscreen) and fix.xess_frame_generation is on: the flat resolver then keeps
// the motion of the strip a turn brings into view (FlatMonoResolveFrame::keepOffscreenMotion) instead of writing 0
// there. The render thread's.
bool xessFgWanted();
// True once a bridge was made this session, and stays true after it stopped (fell back, was torn down): the Monitor and the frame-rate readout show a
// presented rate whenever this is true, so a bridge that stopped is told from a session that never had one.
bool xessFgBridgeThisSession();
// The generated frames per rendered frame the bridge is making right now: 0 while frame generation is off, not wanted or stopped. The render thread's.
uint32_t xessFgActiveGeneratedFrames();
// Frames that reached the screen: the cumulative count and the QPC time of its latest sample, and the source's name (a static string). False when there is
// no count: no bridge this session, nothing sampled yet, or neither source works (xessFgPresentUnavailableWhy says which, "" before the first sample).
// Any thread. Never rendered * (1 + generated) (xess_fg_presented.h).
bool xessFgPresentStats(uint64_t* presented, uint64_t* qpc, const char** source);
// Why no presented count is available, a static string; "" while it is only not sampled yet.
const char* xessFgPresentUnavailableWhy();

// device_hook.cpp, the game factory's swap chain creation, before the real
// call. When the flat profile launched with fix.xess_frame_generation on and AA
// mode xess, and the bridge's D3D12 side could be made, returns the hidden window
// to create Elite's swap chain on (and
// fills a zero size from the real window's client area); otherwise null, and
// the swap chain is created as it always was.
HWND xessFgBeginCreate(IUnknown* device, HWND realWindow, UINT* width, UINT* height, UINT format);
// After the real call made `hidden` on that window: makes the plain D3D11
// swap chain on the real window. False tears the bridge down; the caller then
// releases `hidden` and creates the swap chain on the real window as before.
bool xessFgFinishCreate(IDXGISwapChain* hidden, HWND realWindow);
// After a failed finish (and after the caller released the hidden swap chain): drops the bridge and its window.
void xessFgAbandon();
// Elite is making another swap chain on the bridged window: drops the bridge so that one can own the window. The
// hidden window stays (the old swap chain may still be alive on it).
void xessFgRelease();

// device_hook.cpp provides:
// The bridge's plain swap chain, made on Elite's own factory so Elite's window association applies (xess_fg.cpp makePlain).
// Straight to the real CreateSwapChainForHwnd: through the hook it would re-enter createBridged, which releases the
// bridge, and be hooked as the game's swap chain.
HRESULT xessFgCreateSwapChainUnhooked(IDXGIFactory2* factory, IUnknown* device, HWND window,
                                      const DXGI_SWAP_CHAIN_DESC1* desc, IDXGISwapChain1** out);

// True when `swapChain` is the bridged one: its Present, resize and window
// queries go through the functions below instead of DXGI.
bool xessFgOwns(const void* swapChain);
HRESULT xessFgPresent(IDXGISwapChain* hidden, UINT syncInterval, UINT flags);
// Around the real ResizeBuffers of the hidden swap chain. Before fills a zero
// size from the real window; after makes the presenting swap chain follow the
// hidden one's back buffer (Present does the same if a resize was missed).
void xessFgBeforeResize(UINT* width, UINT* height);
HRESULT xessFgAfterResize(HRESULT hiddenResult);
HWND xessFgWindow();
HRESULT xessFgSetFullscreenState(BOOL fullscreen, IDXGIOutput* target);
HRESULT xessFgGetFullscreenState(BOOL* fullscreen, IDXGIOutput** target);
HRESULT xessFgResizeTarget(const DXGI_MODE_DESC* mode);
HRESULT xessFgGetContainingOutput(IDXGIOutput** output);
HRESULT xessFgGetFrameStatistics(DXGI_FRAME_STATISTICS* stats);
HRESULT xessFgGetLastPresentCount(UINT* count);
// End of hookedPresent: XeLL's frame start (its sleep, then the frame's first markers).
void xessFgFrameStart();

// flat_mono_resolve.cpp, before xessEvaluate: the frame's camera as the
// resolver's row-vector clip rows (b1[270..273]: clip = [p,1] * rows; rows[3][2]
// is the reversed infinite projection's near plane).
void xessFgNoteCamera(const float rows[4][4], float frameMs);

}  // namespace edvr
