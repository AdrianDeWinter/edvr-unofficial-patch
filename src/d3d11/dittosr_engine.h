// DittoSR 0.5.0 at the same door as FSR (fsr3_engine.h). Vancete's upscaler
// ships as a DirectX 12 / Vulkan stand-in for the FidelityFX DLL. Elite is
// Direct3D 11 and ED-UP's FSR is compiled into d3d11.dll, so that DLL is never
// loaded. These entry points run DittoSR's four compute passes on the game's
// device, on the colour, depth and motion the temporal pass already builds
// for DLSS and FSR. third_party/dittosr/README.md names the pin and the two
// Direct3D 11 differences (shader model 5 bytecode, RGBA16F history).
#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

bool dittoAvailable(ID3D11Device* dev, const char** why);

// One slot, one frame. `slot` is dlaa.h's kUpscalerSlots. colour, depth, mv
// and out are the same textures fsr3Evaluate takes; reactive may be null.
// out, and every texture the passes write for themselves, need both an
// unordered-access and a shader-resource bind. False, with why, on a refusal.
// hdr: the colour is scene radiance (the flat HDR route). Reversed-Z depth is
// always on, matching FSR's create flags.
bool dittoEvaluate(ID3D11DeviceContext* ctx, unsigned slot, ID3D11Texture2D* colour,
                   ID3D11Texture2D* depth, ID3D11Texture2D* mv, ID3D11Texture2D* reactive,
                   ID3D11Texture2D* out, uint32_t w, uint32_t h, uint32_t outW, uint32_t outH,
                   float jx, float jy, bool reset, const char** why, bool hdr = false);

void dittoReleaseFeatures();
void dittoShutdown();

// "DittoSR 0.5.0", what the price line prints.
const char* dittoVersionLabel();

bool dittoTotals(uint32_t* evaluations, double* avgMs, double* maxMs, uint32_t* resets);

}  // namespace edvr
