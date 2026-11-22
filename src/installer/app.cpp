#include "app.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>

#include "apply.h"
#include "detect.h"
#include "logbundle.h"
#include "mirror.h"
#include "payload.h"
#include "../common/elite_graphics_folder.h"
#ifdef EDUP_LAUNCHER
#include "launch_route.h"
#include "launcher_main.h"
#endif

namespace edvr::installer {
namespace {

void writeOut(const std::string& utf8) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        const std::wstring w = fromUtf8(utf8);
        DWORD written = 0;
        WriteConsoleW(h, w.c_str(), static_cast<DWORD>(w.size()), &written, nullptr);
    } else {
        DWORD written = 0;
        WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

std::string bullets(const char* title, const std::vector<std::string>& items) {
    if (items.empty()) return std::string();
    std::string out = std::string(title) + "\r\n";
    for (const std::string& i : items) out += "    " + i + "\r\n";
    return out;
}

}  // namespace

std::string usageText() {
#ifdef EDUP_LAUNCHER
    const char* exe = "ed-up-launcher.exe";
    std::string out = "ED-UP launcher\r\n";
#else
    const char* exe = "edvr-installer.exe";
    std::string out = "ED-UP installer\r\n";
#endif
    out +=
           "\r\n"
           "  " + std::string(exe) + "                        open the window\r\n"
           "  " + std::string(exe) + " --install [--dir D]    install or update\r\n"
           "  " + std::string(exe) + " --repair  [--dir D]    repair this install\r\n"
           "  " + std::string(exe) + " --uninstall [--dir D]  remove ED-UP, restoring what it replaced\r\n"
           "  " + std::string(exe) + " --collect-logs         zip the latest logs onto the Desktop\r\n"
           "\r\n"
           "  --dir <path>         the folder holding EliteDangerous64.exe. Without it, the\r\n"
           "                       installer finds your installs itself and uses the only one.\r\n"
           "  --store epic|steam|frontier  the store for an exact --dir product folder, when\r\n"
           "                       detection in this Windows prefix cannot identify its store.\r\n"
           "  --scope both|vr|flat what to install: both (VR and flat), vr (VR only) or flat\r\n"
           "                       (flat only). Without it, the scope already installed is kept,\r\n"
           "                       and a new install gets both.\r\n"
           "  --dry-run            say what would happen; touch nothing.\r\n"
           "  --convert-profile    accepted for older scripts; no longer needed. A scope change\r\n"
           "                       is an ordinary install with --scope.\r\n"
           "  --replace-settings   replace your settings files with the shipped defaults instead\r\n"
           "                       of keeping your values.\r\n"
           "  --remove-settings    with --uninstall, delete the settings files of the installed\r\n"
           "                       scope too.\r\n"
           "  --purge              clean uninstall: removes ED-UP, both settings files, edup_logs\\,\r\n"
           "                       edup_backup\\, edup_install\\, and this install's user settings folder.\r\n"
           "  --help               this.\r\n"
#ifdef EDUP_LAUNCHER
           "\r\n"
           "  --check-update       ask GitHub whether a newer release exists, check its signature, and\r\n"
           "                       say what it found. Writes only the launcher's own state.\r\n"
           "  --update             --check-update, then download the release's files into the launcher's\r\n"
           "                       cache, verify each, and adopt its manifest. Touches no game folder.\r\n"
           "  --channel stable|rc  which releases to look at, for this run (rc includes release candidates).\r\n"
           "  --payload <dir>      install from an unpacked ed-up-<version>.zip instead of the cache.\r\n"
           "  --data-root <dir>    keep the launcher's folders, and your settings, under <dir>\r\n"
           "                       instead of %LOCALAPPDATA%\\EDUP, and do not copy, register or update\r\n"
           "                       the launcher itself.\r\n"
           "  --unregister         remove the launcher, its Start menu shortcut and its downloaded files.\r\n"
           "                       ED-UP in your game folders and your settings copies are left alone.\r\n"
           "  --direct-dry-run --dir <game folder> --vr|--novr\r\n"
           "                       sign in to Frontier as a direct launch would, print the command line it\r\n"
           "                       would start Elite with, every token, your machine ID, email and password\r\n"
           "                       replaced by <masked>, and start nothing. Detected Epic installs need\r\n"
           "                       Epic's launcher instead; use Play or --headless. Steam needs its client\r\n"
           "                       running. Frontier uses only the login the window stored, never asking\r\n"
           "                       for one. An unrecognised folder uses Steam under steamapps, otherwise\r\n"
           "                       Frontier.\r\n"
           "  --licenses           print the licence of the third-party code the launcher is ported from.\r\n"
           "  --close-after-start  close the launcher once Elite has started, instead of staying open\r\n"
           "                       until it exits. Also a checkbox in the window, which this flag does not change.\r\n"
           "  --frontier-login     with direct launch on a Steam install, sign in with a Frontier\r\n"
           "                       account instead of Steam.\r\n"
           "  --headless [--dir <game folder>] [--vr|--novr|/vr|/novr] [--update] [--] [%command%]\r\n"
           "                       start Elite with no window, through the same route Play uses, and print\r\n"
           "                       its progress. Without --dir, uses a wrapped command's folder or this\r\n"
           "                       exe's folder. Without --vr/--novr, uses the install's scope, remembered\r\n"
           "                       mode, or Elite's Settings.xml. Trailing arguments (such as Steam's\r\n"
           "                       %command%) are accepted. Combine --update with --install or --repair\r\n"
           "                       to update and apply headlessly (e.g. from min-ed-launcher).\r\n"
           "                       Exit 0 when Elite started, 1 when it did not, 2 for bad arguments.\r\n"
#endif
           "\r\n"
           "  Your settings files (and a backup of the install record) live outside the game\r\n"
           "  folder, in %LOCALAPPDATA%\\EDUP, so a game update that wipes the folder cannot take\r\n"
           "  them too, and editing settings never needs administrator rights.\r\n";
    return out;
}

AppArgs parseArgs(int argc, wchar_t** argv) {
    AppArgs a;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&](std::wstring* out) {
            if (i + 1 < argc) {
                *out = argv[++i];
                return true;
            }
            a.badArg = true;
            a.badArgText = arg + L" needs a value";
            return false;
        };
        auto setScope = [&](const std::wstring& value) {
            if (value == L"both" || value == L"vr" || value == L"flat") {
                a.scope = toUtf8(value);
                return;
            }
            a.badArg = true;
            a.badArgText = L"--scope takes both, vr or flat, not \"" + value + L"\"";
        };
        auto setStore = [&](const std::wstring& value) {
            if (value == L"epic") a.store = L"Epic";
            else if (value == L"steam") a.store = L"Steam";
            else if (value == L"frontier") a.store = L"Frontier launcher";
            else {
                a.badArg = true;
                a.badArgText = L"--store takes epic, steam or frontier, not \"" + value + L"\"";
            }
        };
        if (arg == L"--install")
            a.action = AppArgs::Act::Install;
        else if (arg == L"--repair")
            a.action = AppArgs::Act::Repair;
        else if (arg == L"--uninstall")
            a.action = AppArgs::Act::Uninstall;
        else if (arg == L"--collect-logs")
            a.action = AppArgs::Act::CollectLogs;
        else if (arg == L"--dir")
            next(&a.dir);
        else if (arg.rfind(L"--dir=", 0) == 0)
            a.dir = arg.substr(6);
        else if (arg == L"--store") {
            std::wstring value;
            if (next(&value)) setStore(value);
        } else if (arg.rfind(L"--store=", 0) == 0)
            setStore(arg.substr(8));
        else if (arg == L"--scope") {
            std::wstring value;
            if (next(&value)) setScope(value);
        } else if (arg.rfind(L"--scope=", 0) == 0)
            setScope(arg.substr(8));
        else if (arg == L"--dry-run")
            a.dryRun = true;
        else if (arg == L"--convert-profile")
            a.convertProfile = true;
        else if (arg == L"--replace-settings")
            a.keepSettings = false;
        else if (arg == L"--remove-settings")
            a.removeSettings = true;
        else if (arg == L"--purge") {
            a.purge = true;
            a.removeSettings = true;
            if (a.action == AppArgs::Act::None) a.action = AppArgs::Act::Uninstall;
        } else if (arg == L"--autorun")
            a.autorun = true;
#ifdef EDUP_LAUNCHER
        else if (arg == L"--payload")
            next(&a.payloadDir);
        else if (arg == L"--data-root")
            next(&a.dataRoot);
        else if (arg == L"--user-data-root")
            next(&a.userDataRoot);
        else if (arg == L"--apply-plan")
            next(&a.planFile);
        else if (arg == L"--channel") {
            std::wstring value;
            if (next(&value)) {
                if (value == L"stable" || value == L"rc") {
                    a.channel = toUtf8(value);
                } else {
                    a.badArg = true;
                    a.badArgText = L"--channel takes stable or rc, not \"" + value + L"\"";
                }
            }
        } else if (arg == L"--check-update")
            a.checkUpdate = true;
        else if (arg == L"--update")
            a.update = true;
        else if (arg == L"--unregister")
            a.unregister = true;
        else if (arg == L"--updated")
            a.updated = true;
        else if (arg == L"--direct-dry-run")
            a.directDryRun = true;
        else if (arg == L"--vr" || _wcsicmp(arg.c_str(), L"/vr") == 0)
            a.vr = true;
        else if (arg == L"--novr" || _wcsicmp(arg.c_str(), L"/novr") == 0)
            a.novr = true;
        else if (arg == L"--licenses")
            a.licenses = true;
        else if (arg == L"--close-after-start")
            a.closeAfterStart = true;
        else if (arg == L"--headless")
            a.headless = true;
        else if (arg == L"--frontier-login")
            a.frontierLogin = true;
        else if (arg == L"--" && a.headless) {
            for (int j = i + 1; j < argc; ++j) {
                const std::wstring w = argv[j];
                if (_wcsicmp(w.c_str(), L"/vr") == 0) a.vr = true;
                else if (_wcsicmp(w.c_str(), L"/novr") == 0) a.novr = true;
                a.wrappedArgs.push_back(w);
            }
            break;
        }
#endif
        else if (arg == L"--help" || arg == L"-h" || arg == L"/?")
            a.help = true;
        else {
#ifdef EDUP_LAUNCHER
            // After --headless, collect trailing wrapped command tokens (e.g. Steam's %command%)
            // only when the token looks like an executable path (ends with .exe, not a flag).
            // Arbitrary commands can still be passed after a literal `--`.
            const bool looksLikeExe = arg.size() > 4 && arg[0] != L'-' &&
                                      _wcsicmp(arg.c_str() + arg.size() - 4, L".exe") == 0;
            if (a.headless && looksLikeExe) {
                for (int j = i; j < argc; ++j) {
                    const std::wstring w = argv[j];
                    if (_wcsicmp(w.c_str(), L"/vr") == 0) a.vr = true;
                    else if (_wcsicmp(w.c_str(), L"/novr") == 0) a.novr = true;
                    a.wrappedArgs.push_back(w);
                }
                break;
            }
#endif
            a.badArg = true;
            a.badArgText = L"unknown argument: " + arg;
        }
    }
    if (!a.store.empty() && a.dir.empty()) {
        a.badArg = true;
        a.badArgText = L"--store needs --dir with the exact game's product folder";
    }
    return a;
}

