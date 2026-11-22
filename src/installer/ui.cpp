#include "ui.h"

#include <commctrl.h>
#include <dwmapi.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace edvr::installer::ui {
namespace {

Theme     g_theme;
Fonts     g_fonts;
ThemeMode g_themeMode = ThemeMode::System;
bool      g_themeRead = false;
bool  g_keyboardFocus = false;
bool  g_mousePressing = false;

// Per-control state for the owner-drawn ones. A handful of buttons and
// checkboxes, so a vector scan is cheaper than a map and easier to read.
struct Owned {
    HWND        hwnd = nullptr;
    ButtonStyle style = ButtonStyle::Secondary;
    bool        isCheckbox = false;
    bool        checked = false;
    bool        hot = false;
    bool        pressed = false;
};
std::vector<Owned> g_owned;

Owned* find(HWND hwnd) {
    for (Owned& o : g_owned) {
        if (o.hwnd == hwnd) return &o;
    }
    return nullptr;
}

COLORREF mix(COLORREF a, COLORREF b, int percentB) {
    const int r = (GetRValue(a) * (100 - percentB) + GetRValue(b) * percentB) / 100;
    const int g = (GetGValue(a) * (100 - percentB) + GetGValue(b) * percentB) / 100;
    const int bl = (GetBValue(a) * (100 - percentB) + GetBValue(b) * percentB) / 100;
    return RGB(r, g, bl);
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

uint32_t packPremul(float r, float g, float b, float alpha) {
    const float a = clampf(alpha, 0.0f, 1.0f);
    const uint32_t ab = static_cast<uint32_t>(a * 255.0f + 0.5f);
    if (ab == 0) return 0u;
    const uint32_t rb = static_cast<uint32_t>(clampf(r, 0.0f, 255.0f) * a + 0.5f);
    const uint32_t gb = static_cast<uint32_t>(clampf(g, 0.0f, 255.0f) * a + 0.5f);
    const uint32_t bb = static_cast<uint32_t>(clampf(b, 0.0f, 255.0f) * a + 0.5f);
    return (ab << 24) | ((rb > ab ? ab : rb) << 16) | ((gb > ab ? ab : gb) << 8) |
           (bb > ab ? ab : bb);
}

uint32_t packPremulColor(COLORREF c, float alpha) {
    return packPremul(static_cast<float>(GetRValue(c)), static_cast<float>(GetGValue(c)),
                      static_cast<float>(GetBValue(c)), alpha);
}

// Analytical signed distance to a rounded box centred at (cx, cy) with
// half-extents (hx, hy) and corner radius r. Negative inside, zero on edge.
float sdRoundedBox(float px, float py, float cx, float cy, float hx, float hy, float r) {
    const float qx = std::fabs(px - cx) - (hx - r);
    const float qy = std::fabs(py - cy) - (hy - r);
    const float ox = qx > 0.0f ? qx : 0.0f;
    const float oy = qy > 0.0f ? qy : 0.0f;
    const float inside = qx > qy ? qx : qy;
    return std::sqrt(ox * ox + oy * oy) + (inside < 0.0f ? inside : 0.0f) - r;
}

float sdSegment(float px, float py, float ax, float ay, float bx, float by) {
    const float pax = px - ax;
    const float pay = py - ay;
    const float bax = bx - ax;
    const float bay = by - ay;
    const float denom = bax * bax + bay * bay;
    const float h = denom > 0.0f ? clampf((pax * bax + pay * bay) / denom, 0.0f, 1.0f) : 0.0f;
    return std::hypot(pax - bax * h, pay - bay * h);
}

// Renders a tile [x0, x0 + w) x [y0, y0 + h) into a premultiplied 32-bit BGRA
// DIB and composites it onto dc via GdiAlphaBlend. Gives identical analytical
// subpixel antialiasing across Windows and Wine without GDI+ region fallbacks.
template <typename PixelFn>
void blendTile(HDC dc, int x0, int y0, int w, int h, PixelFn&& pixelAt) {
    if (w <= 0 || h <= 0) return;
    HDC memDc = CreateCompatibleDC(dc);
    if (!memDc) return;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(memDc);
        return;
    }
    uint32_t* dst = static_cast<uint32_t*>(bits);
    for (int iy = 0; iy < h; ++iy) {
        const float py = static_cast<float>(y0 + iy) + 0.5f;
        uint32_t* row = dst + static_cast<size_t>(iy) * static_cast<size_t>(w);
        for (int ix = 0; ix < w; ++ix) {
            const float px = static_cast<float>(x0 + ix) + 0.5f;
            row[ix] = pixelAt(px, py);
        }
    }
    HGDIOBJ prev = SelectObject(memDc, bmp);
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    GdiAlphaBlend(dc, x0, y0, w, h, memDc, 0, 0, w, h, bf);
    SelectObject(memDc, prev);
    DeleteObject(bmp);
    DeleteDC(memDc);
}

void fillAndStrokeRounded(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF stroke,
                          bool hasFill, bool hasStroke) {
    const int w = rect.right - rect.left;
    const int h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0 || (!hasFill && !hasStroke)) return;

    const float hx = static_cast<float>(w) * 0.5f;
    const float hy = static_cast<float>(h) * 0.5f;
    const float cx = static_cast<float>(rect.left) + hx;
    const float cy = static_cast<float>(rect.top) + hy;
    const float maxR = hx < hy ? hx : hy;
    const float r = clampf(static_cast<float>(radius), 0.0f, maxR);

