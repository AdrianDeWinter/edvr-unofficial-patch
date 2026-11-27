// The HMD recentre key as the seat memory reads it (src/d3d11/elite_binds.cpp eliteBindsHmdReset*): Elite's HMDReset action out of .binds
// text, slot by slot, with what watching each takes. Keyboard (with modifiers), XInput pad and HOTAS/joystick slots are observable; the
// mouse, an axis and an unnamed key are not. Nothing is opened: no device, no window, no game; the folder cases write to a scratch folder.
//   seat_press_test --self-test | --dry-run
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/d3d11/elite_binds.h"
#include "../../src/common/hotkey.h"

using namespace edvr;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", text); }
}

// A .binds file with only the HMDReset element's slots varying; the rest is the neighbours a real file has.
std::string binds(const std::string& hmd) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<Root PresetName=\"Custom\" MajorVersion=\"4\" MinorVersion=\"2\">\n"
           "\t<ResetPowerDistribution>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F11\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</ResetPowerDistribution>\n" +
           hmd +
           "\t<ToggleCargoScoop>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_Home\" />\n\t\t<Secondary Device=\"231D012D\" Key=\"Joy_33\" />\n\t\t<ToggleOn Value=\"1\" />\n\t</ToggleCargoScoop>\n</Root>\n";
}
bool parse(const std::string& hmd, EliteResetBind* out) {
    const std::string text = binds(hmd);
    return eliteBindsHmdResetIn(text.data(), text.size(), out);
}

void seansBind() {
    // Sean's real file (Custom.4.2.binds, 2026-10-14): F12 on the keyboard, nothing on the secondary.
    EliteResetBind b;
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F12\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b),
          "Sean's HMDReset is found");
    check(b.present && b.count == 1, "one bound slot (the {NoDevice} secondary has no key)");
    check(b.slot[0].observable && b.slot[0].binding.kind == HotkeyKind::Key && b.slot[0].binding.vk == VK_F12 && b.slot[0].binding.mods == 0,
          "the keyboard slot is F12, observable, no modifier");
    check(std::strcmp(b.slot[0].label, "Keyboard:Key_F12") == 0, "its label names the device and the key as Elite wrote them");
    char text[64];
    check(hotkeyFormatBinding(b.slot[0].binding, text, sizeof text) > 0 && std::strcmp(text, "F12") == 0, "the binding formats as the hotkey machinery's F12");
}

void keyboardAndHotas() {
    EliteResetBind b;
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"231D0200\" Key=\"Joy_33\" />\n"
                "\t\t<Secondary Device=\"Keyboard\" Key=\"Key_F12\">\n\t\t\t<Modifier Device=\"Keyboard\" Key=\"Key_LeftControl\" />\n\t\t</Secondary>\n\t</HMDReset>\n", &b),
          "a HOTAS primary with a chorded keyboard secondary is found");
    check(b.count == 2, "two slots");
    check(b.slot[0].observable && b.slot[0].binding.kind == HotkeyKind::Joy && b.slot[0].binding.joyDevice == 0x231D0200u && b.slot[0].binding.joyInput == 32,
          "the HOTAS slot is device 231D0200, Joy_33 (input 32)");
    check(std::strcmp(b.slot[0].label, "231D0200:Joy_33") == 0, "its label is the device id and the key");
    check(b.slot[1].observable && b.slot[1].binding.kind == HotkeyKind::Key && b.slot[1].binding.vk == VK_F12 && (b.slot[1].binding.mods & kHotkeyCtrl),
          "the keyboard slot is F12 with Ctrl");
    check(std::strcmp(b.slot[1].label, "Keyboard:Key_LeftControl+Key_F12") == 0 || std::strncmp(b.slot[1].label, "Keyboard:", 9) == 0, "its label names the keyboard");

    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"231D0200\" Key=\"Joy_POV1Up\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.slot[0].observable && b.slot[0].binding.kind == HotkeyKind::Joy && b.slot[0].binding.joyInput == kJoyPovBase,
          "a POV hat direction is a joystick input too");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"GamePad\" Key=\"GamePad_Back\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.slot[0].observable && b.slot[0].binding.kind == HotkeyKind::Pad && b.slot[0].binding.padButtons != 0,
          "an XInput pad button is observable");
}

