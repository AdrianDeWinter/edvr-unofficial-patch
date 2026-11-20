// fake_proc -- the programs launcher_test starts in place of EDLaunch and Elite.
//
// One source, built twice by build.bat into build\launcher_test_fakes\:
//   ed_up_fake_edlaunch.exe   (-DFAKE_EDLAUNCH) has a top-level window that is never shown, so
//                             the watcher has a window to post WM_CLOSE to without anything
//                             appearing on the desktop;
//   ed_up_fake_game.exe       has no window at all.
// Neither is named like a real program, so nothing the rig starts can be taken for the real thing.
//
//   --run-ms N     exit after N milliseconds (default: run until closed or ended by the rig)
//   --log FILE     append one line per event: "started", "WM_CLOSE", "exiting"
//   --obey-close   (EDLaunch) leave when WM_CLOSE arrives; without it WM_CLOSE is logged and ignored
//
// A GUI-subsystem program, so no console is ever created for it.
#include <windows.h>
#include <shellapi.h>

#include <string>

namespace {

std::wstring g_log;
bool g_obeyClose = false;

void logLine(const char* text) {
    if (g_log.empty()) return;
    HANDLE f = CreateFileW(g_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const std::string line = std::string(text) + "\n";
    DWORD written = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(f);
}

#ifdef FAKE_EDLAUNCH
LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CLOSE:
            logLine("WM_CLOSE");
            if (g_obeyClose) DestroyWindow(window);
            return 0;
        case WM_TIMER:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}
#endif

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    DWORD runMs = 0;
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--run-ms" && i + 1 < argc) runMs = static_cast<DWORD>(_wtoi(argv[++i]));
        else if (arg == L"--log" && i + 1 < argc) g_log = argv[++i];
        else if (arg == L"--obey-close") g_obeyClose = true;
    }
    if (argv) LocalFree(argv);
    logLine("started");

#ifdef FAKE_EDLAUNCH
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = proc;
    cls.hInstance = instance;
    cls.lpszClassName = L"EdUpFakeEdlaunch";
    RegisterClassExW(&cls);
    // WS_POPUP and never ShowWindow: a real top-level window that EnumWindows finds and that
    // WM_CLOSE reaches, and that the desktop never draws.
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"ed-up fake edlaunch", WS_POPUP, 0, 0, 0, 0,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    if (runMs) SetTimer(window, 1, runMs, nullptr);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
#else
    (void)instance;
    Sleep(runMs ? runMs : INFINITE);
#endif
    logLine("exiting");
    return 0;
}
