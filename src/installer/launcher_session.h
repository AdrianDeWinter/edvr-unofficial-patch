// What the launcher's window does besides installing: loading the payload, asking GitHub
// about an update, fetching it, switching to it, and starting and watching the game.
//
// Everything slow runs on a worker thread and reports back through `post`, which runs a
// job on the window's thread: the window never waits, and nothing here touches a control.
// All state lives on that one thread. The class owns no HWND, so tools\launcher_test drives
// it with a queue standing in for the message loop, a scripted GitHub, and fake programs.
//
// Nothing downloads or changes until the user presses Update (beginUpdate), and a failure to
// reach GitHub is one quiet line, never a dialog.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "direct_launch.h"
#include "launch_route.h"
#include "launcher_core.h"
#include "launcher_plan.h"
#include "selfupdate.h"
#include "updater.h"

namespace edvr::installer {

// What the window asks before it turns direct launch on. No is the default button, and a No leaves it off.
inline constexpr const char* kDirectLaunchQuestion =
    "Start Elite directly instead of through Frontier's launcher?\n\n"
    "The launcher signs in to Frontier's servers itself with your Steam login, then starts the game. "
    "It starts faster, and you can pick VR or flat here. No password or token is stored.\n\n"
    "Frontier has not approved this. It could act against an account that signs in this way, and the choice "
    "is yours. Untick this at any time to go back to Frontier's launcher.\n\n"
    "Turn it on?";

// Epic still starts EDLaunch.exe and passes the exchange code. The stand-in signs in to Epic and
// Frontier, starts Elite itself, and exits once Elite is running. Frontier's binary is kept aside
// until the box is unticked.
inline constexpr const char* kDirectLaunchQuestionEpic =
    "Skip the Epic launcher and pick VR or flat here?\n\n"
    "The stand-in signs in to Epic and Frontier, then starts Elite itself. You can pick VR or flat here. "
    "Frontier's EDLaunch is kept as EDLaunch_orig.exe until you untick this, and this program exits once "
    "Elite is running.\n\n"
    "Frontier has not approved this. It could act against an account that signs in this way, and the choice "
    "is yours. Untick this at any time to put Frontier's EDLaunch back.\n\n"
    "Turn it on?";

// The same question for an install from Frontier's own store, where the launcher asks for the login itself.
// Unticking deletes what it stored, and the question says so.
inline constexpr const char* kDirectLaunchQuestionFrontier =
    "Start Elite directly instead of through Frontier's launcher?\n\n"
    "The launcher signs in to Frontier's servers itself, then starts the game. It starts faster, and you can "
    "pick VR or flat here. It will ask for your Frontier email and password, and may need a code Frontier "
    "emails you, once on this PC. The login is stored encrypted for this Windows user; the password only if "
    "you tick Remember my password.\n\n"
    "Frontier has not approved this. It could act against an account that signs in this way, and the choice "
    "is yours. Untick this at any time to go back to Frontier's launcher; unticking also deletes the stored login.\n\n"
    "Turn it on?";

// Asked the first time the user turns on the optional tailored 2DMigoto runtime for EDHM.
// No is the default button, and a No leaves it off.
inline constexpr const char* kEdhm2dMigotoQuestion =
    "Use the experimental tailored 2DMigoto d3d11_edhm.dll for EDHM?\n\n"
    "When turned on, ED-UP backs up your current standard d3d11_edhm.dll and installs the "
    "signed tailored 2DMigoto compatibility build from Fenris159/EDHM_2DMigoto (derived from "
    "3Dmigoto / XXMI, licensed under GNU GPLv3, with PCRE2 BSD and DirectXTK MIT notices "
    "shipped beside the game and listed under --licenses).\n\n"
    "If d3dx.ini is present, ED-UP also backs up your current [System] load_library_redirect, "
    "[System] allow_platform_update, and [Device] upscaling values and sets them to 0, 1, and 0 "
    "for better compatibility with ED-UP.\n\n"
    "Turning this off restores your backed-up original d3d11_edhm.dll and d3dx.ini settings. "
    "If you later update EDHM yourself, ED-UP will not silently overwrite your newer DLL.\n\n"
    "Turn it on?";

// Shown below the Settings option when the tailored 2DMigoto DLL goes missing (e.g. antivirus
// quarantine) and the launcher automatically unchecks the option and restores the original.
inline constexpr const char* kEdhmInterferenceWarning =
    "Something has interfered with the download, please check your antivirus and try again.";

enum class PayloadState { Loading, Ready, None };
enum class CheckState { NotYet, Checking, UpToDate, Available, Refused, Skipped };

struct SessionEnv {
    LauncherRoots roots;
    std::wstring payloadDir;       // --payload, or empty
    std::string  channelOverride;  // --channel for this run, or empty to read launcher.ini
    bool         closeAfterStartArg = false;  // --close-after-start for this run; the saved setting is read too
    bool         frontierLoginArg = false;    // --frontier-login for this run; uses Frontier account on Steam direct launch
    bool         updatedFlag = false;  // started by a self-update: carry on into the update
    std::wstring selfExe;          // the running exe: hashed for the check, replaced by self-update
    const char* const* keys = nullptr;  // null means the release keys
    std::string  apiBase;          // empty means GitHub

