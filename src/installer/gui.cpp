// The window.
//
// One screen per job, no wizard: which install, which halves, and a pane that
// says exactly what is about to happen -- because the thing being modified is
// somebody's game folder, and an installer that does its work behind a progress
// bar is asking for trust it has not earned. Every button shows the plan and
// waits for a yes before a file moves.
//
// The look is drawn rather than inherited; see ui.h for why and how. What is
// still native here is deliberate: the install picker is a real combo box and
// the report is a real edit control, because a dropdown and a scrolling text
// view are the two things a hand-rolled widget gets subtly wrong -- keyboard
// selection, mouse wheel, text selection for copying into a bug report.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>

#include <string>
#include <vector>

#include "app.h"
#include "apply.h"
#include "edhm_d3dx.h"
#include "logbundle.h"
#include "mirror.h"
#include "payload.h"
#include "product_title.h"
#include "settings.h"
#include "settings_view.h"
#include "launcher_l10n.h"
#include "ui.h"
#ifdef EDUP_LAUNCHER
#include "game_language.h"
#include "launch_route.h"
#include "launcher_login_dialog.h"
#include "launcher_main.h"
#include "launcher_session.h"
#include "layout_dpi.h"
#endif

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace edvr::installer {
namespace {

enum : int {
    kIdCombo = 1001,
    kIdBrowse = 1002,
    kIdChkKeep = 1012,
    kIdChkPurge = 1013,
    kIdInstall = 1020,
    kIdRepair = 1021,
    kIdUninstall = 1022,
    kIdCollectLogs = 1024,
    kIdCopyPath = 1025,
    kIdShowFolder = 1026,
    kIdReport = 1030,
    kIdKofi = 1040,
    kIdDiscord = 1041,
#ifdef EDUP_LAUNCHER
    kIdTabPlay = 1049,
#endif
    kIdTabInstall = 1050,
    kIdTabSettings = 1051,
    kIdSearch = 1060,
    kIdSettingsList = 1061,
    kIdSettingsVr = 1062,
    kIdSettingsFlat = 1063,
    kIdScope = 1070,
    kIdLanguage = 1071,
};

const UINT kMsgFirstScan = WM_APP + 1;

#ifndef EDUP_LAUNCHER   // the launcher has no tip link
const wchar_t* kTipUrl = L"https://ko-fi.com/seancharacterecho";
const wchar_t* kTipText = L"ED-UP is free. If it makes your flight more enjoyable, you can";
#endif
const wchar_t* kDiscordUrl = L"https://discord.gg/AWJ6djKKzr";
constexpr int kTabY = 140;
constexpr int kTabH = 32;
constexpr int kTabRuleY = kTabY + kTabH;
constexpr int kContentTop = 184;
#ifdef EDUP_LAUNCHER
// Launcher layout (96-dpi units): three workflow screens (Play, Setup, Settings).
// Play holds Launch hero + optional Update banner + auto-sized Status card.
// Setup holds Updates -> Install & repair (with scope selector) -> tall Log.
constexpr int kContentBottom = 684;
constexpr int kUpdatesTop = 184;
constexpr int kUpdatesBottom = 256;
constexpr int kActionsTop = 268;
constexpr int kActionsBottom = 434;
constexpr int kScopeY = 276;
constexpr int kActionsChkY = 318;
constexpr int kActionsBtnY = 350;
constexpr int kActionsNoteY = 398;
constexpr int kReportTop = 446;
constexpr int kReportBottom = 684;
constexpr int kReportEditY = 484;
constexpr int kReportEditH = 190;
constexpr int kTipTop = 694;
#else
constexpr int kContentBottom = 650;
constexpr int kStatusTop = 184;
constexpr int kStatusBottom = 372;
constexpr int kActionsTop = 382;
constexpr int kActionsBottom = 522;
constexpr int kScopeY = 390;
constexpr int kActionsChkY = 432;
constexpr int kActionsBtnY = 462;
constexpr int kActionsNoteY = 500;
constexpr int kReportTop = 532;
constexpr int kReportBottom = 650;
constexpr int kReportEditY = 558;
constexpr int kReportEditH = 84;
constexpr int kTipTop = 658;  // the tip line's top: the painted sentence and both links share it
#endif

// The client area and every control position below are in 96-dpi units, scaled
// once at layout time.
const int kClientWidth = 740;
#ifdef EDUP_LAUNCHER
const int kClientHeight = 720;
#else
const int kClientHeight = 688;
#endif
const int kMargin = 24;
const int kCardPad = 20;

// The scope prompt's three choices, in the order the combo lists them. The
// default is the folder's own scope, else both.
const wchar_t* kScopeChoices[] = {L"VR and flat (recommended)", L"VR only", L"Flat only"};
RECT g_scopePromptPx{};
const char* kLanguageCodes[] = {"en", "de", "es", "fr", "pt-BR", "ru"};

#ifdef EDUP_LAUNCHER
enum class Screen { Play, Install, Settings };
#else
enum class Screen { Install, Settings };
#endif

enum class Tone { Good, Warn, Bad, Muted };

struct StatusRow {
    std::wstring label;
    std::wstring value;
    Tone         tone = Tone::Muted;
};

struct Place {
    HWND hwnd = nullptr;
    int  x = 0, y = 0, w = 0, h = 0;
    Screen screen = Screen::Install;
    bool bothScreens = false;
};

struct Gui {
    HWND window = nullptr;
    HWND combo = nullptr;
    HWND scopeCombo = nullptr;
    HWND languageCombo = nullptr;
    HWND report = nullptr;
    HWND chkKeep = nullptr;
    HWND chkPurge = nullptr;
    HWND install = nullptr;
    HWND repair = nullptr;
    HWND uninstall = nullptr;
    HWND collectLogs = nullptr;
    HWND copyPath = nullptr;    // Wine only: shown after a successful Save logs
    HWND showFolder = nullptr;  // Wine only: shown after a successful Save logs
    bool hostButtonsShown = false;
    std::string  savedReport;      // the report Save logs showed, which the two buttons add a line to
    std::string  savedHostPath;    // the zip's path as the host sees it
    std::wstring savedFolder;      // the folder it went into, as Windows sees it
#ifdef EDUP_LAUNCHER
    HWND tabPlay = nullptr;
#endif
    HWND tabInstall = nullptr;
    HWND tabSettings = nullptr;
    HWND settingsVr = nullptr;
    HWND settingsFlat = nullptr;
    HWND tip = nullptr;
    HWND discord = nullptr;
    HWND search = nullptr;
    HWND settingsList = nullptr;
    SettingsModel settings;
    std::string   settingsEdition = "vr";   // the file the settings screen edits: "vr" (edvr.ini) or "flat" (edvr-flat.ini)
    edvr::ModeRead mode = edvr::readGameMode(nullptr);  // Elite's 3D mode, read when the folder is surveyed
    UINT dpi = 96;      // what the client area is laid out at: the real dpi, or less when the window would not fit
    UINT realDpi = 96;  // the window's own, which its frame and title bar are drawn at

    std::vector<Place>      places;
    std::vector<StatusRow>  status;
    std::wstring            statusPath;   // the full folder, under the picker
    std::wstring            modeLine;     // "Elite's 3D mode is ..." under the status rows
    std::wstring            scopeLine;    // "This install does not include ..." when the scope leaves the mode out

    std::vector<GameInstall> installs;
    Survey                   survey;
    bool                     haveSurvey = false;
    Screen                   screen = Screen::Install;
    AppArgs                  args;
    bool                     reportDrag = false;   // slim-scrollbar drag state
    int                      reportDragOffset = 0;
};

Gui g;

int dp(int v) { return ui::dp(v, g.dpi); }

void setText(HWND control, const std::string& utf8) {
    SetWindowTextW(control, fromUtf8(utf8).c_str());
}

// Where this game's settings copy lives. The launcher keeps it under its data root, so a run
// with --data-root leaves the user's real folder alone.
std::wstring mirrorRootOfApp() {
#ifdef EDUP_LAUNCHER
    return launcherMirrorRoot(g.args);
#else
    return userDataRootOf(g.args.userDataRoot);
#endif
}

std::wstring mirrorDirOfGame(const GameInstall& game) {
    return mirrorDirFor(game, mirrorRootOfApp());
}

void loadSettings(const std::string& edition);

// The report keeps no native scrollbar (an EDIT's bar is the one piece of
// stock chrome the drawn look could not restyle); the parent paints the
// same slim thumb the settings list uses, in the report card's right
// gutter, and drives the EDIT by lines when it is dragged.
const RECT kReportCard{kMargin, kReportTop, kClientWidth - kMargin, kReportBottom};

RECT reportCardRect() {
    return RECT{dp(kReportCard.left), dp(kReportCard.top), dp(kReportCard.right),
                dp(kReportCard.bottom)};
}

int reportLineHeight() { return ui::textHeightPx(ui::fonts().body); }

// content/page/pos of the report in device pixels, for the slim thumb.
void reportScrollState(int* content, int* page, int* pos) {
    RECT client{};
    GetClientRect(g.report, &client);
    const int lineHeight = reportLineHeight();
    *content = static_cast<int>(SendMessageW(g.report, EM_GETLINECOUNT, 0, 0)) * lineHeight;
    *page = client.bottom - client.top;
    *pos = static_cast<int>(SendMessageW(g.report, EM_GETFIRSTVISIBLELINE, 0, 0)) * lineHeight;
}

void invalidateReportGutter() {
    const RECT card = reportCardRect();
    const RECT gutter{card.right - dp(12), card.top, card.right, card.bottom};
    InvalidateRect(g.window, &gutter, FALSE);
}

// The EDIT scrolls itself on wheel, keys and selection drags; the thumb is
// painted by the parent and has to follow.
LRESULT CALLBACK reportProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                            UINT_PTR /*id*/, DWORD_PTR /*data*/) {
    // A multiline EDIT scrolls on the wheel only when it has WS_VSCROLL, and
    // this one deliberately has no scroll bar -- the card draws its own slim
    // gutter instead. So the control ignores the tick however it is
    // delivered, and routing it correctly (which the pump now does) was
    // necessary but not sufficient: two separate faults, one symptom, and
    // the first fix looked like it had simply failed.
    //
    // So scroll it here. EM_LINESCROLL is what the EDIT would have done for
    // itself with the style it does not have.
    if (message == WM_MOUSEWHEEL) {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == 0) lines = 3;          // "do not scroll" -- still scroll
        if (lines > 24) lines = 24;         // WHEEL_PAGESCROLL is 0xFFFFFFFF
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        const int step = -delta * static_cast<int>(lines) / WHEEL_DELTA;
        if (step != 0) SendMessageW(hwnd, EM_LINESCROLL, 0, step);
        invalidateReportGutter();
        return 0;
    }
    const LRESULT out = DefSubclassProc(hwnd, message, wparam, lparam);
    switch (message) {
        case WM_KEYDOWN:
        case WM_MOUSEMOVE:
        case WM_TIMER:   // the EDIT's own drag-select autoscroll
            invalidateReportGutter();
            break;
        default:
            break;
    }
    return out;
}

