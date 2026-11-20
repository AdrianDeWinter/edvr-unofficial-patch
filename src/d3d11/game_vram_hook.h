#pragma once
#include <cstdint>

struct IDXGIAdapter;
namespace edvr {
// Install the GetDesc interception once per process, before the game's
// device is created. `adapter` is what the game passed to
// D3D11CreateDevice (may be null). Safe to call more than once.
void gameVramInstall(IDXGIAdapter* adapter);

// Once a second from the graphics memory watch: this process's local usage
// and budget (bytes). Teaches advanced.game_vram = auto; free otherwise.
void gameVramObserve(uint64_t nowMs, uint64_t usageBytes, uint64_t budgetBytes);
}