void unobservable() {
    EliteResetBind b;
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Mouse\" Key=\"Mouse_9\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.count == 1 && !b.slot[0].observable && std::strcmp(b.slot[0].label, "Mouse:Mouse_9") == 0,
          "a mouse button past the fifth is bound and is NOT observable");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Mouse\" Key=\"Mouse_XAxis\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.count == 1 && !b.slot[0].observable, "a mouse axis is bound and is NOT observable");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"231D0200\" Key=\"Joy_XAxis\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.count == 1 && !b.slot[0].observable, "a joystick axis is bound and is NOT observable");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F12\" />\n\t\t<Secondary Device=\"231D0200\" Key=\"Joy_XAxis\" />\n\t</HMDReset>\n", &b) &&
          b.count == 2 && b.slot[0].observable && !b.slot[1].observable,
          "a key plus a joystick axis: the axis slot is the one nothing sees (the status is then unobservable for the pair)");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_NoSuchKeyAnywhere\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.count == 1 && !b.slot[0].observable, "a key this build has no name for is bound and not observable");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"{NoDevice}\" Key=\"\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) && b.present && b.count == 0,
          "an unbound HMDReset is present with no slots");
}

void mouse() {
    EliteResetBind b;
    const int expect[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (int n = 1; n <= 5; ++n) {
        char slot[200];
        std::snprintf(slot, sizeof slot, "\t<HMDReset>\n\t\t<Primary Device=\"Mouse\" Key=\"Mouse_%d\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", n);
        check(parse(slot, &b) && b.count == 1 && b.slot[0].observable && b.slot[0].binding.kind == HotkeyKind::Key && b.slot[0].binding.vk == expect[n - 1],
              "Mouse_1 to Mouse_5 are the left, right, middle and two side buttons as GetAsyncKeyState names them");
        check(b.slot[0].needsProof, "...and are trusted only after a first press has been seen");
        char text[64];
        HotkeyBinding back;
        check(hotkeyFormatBinding(b.slot[0].binding, text, sizeof text) > 0 && hotkeyParseBinding(text, &back, true) && hotkeyBindingsEqual(back, b.slot[0].binding),
              "...and the hotkey machinery reads the binding back as the same mouse button");
    }
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Mouse\" Key=\"Mouse_01\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) && !b.slot[0].observable,
          "a spelling that is not exactly Mouse_N is not guessed at");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F12\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) && !b.slot[0].needsProof,
          "a keyboard key needs no proof");
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"231D0200\" Key=\"Joy_5\" />\n\t\t<Secondary Device=\"GamePad\" Key=\"GamePad_Back\" />\n\t</HMDReset>\n", &b) &&
          b.slot[0].needsProof && b.slot[1].needsProof, "a joystick and a pad button are trusted after a first press");
}

void absentAndNeighbours() {
    EliteResetBind b;
    check(!parse("", &b) && !b.present && b.count == 0, "no HMDReset element: not present");
    // The neighbours carry keys and joystick bindings of their own and must not be read as the HMD key.
    check(parse("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F12\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n", &b) &&
          b.count == 1 && b.slot[0].binding.vk == VK_F12, "ResetPowerDistribution (F11) and ToggleCargoScoop (a joystick secondary) are not the HMD key");
    EliteResetBind nul;
    check(!eliteBindsHmdResetIn(nullptr, 0, &nul) && !nul.present, "no text: not present");
    check(!eliteBindsHmdResetIn("x", 1, nullptr), "no output: false");
}

