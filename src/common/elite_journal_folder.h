// Where Elite writes Journal.*.log and Status.json.
//
// The folder is under Windows' Saved Games folder:
//   <Saved Games>\Frontier Developments\Elite Dangerous
// Saved Games is a known folder (FOLDERID_SavedGames). Its own name is the
// user's language ("Saved Games", "Gespeicherte Spiele", and the rest) and
// the user can move it, so the path has to come from Windows. The publisher
// folder under it is not translated. d3d11.journal_dir, when set, is the
// Elite folder itself and replaces this. Header-only and free of any Windows
// API, the same way elite_graphics_folder.h is: the caller supplies the
// known-folder path.
#pragma once

#include <string>

namespace edvr {

// explicitDir: d3d11.journal_dir, used whole. knownSavedGames: the path Windows
// gave for Saved Games, empty if it gave none. userProfile: USERPROFILE, used
// only when Windows gave none, as "<profile>\Saved Games". Empty when none of
// the three can name a folder.
inline std::wstring eliteJournalFolder(const std::wstring& explicitDir,
                                      const std::wstring& knownSavedGames,
                                      const std::wstring& userProfile) {
    if (!explicitDir.empty()) return explicitDir;
    std::wstring root = knownSavedGames;
    if (root.empty()) {
        if (userProfile.empty()) return std::wstring();
        root = userProfile;
        if (root.back() != L'\\' && root.back() != L'/') root += L'\\';
        root += L"Saved Games";
    }
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    root += L"\\Frontier Developments\\Elite Dangerous";
    return root;
}

}  // namespace edvr
