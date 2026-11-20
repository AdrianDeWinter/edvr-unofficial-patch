// The Steam half of a direct launch: a session ticket from the Steam client that is running on
// this machine, which Frontier's servers accept in place of a password. A port of Steam.fs from
// min-ed-launcher (https://github.com/rfvgyhn/min-ed-launcher, commit
// 139e4ff7ab9027925648fb27767cb87acdcf290a, MIT licence, "Copyright (c) 2017 Chris"; the notice is
// third_party\min_ed_launcher\LICENSE).
//
// SteamSession is the interface direct_launch.cpp uses, so tools\launcher_test can put a fake in its
// place, and makeSteamSession is the real one. The real one:
//
//   * sets SteamAppId=359320 in this process while Steam is loaded, which is what makes the Steam
//     client see Elite, and puts back what was there on release(). The game needs it too (T3 of the
//     design relied on it), so a Steam install's start sets it again for that start alone
//     (DirectPlan::steamAppId). Left set, every later start from the same window inherited it: a
//     Frontier-store game, EDLaunch, a restarted launcher;
//   * loads steam_api64.dll by the absolute path it is given, from the game's install, with
//     LOAD_WITH_ALTERED_SEARCH_PATH, so a copy beside some other program is never picked up;
//   * calls SteamAPI_Init, then the client, pipe, global user and user interface, the Steam ID
//     (which must be non-zero) and GetAuthSessionTicket, and returns the ticket as hex.
//
// release() cancels the ticket, calls SteamAPI_Shutdown and unloads the library. The caller makes
// the call on every path, and always before the game is started; the destructor makes it once more.
#pragma once

#include <memory>
#include <string>

namespace edvr::installer {

class SteamSession {
public:
    virtual ~SteamSession() = default;
    // True with the ticket as upper-case hex. On false, `error` is a sentence for the window.
    virtual bool getTicket(std::string* ticketHex, std::string* error) = 0;
    // Idempotent. Nothing of Steam is left loaded or initialised afterwards.
    virtual void release() = 0;
};

// The real session over the steam_api64.dll at `dllPath` (absolute).
std::unique_ptr<SteamSession> makeSteamSession(const std::wstring& dllPath);

// One variable of this process's environment set for the object's lifetime, then put back as it was:
// the old value, or no variable at all. A child process inherits the environment as it is when it is
// started, so this gives one start a variable without giving it to the next.
class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const wchar_t* name, const wchar_t* value);
    ~ScopedEnvironmentVariable();
    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    std::wstring name_, old_;
    bool hadOld_ = false;
};

}  // namespace edvr::installer