bool describeSelectedDir(const AppArgs& args, GameInstall* out,
                         const std::vector<GameInstall>* installs) {
    if (args.dir.empty()) return false;
    // Store context supplied by a host wrapper cannot label a descendant chosen
    // while searching an arbitrary launcher/library root.
    if (!args.store.empty() && !gameDirLooksRight(args.dir)) return false;
    const bool described = installs ? describeDirWithInstalls(args.dir, L"Folder you chose", *installs, out)
                                   : describeDir(args.dir, L"Folder you chose", out);
    if (!described) return false;
    if (!args.store.empty()) out->source = args.store;
    return true;
}

Options optionsFor(const AppArgs& args, bool repair) {
    Options o;
    o.scope = args.scope;
    o.keepSettings = args.keepSettings;
    o.removeSettings = args.removeSettings || args.purge;
    o.purge = args.purge;
    o.repair = repair;
    o.backupStamp = timestampName();
    o.nowUtc = utcNow();
    return o;
}

std::string scopeFor(const AppArgs& args, const Survey& survey) {
    return args.scope.empty() ? defaultScope(survey) : args.scope;
}

edvr::ModeRead eliteGameMode() {
    PWSTR path = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) && path) {
        base = path;
        CoTaskMemFree(path);
    } else {
        wchar_t env[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH)) base = env;
    }
    const std::wstring folder = edvr::eliteGraphicsFolderUnder(base);
    if (folder.empty()) return edvr::readGameMode(nullptr);
    const std::string xml = readTextFile(joinPath(folder, L"Settings.xml"));
    return edvr::readGameMode(xml.empty() ? nullptr : xml.c_str());
}

