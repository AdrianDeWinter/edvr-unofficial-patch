#pragma once
#include "flat_camera_phase.h"
#include "flat_projection_runtime.h"
#include <d3d11_1.h>
#include <wrl/client.h>

namespace edvr {

// This admission is deliberately for centered projection families. An unknown
// native off-center projection cannot establish the injected phase from one
// publication. Rows must come from the currently bound, complete CPU witness.
inline bool flatForegroundRowsCarryPhase(const float (&rows)[6][4],
                                         float pixelX, float pixelY,
                                         uint32_t width, uint32_t height,
                                         float expectedNear) {
    FlatProjectionJitter expected{};
    double actualX = 0, actualY = 0;
    for (unsigned r = 0; r < 4; ++r)
        for (float value : rows[r]) if (!std::isfinite(value)) return false;
    return std::isfinite(expectedNear) && expectedNear > 0 &&
        rows[0][2] == 0 && rows[1][2] == 0 && rows[2][2] == 0 &&
        rows[3][2] == expectedNear &&
        flatProjectionJitter(pixelX, pixelY, width, height, expected) &&
        flatCameraMeasureRowShift(rows, actualX, actualY) &&
        std::abs(actualX - expected.ndcX) <= kFlatCameraPairTolerance &&
        std::abs(actualY - expected.ndcY) <= kFlatCameraPairTolerance;
}

inline bool flatForegroundProjectionRowsCarryPhase(const float (&projectionRows)[4][4],
                                     FlatProjectionPatchLayout layout,
                                     float pixelX, float pixelY,
                                     uint32_t width, uint32_t height,
                                     float expectedNear) {
    float rows[6][4]{};
    if (layout == FlatProjectionPatchLayout::ForwardColumns)
        std::memcpy(rows, projectionRows, sizeof(projectionRows));
    else if (layout == FlatProjectionPatchLayout::ForwardDp4) {
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned c = 0; c < 4; ++c) rows[r][c] = projectionRows[c][r];
    } else return false;
    return flatForegroundRowsCarryPhase(rows, pixelX, pixelY, width, height, expectedNear);
}