// The SRV turret view's key (Elite's ToggleBuggyTurretButton, issue 179) goes through the same parser by element name (eliteBindsActionIn); HMDReset above is the same parser with its name fixed.
bool parseTurret(const std::string& body, EliteResetBind* out) {
    const std::string text = binds(body);
    return eliteBindsActionIn("ToggleBuggyTurretButton", text.data(), text.size(), out);
}
void turretKey() {
    EliteResetBind b;
    check(parseTurret("\t<ToggleBuggyTurretButton>\n\t\t<Primary Device=\"GamePad\" Key=\"GamePad_FaceUp\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</ToggleBuggyTurretButton>\n", &b) &&
              b.present && b.count == 1 && b.slot[0].observable && b.slot[0].needsProof && b.slot[0].binding.kind == HotkeyKind::Pad && b.slot[0].binding.padButtons != 0 &&
              std::strcmp(b.slot[0].label, "GamePad:GamePad_FaceUp") == 0,
          "Sean's turret key (GamePad_FaceUp) is a pad button: found, observable, trusted after a first press");
    check(parseTurret("\t<ToggleBuggyTurretButton>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_T\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</ToggleBuggyTurretButton>\n", &b) &&
              b.count == 1 && b.slot[0].observable && !b.slot[0].needsProof && b.slot[0].binding.kind == HotkeyKind::Key && b.slot[0].binding.vk == 'T',
          "a keyboard turret key is observable and needs no proof");
    check(parseTurret("\t<ToggleBuggyTurretButton>\n\t\t<Primary Device=\"231D0200\" Key=\"Joy_12\" />\n\t\t<Secondary Device=\"Keyboard\" Key=\"Key_F9\" />\n\t</ToggleBuggyTurretButton>\n", &b) &&
              b.count == 2 && b.slot[0].observable && b.slot[0].binding.kind == HotkeyKind::Joy && b.slot[0].binding.joyDevice == 0x231D0200u && b.slot[0].binding.joyInput == 11 &&
              b.slot[1].observable && b.slot[1].binding.vk == VK_F9,
          "a joystick button with a keyboard secondary: both slots observable");
    check(parseTurret("\t<ToggleBuggyTurretButton>\n\t\t<Primary Device=\"Mouse\" Key=\"Mouse_7\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</ToggleBuggyTurretButton>\n", &b) &&
              b.count == 1 && !b.slot[0].observable,
          "a mouse button past the fifth is bound and cannot be seen (the press falls back)");
    check(!parseTurret("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F12\" />\n\t</HMDReset>\n", &b) && !b.present && b.count == 0,
          "a file with HMDReset and no turret element: not present, and the HMD key is not read as the turret key");
    check(parseTurret("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F12\" />\n\t</HMDReset>\n\t<ToggleBuggyTurretButton>\n\t\t<Primary Device=\"GamePad\" Key=\"GamePad_FaceUp\" />\n\t</ToggleBuggyTurretButton>\n", &b) &&
              b.count == 1 && b.slot[0].binding.kind == HotkeyKind::Pad, "with both elements the turret key is the turret element's, not HMDReset's");
    check(!eliteBindsActionIn(nullptr, "x", 1, &b) && !eliteBindsActionIn("", "x", 1, &b) && !eliteBindsActionIn("ToggleBuggyTurretButton", "x", 1, nullptr), "no element name, or no output: false");
}

std::wstring scratchDir() {
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    wchar_t name[64];
    std::swprintf(name, 64, L"edvr_seat_press_%lu_%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    return std::wstring(tmp) + name;
}
void writeFile(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &put, nullptr);
    CloseHandle(f);
}

