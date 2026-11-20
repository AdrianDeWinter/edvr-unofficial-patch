#include "launcher_login_dialog.h"

#include "frontier_login.h"
#include "ui.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

enum class Kind { Login, Code };

enum : int { kIdOk = 1, kIdCancel = 2, kIdRemember = 3, kIdEmail = 10, kIdPassword = 11, kIdCode = 12 };

constexpr int kClientWidth = 420;
constexpr int kPad = 24;

struct State {
    Kind kind = Kind::Login;
    UINT dpi = 96;
    bool underWine = false;
    std::wstring title, body, note, privacy;
    HWND window = nullptr, email = nullptr, password = nullptr, code = nullptr, remember = nullptr, ok = nullptr, cancel = nullptr;
    int clientHeight = 0;
    bool finished = false;
    bool accepted = false;
    LoginForm form;
    std::string codeText;
};

int dp(const State& s, int v) { return ui::dp(v, s.dpi); }

RECT unitsRect(const State& s, int left, int top, int right, int bottom) { return RECT{dp(s, left), dp(s, top), dp(s, right), dp(s, bottom)}; }

// Where each part sits, in design units. The two dialogs share a column; the login has two boxes and a
// tick box, the code one box.
struct Layout {
    RECT title, body, note, privacy;
    RECT labelA, frameA, labelB, frameB;
    RECT remember, ok, cancel;
    int height = 0;
};

Layout layoutFor(const State& s) {
    Layout l{};
    const int right = kClientWidth - kPad;
    l.title = unitsRect(s, kPad, 16, right, 44);
    l.note = unitsRect(s, kPad, 46, right, 82);
    if (s.kind == Kind::Login) {
        l.labelA = unitsRect(s, kPad, 90, right, 108);
        l.frameA = unitsRect(s, kPad, 110, right, 142);
        l.labelB = unitsRect(s, kPad, 152, right, 170);
        l.frameB = unitsRect(s, kPad, 172, right, 204);
        l.remember = unitsRect(s, kPad, 214, right, 236);
        l.privacy = unitsRect(s, kPad, 244, right, 286);
        l.height = 340;
        l.ok = unitsRect(s, right - 208, 296, right - 98, 330);
        l.cancel = unitsRect(s, right - 90, 296, right, 330);
    } else {
        l.body = unitsRect(s, kPad, 46, right, 84);
        l.note = unitsRect(s, kPad, 86, right, 106);
        l.labelA = unitsRect(s, kPad, 114, right, 132);
        l.frameA = unitsRect(s, kPad, 134, right, 166);
        l.height = 226;
        l.ok = unitsRect(s, right - 208, 180, right - 98, 214);
        l.cancel = unitsRect(s, right - 90, 180, right, 214);
    }
    return l;
}

void placeEdit(const State& s, HWND edit, const RECT& frame) {
    const int textHeight = ui::textHeightPx(ui::fonts().body);
    const int inset = dp(s, 10);
    const int top = frame.top + ((frame.bottom - frame.top) - textHeight) / 2;
    MoveWindow(edit, frame.left + inset, top, (frame.right - frame.left) - 2 * inset, textHeight, TRUE);
}

void placeButton(HWND button, const RECT& r) { MoveWindow(button, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE); }

std::string readEdit(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int got = GetWindowTextW(edit, &text[0], length + 1);
    text.resize(static_cast<size_t>(got > 0 ? got : 0));
    const std::string utf8 = narrowUtf8(text);
    SecureZeroMemory(&text[0], text.size() * sizeof(wchar_t));
    return utf8;
}

std::string trimmed(std::string text) {
    const char* space = " \t\r\n";
    const size_t first = text.find_first_not_of(space);
    if (first == std::string::npos) return std::string();
    const size_t last = text.find_last_not_of(space);
    return text.substr(first, last - first + 1);
}

// The Sign in or Continue button is available only when the form could be submitted.
void refreshOk(State* s) {
    if (!s->ok) return;  // setting the first email text, while the controls are being made
    bool complete;
    if (s->kind == Kind::Login) {
        LoginForm f;
        f.email = trimmed(readEdit(s->email));
        f.password = readEdit(s->password);
        complete = loginFormComplete(f);
        wipeString(&f.password);
    } else {
        complete = !trimmed(readEdit(s->code)).empty();
    }
    EnableWindow(s->ok, complete);
}

void say(State* s, const std::wstring& note) {
    s->note = note;
    InvalidateRect(s->window, nullptr, FALSE);
}

