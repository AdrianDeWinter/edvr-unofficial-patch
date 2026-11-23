#pragma once
#include <cstdint>
#include "../common/runtime_profile.h"
#include "../openxr/native_cpu_trace.h"

namespace edvr {

// The flat profile's CPU markers (event 5 and the spans 0x200-0x207). Emitted only while an ETW session has the
// provider on AND the flat profile runs, so a VR trace is unchanged. With no session the cost is one acquire load
// of the enabled flag (a plain load on x86); runtimeFlatProfile() is not reached.
inline bool flatCpuTraceOn() noexcept {
    return openxr::NativeCpuTrace::get().enabled() && runtimeFlatProfile();
}

// The render thread's own state between owned Presents (device_hook.cpp hookedPresent). Only that thread touches it.
struct FlatCpuTraceFrameState {
    uint64_t prevHookReturnUs = 0;   // hookReturnUs of the previous owned Present; 0 = unknown
    unsigned long prevThread = 0;    // the thread that made that Present
};
inline FlatCpuTraceFrameState g_flatCpuTraceFrame{};

}  // namespace edvr