    // Test seams. Empty means the real one.
    std::function<std::unique_ptr<HttpClient>()> makeHttp;
    FileMover move;
    std::function<bool(const std::wstring& exe)> startNewLauncher;

    // The window's side. `post` runs a job on the window's thread.
    std::function<void(std::function<void()>)> post;
    std::function<void()> changed;          // state moved: repaint, re-enable
    std::function<void()> payloadSwitched;  // an update was adopted and the payload is now it: carry on to the install
    std::function<void()> closeWindow;      // the new launcher is running: this one is done
    // The game started (true) or is over (false), on every route. The window is minimised while the game
    // runs and comes back when it exits, and stays open for another Play.
    std::function<void(bool aside)> windowAside;
    // Close-after-start (the setting or --close-after-start): when the game has started, closeWindow is
    // called in place of windowAside(true). The window posts WM_CLOSE; headless play ends its loop.

    // False where direct launch is not offered (the launcher under Wine on Linux): directLaunch() is then false
    // even if launcher.ini says direct_launch=on, Play never takes the direct path, and the window hides the
    // checkbox. The saved setting itself is left alone.
    bool directLaunchOffered = true;

    // Direct launch's seams. Empty or default means the real one; the rig uses its own.
    std::string frontierBase;                  // empty means api.zaonce.net
    std::function<std::unique_ptr<SteamSession>(const std::wstring& dllPath)> makeSteam;
    MachineIdKeys machineKeys;
    ApiClock clock;
    GameStarter startGame;                     // returns the process to wait on; empty means startGameProcess
    // A Frontier-store install's questions. Each runs on the window's thread, shows its modal window and
    // returns the answer, false for cancelled. The session asks from its worker and waits. Empty: a
    // Frontier-store install cannot ask, so direct launch stops where it would have.
    std::function<bool(const std::string& note, LoginForm* form)> askLogin;
    std::function<bool(const std::string& email, const std::string& note, std::string* code)> askCode;

    // Optional pre-launch Settings.xml helpers for store routes that do not pass /vr or /novr
    // (Steam and Epic non-direct):
    // - detectVrMode: reads an existing 3..6 VR mode from Settings.xml to seed vr_3d_mode once.
    // - prepareLaunchMode: writes Settings.xml before startRoute; returning false stops Play.
    // - restoreLaunchMode: rolls Settings.xml back from its snapshot if startRoute fails.
    std::function<edvr::VrHeadsetMode(const std::wstring& gameDir)> detectVrMode;
    std::function<bool(bool vr, edvr::VrHeadsetMode preferredVrMode, std::string* error)> prepareLaunchMode;
    std::function<void()> restoreLaunchMode;
};

class LauncherSession {
public:
    explicit LauncherSession(SessionEnv env);
    ~LauncherSession() { shutdown(); }
    LauncherSession(const LauncherSession&) = delete;
    LauncherSession& operator=(const LauncherSession&) = delete;

