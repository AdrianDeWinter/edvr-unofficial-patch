// launcher.ini: the launcher's own memory, one key=value per line.
//
//   channel            stable or rc
//   current_manifest   SHA-256 of the manifest the payload was last switched to
//   highest_issued     the highest `issued` of any manifest accepted; never lowered
//   direct_launch      on: a Steam install, or one from Frontier's own store, is started by the
//                      launcher itself, without Frontier's launcher. Missing, or anything but "on",
//                      is off. Turning it off also deletes frontier-login.dat (frontier_login.h).
//   close_after_start  on: the launcher closes itself once Elite has started, instead of staying
//                      open until it exits. Missing, or anything but "on", is off.
//   mode.<16 hex>      the play mode remembered for one install folder: the first
//                      16 hex characters of the SHA-256 of the folder's path,
//                      lower-cased, with backslashes and no trailing separator
//   last_install       the install folder Play was last pressed for, as a path; the
//                      window opens on it while it is still an install it can use
//
// A line this code does not know (another version's key, a comment) is kept as
// it was, in place, so a newer launcher's file survives an older one. Saving is
// atomic: the file is the old bytes or the whole new text.
#pragma once

#include <string>
#include <vector>

namespace edvr::installer {

class LauncherState {
public:
    // Reads `path`. A missing file is an empty state and succeeds; a file that
    // exists and cannot be read fails.
    bool load(const std::wstring& path, std::string* error);

    // Writes the file back through the shared atomic writer. The folder must exist.
    bool save(std::string* error) const;

    const std::wstring& path() const { return path_; }

    std::string get(const std::string& key) const;
    void set(const std::string& key, const std::string& value);

    // "stable" unless the file says "rc".
    std::string channel() const;
    void setChannel(const std::string& channel);

    // A 64-character lower-case hex hash, or empty.
    std::string currentManifest() const;
    void setCurrentManifest(const std::string& sha256);

    long long highestIssued() const;
    // Raises the stored value; a lower or equal one changes nothing.
    void raiseHighestIssued(long long issued);

    // True only when the file says "on". Off is the default, and writing it removes no line: it says "off".
    bool directLaunch() const;
    void setDirectLaunch(bool on);

    // True only when the file says "on". Off is the default.
    bool closeAfterStart() const;
    void setCloseAfterStart(bool on);

    static std::string modeKey(const std::wstring& installFolder);
    std::string mode(const std::wstring& installFolder) const;
    void setMode(const std::wstring& installFolder, const std::string& mode);

    // Empty when the file has none.
    std::wstring lastInstall() const;
    void setLastInstall(const std::wstring& installFolder);

    // The same folder by modeKey's rules: case, slash direction and a trailing separator do not count.
    static bool sameInstall(const std::wstring& a, const std::wstring& b);

private:
    std::wstring path_;
    std::vector<std::string> lines_;
};

// The index of `remembered` in `folders`, by sameInstall; folders.size() when it is empty or not there.
size_t rememberedInstallIndex(const std::vector<std::wstring>& folders, const std::wstring& remembered);

struct GameInstall;  // detect.h

// Which install the window opens on, given `remembered` (last_install). kNoRememberedInstall when there is none
// to use: nothing remembered, or a folder that is gone, holds no game, or holds a legacy one. Otherwise an index
// into `installs`, or installs.size() when the folder is not in the list but still holds a game the window takes
// (one that was browsed to): *restored then holds it, described as Browse would, and the caller appends it. A
// folder that describes into one already listed (a launcher folder above it, say) is that one, not a second entry.
// `describe` and `isLegacyExe` are describeDir and eliteExeIsLegacy, or the rig's stand-ins.
constexpr size_t kNoRememberedInstall = static_cast<size_t>(-1);
size_t rememberedInstallChoice(const std::vector<GameInstall>& installs, const std::wstring& remembered,
                               bool (*describe)(const std::wstring&, const std::wstring&, GameInstall*),
                               bool (*isLegacyExe)(const std::wstring&), GameInstall* restored);

}  // namespace edvr::installer