    if (r <= 0.25f) {
        if (hasFill) fillRect(dc, rect, fill);
        if (hasStroke) {
            fillRect(dc, RECT{rect.left, rect.top, rect.right, rect.top + 1}, stroke);
            fillRect(dc, RECT{rect.left, rect.bottom - 1, rect.right, rect.bottom}, stroke);
            fillRect(dc, RECT{rect.left, rect.top + 1, rect.left + 1, rect.bottom - 1}, stroke);
            fillRect(dc, RECT{rect.right - 1, rect.top + 1, rect.right, rect.bottom - 1}, stroke);
        }
        return;
    }

    const float fr = static_cast<float>(GetRValue(fill));
    const float fg = static_cast<float>(GetGValue(fill));
    const float fb = static_cast<float>(GetBValue(fill));
    const float sr = static_cast<float>(GetRValue(stroke));
    const float sg = static_cast<float>(GetGValue(stroke));
    const float sb = static_cast<float>(GetBValue(stroke));

    auto pixelAt = [=](float px, float py) -> uint32_t {
        const float sd = sdRoundedBox(px, py, cx, cy, hx, hy, r);
        if (hasFill && hasStroke) {
            const float outerCov = clampf(0.5f - sd, 0.0f, 1.0f);
            if (outerCov <= 0.0f) return 0u;
            const float innerCov = clampf(0.5f - (sd + 1.0f), 0.0f, 1.0f);
            const float rr = sr + (fr - sr) * innerCov;
            const float gg = sg + (fg - sg) * innerCov;
            const float bb = sb + (fb - sb) * innerCov;
            return packPremul(rr, gg, bb, outerCov);
        }
        if (hasFill) {
            const float cov = clampf(0.5f - sd, 0.0f, 1.0f);
            return cov > 0.0f ? packPremul(fr, fg, fb, cov) : 0u;
        }
        const float d = std::fabs(sd + 0.5f) - 0.5f;
        const float cov = clampf(0.5f - d, 0.0f, 1.0f);
        return cov > 0.0f ? packPremul(sr, sg, sb, cov) : 0u;
    };

    const int corner = static_cast<int>(std::ceil(r)) + 1;
    if (w <= corner * 2 || h <= corner * 2 || static_cast<long long>(w) * h <= 65536LL) {
        blendTile(dc, rect.left, rect.top, w, h, pixelAt);
        return;
    }

    // Large cards: blend only the four small corner tiles and paint the
    // straight edges and interior directly with GDI FillRect.
    blendTile(dc, rect.left, rect.top, corner, corner, pixelAt);
    blendTile(dc, rect.right - corner, rect.top, corner, corner, pixelAt);
    blendTile(dc, rect.left, rect.bottom - corner, corner, corner, pixelAt);
    blendTile(dc, rect.right - corner, rect.bottom - corner, corner, corner, pixelAt);

    if (hasFill && hasStroke) {
        fillRect(dc, RECT{rect.left + corner, rect.top, rect.right - corner, rect.top + 1}, stroke);
        fillRect(dc, RECT{rect.left + corner, rect.top + 1, rect.right - corner, rect.top + corner},
                 fill);
        fillRect(dc, RECT{rect.left + corner, rect.bottom - 1, rect.right - corner, rect.bottom},
                 stroke);
        fillRect(
            dc,
            RECT{rect.left + corner, rect.bottom - corner, rect.right - corner, rect.bottom - 1},
            fill);
        fillRect(dc, RECT{rect.left, rect.top + corner, rect.left + 1, rect.bottom - corner},
                 stroke);
        fillRect(dc, RECT{rect.right - 1, rect.top + corner, rect.right, rect.bottom - corner},
                 stroke);
        fillRect(dc, RECT{rect.left + 1, rect.top + corner, rect.right - 1, rect.bottom - corner},
                 fill);
    } else if (hasFill) {
        fillRect(dc, RECT{rect.left + corner, rect.top, rect.right - corner, rect.top + corner},
                 fill);
        fillRect(dc, RECT{rect.left, rect.top + corner, rect.right, rect.bottom - corner}, fill);
        fillRect(dc,
                 RECT{rect.left + corner, rect.bottom - corner, rect.right - corner, rect.bottom},
                 fill);
    } else {
        fillRect(dc, RECT{rect.left + corner, rect.top, rect.right - corner, rect.top + 1}, stroke);
        fillRect(dc, RECT{rect.left + corner, rect.bottom - 1, rect.right - corner, rect.bottom},
                 stroke);
        fillRect(dc, RECT{rect.left, rect.top + corner, rect.left + 1, rect.bottom - corner},
                 stroke);
        fillRect(dc, RECT{rect.right - 1, rect.top + corner, rect.right, rect.bottom - corner},
                 stroke);
    }
}

bool systemPrefersDark() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1, size = sizeof(value), type = 0;
    const LSTATUS st = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                        reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return st == ERROR_SUCCESS && type == REG_DWORD && value == 0;
}

