// The scale the launcher's window is laid out at, when the real one would not fit the screen.
//
// The window is 786 design units tall. At 150% that is 1179 pixels, taller than a 1080p monitor's work
// area, and at 100% it is taller than the Steam Deck's 800 pixel screen: the Play buttons, at the bottom,
// are then off the screen. layoutDpiFor returns the dpi to lay the client area out at: the system's when
// it fits, otherwise the largest that does, and never below 72 (75%), where the text stops being readable.
// The window's frame and title bar keep the real dpi; only the client area shrinks.
#pragma once

namespace edvr::installer {

// `workAreaHeightPx`: the monitor's work area. `clientHeightUnits`: the client area in 96 dpi units.
// `chromeHeightPx`: the frame and title bar at the real dpi, which the client area has to leave room for.
inline unsigned layoutDpiFor(unsigned systemDpi, int workAreaHeightPx, int clientHeightUnits, int chromeHeightPx) {
    constexpr unsigned kFloor = 72;
    if (clientHeightUnits <= 0 || workAreaHeightPx <= chromeHeightPx) return systemDpi;
    const long long room = static_cast<long long>(workAreaHeightPx) - chromeHeightPx;
    // The largest dpi at which clientHeightUnits * dpi / 96 still fits in `room`, rounded down.
    const long long fits = room * 96 / clientHeightUnits;
    unsigned dpi = fits < static_cast<long long>(systemDpi) ? static_cast<unsigned>(fits) : systemDpi;
    if (dpi < kFloor) dpi = kFloor;
    // A system dpi that is already below the floor is left alone: it is the user's own choice.
    return dpi < systemDpi ? dpi : systemDpi;
}

}  // namespace edvr::installer
