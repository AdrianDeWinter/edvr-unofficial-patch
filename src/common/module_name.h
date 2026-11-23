#pragma once
#include <cstddef>
#include <cstdint>

// Brief module-plus-offset naming shared by diagnostic probes: the file's own
// image is prefixed so a stack never blames the proxy by mistake. (Two local
// copies already exist -- vtable_hook.cpp's ownerModuleBrief and
// flat_runtime.cpp's witnessModuleBrief; new users take this one.)
namespace edvr {
inline const char* moduleBrief(void* p, char* buf, size_t bufLen) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) || !mbi.AllocationBase) return "no module";
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), path, sizeof(path))) return "no module";
    const char* leaf = path;
    for (const char* c = path; *c; ++c) if (*c == '\\' || *c == '/') leaf = c + 1;
    static HMODULE self = nullptr;
    if (!self) {
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&moduleBrief), &self);
    }
    _snprintf_s(buf, bufLen, _TRUNCATE, "%s%s+0x%llX",
                (self && mbi.AllocationBase == self) ? "ED-UP's own " : "", leaf,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p) -
                                                reinterpret_cast<uintptr_t>(mbi.AllocationBase)));
    return buf;
}
} // namespace edvr
