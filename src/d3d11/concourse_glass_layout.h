#pragma once
// The concourse's window glass, recognised by the layout of its pixel shader. The game builds a different glass pixel shader for each Material Quality level (and
// for Spot Shadows below High); the vertex shader stays the same. Every one of them declares the same interface, so the check reads only the declaration section of
// the program: constant buffers 1 (any size) and 2 (size 3) and no others; pixel inputs v0.x, v1.xyz, v2.xyz, v3.xyz, v4.xy and no others (the system-value inputs
// are not counted); one output, o0.xyzw. Textures, samplers and the instructions are not looked at. The walk advances by instructionLength alone, and stops at the
// first instruction that is neither a declaration nor custom data. Pure: no D3D and no state.
//
// It runs once for every pixel shader the game creates, so it reads the container in place: the chunk table, then the first kPrefixWords of the program. It does
// not copy the whole shader or recompute its checksum (D3D has accepted the bytecode before this runs). A declaration section longer than the prefix is not the
// glass's.
#include "dxbc_container.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace edvr {
namespace concourse {

namespace glass_layout_detail {
constexpr uint32_t kPs50 = 0x00000050u;
constexpr uint32_t kTagShex = 0x58454853u, kTagShdr = 0x52444853u;
constexpr uint32_t kOpCustomData = 53, kOpDclConstantBuffer = 89, kOpDclInputPs = 98, kOpDclOutput = 101;
constexpr uint32_t kOpDclInputPsSgv = 99, kOpDclInputPsSiv = 100, kOpDclOutputSgv = 102, kOpDclOutputSiv = 103;
constexpr uint32_t kOperandInput = 1, kOperandOutput = 2, kOperandCb = 8;
constexpr size_t kPrefixWords = 1024;   // the glass's declarations take about a hundred

inline bool readWord(const unsigned char* p, size_t len, size_t offset, uint32_t& value) {
    if (offset > len || len - offset < 4) return false;
    std::memcpy(&value, p + offset, 4);
    return true;
}

inline bool isDeclarationOp(uint32_t op) {
    return (op >= 88 && op <= 106) || (op >= 143 && op <= 163) || op == kOpCustomData;
}

// One operand of a declaration: the type, the component selection and up to two immediate indices. False for anything else (relative indices, 64-bit indices,
// an extended operand token, an operand that runs past the program).
struct DclOperand {
    uint32_t type = 0, mode = 0, mask = 0, dim = 0, idx[2] = {0, 0};
};
inline bool readDclOperand(const std::vector<uint32_t>& t, size_t at, size_t end, DclOperand& op) {
    if (at >= end) return false;
    const uint32_t tok = t[at++];
    if (tok & 0x80000000u) return false;
    op.type = (tok >> 12) & 255u;
    op.mode = (tok >> 2) & 3u;
    op.mask = (tok >> 4) & 15u;
    op.dim = (tok >> 20) & 3u;
    if ((tok & 3u) != 2u || op.dim > 2) return false;   // four components
    for (uint32_t d = 0; d < op.dim; ++d) {
        if (((tok >> (22 + 3 * d)) & 7u) != 0) return false;   // immediate 32-bit index only
        if (at >= end) return false;
        op.idx[d] = t[at++];
    }
    return at == end;   // nothing else in the declaration
}
}  // namespace glass_layout_detail

// True when the pixel shader's container holds a ps_5_0 program with exactly the glass layout. Never throws.
inline bool glassLayoutPs(const void* bytecode, size_t len) noexcept {
    using namespace glass_layout_detail;
    try {
        if (!bytecode || len < 32) return false;
        const auto* p = static_cast<const unsigned char*>(bytecode);
        uint32_t total = 0, count = 0;
        if (std::memcmp(p, "DXBC", 4) != 0 || !readWord(p, len, 24, total) || total != len || !readWord(p, len, 28, count) || count > 128) return false;
        const unsigned char* program = nullptr;
        size_t programBytes = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t offset = 0, tag = 0, size = 0;
            if (!readWord(p, len, 32 + size_t(i) * 4, offset) || !readWord(p, len, offset, tag) || !readWord(p, len, size_t(offset) + 4, size)) return false;
            if (size > len - offset - 8) return false;   // offset + 8 is within len: the size word was read
            if (tag != kTagShex && tag != kTagShdr) continue;
            if (program) return false;   // one program only
            program = p + offset + 8;
            programBytes = size;
        }
        if (!program || programBytes < 8 || (programBytes & 3)) return false;
        uint32_t version = 0, words = 0;
        std::memcpy(&version, program, 4);
        std::memcpy(&words, program + 4, 4);
        if (version != kPs50 || words != programBytes / 4) return false;
        std::vector<uint32_t> t(std::min<size_t>(words, kPrefixWords));
        std::memcpy(t.data(), program, t.size() * 4);

        uint32_t cbSeen = 0;                 // bit per slot: 1 and 2 only
        uint32_t cbCount = 0, cb2Size = 0;
        uint32_t inputMask[5] = {0, 0, 0, 0, 0};
        bool inputSeen[5] = {false, false, false, false, false};
        uint32_t inputCount = 0;
        uint32_t outputCount = 0;
        size_t at = 2;
        while (at < t.size()) {
            const uint32_t opcode = t[at] & 0x7ffu;
            if (!isDeclarationOp(opcode)) break;
            const size_t length = dxbc_container::instructionLength(t, at);
            const size_t end = at + length;
            if (opcode != kOpCustomData) {
                size_t body = at + 1;   // past any extended instruction tokens
                uint32_t head = t[at];
                while ((head & 0x80000000u) != 0 && body < end) head = t[body++];
                if (opcode == kOpDclConstantBuffer) {
                    DclOperand op;
                    if (!readDclOperand(t, body, end, op) || op.type != kOperandCb || op.dim != 2) return false;
                    const uint32_t slot = op.idx[0], size = op.idx[1];
                    if (slot != 1 && slot != 2) return false;
                    if (cbSeen & (1u << slot)) return false;
                    cbSeen |= 1u << slot;
                    ++cbCount;
                    if (slot == 2) cb2Size = size;
                } else if (opcode == kOpDclInputPs) {
                    DclOperand op;
                    if (!readDclOperand(t, body, end, op) || op.type != kOperandInput || op.dim != 1 || op.mode != 0) return false;
                    if (op.idx[0] > 4 || inputSeen[op.idx[0]]) return false;
                    inputSeen[op.idx[0]] = true;
                    inputMask[op.idx[0]] = op.mask;
                    ++inputCount;
                } else if (opcode == kOpDclOutput) {
                    DclOperand op;
                    if (!readDclOperand(t, body, end, op) || op.type != kOperandOutput || op.dim != 1 || op.mode != 0) return false;
                    if (op.idx[0] != 0 || op.mask != 0xFu) return false;
                    ++outputCount;
                } else if (opcode == kOpDclOutputSgv || opcode == kOpDclOutputSiv) {
                    return false;
                }
                // dcl_input_ps_sgv and dcl_input_ps_siv are ignored, as is every other declaration.
            }
            at = end;
        }
        if (at >= t.size() && t.size() < words) return false;   // the declarations ran past the prefix
        if (cbCount != 2 || cb2Size != 3) return false;
        static const uint32_t kMasks[5] = {0x1u, 0x7u, 0x7u, 0x7u, 0x3u};
        if (inputCount != 5) return false;
        for (int i = 0; i < 5; ++i)
            if (!inputSeen[i] || inputMask[i] != kMasks[i]) return false;
        return outputCount == 1;
    } catch (...) {
        return false;
    }
}

}  // namespace concourse
}  // namespace edvr
