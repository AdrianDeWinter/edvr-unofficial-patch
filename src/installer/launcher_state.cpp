#include "launcher_state.h"

#include <windows.h>

#include <cstdlib>

#include "../common/iniedit.h"
#include "detect.h"
#include "probe.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

bool lineHasKey(const std::string& line, const std::string& key) {
    return line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=';
}

std::string singleLine(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c != '\r' && c != '\n') out.push_back(c);
    }
    return out;
}

}  // namespace

bool LauncherState::load(const std::wstring& path, std::string* error) {
    path_ = path;
    lines_.clear();
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
        if (error) *error = "launcher.ini cannot be examined";
        return false;
    }
    std::string text;
    if (!readFileBytes(path, &text, 1u << 20, error)) return false;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines_.push_back(std::move(line));
        start = end + 1;
    }
    return true;
}

bool LauncherState::save(std::string* error) const {
    std::string text;
    for (const std::string& line : lines_) {
        text += line;
        text += '\n';
    }
    std::wstring why;
    if (!writeFileAtomic(path_, text, &why)) {
        if (error) *error = "launcher.ini could not be written";
        return false;
    }
    return true;
}

std::string LauncherState::get(const std::string& key) const {
    // The last occurrence wins, as it would for a person editing the file.
    std::string value;
    for (const std::string& line : lines_) {
        if (lineHasKey(line, key)) value = line.substr(key.size() + 1);
    }
    return value;
}

void LauncherState::set(const std::string& key, const std::string& value) {
    const std::string line = key + "=" + singleLine(value);
    for (std::string& existing : lines_) {
        if (lineHasKey(existing, key)) {
            existing = line;
            return;
        }
    }
    lines_.push_back(line);
}

std::string LauncherState::channel() const { return get("channel") == "rc" ? "rc" : "stable"; }

void LauncherState::setChannel(const std::string& channel) { set("channel", channel == "rc" ? "rc" : "stable"); }

std::string LauncherState::currentManifest() const {
    const std::string value = get("current_manifest");
    return isLowerHex(value, 64) ? value : std::string();
}

void LauncherState::setCurrentManifest(const std::string& sha256) { set("current_manifest", sha256); }

long long LauncherState::highestIssued() const {
    const std::string value = get("highest_issued");
    if (value.empty() || value.size() > 18) return 0;
    for (char c : value) {
        if (c < '0' || c > '9') return 0;
    }
    return std::strtoll(value.c_str(), nullptr, 10);
}

void LauncherState::raiseHighestIssued(long long issued) {
    if (issued > highestIssued()) set("highest_issued", std::to_string(issued));
}

bool LauncherState::directLaunch() const { return get("direct_launch") == "on"; }

void LauncherState::setDirectLaunch(bool on) { set("direct_launch", on ? "on" : "off"); }

bool LauncherState::closeAfterStart() const { return get("close_after_start") == "on"; }

void LauncherState::setCloseAfterStart(bool on) { set("close_after_start", on ? "on" : "off"); }

bool LauncherState::steamFrontierLogin() const { return get("steam_frontier_login") == "on"; }

void LauncherState::setSteamFrontierLogin(bool on) { set("steam_frontier_login", on ? "on" : "off"); }

std::string LauncherState::modeKey(const std::wstring& installFolder) {
    std::wstring folder = installFolder;
    for (wchar_t& c : folder) {
        if (c == L'/') c = L'\\';
    }
    while (folder.size() > 1 && folder.back() == L'\\') folder.pop_back();
    if (!folder.empty()) CharLowerBuffW(&folder[0], static_cast<DWORD>(folder.size()));
    const std::string utf8 = narrowUtf8(folder);
    return "mode." + sha256Bytes(utf8.data(), utf8.size()).substr(0, 16);
}

std::string LauncherState::mode(const std::wstring& installFolder) const { return get(modeKey(installFolder)); }

void LauncherState::setMode(const std::wstring& installFolder, const std::string& mode) {
    set(modeKey(installFolder), mode);
}

std::wstring LauncherState::lastInstall() const { return widenUtf8(get("last_install")); }

void LauncherState::setLastInstall(const std::wstring& installFolder) { set("last_install", narrowUtf8(installFolder)); }

std::string LauncherState::language() const { return get("language"); }

void LauncherState::setLanguage(const std::string& code) { set("language", code); }

edvr::VrHeadsetMode LauncherState::vrStereoMode() const {
    return edvr::vrHeadsetModeFromConfig(get("vr_3d_mode"));
}

void LauncherState::setVrStereoMode(edvr::VrHeadsetMode mode) {
    set("vr_3d_mode", edvr::vrHeadsetModeConfigValue(mode));
}

bool LauncherState::edhm2dMigoto() const { return get("edhm_2dmigoto") == "on"; }

void LauncherState::setEdhm2dMigoto(bool on) { set("edhm_2dmigoto", on ? "on" : "off"); }

std::string LauncherState::edhmOptInFailedManifest() const {
    const std::string value = get("edhm_optin_failed_manifest");
    return isLowerHex(value, 64) ? value : std::string();
}

void LauncherState::setEdhmOptInFailedManifest(const std::string& sha256) {
    set("edhm_optin_failed_manifest", sha256);
}

bool LauncherState::sameInstall(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && modeKey(a) == modeKey(b);
}

size_t rememberedInstallIndex(const std::vector<std::wstring>& folders, const std::wstring& remembered) {
    for (size_t i = 0; i < folders.size(); ++i) {
        if (LauncherState::sameInstall(folders[i], remembered)) return i;
    }
    return folders.size();
}

size_t rememberedInstallChoice(const std::vector<GameInstall>& installs, const std::wstring& remembered,
                               bool (*describe)(const std::wstring&, const std::wstring&, GameInstall*),
                               bool (*isLegacyExe)(const std::wstring&), GameInstall* restored) {
    if (remembered.empty()) return kNoRememberedInstall;
    std::vector<std::wstring> folders;
    for (const GameInstall& game : installs) folders.push_back(game.dir);
    const size_t listed = rememberedInstallIndex(folders, remembered);
    if (listed < folders.size()) return listed;
    GameInstall game;
    if (!describe || !describe(remembered, L"Folder you chose", &game)) return kNoRememberedInstall;
    if (isLegacyExe && withoutLegacyInstalls({game}, isLegacyExe).empty()) return kNoRememberedInstall;
    const size_t same = rememberedInstallIndex(folders, game.dir);
    if (same < folders.size()) return same;
    *restored = game;
    return installs.size();
}

}  // namespace edvr::installer
