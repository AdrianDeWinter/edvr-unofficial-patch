#pragma once

#include <cstdint>

struct IUnknown;
struct IDXGIOutput;
struct IDXGISwapChain;
struct DXGI_MODE_DESC;
struct DXGI_FRAME_STATISTICS;
typedef struct HWND__* HWND;
typedef long HRESULT;
typedef int BOOL;
typedef unsigned int UINT;

namespace edvr {

// XeSS Frame Generation and Xe Low Latency in the flat profile
// (docs/xess-frame-generation-2026-11-12.md).
//
// Intel's XeSS-FG and XeLL are D3D12 only, and XeSS-FG works by owning the
// window's swap chain. Elite is D3D11, so EDVR bridges: when
// fix.xess_frame_generation is on as the flat profile creates its swap chain,
// Elite's D3D11 swap chain is made on a hidden window of EDVR's, and each
// Present is copied over the XeSS-SR D3D11/D3D12 device pair (xess_engine.cpp)
// into an XeSS-FG proxy swap chain on Elite's real window, which XeSS-FG paces
// with XeLL. The motion vectors and depth XeSS-SR already receives are tagged
// for frame generation on the same D3D12 command list.
//
// A fault in the bridge (a GPU wait that times out, a resize the proxy refuses,
// a format XeSS-FG cannot present) stops frame generation for the session and
// hands the real window to a plain D3D11 flip swap chain of EDVR's, so Elite's
// frame keeps reaching the screen.
//
// Settings, read live (on a configuration change) while the bridge exists:
//   fix.xess_frame_generation  on | off    frames are generated only on frames
//                                          the flat XeSS-SR resolve ran
//   fix.xess_generated_frames  1 | 2 | 3   generated frames per rendered frame,
//                                          clamped to the GPU's maximum
// Turning the first on when the bridge was not made at launch takes effect at
// the next launch: the bridge can only be made when Elite creates its swap chain.

// The settings panel's view.
// True when the bridge owns Elite's window this session.
bool xessFgBridged();
// The GPU's maximum generated frames per rendered frame (Intel Arc: up to 3,
// other vendors: 1), or 0 while it is not known (no bridge was made).
uint32_t xessFgMaxGeneratedFrames();
// Why frame generation did not start this session, or stopped: null when it
// did not fail (never tried, or running). The text stays the same once set.
const char* xessFgProblem();
// True while the bridge generates (not fallen back) and fix.xess_frame_generation is on: the flat resolver then keeps
// the motion of the strip a turn brings into view (FlatMonoResolveFrame::keepOffscreenMotion) instead of writing 0
// there. The render thread's.
bool xessFgWanted();

// device_hook.cpp, the game factory's swap chain creation, before the real
// call. When the bridge is wanted and its D3D12 side could be made, returns
// the hidden window to create Elite's swap chain on (and fills a zero size
// from the real window's client area); otherwise null, and the swap chain is
// created as it always was.
HWND xessFgBeginCreate(IUnknown* device, HWND realWindow, UINT* width, UINT* height, UINT format);
// After the real call made `hidden` on that window: makes the proxy on the
// real window. False tears the bridge down; the caller then releases `hidden`
// and creates the swap chain on the real window as before.
bool xessFgFinishCreate(IDXGISwapChain* hidden, HWND realWindow);
// After a failed finish (and after the caller released the hidden swap chain): drops the bridge and its window.
void xessFgAbandon();
// Elite is making another swap chain on the bridged window: drops the bridge so that one can own the window. The
// hidden window stays (the old swap chain may still be alive on it).
void xessFgRelease();

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
