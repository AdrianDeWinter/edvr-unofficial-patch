// Ported from Steam.fs of min-ed-launcher, commit 139e4ff7ab9027925648fb27767cb87acdcf290a.
// MIT licence, "Copyright (c) 2017 Chris": see third_party\min_ed_launcher\LICENSE.
#include "steam_ticket.h"

#include <windows.h>

#include <cstdint>

#include "launch_route.h"

namespace edvr::installer {
namespace {

using InitFn = bool(__cdecl*)();
using ShutdownFn = void(__cdecl*)();
using ClientFn = void*(__cdecl*)();
using PipeFn = int(__cdecl*)();
using ConnectFn = int(__cdecl*)(void* client, int pipe);
using GetUserFn = void*(__cdecl*)(void* client, int user, int pipe, const char* version);
using SteamIdFn = std::uint64_t(__cdecl*)(void* user);
// Newer SDKs take a fifth argument, an identity pointer, which Steam.fs never passes. Passing null is
// harmless to an older library: the caller cleans the stack on x64.
using TicketFn = std::uint32_t(__cdecl*)(void* user, void* buffer, int size, std::uint32_t* count, const void* identity);
using CancelFn = void(__cdecl*)(void* user, std::uint32_t ticket);

class RealSteamSession : public SteamSession {
public:
    explicit RealSteamSession(std::wstring dllPath) : dllPath_(std::move(dllPath)) {}
    ~RealSteamSession() override { release(); }

    bool getTicket(std::string* ticketHex, std::string* error) override {
        auto fail = [&](const char* text) {
            if (error) *error = text;
            return false;
        };
        if (released_) return fail("the Steam session was already closed");
        // The Steam client's idea of which game this is, for as long as Steam is loaded here.
        if (!appId_) appId_.reset(new ScopedEnvironmentVariable(L"SteamAppId", kSteamAppId));
        if (dllPath_.empty() || dllPath_.find_first_of(L"\\/") == std::wstring::npos)
            return fail("steam_api64.dll was not found beside Elite's launcher");
        library_ = LoadLibraryExW(dllPath_.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!library_) return fail("steam_api64.dll could not be loaded from the game's folder");
        init_ = reinterpret_cast<InitFn>(GetProcAddress(library_, "SteamAPI_Init"));
        shutdown_ = reinterpret_cast<ShutdownFn>(GetProcAddress(library_, "SteamAPI_Shutdown"));
        client_ = reinterpret_cast<ClientFn>(GetProcAddress(library_, "SteamClient"));
        pipe_ = reinterpret_cast<PipeFn>(GetProcAddress(library_, "SteamAPI_GetHSteamPipe"));
        connect_ = reinterpret_cast<ConnectFn>(GetProcAddress(library_, "SteamAPI_ISteamClient_ConnectToGlobalUser"));
        getUser_ = reinterpret_cast<GetUserFn>(GetProcAddress(library_, "SteamAPI_ISteamClient_GetISteamUser"));
        steamId_ = reinterpret_cast<SteamIdFn>(GetProcAddress(library_, "SteamAPI_ISteamUser_GetSteamID"));
        ticket_ = reinterpret_cast<TicketFn>(GetProcAddress(library_, "SteamAPI_ISteamUser_GetAuthSessionTicket"));
        cancel_ = reinterpret_cast<CancelFn>(GetProcAddress(library_, "SteamAPI_ISteamUser_CancelAuthTicket"));
        if (!init_ || !shutdown_ || !client_ || !pipe_ || !connect_ || !getUser_ || !steamId_ || !ticket_ || !cancel_)
            return fail("steam_api64.dll is not a version the launcher can use");

        if (!init_()) return fail("Steam did not start a session for Elite. Make sure the Steam client is running");
        initialised_ = true;

        static const char kUnableToGetUser[] = "Steam would not give the launcher your account. Make sure the Steam client is running";
        void* client = client_();
        if (!client) return fail(kUnableToGetUser);
        const int pipe = pipe_();
        if (pipe == 0) return fail(kUnableToGetUser);
        const int globalUser = connect_(client, pipe);
        if (globalUser == 0) return fail(kUnableToGetUser);
        user_ = getUser_(client, globalUser, pipe, "SteamUser019");
        if (!user_) return fail(kUnableToGetUser);
        if (steamId_(user_) == 0) return fail(kUnableToGetUser);

        unsigned char raw[1024] = {};
        std::uint32_t count = 0;
        ticketHandle_ = ticket_(user_, raw, static_cast<int>(sizeof(raw)), &count, nullptr);
        if (ticketHandle_ == 0 || count == 0 || count > sizeof(raw)) {
            ticketHandle_ = 0;
            return fail(kUnableToGetUser);
        }
        static const char digits[] = "0123456789ABCDEF";
        std::string hex;
        hex.reserve(count * 2);
        for (std::uint32_t i = 0; i < count; ++i) {
            hex.push_back(digits[raw[i] >> 4]);
            hex.push_back(digits[raw[i] & 15]);
        }
        *ticketHex = hex;
        return true;
    }

    void release() override {
        if (released_) return;
        released_ = true;
        // The order Steam.fs disposes in: the ticket, then Steam.
        if (user_ && ticketHandle_ != 0 && cancel_) cancel_(user_, ticketHandle_);
        if (initialised_ && shutdown_) shutdown_();
        if (library_) FreeLibrary(library_);
        library_ = nullptr;
        user_ = nullptr;
        ticketHandle_ = 0;
        initialised_ = false;
        appId_.reset();  // SteamAppId as it was before getTicket
    }

private:
    std::wstring dllPath_;
    std::unique_ptr<ScopedEnvironmentVariable> appId_;
    HMODULE library_ = nullptr;
    bool released_ = false;
    bool initialised_ = false;
    void* user_ = nullptr;
    std::uint32_t ticketHandle_ = 0;
    InitFn init_ = nullptr;
    ShutdownFn shutdown_ = nullptr;
    ClientFn client_ = nullptr;
    PipeFn pipe_ = nullptr;
    ConnectFn connect_ = nullptr;
    GetUserFn getUser_ = nullptr;
    SteamIdFn steamId_ = nullptr;
    TicketFn ticket_ = nullptr;
    CancelFn cancel_ = nullptr;
};

}  // namespace

std::unique_ptr<SteamSession> makeSteamSession(const std::wstring& dllPath) {
    return std::unique_ptr<SteamSession>(new RealSteamSession(dllPath));
}

ScopedEnvironmentVariable::ScopedEnvironmentVariable(const wchar_t* name, const wchar_t* value) : name_(name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed > 0) {
        std::wstring old(needed, L'\0');
        SetLastError(ERROR_SUCCESS);  // an empty value reads as 0 characters with no error
        const DWORD got = GetEnvironmentVariableW(name, &old[0], needed);
        if (got > 0 && got < needed) {
            old.resize(got);
            old_ = old;
            hadOld_ = true;
        } else if (got == 0 && GetLastError() == ERROR_SUCCESS) {
            hadOld_ = true;  // set, and empty
        }
    }
    SetEnvironmentVariableW(name, value);
}

ScopedEnvironmentVariable::~ScopedEnvironmentVariable() { SetEnvironmentVariableW(name_.c_str(), hadOld_ ? old_.c_str() : nullptr); }

}  // namespace edvr::installer
