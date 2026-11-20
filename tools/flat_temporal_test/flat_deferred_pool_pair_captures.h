#pragma once
// A real triple from a flat field report (v0.22.0-rc.7, 3440x1440 render = output, the HDR route; edvr_gfx_20261109_171205 and its
// failure snapshot flat_trace_42357.bin): on foot with a tool held, two source records stand on the deferred lighting pass's depth,
// the world's camera (near 0.025, first VS AACFDCF2FB9AD809 / PS CF534B32F491561A, 360 draws) and the tool's first-person camera
// (near 0.0675, VS AACFDCF2FB9AD809 / PS CF534B32F491561A, 5 draws), both into SRV0. CB2 rows 10..16 and the phase bits are the log's
// `flat deferred failure original bytes` and `witness` lines for the frame; the two cameras are the trace's 96-byte blocks.
// Generated, not typed: the scratch script read the log line and the trace event bytes.
namespace deferred_pool_pair {
struct Capture { unsigned frame; unsigned width, height; unsigned phaseX, phaseY; const char* world; const char* firstPerson; const char* cb2; };
inline const Capture kCaptures[] = {
    { 42357u, 3440, 1440, 0xBEC00000U, 0xBD638E38U,
      "46AFCB3EBC2734BD00000000ACFA9DBEC16105BEE48600BE00000000EC1E71BF5BE1973ACEB97D3F000000005D1508BE0000000000000000CDCCCC3C00000000ACFA9DBEEC1E71BF5D1508BE00000000C5F7034018E63241C88D3A4000000000",
      "EC326A3FD719CFBD00000000ACFA9DBEEE8399BE5BBF93BE00000000EC1E71BF782E2C3B1AE21140000000005D1508BE0000000000000000713D8A3D00000000ACFA9DBEEC1E71BF5D1508BE00000000C5F7034018E63241C88D3A4000000000",
      "46AFCB3EC16105BE5BE1973A68FA213FBC2734BDE48600BECEB97D3F968CB2BF000000000000000000000000CDCCCC3CACFA9DBEEC1E71BF5D1508BEECE038410B469140C20EB43D000000009BD527C07A98BEBFD673803E00000000CA46A5BE9544533C76BAFDBF00000000960E5A3F" },
};
}
