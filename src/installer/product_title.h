// What the window calls the product: its heading, title bar and message boxes.
// Running text keeps the short name, ED-UP, where the full title would not fit.
#pragma once

namespace edvr::installer {

inline constexpr const wchar_t* kProductTitle = L"ED Unofficial Patch";

#ifdef EDUP_LAUNCHER
inline constexpr const wchar_t* kAppTitle = L"ED Unofficial Patch";
#else
inline constexpr const wchar_t* kAppTitle = L"ED Unofficial Patch installer";
#endif

}  // namespace edvr::installer
