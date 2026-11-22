// The settings file's two names, spelled in this one place.
//
// The VR profile reads and writes edvr.ini. The flat profile keeps its own settings in
// edvr-flat.ini, so a flat install over a VR one (or back) never clobbers the other
// profile's tuning, and it reads edvr.ini INSTEAD, whole, only while edvr-flat.ini does not
// exist yet (Config::init, whose comment tells the fallback). What a flat process reads and
// writes is therefore one of two files, and a message that says "edvr.ini" for the other one
// sends somebody to edit a file the build never opens. That happened on 2026-09-30: a chained
// mod stayed loaded because the line that turned it off had been commented out in edvr.ini
// while edvr-flat.ini, the file the flat build read, still had it active, and the panel, the
// config audit and two hook notes had all said "edvr.ini".
//
// So no message in the DLLs spells either name. Config::iniName() says which file this process
// actually opened -- the path the loader chose, not a second guess from the profile -- and
// iniNameOfPath() is what it answers with, for a caller that holds a path of its own.
// tools\config_test scans the sources and fails the build for a message that spells a name,
// and holds the resolver to the four situations a process can be in.
//
// The installer is a different program with a different question: it manages BOTH files and
// says which it means (src/installer), so it is not held to this.
#pragma once

#include <cwchar>
#include <string>

namespace edvr {

inline constexpr const char* kIniNameVr = "edup.ini";               // the VR profile's file, and the flat profile's fallback
inline constexpr const char* kIniNameFlat = "edup-flat.ini";        // the flat profile's own file
inline constexpr const wchar_t* kIniNameVrW = L"edup.ini";
inline constexpr const wchar_t* kIniNameFlatW = L"edup-flat.ini";

// Legacy settings file names read before an older install is migrated to edup.ini / edup-flat.ini.
inline constexpr const char* kLegacyIniNameVr = "edvr.ini";
inline constexpr const char* kLegacyIniNameFlat = "edvr-flat.ini";
inline constexpr const wchar_t* kLegacyIniNameVrW = L"edvr.ini";
inline constexpr const wchar_t* kLegacyIniNameFlatW = L"edvr-flat.ini";

inline bool isFlatIniName(const char* name) {
    return name && (_stricmp(name, kIniNameFlat) == 0 || _stricmp(name, kLegacyIniNameFlat) == 0);
}

// The name of the settings file at `path`: its last component, compared without regard to case.
// Recognises both the current edup*.ini names and un-migrated legacy edvr*.ini names.
inline const char* iniNameOfPath(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const wchar_t* leaf = path.c_str() + (slash == std::wstring::npos ? 0 : slash + 1);
    if (_wcsicmp(leaf, kIniNameFlatW) == 0) return kIniNameFlat;
    if (_wcsicmp(leaf, kLegacyIniNameFlatW) == 0) return kLegacyIniNameFlat;
    if (_wcsicmp(leaf, kLegacyIniNameVrW) == 0) return kLegacyIniNameVr;
    return kIniNameVr;
}

}  // namespace edvr