// Gives a box the keyboard, when the window is one the user can see. SetFocus activates the window it is in, which a
// window nobody sees (the rig's) must not do.
void focusBox(State* s, HWND box) {
    if (IsWindowVisible(s->window)) SetFocus(box);
}

void finish(State* s, bool accepted) {
    s->accepted = accepted;
    s->finished = true;
}

// Sign in, Continue, and Return. An incomplete form is never submitted: the empty box is named and given the focus.
void submit(State* s) {
    if (s->kind == Kind::Login) {
        LoginForm f;
        f.email = trimmed(readEdit(s->email));
        f.password = readEdit(s->password);
        f.remember = ui::checkboxChecked(s->remember);
        if (!loginFormComplete(f)) {
            wipeString(&f.password);
            say(s, L"Enter both the email and the password.");
            focusBox(s, f.email.empty() ? s->email : s->password);
            return;
        }
        s->form = f;
        wipeString(&f.password);
    } else {
        s->codeText = trimmed(readEdit(s->code));
        if (s->codeText.empty()) {
            say(s, L"Type the code from the email.");
            focusBox(s, s->code);
            return;
        }
    }
    finish(s, true);
}

void paint(State* s) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(s->window, &ps);
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    RECT client;
    GetClientRect(s->window, &client);
    ui::fillRect(dc, client, t.windowBg);
    const Layout l = layoutFor(*s);
    ui::drawText(dc, s->title, l.title, f.heading, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (s->kind == Kind::Code) ui::drawText(dc, s->body, l.body, f.body, t.text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    if (!s->note.empty()) ui::drawText(dc, s->note, l.note, f.body, t.warn, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    const int radius = dp(*s, 6);
    for (const RECT* frame : {&l.frameA, &l.frameB}) {
        if (s->kind == Kind::Code && frame == &l.frameB) continue;
        ui::fillRounded(dc, *frame, radius, t.control);
        ui::strokeRounded(dc, *frame, radius, t.controlBorder);
    }
    if (s->kind == Kind::Login) {
        ui::drawText(dc, L"Email", l.labelA, f.caption, t.subtext, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
        ui::drawText(dc, L"Password", l.labelB, f.caption, t.subtext, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
        ui::drawText(dc, s->privacy, l.privacy, f.caption, t.subtext, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    } else {
        ui::drawText(dc, L"Code", l.labelA, f.caption, t.subtext, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    }
    EndPaint(s->window, &ps);
}

HWND makeEdit(HWND parent, int id, bool masked) {
    HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (masked ? ES_PASSWORD : 0), 0, 0, 10, 10,
                                parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    if (edit) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(ui::fonts().body), TRUE);
    return edit;
}

void createControls(State* s) {
    const ui::Fonts& f = ui::fonts();
    const Layout l = layoutFor(*s);
    if (s->kind == Kind::Login) {
        s->email = makeEdit(s->window, kIdEmail, false);
        s->password = makeEdit(s->window, kIdPassword, true);
        s->remember = ui::makeCheckbox(s->window, L"Remember my password on this PC", kIdRemember, s->form.remember, f.body);
        placeEdit(*s, s->email, l.frameA);
        placeEdit(*s, s->password, l.frameB);
        MoveWindow(s->remember, l.remember.left, l.remember.top, l.remember.right - l.remember.left, l.remember.bottom - l.remember.top, TRUE);
        SetWindowTextW(s->email, widenUtf8(s->form.email).c_str());
    } else {
        s->code = makeEdit(s->window, kIdCode, false);
        placeEdit(*s, s->code, l.frameA);
    }
    s->ok = ui::makeButton(s->window, s->kind == Kind::Login ? L"Sign in" : L"Continue", kIdOk, ui::ButtonStyle::Primary, f.body);
    s->cancel = ui::makeButton(s->window, L"Cancel", kIdCancel, ui::ButtonStyle::Secondary, f.body);
    placeButton(s->ok, l.ok);
    placeButton(s->cancel, l.cancel);
    refreshOk(s);
}

HBRUSH editBrush() {
    static HBRUSH brush = nullptr;
    static COLORREF colour = 0;
    const COLORREF wanted = ui::theme().control;
    if (!brush || colour != wanted) {
        if (brush) DeleteObject(brush);
        brush = CreateSolidBrush(wanted);
        colour = wanted;
    }
    return brush;
}

LRESULT CALLBACK dialogProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    State* s = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }
        case WM_CREATE:
            if (s) {
                s->window = window;
                createControls(s);
            }
            return 0;
        case WM_PAINT:
            if (s) paint(s);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM:
            if (s && ui::drawOwnerDrawn(reinterpret_cast<const DRAWITEMSTRUCT*>(lparam), s->dpi)) return TRUE;
            break;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, ui::theme().text);
            SetBkColor(dc, ui::theme().control);
            return reinterpret_cast<LRESULT>(editBrush());
        }
        case WM_COMMAND:
            if (!s) break;
            if (LOWORD(wparam) == kIdOk && HIWORD(wparam) == BN_CLICKED) {
                submit(s);
                return 0;
            }
            if (LOWORD(wparam) == kIdCancel && HIWORD(wparam) == BN_CLICKED) {
                finish(s, false);
                return 0;
            }
            if (LOWORD(wparam) == kIdRemember && HIWORD(wparam) == BN_CLICKED) {
                ui::toggleCheckbox(s->remember);
                return 0;
            }
            if (HIWORD(wparam) == EN_CHANGE) {
                if (!s->note.empty()) say(s, std::wstring());
                refreshOk(s);
                return 0;
            }
            break;
        case WM_CLOSE:
            if (s) finish(s, false);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool ensureClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = dialogProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = nullptr;
    cls.lpszClassName = kFrontierDialogClass;
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    registered = true;
    return true;
}

// Runs the dialog to its end. False when it could not be made, which is a cancel.
bool run(State* s, const FrontierDialogOptions& options) {
    if (!ensureClass()) return false;
    s->dpi = options.dpi ? options.dpi : 96;
    s->underWine = options.underWine;
    const Layout l = layoutFor(*s);
    s->clientHeight = l.height;
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    // A window nobody sees (the rig's) must not take the focus either.
    const DWORD exStyle = WS_EX_DLGMODALFRAME | (options.visible ? 0 : WS_EX_NOACTIVATE);
    RECT size{0, 0, dp(*s, kClientWidth), dp(*s, l.height)};
    AdjustWindowRectEx(&size, style, FALSE, exStyle);
    const int width = size.right - size.left, height = size.bottom - size.top;
    RECT around{};
    if (options.owner && GetWindowRect(options.owner, &around)) {
        // centred on the launcher's window
    } else {
        around = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int x = around.left + ((around.right - around.left) - width) / 2;
    const int y = around.top + ((around.bottom - around.top) - height) / 2;
    HWND window = CreateWindowExW(exStyle, kFrontierDialogClass, s->title.c_str(), style, x, y, width, height, options.owner, nullptr,
                                  GetModuleHandleW(nullptr), s);
    if (!window) return false;
    ui::applyWindowChrome(window);

    if (options.owner) EnableWindow(options.owner, FALSE);
    if (options.visible) {
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        // The box the user types in first: the email, or the password when the email is already known.
        SetFocus(s->kind == Kind::Login ? (s->form.email.empty() ? s->email : s->password) : s->code);
    }

    MSG message;
    while (!s->finished) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(message.wParam));  // not ours to swallow
            break;
        }
        const bool inside = message.hwnd == window || IsChild(window, message.hwnd);
        if (inside && message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            finish(s, false);
            continue;
        }
        if (inside && message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
            // Return on Cancel is Cancel; anywhere else it is Sign in, which refuses an empty form.
            if (GetFocus() == s->cancel) finish(s, false);
            else submit(s);
            continue;
        }
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // Nothing typed stays in a control a moment longer than the window.
    for (HWND edit : {s->email, s->password, s->code}) {
        if (edit) SetWindowTextW(edit, L"");
    }
    if (options.owner) EnableWindow(options.owner, TRUE);
    DestroyWindow(window);
    if (options.owner) SetForegroundWindow(options.owner);
    return true;
}

}  // namespace

bool showFrontierLogin(const FrontierDialogOptions& options, const std::string& note, LoginForm* form) {
    State s;
    s.kind = Kind::Login;
    s.title = L"Sign in to Frontier";
    s.note = widenUtf8(note);
    s.privacy = widenUtf8(loginPrivacyLine(options.underWine));
    s.form.email = form->email;
    s.form.remember = form->remember;
    const bool ran = run(&s, options);
    const bool accepted = ran && s.accepted;
    if (accepted) {
        *form = s.form;
    }
    wipeString(&s.form.password);
    return accepted;
}

bool showFrontierCode(const FrontierDialogOptions& options, const std::string& email, const std::string& note, std::string* code) {
    State s;
    s.kind = Kind::Code;
    s.title = L"Enter the code from Frontier";
    s.body = L"Frontier emailed a verification code to " + widenUtf8(email) + L".";
    s.note = widenUtf8(note);
    const bool accepted = run(&s, options) && s.accepted;
    if (accepted) *code = s.codeText;
    return accepted;
}

}  // namespace edvr::installer