// The system accent, so the primary button belongs to the machine it is running
// on rather than to a palette chosen here. Falls back to a blue that works in
// both themes.
COLORREF systemAccent(bool dark) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", 0, KEY_READ,
                      &key) == ERROR_SUCCESS) {
        DWORD value = 0, size = sizeof(value), type = 0;
        const LSTATUS st = RegQueryValueExW(key, L"AccentColor", nullptr, &type,
                                            reinterpret_cast<BYTE*>(&value), &size);
        RegCloseKey(key);
        if (st == ERROR_SUCCESS && type == REG_DWORD) {
            // Stored ABGR.
            const COLORREF accent = RGB(value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF);
            // Some accents are too dark or too light to carry white text on a
            // button; nudge rather than refuse.
            const int luma = (GetRValue(accent) * 299 + GetGValue(accent) * 587 +
                              GetBValue(accent) * 114) / 1000;
            if (luma > 150) return mix(accent, RGB(0, 0, 0), 25);
            if (luma < 40) return mix(accent, RGB(255, 255, 255), 20);
            return accent;
        }
    }
    return dark ? RGB(76, 148, 255) : RGB(0, 95, 184);
}

bool fontExists(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    EnumFontFamiliesExW(
        dc, &lf,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param) -> int {
            *reinterpret_cast<bool*>(param) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

HFONT makeFont(const wchar_t* face, int pointsTimesTen, int weight, UINT dpi) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(pointsTimesTen, static_cast<int>(dpi), 720);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, face);
    return CreateFontIndirectW(&lf);
}

