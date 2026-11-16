// The flat F8 menu's footer, first line: what the keys do on the highlighted row (menu.cpp, the flat branch of the footer). A hotkey row is rebound with
// Enter (or A) and Left/Right do nothing there; while a row waits for its key, the capture's own keys (a key, a pad button or a HOTAS button is taken).
// The footer line is drawn in the Small face (em = the cap height); tools\native_menu_test measures every legend here against the line at each flat size.
//
// Pure: no windows.h, no D3D.
#pragma once

namespace edvr {

inline constexpr char kFlatFooterKeys[]       = "Up/Down row  Left/Right change  Tab page";
inline constexpr char kFlatFooterHotkey[]     = "Up/Down row  Enter rebind  Tab page";
inline constexpr char kFlatFooterPad[]        = "D-pad move/change  A select  B back";
inline constexpr char kFlatFooterPadHotkey[]  = "D-pad move  A rebind  B back";
inline constexpr char kFlatFooterCapture[]    = "Press a key or button  Esc cancels  Delete clears";
inline constexpr char kFlatFooterCaptureKeep[] = "Press a key or button  Esc cancels";   // the menu key, which cannot be cleared

inline constexpr const char* kFlatFooterLegends[] = {kFlatFooterKeys, kFlatFooterHotkey, kFlatFooterPad, kFlatFooterPadHotkey, kFlatFooterCapture,
                                                     kFlatFooterCaptureKeep};

}  // namespace edvr