void folder() {
    const std::wstring dir = scratchDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    EliteResetBind b;
    check(!eliteBindsHmdResetDir(dir.c_str(), &b) && b.filesSeen == 0, "no preset and no .binds: not found, no file seen");
    writeFile(dir + L"\\StartPreset.4.start", "Custom\r\n");
    check(!eliteBindsHmdResetDir(dir.c_str(), &b) && b.filesSeen == 0, "a preset naming files that are not there: no file seen");
    writeFile(dir + L"\\Custom.4.2.binds", binds("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F12\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n"));
    check(eliteBindsHmdResetDir(dir.c_str(), &b) && b.present && b.count == 1 && b.filesSeen == 1 && std::strcmp(b.file, "Custom.4.2.binds") == 0 &&
          b.slot[0].binding.vk == VK_F12, "the active preset's file answers, and names itself");
    // The newest active file answers alone, even if an older one would have said something else.
    Sleep(30);
    writeFile(dir + L"\\Custom.4.3.binds", binds("\t<HMDReset>\n\t\t<Primary Device=\"231D0200\" Key=\"Joy_12\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n"));
    check(eliteBindsHmdResetDir(dir.c_str(), &b) && std::strcmp(b.file, "Custom.4.3.binds") == 0 && b.slot[0].binding.kind == HotkeyKind::Joy,
          "the newest active file wins (a rebind to a HOTAS button)");
    // A file for another preset is not consulted.
    Sleep(30);
    writeFile(dir + L"\\Other.4.2.binds", binds("\t<HMDReset>\n\t\t<Primary Device=\"Keyboard\" Key=\"Key_F1\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</HMDReset>\n"));
    check(eliteBindsHmdResetDir(dir.c_str(), &b) && std::strcmp(b.file, "Custom.4.3.binds") == 0, "a newer file of another preset is not the active one");
    // A file without the element: the next active file is consulted (first that CONTAINS it answers).
    Sleep(30);
    writeFile(dir + L"\\Custom.4.4.binds", binds(""));
    check(eliteBindsHmdResetDir(dir.c_str(), &b) && b.filesSeen == 2 && std::strcmp(b.file, "Custom.4.3.binds") == 0,
          "the newest file without the element is passed over for the next that has it");
    DeleteFileW((dir + L"\\StartPreset.4.start").c_str());
    DeleteFileW((dir + L"\\Custom.4.2.binds").c_str());
    DeleteFileW((dir + L"\\Custom.4.3.binds").c_str());
    DeleteFileW((dir + L"\\Custom.4.4.binds").c_str());
    DeleteFileW((dir + L"\\Other.4.2.binds").c_str());
    RemoveDirectoryW(dir.c_str());
    check(GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES, "the scratch folder is gone");
}