void setReport(const std::string& utf8) {
    setText(g.report, utf8);
    // Scrolled to the END: outcomes are appended below the plan, and the
    // pane is the only place they are said now -- the freshest line must
    // be the visible one.
    const LRESULT len = SendMessageW(g.report, WM_GETTEXTLENGTH, 0, 0);
    SendMessageW(g.report, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    SendMessageW(g.report, EM_SCROLLCARET, 0, 0);
    invalidateReportGutter();
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------

void place(HWND hwnd, int x, int y, int w, int h, Screen screen, bool bothScreens = false) {
    Place p;
    p.hwnd = hwnd;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    p.screen = screen;
    p.bothScreens = bothScreens;
    g.places.push_back(p);
}

// The scope the prompt names: both, vr or flat. The combo is the only record of it.
std::string scopeChoice() {
    if (!g.scopeCombo) return "both";
    switch (SendMessageW(g.scopeCombo, CB_GETCURSEL, 0, 0)) {
        case 1: return "vr";
        case 2: return "flat";
        default: return "both";
    }
}

int scopeIndex(const std::string& scope) {
    return scope == "vr" ? 1 : scope == "flat" ? 2 : 0;
}

// The settings file the screen edits for a scope: the one scope that installs only one of
// them, else the file the game's 3D mode runs on (flat for modes 0 and 1, VR otherwise and when
// the mode is unknown).
std::string editionFor(const std::string& scope) {
    if (scope == "vr") return "vr";
    if (scope == "flat") return "flat";
    return g.mode.mode == edvr::GameMode::Flat ? "flat" : "vr";
}

// The VR/Flat selector offers only the files the scope installs, and shows the one in use.
void syncSettingsSelector() {
    if (!g.settingsVr || !g.settingsFlat) return;
    const std::string scope = scopeChoice();
    ui::setButtonStyle(g.settingsVr, g.settingsEdition == "vr" ? ui::ButtonStyle::SegmentActive : ui::ButtonStyle::Segment);
    ui::setButtonStyle(g.settingsFlat, g.settingsEdition == "flat" ? ui::ButtonStyle::SegmentActive : ui::ButtonStyle::Segment);
    const bool settingsShown = g.screen == Screen::Settings;
    ShowWindow(g.settingsVr, settingsShown && scope != "flat" ? SW_SHOW : SW_HIDE);
    ShowWindow(g.settingsFlat, settingsShown && scope != "vr" ? SW_SHOW : SW_HIDE);
}

void rebuildStatus();
void applyLayout();
void showScreen(Screen screen);
#ifdef EDUP_LAUNCHER
#include "launcher_gui.inc"
#endif

// Everything moved and shown or hidden for the current screen and scale.
//
// The first version of this took the window's size from GetDpiForSystem and the
// control positions from GetDpiForWindow. On a 150% display those are different
// numbers, so the controls were laid out half as big again as the window meant
// to hold them, and the right-hand third was off the edge.
void applyLayout() {
    for (const Place& p : g.places) {
        const bool visible = p.bothScreens || p.screen == g.screen;
        ShowWindow(p.hwnd, visible ? SW_SHOW : SW_HIDE);
        if (visible) MoveWindow(p.hwnd, dp(p.x), dp(p.y), dp(p.w), dp(p.h), TRUE);
    }

    // Navigation tabs are measured to their localized labels so longer translations
    // (such as German "Einstellungen") never clip.
    {
        HDC dc = GetDC(g.window);
        const HFONT font = ui::fonts().bodyBold;
        int tx = dp(kMargin);
        const int ty = dp(kTabY);
        const int th = dp(kTabH);
        const int gap = dp(6);
#ifdef EDUP_LAUNCHER
        HWND tabs[] = {g.tabPlay, g.tabInstall, g.tabSettings};
#else
        HWND tabs[] = {g.tabInstall, g.tabSettings};
#endif
        for (HWND tab : tabs) {
            if (!tab) continue;
            wchar_t label[128]{};
            GetWindowTextW(tab, label, 128);
            int w = ui::textWidth(dc, label, font) + dp(32);
            if (w < dp(84)) w = dp(84);
            MoveWindow(tab, tx, ty, w, th, TRUE);
            tx += w + gap;
        }
        ReleaseDC(g.window, dc);
    }

    // The four action buttons (plus Wine's two log-folder buttons) are sized to their labels.
    // A translation of "Save logs" is wider than the English word, and a fixed width clips it.
    if (g.install && g.screen == Screen::Install) {
        HWND buttons[] = {g.install, g.repair, g.uninstall, g.collectLogs};
        HDC dc = GetDC(g.window);
        const HFONT font = ui::fonts().body;
        int x = dp(kMargin + kCardPad);
        const int y = dp(kActionsBtnY);
        const int h = dp(34);
        const int gap = dp(8);
        const int limit = dp(kClientWidth - kMargin - kCardPad);
        for (HWND button : buttons) {
            if (!button) continue;
            wchar_t label[256]{};
            GetWindowTextW(button, label, 256);
            int width = ui::textWidth(dc, label, font) + dp(28);
            if (width < dp(92)) width = dp(92);
            if (x + width > limit) width = limit - x;
            if (width < dp(48)) width = dp(48);
            MoveWindow(button, x, y, width, h, TRUE);
            x += width + gap;
        }
        // The two Wine buttons sit right after Save logs when shown, sized to fit cleanly inside the card.
        for (HWND hostButton : {g.copyPath, g.showFolder}) {
            if (!hostButton) continue;
            const bool showHost = g.hostButtonsShown && g.screen == Screen::Install;
            ShowWindow(hostButton, showHost ? SW_SHOW : SW_HIDE);
            if (!showHost) continue;
            wchar_t label[128]{};
            GetWindowTextW(hostButton, label, 128);
            int width = ui::textWidth(dc, label, font) + dp(20);
            if (width < dp(68)) width = dp(68);
            if (x + width > limit) width = limit - x;
            if (width >= dp(40)) {
                MoveWindow(hostButton, x, y, width, h, TRUE);
                x += width + gap;
            }
        }
        ReleaseDC(g.window, dc);
    } else {
        for (HWND hostButton : {g.copyPath, g.showFolder}) {
            if (hostButton) ShowWindow(hostButton, SW_HIDE);
        }
    }

    // The scope prompt sits right-aligned inside the Install & repair card header row,
    // right beside the actions it controls.
    if (g.scopeCombo && g.screen == Screen::Install) {
        HDC dc = GetDC(g.window);
        const wchar_t* prompt = tr(L"Install ED-UP for:");
        const int textW = ui::textWidth(dc, prompt, ui::fonts().body);
        ReleaseDC(g.window, dc);
        const int right = dp(kClientWidth - kMargin - kCardPad);
        const int comboW = dp(218);
        const int comboLeft = right - comboW;
        int labelLeft = comboLeft - dp(10) - textW;
        const int minLabelLeft = dp(kMargin + kCardPad + 160);
        if (labelLeft < minLabelLeft) labelLeft = minLabelLeft;
        g_scopePromptPx = RECT{labelLeft, dp(kScopeY), comboLeft - dp(8), dp(kScopeY + 30)};
        MoveWindow(g.scopeCombo, comboLeft, dp(kScopeY), comboW, dp(30), TRUE);
    }

    // The tip link finishes a sentence that is painted, not a control, so where
    // it goes depends on how wide that sentence renders -- which changes with
    // the font, the scale and the language of the machine it is running on.
    // Both links are sized to their measured labels and sit on the sentence's own line (its rect's top, kTipTop), so the
    // link continues the sentence after one ordinary space, and "Ask on Discord" ends at the right margin.
    if (g.tip || g.discord) {
        HDC dc = GetDC(g.window);
        const HFONT caption = ui::fonts().caption;
        wchar_t discordLabel[128] = L"Ask on Discord";
        if (g.discord) GetWindowTextW(g.discord, discordLabel, 128);
        const int discordWidth = ui::textWidth(dc, discordLabel, caption) + dp(2);
#ifndef EDUP_LAUNCHER   // the launcher has no tip link, so there is no sentence to measure
        const int prefix = ui::textWidth(dc, kTipText, caption);
        const int space = ui::textWidth(dc, L" ", caption);
        const int tipWidth = ui::textWidth(dc, L"leave a tip", caption) + dp(2);
        if (g.tip) MoveWindow(g.tip, dp(kMargin) + prefix + space, dp(kTipTop), tipWidth, dp(20), TRUE);
#endif
        ReleaseDC(g.window, dc);
        if (g.discord)
            MoveWindow(g.discord, dp(kClientWidth - kMargin) - discordWidth, dp(kTipTop), discordWidth, dp(20), TRUE);
    }

    // A combo's field height is its item height plus its own frame, not
    // what MoveWindow asked for. The frame is measured and the item height
    // set to land the field at exactly the 30 units the Browse button
    // beside it gets; the text centres itself within the item.
    // The scope prompt's combo is sized the same way.
#ifdef EDUP_LAUNCHER
    if (g.languageCombo)
        MoveWindow(g.languageCombo, dp(kClientWidth - kMargin - 168), dp(16), dp(168), dp(30), TRUE);
#endif
    for (HWND combo : {g.combo, g.scopeCombo, g.languageCombo}) {
        if (!combo) continue;
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, dp(24));  // dropdown rows
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), dp(22));
        RECT rc{};
        GetWindowRect(combo, &rc);
        const int frame = (rc.bottom - rc.top) - dp(22);
        const int fieldItem = dp(30) - frame;
        if (fieldItem > 0) {
            SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), fieldItem);
        }
    }

    // Single-line EDITs draw their text at the top of the control, so the
    // search box is sized to its text and centred within the frame painted
    // around it (230..262 in design units) instead of filling the frame with
    // the text riding high -- the same treatment the settings list gives its
    // inline value editor.
    if (g.search) {
        const int textH = ui::textHeightPx(ui::fonts().body);
        const int frameTop = dp(230);
        const int frameH = dp(262) - frameTop;
        const int y = frameTop + (frameH > textH ? (frameH - textH) / 2 : 0);
        MoveWindow(g.search, dp(kMargin + kCardPad), y, dp(320), textH, TRUE);
    }

    // Shown or hidden by the scope, not by the screen alone, so it runs after the loop above.
    syncSettingsSelector();