// The shader recipe must establish that these are its actual four clip rows;
// DP4 additionally needs homogeneous source.w == 1 for constant clip Z.
struct FlatForegroundProjectionLookupFailure {
    const void* buffer = nullptr;
    UINT slot = 0, row = 0, first = 0, count = 0, byteWidth = 0;
    uint32_t offset = 0;
    FlatProjectionPatchLayout layout = FlatProjectionPatchLayout::ForwardColumns;
    FlatProjectionCopyFailure copy{};
};
inline bool flatForegroundBoundProjectionPhase(ID3D11DeviceContext* ctx,
                                     FlatProjectionRuntime* projection,
                                     UINT slot, UINT row,
                                     FlatProjectionPatchLayout layout,
                                     float pixelX, float pixelY,
                                     uint32_t width, uint32_t height,
                                     float expectedNear,
                                      const char** reason = nullptr,
                                      FlatForegroundProjectionLookupFailure* lookupFailure = nullptr,
                                      ID3D11DeviceContext1* cachedCtx1 = nullptr) {
    auto refuse = [&](const char* why) { if (reason) *reason = why; return false; };
    if (reason) *reason = nullptr;
    if (lookupFailure) lookupFailure->copy.kind = FlatProjectionCopyFailureKind::None;
    if (!ctx || !projection || slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT || row > 4092)
        return refuse("foreign phase missing CPU witness or recipe range");
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1;
    ID3D11DeviceContext1* activeCtx1 = cachedCtx1;
    UINT first = 0, count = 4096;
    if (!activeCtx1 && SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) activeCtx1 = ctx1.Get();
    if (activeCtx1) activeCtx1->VSGetConstantBuffers1(slot, 1, &buffer, &first, &count);
    else ctx->VSGetConstantBuffers(slot, 1, &buffer);
    if (!buffer || count < row + 4) return refuse("foreign phase short bound projection");
    const uint64_t offset = (uint64_t(first) + row) * 16;
    struct BoundPhaseMemo {
        bool valid = false;
        const void* ctx = nullptr;
        const void* projection = nullptr;
        const void* buffer = nullptr;
        UINT slot = 0;
        uint64_t offset = 0;
        FlatProjectionPatchLayout layout = FlatProjectionPatchLayout::ForwardColumns;
        uint32_t width = 0, height = 0;
        float pixelX = 0, pixelY = 0, expectedNear = 0;
        uint64_t generation = 0, mutationSerial = 0, writeGeneration = 0, bankEpoch = 0, writeFrame = 0;
        uint32_t writeSequence = 0, metaWidth = 0;
    };
    static thread_local BoundPhaseMemo s_memo{};
    const FlatProjectionShadowMetadata meta = projection->constantsMetadata(buffer.Get());
    const bool trackable = meta.tracked && meta.shadowPresent && !meta.mapped && offset + 64 <= meta.width;
    if (trackable && s_memo.valid &&
        s_memo.ctx == ctx && s_memo.projection == projection && s_memo.buffer == buffer.Get() &&
        s_memo.slot == slot && s_memo.offset == offset && s_memo.layout == layout &&
        s_memo.width == width && s_memo.height == height &&
        s_memo.pixelX == pixelX && s_memo.pixelY == pixelY && s_memo.expectedNear == expectedNear &&
        s_memo.generation == meta.generation && s_memo.mutationSerial == meta.mutationSerial &&
        s_memo.writeGeneration == meta.writeGeneration && s_memo.bankEpoch == meta.bankEpoch &&
        s_memo.writeFrame == meta.writeFrame && s_memo.writeSequence == meta.writeSequence &&
        s_memo.metaWidth == meta.width) {
        return true;
    }
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (offset + 64 > desc.ByteWidth) return refuse("foreign phase projection range outside buffer");
    float rows[4][4]{};
    FlatProjectionCopyFailure copyFailure{};
    if (!projection->copyConstants(buffer.Get(), uint32_t(offset), 64, rows,
                                   lookupFailure ? &copyFailure : nullptr)) {
        if (lookupFailure) *lookupFailure = {buffer.Get(), slot, row, first, count,
            desc.ByteWidth, uint32_t(offset), layout, copyFailure};
        return refuse("foreign phase missing bound projection publication");
    }
    if (!flatForegroundProjectionRowsCarryPhase(rows, layout, pixelX, pixelY, width, height, expectedNear))
        return refuse("foreign phase or near differs from bound projection");
    if (trackable) {
        s_memo.valid = true;
        s_memo.ctx = ctx;
        s_memo.projection = projection;
        s_memo.buffer = buffer.Get();
        s_memo.slot = slot;
        s_memo.offset = offset;
        s_memo.layout = layout;
        s_memo.width = width;
        s_memo.height = height;
        s_memo.pixelX = pixelX;
        s_memo.pixelY = pixelY;
        s_memo.expectedNear = expectedNear;
        s_memo.generation = meta.generation;
        s_memo.mutationSerial = meta.mutationSerial;
        s_memo.writeGeneration = meta.writeGeneration;
        s_memo.bankEpoch = meta.bankEpoch;
        s_memo.writeFrame = meta.writeFrame;
        s_memo.writeSequence = meta.writeSequence;
        s_memo.metaWidth = meta.width;
    }
    return true;
}

inline bool flatForegroundBoundPhase(ID3D11DeviceContext* ctx,
                                     FlatProjectionRuntime* projection,
                                     float pixelX, float pixelY,
                                     uint32_t width, uint32_t height,
                                     float expectedNear,
                                     const char** reason = nullptr) {
    return flatForegroundBoundProjectionPhase(ctx, projection, 1, 270,
        FlatProjectionPatchLayout::ForwardColumns, pixelX, pixelY, width, height, expectedNear, reason);
}

} // namespace edvr