// The SRV's bind context (issue 179): Sean's real StartPreset.4.start is "Custom | Custom | ConsoleX360 | Custom", one preset per context, the third the SRV's. Custom.4.2.binds holds a stale
// FaceUp for the turret; the stock ConsoleX360 preset, in the game directory's ControlSchemes folder, binds LThumb.
void srvContext() {
    const std::wstring root = scratchDir() + L"_srv";
    const std::wstring opt = root + L"\\Options", game = root + L"\\Game", schemes = game + L"\\ControlSchemes";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(opt.c_str(), nullptr);
    CreateDirectoryW(game.c_str(), nullptr);
    CreateDirectoryW(schemes.c_str(), nullptr);
    const std::string face = "\t<ToggleBuggyTurretButton>\n\t\t<Primary Device=\"GamePad\" Key=\"GamePad_FaceUp\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</ToggleBuggyTurretButton>\n";
    const std::string lthumb = "\t<ToggleBuggyTurretButton>\n\t\t<Primary Device=\"GamePad\" Key=\"GamePad_LThumb\" />\n\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\n\t</ToggleBuggyTurretButton>\n";
    const std::string back = "\t<ToggleBuggyTurretButton>\n\t\t<Primary Device=\"GamePad\" Key=\"GamePad_Back\" />\n\t</ToggleBuggyTurretButton>\n";
    EliteResetBind b;
    writeFile(opt + L"\\Custom.4.2.binds", binds(face));
    // the old one-line file: today's answer (the Custom file)
    writeFile(opt + L"\\StartPreset.start", "Custom\r\n");
    check(eliteBindsActionSrvDir("ToggleBuggyTurretButton", opt.c_str(), game.c_str(), &b) && std::strcmp(b.file, "Custom.4.2.binds") == 0 && b.slot[0].binding.padButtons == 0x8000,
          "with only the old StartPreset.start the answer is the active preset's file, as before");
    writeFile(opt + L"\\StartPreset.4.start", "Custom\r\nCustom\r\n");
    check(eliteBindsActionSrvDir("ToggleBuggyTurretButton", opt.c_str(), game.c_str(), &b) && std::strcmp(b.file, "Custom.4.2.binds") == 0,
          "two preset lines with trailing CRLF use the all-lines selection");
    writeFile(opt + L"\\StartPreset.4.start", "Custom\r\nCustom\r\n \t\r\nCustom\r\n");
    check(eliteBindsActionSrvDir("ToggleBuggyTurretButton", opt.c_str(), game.c_str(), &b) && std::strcmp(b.file, "Custom.4.2.binds") == 0,
          "a whitespace-only third line uses the all-lines selection");
    // Sean's file
    writeFile(opt + L"\\StartPreset.4.start", "Custom\r\nCustom\r\nConsoleX360\r\nCustom\r\n");
    check(!eliteBindsActionSrvDir("ToggleBuggyTurretButton", opt.c_str(), game.c_str(), &b), "the SRV line names ConsoleX360, which is nowhere yet: not found (the stale Custom file is not used)");
    writeFile(schemes + L"\\ConsoleX360.binds", binds(lthumb));
    check(eliteBindsActionSrvDir("ToggleBuggyTurretButton", opt.c_str(), game.c_str(), &b) && b.count == 1 && b.slot[0].observable && b.slot[0].binding.kind == HotkeyKind::Pad &&
              b.slot[0].binding.padButtons == 0x0040 && std::strcmp(b.file, "ControlSchemes\\ConsoleX360.binds") == 0 && std::strcmp(b.slot[0].label, "GamePad:GamePad_LThumb") == 0,
          "Custom with FaceUp beside ConsoleX360 with LThumb in ControlSchemes: the SRV line gives LThumb, and the file name is ControlSchemes\\ConsoleX360.binds");
    const struct {
        const char* contents;
        const char* description;
    } presets[] = {
        {"Custom\nCustom\nConsoleX360", "three lines with LF and no final newline give SRV LThumb from line three"},
        {"Custom\nCustom\nConsoleX360\n", "three lines with a final LF give SRV LThumb from line three"},
        {"Custom\r\nCustom\r\nConsoleX360", "three lines with CRLF and no final newline give SRV LThumb from line three"},
        {"Custom\r\nCustom\r\nConsoleX360\r\n", "three lines with a final CRLF give SRV LThumb from line three"},
        {"Custom\r\nCustom\r\nConsoleX360\r\nCustom", "four lines with no final newline give SRV LThumb from line three"},
    };
    for (const auto& preset : presets) {
        writeFile(opt + L"\\StartPreset.4.start", preset.contents);
        check(eliteBindsActionSrvDir("ToggleBuggyTurretButton", opt.c_str(), game.c_str(), &b) &&
                  b.count == 1 && b.slot[0].binding.padButtons == 0x0040 &&
                  std::strcmp(b.file, "ControlSchemes\\ConsoleX360.binds") == 0,
              preset.description);
    }
    check(eliteBindsActionDir("ToggleBuggyTurretButton", opt.c_str(), &b) && b.slot[0].binding.padButtons == 0x8000, "...while the ordinary lookup (HMDReset's and the FSS's way) still answers FaceUp from the Custom file");
    // a maintained copy in Options\Bindings wins over the stock one
    Sleep(30);
    writeFile(opt + L"\\ConsoleX360.4.2.binds", binds(back));
    check(eliteBindsActionSrvDir("ToggleBuggyTurretButton", opt.c_str(), game.c_str(), &b) && std::strcmp(b.file, "ConsoleX360.4.2.binds") == 0 && b.slot[0].binding.padButtons == 0x0020,
          "a maintained ConsoleX360 file in Options\\Bindings is looked up first");
    writeFile(opt + L"\\ConsoleX360.4.2.binds", binds(""));
    check(eliteBindsActionSrvDir("ToggleBuggyTurretButton", opt.c_str(), game.c_str(), &b) && std::strcmp(b.file, "ControlSchemes\\ConsoleX360.binds") == 0,
          "...and one without the element is passed over for the stock file");
    DeleteFileW((opt + L"\\StartPreset.start").c_str());
    DeleteFileW((opt + L"\\StartPreset.4.start").c_str());
    DeleteFileW((opt + L"\\Custom.4.2.binds").c_str());
    DeleteFileW((opt + L"\\ConsoleX360.4.2.binds").c_str());
    DeleteFileW((schemes + L"\\ConsoleX360.binds").c_str());
    RemoveDirectoryW(schemes.c_str());
    RemoveDirectoryW(game.c_str());
    RemoveDirectoryW(opt.c_str());
    RemoveDirectoryW(root.c_str());
    check(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES, "the SRV scratch folder is gone");
}