    // Loads the payload and checks for an update, both on worker threads.
    void start();
    // Cancels what is running and waits for the workers. Leaves the game and EDLaunch alone.
    void shutdown();

    PayloadState payloadState() const { return payloadState_; }
    const PayloadLoad& payload() const { return payload_; }

    CheckState checkState() const { return checkState_; }
    const UpdateCheck& check() const { return check_; }
    // The update line: "Checking...", "ED-UP 0.23.0 is available.", progress, or the quiet reason.
    const std::string& updateLine() const { return updateLine_; }
    bool updating() const { return updating_; }

    bool includeReleaseCandidates() const { return channel_ == "rc"; }
    // Saves the channel to launcher.ini and asks again.
    void setIncludeReleaseCandidates(bool include);

    // The Update button. Self-update first if the manifest wants a newer launcher (and this is
    // the canonical copy), restarting; then the cache, the manifest and the payload.
    void beginUpdate();

    // Starts the game and watches until it is over. `elevated` refuses, before anything is contacted.
    // A Steam or Frontier-store install ("Frontier launcher") with direct launch on signs in to Frontier and
    // starts the game itself, and falls back to its own route, saying why, when it cannot. A cancelled
    // question starts nothing and says "Sign-in cancelled." Every other install takes its route.
    // The Epic stand-in: our exe takes EDLaunch.exe's name, and Frontier's binary is kept beside it.
    // `on` false puts the original back. The mode file is written only when turning it on.
    bool standInForEdLaunch(const std::wstring& gameDir, bool on, bool vr, std::string* error);
    void beginPlay(const std::wstring& source, const std::wstring& gameDir, bool vr, bool elevated,
                   const RouteOps& ops, const WatchNames& names, const WatchTiming& timing);
    bool playing() const { return playing_; }
    const std::string& playLine() const { return playLine_; }

    // launcher.ini's direct_launch: off unless it says on. Turning it on calls `confirm` first (the
    // window's question), and a false from it leaves the setting as it was. Turning it off deletes the
    // stored Frontier login. Returns the setting after.
    // The saved setting, unless this platform does not offer direct launch (SessionEnv::directLaunchOffered).
    bool directLaunch() const { return directLaunch_ && env_.directLaunchOffered; }
    bool directLaunchOffered() const { return env_.directLaunchOffered; }
    bool setDirectLaunch(bool on, const std::function<bool()>& confirm);

    // Whether the launcher closes itself once the game has started: the saved setting, or --close-after-start.
    bool closeAfterStart() const { return closeAfterStart_ || env_.closeAfterStartArg; }
    // The saved setting alone, which is what the window's checkbox shows.
    bool closeAfterStartSetting() const { return closeAfterStart_; }
    // Whether direct launch on a Steam install signs in with a Frontier account instead of Steam.
    bool steamFrontierLogin() const { return steamFrontierLogin_ || env_.frontierLoginArg; }
    bool steamFrontierLoginSetting() const { return steamFrontierLogin_; }
    // `clearSavedLogin`: delete the shared frontier-login.dat when transitioning on -> off.
    // Pass false when a Frontier-store install also exists so its saved login is preserved.
    void setSteamFrontierLogin(bool on, bool clearSavedLogin = false);
    // The saved language code, or empty when the file names none (the window then uses Windows).
    const std::string& language() const { return language_; }
    void setLanguage(const std::string& code);
    // Preferred VR headset mode, or VrHeadsetMode::NotSet when unset.
    edvr::VrHeadsetMode vrStereoMode() const { return vrStereoMode_; }
    void setVrStereoMode(edvr::VrHeadsetMode mode);
    // If vrStereoMode() is NotSet and env_.detectVrMode finds an existing 3..6 mode in
    // Settings.xml, persists that mode to launcher.ini as the new saved default.
    edvr::VrHeadsetMode seedVrStereoModeFromSettings(const std::wstring& gameDir);
    // Saves the setting to launcher.ini. The window's checkbox; --close-after-start does not write it.
    void setCloseAfterStart(bool on);
    // Optional tailored 2DMigoto runtime for EDHM (launcher.ini edhm_2dmigoto).
    // Turning it on calls `confirm` first; a false leaves it off. Turning it off never asks.
    bool edhm2dMigoto() const { return edhm2dMigoto_; }
    bool setEdhm2dMigoto(bool on, const std::function<bool()>& confirm);
    // Transient warning displayed under the EDHM 2DMigoto setting (e.g. antivirus interference).
    const std::string& edhmWarning() const { return edhmWarning_; }
    void setEdhmWarning(const std::string& warning);
    // True once the game has been seen running (or the direct start gave a process), for the run's exit code.
    bool gameStarted() const { return gameStarted_; }