std::string statusReport(const Survey& s, const PayloadInfo& payload, const std::string& scope) {
    std::string out;
    out += "Folder:  " + toUtf8(s.game.dir) + "\r\n";
    out += "Found by: " + toUtf8(s.game.source);
    if (!s.game.product.empty()) out += "   (" + toUtf8(s.game.product) + ")";
    out += "\r\n\r\n";
    const std::string detected = installedScope(s);
    out += "Installed scope: " + (detected.empty() ? std::string("none") : detected) +
           "   Install scope: " + scope + "\r\n";

    out += "d3d11.dll         " + toUtf8(describeDll(s.d3d11)) + "\r\n";
    if (s.haveOpenvrDir) {
        // Relative to the folder printed above: the absolute path is the same
        // one again with a suffix, and it wraps across two lines of the pane
        // for no gain.
        std::wstring where = s.game.openvrDir;
        if (where.size() > s.game.dir.size() &&
            _wcsnicmp(where.c_str(), s.game.dir.c_str(), s.game.dir.size()) == 0) {
            where = where.substr(s.game.dir.size() + 1);
        }
        out += "openvr_api.dll    " + toUtf8(describeDll(s.openvrCurrent)) + "\r\n";
        out += "  the original    " + toUtf8(describeDll(s.openvrOrig)) + "\r\n";
        out += "  in              " + toUtf8(where) + "\\\r\n";
    } else {
        out += "openvr_api.dll    the game's own copy was not found under this folder\r\n";
    }
    out += std::string("edup.ini          ") + (s.iniPresent ? "present" : "not present") + "\r\n";
    // The flat settings file: shown when the scope installs flat, or whenever one is there.
    // "not present" beside a present edup.ini means the flat runtime reads edup.ini until
    // the first flat change starts its own from it.
    if (scope != "vr" || s.flatIniPresent)
        out += std::string("edup-flat.ini     ") + (s.flatIniPresent ? "present" : "not present") + "\r\n";
    out += std::string("nvngx_dlss.dll    ") +
           (s.ngx.kind == DllKind::Absent ? std::string("not present (NVIDIA's DLSS runtime)")
                                          : toUtf8(describeDll(s.ngx))) +
           "\r\n";
    out += std::string("NVIDIA card       ") +
           (s.nvidiaAdapter ? toUtf8(s.nvidiaAdapterName)
                            : std::string("none found -- temporal_aa = dlaa needs one")) +
           "\r\n";

    if (s.state.present) {
        out += "\r\nLast installed by this installer: " + s.state.edvrVersion;
        if (!s.state.installedUtc.empty()) out += " on " + s.state.installedUtc;
        out += "\r\n";
        if (!s.state.chainTarget.empty()) {
            out += "Passing calls through to " + toUtf8(s.state.chainTarget);
            if (!s.state.chainMod.empty()) out += " (" + toUtf8(s.state.chainMod) + ")";
            out += "\r\n";
        }
    }
    if (!payload.version.empty()) out += "\r\nThis installer carries ED-UP " + payload.version + ".\r\n";
    if (s.gameRunningHere) {
        out += "\r\n!  Elite Dangerous is running. Close it before installing anything.\r\n";
    } else if (s.gameRunningElsewhere) {
        // Worth a line of its own. Somebody with two installs, told nothing,
        // reads a status that ignores the game they can see running.
        out += "\r\n-  Elite Dangerous is running, but from a different folder. This install is "
               "not the one in use.\r\n";
    } else if (s.gameRunStateUnknown) {
        // A check that failed says so. Reading it as "not running" is how the
        // installer used to walk into a folder the game had open.
        out += "\r\n!  Could not tell whether Elite Dangerous is running (Windows would not list "
               "the running programs). Nothing will be installed until that is settled.\r\n";
    }
    return out;
}