void stockFallbackAcrossPresetLines() {
    const std::wstring root = scratchDir() + L"_stock";
    const std::wstring opt = root + L"\\Bindings";
    const std::wstring schemes = root + L"\\ControlSchemes";
    const std::wstring start = opt + L"\\StartPreset.4.start";
    const std::wstring user = opt + L"\\Pilot.4.2.binds";
    const std::wstring stock = schemes + L"\\Pilot.binds";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(opt.c_str(), nullptr);
    CreateDirectoryW(schemes.c_str(), nullptr);
    writeFile(start, "Missing1\r\nMissing2\r\nMissing3\r\nPilot\r\n");
    writeFile(user,
              "<Root><HMDReset><Primary Device=\"Keyboard\" Key=\"Key_F8\" /></HMDReset>"
              "<ExplorationFSSEnter><Primary Device=\"Keyboard\" Key=\"Key_F9\" /></ExplorationFSSEnter></Root>");
    const std::string stockText =
        "<Root><HMDReset><Primary Device=\"Keyboard\" Key=\"Key_F12\" /></HMDReset>"
        "<ExplorationFSSEnter><Primary Device=\"Keyboard\" Key=\"Key_F5\" />"
        "<Secondary Device=\"GamePad\" Key=\"GamePad_Back\" /></ExplorationFSSEnter>"
        "<UI_Up><Primary Device=\"Keyboard\" Key=\"Key_UpArrow\" /></UI_Up></Root>";
    writeFile(stock, stockText);
    EliteResetBind hmd;
    check(eliteBindsHmdResetDir(opt.c_str(), &hmd, root.c_str()) && hmd.count == 1 &&
              hmd.slot[0].binding.vk == VK_F8 && std::strcmp(hmd.file, "Pilot.4.2.binds") == 0,
          "the maintained preset on the fourth line wins over its stock file for HMDReset");
    char key[40];
    check(eliteBindsLookupDir(opt.c_str(), "ExplorationFSSEnter", key, sizeof(key), nullptr, root.c_str()) &&
              std::strcmp(key, "F9") == 0,
          "the FSS keyboard lookup considers the maintained preset on the fourth line");
    check(!eliteBindsLookupPadDir(opt.c_str(), "ExplorationFSSEnter", key, sizeof(key), root.c_str()),
          "an FSS element with no pad slot in the maintained file does not borrow the stock slot");
    EliteKeySlots ui;
    check(eliteBindsLookupSlotsDir(opt.c_str(), "UI_Up", kEliteKeyAllowModifierMain, &ui, root.c_str()) &&
              ui.count == 1 && std::strcmp(ui.slot[0].binding, "UP") == 0 &&
              std::strcmp(ui.file, "ControlSchemes\\Pilot.binds") == 0,
          "a menu element absent from the maintained file falls through to the named stock preset");
    EliteKeyboardUse keyboard[8];
    EliteBindUse uses[8];
    char file[64];
    check(eliteBindsKeyboardUsesDir(opt.c_str(), keyboard, 8, file, sizeof(file), root.c_str()) == 2 &&
              std::strcmp(file, "Pilot.4.2.binds") == 0 && std::strcmp(keyboard[0].binding, "F8") == 0 &&
              std::strcmp(keyboard[1].binding, "F9") == 0,
          "keyboard clashes use every keyboard slot from the first readable maintained preset");

    DeleteFileW(user.c_str());
    check(eliteBindsHmdResetDir(opt.c_str(), &hmd, root.c_str()) && hmd.count == 1 &&
              hmd.slot[0].binding.vk == VK_F12 && std::strcmp(hmd.file, "ControlSchemes\\Pilot.binds") == 0,
          "HMDReset can be adopted from the stock preset on the fourth line");
    check(eliteBindsLookupDir(opt.c_str(), "ExplorationFSSEnter", key, sizeof(key), nullptr, root.c_str()) &&
              std::strcmp(key, "F5") == 0,
          "the FSS keyboard lookup can use the named stock preset");
    check(eliteBindsLookupPadDir(opt.c_str(), "ExplorationFSSEnter", key, sizeof(key), root.c_str()) &&
              std::strcmp(key, "GamePad_Back") == 0,
          "the FSS pad lookup can use the named stock preset");
    check(eliteBindsAllUsesDir(opt.c_str(), uses, 8, file, sizeof(file), root.c_str()) == 4 &&
              std::strcmp(file, "ControlSchemes\\Pilot.binds") == 0 &&
              std::strcmp(uses[0].element, "HMDReset") == 0 && uses[0].binding.vk == VK_F12 &&
              std::strcmp(uses[1].element, "ExplorationFSSEnter") == 0 && uses[1].binding.vk == VK_F5 &&
              uses[2].binding.kind == HotkeyKind::Pad && uses[2].binding.padButtons == 0x0020 &&
              std::strcmp(uses[3].element, "UI_Up") == 0 && uses[3].binding.vk == VK_UP,
          "stock-only clash checks include actions and buttons without assigning the file to a context");

    const std::wstring escape = root + L"\\Escape.binds";
    writeFile(start, "..\\Escape\r\n");
    writeFile(escape, stockText);
    check(!eliteBindsHmdResetDir(opt.c_str(), &hmd, root.c_str()) && hmd.filesSeen == 0,
          "a preset line with a path separator cannot select a file outside ControlSchemes");
    DeleteFileW(escape.c_str());

    const char embeddedNul[] = "Pilot\0ignored\r\nPilot\r\n";
    writeFile(start, std::string(embeddedNul, sizeof(embeddedNul) - 1));
    check(eliteBindsHmdResetDir(opt.c_str(), &hmd, root.c_str()) && hmd.count == 1 &&
              hmd.slot[0].binding.vk == VK_F12 && std::strcmp(hmd.file, "ControlSchemes\\Pilot.binds") == 0,
          "a malformed line with an embedded NUL does not hide a valid later preset");

    const std::wstring namedUser = opt + L"\\Odyssey VR Controls Pilot Profile v4.2.4.2.binds";
    const std::wstring namedStock = schemes + L"\\Odyssey VR Controls Pilot Profile v4.2.binds";
    writeFile(namedUser, stockText);
    writeFile(namedStock, stockText);
    writeFile(start, "Odyssey VR Controls Pilot Profile v4.2\r\n");
    check(eliteBindsHmdResetDir(opt.c_str(), &hmd, root.c_str()) && hmd.count == 1 &&
              hmd.slot[0].binding.vk == VK_F12 && std::strcmp(hmd.file, "Odyssey VR Controls Pilot Profile v4.2.4.2.binds") == 0,
          "a long, dotted and spaced user preset can select its versioned bindings");
    DeleteFileW(namedUser.c_str());
    check(eliteBindsHmdResetDir(opt.c_str(), &hmd, root.c_str()) && hmd.count == 1 &&
              hmd.slot[0].binding.vk == VK_F12 &&
              std::strcmp(hmd.file, "ControlSchemes\\Odyssey VR Controls Pilot Profile v4.2.binds") == 0,
          "a long, dotted and spaced preset can select its exact stock bindings");

    writeFile(user, stockText);
    writeFile(start, "Pilot*\r\nPilo?\r\nPilot:stream\r\n");
    check(!eliteBindsHmdResetDir(opt.c_str(), &hmd, root.c_str()) && hmd.filesSeen == 0,
          "wildcards and stream syntax cannot select bindings");
    writeFile(start, "NUL\r\n");
    check(!eliteBindsHmdResetDir(opt.c_str(), &hmd, root.c_str()) && hmd.filesSeen == 0,
          "a DOS device alias cannot be read as a stock bindings file");
    DeleteFileW(user.c_str());
    DeleteFileW(namedStock.c_str());
    DeleteFileW(stock.c_str());
    DeleteFileW(start.c_str());
    RemoveDirectoryW(schemes.c_str());
    RemoveDirectoryW(opt.c_str());
    RemoveDirectoryW(root.c_str());
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--dry-run")) {
        std::puts("seat_press_test: dry-run (no files, no devices)");
        return 0;
    }
    if (argc != 2 || wcscmp(argv[1], L"--self-test")) {
        std::fputs("usage: --self-test | --dry-run\n", stderr);
        return 2;
    }
    seansBind();
    keyboardAndHotas();
    unobservable();
    mouse();
    absentAndNeighbours();
    turretKey();
    folder();
    srvContext();
    stockFallbackAcrossPresetLines();
    std::printf("seat_press_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