#ifdef EDUP_LAUNCHER
    layoutLauncherControls();
#endif
    InvalidateRect(g.window, nullptr, TRUE);
}

// The dpi to lay the client area out at. The installer's window always fits, and takes the window's dpi.
// The launcher's is taller: where it would not fit the monitor's work area (150% on a 1080p screen, or
// 100% on the Steam Deck's 800 pixels) it is laid out smaller, so that Play is on the screen. `onMonitor`
// names the monitor when the window has not moved there yet (WM_DPICHANGED); null means the window's own.
UINT fitLayoutDpi(HWND window, UINT realDpi, const RECT* onMonitor) {
#ifdef EDUP_LAUNCHER
    const HMONITOR monitor = onMonitor ? MonitorFromRect(onMonitor, MONITOR_DEFAULTTONEAREST)
                                       : MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return realDpi;
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE, 0, realDpi);
    return edvr::installer::layoutDpiFor(realDpi, info.rcWork.bottom - info.rcWork.top, kClientHeight, frame.bottom - frame.top);
#else
    (void)window;
    (void)onMonitor;
    return realDpi;
#endif
}

void sizeWindowToLayout() {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(g.window, GWL_STYLE));
    RECT wanted{0, 0, dp(kClientWidth), dp(kClientHeight)};
    // The frame and title bar are the system's size, whatever the client area was laid out at.
    AdjustWindowRectExForDpi(&wanted, style, FALSE, 0, g.realDpi);
    SetWindowPos(g.window, nullptr, 0, 0, wanted.right - wanted.left, wanted.bottom - wanted.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#ifdef EDUP_LAUNCHER
    // A window that opened low on the screen is moved up until its bottom edge, and Play, is in view.
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT now{};
    if (GetWindowRect(g.window, &now) &&
        GetMonitorInfoW(MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST), &info) && now.bottom > info.rcWork.bottom) {
        const int top = now.top - (now.bottom - info.rcWork.bottom);
        SetWindowPos(g.window, nullptr, now.left, top < info.rcWork.top ? info.rcWork.top : top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
#endif
}

// ---------------------------------------------------------------------------
// what the folder looks like, as rows rather than as a paragraph
// ---------------------------------------------------------------------------

Tone toneForOurs(DllKind kind) {
    switch (kind) {
        case DllKind::Edvr: return Tone::Good;
        case DllKind::Absent: return Tone::Muted;
        case DllKind::Unreadable: return Tone::Bad;
        default: return Tone::Warn;
    }
}

void rebuildStatus() {
    g.status.clear();
    g.modeLine.clear();
    g.scopeLine.clear();
    if (!g.haveSurvey) {
        g.statusPath.clear();
        return;
    }
    const Survey& s = g.survey;
    g.statusPath = s.game.dir;

    StatusRow fixes;
    fixes.label = tr(L"The fixes");
    fixes.tone = toneForOurs(s.d3d11.kind);
    // A record older than the files says nothing about what is installed, its version included.
    const bool stale = installRecordStale(s);
    if (s.d3d11.kind == DllKind::Edvr) {
        fixes.value = tr(L"ED-UP installed as d3d11.dll");
        if (!s.state.edvrVersion.empty() && !stale) fixes.value += L" \x00b7 " + fromUtf8(s.state.edvrVersion);
    } else if (s.d3d11.kind == DllKind::Absent) {
        fixes.value = tr(L"not installed");
    } else {
        const bool edupPresent = s.state.present || s.openvrCurrent.kind == DllKind::Edvr;
        fixes.value = describeDll(s.d3d11) + L" \x2014 " +
                      tr(edupPresent ? L"holds d3d11.dll; Repair chains both"
                                     : L"holds d3d11.dll; Install chains both");
    }
    g.status.push_back(fixes);

    StatusRow vr;
    vr.label = tr(L"Flash fix, native on foot");
    if (!s.haveOpenvrDir) {
        vr.tone = Tone::Warn;
        vr.value = tr(L"no Openvr folder found under this install");
    } else if (s.openvrCurrent.kind == DllKind::Edvr) {
        vr.tone = Tone::Good;
        vr.value = tr(L"ED-UP installed as openvr_api.dll");
    } else if (s.openvrCurrent.kind == DllKind::OpenVrRuntime) {
        vr.tone = Tone::Muted;
        vr.value = tr(L"not installed \x00b7 the game's own runtime is in place");
    } else {
        vr.tone = toneForOurs(s.openvrCurrent.kind);
        vr.value = describeDll(s.openvrCurrent);
    }
    g.status.push_back(vr);

    if (stale) {
        StatusRow record;
        record.label = tr(L"Install record");
        record.tone = Tone::Warn;
        record.value = tr(L"older than the files here; Update refreshes it");
        g.status.push_back(record);
    }

    // The game's runtime depends on the scope: flat expects the game's own openvr_api.dll, and both and vr
    // run EDVR's own runtime as openvr_api.dll.
    const std::string scope = scopeChoice();
    if (s.haveOpenvrDir) {
        StatusRow orig;
        orig.label = tr(L"The game's runtime");
        if (scope == "flat") {
            if (s.openvrCurrent.kind == DllKind::OpenVrRuntime) {
                orig.tone = Tone::Good;
                orig.value = tr(L"the game's own runtime is in place");
            } else if (s.openvrOrig.kind == DllKind::OpenVrRuntime) {
                orig.tone = Tone::Good;
                orig.value = tr(L"safe, renamed openvr_api_orig.dll");
            } else if (s.openvrCurrent.kind == DllKind::Edvr) {
                orig.tone = Tone::Warn;
                orig.value = tr(L"ED-UP's runtime is in place; flat restores the game's own");
            } else {
                orig.tone = Tone::Muted;
                orig.value = tr(L"not found");
            }
        } else {
            if (s.openvrCurrent.kind == DllKind::Edvr) {
                orig.tone = Tone::Good;
                orig.value = tr(L"ED-UP's own runtime is in place");
            } else if (s.openvrCurrent.kind == DllKind::OpenVrRuntime) {
                orig.tone = Tone::Muted;
                orig.value = tr(L"the game's own runtime; the install replaces it");
            } else if (s.openvrOrig.kind == DllKind::OpenVrRuntime) {
                orig.tone = Tone::Good;
                orig.value = tr(L"safe, renamed openvr_api_orig.dll");
            } else {
                orig.tone = Tone::Muted;
                orig.value = tr(L"not found");
            }
        }
        g.status.push_back(orig);
    }

    // One row for the scope's settings files, each with whether it is here and kept. The flat file is read
    // from edup-flat.ini and falls back to the shared edup.ini until it has its own.
    StatusRow settings;
    settings.label = tr(L"Your settings");
    std::wstring parts;
    bool allHere = true;
    if (scope != "flat") {
        parts += s.iniPresent ? tr(L"edup.ini kept") : tr(L"edup.ini not yet");
        allHere = allHere && s.iniPresent;
    }
    if (scope == "both") parts += L"; ";
    if (scope != "vr") {
        parts += s.flatIniPresent ? tr(L"edup-flat.ini kept")
                                  : (s.iniPresent ? tr(L"edup-flat.ini not yet (edup.ini is copied into it)")
                                                  : tr(L"edup-flat.ini not yet"));
        allHere = allHere && s.flatIniPresent;
    }
    settings.value = parts;
    settings.tone = allHere ? Tone::Good : Tone::Muted;
    g.status.push_back(settings);

    if (!s.state.chainTarget.empty()) {
        StatusRow chain;
        chain.label = tr(L"Alongside");
        chain.tone = Tone::Good;
        chain.value = (s.state.chainMod.empty() ? std::wstring(tr(L"another mod"))
                                                : s.state.chainMod) +
                      tr(L", chained through ") + s.state.chainTarget;
        g.status.push_back(chain);
    }

    if (s.gameRunningHere) {
        StatusRow running;
        running.label = tr(L"Elite Dangerous");
        running.tone = Tone::Bad;
        running.value = tr(L"running \x2014 close it before installing anything");
        g.status.push_back(running);
    } else if (s.gameRunningElsewhere) {
        // Not a warning: the copy running belongs to another install and this
        // one is free. Said anyway, because somebody who can see the game on
        // their other monitor is owed the reason the buttons are still live.
        StatusRow running;
        running.label = tr(L"Elite Dangerous");
        running.tone = Tone::Muted;
        running.value = tr(L"running from a different folder \x2014 not this install");
        g.status.push_back(running);
    } else if (s.gameRunStateUnknown) {
        // The buttons stay live on purpose: a click re-surveys, which may well
        // succeed the second time, and when it does not the plan says why it
        // refuses rather than the buttons being greyed with no reason given.
        StatusRow running;
        running.label = tr(L"Elite Dangerous");
        running.tone = Tone::Warn;
        running.value = tr(L"could not check whether it is running \x2014 nothing will be installed");
        g.status.push_back(running);
    }

    // Elite's 3D mode decides which half of EDVR runs. Use the folder's installed scope
    // when ED-UP is already installed so changing the dropdown does not misstate active behaviour.
    const std::string activeScope = installedScope(s);
    const std::string modeScope = activeScope.empty() ? scope : activeScope;
    if (!g.mode.known) {
        g.modeLine = modeScope == "flat" ? tr(L"Elite's 3D mode is unset: flat install runs its flat fixes.")
                                         : tr(L"Elite's 3D mode is unknown: ED-UP starts in VR.");
    } else if (g.mode.mode == edvr::GameMode::Flat) {
        g.modeLine = tr(L"Elite's 3D mode is off: ED-UP runs its flat fixes.");
    } else {
        g.modeLine = tr(L"Elite's 3D mode is on: ED-UP runs its VR fixes.");
    }
    if (g.mode.known && g.mode.mode == edvr::GameMode::Flat && modeScope == "vr") {
        g.scopeLine = tr(L"This install does not include flat: ED-UP stays out of the way.");
    } else if (g.mode.known && g.mode.mode == edvr::GameMode::Vr && modeScope == "flat") {
        g.scopeLine = tr(L"This install does not include VR: turn 3D off in Elite or use Play flat.");
    }
}

COLORREF colourFor(Tone tone) {
    const ui::Theme& t = ui::theme();
    switch (tone) {
        case Tone::Good: return t.good;
        case Tone::Warn: return t.warn;
        case Tone::Bad: return t.bad;
        default: return t.muted;
    }
}

// ---------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------

// The install picker and the folder it names sit above both screens -- and
// above the TABS: the settings being edited are the settings of whichever
// install is selected, and a Settings tab that does not say which folder it
// is writing to is asking to be used on the wrong one.
void paintInstallPicker(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    if (!g.statusPath.empty()) {
        RECT path{dp(kMargin), dp(114), dp(kClientWidth - kMargin), dp(132)};
        ui::drawText(dc, g.statusPath, path, f.caption, t.subtext,
                     DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }
    RECT rule{dp(kMargin), dp(kTabRuleY), dp(kClientWidth - kMargin), dp(kTabRuleY) + 1};
    ui::fillRect(dc, rule, t.cardBorder);
}

void paintStatusCard(HDC dc, int top, int maxBottom) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    const int rowH = 20;
    int needed = 30 + static_cast<int>(g.status.size()) * rowH;
    if (!g.modeLine.empty()) needed += 22;
    if (!g.scopeLine.empty()) needed += 18;
    needed += 10;
    int bottom = top + needed;
    if (bottom < top + 96) bottom = top + 96;
    if (bottom > maxBottom) bottom = maxBottom;

    RECT card{dp(kMargin), dp(top), dp(kClientWidth - kMargin), dp(bottom)};
    ui::paintCard(dc, card, g.dpi);

    RECT statusHead{dp(kMargin + kCardPad), dp(top + 8), dp(kClientWidth - kMargin - kCardPad),
                    dp(top + 26)};
    ui::drawText(dc, tr(L"Status"), statusHead, f.heading, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    int y = top + 30;
    for (const StatusRow& row : g.status) {
        if (y + rowH > bottom - 4) break;
        const int centre = dp(y + rowH / 2);
        ui::fillCircle(dc, dp(kMargin + kCardPad + 5), centre, dp(4), colourFor(row.tone));

        RECT label{dp(kMargin + kCardPad + 20), dp(y), dp(kMargin + kCardPad + 206), dp(y + rowH)};
        ui::drawText(dc, row.label, label, f.body, t.subtext,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT value{dp(kMargin + kCardPad + 216), dp(y), dp(kClientWidth - kMargin - kCardPad),
                   dp(y + rowH)};
        ui::drawText(dc, row.value, value, f.body, t.text,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += rowH;
    }

    if (!g.modeLine.empty() && y + 18 <= bottom - 4) {
        RECT mode{dp(kMargin + kCardPad), dp(y + 4), dp(kClientWidth - kMargin - kCardPad), dp(y + 20)};
        ui::drawText(dc, g.modeLine, mode, f.caption, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!g.scopeLine.empty() && y + 36 <= bottom - 4) {
            RECT scope{dp(kMargin + kCardPad), dp(y + 21), dp(kClientWidth - kMargin - kCardPad),
                       dp(y + 37)};
            ui::drawText(dc, g.scopeLine, scope, f.caption, colourFor(Tone::Warn),
                         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
}

#ifdef EDUP_LAUNCHER
void paintPlayScreen(HDC dc) {
    paintLauncherPlay(dc);
    paintStatusCard(dc, launcherStatusTop(), kContentBottom);
}
#endif

void paintInstallScreen(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();

#ifdef EDUP_LAUNCHER
    paintLauncherUpdatesCard(dc);
#else
    paintStatusCard(dc, kStatusTop, kStatusBottom);
#endif

    RECT actions{dp(kMargin), dp(kActionsTop), dp(kClientWidth - kMargin), dp(kActionsBottom)};
    ui::paintCard(dc, actions, g.dpi);

    RECT actionsHead{dp(kMargin + kCardPad), dp(kActionsTop + 10), dp(kMargin + kCardPad + 180),
                     dp(kActionsTop + 30)};
    ui::drawText(dc, tr(L"Install & repair"), actionsHead, f.heading, t.text,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT scopeLabel = g_scopePromptPx.right > g_scopePromptPx.left
                          ? g_scopePromptPx
                          : RECT{dp(340), dp(kScopeY), dp(470), dp(kScopeY + 30)};
    ui::drawText(dc, tr(L"Install ED-UP for:"), scopeLabel, f.body, t.subtext,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT reassure{dp(kMargin + kCardPad), dp(kActionsNoteY), dp(kClientWidth - kMargin - kCardPad),
                  dp(kActionsNoteY + 20)};
    ui::drawText(dc,
                 tr(L"Nothing is changed until you confirm it, and every file replaced is copied "
                    L"into edup_backup\\ first."),
                 reassure, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    const RECT report = reportCardRect();
    ui::paintCard(dc, report, g.dpi);

    RECT reportHead{dp(kMargin + kCardPad), dp(kReportTop + 8), dp(kClientWidth - kMargin - kCardPad),
                    dp(kReportTop + 26)};
    ui::drawText(dc, tr(L"Log"), reportHead, f.heading, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (g.report) {
        int content = 0, page = 0, pos = 0;
        reportScrollState(&content, &page, &pos);
        ui::drawSlimScrollbar(dc, report, content, page, pos, g.dpi, g.reportDrag);
    }
}

void paintSettingsScreen(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    RECT card{dp(kMargin), dp(kContentTop), dp(kClientWidth - kMargin), dp(kContentBottom)};
    ui::paintCard(dc, card, g.dpi);

    // No "Settings" heading inside the card: the active tab above already
    // says it, and a heading repeating a tab is the same redundancy the
    // Setup tab was renamed to avoid. The note carries the useful part.
    RECT note{dp(kMargin + kCardPad), dp(198), dp(kClientWidth - kMargin - kCardPad), dp(218)};
    const std::wstring where =
        g.settings.loaded()
            ? std::wstring(tr(L"Written into the ")) + settingsLeafFor(g.settingsEdition) +
                  tr(L" of the install above, and live within a second. The few that need a game restart are marked.")
            : tr(L"Pick an install above first.");
    ui::drawText(dc, where, note, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // The search box is a real edit control with its 3D border taken off, so
    // the frame around it is drawn here to match everything else.
    // Its right edge stops cleanly short of the VR/Flat selector at x=390, so the two read as separate controls.
    RECT search{dp(kMargin + kCardPad) - dp(6), dp(230), dp(kMargin + kCardPad) + dp(328),
                dp(262)};
    ui::fillRounded(dc, search, dp(6), t.control);
    ui::strokeRounded(dc, search, dp(6), t.controlBorder);
}

void paintWindow(HWND window) {
    PAINTSTRUCT ps;
    HDC screenDc = BeginPaint(window, &ps);

    RECT client;
    GetClientRect(window, &client);

    // Drawn into a bitmap and blitted: cards, text and dots painted straight
    // onto the window flicker visibly every time the report pane changes.
    HDC dc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, client.right, client.bottom);
    HBITMAP previous = static_cast<HBITMAP>(SelectObject(dc, bitmap));

    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    ui::fillRect(dc, client, t.windowBg);

    RECT title{dp(kMargin), dp(18), dp(kClientWidth - kMargin), dp(50)};
#ifdef EDUP_LAUNCHER
    RECT languageLabel{};
    if (g.languageCombo) {
        RECT comboRc{};
        GetWindowRect(g.languageCombo, &comboRc);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&comboRc), 2);
        languageLabel = comboRc;
        languageLabel.right = comboRc.left - dp(8);
        languageLabel.left = languageLabel.right - dp(120);
        if (languageLabel.left < dp(kMargin)) languageLabel.left = dp(kMargin);
        title.right = languageLabel.left - dp(12);
    }
#endif
    ui::drawText(dc, kProductTitle, title, f.title, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
#ifdef EDUP_LAUNCHER
    if (g.languageCombo)
        ui::drawText(dc, tr(L"Language"), languageLabel, f.body, t.subtext, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
#endif

#ifdef EDUP_LAUNCHER
    const std::wstring subtitle = launcherSubtitle();
#else
    const std::wstring subtitle = fromUtf8(payloadInfo().version) +
        L"  \x00b7  unofficial patch for Elite Dangerous in VR and flat";
#endif
    RECT sub{dp(kMargin), dp(52), dp(kClientWidth - kMargin), dp(70)};
    ui::drawText(dc, subtitle, sub, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    paintInstallPicker(dc);
#ifdef EDUP_LAUNCHER
    if (g.screen == Screen::Play)
        paintPlayScreen(dc);
    else
#endif
    if (g.screen == Screen::Install)
        paintInstallScreen(dc);
    else
        paintSettingsScreen(dc);

#ifndef EDUP_LAUNCHER   // no tip sentence in the launcher: the Discord link stands alone at the right
    RECT tip{dp(kMargin), dp(kTipTop), dp(kClientWidth - kMargin), dp(kTipTop + 20)};
    ui::drawText(dc, kTipText, tip, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE);
#endif

    BitBlt(screenDc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(window, &ps);
}

// ---------------------------------------------------------------------------
// behaviour
// ---------------------------------------------------------------------------

const wchar_t* installButtonLabel() {
    if (!g.haveSurvey) return tr(L"Install");
    const bool installed = g.survey.d3d11.kind == DllKind::Edvr ||
                           g.survey.openvrCurrent.kind == DllKind::Edvr || g.survey.state.present;
    if (!installed) return tr(L"Install");
    std::string payloadVersion;
#ifdef EDUP_LAUNCHER
    if (launcherPayloadReady()) payloadVersion = lg.session->payload().version;
#else
    payloadVersion = payloadInfo().version;
#endif
    const bool sameVersion = !payloadVersion.empty() && g.survey.d3d11.kind == DllKind::Edvr &&
                             !installRecordStale(g.survey) &&
                             g.survey.state.edvrVersion == payloadVersion &&
                             installedScope(g.survey) == scopeChoice();
    return sameVersion ? tr(L"Reinstall") : tr(L"Update");
}

void showSurvey() {
    // Read again on every change: the game's 3D switch is flipped outside this window.
    g.mode = eliteGameMode();
    rebuildStatus();
    if (!g.haveSurvey) {
        EnableWindow(g.install, FALSE);
        EnableWindow(g.repair, FALSE);
        EnableWindow(g.uninstall, FALSE);
        EnableWindow(g.collectLogs, FALSE);
#ifdef EDUP_LAUNCHER
        updateLauncherControls();
#endif
        InvalidateRect(g.window, nullptr, TRUE);
        return;
    }

    // This folder's copy, not the machine's. Another install being in a jump
    // has no hold on the files here.
    const bool running = g.survey.gameRunningHere;
    EnableWindow(g.install, !running);
    EnableWindow(g.repair, !running);
    EnableWindow(g.uninstall, !running);
    // Collecting logs reads files and writes a zip somewhere else entirely, so
    // it is the one action that stays available while the game is running --
    // which is exactly when somebody wants it.
    EnableWindow(g.collectLogs, TRUE);

    SetWindowTextW(g.install, installButtonLabel());
#ifdef EDUP_LAUNCHER
    updateLauncherControls();
#endif
    applyLayout();
}

// Loads one settings file into the settings screen. Before a folder is chosen there
// is nothing to load; the choice is kept for when there is.
void loadSettings(const std::string& edition) {
    g.settingsEdition = edition;
    if (!g.haveSurvey) {
#ifdef EDUP_LAUNCHER
        launcherUpdateSettingsToggle();
#endif
        return;
    }
    const std::wstring mirrorDir = mirrorDirOfGame(g.survey.game);
    g.settings.load(g.survey.game.dir, edition, mirrorDir);
    settingsListSetModel(g.settingsList, &g.settings, mirrorDir);
#ifdef EDUP_LAUNCHER
    launcherUpdateSettingsToggle();
#endif
}

void selectInstall(size_t index) {
    if (index >= g.installs.size()) {
        g.haveSurvey = false;
        showSurvey();
        return;
    }
    g.survey = surveyTarget(g.installs[index], mirrorDirOfGame(g.installs[index]));
    g.haveSurvey = true;
#ifdef EDUP_LAUNCHER
    invalidateLauncherCache();
#endif
    // The scope this folder gets: the one the run was started with, else the folder's own, else both.
    SendMessageW(g.scopeCombo, CB_SETCURSEL, scopeIndex(g.args.scope.empty() ? defaultScope(g.survey) : g.args.scope), 0);
    g.mode = eliteGameMode();
    loadSettings(editionFor(scopeChoice()));
    showSurvey();
#ifdef EDUP_LAUNCHER
    launcherCheckEdhmInterference();
#endif
}

// The scope prompt changed. A settings screen on a file the new scope does not install moves to
// the one it does; the status and the selector follow.
void scopeChanged() {
    const std::string scope = scopeChoice();
    if (scope != "both" && scope != g.settingsEdition) loadSettings(scope);
    showSurvey();
    syncSettingsSelector();
}

void settingsEditionPicked(const std::string& edition) {
    if (edition == g.settingsEdition) return;
    loadSettings(edition);
    syncSettingsSelector();
    InvalidateRect(g.window, nullptr, TRUE);
}

void addInstall(const GameInstall& game) {
    for (size_t i = 0; i < g.installs.size(); ++i) {
        if (_wcsicmp(g.installs[i].dir.c_str(), game.dir.c_str()) == 0) {
            SendMessageW(g.combo, CB_SETCURSEL, i, 0);
            selectInstall(i);
            return;
        }
    }
    g.installs.push_back(game);
    const std::wstring label = game.source + L"  \x00b7  " + game.dir;
    SendMessageW(g.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    const size_t index = g.installs.size() - 1;
    SendMessageW(g.combo, CB_SETCURSEL, index, 0);
    selectInstall(index);
}

void scanForInstalls() {
    SendMessageW(g.combo, CB_RESETCONTENT, 0, 0);
    g.installs.clear();
    size_t legacyLeftOut = 0;
    std::vector<GameInstall> found = findInstalls(&legacyLeftOut);
    // A rescan still knows the host-supplied store for the original exact
    // product. Every other install keeps the source its own detection found.
    GameInstall supplied;
    if (!g.args.store.empty() && describeSelectedDir(g.args, &supplied, &found)) {
        bool listed = false;
        for (GameInstall& game : found) {
            if (_wcsicmp(game.dir.c_str(), supplied.dir.c_str()) != 0) continue;
            game = supplied;
            listed = true;
        }
        if (!listed) found.push_back(supplied);
    }
    for (const GameInstall& game : found) {
        g.installs.push_back(game);
        const std::wstring label = game.source + L"  \x00b7  " + game.dir;
        SendMessageW(g.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }

#ifdef EDUP_LAUNCHER
    // The launcher opens on the install Play was last pressed for, while it is still there.
    const size_t remembered = launcherRememberedInstall();
    if (remembered < g.installs.size()) {
        SendMessageW(g.combo, CB_SETCURSEL, remembered, 0);
        selectInstall(remembered);
        return;
    }
#endif

    if (g.installs.empty()) {
        g.haveSurvey = false;
        // An install that is there but not supported is not "not found".
        const std::string first = legacyLeftOut > 0
                                      ? std::string(onlyLegacyInstallsText()) + "\r\n\r\nIf Odyssey is installed somewhere else, press Browse"
                                      : std::string("No Elite Dangerous install was found.\r\n\r\nPress Browse");
        setReport(first +
                  " and point at the folder that holds EliteDangerous64.exe. For a "
                  "Frontier launcher install that is usually a folder ending in\r\n"
                  "    Products\\elite-dangerous-odyssey-64");
        showSurvey();
        return;
    }

    // With more than one install -- and three storefronts on one machine is not
    // unusual -- the one that already has EDVR in it is the one being updated,
    // and a better guess than whichever store answered first.
    size_t pick = 0;
    for (size_t i = 0; i < g.installs.size(); ++i) {
        const Survey candidate = surveyTarget(g.installs[i], mirrorDirOfGame(g.installs[i]));
        if (candidate.state.present || candidate.d3d11.kind == DllKind::Edvr ||
            candidate.openvrCurrent.kind == DllKind::Edvr) {
            pick = i;
            break;
        }
    }
    SendMessageW(g.combo, CB_SETCURSEL, pick, 0);
    selectInstall(pick);
}

void browseForFolder() {
    // The modern picker, which shows the path and lets it be pasted in --
    // SHBrowseForFolder's tree is not a reasonable way to find a folder eight
    // levels down a Steam library.
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Pick the folder that holds EliteDangerous64.exe");
    if (g.haveSurvey && !g.survey.game.dir.empty()) {
        IShellItem* initial = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(g.survey.game.dir.c_str(), nullptr,
                                                  IID_PPV_ARGS(&initial))) &&
            initial) {
            dialog->SetFolder(initial);
            initial->Release();
        }
    } else {
        // When no install is detected, open on This PC / My Computer rather than Documents (#234).
        IShellItem* computer = nullptr;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr,
                                           IID_PPV_ARGS(&computer))) &&
            computer) {
            dialog->SetFolder(computer);
            computer->Release();
        }
    }

    std::wstring picked;
    if (SUCCEEDED(dialog->Show(g.window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                picked = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (picked.empty()) return;

    GameInstall game;
    if (!describeDir(picked, L"Folder you chose", &game)) {
        MessageBoxW(g.window,
                    L"There is no EliteDangerous64.exe in that folder, or in a Products folder "
                    L"under it.\n\nPick the folder that holds EliteDangerous64.exe itself.",
                    kAppTitle, MB_OK | MB_ICONWARNING);
        return;
    }
    addInstall(game);
}

std::wstring confirmText(const Plan& plan, const wchar_t* verb, bool purge = false) {
    std::wstring text = std::wstring(verb) + L"\n\n";
    for (const std::string& note : plan.notes) text += L"\x2022 " + fromUtf8(note) + L"\n";
    if (!plan.problems.empty()) {
        text += L"\nWorth knowing:\n";
        for (const std::string& p : plan.problems) text += L"! " + fromUtf8(p) + L"\n";
    }
#ifdef EDUP_LAUNCHER
    if (!lg.developerNote.empty()) text += L"\n! " + lg.developerNote + L"\n";
#endif
    if (purge) {
        text += L"\n";
        text += tr(L"Clean purge permanently deletes ED-UP settings, logs, install records, the "
                   L"user settings folder, and everything inside edup_backup\\ (including any backups "
                   L"of other mods' files or earlier edup.ini copies).");
        text += L"\n\nGo ahead?";
    } else {
        text += L"\nA copy of every file replaced goes into edup_backup\\.\n\nGo ahead?";
    }
    return text;
}

void runAction(AppArgs::Act action) {
    if (!g.haveSurvey) return;
#ifdef EDUP_LAUNCHER
    if (g.screen != Screen::Install) showScreen(Screen::Install);
#endif

    const std::wstring mirrorRoot = mirrorRootOfApp();
    const std::wstring mirrorDir = mirrorDirFor(g.survey.game, mirrorRoot);

    // The folder can change between the survey and the button -- the game gets
    // started, another installer runs. Ask the disk again rather than acting on
    // a picture that may be minutes old.
    g.survey = surveyTarget(g.survey.game, mirrorDir);

#ifdef EDUP_LAUNCHER
    if (action != AppArgs::Act::Uninstall && !launcherPayloadReady()) {
        setReport(toUtf8(launcherNoPayloadText()));
        return;
    }
    launcherBeginAction();
#endif

    AppArgs args = g.args;
    args.action = action;
    args.dir = g.survey.game.dir;
    const std::wstring& selectedSource = g.survey.game.source;
    args.store = selectedSource == L"Epic" || selectedSource == L"Steam" || selectedSource == L"Frontier launcher"
                     ? selectedSource : std::wstring();
    args.keepSettings = ui::checkboxChecked(g.chkKeep);
    args.purge = (action == AppArgs::Act::Uninstall) && ui::checkboxChecked(g.chkPurge);
    args.removeSettings = args.purge;
    args.scope = scopeChoice();
#ifdef EDUP_LAUNCHER
    args.edhm2dMigoto = lg.session && lg.session->edhm2dMigoto();
#endif

    const bool canMirror = action != AppArgs::Act::Uninstall;

    if (canMirror && args.keepSettings) {
        const MirrorInfo mirror = readMirror(mirrorDir);
        const std::vector<std::string> missing = settingsToRestore(mirrorDir, mirror, args.scope);
        if (!missing.empty() && !g.survey.iniPresent && !g.survey.flatIniPresent) {
            const std::wstring wanted = fromUtf8(settingsNamesOf(missing));
            const std::wstring text =
                L"No " + wanted + L" was found, but settings from an earlier install "
                L"were found in " + mirror.dir + L", last saved " +
                fromUtf8(mirror.savedUtc) +
                L".\n\nRestore them before installing?";
            const int answer = MessageBoxW(g.window, text.c_str(), kAppTitle,
                                           MB_YESNO | MB_ICONQUESTION);
#ifdef EDUP_LAUNCHER
            const bool restoreNow = launcherRestoreHere(answer == IDYES);
#else
            const bool restoreNow = true;
#endif
            if (answer == IDYES && restoreNow) {
                std::vector<std::string> notes;
                const bool restored = restoreFromMirror(g.survey.game.dir, mirror, args.scope, &notes, mirrorDir);
                if (!restored) {
                    std::wstring why = L"Not all of your saved settings could be restored:\n\n";
                    for (const std::string& n : notes) why += L"\x2022 " + fromUtf8(n) + L"\n";
                    why += L"\nThe install carries on with what is there now.";
                    MessageBoxW(g.window, why.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
                }
                g.survey = surveyTarget(g.survey.game, mirrorDir);
            }
        }
    }
    Options options = optionsFor(args, action == AppArgs::Act::Repair);
    options.settingsDir = mirrorDir;
    const Plan plan = action == AppArgs::Act::Uninstall
                          ? planUninstall(g.survey, options, payloadInfo())
                          : planInstall(g.survey, options, payloadInfo());

    setReport(planReport(plan));

    // Outcomes land in the report pane, not a message box: the pane is
    // right there, it scrolls to the freshest line, and it can be read
    // again after the fact. Only QUESTIONS get a box from here on.
    if (plan.blocked || plan.nothingToDo) {
        // Nothing changed in the folder, but this may be the first run of an
        // installer new enough to mirror at all -- an install already at the
        // latest build must not have to wait for its next update to get one.
        if (canMirror && plan.nothingToDo) updateMirror(g.survey.game.dir, plan.backupDir, mirrorDir);
        if (plan.nothingToDo && action == AppArgs::Act::Uninstall && args.purge) {
            const bool shared = mirrorSharedWithAnotherInstall(g.survey.game, mirrorDir, mirrorRoot, g.installs);
            const bool hasMirror = !mirrorDir.empty() && dirExists(mirrorDir) && !shared;
#ifdef EDUP_LAUNCHER
            const std::wstring edlaunch = findEdLaunch(g.survey.game.dir);
            const size_t slash = edlaunch.find_last_of(L"\\/");
            const bool hasShim = !edlaunch.empty() && slash != std::wstring::npos &&
                                 fileExists(joinPath(edlaunch.substr(0, slash), kEdLaunchMarkerName));
#else
            const bool hasShim = false;
#endif
            if (hasMirror || hasShim) {
                if (args.autorun ||
                    MessageBoxW(g.window, confirmText(plan, L"About to remove ED-UP:", true).c_str(), kAppTitle,
                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                    std::vector<std::string> purgeNotes;
                    purgeAfterUninstall(g.survey.game.dir, mirrorDir, &purgeNotes, !shared);
                    std::string out = planReport(plan) + "\r\n----\r\n";
                    for (const std::string& note : purgeNotes) out += "  " + note + "\r\n";
                    out += "\r\nClean purge complete.\r\n";
                    setReport(out);
                    showSurvey();
                    return;
                }
            }
        }
        setReport(planReport(plan) + "\r\n----\r\n" + planSummary(plan) + "\r\n");
        showSurvey();
        return;
    }

    const wchar_t* verb = action == AppArgs::Act::Uninstall ? L"About to remove ED-UP:"
                          : action == AppArgs::Act::Repair  ? L"About to repair this install:"
                                                            : L"About to install ED-UP:";
    if (!args.autorun) {
        // No is the default button, deliberately. Install is this window's
        // default push button, so Return reaches it -- and if the confirmation
        // took Return as yes, two stray keystrokes at a window that had just
        // taken the foreground would be enough to modify a game folder. That
        // is not hypothetical: it happened twice during development, to a real
        // install, while windows were being opened for screenshots.
        const int answer = MessageBoxW(g.window, confirmText(plan, verb, args.purge).c_str(), kAppTitle,
                                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDYES) return;
    }

#ifdef EDUP_LAUNCHER
    // The launcher never runs itself elevated: a short-lived copy applies the plan.
    if (launcherNeedsElevatedCopy()) {
        launcherApplyElevated(args, plan);
        return;
    }
#endif
    if (needsElevationFor(g.survey) && !isElevated()) {
        const int answer = MessageBoxW(
            g.window,
            L"This game folder can only be written to by an administrator (it is usually under "
            L"Program Files).\n\nRestart the installer with administrator rights and carry on?",
            kAppTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDYES) return;
        AppArgs elevated = args;
        elevated.autorun = true;
        if (relaunchElevated(elevated)) {
            PostMessageW(g.window, WM_CLOSE, 0, 0);
        } else {
            setReport(planReport(plan) +
                      "\r\n----\r\nWindows would not start the installer with "
                      "administrator rights, so nothing was changed.\r\n");
        }
        return;
    }

    const ApplyResult result = applyPlan(plan, payloadItem);

    std::string text = planReport(plan);
    text += "\r\n----\r\n";
    for (const std::string& line : result.done) text += "  " + line + "\r\n";
    if (result.ok) {
        if (action == AppArgs::Act::Uninstall && args.purge) {
            const bool shared = mirrorSharedWithAnotherInstall(g.survey.game, mirrorDir, mirrorRoot, g.installs);
            std::vector<std::string> purgeNotes;
            purgeAfterUninstall(g.survey.game.dir, mirrorDir, &purgeNotes, !shared);
            for (const std::string& note : purgeNotes) text += "  " + note + "\r\n";
            text += "\r\nClean purge complete.\r\n";
        } else {
            text += "\r\nDone. Start Elite Dangerous; ED-UP writes what it managed to install into "
                    "edup_logs\\ next to the game.\r\n";
        }
        if (canMirror) {
            const MirrorResult m = updateMirror(g.survey.game.dir, plan.backupDir, mirrorDir);
            if (m.ok) {
                text += "\r\nMirrored ";
                for (size_t i = 0; i < m.saved.size(); ++i) {
                    if (i) text += ", ";
                    text += m.saved[i];
                }
                text += " to " + toUtf8(mirrorDir) +
                        " (outside the game folder, so a game update wiping this folder cannot "
                        "take it too).\r\n";
            }
        }
    } else {
        text += "\r\n" + result.error + "\r\n";
        if (result.rolledBack) {
            text += "Installed files and metadata were restored; recovery backups were kept.\r\n";
        }
    }
    setReport(text);

    g.survey = surveyTarget(g.survey.game, mirrorDir);
    loadSettings(g.settingsEdition);
    showSurvey();
}

// Wine only. The two buttons add a status line under the saved report and keep the report itself,
// so the crash dump's path and its warning stay on screen.
void copySavedPath() {
    std::string error;
    if (copyTextToClipboard(g.window, g.savedHostPath, &error)) {
        setReport(g.savedReport + "\r\nCopied " + g.savedHostPath + " to the clipboard.\r\n");
    } else {
        setReport(g.savedReport + "\r\nCould not copy the path (" + error + "). It is " + g.savedHostPath +
                  "\r\n");
    }
}

void showSavedFolder() {
    if (!showFolderOnHost(g.savedFolder)) {
        setReport(g.savedReport + "\r\nCould not open the folder. The zip is at " + g.savedHostPath + "\r\n");
    }
}

// One zip on the Desktop with the logs, the breadcrumbs, any fatal note, the
// settings file and the install record -- the four things the README asks a
// reporter to attach, from three different folders, chosen from the right
// session.
void saveLogs() {
    if (!g.haveSurvey) return;
    const bool wine = runningUnderWine();
    const LogBundle bundle = collectLogs(g.survey.game.dir, logSaveFolder());
    g.hostButtonsShown = false;

    std::string text;
    if (bundle.ok) {
        text = "Saved " + hostPathFor(bundle.zipPath) + "\r\n\r\nIt contains:\r\n";
        for (const std::wstring& name : bundle.included) text += "  " + toUtf8(name) + "\r\n";
        if (!bundle.notes.empty()) {
            text += "\r\n";
            for (const std::string& note : bundle.notes) text += "  " + note + "\r\n";
        }
        text += "\r\nAttach it to a GitHub issue, or drop it on Discord. It holds ED-UP's own "
                "logs and settings and nothing else.\r\n";
        // The same zip, as a copied file: Ctrl+V in Discord or a GitHub comment attaches it. Only the
        // logs zip -- the crash dump below is private and never goes on the clipboard.
        std::string clipError;
        if (copyFileToClipboard(g.window, bundle.zipPath, &clipError)) {
            text += "\r\nThe zip is also on your clipboard: press Ctrl+V in a Discord message or a "
                    "GitHub comment to attach it.\r\n";
            if (runningUnderWine()) {
                text += "On Linux, paste before you close this window; the copy may not outlive it.\r\n";
            }
        } else {
            text += "\r\nCould not copy it to the clipboard (" + clipError + "). The file on the " +
                    std::string(wine ? "disk" : "Desktop") + " is fine.\r\n";
        }
        if (!bundle.dumpZipPath.empty()) {
            // The dump is a separate file for a reason: it is a copy of the game's
            // memory, which is not for a public issue.
            text += "\r\nThe game crashed in that session, so its crash dump is saved apart from the logs: " +
                    hostPathFor(bundle.dumpZipPath) +
                    "\r\nIt holds a copy of the game's memory. Do not attach it to a public issue; "
                    "send it to the developer privately if asked.\r\n";
        }
        if (wine) {
            g.savedHostPath = hostPathFor(bundle.zipPath);
            const size_t slash = bundle.zipPath.find_last_of(L'\\');
            g.savedFolder = slash == std::wstring::npos ? bundle.zipPath : bundle.zipPath.substr(0, slash);
            g.hostButtonsShown = true;
        }
    } else {
        text = bundle.error + "\r\n";
        for (const std::string& note : bundle.notes) text += "  " + note + "\r\n";
    }
    if (wine) g.savedReport = text;
    setReport(text);
    if (wine) applyLayout();  // brings up the two buttons
}
void showScreen(Screen screen) {
    g.screen = screen;
#ifdef EDUP_LAUNCHER
    if (g.tabPlay)
        ui::setButtonStyle(g.tabPlay,
                           screen == Screen::Play ? ui::ButtonStyle::TabActive : ui::ButtonStyle::Tab);
#endif
    ui::setButtonStyle(g.tabInstall,
                       screen == Screen::Install ? ui::ButtonStyle::TabActive : ui::ButtonStyle::Tab);
    ui::setButtonStyle(g.tabSettings, screen == Screen::Settings ? ui::ButtonStyle::TabActive
                                                                 : ui::ButtonStyle::Tab);
    applyLayout();
}

void createControls(HWND window) {
    g.realDpi = GetDpiForWindow(window);
    g.dpi = fitLayoutDpi(window, g.realDpi, nullptr);
    ui::buildFonts(g.dpi);
    const ui::Fonts& f = ui::fonts();

    // The tabs sit UNDER the install picker, not beside the title: the
    // screens are views of whichever install is selected above, and the
    // reading order should say so. Widths are measured in applyLayout().
#ifdef EDUP_LAUNCHER
    g.tabPlay = ui::makeButton(window, L"Play", kIdTabPlay, ui::ButtonStyle::Tab, f.body);
    place(g.tabPlay, kMargin, kTabY, 84, kTabH, Screen::Install, true);
#endif
    g.tabInstall = ui::makeButton(window, L"Setup", kIdTabInstall, ui::ButtonStyle::TabActive,
                                  f.body);
    g.tabSettings = ui::makeButton(window, L"Settings", kIdTabSettings, ui::ButtonStyle::Tab,
                                   f.body);
    place(g.tabInstall, kMargin + 90, kTabY, 84, kTabH, Screen::Install, true);
    place(g.tabSettings, kMargin + 180, kTabY, 92, kTabH, Screen::Install, true);

    // The scope prompt sits inside the Install & repair card header row beside the
    // actions it controls. Owner-drawn like the install combo, sized the same way.
    g.scopeCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                                       CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                   0, 0, 10, 10, window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdScope)), nullptr,
                                   nullptr);
    SendMessageW(g.scopeCombo, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    for (const wchar_t* choice : kScopeChoices) {
        SendMessageW(g.scopeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice));
    }
    SendMessageW(g.scopeCombo, CB_SETCURSEL, 0, 0);
    place(g.scopeCombo, kClientWidth - kMargin - kCardPad - 218, kScopeY, 218, 30, Screen::Install);

    // The settings screen's file selector, two segmented buttons inside the Settings card.
    g.settingsVr = ui::makeButton(window, L"VR", kIdSettingsVr, ui::ButtonStyle::SegmentActive, f.body);
    g.settingsFlat = ui::makeButton(window, L"Flat", kIdSettingsFlat, ui::ButtonStyle::Segment, f.body);
    place(g.settingsVr, 390, 230, 148, 32, Screen::Settings);
    place(g.settingsFlat, 546, 230, 150, 32, Screen::Settings);

    // Still a real combo -- keyboard selection and the wheel are the two
    // things a hand-rolled dropdown gets subtly wrong -- but owner-drawn,
    // so the field and the list are painted in the house font and colours,
    // and its field height is converged on the Browse button's in
    // applyLayout (a combo sizes itself from its item height, not from
    // MoveWindow).
    g.combo = CreateWindowExW(0, L"COMBOBOX", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                                  CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                              0, 0, 10, 10, window,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCombo)), nullptr,
                              nullptr);
    SendMessageW(g.combo, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    place(g.combo, kMargin, 80, kClientWidth - 2 * kMargin - 118, 30, Screen::Install, true);

    HWND browse = ui::makeButton(window, L"Browse...", kIdBrowse, ui::ButtonStyle::Secondary,
                                 f.body);
    place(browse, kClientWidth - kMargin - 110, 80, 110, 30, Screen::Install, true);

    // Seeded from the command line, because the elevated relaunch carries
    // --replace-settings and this checkbox is what runAction reads. Defaulting
    // it to checked threw that flag away without a word.
    g.chkKeep = ui::makeCheckbox(window, L"Keep the settings I have changed", kIdChkKeep,
                                 g.args.keepSettings, f.body);
    place(g.chkKeep, kMargin + kCardPad, kActionsChkY, 320, 22, Screen::Install);

    g.chkPurge = ui::makeCheckbox(window, L"Clean purge on uninstall", kIdChkPurge,
                                  g.args.purge, f.body);
    place(g.chkPurge, kMargin + kCardPad + 332, kActionsChkY, 320, 22, Screen::Install);

    g.install = ui::makeButton(window, L"Install", kIdInstall, ui::ButtonStyle::Primary, f.body);
    g.repair = ui::makeButton(window, L"Repair", kIdRepair, ui::ButtonStyle::Secondary, f.body);
    g.uninstall = ui::makeButton(window, L"Uninstall", kIdUninstall, ui::ButtonStyle::Secondary,
                                 f.body);
    g.collectLogs = ui::makeButton(window, L"Save logs", kIdCollectLogs,
                                   ui::ButtonStyle::Secondary, f.body);
    place(g.install, kMargin + kCardPad, kActionsBtnY, 124, 34, Screen::Install);
    place(g.repair, 172, kActionsBtnY, 104, 34, Screen::Install);
    place(g.uninstall, 284, kActionsBtnY, 104, 34, Screen::Install);
    place(g.collectLogs, 396, kActionsBtnY, 116, 34, Screen::Install);
    if (runningUnderWine()) {
        g.copyPath = ui::makeButton(window, L"Copy path", kIdCopyPath, ui::ButtonStyle::Secondary, f.body);
        g.showFolder = ui::makeButton(window, L"Show folder", kIdShowFolder, ui::ButtonStyle::Secondary, f.body);
        ShowWindow(g.copyPath, SW_HIDE);
        ShowWindow(g.showFolder, SW_HIDE);
    }
    // No Close button: it was the one control at list height marked
    // both-screens, so it painted through the settings page -- and the
    // window's own close box already does the job.

    g.search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0,
                               10, 10, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSearch)), nullptr,
                               nullptr);
    SendMessageW(g.search, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    SendMessageW(g.search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(L"Search settings")));
    place(g.search, kMargin + kCardPad, 232, 320, 28, Screen::Settings);

    g.settingsList = createSettingsList(window, kIdSettingsList, g.dpi);
    place(g.settingsList, kMargin + 2, 274, kClientWidth - 2 * kMargin - 4, kContentBottom - 278,
          Screen::Settings);

    g.report = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY, 0,
                               0, 10, 10, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdReport)), nullptr,
                               nullptr);
    SendMessageW(g.report, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    SetWindowSubclass(g.report, reportProc, 1, 0);
    place(g.report, kMargin + kCardPad - 4, kReportEditY, kClientWidth - 2 * kMargin - 2 * kCardPad + 4,
          kReportEditH, Screen::Install);

#ifndef EDUP_LAUNCHER   // the launcher carries no tip link
    g.tip = ui::makeButton(window, L"leave a tip", kIdKofi, ui::ButtonStyle::Link, f.caption);
    place(g.tip, kMargin, kTipTop, 76, 20, Screen::Install, true);  // x and width fixed up in applyLayout
#endif

    g.discord = ui::makeButton(window, L"Ask on Discord", kIdDiscord, ui::ButtonStyle::Link,
                               f.caption);
    place(g.discord, kClientWidth - kMargin - 110, kTipTop, 110, 20, Screen::Install, true);  // sized in applyLayout

    // Dark mode does not reach the native controls on its own. These two theme
    // names are what File Explorer uses for exactly this, and on a light theme
    // (or an older Windows) they simply do nothing.
    if (ui::theme().dark) {
        SetWindowTheme(g.combo, L"DarkMode_CFD", nullptr);
        SetWindowTheme(g.scopeCombo, L"DarkMode_CFD", nullptr);
        SetWindowTheme(g.report, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(g.search, L"DarkMode_CFD", nullptr);
        SetWindowTheme(g.settingsList, L"DarkMode_Explorer", nullptr);
    }

#ifdef EDUP_LAUNCHER
    g.languageCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                                          CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                      0, 0, 10, 10, window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdLanguage)), nullptr, nullptr);
    SendMessageW(g.languageCombo, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    if (ui::theme().dark) SetWindowTheme(g.languageCombo, L"DarkMode_CFD", nullptr);
    fillLanguageCombo();
    createLauncherControls(window);
    {
        LauncherState state;
        state.load(joinPath(launcherRootsFor(g.args).launcherDir(), L"launcher.ini"), nullptr);
        applyLauncherLanguage(state.language());
    }
#endif

    // Deliberately not an explanation of what Install, Repair and Uninstall do.
    // A first reader of this window already knows, and the pane is where the
    // plan goes -- filling it with a lecture beforehand made the one thing it
    // is for look like more of the same text. (Field feedback, on the first
    // build anybody but its author had seen.)
    setReport("What each button is about to do appears here, before anything is changed.");

    showScreen(Screen::Install);
    sizeWindowToLayout();
}

void rescale(HWND window, UINT dpi, const RECT* suggested) {
    g.realDpi = dpi;
    g.dpi = fitLayoutDpi(window, dpi, suggested);
    ui::buildFonts(g.dpi);
    const ui::Fonts& f = ui::fonts();
    for (const Place& p : g.places) {
        SendMessageW(p.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    }
    SendMessageW(g.report, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    settingsListRescale(g.settingsList, g.dpi);
    applyLayout();
    if (suggested) {
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    sizeWindowToLayout();
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            g.window = window;
            ui::applyWindowChrome(window);
            createControls(window);
            PostMessageW(window, kMsgFirstScan, 0, 0);
            return 0;

        case WM_PAINT:
            paintWindow(window);
            return 0;

        case WM_ERASEBKGND:
            return 1;  // the paint above covers every pixel

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (item->CtlID == kIdCombo || item->CtlID == kIdScope || item->CtlID == kIdLanguage
#ifdef EDUP_LAUNCHER
                || item->CtlID == kIdVrMode
#endif
            ) {
                ui::drawComboItem(item, g.dpi);
                return TRUE;
            }
            if (ui::drawOwnerDrawn(item, g.dpi)) return TRUE;
            break;
        }

        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (measure->CtlID == kIdCombo || measure->CtlID == kIdScope || measure->CtlID == kIdLanguage
#ifdef EDUP_LAUNCHER
                || measure->CtlID == kIdVrMode
#endif
            ) {
                measure->itemHeight = static_cast<UINT>(dp(24));
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);
            if (id == kIdSearch && code == EN_CHANGE) {
                wchar_t buffer[256]{};
                GetWindowTextW(g.search, buffer, 256);
                settingsListSetFilter(g.settingsList, buffer);
                return 0;
            }
            if (id == kIdCombo && code == CBN_SELCHANGE) {
                const LRESULT index = SendMessageW(g.combo, CB_GETCURSEL, 0, 0);
                if (index != CB_ERR) selectInstall(static_cast<size_t>(index));
                return 0;
            }
            if (id == kIdScope && code == CBN_SELCHANGE) {
                scopeChanged();
                return 0;
            }
#ifdef EDUP_LAUNCHER
            if (id == kIdLanguage && code == CBN_SELCHANGE) {
                const LRESULT index = SendMessageW(g.languageCombo, CB_GETCURSEL, 0, 0);
                const auto stored = index == CB_ERR ? nullptr
                    : reinterpret_cast<const char*>(SendMessageW(g.languageCombo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0));
                if (stored) {
                    if (lg.session) lg.session->setLanguage(stored);
                    else {
                        LauncherState state;
                        const std::wstring path = joinPath(launcherRootsFor(g.args).launcherDir(), L"launcher.ini");
                        state.load(path, nullptr);
                        state.setLanguage(stored);
                        state.save(nullptr);
                    }
                    applyLauncherLanguage(stored);
                }
                return 0;
            }
#endif
#ifdef EDUP_LAUNCHER
            if (launcherCommand(id, code)) return 0;
#endif
            if (code != BN_CLICKED) return 0;
            switch (id) {
                case kIdBrowse: browseForFolder(); return 0;
                case kIdSettingsVr: settingsEditionPicked("vr"); return 0;
                case kIdSettingsFlat: settingsEditionPicked("flat"); return 0;
                case kIdInstall: runAction(AppArgs::Act::Install); return 0;
                case kIdRepair: runAction(AppArgs::Act::Repair); return 0;
                case kIdUninstall: runAction(AppArgs::Act::Uninstall); return 0;
                case kIdCollectLogs: saveLogs(); return 0;
                case kIdCopyPath: copySavedPath(); return 0;
                case kIdShowFolder: showSavedFolder(); return 0;
#ifdef EDUP_LAUNCHER
                case kIdTabPlay: showScreen(Screen::Play); return 0;
#endif
                case kIdTabInstall: showScreen(Screen::Install); return 0;
                case kIdTabSettings: showScreen(Screen::Settings); return 0;
#ifndef EDUP_LAUNCHER   // no control has this id in the launcher
                case kIdKofi:
                    ShellExecuteW(window, L"open", kTipUrl, nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
#endif
                case kIdDiscord:
                    ShellExecuteW(window, L"open", kDiscordUrl, nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case kIdChkKeep:
                case kIdChkPurge:
                    ui::toggleCheckbox(reinterpret_cast<HWND>(lparam));
                    return 0;
                default: return 0;
            }
        }

        // The report card's slim scrollbar is parent territory (the EDIT
        // keeps no bar of its own): a press in the right gutter grabs or
        // jumps the thumb, and the drag drives the EDIT by whole lines.
        case WM_LBUTTONDOWN: {
            if (g.screen != Screen::Install || !g.report) break;
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            const RECT card = reportCardRect();
            if (x < card.right - dp(12) || x >= card.right || y < card.top || y >= card.bottom)
                break;
            int content = 0, page = 0, pos = 0;
            reportScrollState(&content, &page, &pos);
            const RECT thumb = ui::slimThumb(card, content, page, pos, g.dpi);
            if (thumb.right <= thumb.left) break;
            if (y >= thumb.top && y < thumb.bottom) {
                g.reportDragOffset = y - thumb.top;
            } else {
                g.reportDragOffset = (thumb.bottom - thumb.top) / 2;
            }
            g.reportDrag = true;
            SetCapture(window);
            // fall through to the move handler's math via a synthetic move
        }
            [[fallthrough]];
        case WM_MOUSEMOVE: {
            if (!g.reportDrag) break;
            int content = 0, page = 0, pos = 0;
            reportScrollState(&content, &page, &pos);
            const int wanted =
                ui::slimPosFromThumbTop(reportCardRect(), content, page,
                                        GET_Y_LPARAM(lparam) - g.reportDragOffset, g.dpi);
            const int lineHeight = reportLineHeight();
            const int delta = wanted / lineHeight - pos / lineHeight;
            if (delta != 0) SendMessageW(g.report, EM_LINESCROLL, 0, delta);
            invalidateReportGutter();
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (g.reportDrag) {
                g.reportDrag = false;
                if (message == WM_LBUTTONUP) ReleaseCapture();
                invalidateReportGutter();
            }
            break;

        // WM_MOUSEWHEEL goes to whichever control has keyboard focus, not
        // the one under the cursor -- and nobody clicks a read-only status
        // pane before trying to scroll it. The settings list gets away with
        // focus-routing because its rows take focus on click; the report
        // needs the tick forwarded here so hovering it is enough.
        case WM_MOUSEWHEEL: {
            if (g.screen != Screen::Install || !g.report) break;
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};  // screen coords
            ScreenToClient(window, &pt);
            const RECT card = reportCardRect();
            if (!PtInRect(&card, pt)) break;
            return SendMessageW(g.report, WM_MOUSEWHEEL, wparam, lparam);
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX: {
            // The report pane sits inside a card and has to be the same colour
            // as it, or it reads as a sunken 1990s text box. The search box sits
            // inside a frame painted behind it, and takes that frame's fill.
            HDC dc = reinterpret_cast<HDC>(wparam);
            const ui::Theme& t = ui::theme();
            const bool isSearch = reinterpret_cast<HWND>(lparam) == g.search;
            const COLORREF fill = isSearch ? t.control : t.cardBg;
            SetTextColor(dc, t.text);
            SetBkColor(dc, fill);
            static HBRUSH brushes[2] = {nullptr, nullptr};
            static COLORREF colours[2] = {0, 0};
            const int slot = isSearch ? 1 : 0;
            if (!brushes[slot] || colours[slot] != fill) {
                if (brushes[slot]) DeleteObject(brushes[slot]);
                brushes[slot] = CreateSolidBrush(fill);
                colours[slot] = fill;
            }
            return reinterpret_cast<LRESULT>(brushes[slot]);
        }

        case WM_SETTINGCHANGE:
            // The system switched between light and dark while we were open.
            if (lparam && wcscmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) {
                ui::refreshTheme();
                ui::applyWindowChrome(window);
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;

        case WM_DPICHANGED:
            rescale(window, HIWORD(wparam), reinterpret_cast<const RECT*>(lparam));
            return 0;

#ifdef EDUP_LAUNCHER
        case kMsgLauncherJob:
            runLauncherJob(lparam);
            return 0;
#endif

        case kMsgFirstScan: {
#ifdef EDUP_LAUNCHER
            launcherBegin();  // the payload and the update check run beside the scan
#endif
            // After the window is up, so the first paint is not held behind a
            // registry and filesystem sweep.
            if (!g.args.dir.empty()) {
                GameInstall game;
                if (describeSelectedDir(g.args, &game))
                    addInstall(game);
                else if (!g.args.store.empty())
                    setReport("--store needs --dir with an existing, exact game product folder. No install was selected.");
                else
                    scanForInstalls();
            } else {
                scanForInstalls();
            }
            if (g.args.autorun && g.args.action != AppArgs::Act::None && g.haveSurvey) {
                runAction(g.args.action);
            }
#ifdef EDUP_LAUNCHER
            launcherAfterScan();
#endif
            return 0;
        }

        case WM_CLOSE:
#ifdef EDUP_LAUNCHER
            if (!launcherMayClose()) return 0;
#endif
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
#ifdef EDUP_LAUNCHER
            launcherShutdown();  // stops the watcher and the workers; the game and EDLaunch are left running
#endif
            ui::releaseFonts();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int runGui(HINSTANCE instance, const AppArgs& args) {
    g.args = args;

    const INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ui::startup();

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = nullptr;  // painted, not erased
    cls.style = CS_HREDRAW | CS_VREDRAW;
#ifdef EDUP_LAUNCHER
    cls.lpszClassName = kLauncherWindowClass;
#else
    cls.lpszClassName = L"EdvrInstallerWindow";
#endif
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hIconSm = cls.hIcon;
    RegisterClassExW(&cls);

    HWND window = CreateWindowExW(0, cls.lpszClassName, kAppTitle,
                                  (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) |
                                      WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, kClientWidth, kClientHeight,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // A wheel tick is delivered to whatever holds keyboard FOCUS, not to
        // whatever the cursor is over. Every wheel handler in this program
        // sits in a window proc, so unless that window happens to be focused
        // its handler never runs at all -- which is why the report pane's own
        // fix did nothing in the field: the tick was going to whichever
        // button had focus and stopping there.
        //
        // Redirecting here, before dispatch, is the one place that fixes it
        // for every control at once. Anything outside our own window tree is
        // left alone, and WindowFromPoint skips hidden and disabled children,
        // so a pane in either state still falls through to the parent's
        // handler.
        if (message.message == WM_MOUSEWHEEL) {
            const POINT pt{GET_X_LPARAM(message.lParam),
                           GET_Y_LPARAM(message.lParam)};   // screen coords
            const HWND under = WindowFromPoint(pt);
            if (under && (under == window || IsChild(window, under))) {
                message.hwnd = under;
            }
        }
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    ui::shutdown();
    CoUninitialize();
    return 0;
}

}  // namespace edvr::installer