std::string planReport(const Plan& plan) {
    std::string out = planSummary(plan);
    // Named for the file the plan works on: the flat edition's is edvr-flat.ini,
    // and a report about "your edvr.ini" would send somebody to the wrong one.
    const std::string kept = "\r\n  Settings kept from your " +
                             (plan.settingsFile.empty() ? std::string("edup.ini") : plan.settingsFile) + ":";
    out += bullets(kept.c_str(), plan.merge.kept);
    out += bullets("  New defaults adopted (you had not changed these):", plan.merge.adopted);
    out += bullets("  Set by the installer:", plan.merge.forced);
    out += bullets("  Followed a setting that moved, and still applies:", plan.merge.followed);
    out += bullets("  Carried over, no longer used by this version:", plan.merge.retired);
    out += bullets("  Carried over, not an ED-UP setting:", plan.merge.carried);
    out += bullets("  Lines you had removed, left commented out:", plan.merge.removed);
    return out;
}

bool needsElevationFor(const Survey& s) {
    if (!canWriteInto(s.game.dir)) return true;
    if (s.haveOpenvrDir && !canWriteInto(s.game.openvrDir)) return true;
    return false;
}

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool elevationBlocksPlay(bool tokenElevated, unsigned long elevationType) {
    return tokenElevated && elevationType == TokenElevationTypeFull;
}

