#pragma once

#include <windows.h>

namespace edvr {

// One export of a dynamically loaded library, cast to the type of `target`. False when the module lacks it.
// Used for the XeSS runtimes (xess_engine.cpp, xess_fg.cpp), which EDVR loads without an import library.
template <typename T> bool loadProc(HMODULE module, const char* name, T& target) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    return target != nullptr;
}

}  // namespace edvr
