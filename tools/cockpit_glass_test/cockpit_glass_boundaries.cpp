// Connect the production glass code to an unhooked D3D11 rig. The depth/stencil
// fixture supplies a constant-color shader at the shader factory boundary.
#include "../../src/common/log.h"
#include "../../src/common/guard.h"
#include "../../src/d3d11/shader_swap.h"
#include "../../src/d3d11/vscreen.h"

#include <d3d11.h>
#include <cstdlib>

namespace glass_test {
ID3D11PixelShader* overlayPixelShader = nullptr;
}

namespace edvr {

Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
void Log::note(const char*, ...) {}
int64_t qpcNow() { return 0; }
int64_t qpcFrequency() { return 1; }

void breadcrumb(const char*) {}
int guardFilter(unsigned long, const char*) { return EXCEPTION_CONTINUE_SEARCH; }
void FaultBudget::charge() { std::abort(); }

ID3D11PixelShader* shaderSwapCreatePs(ID3D11DeviceContext* ctx, const void* bytecode, size_t bytes,
                                     const char*, const char*) {
    if (glass_test::overlayPixelShader) {
        glass_test::overlayPixelShader->AddRef();
        return glass_test::overlayPixelShader;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11PixelShader* ps = nullptr;
    dev->CreatePixelShader(bytecode, bytes, nullptr, &ps);
    dev->Release();
    return ps;
}

void vScreenPSSetShaderRaw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps,
                          ID3D11ClassInstance* const* classes, uint32_t count) {
    ctx->PSSetShader(ps, classes, count);
}
void vScreenPSSetShaderResourcesRaw(ID3D11DeviceContext* ctx, uint32_t start, uint32_t count,
                                    ID3D11ShaderResourceView* const* views) {
    ctx->PSSetShaderResources(start, count, views);
}
void vScreenPSSetConstantBuffersRaw(ID3D11DeviceContext* ctx, uint32_t start, uint32_t count,
                                    ID3D11Buffer* const* buffers) {
    ctx->PSSetConstantBuffers(start, count, buffers);
}
void vScreenOMSetDepthStencilStateRaw(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* state, uint32_t ref) {
    ctx->OMSetDepthStencilState(state, ref);
}
void vScreenOMSetBlendStateRaw(ID3D11DeviceContext* ctx, ID3D11BlendState* state,
                              const float factor[4], uint32_t mask) {
    ctx->OMSetBlendState(state, factor, mask);
}
void vScreenUpdateSubresourceRaw(ID3D11DeviceContext* ctx, ID3D11Resource* dest, uint32_t subresource,
                                const D3D11_BOX* box, const void* data, uint32_t rowPitch, uint32_t depthPitch) {
    ctx->UpdateSubresource(dest, subresource, box, data, rowPitch, depthPitch);
}

}  // namespace edvr
