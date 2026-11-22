#include "hotkey.h"

#include <atomic>
#include <cctype>
#include <string>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "pad_names.h"

namespace edvr {

namespace {

void hotkeyLogNote(const char* line) { Log::get().note("%s", line); }

struct HotkeyNoteSinkRegistrar {
    HotkeyNoteSinkRegistrar() { hotkeySetNoteSink(&hotkeyLogNote); }
} g_hotkeyNoteSinkRegistrar;

}  // namespace

// Does the foreground window belong to this process?
//
// GetAsyncKeyState is global: it reports the key whoever is typing, in
// whatever application. Without this check, Scroll Lock pressed in a browser
// toggled the brightness fix and Pause wrote a camera dump.
//
// THE CHECK IS NO LONGER APPLIED TO EVERY BINDING, and the line it is drawn
// along is whose key it is (2026-08-16):
//
//   * EDVR's OWN controls -- the exposure toggle, the history dump -- keep
//     it. The game does nothing with those keys, so there is no game state to
//     stay in step with and a press typed elsewhere is pure noise.
//
//   * Bindings ADOPTED FROM ELITE'S OWN CONFIGURATION do not. There EDVR is
//     mirroring the game's reading of a key, and Elite takes its input
//     through DirectInput, which keeps delivering while its window is not
//     foreground. Requiring focus made EDVR disagree with the game about
//     which mode the player was in: measured 2026-08-16, a session where the
//     camera key was pressed, the flat panel stopped, the stereo scene came
//     up, and the gate sat at `intent=CLEAR, pressed not yet this session`
//     while it watched the player cycle presets. Worse than losing one entry
//     -- the camera key is a TOGGLE, so a swallowed press inverts every press
//     after it.
//
// The stray-key exposure that reopens is bounded ON ENTRY by the gate rather
// than by this check: intent alone arms nothing, because arming also needs
// the flat panel to have stopped and a stereo scene to be drawing. A camera
// key pressed in a browser while the on-foot screen is up changes nothing
// visible.
//
// EXIT IS NOT BOUNDED THE SAME WAY, and the first version of this note got
// that wrong by claiming a stray press was "recoverable by pressing again".
// It is not, within one camera stay: the exit path needs no render evidence
// at all, and gatePanelRun is zeroed after 90 panel-less frames, so pressing
// again sets intent but cannot re-enter -- entry wants gatePanelRun > 30 and
// only the flat panel returning rebuilds it. A genuine exit self-heals
// because the panel does come back; a stray press while still in the camera
// costs the offset for the rest of that stay. That is the real price of this
// decision, it is larger than the price of the bug it replaces, and it is
// worth fixing in the gate rather than papering over here.
//
// Controllers inherit the right answer for free: an adopted binding is
// game-mirrored by definition, and neither XInput nor DirectInput has a
// concept of focus to consult.
static bool gameHasFocus() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// The keyboard. GetAsyncKeyState, unless a rig has put a reader of its own in (hotkey.h).
static HotkeyKeyDownFn g_keyReader = nullptr;
void hotkeySetKeyboardReaderForTest(HotkeyKeyDownFn fn) { g_keyReader = fn; }
static bool rawKeyDown(int vk) {
    return g_keyReader ? g_keyReader(vk) : (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Which modifiers are physically down right now.
static uint32_t heldMods() {
    uint32_t m = 0;
    if (rawKeyDown(VK_CONTROL)) m |= kHotkeyCtrl;
    if (rawKeyDown(VK_MENU)) m |= kHotkeyAlt;
    if (rawKeyDown(VK_SHIFT)) m |= kHotkeyShift;
    return m;
}

// SUBSET, not equality: the binding's modifiers must be held, and extra ones
// are allowed.
//
// Equality was the first attempt and it is wrong for the job. These are the
// player's OWN Elite bindings, and EDVR is trying to see the same press the
// game sees -- while the player is holding Shift to sprint, or Ctrl for
// something else. Under equality, CTRL+ALT+SPACE with Shift also down does not
// match, EDVR misses the press, and the intent toggle desynchronises for the
// session. Missing a press is the expensive failure here; that is the whole
// class of bug this gate has been chasing.
//
// It also silently changed two settings that predate all of this. Scroll Lock
// and Pause used to fire whatever was held, because nothing looked at
// modifiers at all. Equality quietly made them fire only when bare -- an
// unannounced change to behaviour people already had muscle memory for.
// Subset restores it.
static bool modsSatisfied(uint32_t want, uint32_t held) {
    return (want & ~held) == 0;
}

// Bindings that exist, so a plain one can tell when a combo on the same key is
// the better match.
//
// Subset matching alone lets ONE physical press fire TWO bindings: with SHIFT+F
// and bare F both bound, pressing SHIFT+F satisfies both. For a toggle that
// is not cosmetic: a double fire sets the intent and immediately clears it.
//
// A registry rather than an ordering rule, because the bindings are independent
// objects polled in whatever order the frame loop happens to use, and a rule
// that depends on the combo being polled first would be right only by accident.
//
// COUNTED, since 2026-10-08: an entry is one live Hotkey's binding (two Hotkeys
// on the same key share it), and it goes when the last of them rebinds or is
// destroyed. The registry was append-only while a binding was set once at launch;
// the settings menu's Hotkeys page rebinds them while the game runs, so a stale
// entry would keep suppressing a bare key that a long-gone chord once shadowed,
// and sixteen of them would have starved every later binding.
struct Registered { int vk; uint32_t mods; int refs; };
static Registered g_bindings[16];
static unsigned   g_bindingCount = 0;

static void registerBinding(int vk, uint32_t mods) {
    if (!vk) return;
    for (unsigned i = 0; i < g_bindingCount; ++i) {
        if (g_bindings[i].vk == vk && g_bindings[i].mods == mods) {
            ++g_bindings[i].refs;
            return;
        }
    }
    if (g_bindingCount < 16) g_bindings[g_bindingCount++] = {vk, mods, 1};
}

static void unregisterBinding(int vk, uint32_t mods) {
    for (unsigned i = 0; i < g_bindingCount; ++i) {
        if (g_bindings[i].vk != vk || g_bindings[i].mods != mods) continue;
        if (--g_bindings[i].refs <= 0) {
            g_bindings[i] = g_bindings[--g_bindingCount];
        }
        return;
    }
}

// Is some OTHER binding on this key a strictly better match right now?
static bool betterMatchExists(int vk, uint32_t mine, uint32_t held) {
    for (unsigned i = 0; i < g_bindingCount; ++i) {
        const Registered& b = g_bindings[i];
        if (b.vk != vk || b.mods == mine) continue;
        // More modifiers, all of them held: that binding is what the player
        // pressed, and this one is the accidental subset.
        if (modsSatisfied(b.mods, held) && (b.mods & ~mine) != 0) return true;
    }
    return false;
}

// The pad / joystick state reader (hotkey.h). One pointer, set once by the
// d3d11 half before any hotkey is polled.
static HotkeyHeldFn g_nonKeyboardReader = nullptr;

void hotkeySetNonKeyboardReader(HotkeyHeldFn fn) { g_nonKeyboardReader = fn; }

// Every live Hotkey, so a suspension that lifts can prime them all (hotkey.h). A fixed array of
// pointers and a lock, both constant-initialised: a Hotkey in another file's static storage
// (any file-scope Hotkey) registers itself from its constructor whatever the order the files
// initialise in, and the registry needs nothing from the CRT to be ready.
constexpr int kLiveMax = 128;
static Hotkey*  g_live[kLiveMax];
static SRWLOCK  g_liveLock = SRWLOCK_INIT;

static void liveAdd(Hotkey* h) {
    AcquireSRWLockExclusive(&g_liveLock);
    for (Hotkey*& slot : g_live) {
        if (!slot) {
            slot = h;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_liveLock);
}

static void liveRemove(Hotkey* h) {
    AcquireSRWLockExclusive(&g_liveLock);
    for (Hotkey*& slot : g_live) {
        if (slot == h) slot = nullptr;
    }
    ReleaseSRWLockExclusive(&g_liveLock);
}

void hotkeysPrimeAll() {
    AcquireSRWLockExclusive(&g_liveLock);
    for (Hotkey* h : g_live) {
        if (h) h->primeToHeldNow();
    }
    ReleaseSRWLockExclusive(&g_liveLock);
}

int hotkeysLiveCount() {
    int n = 0;
    AcquireSRWLockExclusive(&g_liveLock);
    for (const Hotkey* h : g_live) n += h != nullptr;
    ReleaseSRWLockExclusive(&g_liveLock);
    return n;
}

static std::atomic<bool> g_suspended{false};
// Lifting primes first, while still suspended, so no poll can fall between the two.
void hotkeysSuspend(bool on) {
    if (!on && g_suspended.load(std::memory_order_relaxed)) hotkeysPrimeAll();
    g_suspended.store(on, std::memory_order_relaxed);
}
bool hotkeysSuspended() { return g_suspended.load(std::memory_order_relaxed); }

bool hotkeyBindingsEqual(const HotkeyBinding& a, const HotkeyBinding& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case HotkeyKind::None: return true;
        case HotkeyKind::Key: return a.vk == b.vk && a.mods == b.mods;
        case HotkeyKind::Pad: return a.padButtons == b.padButtons && a.padTrigger == b.padTrigger;
        case HotkeyKind::Joy: return a.joyDevice == b.joyDevice && a.joyInput == b.joyInput;
    }
    return false;
}

// Is this binding held at this moment? The keyboard answer is the raw key (the
// modifiers are not part of "held": m_down latches the key alone, see
// pressedWith); the others come from the reader.
bool Hotkey::readDownNow() const {
    switch (m_bind.kind) {
        case HotkeyKind::Key: return rawKeyDown(m_bind.vk);
        case HotkeyKind::Pad:
        case HotkeyKind::Joy: return g_nonKeyboardReader && g_nonKeyboardReader(m_bind);
        default: return false;
    }
}

void Hotkey::takeRegistryEntry() {
    if (m_registered || m_bind.kind != HotkeyKind::Key) return;
    registerBinding(m_bind.vk, m_bind.mods);
    m_registered = true;
}

void Hotkey::giveRegistryEntry() {
    if (!m_registered) return;
    unregisterBinding(m_bind.vk, m_bind.mods);
    m_registered = false;
}

Hotkey::Hotkey() { liveAdd(this); }

Hotkey::Hotkey(int vk) {
    liveAdd(this);
    setKey(vk);
}

Hotkey::Hotkey(const Hotkey& o)
    : m_bind(o.m_bind), m_down(o.m_down), m_missedUnfocused(o.m_missedUnfocused),
      m_gameMirrored(o.m_gameMirrored) {
    liveAdd(this);
    if (o.m_registered) takeRegistryEntry();
}

Hotkey& Hotkey::operator=(const Hotkey& o) {
    if (this == &o) return *this;
    giveRegistryEntry();
    m_bind = o.m_bind;
    m_down = o.m_down;
    m_missedUnfocused = o.m_missedUnfocused;
    m_gameMirrored = o.m_gameMirrored;
    if (o.m_registered) takeRegistryEntry();
    return *this;
}

Hotkey::~Hotkey() {
    liveRemove(this);
    giveRegistryEntry();
}

// setKey does not register (it never did): a key set by number is not a binding
// the better-match rule or the menu's adoption check should hear about.
void Hotkey::setKey(int vk, uint32_t mods) {
    HotkeyBinding b;
    if (vk) {
        b.kind = HotkeyKind::Key;
        b.vk = vk;
        b.mods = mods;
    }
    const bool changed = !hotkeyBindingsEqual(b, m_bind);
    giveRegistryEntry();
    m_bind = b;
    if (changed) m_down = readDownNow();
}

void Hotkey::setBinding(const char* name) {
    HotkeyBinding b;
    hotkeyParseBinding(name, &b);
    const bool changed = !hotkeyBindingsEqual(b, m_bind);
    giveRegistryEntry();
    m_bind = b;
    takeRegistryEntry();
    // A new binding starts with its latch at the key's real state. The menu
    // writes the ini while the key it just captured is still down; without
    // this the reload's first poll saw a press that began before the binding
    // existed, and Explorer Cam would have been entered by binding its key.
    if (changed) m_down = readDownNow();
}

bool hotkeyWouldFire(int vk, uint32_t mods, uint32_t held) {
    if (!vk) return false;
    return modsSatisfied(mods, held) && !betterMatchExists(vk, mods, held);
}

int hotkeyRegisteredKeys(int* vks, int max) {
    if (!vks || max <= 0) return 0;
    int n = 0;
    for (unsigned i = 0; i < g_bindingCount && n < max; ++i) vks[n++] = g_bindings[i].vk;
    return n;
}

bool Hotkey::pressed() {
    if (m_bind.kind == HotkeyKind::None) return false;
    // A game-mirrored binding is never filtered by focus; see the note above.
    // A pad or joystick binding takes the same rule: neither device has a
    // window, so "focused" is whether the game has it, as for a key.
    //
    // With the binding up, pressedWith's result and every member it writes
    // (m_down = false, no edge, no m_missedUnfocused) are the same whatever
    // `held` and `focused` are, and hotkeysSuspended() is not reached. So the
    // modifier reads and the foreground-window query are made only when the
    // binding is down.
    const bool down = readDownNow();
    if (!down) return pressedWith(false, 0, false);
    return pressedWith(true, m_bind.kind == HotkeyKind::Key ? heldMods() : 0,
                       m_gameMirrored || gameHasFocus());
}

bool Hotkey::pressedWith(bool keyDown, uint32_t held, bool focused) {
    if (m_bind.kind == HotkeyKind::None) return false;
    const bool matches =
        keyDown && (m_bind.kind != HotkeyKind::Key || hotkeyWouldFire(m_bind.vk, m_bind.mods, held));
    const bool fire = matches && focused;
    const bool edge = fire && !m_down && !hotkeysSuspended();

    // A press that matched the binding and was thrown away only because
    // another window had focus is recorded so somebody can be told. Only
    // EDVR's own keys can reach this now -- a game-mirrored binding passes
    // focused=true always -- and it uses !m_down for the same reason the real
    // edge does: one physical press, one report.
    if (matches && !focused && !m_down) m_missedUnfocused = true;

    // m_down latches the RAW key, not whether this binding fired.
    //
    // It used to latch the composite, and that let one physical press produce
    // an edge from a binding that had already been suppressed. Press
    // CTRL+ALT+SPACE with a bare SPACE binding also configured: SPACE is
    // suppressed, so its m_down stays false. Release CTRL and ALT while SPACE
    // is still held and the suppression lifts -- keyDown is still true, m_down
    // is still false, and a fresh edge is minted from a press the player made
    // once and finished with.
    //
    // On a toggle that is not a stray event, it is an inversion: the combo set
    // the intent, and letting go of the modifiers clears it again.
    //
    // Latching the raw key makes the suppression unable to create edges at all,
    // because an edge now requires the key to have been physically UP. It also
    // fixes the same shape for focus: a press made while another window has
    // focus no longer fires the moment focus returns.
    m_down = keyDown;
    return edge;
}

}  // namespace edvr
