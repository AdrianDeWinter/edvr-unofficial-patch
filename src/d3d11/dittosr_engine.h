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
struct ID3D11ShaderResourceView;
struct ID3D11UnorderedAccessView;

namespace edvr {

bool dittoAvailable(ID3D11Device* dev, const char** why);

// The caller owns these views for the duration of dittoEvaluate. Reactive is optional.
struct DittoFrameViews {
    ID3D11ShaderResourceView* colour;
    ID3D11ShaderResourceView* depth;
    ID3D11ShaderResourceView* motion;
    ID3D11ShaderResourceView* reactive;
    ID3D11UnorderedAccessView* output;
};

// One slot from dlaa.h's kUpscalerSlots, with the caller's route name for the log.
// HDR marks scene radiance. Depth is always reversed-Z, matching FSR's create flags.
// Returns false and sets why on a refusal.
bool dittoEvaluate(ID3D11DeviceContext* ctx, unsigned slot, const char* route, const DittoFrameViews& views,
                   uint32_t w, uint32_t h, uint32_t outW, uint32_t outH, float jx, float jy,
                   bool reset, const char** why, bool hdr);

void dittoReleaseFeatures();
void dittoShutdown();

// "DittoSR 0.5.0", what the price line prints.
const char* dittoVersionLabel();

bool dittoTotals(uint32_t* evaluations, double* avgMs, double* maxMs, uint32_t* resets);

}  // namespace edvr