bool isElevatedForPlay() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    if (!ok || elevation.TokenIsElevated == 0) {
        CloseHandle(token);
        return false;
    }
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD typeSize = sizeof(type);
    const BOOL typeOk = GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &typeSize);
    CloseHandle(token);
    if (!typeOk) return true;
    return elevationBlocksPlay(true, static_cast<unsigned long>(type));
}

bool relaunchElevated(const AppArgs& args) {
    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return false;

    std::wstring cmd;
    switch (args.action) {
        case AppArgs::Act::Install: cmd += L"--install "; break;
        case AppArgs::Act::Repair: cmd += L"--repair "; break;
        case AppArgs::Act::Uninstall: cmd += L"--uninstall "; break;
        // Collecting logs never needs elevation: it reads, and writes to the
        // Desktop.
        case AppArgs::Act::CollectLogs:
        case AppArgs::Act::None: break;
    }
    if (!args.dir.empty()) cmd += L"--dir \"" + args.dir + L"\" ";
    if (!args.store.empty()) {
        const wchar_t* store = args.store == L"Epic" ? L"epic" : args.store == L"Steam" ? L"steam" : L"frontier";
        cmd += std::wstring(L"--store ") + store + L" ";
    }
    if (!args.scope.empty()) cmd += L"--scope " + fromUtf8(args.scope) + L" ";
    if (!args.keepSettings) cmd += L"--replace-settings ";
    if (args.removeSettings) cmd += L"--remove-settings ";
    if (args.purge) cmd += L"--purge ";
    if (args.dryRun) cmd += L"--dry-run ";
    if (args.convertProfile) cmd += L"--convert-profile ";
#ifndef EDUP_LAUNCHER
    // The elevated copy can run as another account (an administrator's password typed on a standard user's desktop); the
    // settings it moves belong to the user who asked, so it is told their folder (checked there: userDataRootOf).
    {
        const std::wstring userRoot = args.userDataRoot.empty() ? defaultMirrorRoot() : args.userDataRoot;
        if (!userRoot.empty()) cmd += L"--user-data-root \"" + userRoot + L"\" ";
    }
#endif
    cmd += L"--autorun";

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = self;
    info.lpParameters = cmd.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return false;  // declined at the UAC prompt, or failed
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

int runConsole(const AppArgs& args) {
    if (args.help || args.badArg) {
        if (args.badArg) writeOut(toUtf8(args.badArgText) + "\r\n\r\n");
        writeOut(usageText());
        return args.badArg ? 2 : 0;
    }

#ifdef EDUP_LAUNCHER
    // The payload comes from --payload or the adopted manifest, not from this exe.
    std::string payloadNote;
    const bool havePayload = launcherPreparePayload(args, &payloadNote);
#endif
    GameInstall game;
    if (!args.dir.empty()) {
        if (!describeSelectedDir(args, &game)) {
            writeOut("No EliteDangerous64.exe under " + toUtf8(args.dir) + "\r\n");
            return 2;
        }
    } else {
        size_t legacyLeftOut = 0;
        const std::vector<GameInstall> found = findInstalls(&legacyLeftOut);
        if (found.empty()) {
            if (legacyLeftOut > 0) writeOut(std::string(onlyLegacyInstallsText()) + "\r\n");
            writeOut(legacyLeftOut > 0 ? "If Odyssey is installed somewhere else, pass --dir with the folder that holds EliteDangerous64.exe.\r\n"
                                       : "No Elite Dangerous install found. Pass --dir with the folder that holds "
                                         "EliteDangerous64.exe.\r\n");
            return 2;
        }
        if (found.size() > 1) {
            std::string list;
            for (const GameInstall& g : found)
                list += "    " + toUtf8(g.source) + ": " + toUtf8(g.dir) + "\r\n";
            writeOut("More than one install found; name one with --dir:\r\n" + list);
            return 2;
        }
        game = found.front();
    }

#ifdef EDUP_LAUNCHER
    const std::wstring mirrorRoot = launcherMirrorRoot(args);
    if (args.dataRoot.empty()) migrateLegacyDataRoot(mirrorRoot);
#else
    const std::wstring mirrorRoot = userDataRootOf(args.userDataRoot);
    migrateLegacyDataRoot(mirrorRoot);
#endif
    const std::wstring mirrorDir = mirrorDirFor(game, mirrorRoot);
    Survey survey = surveyTarget(game, mirrorDir);
    const PayloadInfo& payload = payloadInfo();
    std::string scope = scopeFor(args, survey);
    writeOut(statusReport(survey, payload, scope) + "\r\n");
    if (args.convertProfile) {
        writeOut("--convert-profile is no longer needed: a scope change is an ordinary install "
                 "(--scope). Nothing was converted.\r\n");
    }

    if (args.action == AppArgs::Act::None) return 0;

    if (args.action == AppArgs::Act::CollectLogs) {
        const std::wstring saveFolder = logSaveFolder(args.dryRun);
        if (args.dryRun) {
            writeOut("\r\nDry run: would save the log zip to " + hostPathFor(saveFolder) +
                     ". Nothing was written.\r\n");
            return 0;
        }
        const LogBundle bundle = collectLogs(survey.game.dir, saveFolder);
        for (const std::string& note : bundle.notes) writeOut("  " + note + "\r\n");
        if (!bundle.ok) {
            writeOut(bundle.error + "\r\n");
            return 1;
        }
        writeOut("\r\nSaved " + hostPathFor(bundle.zipPath) + "\r\n");
        for (const std::wstring& name : bundle.included) writeOut("  " + toUtf8(name) + "\r\n");
        if (!bundle.dumpZipPath.empty()) {
            writeOut("\r\nCrash dump (private; not for a public issue): " + hostPathFor(bundle.dumpZipPath) + "\r\n");
            for (const std::wstring& name : bundle.dumpEntries) writeOut("  " + toUtf8(name) + "\r\n");
        }
        return 0;
    }

#ifdef EDUP_LAUNCHER
    if (!havePayload && args.action != AppArgs::Act::Uninstall) {
        writeOut("\r\nThere is nothing to install from yet: " + payloadNote + "\r\n");
        return 5;
    }
#endif
    const bool canMirror = args.action != AppArgs::Act::Uninstall;

    if (canMirror && args.keepSettings) {
        const MirrorInfo mirror = readMirror(mirrorDir);
        const std::vector<std::string> missing = settingsToRestore(mirrorDir, mirror, scope);
        if (!missing.empty() && !survey.iniPresent && !survey.flatIniPresent) {
            const std::string wanted = settingsNamesOf(missing);
            writeOut("No " + wanted + " was found, but settings from an earlier install "
                     "were found in " + toUtf8(mirror.dir) + ", last saved " +
                     mirror.savedUtc + ".\r\n");
            if (args.dryRun) {
                writeOut("  Would restore them before installing -- skipped, this is a dry run. "
                         "The plan below is for this folder as it is now, with no " + wanted +
                         ".\r\n\r\n");
            } else {
                std::vector<std::string> notes;
                const bool restored = restoreFromMirror(survey.game.dir, mirror, scope, &notes, mirrorDir);
                for (const std::string& n : notes) writeOut("  " + n + "\r\n");
                if (!restored) {
                    writeOut("  Could not restore all of it -- carrying on with what is there now.\r\n");
                }
                survey = surveyTarget(game, mirrorDir);
                scope = scopeFor(args, survey);
                writeOut("\r\n");
            }
        }
    }

    Options options = optionsFor(args, args.action == AppArgs::Act::Repair);
    options.settingsDir = mirrorDir;
    const Plan plan = args.action == AppArgs::Act::Uninstall ? planUninstall(survey, options, payload)
                                                             : planInstall(survey, options, payload);
    writeOut(planReport(plan));

    if (plan.blocked) return 1;
    if (plan.nothingToDo) {
        // Nothing changed in the folder, but this may be the first run of an
        // installer new enough to mirror at all -- an install already at the
        // latest build must not have to wait for its next update to get one.
        if (canMirror && !args.dryRun) updateMirror(survey.game.dir, plan.backupDir, mirrorDir);
        if (args.action == AppArgs::Act::Uninstall && args.purge && !args.dryRun) {
            const std::vector<GameInstall> known = findInstalls();
            const bool shared = mirrorSharedWithAnotherInstall(survey.game, mirrorDir, mirrorRoot, known);
            std::vector<std::string> purgeNotes;
            if (purgeAfterUninstall(survey.game.dir, mirrorDir, &purgeNotes, !shared)) {
                for (const std::string& note : purgeNotes) writeOut("  " + note + "\r\n");
                writeOut("\r\nClean purge complete.\r\n");
            } else {
                for (const std::string& note : purgeNotes) writeOut("  " + note + "\r\n");
            }
        }
        return 0;
    }
    if (args.dryRun) return 0;

    if (needsElevationFor(survey) && !isElevated()) {
        writeOut("\r\nThis folder needs administrator rights. Run the same command from an "
                 "elevated prompt.\r\n");
        return 3;
    }

    const ApplyResult result = applyPlan(plan, payloadItem);
    for (const std::string& line : result.done) writeOut("  " + line + "\r\n");
    if (!result.ok) {
        writeOut("\r\n" + result.error + "\r\n");
        if (result.rolledBack) {
            writeOut("Installed files and metadata were restored; recovery backups were kept.\r\n");
        }
        return 1;
    }
    if (canMirror) {
        const MirrorResult m = updateMirror(survey.game.dir, plan.backupDir, mirrorDir);
        if (m.ok) writeOut(bullets(("\r\nMirrored to " + toUtf8(mirrorDir) +
                                    " (outside the game folder, so a game update wiping this "
                                    "folder cannot take it too):")
                                       .c_str(),
                                   m.saved));
    } else if (args.purge) {
        const std::vector<GameInstall> known = findInstalls();
        const bool shared = mirrorSharedWithAnotherInstall(survey.game, mirrorDir, mirrorRoot, known);
        std::vector<std::string> purgeNotes;
        purgeAfterUninstall(survey.game.dir, mirrorDir, &purgeNotes, !shared);
        for (const std::string& note : purgeNotes) writeOut("  " + note + "\r\n");
    }
    writeOut("\r\nDone.\r\n");
    return 0;
}

bool mirrorSharedWithAnotherInstall(const GameInstall& target, const std::wstring& mirrorDir,
                                    const std::wstring& mirrorRoot,
                                    const std::vector<GameInstall>& knownInstalls) {
    if (mirrorDir.empty() || mirrorRoot.empty()) return false;
    for (const GameInstall& other : knownInstalls) {
        if (_wcsicmp(other.dir.c_str(), target.dir.c_str()) == 0) continue;
        const std::wstring otherMirror = mirrorDirFor(other, mirrorRoot);
        if (!otherMirror.empty() && _wcsicmp(otherMirror.c_str(), mirrorDir.c_str()) == 0) return true;
    }
    return false;
}

bool purgeAfterUninstall(const std::wstring& gameDir, const std::wstring& mirrorDir,
                         std::vector<std::string>* notes, bool removeMirror) {
    bool removedAny = false;
#ifdef EDUP_LAUNCHER
    std::string shimError;
    const std::wstring edlaunch = findEdLaunch(gameDir);
    const size_t slash = edlaunch.find_last_of(L"\\/");
    const bool hadShim = !edlaunch.empty() && slash != std::wstring::npos &&
                         fileExists(joinPath(edlaunch.substr(0, slash), kEdLaunchMarkerName));
    if (!removeEdLaunchShim(gameDir, &shimError)) {
        if (!shimError.empty() && notes) notes->push_back(shimError);
    } else if (hadShim) {
        removedAny = true;
        if (notes) notes->push_back("Restored original EDLaunch.exe.");
    }
#else
    (void)gameDir;
#endif
    if (!mirrorDir.empty() && dirExists(mirrorDir)) {
        if (!removeMirror) {
            if (notes) {
                notes->push_back("Kept user settings folder " + toUtf8(mirrorDir) +
                                 " because another install shares it.");
            }
        } else if (removeDirectoryTree(mirrorDir)) {
            removedAny = true;
            if (notes) notes->push_back("Removed user settings folder " + toUtf8(mirrorDir) + ".");
        }
    }
    if (removeMirror && !mirrorDir.empty()) {
        const std::wstring legacyDir = legacyMirrorDirFor(mirrorDir);
        if (!legacyDir.empty() && dirExists(legacyDir) && removeDirectoryTree(legacyDir)) {
            removedAny = true;
            if (notes) notes->push_back("Removed legacy settings mirror " + toUtf8(legacyDir) + ".");
        }
    }
    return removedAny;
}

}  // namespace edvr::installer
