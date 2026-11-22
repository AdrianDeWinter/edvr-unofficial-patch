#include "settings_view.h"
#include "product_title.h"

#include <commctrl.h>
#include <windowsx.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <vector>

#include "detect.h"
#include "mirror.h"
#include "ui.h"
#include "../d3d11/menu_flat_rows.h"

#pragma comment(lib, "comctl32.lib")

namespace edvr::installer {
namespace {

const wchar_t* kClassName = L"EdvrSettingsList";
const int kEditId = 9001;
const int kChoiceId = 9002;

// Everything below is in 96-dpi units.
const int kPad = 18;
const int kHeaderHeight = 38;
const int kRowHeight = 68;
const int kLauncherRowHeight = 88;
const int kLauncherCompactRowHeight = 70;
const int kLauncherRowWithWarningHeight = 128;
const int kControlWidth = 240;
// Kept back from each end of a segment. The corners are rounded, and DrawText
// ellipsises a little inside the rect, so a label wider than what remains
// would read with a "..." on the button.
const int kSegmentTextInset = 12;

enum class ItemKind { Header, Setting, LauncherToggle };

struct Item {
    ItemKind    kind = ItemKind::Setting;
    std::wstring title;   // headers
    size_t      row = 0;  // settings or launcherRows: index into the corresponding vector
    int         y = 0;    // laid out, in 96-dpi units
    int         height = 0;
};

struct List {
    HWND           hwnd = nullptr;
    HWND           edit = nullptr;
    SettingsModel* model = nullptr;
    std::wstring   mirrorDir;  // where each write is echoed; see mirror.h
    std::vector<LauncherToggleRow> launcherRows;
    UINT           dpi = 96;
    int            scroll = 0;      // pixels
    int            contentHeight = 0;
    std::vector<Item> items;
    std::wstring   filter;
    size_t         editingRow = SIZE_MAX;
    std::string    error;
    bool           dragThumb = false;   // the slim scrollbar's drag state
    int            dragOffset = 0;      // grab point within the thumb
    // One dropdown, created on the first choice that needs one and shown over
    // that row. choicePress is the mouse-down on its closed control: the list
    // opens on mouse-up, so the button-up does not land on the popup and
    // dismiss it in the same click.
    HWND           choice = nullptr;
    size_t         choiceRow = SIZE_MAX;
    std::string    choiceOriginal;
    bool           choiceClosing = false;
    bool           choiceFilling = false;
    bool           choiceCancel = false;
    bool           choicePress = false;
};

List* listOf(HWND hwnd) {
    return reinterpret_cast<List*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int dp(const List* list, int v) { return ui::dp(v, list->dpi); }

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

bool isVscreenKey(const SettingDef& def, const char* key) {
    return strcmp(def.section, "fix") == 0 && strcmp(def.key, key) == 0;
}

// A row's effective value: what the file says, or what it ships as when
// the file says nothing.
std::string effectiveValue(const SettingRow& row) {
    return row.value.empty() ? std::string(row.def->shipped) : row.value;
}

bool matches(const SettingRow& row, const std::wstring& needle) {
    if (needle.empty()) return true;
    const std::wstring hay = lower(fromUtf8(std::string(row.def->label) + " " + row.def->key +
                                            " " + row.def->section + " " + row.def->description));
    return hay.find(needle) != std::wstring::npos;
}

// edvr.ini's own headings -- "The on-foot screen", "The Full System Scanner"
// -- are what the window shows. A setting with no heading above it falls back
// to its section name, which is an address rather than a heading, but is
// better than an untitled run of rows.
std::wstring headingFor(const SettingDef& def) {
    if (def.group && *def.group) return fromUtf8(def.group);
    const std::string section = def.section;
    if (section == "fix") return L"The fixes";
    if (section == "advanced") return L"Advanced";
    return fromUtf8(section);
}

bool matchesLauncherRow(const LauncherToggleRow& row, const std::wstring& needle) {
    if (!row.visible) return false;
    if (needle.empty()) return true;
    const std::wstring hay =
        lower(row.group + L" " + row.label + L" " + row.description + L" " + row.warning + L" " + row.searchTerms);
    return hay.find(needle) != std::wstring::npos;
}

void rebuildItems(List* list) {
    list->items.clear();

    const std::wstring needle = lower(list->filter);
    std::wstring heading;
    bool haveHeading = false;
    int y = kPad;

    for (size_t i = 0; i < list->launcherRows.size(); ++i) {
        const LauncherToggleRow& row = list->launcherRows[i];
        if (!matchesLauncherRow(row, needle)) continue;
        if (!row.group.empty() && (!haveHeading || row.group != heading)) {
            heading = row.group;
            haveHeading = true;
            Item header;
            header.kind = ItemKind::Header;
            header.title = heading;
            header.y = y;
            header.height = kHeaderHeight;
            list->items.push_back(header);
            y += kHeaderHeight;
        }
        Item item;
        item.kind = ItemKind::LauncherToggle;
        item.row = i;
        item.y = y;
        item.height = !row.warning.empty()
                          ? kLauncherRowWithWarningHeight
                          : (row.control == LauncherControlKind::Checkbox ? kLauncherCompactRowHeight
                                                                          : kLauncherRowHeight);
        list->items.push_back(item);
        y += item.height;
    }

    if (!list->model) {
        list->contentHeight = y + kPad;
        return;
    }

    const std::vector<SettingRow>& rows = list->model->rows();

    // The DLSS preset, DittoSR's rows and XeSS Frame Generation appear only while their parent is selected.
    // Found fresh each rebuild.
    std::string aaHeld = "off";
    bool fgOn = false;
    for (const SettingRow& r : rows) {
        if (isVscreenKey(*r.def, "temporal_aa")) aaHeld = effectiveValue(r);
        else if (isVscreenKey(*r.def, "xess_frame_generation"))
            fgOn = _stricmp(effectiveValue(r).c_str(), "on") == 0;
    }

    for (size_t i = 0; i < rows.size(); ++i) {
        if (!matches(rows[i], needle)) continue;
        if (rows[i].def && aaDependentRowHidden(rows[i].def->key, aaHeld.c_str(), fgOn)) continue;
        const std::wstring wanted = headingFor(*rows[i].def);
        if (!haveHeading || wanted != heading) {
            heading = wanted;
            haveHeading = true;
            Item header;
            header.kind = ItemKind::Header;
            header.title = heading;
            header.y = y;
            header.height = kHeaderHeight;
            list->items.push_back(header);
            y += kHeaderHeight;
        }
        Item item;
        item.kind = ItemKind::Setting;
        item.row = i;
        item.y = y;
        item.height = kRowHeight;
        list->items.push_back(item);
        y += kRowHeight;
    }
    list->contentHeight = y + kPad;
}

// Clamps the position; the bar itself is the slim thumb paint() draws (the
// native scrollbar was the one piece of stock chrome left on this window,
// and it never matched the cards it sat beside).
void updateScrollbar(List* list) {
    RECT client{};
    GetClientRect(list->hwnd, &client);
    const int page = client.bottom - client.top;
    const int content = dp(list, list->contentHeight);
    list->scroll = std::max(0, std::min(list->scroll, std::max(0, content - page)));
}

void commitEdit(List* list);
void closeChoice(List* list, bool apply);

void hideEdit(List* list) {
    if (list->edit) ShowWindow(list->edit, SW_HIDE);
    list->editingRow = SIZE_MAX;
}

void scrollTo(List* list, int position) {
    closeChoice(list, true);
    commitEdit(list);
    hideEdit(list);
    RECT client{};
    GetClientRect(list->hwnd, &client);
    const int page = client.bottom - client.top;
    const int content = dp(list, list->contentHeight);
    const int clamped = std::max(0, std::min(position, std::max(0, content - page)));
    if (clamped == list->scroll) return;
    list->scroll = clamped;
    updateScrollbar(list);
    InvalidateRect(list->hwnd, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// geometry within a row, all in device pixels
// ---------------------------------------------------------------------------

struct RowGeometry {
    RECT label;
    RECT description;
    RECT control;      // the toggle, segments or value box
    RECT recommended;  // the pill under it
};

RowGeometry geometryFor(const List* list, const RECT& row) {
    RowGeometry geo{};
    const int pad = dp(list, kPad);
    const int controlWidth = dp(list, kControlWidth);

    geo.label = {row.left + pad, row.top + dp(list, 8), row.right - controlWidth - pad,
                 row.top + dp(list, 28)};
    geo.description = {row.left + pad, row.top + dp(list, 28), row.right - controlWidth - pad,
                       row.top + dp(list, 62)};
    geo.control = {row.right - controlWidth, row.top + dp(list, 10), row.right - pad,
                   row.top + dp(list, 38)};
    geo.recommended = {row.right - controlWidth, row.top + dp(list, 42), row.right - pad,
                       row.top + dp(list, 60)};
    return geo;
}

// Segment rectangles for a choice row, right-aligned inside the control area.
// choiceUsesDropdown paints a dropdown in this same rect when the segments
// would cut a label off, and whenever there are more than three of them.
std::vector<RECT> segmentsFor(const List* list, const RowGeometry& geo, size_t count) {
    std::vector<RECT> out;
    if (count == 0) return out;
    const int gap = dp(list, 4);
    const int total = geo.control.right - geo.control.left;
    const int width = (total - gap * static_cast<int>(count - 1)) / static_cast<int>(count);
    int x = geo.control.left;
    for (size_t i = 0; i < count; ++i) {
        RECT r{x, geo.control.top, x + width, geo.control.bottom};
        out.push_back(r);
        x += width + gap;
    }
    return out;
}

// More than three choices, or any row whose labels do not fit the segments
// above, is a dropdown. The closed control names the current choice; the
// list names every one in full.
bool choiceUsesDropdown(const List* list, HDC dc, const RowGeometry& geo, const SettingRow& row) {
    if (row.choices.size() > 3) return true;
    if (row.choices.empty()) return false;
    const std::vector<RECT> segments = segmentsFor(list, geo, row.choices.size());
    if (segments.empty()) return true;
    int narrowest = segments[0].right - segments[0].left;
    for (const RECT& segment : segments) {
        const int width = segment.right - segment.left;
        if (width < narrowest) narrowest = width;
    }
    const int room = narrowest - dp(list, kSegmentTextInset);
    if (room <= 0) return true;
    const HFONT font = ui::fonts().caption;
    for (const Choice& choice : row.choices) {
        if (ui::textWidth(dc, fromUtf8(choice.label), font) > room) return true;
    }
    return false;
}

std::wstring currentChoiceLabel(const SettingRow& row) {
    for (const Choice& choice : row.choices) {
        if (choice.value == row.value) return fromUtf8(choice.label);
    }
    return fromUtf8(row.value);
}

void paintChoiceDropdown(const List* list, HDC dc, const RowGeometry& geo, const SettingRow& row) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    const RECT box = geo.control;
    ui::fillRounded(dc, box, dp(list, 5), t.control);
    ui::strokeRounded(dc, box, dp(list, 5), t.controlBorder);
    RECT text = box;
    text.left += dp(list, 10);
    text.right -= dp(list, 22);
    ui::drawText(dc, currentChoiceLabel(row), text, f.body, t.text,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT chevron = box;
    chevron.left = chevron.right - dp(list, 22);
    ui::drawText(dc, L"\u25BE", chevron, f.caption, t.subtext,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

RECT toggleRect(const List* list, const RowGeometry& geo) {
    const int width = dp(list, 46);
    const int height = dp(list, 24);
    RECT r{geo.control.right - width, geo.control.top + dp(list, 2), geo.control.right,
           geo.control.top + dp(list, 2) + height};
    return r;
}

RECT valueRect(const List* list, const RowGeometry& geo) {
    const int width = dp(list, 140);
    RECT r{geo.control.right - width, geo.control.top, geo.control.right, geo.control.bottom};
    return r;
}

COLORREF mix(COLORREF a, COLORREF b, int percentB) {
    const int r = (GetRValue(a) * (100 - percentB) + GetRValue(b) * percentB) / 100;
    const int g = (GetGValue(a) * (100 - percentB) + GetGValue(b) * percentB) / 100;
    const int bl = (GetBValue(a) * (100 - percentB) + GetBValue(b) * percentB) / 100;
    return RGB(r, g, bl);
}

bool isOn(const std::string& value) {
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

// ---------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------

// Only the ones that need a restart are marked. Every other row is live,
// which the line above the list says once -- twenty rows each carrying a
// "live" badge would say it twenty times and mean less each time.
void drawRestartBadge(const List* list, HDC dc, const RECT& labelRect,
                      const std::wstring& label) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    const std::wstring mark = L"restart the game";
    const int width = ui::textWidth(dc, mark, f.caption) + dp(list, 16);
    const int labelWidth = ui::textWidth(dc, label, f.bodyBold);
    RECT badge{labelRect.left + labelWidth + dp(list, 10), labelRect.top + dp(list, 2),
               labelRect.left + labelWidth + dp(list, 10) + width,
               labelRect.bottom + dp(list, 1)};
    if (badge.right < labelRect.right) {
        ui::fillRounded(dc, badge, (badge.bottom - badge.top) / 2, mix(t.cardBg, t.warn, 18));
        ui::drawText(dc, mark, badge, f.caption, t.warn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// A two-way choice where one side is "leave the game alone" reads better as
// a switch than as two buttons: on is the fix, off is stock. The value
// written stays the choice string -- only the control changes.
bool twoChoiceToggle(const SettingRow& row, std::string* onValue,
                     std::string* offValue) {
    if (!row.def || row.def->kind != SettingKind::Choice ||
        row.choices.size() != 2) {
        return false;
    }
    int off = -1;
    for (int i = 0; i < 2; ++i) {
        if (row.choices[i].value == "stock" || row.choices[i].value == "off") {
            off = i;
        }
    }
    if (off < 0) return false;
    if (onValue) *onValue = row.choices[1 - off].value;
    if (offValue) *offValue = row.choices[off].value;
    return true;
}

// What a switch is SET to, in the file's own word. A switch shows a position,
// not a word, and nothing else on the row says that on here writes "steady"
// and off writes "stock". Suppressed where the word would only repeat the
// switch -- a bool, or a choice whose two values already are on and off -- and
// absent for a segmented choice, whose segments carry the words themselves.
std::wstring stateWord(const SettingRow& row) {
    std::string onValue, offValue;
    if (!twoChoiceToggle(row, &onValue, &offValue)) return std::wstring();
    if (onValue == "on" && offValue == "off") return std::wstring();
    const std::string current = row.value == onValue ? onValue : offValue;
    for (const Choice& choice : row.choices) {
        if (choice.value == current) return fromUtf8(choice.label);
    }
    return fromUtf8(current);
}

// The recommended value as somebody would say it: a toggle holds 1 or 0 in the
// file, and "1" under a switch names nothing.
std::wstring recommendedWord(const SettingRow& row) {
    if (row.def->kind == SettingKind::Toggle) {
        return isOn(row.def->recommended) ? L"on" : L"off";
    }
    return row.shownRecommended();
}

// The one link in a row, shown only when the value is not the recommended one,
// and worded for what it DOES. The old text named the value instead -- "use
// splash" under a switch, which the field read as a state label, and then read
// "use auto" beside an auto/on/off control as the control being wrong about
// itself. Where the recommendation IS the shipped default this is the way
// back; where it is not -- a 0.3 curve with a 0.7 distance is a tested pairing
// and neither number is the default -- it is an invitation. One word, because
// the longest helper it sits beside -- the resolution rows' default and their
// range -- leaves 74 px of the 222 this column has, and a range ellipsised to
// "640 to 819" is a wrong number rather than a shortened one.
std::wstring recommendLink(const SettingRow& row) {
    if (row.isRecommended) return std::wstring();
    // A text row that ships empty gets no link -- every one of them is a
    // per-headset list (fix.openxr_resolution, and the field-of-view trims
    // while they were [fix] rows), which this window edits as the text it is.
    // "Reset to nothing" is what
    // clearing the field already does, and one click that empties every
    // headset's entry is a hazard, not a convenience
    // (docs/openxr-resolution-per-headset-2026-09-14.md, "Menu").
    if (row.def->kind == SettingKind::Text && !row.def->recommended[0]) return std::wstring();
    if (strcmp(row.def->recommended, row.def->shipped) == 0) return L"reset";
    return L"try " + recommendedWord(row);
}

// The link's own rectangle: the whole of the click target and none of the
// quiet text beside it. The old line was one target end to end, so a click on
// the "default splash" half applied the recommendation -- which is exactly why
// one line of two phrases read as two links.
RECT recommendLinkRect(const List* list, HDC dc, const RowGeometry& geo,
                       const std::wstring& label) {
    RECT r{0, 0, 0, 0};
    if (label.empty()) return r;
    r = geo.recommended;
    r.left = r.right - ui::textWidth(dc, label, ui::fonts().caption) - dp(list, 4);
    return r;
}

void paintRow(const List* list, HDC dc, const RECT& rowRect, const SettingRow& row) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    const RowGeometry geo = geometryFor(list, rowRect);

    RECT labelRect = geo.label;
    ui::drawText(dc, fromUtf8(row.def->label), labelRect, f.bodyBold, t.text,
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (row.def->needsRestart) {
        drawRestartBadge(list, dc, labelRect, fromUtf8(row.def->label));
    }
    RECT description = geo.description;
    ui::drawText(dc, fromUtf8(row.def->summary), description, f.caption, t.subtext,
                 DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

    switch (row.def->kind) {
        case SettingKind::Toggle: {
            ui::paintToggle(dc, toggleRect(list, geo), isOn(row.value), list->dpi);
            break;
        }
        case SettingKind::Choice: {
            std::string onValue;
            if (twoChoiceToggle(row, &onValue, nullptr)) {
                ui::paintToggle(dc, toggleRect(list, geo), row.value == onValue, list->dpi);
                break;
            }
            if (choiceUsesDropdown(list, dc, geo, row)) {
                paintChoiceDropdown(list, dc, geo, row);
                break;
            }
            const std::vector<RECT> segments = segmentsFor(list, geo, row.choices.size());
            for (size_t i = 0; i < segments.size(); ++i) {
                const bool selected = row.choices[i].value == row.value;
                ui::fillRounded(dc, segments[i], dp(list, 5), selected ? t.accent : t.control);
                if (!selected) ui::strokeRounded(dc, segments[i], dp(list, 5), t.controlBorder);
                RECT text = segments[i];
                ui::drawText(dc, fromUtf8(row.choices[i].label), text, f.caption,
                             selected ? t.accentText : t.text,
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            break;
        }
        default: {
            const RECT box = valueRect(list, geo);
            ui::fillRounded(dc, box, dp(list, 5), t.control);
            ui::strokeRounded(dc, box, dp(list, 5), t.controlBorder);
            RECT text = box;
            text.left += dp(list, 8);
            text.right -= dp(list, 8);
            const std::wstring shown =
                row.value.empty() ? std::wstring(L"(not set)") : row.shown();
            ui::drawText(dc, shown, text, f.body,
                         row.value.empty() ? t.muted : t.text,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            break;
        }
    }

    // Under the control, at most two things and never two links: one quiet
    // word for what a switch is set to (for a number, what it ships as and
    // what it will take), and -- only when the value is not the recommended
    // one -- the single link that puts it right. The link is drawn first, so a
    // long quiet half is the half that gets ellipsised.
    const bool isChoice = row.def->kind == SettingKind::Choice;
    const std::wstring link = recommendLink(row);
    const std::wstring quiet = isChoice ? stateWord(row) : row.helper();
    RECT line = geo.recommended;
    if (!link.empty()) {
        ui::drawText(dc, link, line, f.caption, t.accent, DT_RIGHT | DT_SINGLELINE);
        line.right -= ui::textWidth(dc, link, f.caption) + dp(list, 12);
    }
    if (!quiet.empty() && line.right > line.left) {
        ui::drawText(dc, quiet, line, f.caption, isChoice ? t.subtext : t.muted,
                     DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

struct LauncherRowGeo {
    RECT rail;
    RECT label;
    RECT description;
    RECT controlVisual;
    RECT controlHit;
    RECT status;
    RECT action;
    RECT warningBox;
    RECT warningText;
};

LauncherRowGeo launcherGeometryFor(const List* list, const RECT& rowRect, const LauncherToggleRow& row) {
    LauncherRowGeo g{};
    const bool hasWarning = !row.warning.empty();
    const bool isCheckbox = row.control == LauncherControlKind::Checkbox;
    const int pad = dp(list, kPad);
    const int rightCol = dp(list, 198);
    const int bodyBottom = hasWarning ? (rowRect.top + dp(list, 82)) : (rowRect.bottom - dp(list, 8));
    g.rail = {rowRect.left + dp(list, 8), rowRect.top + dp(list, 10), rowRect.left + dp(list, 10),
              rowRect.bottom - dp(list, 12)};
    if (isCheckbox) {
        g.label = {rowRect.left + pad, rowRect.top + dp(list, 10), rowRect.right - rightCol - pad,
                   rowRect.top + dp(list, 30)};
        g.description = {rowRect.left + pad, rowRect.top + dp(list, 32), rowRect.right - rightCol - pad, bodyBottom};
        const int side = dp(list, 18);
        g.controlVisual = {rowRect.right - pad - side, rowRect.top + dp(list, 12), rowRect.right - pad,
                           rowRect.top + dp(list, 12) + side};
        g.controlHit = {g.controlVisual.left - dp(list, 8), g.controlVisual.top - dp(list, 6),
                        g.controlVisual.right + dp(list, 6), g.controlVisual.bottom + dp(list, 6)};
        g.status = {rowRect.right - rightCol, rowRect.top + dp(list, 36), rowRect.right - pad,
                    rowRect.top + dp(list, 54)};
        g.action = {rowRect.right - rightCol, rowRect.top + dp(list, 54), rowRect.right - pad,
                    rowRect.top + dp(list, 68)};
    } else {
        g.label = {rowRect.left + pad, rowRect.top + dp(list, 8), rowRect.right - rightCol - pad,
                   rowRect.top + dp(list, 44)};
        g.description = {rowRect.left + pad, rowRect.top + dp(list, 46), rowRect.right - rightCol - pad, bodyBottom};
        const int tw = dp(list, 46);
        const int th = dp(list, 24);
        g.controlVisual = {rowRect.right - pad - tw, rowRect.top + dp(list, 12), rowRect.right - pad,
                           rowRect.top + dp(list, 12) + th};
        g.controlHit = g.controlVisual;
        g.status = {rowRect.right - rightCol, rowRect.top + dp(list, 44), rowRect.right - pad,
                    rowRect.top + dp(list, 62)};
        g.action = {rowRect.right - rightCol, rowRect.top + dp(list, 64), rowRect.right - pad,
                    rowRect.top + dp(list, 80)};
    }
    if (hasWarning) {
        g.warningBox = {rowRect.left + pad, rowRect.top + dp(list, 86), rowRect.right - pad,
                        rowRect.bottom - dp(list, 10)};
        g.warningText = {g.warningBox.left + dp(list, 10), g.warningBox.top + dp(list, 4),
                         g.warningBox.right - dp(list, 10), g.warningBox.bottom - dp(list, 4)};
    }
    return g;
}

RECT launcherActionRect(const List* list, HDC dc, const LauncherRowGeo& geo, const std::wstring& link) {
    RECT r{0, 0, 0, 0};
    if (link.empty()) return r;
    r = geo.action;
    r.left = r.right - ui::textWidth(dc, link, ui::fonts().caption) - dp(list, 4);
    return r;
}

COLORREF toneColor(LauncherRowTone tone) {
    const ui::Theme& t = ui::theme();
    switch (tone) {
        case LauncherRowTone::Good: return t.good;
        case LauncherRowTone::Accent: return t.accent;
        case LauncherRowTone::Warn: return t.warn;
        case LauncherRowTone::Bad: return t.bad;
        default: return t.muted;
    }
}

void paintLauncherToggleRow(const List* list, HDC dc, const RECT& rowRect, const LauncherToggleRow& row) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    const bool hasWarning = !row.warning.empty();
    const bool disabled = !row.enabled || row.busy;
    const LauncherRowGeo geo = launcherGeometryFor(list, rowRect, row);

    // Thin left rail echoes the Play hero card and marks this as a launcher-managed runtime option.
    const COLORREF railColor =
        hasWarning ? t.warn
                   : (!row.enabled ? t.cardBorder
                                   : (row.on ? t.accent
                                             : (row.control == LauncherControlKind::Checkbox
                                                    ? t.controlBorder
                                                    : mix(t.cardBg, t.warn, 55))));
    ui::fillRounded(dc, geo.rail, dp(list, 1), railColor);

    RECT labelRect = geo.label;
    ui::drawText(dc, row.label, labelRect, f.bodyBold, row.enabled ? t.text : t.muted, DT_LEFT | DT_WORDBREAK);

    RECT descRect = geo.description;
    ui::drawText(dc, row.description, descRect, f.caption, row.enabled ? t.subtext : t.muted,
                 DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

    if (row.control == LauncherControlKind::Checkbox) {
        ui::paintCheckbox(dc, geo.controlVisual, row.on, disabled, false, list->dpi);
    } else {
        ui::paintToggle(dc, geo.controlVisual, row.on, list->dpi);
    }

    if (!row.statusText.empty()) {
        RECT stRect = geo.status;
        const COLORREF textCol =
            row.statusTone == LauncherRowTone::Bad
                ? t.bad
                : (row.statusTone == LauncherRowTone::Warn
                       ? t.warn
                       : (row.statusTone == LauncherRowTone::Accent
                              ? t.accent
                              : (row.statusTone == LauncherRowTone::Good ? t.subtext : t.muted)));
        ui::drawText(dc, row.statusText, stRect, f.caption, textCol, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (row.statusTone != LauncherRowTone::Muted) {
            const int tw = ui::textWidth(dc, row.statusText, f.caption);
            const int dotX = stRect.right - tw - dp(list, 9);
            const int dotY = (stRect.top + stRect.bottom) / 2;
            if (dotX > stRect.left + dp(list, 4)) {
                ui::fillCircle(dc, dotX, dotY, dp(list, 3), toneColor(row.statusTone));
            }
        }
    }

    if (!row.actionLink.empty()) {
        RECT actRect = geo.action;
        ui::drawText(dc, row.actionLink, actRect, f.caption, t.accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    if (hasWarning) {
        ui::fillRounded(dc, geo.warningBox, dp(list, 5), mix(t.cardBg, t.warn, 14));
        ui::strokeRounded(dc, geo.warningBox, dp(list, 5), mix(t.cardBg, t.warn, 42));
        RECT warnRect = geo.warningText;
        ui::drawText(dc, row.warning, warnRect, f.caption, t.warn, DT_LEFT | DT_VCENTER | DT_WORDBREAK);
    }
}

void paint(List* list) {
    PAINTSTRUCT ps;
    HDC screen = BeginPaint(list->hwnd, &ps);
    RECT client{};
    GetClientRect(list->hwnd, &client);

    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    HBITMAP previous = static_cast<HBITMAP>(SelectObject(dc, bitmap));

    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    ui::fillRect(dc, client, t.cardBg);

    bool anyLauncherVisible = false;
    for (const LauncherToggleRow& r : list->launcherRows) {
        if (r.visible) { anyLauncherVisible = true; break; }
    }
    if ((!list->model && !anyLauncherVisible) || list->items.empty()) {
        RECT empty = client;
        empty.left += dp(list, kPad);
        empty.top += dp(list, kPad);
        ui::drawText(dc,
                     (list->model || anyLauncherVisible) ? L"Nothing matches that."
                                                         : L"Pick an install on the Setup tab first.",
                     empty, f.body, t.subtext, DT_LEFT | DT_WORDBREAK);
    }

    for (const Item& item : list->items) {
        RECT rect{client.left, dp(list, item.y) - list->scroll, client.right,
                  dp(list, item.y + item.height) - list->scroll};
        if (rect.bottom < 0 || rect.top > client.bottom) continue;

        if (item.kind == ItemKind::Header) {
            RECT text = rect;
            text.left += dp(list, kPad);
            text.top += dp(list, 12);
            ui::drawText(dc, item.title, text, f.heading, t.subtext, DT_LEFT | DT_SINGLELINE);
        } else if (item.kind == ItemKind::LauncherToggle) {
            paintLauncherToggleRow(list, dc, rect, list->launcherRows[item.row]);
            RECT line{rect.left + dp(list, kPad), rect.bottom - 1, rect.right - dp(list, kPad),
                      rect.bottom};
            ui::fillRect(dc, line, t.cardBorder);
        } else {
            paintRow(list, dc, rect, list->model->rows()[item.row]);
            RECT line{rect.left + dp(list, kPad), rect.bottom - 1, rect.right - dp(list, kPad),
                      rect.bottom};
            ui::fillRect(dc, line, t.cardBorder);
        }
    }

    ui::drawSlimScrollbar(dc, client, dp(list, list->contentHeight), client.bottom, list->scroll,
                          list->dpi, list->dragThumb);

    BitBlt(screen, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(list->hwnd, &ps);
}

// ---------------------------------------------------------------------------
// editing
// ---------------------------------------------------------------------------

void applyValue(List* list, size_t rowIndex, const std::string& value) {
    if (!list->model) return;
    // Child rows appear or leave with Anti-aliasing, and the frames row with Frame generation.
    // The click returns as soon as this write finishes, so rebuilding the
    // list here is the paint the next frame sees.
    const bool reshapes = rowIndex < list->model->rows().size() && list->model->rows()[rowIndex].def &&
                          (isVscreenKey(*list->model->rows()[rowIndex].def, "temporal_aa") ||
                           isVscreenKey(*list->model->rows()[rowIndex].def, "xess_frame_generation"));
    if (!list->model->set(rowIndex, value)) {
        list->error = list->model->lastError();
        MessageBoxW(GetParent(list->hwnd), fromUtf8(list->error).c_str(), kAppTitle,
                    MB_OK | MB_ICONWARNING);
    } else {
        list->error.clear();
        // Echoed outside the game folder on every successful write, not just
        // on install: the mirror exists so a game update between now and the
        // next install cannot take today's change with it.
        updateMirrorIni(list->model->gameDir(), list->mirrorDir);
        if (reshapes) {
            rebuildItems(list);
            updateScrollbar(list);
        }
    }
    InvalidateRect(list->hwnd, nullptr, TRUE);
}

void commitEdit(List* list) {
    if (list->editingRow == SIZE_MAX || !list->edit) return;
    wchar_t buffer[512]{};
    GetWindowTextW(list->edit, buffer, 512);
    const size_t row = list->editingRow;
    list->editingRow = SIZE_MAX;  // before applying: the write repaints
    if (!list->model) return;
    std::string wanted;
    // "30", "30%" and "0.3" all mean the same on a percentage setting, and
    // anything that is not a number at all is dropped rather than written.
    if (!list->model->rows()[row].parseTyped(buffer, &wanted)) return;
    if (wanted != list->model->rows()[row].value) applyValue(list, row, wanted);
}

LRESULT CALLBACK editProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                          DWORD_PTR ref) {
    List* list = reinterpret_cast<List*>(ref);
    switch (message) {
        case WM_KEYDOWN:
            if (wparam == VK_RETURN) {
                commitEdit(list);
                hideEdit(list);
                SetFocus(list->hwnd);
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                list->editingRow = SIZE_MAX;
                hideEdit(list);
                SetFocus(list->hwnd);
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            commitEdit(list);
            hideEdit(list);
            break;
        case WM_CHAR:
            // Swallow the beep an EDIT makes on Return.
            if (wparam == VK_RETURN || wparam == VK_ESCAPE) return 0;
            break;
        case WM_MOUSEWHEEL:
            // A single-line EDIT has nothing to scroll vertically, so it eats
            // the tick and the list underneath stays put. That is exactly what
            // "the mouse wheel does not work in the text box" is: the box is
            // open, it holds focus, and it silently swallows every tick. Hand
            // it to the list, which is what the cursor is really over.
            if (list && list->hwnd) {
                return SendMessageW(list->hwnd, message, wparam, lparam);
            }
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void beginEdit(List* list, size_t rowIndex, const RECT& box) {
    if (!list->edit) {
        list->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10,
                                     list->hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)),
                                     nullptr, nullptr);
        SetWindowSubclass(list->edit, editProc, 1, reinterpret_cast<DWORD_PTR>(list));
    }
    SendMessageW(list->edit, WM_SETFONT, reinterpret_cast<WPARAM>(ui::fonts().body), TRUE);
    const std::wstring value = list->model->rows()[rowIndex].shown();
    SetWindowTextW(list->edit, value.c_str());
    // Sized to the text and centred in the value box: a single-line EDIT
    // draws its text at the top of the control, so a control that fills
    // the box shows the text riding high.
    const int textH = ui::textHeightPx(ui::fonts().body);
    const int boxH = box.bottom - box.top;
    const int top = box.top + (boxH > textH ? (boxH - textH) / 2 : 0);
    MoveWindow(list->edit, box.left + dp(list, 6), top,
               (box.right - box.left) - dp(list, 12), textH, TRUE);
    ShowWindow(list->edit, SW_SHOW);
    SetFocus(list->edit);
    SendMessageW(list->edit, EM_SETSEL, 0, -1);
    list->editingRow = rowIndex;
}

// While the dropdown list is closed the combo has nothing to scroll, and it
// would swallow the wheel the way the text box used to. The open list keeps it.
LRESULT CALLBACK choiceProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                            DWORD_PTR ref) {
    List* list = reinterpret_cast<List*>(ref);
    if (message == WM_MOUSEWHEEL && list && list->hwnd) {
        COMBOBOXINFO info{};
        info.cbSize = sizeof(info);
        const bool dropped =
            GetComboBoxInfo(hwnd, &info) && info.hwndList && IsWindowVisible(info.hwndList);
        if (!dropped) return SendMessageW(list->hwnd, message, wparam, lparam);
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void ensureChoice(List* list) {
    if (list->choice) return;
    list->choice = CreateWindowExW(
        0, L"COMBOBOX", L"",
        WS_CHILD | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 10, 10, list->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChoiceId)),
        nullptr, nullptr);
    if (!list->choice) return;
    SetWindowSubclass(list->choice, choiceProc, 1, reinterpret_cast<DWORD_PTR>(list));
    if (ui::theme().dark) SetWindowTheme(list->choice, L"DarkMode_CFD", nullptr);
}

// The closed field is the row's control. The window height of a comctl v6
// combo is that field; the list itself is a popup.
void fitChoiceField(List* list, const RECT& box) {
    const int field = box.bottom - box.top;
    const int width = box.right - box.left;
    MoveWindow(list->choice, box.left, box.top, width, field, FALSE);
    SendMessageW(list->choice, CB_SETITEMHEIGHT, 0, dp(list, 24));
    SendMessageW(list->choice, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), dp(list, 22));
    RECT rc{};
    GetWindowRect(list->choice, &rc);
    const int selection = static_cast<int>(
        SendMessageW(list->choice, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0));
    const int frame = (rc.bottom - rc.top) - selection;
    const int item = field - (frame > 0 ? frame : 0);
    if (item > 0) {
        SendMessageW(list->choice, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), item);
    }
    MoveWindow(list->choice, box.left, box.top, width, field, TRUE);
}

std::string selectedChoiceValue(const List* list) {
    if (!list->choice || list->choiceRow == SIZE_MAX || !list->model) return std::string();
    if (list->choiceRow >= list->model->rows().size()) return std::string();
    const LRESULT sel = SendMessageW(list->choice, CB_GETCURSEL, 0, 0);
    if (sel < 0) return std::string();
    const LRESULT data = SendMessageW(list->choice, CB_GETITEMDATA, static_cast<WPARAM>(sel), 0);
    if (data == CB_ERR) return std::string();
    const std::vector<Choice>& choices = list->model->rows()[list->choiceRow].choices;
    if (data < 0 || static_cast<size_t>(data) >= choices.size()) return std::string();
    return choices[static_cast<size_t>(data)].value;
}

void closeChoice(List* list, bool apply) {
    if (list->choiceClosing) return;
    if (!list->choice || list->choiceRow == SIZE_MAX) return;
    list->choiceClosing = true;
    const size_t row = list->choiceRow;
    const std::string original = list->choiceOriginal;
    const bool cancel = list->choiceCancel;
    const std::string selected = apply && !cancel ? selectedChoiceValue(list) : std::string();
    list->choiceRow = SIZE_MAX;
    list->choiceOriginal.clear();
    list->choiceCancel = false;
    if (GetFocus() == list->choice) SetFocus(list->hwnd);
    ShowWindow(list->choice, SW_HIDE);
    list->choiceClosing = false;
    InvalidateRect(list->hwnd, nullptr, TRUE);
    if (apply && !cancel && !selected.empty() && selected != original) applyValue(list, row, selected);
}

void openChoice(List* list, size_t rowIndex, const RECT& box) {
    if (!list->model || rowIndex >= list->model->rows().size()) return;
    const SettingRow& row = list->model->rows()[rowIndex];
    ensureChoice(list);
    if (!list->choice) return;
    SendMessageW(list->choice, WM_SETFONT, reinterpret_cast<WPARAM>(ui::fonts().body), TRUE);
    list->choiceFilling = true;
    list->choiceCancel = false;
    SendMessageW(list->choice, CB_RESETCONTENT, 0, 0);
    int selected = -1;
    for (size_t i = 0; i < row.choices.size(); ++i) {
        const std::wstring label = fromUtf8(row.choices[i].label);
        const LRESULT index =
            SendMessageW(list->choice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (index < 0) continue;
        SendMessageW(list->choice, CB_SETITEMDATA, static_cast<WPARAM>(index),
                     static_cast<LPARAM>(i));
        if (row.choices[i].value == row.value) selected = static_cast<int>(index);
    }
    if (selected >= 0) SendMessageW(list->choice, CB_SETCURSEL, selected, 0);
    list->choiceOriginal = row.value;
    list->choiceRow = rowIndex;
    list->choiceFilling = false;

    int dropped = box.right - box.left;
    HDC dc = GetDC(list->choice);
    for (const Choice& choice : row.choices) {
        const int width = ui::textWidth(dc, fromUtf8(choice.label), ui::fonts().body) + dp(list, 40);
        if (width > dropped) dropped = width;
    }
    ReleaseDC(list->choice, dc);
    SendMessageW(list->choice, CB_SETDROPPEDWIDTH, dropped, 0);

    // Placed while hidden, then shown, then measured. A hidden combo has not
    // built its frame, and showing it at 0,0 first would flash in the corner.
    MoveWindow(list->choice, box.left, box.top, box.right - box.left, box.bottom - box.top, FALSE);
    ShowWindow(list->choice, SW_SHOWNA);
    fitChoiceField(list, box);
    SetFocus(list->choice);
    SendMessageW(list->choice, CB_SHOWDROPDOWN, TRUE, 0);
}

// The choice control under the pointer, when that row is a dropdown.
bool dropdownAt(List* list, int x, int y, size_t* rowIndex, RECT* box) {
    if (!list->model) return false;
    RECT client{};
    GetClientRect(list->hwnd, &client);
    HDC dc = GetDC(list->hwnd);
    bool found = false;
    for (const Item& item : list->items) {
        if (item.kind != ItemKind::Setting) continue;
        RECT rect{client.left, dp(list, item.y) - list->scroll, client.right,
                  dp(list, item.y + item.height) - list->scroll};
        if (y < rect.top || y >= rect.bottom) continue;
        const SettingRow& row = list->model->rows()[item.row];
        if (!row.def || row.def->kind != SettingKind::Choice) break;
        if (twoChoiceToggle(row, nullptr, nullptr)) break;
        const RowGeometry geo = geometryFor(list, rect);
        if (!choiceUsesDropdown(list, dc, geo, row)) break;
        const POINT point{x, y};
        if (!PtInRect(&geo.control, point)) break;
        if (rowIndex) *rowIndex = item.row;
        if (box) *box = geo.control;
        found = true;
        break;
    }
    ReleaseDC(list->hwnd, dc);
    return found;
}

// The row whose link is under the pointer, if any. The click and the cursor
// both ask this one question, so what is clickable can never drift from what
// was drawn -- and the quiet text beside the link is no longer part of it.
bool launcherLinkHit(List* list, int x, int y, size_t* rowIndex = nullptr) {
    RECT client{};
    GetClientRect(list->hwnd, &client);
    for (const Item& item : list->items) {
        if (item.kind != ItemKind::LauncherToggle) continue;
        const LauncherToggleRow& row = list->launcherRows[item.row];
        if (!row.visible || row.actionLink.empty()) continue;
        RECT rect{client.left, dp(list, item.y) - list->scroll, client.right,
                  dp(list, item.y + item.height) - list->scroll};
        if (y < rect.top || y >= rect.bottom) continue;
        HDC dc = GetDC(list->hwnd);
        const RECT box = launcherActionRect(list, dc, launcherGeometryFor(list, rect, row), row.actionLink);
        ReleaseDC(list->hwnd, dc);
        const POINT point{x, y};
        if (PtInRect(&box, point) != 0) {
            if (rowIndex) *rowIndex = item.row;
            return true;
        }
    }
    return false;
}

bool launcherControlHit(List* list, int x, int y) {
    RECT client{};
    GetClientRect(list->hwnd, &client);
    const POINT point{x, y};
    for (const Item& item : list->items) {
        if (item.kind != ItemKind::LauncherToggle) continue;
        const LauncherToggleRow& row = list->launcherRows[item.row];
        if (!row.visible || !row.enabled || row.busy) continue;
        RECT rect{client.left, dp(list, item.y) - list->scroll, client.right,
                  dp(list, item.y + item.height) - list->scroll};
        if (y < rect.top || y >= rect.bottom) continue;
        const LauncherRowGeo geo = launcherGeometryFor(list, rect, row);
        if (PtInRect(&geo.controlHit, point) != 0) return true;
    }
    return false;
}

bool linkHit(List* list, int x, int y, size_t* rowIndex) {
    if (!list->model) return false;
    RECT client{};
    GetClientRect(list->hwnd, &client);
    for (const Item& item : list->items) {
        if (item.kind != ItemKind::Setting) continue;
        RECT rect{client.left, dp(list, item.y) - list->scroll, client.right,
                  dp(list, item.y + item.height) - list->scroll};
        if (y < rect.top || y >= rect.bottom) continue;
        const SettingRow& row = list->model->rows()[item.row];
        const std::wstring label = recommendLink(row);
        if (label.empty()) return false;
        HDC dc = GetDC(list->hwnd);
        const RECT box = recommendLinkRect(list, dc, geometryFor(list, rect), label);
        ReleaseDC(list->hwnd, dc);
        const POINT point{x, y};
        if (!PtInRect(&box, point)) return false;
        if (rowIndex) *rowIndex = item.row;
        return true;
    }
    return false;
}

void onClick(List* list, int x, int y) {
    closeChoice(list, true);
    list->choicePress = false;
    commitEdit(list);
    hideEdit(list);

    size_t launcherLinkRow = 0;
    if (launcherLinkHit(list, x, y, &launcherLinkRow)) {
        if (list->launcherRows[launcherLinkRow].onAction) list->launcherRows[launcherLinkRow].onAction();
        return;
    }

    size_t linkRow = 0;
    if (linkHit(list, x, y, &linkRow)) {
        applyValue(list, linkRow, list->model->rows()[linkRow].def->recommended);
        return;
    }

    size_t dropRow = 0;
    RECT dropBox{};
    if (dropdownAt(list, x, y, &dropRow, &dropBox)) {
        list->choicePress = true;
        return;
    }

    RECT client{};
    GetClientRect(list->hwnd, &client);

    for (const Item& item : list->items) {
        if (item.kind == ItemKind::Header) continue;
        RECT rect{client.left, dp(list, item.y) - list->scroll, client.right,
                  dp(list, item.y + item.height) - list->scroll};
        if (y < rect.top || y >= rect.bottom) continue;

        if (item.kind == ItemKind::LauncherToggle) {
            const LauncherToggleRow& row = list->launcherRows[item.row];
            const LauncherRowGeo geo = launcherGeometryFor(list, rect, row);
            const POINT point{x, y};
            if (row.enabled && !row.busy && PtInRect(&geo.controlHit, point) && row.onToggle) {
                row.onToggle(!row.on);
            }
            return;
        }
        if (!list->model) return;

        const SettingRow& row = list->model->rows()[item.row];
        const RowGeometry geo = geometryFor(list, rect);
        POINT point{x, y};

        switch (row.def->kind) {
            case SettingKind::Toggle: {
                const RECT box = toggleRect(list, geo);
                if (PtInRect(&box, point)) applyValue(list, item.row, isOn(row.value) ? "0" : "1");
                return;
            }
            case SettingKind::Choice: {
                std::string onValue, offValue;
                if (twoChoiceToggle(row, &onValue, &offValue)) {
                    const RECT box = toggleRect(list, geo);
                    if (PtInRect(&box, point)) {
                        applyValue(list, item.row,
                                   row.value == onValue ? offValue : onValue);
                    }
                    return;
                }
                HDC dc = GetDC(list->hwnd);
                const bool dropdown = choiceUsesDropdown(list, dc, geo, row);
                ReleaseDC(list->hwnd, dc);
                if (dropdown) return;
                const std::vector<RECT> segments = segmentsFor(list, geo, row.choices.size());
                for (size_t i = 0; i < segments.size(); ++i) {
                    if (PtInRect(&segments[i], point)) {
                        applyValue(list, item.row, row.choices[i].value);
                        return;
                    }
                }
                return;
            }
            default: {
                const RECT box = valueRect(list, geo);
                if (PtInRect(&box, point)) beginEdit(list, item.row, box);
                return;
            }
        }
    }
}

LRESULT CALLBACK listProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    List* list = listOf(hwnd);
    switch (message) {
        case WM_NCCREATE: {
            List* created = new List();
            created->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            break;
        }
        case WM_PAINT:
            if (list) paint(list);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (list) {
                rebuildItems(list);
                updateScrollbar(list);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            if (!list) break;
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            scrollTo(list, list->scroll - delta * dp(list, 40) / WHEEL_DELTA);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (list) {
                SetFocus(hwnd);
                const int x = GET_X_LPARAM(lparam);
                const int y = GET_Y_LPARAM(lparam);
                RECT client{};
                GetClientRect(hwnd, &client);
                const int content = dp(list, list->contentHeight);
                const RECT thumb = ui::slimThumb(client, content, client.bottom, list->scroll,
                                                 list->dpi);
                // The right gutter belongs to the slim scrollbar whenever
                // there is one: on the thumb starts a drag, on the track
                // jumps the thumb to the pointer.
                if (thumb.right > thumb.left && x >= client.right - dp(list, 12)) {
                    list->choicePress = false;
                    if (y >= thumb.top && y < thumb.bottom) {
                        list->dragThumb = true;
                        list->dragOffset = y - thumb.top;
                    } else {
                        const int half = (thumb.bottom - thumb.top) / 2;
                        scrollTo(list, ui::slimPosFromThumbTop(client, content, client.bottom,
                                                               y - half, list->dpi));
                        list->dragThumb = true;
                        list->dragOffset = half;
                    }
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;
                }
                onClick(list, x, y);
                if (list->choicePress) SetCapture(hwnd);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (list && list->dragThumb) {
                RECT client{};
                GetClientRect(hwnd, &client);
                const int content = dp(list, list->contentHeight);
                scrollTo(list, ui::slimPosFromThumbTop(client, content, client.bottom,
                                                       GET_Y_LPARAM(lparam) - list->dragOffset,
                                                       list->dpi));
            }
            return 0;
        case WM_LBUTTONUP:
            if (list && list->choicePress) {
                list->choicePress = false;
                const int x = GET_X_LPARAM(lparam);
                const int y = GET_Y_LPARAM(lparam);
                if (GetCapture() == hwnd) ReleaseCapture();
                size_t row = 0;
                RECT box{};
                if (dropdownAt(list, x, y, &row, &box)) openChoice(list, row, box);
                return 0;
            }
            if (list && list->dragThumb) {
                list->dragThumb = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (list && list->choicePress) list->choicePress = false;
            if (list && list->dragThumb) {
                list->dragThumb = false;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_COMMAND:
            if (!list || LOWORD(wparam) != kChoiceId) break;
            if (list->choiceFilling || list->choiceClosing) return 0;
            if (HIWORD(wparam) == CBN_SELENDCANCEL) list->choiceCancel = true;
            if (HIWORD(wparam) == CBN_CLOSEUP) closeChoice(list, true);
            return 0;
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (list && item->CtlID == kChoiceId) {
                ui::drawComboItem(item, list->dpi);
                return TRUE;
            }
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (list && measure->CtlID == kChoiceId) {
                measure->itemHeight = static_cast<UINT>(dp(list, 24));
                return TRUE;
            }
            break;
        }
        case WM_SHOWWINDOW:
            if (list && wparam == FALSE) {
                closeChoice(list, true);
                list->choicePress = false;
            }
            break;
        case WM_SETCURSOR:
            // The hand over a row's link. Colour alone did not say "click me":
            // the field read one as a label, and reasonably -- it sat under a
            // switch and named a value.
            if (list && LOWORD(lparam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(hwnd, &point);
                if (launcherLinkHit(list, point.x, point.y) || launcherControlHit(list, point.x, point.y) ||
                    linkHit(list, point.x, point.y, nullptr)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        case WM_CTLCOLOREDIT: {
            const ui::Theme& t = ui::theme();
            SetTextColor(reinterpret_cast<HDC>(wparam), t.text);
            SetBkColor(reinterpret_cast<HDC>(wparam), t.control);
            static HBRUSH brush = nullptr;
            static COLORREF colour = 0;
            if (!brush || colour != t.control) {
                if (brush) DeleteObject(brush);
                brush = CreateSolidBrush(t.control);
                colour = t.control;
            }
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_DESTROY:
            if (list) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                delete list;
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

HWND createSettingsList(HWND parent, int id, UINT dpi) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = listProc;
        cls.hInstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = kClassName;
        cls.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&cls);
        registered = true;
    }
    HWND hwnd = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_TABSTOP | WS_CLIPCHILDREN, 0, 0, 10,
                                10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                nullptr, nullptr);
    if (hwnd) {
        List* list = listOf(hwnd);
        if (list) list->dpi = dpi;
    }
    return hwnd;
}

void settingsListSetModel(HWND hwnd, SettingsModel* model, const std::wstring& mirrorDir) {
    List* list = listOf(hwnd);
    if (!list) return;
    closeChoice(list, true);
    list->choicePress = false;
    list->model = model;
    list->mirrorDir = mirrorDir;
    list->scroll = 0;
    rebuildItems(list);
    updateScrollbar(list);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void settingsListSetLauncherToggles(HWND hwnd, std::vector<LauncherToggleRow> rows) {
    List* list = listOf(hwnd);
    if (!list) return;
    bool geometryChanged = list->launcherRows.size() != rows.size();
    if (!geometryChanged) {
        for (size_t i = 0; i < rows.size(); ++i) {
            const LauncherToggleRow& a = list->launcherRows[i];
            const LauncherToggleRow& b = rows[i];
            if (a.visible != b.visible || a.group != b.group || a.control != b.control ||
                a.warning.empty() != b.warning.empty()) {
                geometryChanged = true;
                break;
            }
        }
    }
    list->launcherRows = std::move(rows);
    if (geometryChanged) {
        rebuildItems(list);
        updateScrollbar(list);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

void settingsListSetLauncherToggle(HWND hwnd, const LauncherToggleRow& row) {
    std::vector<LauncherToggleRow> rows;
    if (row.visible) rows.push_back(row);
    settingsListSetLauncherToggles(hwnd, std::move(rows));
}

void settingsListSetFilter(HWND hwnd, const std::wstring& needle) {
    List* list = listOf(hwnd);
    if (!list) return;
    closeChoice(list, true);
    list->choicePress = false;
    commitEdit(list);
    hideEdit(list);
    list->filter = needle;
    list->scroll = 0;
    rebuildItems(list);
    updateScrollbar(list);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void settingsListRescale(HWND hwnd, UINT dpi) {
    List* list = listOf(hwnd);
    if (!list) return;
    closeChoice(list, true);
    list->choicePress = false;
    list->dpi = dpi;
    rebuildItems(list);
    updateScrollbar(list);
    InvalidateRect(hwnd, nullptr, TRUE);
}

const std::string& settingsListLastError(HWND hwnd) {
    static const std::string none;
    List* list = listOf(hwnd);
    return list ? list->error : none;
}

}  // namespace edvr::installer