// Hover and press, which owner-drawn buttons do not get for free.
LRESULT CALLBACK ownedProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                           UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    Owned* owned = find(hwnd);
    switch (message) {
        case WM_MOUSEMOVE: {
            if (owned && !owned->hot) {
                owned->hot = true;
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            if (owned) {
                owned->hot = false;
                owned->pressed = false;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;
        }
        case WM_LBUTTONDOWN:
            g_keyboardFocus = false;
            g_mousePressing = true;
            if (owned) {
                owned->pressed = true;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            {
                const LRESULT res = DefSubclassProc(hwnd, message, wparam, lparam);
                g_mousePressing = false;
                return res;
            }
        case WM_RBUTTONDOWN:
            g_keyboardFocus = false;
            break;
        case WM_LBUTTONUP:
            if (owned) {
                owned->pressed = false;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (!g_keyboardFocus) {
                g_keyboardFocus = true;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;
        case WM_SETFOCUS:
            if (!g_mousePressing && (GetKeyState(VK_LBUTTON) & 0x8000) == 0 &&
                (GetKeyState(VK_MENU) & 0x8000) == 0) {
                const bool navKey = ((GetKeyState(VK_TAB) | GetKeyState(VK_LEFT) |
                                      GetKeyState(VK_RIGHT) | GetKeyState(VK_UP) |
                                      GetKeyState(VK_DOWN)) &
                                     0x8000) != 0;
                if (navKey) g_keyboardFocus = true;
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, ownedProc, 1);
            for (size_t i = 0; i < g_owned.size(); ++i) {
                if (g_owned[i].hwnd == hwnd) {
                    g_owned.erase(g_owned.begin() + i);
                    break;
                }
            }
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

}  // namespace

void startup() {
    refreshTheme();
}

void shutdown() {
    releaseFonts();
}

int dp(int value96, UINT dpi) { return MulDiv(value96, static_cast<int>(dpi), 96); }

void setThemeMode(ThemeMode mode) {
    g_themeMode = mode;
    refreshTheme();
}

ThemeMode themeMode() { return g_themeMode; }

void refreshTheme() {
    const bool dark = g_themeMode == ThemeMode::Dark  ? true
                      : g_themeMode == ThemeMode::Light ? false
                                                        : systemPrefersDark();
    Theme t;
    t.dark = dark;
    if (dark) {
        t.windowBg = RGB(26, 28, 32);
        t.cardBg = RGB(35, 38, 44);
        t.cardBorder = RGB(50, 55, 65);
        t.text = RGB(240, 242, 245);
        t.subtext = RGB(157, 165, 180);
        t.control = RGB(45, 49, 57);
        t.controlHover = RGB(56, 61, 71);
        t.controlBorder = RGB(70, 76, 89);
        t.good = RGB(95, 208, 135);
        t.warn = RGB(242, 184, 87);
        t.bad = RGB(240, 110, 101);
        t.muted = RGB(126, 134, 148);
    } else {
        t.windowBg = RGB(242, 244, 247);
        t.cardBg = RGB(255, 255, 255);
        t.cardBorder = RGB(221, 225, 232);
        t.text = RGB(23, 26, 31);
        t.subtext = RGB(92, 101, 115);
        t.control = RGB(248, 249, 251);
        t.controlHover = RGB(238, 240, 244);
        t.controlBorder = RGB(206, 212, 222);
        t.good = RGB(18, 122, 67);
        t.warn = RGB(168, 98, 0);
        t.bad = RGB(196, 43, 34);
        t.muted = RGB(135, 144, 158);
    }
    t.accent = systemAccent(dark);
    t.accentHover = mix(t.accent, dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 12);
    t.accentText = RGB(255, 255, 255);
    g_theme = t;
    g_themeRead = true;
}

const Theme& theme() {
    if (!g_themeRead) refreshTheme();
    return g_theme;
}

void buildFonts(UINT dpi) {
    releaseFonts();
    // Segoe UI Variable is the Windows 11 face; Segoe UI is the Windows 10 one.
    // Asking for a face that is not installed gets a silent substitution, which
    // is how a program ends up rendering in something nobody chose.
    const wchar_t* display = fontExists(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display"
                                                                     : L"Segoe UI";
    const wchar_t* body = fontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text"
                                                                : L"Segoe UI";
    g_fonts.title = makeFont(display, 180, FW_SEMIBOLD, dpi);
    g_fonts.heading = makeFont(body, 110, FW_SEMIBOLD, dpi);
    g_fonts.body = makeFont(body, 100, FW_NORMAL, dpi);
    g_fonts.bodyBold = makeFont(body, 100, FW_SEMIBOLD, dpi);
    g_fonts.caption = makeFont(body, 90, FW_NORMAL, dpi);
}

void releaseFonts() {
    HFONT* all[] = {&g_fonts.title, &g_fonts.heading, &g_fonts.body, &g_fonts.bodyBold,
                    &g_fonts.caption};
    for (HFONT* f : all) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

const Fonts& fonts() { return g_fonts; }

void applyWindowChrome(HWND window) {
    // Both of these are Windows 10 20H1 / Windows 11 attributes. On anything
    // older DwmSetWindowAttribute simply fails, which is the desired outcome:
    // the window keeps the system's own chrome.
    const BOOL dark = theme().dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const DWORD round = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
}

void fillRounded(HDC dc, RECT rect, int radius, COLORREF fill) {
    fillAndStrokeRounded(dc, rect, radius, fill, fill, true, false);
}

void strokeRounded(HDC dc, RECT rect, int radius, COLORREF stroke, int inset) {
    RECT r{rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset};
    const int rad = radius > inset ? (radius - inset) : 0;
    fillAndStrokeRounded(dc, r, rad, stroke, stroke, false, true);
}

void fillCircle(HDC dc, int centreX, int centreY, int radius, COLORREF fill) {
    if (radius <= 0) return;
    const float cx = static_cast<float>(centreX);
    const float cy = static_cast<float>(centreY);
    const float rad = static_cast<float>(radius);
    const int x0 = centreX - radius - 1;
    const int y0 = centreY - radius - 1;
    const int side = radius * 2 + 2;
    blendTile(dc, x0, y0, side, side, [=](float px, float py) -> uint32_t {
        const float sd = std::hypot(px - cx, py - cy) - rad;
        const float cov = clampf(0.5f - sd, 0.0f, 1.0f);
        return cov > 0.0f ? packPremulColor(fill, cov) : 0u;
    });
}

void paintToggle(HDC dc, RECT box, bool on, UINT dpi) {
    const Theme& t = theme();
    const int w = box.right - box.left;
    const int h = box.bottom - box.top;
    if (w <= 2 || h <= 2) return;

    const float hx = static_cast<float>(w) * 0.5f;
    const float hy = static_cast<float>(h) * 0.5f;
    const float cx = static_cast<float>(box.left) + hx;
    const float cy = static_cast<float>(box.top) + hy;
    const float trackR = hx < hy ? hx : hy;
    const float inset = static_cast<float>(dp(3, dpi));
    const float knobR = trackR - inset;
    const float knobCx = on ? (static_cast<float>(box.right) - trackR)
                            : (static_cast<float>(box.left) + trackR);

    const COLORREF trackFill = on ? t.accent : t.control;
    const COLORREF trackBorder = t.controlBorder;
    const COLORREF knobColor = on ? t.accentText : t.subtext;

    const float fr = static_cast<float>(GetRValue(trackFill));
    const float fg = static_cast<float>(GetGValue(trackFill));
    const float fb = static_cast<float>(GetBValue(trackFill));
    const float sr = static_cast<float>(GetRValue(trackBorder));
    const float sg = static_cast<float>(GetGValue(trackBorder));
    const float sb = static_cast<float>(GetBValue(trackBorder));
    const float kr = static_cast<float>(GetRValue(knobColor));
    const float kg = static_cast<float>(GetGValue(knobColor));
    const float kb = static_cast<float>(GetBValue(knobColor));

    blendTile(dc, box.left, box.top, w, h, [=](float px, float py) -> uint32_t {
        const float trackSd = sdRoundedBox(px, py, cx, cy, hx, hy, trackR);
        const float outerCov = clampf(0.5f - trackSd, 0.0f, 1.0f);
        if (outerCov <= 0.0f) return 0u;

        float rr = fr;
        float gg = fg;
        float bb = fb;
        if (!on) {
            const float innerCov = clampf(0.5f - (trackSd + 1.0f), 0.0f, 1.0f);
            rr = sr + (fr - sr) * innerCov;
            gg = sg + (fg - sg) * innerCov;
            bb = sb + (fb - sb) * innerCov;
        }
        if (knobR > 1.0f) {
            const float knobSd = std::hypot(px - knobCx, py - cy) - knobR;
            const float knobCov = clampf(0.5f - knobSd, 0.0f, 1.0f);
            if (knobCov > 0.0f) {
                rr = rr + (kr - rr) * knobCov;
                gg = gg + (kg - gg) * knobCov;
                bb = bb + (kb - bb) * knobCov;
            }
        }
        return packPremul(rr, gg, bb, outerCov);
    });
}

void fillRect(HDC dc, RECT rect, COLORREF fill) {
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void drawText(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF colour,
              UINT format) {
    HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    const int previousMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF previousColour = SetTextColor(dc, colour);
    DrawTextW(dc, text.c_str(), -1, &rect, format | DT_NOPREFIX);
    SetTextColor(dc, previousColour);
    SetBkMode(dc, previousMode);
    SelectObject(dc, previous);
}

int textWidth(HDC dc, const std::wstring& text, HFONT font) {
    HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, previous);
    return size.cx;
}

void paintCard(HDC dc, RECT rect, UINT dpi) {
    const Theme& t = theme();
    fillAndStrokeRounded(dc, rect, dp(8, dpi), t.cardBg, t.cardBorder, true, true);
}

void paintAccentCard(HDC dc, RECT rect, UINT dpi) {
    const Theme& t = theme();
    paintCard(dc, rect, dpi);
    const int railW = dp(3, dpi) > 2 ? dp(3, dpi) : 2;
    const int insetY = dp(12, dpi);
    if (rect.bottom - rect.top > insetY * 2 + 4) {
        RECT rail{rect.left + dp(3, dpi), rect.top + insetY, rect.left + dp(3, dpi) + railW,
                  rect.bottom - insetY};
        fillRounded(dc, rail, railW / 2 + 1, t.accent);
    }
}

int paintStatusPill(HDC dc, int left, int top, int height, COLORREF toneColour,
                    const std::wstring& text, HFONT font, UINT dpi) {
    const Theme& t = theme();
    const int padX = dp(8, dpi);
    const int dotR = dp(3, dpi) > 2 ? dp(3, dpi) : 2;
    const int dotGap = dp(6, dpi);
    const int labelW = textWidth(dc, text, font);
    const int pillW = padX + dotR * 2 + dotGap + labelW + padX;
    RECT pill{left, top, left + pillW, top + height};

    const COLORREF fill = mix(t.cardBg, toneColour, t.dark ? 22 : 12);
    const COLORREF border = mix(t.cardBorder, toneColour, t.dark ? 44 : 34);
    const COLORREF textColour = t.dark ? mix(toneColour, RGB(255, 255, 255), 22)
                                       : mix(toneColour, RGB(0, 0, 0), 14);
    fillAndStrokeRounded(dc, pill, height / 2, fill, border, true, true);
    fillCircle(dc, left + padX + dotR, top + height / 2, dotR, toneColour);

    RECT labelRect{left + padX + dotR * 2 + dotGap, top, pill.right - padX + dp(2, dpi),
                   pill.bottom};
    drawText(dc, text, labelRect, font, textColour, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return pillW;
}

HWND makeButton(HWND parent, const wchar_t* text, int id, ButtonStyle style, HFONT font) {
    HWND button = CreateWindowExW(0, L"BUTTON", text,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                  parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  nullptr, nullptr);
    if (!button) return nullptr;
    if (font) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    Owned owned;
    owned.hwnd = button;
    owned.style = style;
    g_owned.push_back(owned);
    SetWindowSubclass(button, ownedProc, 1, 0);
    return button;
}

void setButtonStyle(HWND button, ButtonStyle style) {
    if (Owned* owned = find(button)) {
        owned->style = style;
        InvalidateRect(button, nullptr, TRUE);
    }
}

HWND makeCheckbox(HWND parent, const wchar_t* text, int id, bool checked, HFONT font) {
    HWND box = CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                               parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr,
                               nullptr);
    if (!box) return nullptr;
    if (font) SendMessageW(box, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    Owned owned;
    owned.hwnd = box;
    owned.isCheckbox = true;
    owned.checked = checked;
    g_owned.push_back(owned);
    SetWindowSubclass(box, ownedProc, 1, 0);
    return box;
}

bool checkboxChecked(HWND box) {
    const Owned* owned = find(box);
    return owned && owned->checked;
}

void toggleCheckbox(HWND box) {
    if (Owned* owned = find(box)) {
        owned->checked = !owned->checked;
        InvalidateRect(box, nullptr, TRUE);
    }
}

void drawButton(const DRAWITEMSTRUCT* item, UINT dpi) {
    const Owned* owned = find(item->hwndItem);
    if (!owned) return;
    const Theme& t = theme();
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool showFocus = g_keyboardFocus && (item->itemState & ODS_FOCUS) != 0 &&
                           (item->itemState & ODS_NOFOCUSRECT) == 0 && !disabled;

    wchar_t label[256]{};
    GetWindowTextW(item->hwndItem, label, 256);

    RECT rect = item->rcItem;
    const int radius = dp(6, dpi);
    HDC dc = item->hDC;

    // The parent's card or window colour shows through the rounded corners, so
    // the button has to know which surface it is sitting on.
    const bool onWindow = owned->style == ButtonStyle::Tab ||
                          owned->style == ButtonStyle::TabActive ||
                          owned->style == ButtonStyle::Link;
    fillRect(dc, rect, onWindow ? t.windowBg : t.cardBg);

    COLORREF face = t.control;
    COLORREF border = t.controlBorder;
    COLORREF ink = t.text;

    if (owned->style == ButtonStyle::ThemeToggle) {
        fillRect(dc, rect, t.windowBg);
        const int w = rect.right - rect.left;
        const int h = rect.bottom - rect.top;
        if (w < 20 || h < 12) return;

        const COLORREF trackFill = owned->hot ? t.controlHover : t.cardBg;
        const COLORREF trackBorder = owned->hot ? t.controlBorder : t.cardBorder;
        fillAndStrokeRounded(dc, rect, h / 2, trackFill, trackBorder, true, true);
        if (showFocus) strokeRounded(dc, rect, h / 2, t.accent, dp(1, dpi));

        const int pad = dp(3, dpi) > 2 ? dp(3, dpi) : 2;
        const int knobD = h - pad * 2;
        const float knobR = static_cast<float>(knobD) * 0.5f;
        const float sunCx = static_cast<float>(rect.left + pad) + knobR;
        const float moonCx = static_cast<float>(rect.right - pad) - knobR;
        const float cy = static_cast<float>(rect.top) + static_cast<float>(h) * 0.5f;
        const float activeCx = t.dark ? moonCx : sunCx;

        const COLORREF knobFill = mix(t.cardBg, t.accent, t.dark ? 34 : 18);
        const COLORREF knobBorder = mix(t.cardBorder, t.accent, t.dark ? 58 : 48);
        const COLORREF sunCol = t.dark ? t.muted : RGB(196, 120, 0);
        const COLORREF moonCol = t.dark ? RGB(230, 236, 248) : t.muted;
        const float scale = static_cast<float>(dpi) / 96.0f;

        blendTile(dc, rect.left, rect.top, w, h, [=](float px, float py) -> uint32_t {
            // Active circular thumb.
            const float kd = std::hypot(px - activeCx, py - cy) - knobR;
            const float kOuter = clampf(0.5f - kd, 0.0f, 1.0f);
            float outR = 0.0f, outG = 0.0f, outB = 0.0f, outA = 0.0f;
            if (kOuter > 0.0f) {
                const float kInner = clampf(0.5f - (kd + 1.0f), 0.0f, 1.0f);
                outR = static_cast<float>(GetRValue(knobBorder)) +
                       (static_cast<float>(GetRValue(knobFill)) - static_cast<float>(GetRValue(knobBorder))) * kInner;
                outG = static_cast<float>(GetGValue(knobBorder)) +
                       (static_cast<float>(GetGValue(knobFill)) - static_cast<float>(GetGValue(knobBorder))) * kInner;
                outB = static_cast<float>(GetBValue(knobBorder)) +
                       (static_cast<float>(GetBValue(knobFill)) - static_cast<float>(GetBValue(knobBorder))) * kInner;
                outA = kOuter;
            }

            // Sun icon on the left: centre disc + 8 short radial rays.
            float sunSd = std::hypot(px - sunCx, py - cy) - 2.35f * scale;
            const float rIn = 3.75f * scale;
            const float rOut = 5.05f * scale;
            const float rayHalf = 0.58f * scale;
            constexpr float kCos45 = 0.70710678f;
            const float dirs[8][2] = {
                {1.0f, 0.0f},   {-1.0f, 0.0f},  {0.0f, 1.0f},    {0.0f, -1.0f},
                {kCos45, kCos45}, {-kCos45, kCos45}, {kCos45, -kCos45}, {-kCos45, -kCos45},
            };
            for (int i = 0; i < 8; ++i) {
                const float d = sdSegment(px, py, sunCx + dirs[i][0] * rIn, cy + dirs[i][1] * rIn,
                                          sunCx + dirs[i][0] * rOut, cy + dirs[i][1] * rOut) -
                                rayHalf;
                if (d < sunSd) sunSd = d;
            }
            const float sunCov = clampf(0.5f - sunSd, 0.0f, 1.0f);

            // Crescent moon icon on the right: outer disc minus shifted cutout disc.
            const float mOuter = std::hypot(px - moonCx, py - cy) - 4.35f * scale;
            const float mCut = std::hypot(px - (moonCx + 2.05f * scale), py - (cy - 1.55f * scale)) - 3.65f * scale;
            const float moonSd = mOuter > -mCut ? mOuter : -mCut;
            const float moonCov = clampf(0.5f - moonSd, 0.0f, 1.0f);

            auto over = [&](COLORREF col, float cov) {
                if (cov <= 0.0f) return;
                const float r = static_cast<float>(GetRValue(col));
                const float g = static_cast<float>(GetGValue(col));
                const float b = static_cast<float>(GetBValue(col));
                const float newA = cov + outA * (1.0f - cov);
                if (newA > 0.0f) {
                    outR = (r * cov + outR * outA * (1.0f - cov)) / newA;
                    outG = (g * cov + outG * outA * (1.0f - cov)) / newA;
                    outB = (b * cov + outB * outA * (1.0f - cov)) / newA;
                    outA = newA;
                }
            };
            over(sunCol, sunCov);
            over(moonCol, moonCov);
            return outA > 0.0f ? packPremul(outR, outG, outB, outA) : 0u;
        });
        return;
    }

    switch (owned->style) {
        case ButtonStyle::Primary:
            face = owned->hot ? t.accentHover : t.accent;
            border = face;
            ink = t.accentText;
            break;
        case ButtonStyle::Secondary:
            face = owned->hot ? t.controlHover : t.control;
            break;
        case ButtonStyle::TabActive:
            // Top-level tabs use a clean underline only (no card box behind the label).
            face = t.windowBg;
            border = t.windowBg;
            ink = t.text;
            break;
        case ButtonStyle::Tab:
            face = t.windowBg;
            border = t.windowBg;
            ink = owned->hot ? t.text : t.subtext;
            break;
        case ButtonStyle::SegmentActive:
            // Segmented controls inside cards use a button highlight only (no underline).
            face = mix(t.cardBg, t.accent, t.dark ? 22 : 12);
            border = mix(t.cardBorder, t.accent, 55);
            ink = t.text;
            break;
        case ButtonStyle::Segment:
            face = owned->hot ? t.control : t.cardBg;
            border = t.cardBorder;
            ink = owned->hot ? t.text : t.subtext;
            break;
        case ButtonStyle::Link:
            face = t.windowBg;
            border = face;
            ink = owned->hot ? t.text : t.accent;
            break;
        case ButtonStyle::ThemeToggle:
            break;
    }
    if (disabled) {
        face = mix(face, t.windowBg, 55);
        border = mix(border, t.windowBg, 55);
        ink = t.muted;
    }
    if (owned->pressed && !disabled && owned->style != ButtonStyle::Tab &&
        owned->style != ButtonStyle::TabActive) {
        face = mix(face, RGB(0, 0, 0), t.dark ? 0 : 8);
    }

    if (owned->style == ButtonStyle::TabActive) {
        if (!disabled) {
            const int barH = dp(2, dpi) > 2 ? dp(2, dpi) : 2;
            const int padX = dp(8, dpi);
            if (rect.right - rect.left > padX * 2 + 6) {
                RECT bar{rect.left + padX, rect.bottom - barH, rect.right - padX, rect.bottom};
                fillRounded(dc, bar, barH / 2 + 1, t.accent);
            }
        }
    } else if (owned->style != ButtonStyle::Link && owned->style != ButtonStyle::Tab) {
        fillAndStrokeRounded(dc, rect, radius, face, border, true, border != face);
    }
    if (showFocus) strokeRounded(dc, rect, radius, t.accent, dp(2, dpi));

    RECT textRect = rect;
    if (owned->style == ButtonStyle::Link) {
        // A link reads as part of the line it sits in: the caption font that line is painted in, from the box's top-left, so its
        // baseline and size match the words before it (gui.cpp sizes the box to the measured label).
        drawText(dc, label, textRect, fonts().caption, ink, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    const bool boldTab = owned->style == ButtonStyle::TabActive || owned->style == ButtonStyle::SegmentActive;
    drawText(dc, label, textRect, boldTab ? fonts().bodyBold : fonts().body,
             ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void paintCheckbox(HDC dc, RECT box, bool checked, bool disabled, bool hot, UINT dpi) {
    const Theme& t = theme();
    const int side = box.right - box.left;
    const int height = box.bottom - box.top;
    if (side <= 2 || height <= 2) return;

    const COLORREF face = checked ? (disabled ? mix(t.accent, t.cardBg, 55) : t.accent)
                                  : (disabled ? t.cardBg : (hot ? t.controlHover : t.cardBg));
    const COLORREF border = checked ? face : (disabled ? t.cardBorder : t.controlBorder);
    const float halfSide = static_cast<float>(side) * 0.5f;
    const float cx = static_cast<float>(box.left) + halfSide;
    const float cy = static_cast<float>(box.top) + static_cast<float>(height) * 0.5f;
    const float boxR = clampf(static_cast<float>(dp(4, dpi)), 0.0f, halfSide);
    const float s = static_cast<float>(side);
    const float bx = static_cast<float>(box.left);
    const float by = static_cast<float>(box.top);
    const float p0x = bx + s * 0.25f, p0y = by + s * 0.52f;
    const float p1x = bx + s * 0.43f, p1y = by + s * 0.70f;
    const float p2x = bx + s * 0.76f, p2y = by + s * 0.31f;
    const float tickHalfW = static_cast<float>(dp(2, dpi)) * 0.48f;

    const float fr = static_cast<float>(GetRValue(face));
    const float fg = static_cast<float>(GetGValue(face));
    const float fb = static_cast<float>(GetBValue(face));
    const float sr = static_cast<float>(GetRValue(border));
    const float sg = static_cast<float>(GetGValue(border));
    const float sb = static_cast<float>(GetBValue(border));
    const float tr = static_cast<float>(GetRValue(t.accentText));
    const float tg = static_cast<float>(GetGValue(t.accentText));
    const float tb = static_cast<float>(GetBValue(t.accentText));

    blendTile(dc, box.left, box.top, side, height, [=](float px, float py) -> uint32_t {
        const float sd = sdRoundedBox(px, py, cx, cy, halfSide, static_cast<float>(height) * 0.5f, boxR);
        const float outerCov = clampf(0.5f - sd, 0.0f, 1.0f);
        if (outerCov <= 0.0f) return 0u;
        float rr = fr;
        float gg = fg;
        float bb = fb;
        if (!checked) {
            const float innerCov = clampf(0.5f - (sd + 1.0f), 0.0f, 1.0f);
            rr = sr + (fr - sr) * innerCov;
            gg = sg + (fg - sg) * innerCov;
            bb = sb + (fb - sb) * innerCov;
        } else {
            // A tick, drawn analytically rather than typed: Segoe MDL2 is not on
            // every machine and a font fallback would put a box glyph here.
            const float d1 = sdSegment(px, py, p0x, p0y, p1x, p1y);
            const float d2 = sdSegment(px, py, p1x, p1y, p2x, p2y);
            const float tickSd = (d1 < d2 ? d1 : d2) - tickHalfW;
            const float tickCov = clampf(0.5f - tickSd, 0.0f, 1.0f);
            if (tickCov > 0.0f) {
                rr = rr + (tr - rr) * tickCov;
                gg = gg + (tg - gg) * tickCov;
                bb = bb + (tb - bb) * tickCov;
            }
        }
        return packPremul(rr, gg, bb, outerCov);
    });
}

void drawCheckbox(const DRAWITEMSTRUCT* item, UINT dpi) {
    Owned* owned = find(item->hwndItem);
    if (!owned) return;
    const Theme& t = theme();
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    HDC dc = item->hDC;

    wchar_t label[512]{};
    GetWindowTextW(item->hwndItem, label, 512);

    fillRect(dc, item->rcItem, t.cardBg);

    const int side = dp(18, dpi);
    RECT box{item->rcItem.left, 0, 0, 0};
    box.top = item->rcItem.top + ((item->rcItem.bottom - item->rcItem.top) - side) / 2;
    box.right = box.left + side;
    box.bottom = box.top + side;

    paintCheckbox(dc, box, owned->checked, disabled, owned->hot, dpi);

    RECT textRect = item->rcItem;
    textRect.left = box.right + dp(10, dpi);
    drawText(dc, label, textRect, fonts().body, disabled ? t.muted : t.text,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (g_keyboardFocus && (item->itemState & ODS_FOCUS) != 0 &&
        (item->itemState & ODS_NOFOCUSRECT) == 0 && !disabled) {
        RECT focus = item->rcItem;
        strokeRounded(dc, focus, dp(4, dpi), t.accent);
    }
}

bool drawOwnerDrawn(const DRAWITEMSTRUCT* item, UINT dpi) {
    const Owned* owned = find(item->hwndItem);
    if (!owned) return false;
    if (owned->isCheckbox)
        drawCheckbox(item, dpi);
    else
        drawButton(item, dpi);
    return true;
}

RECT slimThumb(const RECT& pane, int contentPx, int pagePx, int posPx, UINT dpi) {
    RECT none{0, 0, 0, 0};
    if (pagePx <= 0 || contentPx <= pagePx) return none;
    const int trackTop = pane.top + dp(3, dpi);
    const int trackBottom = pane.bottom - dp(3, dpi);
    const int track = trackBottom - trackTop;
    if (track <= dp(24, dpi)) return none;

    int thumb = static_cast<int>(static_cast<long long>(track) * pagePx / contentPx);
    const int minThumb = dp(24, dpi);
    if (thumb < minThumb) thumb = minThumb;
    if (thumb > track) thumb = track;

    const int range = contentPx - pagePx;
    int pos = posPx < 0 ? 0 : (posPx > range ? range : posPx);
    const int offset =
        range > 0 ? static_cast<int>(static_cast<long long>(track - thumb) * pos / range) : 0;

    RECT r;
    r.right = pane.right - dp(3, dpi);
    r.left = r.right - dp(5, dpi);
    r.top = trackTop + offset;
    r.bottom = r.top + thumb;
    return r;
}

int slimPosFromThumbTop(const RECT& pane, int contentPx, int pagePx, int thumbTopY, UINT dpi) {
    const RECT ref = slimThumb(pane, contentPx, pagePx, 0, dpi);
    if (ref.right <= ref.left) return 0;
    const int trackTop = pane.top + dp(3, dpi);
    const int track = (pane.bottom - dp(3, dpi)) - trackTop;
    const int thumb = ref.bottom - ref.top;
    const int slack = track - thumb;
    if (slack <= 0) return 0;
    int offset = thumbTopY - trackTop;
    if (offset < 0) offset = 0;
    if (offset > slack) offset = slack;
    return static_cast<int>(static_cast<long long>(contentPx - pagePx) * offset / slack);
}

void drawSlimScrollbar(HDC dc, const RECT& pane, int contentPx, int pagePx, int posPx, UINT dpi,
                       bool active) {
    const RECT thumb = slimThumb(pane, contentPx, pagePx, posPx, dpi);
    if (thumb.right <= thumb.left) return;
    const Theme& t = theme();
    fillRounded(dc, thumb, dp(2, dpi), active ? t.subtext : t.controlBorder);
}

void drawComboItem(const DRAWITEMSTRUCT* item, UINT dpi) {
    const Theme& t = theme();
    const bool inField = (item->itemState & ODS_COMBOBOXEDIT) != 0;
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    const COLORREF bg = inField ? t.control : (selected ? t.controlHover : t.cardBg);
    fillRect(item->hDC, item->rcItem, bg);
    if (item->itemID == static_cast<UINT>(-1)) return;
    wchar_t text[512]{};
    SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
    RECT rc = item->rcItem;
    rc.left += dp(10, dpi);
    rc.right -= dp(6, dpi);
    drawText(item->hDC, text, rc, fonts().body, t.text,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

int textHeightPx(HFONT font) {
    HDC dc = GetDC(nullptr);
    HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previous);
    ReleaseDC(nullptr, dc);
    return metrics.tmHeight > 0 ? metrics.tmHeight : 16;
}

}  // namespace edvr::installer::ui
