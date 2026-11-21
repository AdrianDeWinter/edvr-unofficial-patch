// Entry point: window by default, console when the command line asks for
// something specific.
//
// Built as a GUI-subsystem executable so that double-clicking it -- which is
// what nearly everybody will do -- opens the window with no console flashing
// behind it. That costs one piece of plumbing: a GUI process launched from a
// prompt has no console of its own, so a command-line run has to borrow the
// prompt's. If it cannot (launched from Explorer with arguments, say), it falls
// back to the window with those arguments prefilled rather than doing its work
// invisibly.
#include <windows.h>
#include <shellapi.h>

#include "app.h"
#ifdef EDUP_LAUNCHER
#include "launcher_main.h"
#endif

namespace {

// True when there is somewhere for text to go: a redirected handle inherited
// at startup, or the parent's console.
bool attachToConsole() {
    HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing && existing != INVALID_HANDLE_VALUE) return true;

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    HANDLE out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    SetStdHandle(STD_OUTPUT_HANDLE, out);
    SetStdHandle(STD_ERROR_HANDLE, out);
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    using namespace edvr::installer;

#ifdef EDUP_LAUNCHER
    // Epic starts the file named EDLaunch.exe and passes the exchange code on its command
    // line. When that file is our stand-in, it signs in, starts Elite, and this process exits.
    const int shim = runEdLaunchShimIfInvoked();
    if (shim >= 0) return shim;
#endif

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const AppArgs args = parseArgs(argc ? argc : 0, argv);
    if (argv) LocalFree(argv);

#ifdef EDUP_LAUNCHER
    // The elevated copy has one job and shows nothing; the update and unregister commands print
    // to a console when there is one.
    if (!args.badArg && !args.planFile.empty()) return runApplyPlanFile(args);
    // --headless never opens the GUI window, even when no console is attached (e.g. Steam %command%).
    if (args.headless && !args.help) {
        if (args.badArg) {
            if (attachToConsole()) runConsole(args);
            return 2;
        }
        return runHeadless(args, attachToConsole());
    }
    if (!args.badArg && !args.help && launcherHasCommand(args)) {
        const int code = runLauncherCommand(args, attachToConsole());
        // Allow chaining --update with --install or --repair for headless automation (e.g. min-ed-launcher).
        // Exit code 4 means the update check was skipped (offline/rate-limit); proceed with any already-adopted
        // payload rather than skipping the install. Exit code 3 (refused manifest) or 1 (download failure) stops.
        if (!args.update || args.action == AppArgs::Act::None) return code;
        if (code != 0 && code != 4) return code;
        return runConsole(args);
    }
#endif

    // --autorun is the elevated relaunch of a window session: it belongs in the
    // window, showing the result, not in a console nobody is watching.
    const bool wantsConsole =
        !args.autorun && (args.action != AppArgs::Act::None || args.help || args.dryRun ||
                          args.badArg);

    if (wantsConsole && attachToConsole()) return runConsole(args);
#ifdef EDUP_LAUNCHER
    int startupCode = 0;
    if (!launcherStartup(args, &startupCode)) return startupCode;
#endif
    return runGui(instance, args);
}
