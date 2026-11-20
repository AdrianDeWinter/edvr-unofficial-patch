// fake_steam_api -- the steam_api64.dll launcher_test puts beside a fake EDLaunch.exe.
//
// It has the nine exports the launcher binds (src\installer\steam_ticket.cpp), so the real loader and the
// real export binding run, and it answers with a canned ticket. It talks to nothing: the real Steam client
// and the real steam_api64.dll are never involved.
//
// What it does is written to the file named by EDUP_FAKE_STEAM_LOG, one line per event, so the rig can
// prove the order: "load path=...", "init appid=<SteamAppId at that moment>", "client", "pipe",
// "global_user", "user version=...", "steamid", "ticket", "cancel ticket=N", "shutdown", "unload". How it
// answers is EDUP_FAKE_STEAM_MODE: empty for success, otherwise init_fails, no_client, no_pipe,
// no_global_user, no_user, zero_id or no_ticket.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

std::wstring envText(const wchar_t* name) {
    wchar_t buffer[1024]{};
    const DWORD n = GetEnvironmentVariableW(name, buffer, 1024);
    return n && n < 1024 ? std::wstring(buffer, n) : std::wstring();
}

std::string narrow(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

void logLine(const std::string& text) {
    const std::wstring path = envText(L"EDUP_FAKE_STEAM_LOG");
    if (path.empty()) return;
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const std::string line = text + "\n";
    DWORD written = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(f);
}

bool mode(const wchar_t* name) { return envText(L"EDUP_FAKE_STEAM_MODE") == name; }

int g_client = 0;
int g_user = 0;

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t path[MAX_PATH * 2]{};
        GetModuleFileNameW(module, path, MAX_PATH * 2);
        logLine("load path=" + narrow(path));
    } else if (reason == DLL_PROCESS_DETACH) {
        logLine("unload");
    }
    return TRUE;
}

extern "C" {

__declspec(dllexport) bool SteamAPI_Init() {
    logLine("init appid=" + narrow(envText(L"SteamAppId")));
    return !mode(L"init_fails");
}

__declspec(dllexport) void SteamAPI_Shutdown() { logLine("shutdown"); }

__declspec(dllexport) void* SteamClient() {
    logLine("client");
    return mode(L"no_client") ? nullptr : &g_client;
}

__declspec(dllexport) int SteamAPI_GetHSteamPipe() {
    logLine("pipe");
    return mode(L"no_pipe") ? 0 : 7;
}

__declspec(dllexport) int SteamAPI_ISteamClient_ConnectToGlobalUser(void*, int) {
    logLine("global_user");
    return mode(L"no_global_user") ? 0 : 3;
}

__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamUser(void*, int, int, const char* version) {
    logLine(std::string("user version=") + (version ? version : "(null)"));
    return mode(L"no_user") ? nullptr : &g_user;
}

__declspec(dllexport) std::uint64_t SteamAPI_ISteamUser_GetSteamID(void*) {
    logLine("steamid");
    return mode(L"zero_id") ? 0 : 76561198000000001ull;
}

// The canned ticket, as hex: 14000000ABCDEF010203.
__declspec(dllexport) std::uint32_t SteamAPI_ISteamUser_GetAuthSessionTicket(void*, void* buffer, int size, std::uint32_t* count) {
    logLine("ticket");
    static const unsigned char kTicket[] = {0x14, 0x00, 0x00, 0x00, 0xAB, 0xCD, 0xEF, 0x01, 0x02, 0x03};
    if (mode(L"no_ticket") || size < static_cast<int>(sizeof(kTicket))) return 0;
    std::memcpy(buffer, kTicket, sizeof(kTicket));
    *count = static_cast<std::uint32_t>(sizeof(kTicket));
    return 4242;
}

__declspec(dllexport) void SteamAPI_ISteamUser_CancelAuthTicket(void*, std::uint32_t ticket) {
    logLine("cancel ticket=" + std::to_string(ticket));
}

}  // extern "C"