    // The elevated apply, waited for on a worker. `done` runs on the window's thread.
    void beginElevatedApply(const PlanFile& plan, const ElevatedRunner& runner,
                            std::function<void(const ElevatedResult&)> done);
    bool applying() const { return applying_; }

private:
    void beginCheck();
    void checkDone(const UpdateCheck& result);
    void updateFailed(const std::string& why);
    void finishSelfUpdate();
    void payloadLoaded(const PayloadLoad& load);
    UpdaterSettings updaterSettings() const;
    std::unique_ptr<HttpClient> makeHttp() const;
    void post(std::function<void()> job) { env_.post(std::move(job)); }
    void changed() {
        if (env_.changed) env_.changed();
    }
    void spawn(std::function<void()> work) { workers_.emplace_back(std::move(work)); }
    // Writes last_install, and the install's mode unless `mode` is null.
    void rememberPlay(const std::wstring& gameDir, const char* mode);
    // The game is running, on the window's thread: the window steps aside, or closes when close-after-start is on.
    void gameIsRunning();
    // `direct` is the skip-the-launcher checkbox. Only an Epic install uses it here: Steam and
    // the Frontier store sign in themselves before this is called.
    // `allowUnsetVrMode` is true on a direct-launch fallback: the VR 3D mode selector is hidden
    // while direct launch is on, so an unset mode skips the Settings.xml write rather than failing.
    void startRouteAndWatch(const std::wstring& source, const std::wstring& gameDir, bool vr, bool elevated,
                            const RouteOps& ops, const WatchNames& names, const WatchTiming& timing,
                            const std::string& startLine, bool direct = false, bool allowUnsetVrMode = false);
    void beginDirectPlay(const std::wstring& source, const std::wstring& gameDir, bool vr, const RouteOps& ops,
                         const WatchNames& names, const WatchTiming& timing);
    // Posts `ask` to the window's thread and waits for it from the worker. False when it was cancelled or
    // the session is closing. The answers travel in shared state, so a worker that gives up leaves nothing dangling.
    struct Asked;
    std::shared_ptr<Asked> askOnWindow(const std::function<bool(Asked&)>& ask);

    SessionEnv env_;
    std::vector<std::thread> workers_;
    std::atomic<bool> cancel_{false};

    PayloadState payloadState_ = PayloadState::Loading;
    PayloadLoad payload_;
    CheckState checkState_ = CheckState::NotYet;
    UpdateCheck check_;
    std::string updateLine_;
    std::string channel_ = "stable";
    bool updating_ = false;
    bool autoResumed_ = false;  // the update that followed a self-update's restart was started
    bool playing_ = false;
    bool directLaunch_ = false;
    bool closeAfterStart_ = false;
    bool steamFrontierLogin_ = false;
    bool edhm2dMigoto_ = false;
    std::string edhmWarning_;
    bool gameStarted_ = false;
    edvr::VrHeadsetMode vrStereoMode_ = edvr::VrHeadsetMode::NotSet;
    std::string language_;
    bool applying_ = false;
    std::string playLine_;
};

}  // namespace edvr::installer
