#include "plan.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <utility>

#include "../common/install_scope.h"
#include "../common/real_dll_choice.h"

namespace edvr::installer {
namespace {

// The one settings key the chain lives in. It is one decision for the install, kept once in the shared edvr.ini and read by both
// profiles (real_dll_choice.h); edvr-flat.ini holds it only to say `none` for the flat profile.
const char* const kChainKey = "advanced.real_dll";

// The descriptors EDVR writes, by their exact bytes (CRLF, as every writer produces them). Schema 1 is what the installer and the
// flat edition wrote before the unified install; schema 2 is what install_edvr.py and the unified installer write. A file is EDVR's
// only as one of these bytes: a modified or unknown descriptor matches none of them, whatever it says.
const char* const kDescriptorTexts[] = {
    "[install]\r\nschema = 1\r\nprofile = vr\r\n",
    "[install]\r\nschema = 1\r\nprofile = flat\r\n",
    "[install]\r\nschema = 2\r\nscope = both\r\n",
    "[install]\r\nschema = 2\r\nscope = vr\r\n",
    "[install]\r\nschema = 2\r\nscope = flat\r\n",
};

// The scope a canonical descriptor declares, read through the same parser the DLL uses. False when the hash is none of EDVR's texts.
bool ownDescriptorScope(const std::string& sha, edvr::InstallScope* scope) {
    for (const char* text : kDescriptorTexts) {
        const std::string bytes(text);
        if (sha != sha256Bytes(bytes.data(), bytes.size())) continue;
        *scope = edvr::parseInstallDescriptor(bytes).scope;
        return true;
    }
    return false;
}

// The edition a scope installs: both and vr carry the VR runtime, so they are the VR edition; flat is the flat edition.
std::string editionOfScope(edvr::InstallScope scope) {
    return scope == edvr::InstallScope::Flat ? "flat" : "vr";
}

// Whether `sha` is one of EDVR's own descriptor texts for `edition`.
bool isOwnDescriptor(const std::string& sha, const std::string& edition) {
    edvr::InstallScope scope = edvr::InstallScope::Invalid;
    return ownDescriptorScope(sha, &scope) && editionOfScope(scope) == edition;
}

// The scopes the installer installs, by the names the descriptor and the record use.
bool knownScope(const std::string& scope) { return scope == "both" || scope == "vr" || scope == "flat"; }

// The descriptor a scope writes: the canonical schema-2 text, byte for byte (kDescriptorTexts holds the same bytes).
std::string descriptorTextFor(const std::string& scope) {
    return "[install]\r\nschema = 2\r\nscope = " + scope + "\r\n";
}

// The edition a scope belongs to, as the record's profile names it: flat for the flat scope, vr for the others.
std::string editionFor(const std::string& scope) { return scope == "flat" ? "flat" : "vr"; }

const wchar_t* kD3d11 = L"d3d11.dll";
const wchar_t* kProfile = L"edvr_profile.ini";
const wchar_t* kOpenvr = L"openvr_api.dll";
const wchar_t* kOpenvrOrig = L"openvr_api_orig.dll";
const wchar_t* kNgx = L"nvngx_dlss.dll";   // NVIDIA's DLSS runtime, beside the game's executable
const wchar_t* kXess = L"libxess.dll";
const wchar_t* kXessLicense = L"INTEL-XESS-LICENSE.txt";
const wchar_t* kXessThirdParty = L"INTEL-XESS-THIRD-PARTY.txt";

// Said when the game is running and it is not this folder's copy. Nothing is
// stopped, so it is a note rather than a problem -- but not silence either:
// being refused for a running game is what somebody expects here, and a report
// that says nothing about it looks like the check never ran.
const char* kRunningElsewhere =
    "Elite Dangerous is running, but from a different folder -- this install is not the one in "
    "use, so it can be changed.";

// Said when Windows would not list the running programs. A check that could not
// run is not a check that passed: a game that is running holds every file this
// installer touches, and a half-replaced install is worse than none.
const char* kRunStateUnknown =
    "Could not tell whether Elite Dangerous is running: Windows would not list the running "
    "programs. Nothing was changed. Close the game and try again; if this keeps happening, "
    "restart the installer.";

std::string say(const std::wstring& w) { return toUtf8(w); }

std::vector<std::wstring> subdirsOf(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::vector<std::wstring> filesLike(const std::wstring& dir, const std::wstring& pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(dir, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

const DllInfo* siblingNamed(const std::vector<DllInfo>& siblings, const std::wstring& leaf) {
    for (const DllInfo& d : siblings) {
        if (_wcsicmp(leafOf(d.path).c_str(), leaf.c_str()) == 0) return &d;
    }
    return nullptr;
}

// A filename out of edvr.ini, accepted only if it really is one.
//
// Values read from that file reach MoveFileEx as destinations, and edvr.ini is
// a text file people edit. A value carrying a path separator, a drive, a "..",
// or the name of the very file it stands in for is not a rename target; it is
// a way to move the game's own runtime somewhere else entirely and be told the
// install succeeded. Refused values fall back to the built-in name, which is
// what an install that never touched the setting uses anyway.
std::wstring safeSiblingName(const std::wstring& value, const std::wstring& mustNotEqual) {
    if (value.empty()) return std::wstring();
    if (value.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return std::wstring();
    if (value == L"." || value == L".." || value.find(L"..") != std::wstring::npos)
        return std::wstring();
    if (_wcsicmp(value.c_str(), mustNotEqual.c_str()) == 0) return std::wstring();
    // A rename target that is not a DLL is a typo, not a plan.
    if (value.size() < 5 || _wcsicmp(value.c_str() + value.size() - 4, L".dll") != 0)
        return std::wstring();
    return value;
}

// The Openvr folder as it is written into the install record: relative to the
// game folder, because that is the shape the record can hold safely.
std::wstring relativeOpenvr(const std::wstring& gameDir, const std::wstring& openvrDir) {
    if (openvrDir.empty()) return std::wstring();
    if (openvrDir.size() > gameDir.size() &&
        _wcsnicmp(openvrDir.c_str(), gameDir.c_str(), gameDir.size()) == 0) {
        size_t i = gameDir.size();
        while (i < openvrDir.size() && (openvrDir[i] == L'\\' || openvrDir[i] == L'/')) ++i;
        return openvrDir.substr(i);
    }
    return openvrDir;
}

bool isCanonicalDescriptor(const std::string& sha) {
    edvr::InstallScope scope = edvr::InstallScope::Invalid;
    return ownDescriptorScope(sha, &scope);
}

std::string trimmedSpaces(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return std::string();
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// Whether `text` is an EDVR startup config that names the loader and graphics paths given. The grammar is the runtime's own
// (src\openxr\native_local_config.h): one [openxr] section, version=1, and loader, graphics, runtime and separate_device=1 each
// once; frame_thread_priority, frame_end_overlap and pose_cache are optional. LF and CRLF both parse.
bool isEdvrStartupConfig(const std::string& text, const std::wstring& loaderPath, const std::wstring& graphicsPath) {
    bool section = false;
    std::vector<std::string> keys;
    std::wstring loader, graphics;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trimmedSpaces(line);
        if (line.empty()) continue;
        if (line == "[openxr]") {
            if (section) return false;
            section = true;
            continue;
        }
        const size_t equal = line.find('=');
        if (!section || equal == std::string::npos) return false;
        const std::string key = trimmedSpaces(line.substr(0, equal));
        const std::string value = trimmedSpaces(line.substr(equal + 1));
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) return false;
        keys.push_back(key);
        if (key == "version") {
            if (value != "1") return false;
        } else if (key == "loader") {
            loader = fromUtf8(value);
        } else if (key == "graphics") {
            graphics = fromUtf8(value);
        } else if (key == "runtime") {
            if (value.empty()) return false;
        } else if (key == "separate_device") {
            if (value != "1") return false;
        } else if (key == "frame_thread_priority") {
            if (value != "high" && value != "normal") return false;
        } else if (key == "frame_end_overlap") {
            if (value != "on" && value != "off") return false;
        } else if (key == "pose_cache") {
            if (value != "on" && value != "off") return false;
        } else {
            return false;
        }
    }
    for (const char* required : {"version", "loader", "graphics", "runtime", "separate_device"}) {
        if (std::find(keys.begin(), keys.end(), std::string(required)) == keys.end()) return false;
    }
    return section && _wcsicmp(loader.c_str(), loaderPath.c_str()) == 0 &&
           _wcsicmp(graphics.c_str(), graphicsPath.c_str()) == 0;
}

// The Openvr folder the OpenXR files are written into: the surveyed one, else the default beside the game.
std::wstring nativeDirOf(const Survey& s) {
    return s.game.openvrDir.empty() ? joinPath(s.game.dir, L"Openvr\\win64") : s.game.openvrDir;
}

bool sameSha(const std::string& a, const std::string& b) {
    return !a.empty() && _stricmp(a.c_str(), b.c_str()) == 0;
}

// The tool's receipt lists this path as a file it placed. What the tool REPLACED is somebody else's by definition, so only the placed
// hash is a proof of EDVR's bytes.
bool toolListsPath(const Survey& s, const std::wstring& path) {
    for (const ToolReceipt& r : s.toolReceipts)
        for (const ToolReceiptFile& f : r.files)
            if (_wcsicmp(f.target.c_str(), path.c_str()) == 0) return true;
    return false;
}

bool toolPlacedHash(const Survey& s, const std::wstring& path, const std::string& sha) {
    for (const ToolReceipt& r : s.toolReceipts)
        for (const ToolReceiptFile& f : r.files)
            if (_wcsicmp(f.target.c_str(), path.c_str()) == 0 && sameSha(f.installedSha, sha)) return true;
    return false;
}

// A file is EDVR's to remove only with PROVENANCE -- the record or the tool's receipt lists it, or the proof the caller names -- AND its
// bytes confirmed: the ones the record kept, the tool placed, or this installer carries. Either part alone is not enough. Matching bytes
// alone are what another mod's copy of a standard vendor binary looks like, and a provenance alone is not proof the file is intact.
// A file that is absent or unreadable is not EDVR's by anything.
bool ownedFile(const Survey& s, const DllInfo& info, bool provenance, const std::string& recorded,
               const std::string& carried) {
    if (!provenance || info.kind == DllKind::Absent || info.kind == DllKind::Unreadable || info.sha256.empty())
        return false;
    return sameSha(info.sha256, recorded) || sameSha(info.sha256, carried) ||
           toolPlacedHash(s, info.path, info.sha256);
}

// The startup config names this folder's loader and graphics paths, the same paths the planner writes into it.
bool startupConfigIsEdvrs(const Survey& s) {
    if (s.nativeConfig.kind == DllKind::Absent || s.nativeConfig.kind == DllKind::Unreadable) return false;
    return isEdvrStartupConfig(s.nativeConfigText, joinPath(nativeDirOf(s), L"openxr_loader.dll"),
                               joinPath(s.game.dir, kD3d11));
}

// The startup config is EDVR's by its own text, which names this folder's paths; or by its record or the tool's receipt and its bytes.
bool startupConfigOwned(const Survey& s) {
    if (ownedFile(s, s.nativeConfig,
                  !s.state.nativeConfigSha.empty() || toolListsPath(s, s.nativeConfig.path),
                  s.state.nativeConfigSha, std::string()))
        return true;
    return startupConfigIsEdvrs(s);
}

// The note for a file whose bytes are EDVR's but which nothing shows EDVR placed: another mod's identical copy looks exactly like this.
const char* kMatchesNoProof = "matches ED-UP's copy but nothing shows ED-UP placed it; left alone";

// The game's original runtime that the tool kept when it installed over it: the native receipt's runtime entry, whose backup sits beside
// the runtime. It is adopted only when every proof holds: the receipt names this folder (readToolReceipts), the entry is this folder's
// openvr_api.dll, the backup is in the same Openvr folder, its hash is the one the receipt recorded, and probeDll reads it as a genuine
// OpenVR runtime (not EDVR's). Anything else adopts nothing and stays rejected, as before.
// The current receipt is tried first. After a second tool install its backup is the previous EDVR runtime, which probeDll refuses, and
// the game's own runtime sits in an older receipt's backup; those are tried newest first, each under the same proofs in full.
void adoptToolOriginal(Survey& s) {
    const std::wstring runtimePath = joinPath(s.game.openvrDir, kOpenvr);
    auto folderOf = [](const std::wstring& path) {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    };
    auto adoptFrom = [&](const ToolReceipt& receipt) {
        if (!receipt.native) return false;
        const ToolReceiptFile* runtime = nullptr;
        for (const ToolReceiptFile& file : receipt.files) {
            if (file.key == "runtime" || file.key == "native") { runtime = &file; break; }
        }
        if (!runtime || runtime->backup.empty() || runtime->beforeSha.empty()) return false;
        if (_wcsicmp(runtime->target.c_str(), runtimePath.c_str()) != 0) return false;
        if (_wcsicmp(folderOf(runtime->backup).c_str(), s.game.openvrDir.c_str()) != 0) return false;
        if (!fileExists(runtime->backup) || !sameSha(sha256File(runtime->backup), runtime->beforeSha)) return false;
        const DllInfo copy = probeDll(runtime->backup);
        if (copy.kind != DllKind::OpenVrRuntime) return false;
        s.toolOriginal = copy;
        s.openvrOrigInBackups.insert(s.openvrOrigInBackups.begin(), runtime->backup);
        return true;
    };
    for (const ToolReceipt& receipt : s.toolReceipts)
        if (adoptFrom(receipt)) return;
    for (const ToolReceipt& receipt : s.toolReceiptHistory)
        if (adoptFrom(receipt)) return;
}

// The profiles an install of `scope` covers (install_manifest.h): both is the VR and the flat profile, vr and flat one each.
unsigned profilesOfScope(const std::string& scope) {
    if (scope == "both") return kProfileAll;
    return scope == "flat" ? kProfileFlat : kProfileVr;
}

// Where a manifest target resolves to on this machine: the game folder or the Openvr folder the survey found (nativeDirOf), plus the
// target's path, '/' separated in the manifest and '\\' here. isTargetPath has already refused everything that could leave the root.
std::wstring targetPathOf(const Survey& s, const ManifestTarget& target) {
    std::wstring relative = fromUtf8(target.path);
    for (wchar_t& c : relative) {
        if (c == L'/') c = L'\\';
    }
    return joinPath(target.root == TargetRoot::Openvr ? nativeDirOf(s) : s.game.dir, relative);
}

// Whether a resolved target stays inside its root: the normalised full path is under the root, and no component between the root and
// the file that exists now is a reparse point (a junction or a symbolic link), which would carry the write somewhere else. The root
// itself may be a junction, as a game folder under a library link always could be.
bool targetStaysInside(const std::wstring& root, const std::wstring& path) {
    auto full = [](const std::wstring& in) {
        wchar_t buffer[32768];
        const DWORD n = GetFullPathNameW(in.c_str(), static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])), buffer, nullptr);
        return (n == 0 || n >= sizeof(buffer) / sizeof(buffer[0])) ? std::wstring() : std::wstring(buffer, n);
    };
    std::wstring r = full(root);
    const std::wstring f = full(path);
    if (r.empty() || f.empty()) return false;
    while (r.size() > 1 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    if (f.size() <= r.size() + 1 || _wcsnicmp(f.c_str(), r.c_str(), r.size()) != 0 || f[r.size()] != L'\\') return false;
    // Every directory strictly between the root and the file.
    size_t at = r.size();
    for (;;) {
        const size_t slash = f.find(L'\\', at + 1);
        if (slash == std::wstring::npos) break;
        const DWORD attributes = GetFileAttributesW(f.substr(0, slash).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) break;   // nothing below here exists yet
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
        at = slash;
    }
    return true;
}

// targetPathOf, refused (false) when the result would leave its root.
bool containedTargetPath(const Survey& s, const ManifestTarget& target, std::wstring* path) {
    *path = targetPathOf(s, target);
    return targetStaysInside(target.root == TargetRoot::Openvr ? nativeDirOf(s) : s.game.dir, *path);
}

// What is at `path` now. The files the survey has always held by name come from its fields (what a test sets); any other target is
// looked up in Survey::targets, and failing that read here: present, with its hash, as a file whose kind is not looked into (the
// planner only needs to know whether it is there and whether it is the payload's). `probed` keeps what was read, so a path asked
// twice is read once.
DllInfo targetInfoOf(const Survey& s, const std::wstring& path, std::vector<DllInfo>* probed) {
    const std::wstring openvrDir = nativeDirOf(s);
    const struct { std::wstring path; const DllInfo* info; } known[] = {
        {joinPath(s.game.dir, kD3d11), &s.d3d11},
        {joinPath(s.game.dir, kNgx), &s.ngx},
        {joinPath(s.game.dir, kXess), &s.xess},
        {joinPath(s.game.dir, kXessLicense), &s.xessLicense},
        {joinPath(s.game.dir, kXessThirdParty), &s.xessThirdParty},
        {joinPath(openvrDir, kOpenvr), &s.openvrCurrent},
        {joinPath(openvrDir, L"openxr_loader.dll"), &s.openxrLoader},
        {joinPath(openvrDir, L"OPENXR-LOADER-LICENSE.txt"), &s.openxrLicense},
    };
    for (const auto& entry : known) {
        if (_wcsicmp(entry.path.c_str(), path.c_str()) == 0) return *entry.info;
    }
    for (const DllInfo& d : s.targets) {
        if (_wcsicmp(d.path.c_str(), path.c_str()) == 0) return d;
    }
    for (const DllInfo& d : *probed) {
        if (_wcsicmp(d.path.c_str(), path.c_str()) == 0) return d;
    }
    DllInfo info;
    info.path = path;
    if (fileExists(path)) {
        info.sha256 = sha256File(path);
        info.kind = info.sha256.empty() ? DllKind::Unreadable : DllKind::Foreign;
    }
    probed->push_back(info);
    return info;
}

// The order the planner works through a manifest's files, whatever order the manifest lists them in: what is chained first, then
// what is only ever placed with a proof (owned), the runtime, the plain replacements, and the settings files last, because the
// chain decision made earlier is forced into them.
int rankOfBehaviour(ExistingMode mode) {
    switch (mode) {
        case ExistingMode::Chain: return 0;
        case ExistingMode::Owned: return 1;
        case ExistingMode::Runtime: return 2;
        case ExistingMode::Replace: return 3;
        default: return 4;
    }
}

// Where an install record's file is on this machine, or false when the record is refused: an unknown root, or a path that fails the
// manifest's target path rule (a hand-edited state.ini may name "..\..\Windows\x.dll"). The same rule parseState applies on load; it is
// applied again here because a plan may be given a record that never went through parseState.
bool recordPathOf(const Survey& s, const FileRecord& r, std::wstring* path) {
    if (!recordTargetIsSafe(r)) return false;
    ManifestTarget target;
    target.root = r.root == "openvr" ? TargetRoot::Openvr : TargetRoot::Game;
    target.path = r.path;
    return containedTargetPath(s, target, path);
}

// The behaviour a record was placed under, by its name; Replace for a name this build does not know (parseState refuses those).
ExistingMode modeOfRecord(const FileRecord& r) {
    for (const ExistingMode mode : {ExistingMode::Chain, ExistingMode::Owned, ExistingMode::Runtime, ExistingMode::Settings})
        if (r.existing == existingName(mode)) return mode;
    return ExistingMode::Replace;
}

// How the report names the files it says most about: what it is, and what turns it on. The words only; a file with none is
// placed and reported by its file name. `label` is the family a kept file is named by ("Keeping a changed OpenXR file").
// This table and the startup config's loader name are the id-specific words that remain in the planner.
struct FileBlurb { const char* id; const char* label; const char* what; const char* purpose; };
const FileBlurb kBlurbs[] = {
    {"dlss", "DLSS", "NVIDIA's DLSS runtime", "for temporal_aa = dlaa"},
    {"xess", "XeSS", "Intel's XeSS-SR runtime", "for temporal_aa = xess"},
    {"xess_license", "XeSS", nullptr, nullptr},
    {"xess_third_party", "XeSS", nullptr, nullptr},
    {"openxr_loader", "OpenXR", nullptr, nullptr},
    {"openxr_license", "OpenXR", nullptr, nullptr},
};
const FileBlurb* blurbFor(const std::string& id) {
    for (const FileBlurb& b : kBlurbs) {
        if (id == b.id && b.what) return &b;
    }
    return nullptr;
}

// Where a path's backup is kept, relative to the backup folder: its file name, which keeps every backup where the survey and the mirror
// look for it, unless another file of this plan already took that name. The later one is kept under its folder relative to the game
// (the Openvr folder as "openvr"), so two targets with one file name never overwrite each other's backup.
std::wstring backupNameFor(const Survey& s, const std::wstring& path, std::vector<std::pair<std::wstring, std::wstring>>* taken) {
    const std::wstring leaf = leafOf(path);
    for (const auto& entry : *taken) {
        if (_wcsicmp(entry.first.c_str(), leaf.c_str()) != 0) continue;
        if (_wcsicmp(entry.second.c_str(), path.c_str()) == 0) return leaf;
        const std::wstring openvr = nativeDirOf(s);
        auto under = [&](const std::wstring& dir) {
            return path.size() > dir.size() && _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) == 0 &&
                   (path[dir.size()] == L'\\' || path[dir.size()] == L'/');
        };
        if (openvr != s.game.dir && under(openvr)) return L"openvr" + path.substr(openvr.size());
        if (under(s.game.dir)) return path.substr(s.game.dir.size() + 1);
        return leaf;
    }
    taken->emplace_back(leaf, path);
    return leaf;
}

// One file an operation may take out, with the proof that settles it. Built from the install records, which are the ownership
// evidence: a record for the id whose hash is what is on disk now, or a tool receipt that placed those bytes. A payload, when the caller
// has one, only widens that for a folder whose record was lost (see removalCandidates).
struct Candidate {
    std::string  id;
    std::wstring path;
    DllInfo      info;
    ExistingMode mode = ExistingMode::Replace;
    bool         owned = false;    // EDVR's, by the proofs: it may be removed
    bool         matches = false;  // not proven EDVR's, but identical to the payload's copy: another mod's copy looks like this
    bool         dropped = false;  // an install is not placing its id any more: the payload lacks it
    bool         moved = false;    // an install places its id at another target: this is the old one
    std::string  newTarget;        // for a moved one, where the payload puts it now
};

// "Keeping a changed OpenXR file: x" for an edited copy, or the identical-copy note for a copy nothing shows EDVR placed. The edited
// copy is kept and the plan says so; so is the identical one, which another mod's own copy of a vendor file looks exactly like.
std::string keepNote(const Candidate& c) {
    const std::string leaf = say(leafOf(c.path));
    if (c.matches) return leaf + " " + kMatchesNoProof;
    if (c.dropped) return "Keeping " + leaf + ": it was changed since ED-UP placed it, and this install no longer places it.";
    if (c.moved) return "Keeping the old " + leaf + ": it was changed since ED-UP placed it, and this install places it at " + c.newTarget + " now.";
    const FileBlurb* blurb = nullptr;
    for (const FileBlurb& b : kBlurbs) if (c.id == b.id) blurb = &b;
    return std::string("Keeping a changed ") + (blurb ? std::string(blurb->label) + " " : std::string()) + "file: " + leaf;
}

// The files an operation may take out, in the order the records list them, then the payload's.
//
// A record is a candidate unless this install still places its id. `uninstall` says nothing is placed (every record goes);
// otherwise a record stays out when the payload places its id, at the recorded target, under the install's profiles; an id the payload
// places somewhere else makes the old copy a candidate (moved), and an id it lacks does too (dropped), except an owned one: a vendor
// file a build carries no bytes for stays, and stays recorded. Chain, runtime and settings records are not removed here: the chain
// and the runtime have their own restore rules, and a settings file is the user's.
//
// A payload file with no record is a candidate only for a folder whose record was lost: its provenance is the tool's receipt, or, for a
// file the replace behaviour put in the Openvr folder, EDVR's startup config for this folder; its bytes are the tool's or the payload's.
// An uninstall called with no payload sees only the records and the receipts.
std::vector<Candidate> removalCandidates(const Survey& s, const PayloadInfo& p, bool uninstall, unsigned profiles,
                                         std::vector<std::string>* notes) {
    std::vector<Candidate> out;
    std::vector<DllInfo> probed;
    for (const FileRecord& r : s.state.files) {
        const ExistingMode mode = modeOfRecord(r);
        if (mode == ExistingMode::Chain || mode == ExistingMode::Runtime || mode == ExistingMode::Settings) continue;
        const PayloadFileEntry* entry = p.find(r.id);
        bool dropped = false, moved = false;
        if (!uninstall) {
            if (entry && whenHolds(entry->file.when, profiles, true)) {
                // Still placed. Placed somewhere else than the record says, the old copy is a file this install no longer places.
                if (r.root == targetRootName(entry->file.target.root) && _stricmp(r.path.c_str(), entry->file.target.path.c_str()) == 0)
                    continue;
                moved = true;
            } else if (!entry && mode == ExistingMode::Owned) {
                // A vendor file (owned) this payload does not place is never removed by an install: a build without the SDK carries no
                // bytes for it, and what an earlier install placed stays where it is, recorded. Only an uninstall takes it out.
                continue;
            } else {
                dropped = !entry;
            }
        }
        Candidate c;
        c.id = r.id;
        c.mode = mode;
        c.dropped = dropped;
        c.moved = moved;
        if (moved) c.newTarget = entry->file.target.path;
        if (!recordPathOf(s, r, &c.path)) {
            notes->push_back("The install record names a path outside its folder for " + r.id + "; it was not touched.");
            continue;
        }
        c.info = targetInfoOf(s, c.path, &probed);
        if (c.info.kind == DllKind::Absent) continue;
        // Owned by the record's hash, a tool receipt, or the bytes this payload carries for the id (what the old loader and licence
        // ownership also accepted).
        c.owned = ownedFile(s, c.info, true, r.sha256, p.shaOf(r.id));
        c.matches = sameSha(c.info.sha256, p.shaOf(r.id));
        out.push_back(std::move(c));
    }
    for (const PayloadFileEntry& e : p.files) {
        if (s.state.file(e.file.id)) continue;
        if (e.file.existing != ExistingMode::Replace && e.file.existing != ExistingMode::Owned) continue;
        if (!uninstall && whenHolds(e.file.when, profiles, true)) continue;
        Candidate c;
        c.id = e.file.id;
        c.mode = e.file.existing;
        if (!containedTargetPath(s, e.file.target, &c.path)) continue;
        c.info = targetInfoOf(s, c.path, &probed);
        if (c.info.kind == DllKind::Absent) continue;
        const bool provenance = toolListsPath(s, c.path) ||
            (e.file.existing == ExistingMode::Replace && e.file.target.root == TargetRoot::Openvr && startupConfigIsEdvrs(s));
        c.owned = ownedFile(s, c.info, provenance, std::string(), e.sha);
        c.matches = sameSha(c.info.sha256, e.sha);
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace

// The descriptor on disk is what the DLL reads, so a canonical one names the scope and the record is only the fallback. A record
// left by an earlier install (a flat GUI install, say) must not outvote the files: that made a both install look flat.
std::string descriptorScopeOf(const Survey& s) {
    if (!s.descriptorPresent || s.descriptorSha.empty()) return std::string();
    edvr::InstallScope scope = edvr::InstallScope::Invalid;
    if (!ownDescriptorScope(s.descriptorSha, &scope)) return std::string();
    return edvr::scopeName(scope);
}

std::string canonicalDescriptorScope(const std::string& text) {
    edvr::InstallScope scope = edvr::InstallScope::Invalid;
    if (!ownDescriptorScope(sha256Bytes(text.data(), text.size()), &scope)) return std::string();
    return edvr::scopeName(scope);
}

std::string installedScope(const Survey& s) {
    const std::string fromDescriptor = descriptorScopeOf(s);
    if (!fromDescriptor.empty()) return fromDescriptor;
    if (s.state.present) return recordedScope(s.state);
    return std::string();
}

// The record names a different scope than the descriptor on disk: the install is older than the files here, and Update refreshes it.
bool installRecordStale(const Survey& s) {
    const std::string fromDescriptor = descriptorScopeOf(s);
    return s.state.present && !fromDescriptor.empty() && recordedScope(s.state) != fromDescriptor;
}

std::string defaultScope(const Survey& s) {
    const std::string installed = installedScope(s);
    return installed.empty() ? std::string("both") : installed;
}

std::string installedProfile(const Survey& s) {
    const std::string scope = installedScope(s);
    if (scope.empty()) return std::string();
    return editionFor(scope);
}

bool hasSettingsFor(const Survey& s, const std::string& profile) {
    if (profile == "flat") return s.flatIniPresent || s.iniPresent;
    return s.iniPresent;
}

Survey surveyTarget(const GameInstall& game) {
    Survey s;
    s.game = game;
    s.eliteKind = classifyEliteExecutable(joinPath(game.dir, L"EliteDangerous64.exe"),
                                          &s.eliteFileVersion);
    if (s.game.openvrDir.empty()) s.game.openvrDir = findOpenvrDir(game.dir);
    if (s.game.openvrDir.empty()) s.game.openvrDir = joinPath(game.dir, L"Openvr\\win64");

    // Asked about THIS folder, not about the executable's name: the other
    // install on the machine can be in a jump while this one is patched.
    const GameRunState running = gameRunState(s.game.dir);
    s.gameRunningHere = running == GameRunState::ThisFolder;
    s.gameRunningElsewhere = running == GameRunState::OtherFolder;
    s.gameRunStateUnknown = running == GameRunState::Unknown;

    s.d3d11 = probeDll(joinPath(game.dir, kD3d11));
    for (const std::wstring& name : filesLike(game.dir, L"d3d11_*.dll")) {
        s.otherD3d11.push_back(probeDll(joinPath(game.dir, name)));
    }

    // Both settings files, each as itself: which one an operation works on is the
    // planner's decision, made from the edition it is for.
    const std::wstring iniPath = settingsPathFor(game.dir, "vr");
    s.iniPresent = fileExists(iniPath);
    if (s.iniPresent) s.iniText = readTextFile(iniPath);
    const std::wstring flatIniPath = settingsPathFor(game.dir, "flat");
    s.flatIniPresent = fileExists(flatIniPath);
    if (s.flatIniPresent) s.flatIniText = readTextFile(flatIniPath);
    s.baseIniText = readTextFile(baseIniPath(game.dir));
    s.flatBaseIniText = readTextFile(flatBaseIniPath(game.dir));
    s.state = readState(game.dir);
    if (!game.dir.empty()) {
        s.toolReceipts = readToolReceipts(game.dir);
        s.toolReceiptHistory = readNativeReceiptHistory(game.dir);
    }
    s.descriptorPresent = fileExists(joinPath(game.dir, kProfile));
    if (s.descriptorPresent) s.descriptorSha = sha256File(joinPath(game.dir, kProfile));
    s.ngx = probeDll(joinPath(game.dir, kNgx));
    auto notice = [&](const wchar_t* leaf) {
        DllInfo info; info.path = joinPath(game.dir, leaf);
        if (fileExists(info.path)) {
            info.sha256 = sha256File(info.path);
            info.kind = info.sha256.empty() ? DllKind::Unreadable : DllKind::Foreign;
        }
        return info;
    };
    // The pinned XeSS runtime is 75 MB, above probeDll's 64 MB PE cap.
    // Ownership here needs only a readable content hash, as for notices.
    s.xess = notice(kXess);
    s.xessLicense = notice(kXessLicense);
    s.xessThirdParty = notice(kXessThirdParty);
    s.nvidiaAdapter = nvidiaAdapterPresent(&s.nvidiaAdapterName);

    if (!s.game.openvrDir.empty()) {
        s.haveOpenvrDir = dirExists(s.game.openvrDir);
        s.openxrLoader = probeDll(joinPath(s.game.openvrDir, L"openxr_loader.dll"));
        auto surveyText = [&](const wchar_t* name) {
            DllInfo info; info.path=joinPath(s.game.openvrDir,name);
            if(fileExists(info.path)) {
                info.sha256=sha256File(info.path);
                info.kind=info.sha256.empty()?DllKind::Unreadable:DllKind::Foreign;
            }
            return info;
        };
        s.nativeConfig=surveyText(L"edvr_openxr.ini");
        if(s.nativeConfig.kind!=DllKind::Absent && s.nativeConfig.kind!=DllKind::Unreadable)
            s.nativeConfigText=readTextFile(s.nativeConfig.path);
        s.openxrLicense=surveyText(L"OPENXR-LOADER-LICENSE.txt");
        s.openvrCurrent = probeDll(joinPath(s.game.openvrDir, kOpenvr));

        // The original is always renamed to openvr_api_orig.dll: the name the
        // README documents and this installer writes. (Before 2026-09-16 a
        // hand-edited advanced.real_openvr_dll could choose a different name,
        // for the forwarding proxy's chaining; native OpenXR never chains, so
        // that key is retired and the name is no longer configurable.)
        s.openvrOrigName = kOpenvrOrig;
        s.openvrOrig = probeDll(joinPath(s.game.openvrDir, s.openvrOrigName));
    }

    // Backups this installer made, newest first. The only thing looked for is
    // a genuine OpenVR runtime: it is what makes "the game's original was
    // overwritten" a recoverable mistake instead of a trip through the
    // launcher's file verification.
    const std::wstring backups = backupRootPath(game.dir);
    if (dirExists(backups)) {
        std::vector<std::wstring> stamps = subdirsOf(backups);
        std::sort(stamps.rbegin(), stamps.rend());
        for (const std::wstring& stamp : stamps) {
            // Where a backup puts the runtime: beside the stamp's other files, or under openvr\ when the plan had another file of
            // that name to keep apart (backupNameFor). Both are read.
            for (const wchar_t* sub : {L"", L"openvr"}) {
                const std::wstring candidate = joinPath(sub[0] ? joinPath(joinPath(backups, stamp), sub) : joinPath(backups, stamp), kOpenvr);
                if (!fileExists(candidate)) continue;
                const DllInfo info = probeDll(candidate);
                if (info.kind == DllKind::OpenVrRuntime) {
                    s.openvrOrigInBackups.push_back(candidate);
                    break;
                }
            }
        }
    }
    // The tool's backup of the original is adopted last and placed first: its receipt names exactly which runtime it replaced.
    if (s.haveOpenvrDir) adoptToolOriginal(s);
    return s;
}

Plan planInstall(const Survey& s, const Options& o, const PayloadInfo& p) {
    Plan plan;
    plan.backupDir =
        joinPath(backupRootPath(s.game.dir), o.backupStamp.empty() ? L"backup" : o.backupStamp);

    if (s.game.dir.empty()) {
        plan.blocked = true;
        plan.problems.push_back("No game folder chosen.");
        return plan;
    }
    if (s.gameRunningHere) {
        plan.blocked = true;
        plan.problems.push_back(
            "Elite Dangerous is running. Close it first -- Windows will not let anything replace a "
            "file the game has open, and a half-replaced install is worse than none.");
        return plan;
    }
    if (s.gameRunStateUnknown) {
        plan.blocked = true;
        plan.problems.push_back(kRunStateUnknown);
        return plan;
    }
    // The scope this run installs: the one asked for, else the one the folder already has, else both. A payload with no VR runtime
    // is refused below for a scope that needs one, rather than quietly installing flat.
    const std::string installed = installedScope(s);
    if (s.state.present && !knownScope(installed)) {
        plan.blocked = true; plan.problems.push_back("The existing install record has an unknown profile. Repair it before changing editions."); return plan;
    }
    // What the payload carries, in the manifest's own words: the file that is chained, the runtime, a settings file per profile. The OpenXR
    // loader and its licence are found by id because the startup config names the loader, and the flat conversion proves each by its record.
    const PayloadFileEntry* graphicsFile = p.firstWith(ExistingMode::Chain);
    const PayloadFileEntry* runtimeFile = p.firstWith(ExistingMode::Runtime);
    const PayloadFileEntry* loaderFile = p.find("openxr_loader");
    const PayloadFileEntry* licenseFile = p.find("openxr_license");
    const PayloadFileEntry* vrSettings = p.settingsFor(kProfileVr);
    const PayloadFileEntry* flatSettings = p.settingsFor(kProfileFlat);
    const bool payloadHasRuntime = graphicsFile && graphicsFile->valid && runtimeFile && runtimeFile->valid && loaderFile &&
                                   loaderFile->valid && licenseFile;
    const std::string scope = o.scope.empty() ? defaultScope(s) : o.scope;
    if (!knownScope(scope)) {
        plan.blocked = true; plan.problems.push_back("Unknown scope \"" + scope + "\": use both, vr or flat."); return plan;
    }
    const bool flat = scope == "flat";           // no VR runtime: the game keeps its own openvr_api.dll
    const bool wantsVrIni = scopeHasVrIni(scope);
    const bool wantsFlatIni = scopeHasFlatIni(scope);
    if (!flat && !payloadHasRuntime) {
        plan.blocked = true;
        plan.problems.push_back("This installer carries no VR runtime, so it cannot install scope " + scope +
                                ". Install scope flat, or use the VR installer. Nothing will be installed.");
        return plan;
    }
    if (!graphicsFile || !graphicsFile->valid || (wantsVrIni && !vrSettings) || (wantsFlatIni && !flatSettings)) {
        plan.blocked = true;
        plan.problems.push_back("This installer's payload is incomplete or invalid for scope " + scope + ". Nothing will be installed.");
        return plan;
    }
    // Each scope's descriptor is the planner's own canonical text. Moving to flat from a scope with the VR runtime is the conversion
    // whose proofs follow; no confirmation is asked for it, and no settings file is deleted by it.
    const std::string descriptor = descriptorTextFor(scope);
    const std::string descriptorSha = sha256Bytes(descriptor.data(), descriptor.size());
    const std::string existingProfile = installedProfile(s);
    // Flat retires the VR runtime only when the file in Openvr\win64 is EDVR's own build. The descriptor and the record say what the last
    // run wrote; what is on disk decides whether anything needs retiring. A folder whose runtime is the game's original (a leftover
    // descriptor, a stale record) has nothing to prove, and its openvr_api.dll is left alone.
    const bool retireRuntime = flat && s.openvrCurrent.kind == DllKind::Edvr;
    // The file on disk is the one the record names, or the planner's own text for the scope, or any of EDVR's texts for the edition
    // this folder already has (schema 1 or 2, whichever an earlier install or the tool wrote). That last clause upgrades such a file in
    // place; a modified or unknown file matches none of them and is refused.
    const bool ownEdition = !existingProfile.empty() && isOwnDescriptor(s.descriptorSha, existingProfile);
    const bool recordAgrees = s.state.descriptorSha.empty() ? s.descriptorSha == descriptorSha
                                                            : s.descriptorSha == s.state.descriptorSha;
    if (s.descriptorPresent && (s.descriptorSha.empty() || (!recordAgrees && !ownEdition))) {
        plan.blocked = true; plan.problems.push_back("edvr_profile.ini differs from the recorded file; it was left untouched."); return plan;
    }
    // The files this install no longer places, found from the records (removalCandidates): on a flat conversion the OpenXR files beside
    // the VR runtime, on any update an id the manifest dropped. Each is removed only when it is still EDVR's, by its recorded hash.
    std::vector<std::string> recordNotes;
    const std::vector<Candidate> retirees = removalCandidates(s, p, false, profilesOfScope(scope), &recordNotes);
    if (retireRuntime) {
        // The runtime's own record, by its behaviour: the file it names is the one EDVR placed in the Openvr folder.
        const FileRecord* runtimeRecord = s.state.withExisting(existingName(ExistingMode::Runtime));
        const std::string runtimeSha = runtimeRecord ? runtimeRecord->sha256 : std::string();
        const bool ownedVr = !runtimeSha.empty() && s.openvrCurrent.sha256 == runtimeSha &&
            (s.openvrOrig.kind == DllKind::OpenVrRuntime || !s.openvrOrigInBackups.empty()) &&
            (s.openvrOrig.kind != DllKind::OpenVrRuntime || s.state.openvrOrigSha.empty() ||
             s.openvrOrig.sha256 == s.state.openvrOrigSha);
        if (!s.state.present || !ownedVr) {
            plan.blocked = true; plan.problems.push_back("Cannot prove ED-UP's VR runtime and the game's original; flat conversion left the VR files intact."); return plan;
        }
        // The OpenXR files beside the runtime are retired with it, so each must be EDVR's by a proof; a changed one refuses the conversion.
        bool changedAsset = s.nativeConfig.kind != DllKind::Absent && !startupConfigOwned(s);
        // Only the OpenXR files the old planner retired decide this: a dropped or moved id is kept with a note and blocks nothing.
        for (const Candidate& c : retirees) changedAsset |= c.mode == ExistingMode::Replace && !c.owned && !c.dropped && !c.moved;
        if (changedAsset) {
            plan.blocked = true; plan.problems.push_back("An OpenXR asset differs from ED-UP's record; conversion cannot retire it safely."); return plan;
        }
    }
    switch (s.eliteKind) {
        case EliteExeKind::OdysseyQualified:
            break;
        case EliteExeKind::Legacy:
            plan.blocked = true;
            plan.problems.push_back(
                "This is Elite Dangerous (Horizons), not Elite Dangerous: Odyssey. "
                "ED-UP supports Odyssey only; no ED-UP build supports this game.");
            return plan;
        case EliteExeKind::Unreadable:
            plan.blocked = true;
            plan.problems.push_back(
                "EliteDangerous64.exe could not be read. Verify the game files in the launcher, then try again.");
            return plan;
        case EliteExeKind::OdysseyUnknown: {
            // Not blocked: an Odyssey revision this build was not checked against installs, and the
            // warning goes under "Worth knowing" in the report, the confirmation and the summary.
            std::string msg = "This Elite Dangerous: Odyssey revision";
            if (!s.eliteFileVersion.empty())
                msg += " (version " + toUtf8(s.eliteFileVersion) + ")";
            msg += " is not the one this ED-UP build was checked against. "
                   "ED-UP will install and the game will start; fixes tied to the game build stand down "
                   "and the gfx log names each one, and the Oculus PC runtime may bypass ED-UP's OpenXR "
                   "runtime until an ED-UP update for this revision.";
            plan.problems.push_back(msg);
            break;
        }
    }
    for (const DllInfo* item : {&s.d3d11,&s.openvrCurrent,&s.openxrLoader,&s.nativeConfig,&s.openxrLicense}) {
        if (flat && item != &s.d3d11 && !retireRuntime) continue;
        if(item->kind==DllKind::Unreadable) {
            plan.blocked=true;plan.problems.push_back("A native OpenXR destination cannot be read. Nothing will be installed.");return plan;
        }
    }
    const std::wstring nativeDir=s.game.openvrDir.empty()?joinPath(s.game.dir,L"Openvr\\win64"):s.game.openvrDir;
    if (s.gameRunningElsewhere) plan.notes.push_back(kRunningElsewhere);

    std::vector<Step> body;
    std::vector<std::pair<std::string, std::string>> forced;  // ini keys the installer must set
    bool wantBackupDir = false;
    bool changed = false;

    InstallState next = s.state;
    next.present = true;
    next.edvrVersion = p.version;
    next.installedUtc = o.nowUtc;
    next.profile = editionFor(scope);
    next.scope = scope;
    next.descriptorSha = descriptorSha;
    std::string components = "graphics,profile";
    if (wantsVrIni) components += ",ini";
    if (wantsFlatIni) components += ",flat-ini";
    if (!flat) components += ",openvr,openxr-loader,openxr-license,openxr-config";
    components += ",ngx-optional,xess-optional";
    next.components = components;
    next.openvrDir = flat ? std::wstring() : relativeOpenvr(s.game.dir, nativeDir);

    // The chain is DECIDED by this run, not inherited from the record. The
    // record says what was true last time, and whether it is still true is the
    // question this pass exists to answer -- carrying the value forward made
    // "the mod we chained to has been uninstalled" invisible, because the field
    // was never empty for the check below to notice.
    next.chainTarget.clear();
    next.chainMod.clear();

    std::vector<std::pair<std::wstring, std::wstring>> backupNames;  // file name -> the path that took it (backupNameFor)
    std::vector<std::pair<std::wstring, std::wstring>> backedUp;     // a path backed up in this plan -> where its copy goes
    auto backup = [&](const std::wstring& path, const std::string& why) {
        Step st;
        st.action = Action::Backup;
        st.from = path;
        st.to = joinPath(plan.backupDir, backupNameFor(s, path, &backupNames));
        st.why = why;
        body.push_back(st);
        backedUp.emplace_back(path, st.to);
        wantBackupDir = true;
    };
    auto rename = [&](const std::wstring& from, const std::wstring& to, const std::string& why,
                      const std::string& expectSha = std::string()) {
        Step st;
        st.action = Action::Rename;
        st.from = from;
        st.to = to;
        st.why = why;
        st.expectSha = expectSha;
        st.required = true;   // a rename with nothing to rename is a stale plan
        body.push_back(st);
        changed = true;
    };
    std::vector<std::wstring> targetDirs;  // the folders payload files are written into, for the ones that do not exist yet
    auto writePayload = [&](const std::string& item, const std::wstring& to, const std::string& why) {
        const size_t slash = to.find_last_of(L'\\');
        if (slash != std::wstring::npos) targetDirs.push_back(to.substr(0, slash));
        Step st;
        st.action = Action::WritePayload;
        st.item = item;
        st.to = to;
        st.why = why;
        body.push_back(st);
        changed = true;
    };
    auto writeText = [&](const std::string& text, const std::wstring& to, const std::string& why) {
        Step st;
        st.action = Action::WriteText;
        st.text = text;
        st.to = to;
        st.why = why;
        body.push_back(st);
    };
    auto copyInto = [&](const std::wstring& from, const std::wstring& to, const std::string& why) {
        Step st;
        st.action = Action::Backup;  // a copy that leaves the source alone
        st.from = from;
        st.to = to;
        st.why = why;
        // This one is a restore, not a backup: if the file it reads has gone,
        // skipping it quietly would reinstall the proxy over nothing and call
        // the result a success.
        st.required = true;
        body.push_back(st);
        changed = true;
    };

    // ------------------------------------------------------------------
    // The payload's files, as its manifest describes them (docs\design-manifest-schema2-2026-11-12.md): `when` says whether this install
    // places a file, `target` where, and `existing` how a file already there is treated. There is one handler per behaviour below; what
    // each does is what this planner always did for the file the behaviour was written for, now done for any file the manifest gives it.
    // ------------------------------------------------------------------
    struct Placed {
        const PayloadFileEntry* entry = nullptr;
        std::wstring dst;       // the resolved target
        DllInfo cur;            // what is at dst now
        bool canPlace = true;   // the whole `when` holds; false when the profile holds and the adapter does not
    };
    std::vector<Placed> placed;
    {
        std::vector<DllInfo> probed;
        const unsigned profiles = profilesOfScope(scope);
        for (const PayloadFileEntry& e : p.files) {
            if (!whenHolds(e.file.when, profiles, true)) continue;  // another profile's file
            Placed f;
            f.entry = &e;
            if (!containedTargetPath(s, e.file.target, &f.dst)) {
                plan.problems.push_back("The target of " + say(leafOf(f.dst)) + " (" + e.file.target.path +
                                        ") leaves its folder, through a junction or a link; it was not placed.");
                continue;
            }
            f.cur = targetInfoOf(s, f.dst, &probed);
            f.canPlace = whenHolds(e.file.when, profiles, s.nvidiaAdapter);
            placed.push_back(std::move(f));
        }
        std::stable_sort(placed.begin(), placed.end(), [](const Placed& x, const Placed& y) {
            return rankOfBehaviour(x.entry->file.existing) < rankOfBehaviour(y.entry->file.existing);
        });
    }
    // A file the adapter condition rules out is placed by no behaviour but `owned`, which still has to recognise a copy that is there.
    auto each = [&](ExistingMode mode, auto&& handler) {
        for (const Placed& f : placed) {
            if (f.entry->file.existing == mode && (f.canPlace || mode == ExistingMode::Owned)) handler(f);
        }
    };
    // What the record keeps of a file that was placed or found current: where it is, how it was treated, and what was written. An empty
    // hash means EDVR does not own whatever is there, so there is no record. A file this plan backed up before replacing it records
    // what it displaced and where the copy is, unless what it displaced was EDVR's own earlier copy: then the older record's
    // displaced file (the one that was not EDVR's) stays the one on record.
    auto recordFile = [&](const Placed& f, const std::string& sha) {
        const std::string& id = f.entry->file.id;
        if (sha.empty()) { next.drop(id); return; }
        FileRecord r;
        r.id = id;
        r.root = targetRootName(f.entry->file.target.root);
        r.path = f.entry->file.target.path;
        r.existing = existingName(f.entry->file.existing);
        r.sha256 = sha;
        const FileRecord* earlier = s.state.file(id);
        if (earlier) { r.beforeSha256 = earlier->beforeSha256; r.backup = earlier->backup; }
        for (const auto& saved : backedUp) {
            if (_wcsicmp(saved.first.c_str(), f.dst.c_str()) != 0) continue;
            const bool wasOurs = f.cur.kind == DllKind::Edvr || (earlier && sameSha(earlier->sha256, f.cur.sha256));
            if (wasOurs) break;
            r.beforeSha256 = f.cur.sha256;
            r.backup = relativeOpenvr(s.game.dir, saved.second);
        }
        next.put(r);
    };

    // ---- existing = chain: the proxy graphics DLL. One name, and every mod in this space wants it. ----
    auto placeChain = [&](const Placed& f) {
        const std::wstring& dst = f.dst;
        const DllInfo& cur = f.cur;
        const std::string& id = f.entry->file.id;
        const std::string& sha = f.entry->sha;

        if (cur.kind == DllKind::Unreadable) {
            plan.problems.push_back(
                "There is a d3d11.dll next to the game that cannot be read -- something has it "
                "open, or it is not a DLL. ED-UP's own d3d11.dll was not installed; nothing else "
                "was touched.");
        } else if (cur.kind == DllKind::Edvr) {
            if (cur.sha256 == sha && !o.repair) {
                plan.notes.push_back("d3d11.dll is already this build -- left alone.");
            } else {
                backup(dst, "the ED-UP d3d11.dll being replaced");
                writePayload(id, dst,
                             o.repair ? "reinstalls ED-UP's d3d11.dll" : "updates ED-UP's d3d11.dll");
                plan.notes.push_back(o.repair ? "Reinstalling d3d11.dll (the fixes)."
                                              : "Updating d3d11.dll (the fixes).");
            }
            recordFile(f, sha);
        } else if (cur.kind == DllKind::Absent) {
            writePayload(id, dst, "installs ED-UP's d3d11.dll");
            plan.notes.push_back(
                s.state.file(id)
                    ? "Restoring d3d11.dll -- ED-UP was installed here, and the file is gone. "
                      "(EDHM's uninstaller runs `del d3d11.dll`, which after an ED-UP install is "
                      "ours.)"
                    : "Installing d3d11.dll (the fixes).");
            recordFile(f, sha);
        } else {
            // Somebody else is in the slot. Rename theirs, take the name, and
            // point EDVR at it -- the README's manual procedure, done by the
            // thing that knows which file it is looking at.
            const std::wstring chainLeaf = chainNameFor(cur);
            const std::wstring chainPath = joinPath(s.game.dir, chainLeaf);
            const std::wstring modName = modNameOf(cur);
            const std::string modSay = modName.empty() ? "That mod" : say(modName);

            const DllInfo* existing = siblingNamed(s.otherD3d11, chainLeaf);
            if (existing && existing->kind != DllKind::Absent) {
                // The name we want is taken -- which happens when that mod
                // reinstalled itself over EDVR while its earlier copy was
                // still parked there. Keep the newcomer (it is the newer build
                // of the same mod) and put the old one in the backup folder,
                // where nothing can mistake it for live.
                const std::wstring parkedMod = modNameOf(*existing);
                const bool sameMod = !modName.empty() && parkedMod == modName;
                backup(chainPath, sameMod
                                      ? "the older " + modSay + " copy already parked here"
                                      : "the file already parked under that name");
                if (!sameMod) {
                    plan.problems.push_back(
                        "There was already a " + say(chainLeaf) +
                        " here and it is not the same mod as the one in the d3d11.dll slot. It "
                        "has been copied into the backup folder and replaced, so check that "
                        "folder before deleting anything.");
                }
            }
            backup(dst, "the " + modSay + " d3d11.dll found in ED-UP's place");
            rename(dst, chainPath, "moves " + modSay + " aside, keeping it installed",
                   cur.sha256);
            writePayload(id, dst, "installs ED-UP's d3d11.dll in its place");

            forced.emplace_back(kChainKey, say(chainLeaf));
            next.chainTarget = chainLeaf;
            next.chainMod = modName;

            // EDVR passes calls through ONE other mod. If it was already
            // chained to a different one, that one is still on disk under a
            // name nothing loads -- and saying nothing about it is how
            // somebody's EDHM quietly stops working after installing ReShade.
            if (!s.state.chainTarget.empty() &&
                _wcsicmp(s.state.chainTarget.c_str(), chainLeaf.c_str()) != 0) {
                const DllInfo* previous = siblingNamed(s.otherD3d11, s.state.chainTarget);
                if (previous && previous->kind != DllKind::Absent) {
                    plan.problems.push_back(
                        "ED-UP was passing calls through " + say(s.state.chainTarget) +
                        (s.state.chainMod.empty() ? std::string()
                                                  : " (" + say(s.state.chainMod) + ")") +
                        ", and now passes them through " + modSay +
                        " instead -- ED-UP can only chain to one. The older one is still in the "
                        "folder but nothing loads it. If you want that one back, set "
                        "advanced.real_dll to it by hand.");
                }
            }
            recordFile(f, sha);

            if (s.state.file(id)) {
                plan.notes.push_back(modSay +
                                     " has replaced ED-UP's d3d11.dll since the last install. Both "
                                     "are kept: it becomes " + say(chainLeaf) +
                                     ", ED-UP takes the d3d11.dll name back, and every call passes "
                                     "through to it.");
            } else {
                plan.notes.push_back(modSay + " is installed as d3d11.dll. It is renamed " +
                                     say(chainLeaf) + ", ED-UP takes its place, and "
                                     "advanced.real_dll points back at it -- both mods run.");
            }
            if (!cur.is64) {
                plan.problems.push_back(
                    "The d3d11.dll found in the game folder is 32-bit, which the 64-bit game "
                    "cannot have been loading. It has been kept, but check what put it there.");
            }
        }
    };
    each(ExistingMode::Chain, placeChain);

    // A chain target that has gone away leaves advanced.real_dll pointing at
    // nothing: EDVR then writes a fatal note beside the game on every launch
    // and falls back to the system DLL. Whether the mod we were passing calls
    // through to is still there is asked on every run, including one that did
    // not touch d3d11.dll at all.
    //
    // A kept chain is NOT forced into a settings file that already exists: whoever emptied the key to turn the chain off meant it, and
    // writing it back on every update would undo that. It is written where a settings file is made new (absent, or replaced by the
    // shipped defaults), which is where it used to be lost -- see the settings block below.
    bool keptChain = false;
    if (next.chainTarget.empty() && !s.state.chainTarget.empty()) {
        const DllInfo* target = siblingNamed(s.otherD3d11, s.state.chainTarget);
        if (target && target->kind != DllKind::Absent) {
            next.chainTarget = s.state.chainTarget;
            next.chainMod = s.state.chainMod;
            keptChain = true;
            plan.notes.push_back("Keeping the chain to " + say(s.state.chainTarget) + ".");
        } else {
            forced.emplace_back(kChainKey, "");
            plan.notes.push_back("The mod ED-UP was passing calls through to (" +
                                 say(s.state.chainTarget) +
                                 ") is no longer there, so advanced.real_dll has been cleared.");
        }
    }

    // ---- existing = owned: a vendor runtime or notice (NVIDIA's DLSS, Intel's XeSS-SR) placed beside the game. Three rules: a copy that
    // is not EDVR's -- the user's own, or one the vendor's updater replaced -- is left alone; only the copy the record says EDVR placed
    // is ever replaced or removed, by its hash; and where `when` rules the adapter out (the DLSS runtime is 59 MB and no other card
    // can use it) nothing is placed, though a copy already there is still recognised. The report says what happened to each file
    // it has a name for (kBlurbs), whatever the outcome. ----
    // Intel's licence terms travel with the runtime: one without its notices is not installed at all.
    if (p.find("xess") && (!p.find("xess_license") || !p.find("xess_third_party"))) {
        plan.blocked = true;
        plan.problems.push_back("XeSS runtime lacks its license notices; nothing will be installed.");
        return plan;
    }
    auto placeOwned = [&](const Placed& f) {
        const std::string& id = f.entry->file.id;
        const std::string& sha = f.entry->sha;
        const std::wstring& dst = f.dst;
        const DllInfo& cur = f.cur;
        const std::string leaf = say(leafOf(dst));
        const FileBlurb* blurb = blurbFor(id);
        const std::string named = blurb && blurb->what ? leaf + " (" + blurb->what + ")" : leaf;
        const bool present = cur.kind != DllKind::Absent && cur.kind != DllKind::Unreadable;
        const std::string previous = s.state.shaOf(id);
        const bool ours = present && !previous.empty() && cur.sha256 == previous;
        if (cur.kind == DllKind::Unreadable) {
            // Unreadable says nothing about whose it is: the record stays as it was.
            plan.problems.push_back("The optional file " + leaf + " cannot be read -- something has it open; it was left untouched.");
        } else if (present && cur.sha256 == sha && !o.repair) {
            if (blurb) plan.notes.push_back(named + " is already this build's -- left alone.");
            recordFile(f, sha);
        } else if (present && !ours && cur.sha256 != sha) {
            plan.notes.push_back("A different " + leaf + " is present; left alone.");
            recordFile(f, std::string());
        } else if (!f.canPlace) {
            plan.notes.push_back("No NVIDIA graphics card was found, so " + named + " was not placed." +
                                 (blurb ? std::string(" ") + blurb->purpose + " needs one, and runs ED-UP's own history without it." : std::string()));
            recordFile(f, ours ? previous : std::string());
        } else {
            if (present) backup(dst, "the " + leaf + " ED-UP placed, being replaced");
            writePayload(id, dst, present ? "updates " + leaf : "places " + leaf + " beside the game");
            if (blurb)
                plan.notes.push_back(present ? "Updating " + named + "."
                                             : "Placing " + leaf + " (" + blurb->what + ", " + blurb->purpose + ").");
            recordFile(f, sha);
        }
    };
    each(ExistingMode::Owned, placeOwned);
    for (const FileRecord& r : s.state.files) {
        // This installer carries no bytes for a vendor file (a build without the SDK leaves it out of its manifest): what an earlier
        // one placed stays, and stays recorded as ours.
        if (r.existing != existingName(ExistingMode::Owned) || p.find(r.id)) continue;
        const FileBlurb* blurb = blurbFor(r.id);
        const std::string leaf = r.path.substr(r.path.find_last_of('/') + 1);   // npos + 1 is 0: the whole path
        plan.notes.push_back("This installer carries no " + (blurb ? std::string(blurb->label) + " runtime" : leaf) + "; the " + leaf +
                             " a previous one placed is left where it is.");
    }

    // ---- existing = runtime: the game-facing OpenVR ABI is always provided by native OpenXR. The original DLL is retained for
    // uninstall, never loaded by this backend. ----
    auto placeRuntime = [&](const Placed& f) {
        const std::wstring& runtime = f.dst;
        const std::string& id = f.entry->file.id;
        const std::string& sha = f.entry->sha;
        const std::wstring runtimeDir = runtime.substr(0, runtime.find_last_of(L"\\/"));
        std::wstring originalName = safeSiblingName(s.openvrOrigName, leafOf(runtime));
        if (originalName.empty()) originalName = kOpenvrOrig;
        const std::wstring original = joinPath(runtimeDir, originalName);
        const DllInfo& current = f.cur;
        const DllInfo& saved = s.openvrOrig;
        next.openvrOrigName = originalName;
        next.openvrOrigSha = saved.kind == DllKind::OpenVrRuntime ? saved.sha256 : std::string();
        // The tool kept the game's original under its own name. Copy it to the name this installer keeps it under, so the record and the
        // next survey find it as any other original, and a later flat conversion or uninstall can restore it. The tool's backup stays put.
        // Not when the game's own runtime is in place: that one is renamed to the original name below, as it always was.
        if (saved.kind == DllKind::Absent && current.kind != DllKind::OpenVrRuntime && s.toolOriginal.kind == DllKind::OpenVrRuntime) {
            Step adopt; adopt.action = Action::Backup; adopt.from = s.toolOriginal.path; adopt.to = original;
            adopt.expectSha = s.toolOriginal.sha256; adopt.required = true;
            adopt.why = "keeps the game's original VR library, from the backup the tool made, where ED-UP can restore it";
            body.push_back(adopt); changed = true;
            next.openvrOrigSha = s.toolOriginal.sha256;
            plan.notes.push_back("Adopting the game's original VR library from the backup the tool made, so it can be restored later.");
        }
        if (current.kind == DllKind::Edvr && current.sha256 == sha && !o.repair) {
            plan.notes.push_back("Native OpenXR runtime is already this build -- left alone.");
        } else {
            if (current.kind != DllKind::Absent) {
                backup(runtime, "the previous VR library, for recovery");
                body.back().required = true; body.back().expectSha = current.sha256;
                if (current.kind == DllKind::OpenVrRuntime &&
                    (saved.kind == DllKind::Absent || (saved.kind == DllKind::OpenVrRuntime && saved.sha256 != current.sha256))) {
                    if (saved.kind == DllKind::OpenVrRuntime) {
                        backup(original, "the superseded original runtime");
                        body.back().required = true; body.back().expectSha = saved.sha256;
                        plan.notes.push_back("The game updated its VR library; keeping the newer original for uninstall.");
                    }
                    rename(runtime, original, "preserves the original runtime for uninstall", current.sha256);
                    next.openvrOrigSha = current.sha256;
                }
            }
            writePayload(id, runtime, "installs native OpenXR using the Windows-selected runtime");
            plan.notes.push_back("Installing native OpenXR. The original VR library is not used at runtime.");
        }
        recordFile(f, sha);
        next.nativeOriginalName = next.openvrOrigName; next.nativeOriginalSha = next.openvrOrigSha;
    };
    each(ExistingMode::Runtime, placeRuntime);

    // ---- existing = replace: back up whatever is there, then write; a file that already is this build's is left alone. ----
    auto placeReplace = [&](const Placed& f) {
        const std::string& id = f.entry->file.id;
        const std::string& sha = f.entry->sha;
        const DllInfo& current = f.cur;
        if (!(current.kind != DllKind::Absent && current.sha256 == sha && !o.repair)) {
            if (current.kind != DllKind::Absent) {
                backup(f.dst, "the previous " + say(leafOf(f.dst)));
                body.back().required = true; body.back().expectSha = current.sha256;
            }
            writePayload(id, f.dst, "installs " + say(leafOf(f.dst)));
        }
        recordFile(f, sha);
    };
    each(ExistingMode::Replace, placeReplace);

    if (!flat) {
        // The startup config is a fixed step: it names the loader and the graphics file the payload places, by their resolved targets.
        const std::string config = "[openxr]\r\nversion=1\r\nloader=" +
            toUtf8(targetPathOf(s, loaderFile->file.target)) + "\r\ngraphics=" +
            toUtf8(targetPathOf(s, graphicsFile->file.target)) + "\r\nruntime=system\r\nseparate_device=1\r\n";
        next.nativeConfigSha = sha256Bytes(config.data(), config.size());
        if (s.nativeConfig.sha256 != next.nativeConfigSha || o.repair) {
            const auto destination = joinPath(nativeDir, L"edvr_openxr.ini");
            if (s.nativeConfig.kind != DllKind::Absent) {
                backup(destination, "the previous OpenXR startup configuration");
                body.back().required = true; body.back().expectSha = s.nativeConfig.sha256;
            }
            writeText(config, destination, "selects the Windows OpenXR runtime"); changed = true;
        }
    } else {
        // Flat owns no VR runtime. What a VR install left in Openvr\win64 is retired only by its proofs: the runtime when it is EDVR's
        // build (proved above), and each OpenXR file by its hash or its content, whether or not this folder's record lists it.
        const std::wstring runtime = joinPath(nativeDir, kOpenvr);
        if (retireRuntime) {
            backup(runtime, "the ED-UP VR runtime being retired");
            body.back().required = true; body.back().expectSha = s.openvrCurrent.sha256;
            Step removeRuntime; removeRuntime.action = Action::Delete; removeRuntime.from = runtime;
            removeRuntime.expectSha = s.openvrCurrent.sha256;
            removeRuntime.why = "retires ED-UP's VR runtime"; body.push_back(removeRuntime); changed = true;
            const bool savedOriginal = s.openvrOrig.kind == DllKind::OpenVrRuntime;
            Step restoreOriginal;
            restoreOriginal.action = savedOriginal ? Action::Rename : Action::Backup;
            restoreOriginal.from = savedOriginal ? s.openvrOrig.path : s.openvrOrigInBackups.front();
            restoreOriginal.to = runtime;
            restoreOriginal.expectSha = savedOriginal ? s.openvrOrig.sha256 : sha256File(restoreOriginal.from);
            restoreOriginal.required = true;
            restoreOriginal.why = "restores the game's original VR library";
            body.push_back(restoreOriginal);
        }
        // The startup config is a fixed step with no record of its own: EDVR's by its text, or by its hash and the record or receipt.
        if (s.nativeConfig.kind != DllKind::Absent) {
            if (startupConfigOwned(s)) {
                backup(s.nativeConfig.path, "the ED-UP OpenXR asset being retired");
                body.back().required = true; body.back().expectSha = s.nativeConfig.sha256;
                Step retire; retire.action = Action::Delete; retire.from = s.nativeConfig.path;
                retire.expectSha = s.nativeConfig.sha256; retire.why = "retires ED-UP's OpenXR asset";
                body.push_back(retire);
                changed = true;
            } else {
                plan.notes.push_back("Keeping a changed OpenXR file: " + say(leafOf(s.nativeConfig.path)));
            }
        }
        // The VR runtime and the OpenXR files go from the record: nothing is owned beside the game's own runtime any more.
        next.openvrOrigName.clear(); next.openvrOrigSha.clear(); next.nativeConfigSha.clear();
        next.nativeOriginalName.clear(); next.nativeOriginalSha.clear();
        if (retireRuntime) {
            plan.notes.push_back("Converting VR to flat: the game's original VR library is in place and verified ED-UP OpenXR assets are retired.");
        } else if (!installed.empty() && installed != "flat") {
            plan.notes.push_back("Converting to flat: the VR library here is not ED-UP's, so it is left where it is.");
        }
    }

    // ---- Files an earlier install placed that this one does not: an id the manifest dropped, or one the scope no longer covers (the
    // OpenXR files on a flat conversion). Each goes only while it is still the copy the record names, by hash; an edited copy stays
    // where it is and the plan says so. A replaced file is backed up first; a vendor file (owned) is not, as for an uninstall. ----
    for (const std::string& note : recordNotes) plan.notes.push_back(note);
    if (!s.state.refusedRecords.empty())
        plan.notes.push_back("The install record has " + std::to_string(s.state.refusedRecords.size()) +
                             " entry(ies) it cannot use (a path outside its folder, or an unknown name); they were ignored.");
    for (const Candidate& c : retirees) {
        if (!c.owned) {
            plan.notes.push_back(keepNote(c));
            continue;
        }
        if (c.mode == ExistingMode::Replace) {
            backup(c.path, "the ED-UP file being retired");
            body.back().required = true; body.back().expectSha = c.info.sha256;
        }
        Step retire; retire.action = Action::Delete; retire.from = c.path;
        retire.expectSha = c.info.sha256; retire.why = "retires a file ED-UP placed that this install does not";
        body.push_back(retire);
        changed = true;
        plan.notes.push_back("Removing " + say(leafOf(c.path)) + ": this install no longer places it.");
    }
    // The record keeps what this install placed. An id it did not place is dropped from it, kept or removed alike, except a vendor
    // file (owned) this payload carries no bytes for: that one stays where it is, and stays recorded.
    for (const FileRecord& r : s.state.files) {
        bool placedHere = false;
        for (const Placed& f : placed) placedHere |= f.entry->file.id == r.id;
        const bool keptVendor = r.existing == existingName(ExistingMode::Owned) && !p.find(r.id);
        if (!placedHere && !keptVendor) next.drop(r.id);
    }

    // The descriptor is the scope's canonical text, not the payload's: a scope change rewrites it.
    const std::wstring descriptorPath = joinPath(s.game.dir, kProfile);
    if (!s.descriptorPresent || s.descriptorSha != descriptorSha || o.repair) {
        if (s.descriptorPresent) {
            backup(descriptorPath, "the previous ED-UP profile descriptor");
            body.back().required = true; body.back().expectSha = s.descriptorSha;
        }
        writeText(descriptor, descriptorPath, "declares the installed ED-UP scope");
        changed = true;
    }

    // ------------------------------------------------------------------
    // The settings files the scope installs: edvr.ini for vr and both, edvr-flat.ini for flat and both. Last, because the chain
    // decision above forces a value into them.
    //
    // A file the scope does not install is neither merged from nor written. The flat
    // runtime reads edvr-flat.ini and only falls back to edvr.ini while there is
    // none (config.cpp), so a flat install that merged into edvr.ini reported
    // success while the game went on reading the old flat file -- and, where
    // there was no flat file, changed the VR profile's tuning instead. A scope
    // change leaves the file it stops managing exactly where it is.
    // ------------------------------------------------------------------
    // Each settings file the scope installs is merged on its own, against its own base. The flat file is done first, so the VR
    // file's merge report (plan.merge) is the one left when both are installed.
    plan.settingsFile = wantsVrIni ? "edvr.ini" : "edvr-flat.ini";
    // The base is the shipped text of the version that last wrote the file: edvr.ini.base for edvr.ini, edvr-flat.ini.base for
    // edvr-flat.ini. Which version wrote edvr.ini.base is the RECORD's question, not the descriptor's: the descriptor says what this
    // run installs, and an older flat record left its flat defaults in that base. So the base vouches for the VR file only under a
    // VR-scope record (vr or both); a flat record, or none, vouches for nothing. Where a base does not describe the file, the merge
    // is two-way.
    const std::string baseWriter = s.state.present ? recordedScope(s.state) : std::string();
    const bool vrBaseDescribes = baseWriter == "vr" || baseWriter == "both";
    const std::string* vrBase = (vrBaseDescribes && !s.baseIniText.empty()) ? &s.baseIniText : nullptr;
    const std::string* flatBase = s.flatBaseIniText.empty() ? nullptr : &s.flatBaseIniText;
    std::string vrSha, flatSha;

    // ---- advanced.real_dll: set once, in the shared edvr.ini ----
    // Both profiles read it from there (real_dll_choice.h); edvr-flat.ini may hold `none` to run flat without the chain, or a name to
    // override it. The chain decision above (`forced`) is routed here instead of being written into every file:
    //   a chain found in the d3d11.dll slot      -> edvr.ini; a name left in edvr-flat.ini is cleared, the new chain supersedes it
    //   a chain whose target has gone            -> cleared in edvr.ini, and a name in edvr-flat.ini
    //   a name only in edvr-flat.ini (the layout before this rule) -> moved: copied to edvr.ini, cleared in edvr-flat.ini
    //   a kept chain and a new edvr.ini          -> edvr.ini (see "Keeping the chain" above for why only a new one)
    // `none` in either file is a decision of the user's and is never touched.
    bool setShared = false, setFlat = false;
    std::string sharedSet;
    {
        bool decided = false;
        std::string decision;
        for (auto it = forced.begin(); it != forced.end();) {
            if (it->first == kChainKey) { decided = true; decision = it->second; it = forced.erase(it); }
            else ++it;
        }
        const std::string sharedNow = s.iniPresent ? realDllTrimmed(iniValue(s.iniText, kChainKey)) : std::string();
        const std::string flatNow = s.flatIniPresent ? realDllTrimmed(iniValue(s.flatIniText, kChainKey)) : std::string();
        const bool flatNames = !flatNow.empty() && !realDllIsNone(flatNow) && o.keepSettings;
        const bool sharedNames = !sharedNow.empty() && !realDllIsNone(sharedNow);
        if (decided && !decision.empty()) {
            setShared = true; sharedSet = decision;
            setFlat = flatNames && wantsFlatIni;
            if (setFlat)
                plan.notes.push_back("advanced.real_dll in edvr-flat.ini (" + flatNow + ") is cleared: the chain to " + decision +
                                     " is kept once, in edvr.ini, and applies to both profiles.");
        } else if (decided) {
            setShared = sharedNames;
            setFlat = flatNames && wantsFlatIni;
        } else if (flatNames && sharedNow.empty()) {
            setShared = true; sharedSet = flatNow;
            setFlat = wantsFlatIni;
            plan.notes.push_back(
                "Moving advanced.real_dll (" + flatNow + ") from edvr-flat.ini to edvr.ini, so it is set once and applies to both the VR and "
                "flat profiles." +
                (wantsFlatIni ? std::string(" Both files are backed up first; edvr-flat.ini keeps no value for it (write real_dll = none "
                                            "there to run flat without the chain).")
                              : std::string(" edvr-flat.ini is outside this install and is left as it is.")));
        } else if (keptChain && sharedNow.empty() && (!s.iniPresent || !o.keepSettings)) {
            setShared = true; sharedSet = say(next.chainTarget);
            plan.notes.push_back("Writing advanced.real_dll = " + sharedSet + " to " +
                                 (s.iniPresent ? "the new edvr.ini" : "edvr.ini") +
                                 ": the kept chain applies to both the VR and flat profiles.");
        }
    }
    using IniPairs = std::vector<std::pair<std::string, std::string>>;
    IniPairs vrForced = forced, flatForced = forced;
    if (setShared) vrForced.emplace_back(kChainKey, sharedSet);
    if (setFlat) flatForced.emplace_back(kChainKey, "");

    // ---- existing = settings: the shipped text merged into the file already there, three-way against its .base copy. ----
    auto manageSettings = [&](const Placed& file) {
        const bool isFlat = file.entry->file.when.profiles == kProfileFlat;
        const char* profile = isFlat ? "flat" : "vr";
        const std::string iniName = toUtf8(settingsLeafFor(profile));
        const std::string payloadText = file.entry->text();
        const std::wstring iniPath = file.dst;
        const bool ownPresent = isFlat ? s.flatIniPresent : s.iniPresent;
        const std::string& ownText = isFlat ? s.flatIniText : s.iniText;

        // A flat install that finds only the shared edvr.ini starts its own file
        // from it, so the settings the flat runtime has been reading carry over
        // (install_edvr.py seeds the same way); edvr.ini itself is not touched.
        // Somebody who asked for fresh defaults gets them instead.
        const bool seed = isFlat && !ownPresent && s.iniPresent && o.keepSettings;
        // A seed is edvr.ini's text, so it merges against edvr.ini's base. The file's own base is the one
        // edvr-flat.ini.base the flat install that wrote it kept.
        const std::string* base = (isFlat && !seed) ? flatBase : vrBase;
        // A seed is edvr.ini's text, so it carries the shared chain value; the flat file keeps none (it is the shared file's to hold).
        IniPairs useForced = isFlat ? flatForced : vrForced;
        if (seed) {
            const std::string carried = realDllTrimmed(iniValue(s.iniText, kChainKey));
            if (!carried.empty() && !realDllIsNone(carried)) useForced.emplace_back(kChainKey, "");
        }

        std::string merged;
        if (seed) {
            merged = mergeIni(payloadText, s.iniText, base, useForced, &plan.merge);
            plan.notes.push_back(
                "Creating " + iniName + " from your edvr.ini: the flat edition keeps its own "
                "settings now, and edvr.ini is left exactly as it is.");
        } else if (!ownPresent) {
            merged = mergeIni(payloadText, std::string(), nullptr, useForced, &plan.merge);
            plan.notes.push_back("Writing " + iniName + " (every setting at its default).");
        } else if (!o.keepSettings) {
            merged = mergeIni(payloadText, std::string(), nullptr, useForced, &plan.merge);
            backup(iniPath, "your " + iniName + ", before it is replaced");
            plan.notes.push_back(
                "Replacing " + iniName + " with the shipped defaults. Your old file is in the "
                "backup folder.");
        } else {
            // The user's text is this edition's own file, so a setting this version dropped is
            // "carried over from your" that file: edvr-flat.ini for the flat edition.
            merged = mergeIni(payloadText, ownText, base, useForced, &plan.merge, iniName.c_str());
            if (merged != ownText) {
                backup(iniPath, "your " + iniName + ", before it is updated");

                char line[256];
                sprintf_s(
                    line, "Updating %s: %zu of your settings kept, %zu new defaults adopted%s.",
                    iniName.c_str(), plan.merge.kept.size(), plan.merge.adopted.size(),
                    plan.merge.twoWay ? " (no record of which version you had, so anything that "
                                        "differs from the new defaults was treated as yours)"
                                      : "");
                plan.notes.push_back(line);
                if (!plan.merge.retired.empty()) {
                    plan.notes.push_back(
                        "Some settings you had set no longer exist in this version; they were "
                        "carried to the end of their section with a note rather than dropped.");
                }
            }
        }
        if (merged != ownText) {
            writeText(merged, iniPath, "writes " + iniName);
            changed = true;
        } else {
            // Said once, and only here: an "updating" note followed by "left
            // alone" is two lines contradicting each other about the same file.
            plan.notes.push_back(iniName + " already says what it should -- left alone.");
        }
        (isFlat ? flatSha : vrSha) = sha256Bytes(merged.data(), merged.size());
        recordFile(file, isFlat ? flatSha : vrSha);
    };
    // The flat file first, so the VR file's merge report (plan.merge) is the one left when both are installed.
    for (const bool flatFile : {true, false}) {
        for (const Placed& file : placed) {
            if (file.entry->file.existing == ExistingMode::Settings && file.canPlace &&
                (file.entry->file.when.profiles == kProfileFlat) == flatFile)
                manageSettings(file);
        }
    }
    // A flat-only install does not manage edvr.ini, but the chain lives there: the one value both profiles read. Only that key is
    // written, into the file as it is (everything else in it stays byte for byte), or into a new file that holds nothing else. A copy
    // goes to the backup folder first, as for any file this installer changes.
    if (!wantsVrIni && setShared && (s.iniPresent || !sharedSet.empty())) {
        const std::wstring sharedPath = settingsPathFor(s.game.dir, "vr");
        std::string text;
        if (s.iniPresent) {
            text = mergeIni(s.iniText, s.iniText, &s.iniText, IniPairs{{kChainKey, sharedSet}}, nullptr);
        } else {
            text = "# ED-UP settings shared by the VR and flat profiles. advanced.real_dll names the mod ED-UP chains through;\r\n"
                   "# edvr-flat.ini can override it for the flat profile, or say real_dll = none to run flat without it.\r\n"
                   "[advanced]\r\nreal_dll = " + sharedSet + "\r\n";
        }
        if (text != s.iniText) {
            if (s.iniPresent) backup(sharedPath, "your edvr.ini, before the chain is written to it");
            writeText(text, sharedPath, "writes advanced.real_dll to edvr.ini");
            changed = true;
            plan.notes.push_back(std::string(s.iniPresent ? "Updating" : "Creating") +
                                 " edvr.ini: advanced.real_dll = " + sharedSet + ", the one place both profiles read it from.");
        }
    }
    // The record's settings digest is the VR file's when the scope has one (the file the older records named), else the flat file's.
    next.iniSha = wantsVrIni ? vrSha : flatSha;

    // Which vendor files (owned) EDVR claims changed without a file moving: a copy that is already the payload's is adopted, a claim on
    // a copy someone else replaced is given up. Only the record changes then, and it still has to be written.
    bool ownershipChanged = false;
    for (const FileRecord& r : next.files)
        if (r.existing == existingName(ExistingMode::Owned) && r.sha256 != s.state.shaOf(r.id)) ownershipChanged = true;
    for (const FileRecord& r : s.state.files)
        if (r.existing == existingName(ExistingMode::Owned) && r.sha256 != next.shaOf(r.id)) ownershipChanged = true;
    const bool stateOnly = !changed && body.empty() && ownershipChanged && !o.repair;
    if (stateOnly)
        plan.notes.push_back("Optional runtime ownership changed; only the install record will be updated.");
    if (!changed && !ownershipChanged && !o.repair) {
        plan.nothingToDo = true;
        plan.notes.push_back("Everything is already in place. Nothing to do.");
    }

    // ------------------------------------------------------------------
    // Assemble: directories first, then the body, then the record. The record
    // is written LAST so that a run interrupted half way leaves the old one,
    // which still describes the folder better than a half-true new one.
    // ------------------------------------------------------------------
    if (!plan.nothingToDo) {
        if (wantBackupDir) {
            Step mk;
            mk.action = Action::MakeDir;
            mk.to = plan.backupDir;
            mk.why = "keeps a copy of everything replaced";
            plan.steps.push_back(mk);
        }
        if (!flat) {
            Step nativeFolder;nativeFolder.action=Action::MakeDir;nativeFolder.to=nativeDir;
            nativeFolder.why="holds the native OpenXR runtime and loader";plan.steps.push_back(nativeFolder);
        }
        // A target below the root may name a folder that is not there yet: every written file's folder is made first, unless it is one of
        // the two roots above or already exists.
        std::vector<std::wstring> madeDirs;
        for (const std::wstring& dir : targetDirs) {
            if (_wcsicmp(dir.c_str(), s.game.dir.c_str()) == 0 || _wcsicmp(dir.c_str(), nativeDir.c_str()) == 0 || dirExists(dir)) continue;
            bool made = false;
            for (const std::wstring& d : madeDirs) made |= _wcsicmp(d.c_str(), dir.c_str()) == 0;
            if (made) continue;
            madeDirs.push_back(dir);
            Step mkTarget; mkTarget.action = Action::MakeDir; mkTarget.to = dir;
            mkTarget.why = "holds a file the install places below its folder";
            plan.steps.push_back(mkTarget);
        }
        plan.steps.insert(plan.steps.end(), body.begin(), body.end());

        Step mkState;
        mkState.action = Action::MakeDir;
        mkState.to = stateDirPath(s.game.dir);
        mkState.why = "holds the install record";
        plan.steps.push_back(mkState);

        // Each base is written from its own payload file, by the install that manages that file.
        if (!stateOnly) {
            if (wantsVrIni) {
                Step baseStep;
                baseStep.action = Action::WriteText;
                baseStep.text = vrSettings->text();
                baseStep.to = baseIniPath(s.game.dir);
                baseStep.why = "keeps this version's default edvr.ini, so the next update can tell your changes from a changed default";
                plan.steps.push_back(baseStep);
            }
            if (wantsFlatIni) {
                Step baseStep;
                baseStep.action = Action::WriteText;
                baseStep.text = flatSettings->text();
                baseStep.to = flatBaseIniPath(s.game.dir);
                baseStep.why = "keeps this version's default edvr-flat.ini, so the next update can tell your changes from a changed default";
                plan.steps.push_back(baseStep);
            }
        }

        Step rec;
        rec.action = Action::WriteText;
        rec.text = serializeState(next);
        rec.to = statePath(s.game.dir);
        rec.why = "records what was installed";
        plan.steps.push_back(rec);
    }

    plan.nextState = next;
    return plan;
}

Plan planUninstall(const Survey& s, const Options& o, const PayloadInfo& p) {
    Plan plan;
    const FileRecord* runtimeRecord = s.state.withExisting(existingName(ExistingMode::Runtime));
    const FileRecord* chainRecord = s.state.withExisting(existingName(ExistingMode::Chain));
    if(((runtimeRecord && !runtimeRecord->sha256.empty()) || s.openvrCurrent.nativeRuntimeExports) && s.openvrCurrent.kind==DllKind::Edvr &&
       s.openvrOrig.kind!=DllKind::OpenVrRuntime && s.openvrOrigInBackups.empty()) {
        plan.blocked=true;plan.problems.push_back("The original VR library is missing. Repair the game files before uninstalling; the native pair was left intact.");return plan;
    }
    plan.backupDir =
        joinPath(backupRootPath(s.game.dir), o.backupStamp.empty() ? L"backup" : o.backupStamp);

    if (s.gameRunningHere) {
        plan.blocked = true;
        plan.problems.push_back("Elite Dangerous is running. Close it first.");
        return plan;
    }
    if (s.gameRunStateUnknown) {
        plan.blocked = true;
        plan.problems.push_back(kRunStateUnknown);
        return plan;
    }
    if (s.gameRunningElsewhere) plan.notes.push_back(kRunningElsewhere);

    // Which scope this uninstall removes, and so which settings files are its own.
    // A folder with no record and no descriptor counts as a VR one: the flat
    // edition always writes a descriptor.
    const std::string installed = installedScope(s);
    const std::string scope = installed.empty() ? std::string("vr") : installed;
    const bool flatEdition = scope == "flat";
    // The file the chain was read from, for the report: edvr.ini unless the flat file supplied it.
    std::string runtimeIniName = "edvr.ini";

    std::vector<Step> body;
    bool wantBackupDir = false;
    bool changed = false;

    std::vector<std::pair<std::wstring, std::wstring>> backupNames;  // file name -> the path that took it (backupNameFor)
    auto backup = [&](const std::wstring& path, const std::string& why) {
        Step st;
        st.action = Action::Backup;
        st.from = path;
        st.to = joinPath(plan.backupDir, backupNameFor(s, path, &backupNames));
        st.why = why;
        body.push_back(st);
        wantBackupDir = true;
    };
    auto remove = [&](const std::wstring& path, const std::string& why,
                      const std::string& expectSha = std::string()) {
        Step st;
        st.action = Action::Delete;
        st.from = path;
        st.why = why;
        st.expectSha = expectSha;   // do not delete a file that changed under us
        body.push_back(st);
        changed = true;
    };
    auto rename = [&](const std::wstring& from, const std::wstring& to, const std::string& why,
                      const std::string& expectSha = std::string()) {
        Step st;
        st.action = Action::Rename;
        st.from = from;
        st.to = to;
        st.why = why;
        st.expectSha = expectSha;
        st.required = true;
        body.push_back(st);
        changed = true;
    };
    auto restore = [&](const std::wstring& from, const std::wstring& to, const std::string& why) {
        Step st;
        st.action = Action::Backup;  // a copy: the backup folder keeps its copy
        st.from = from;
        st.to = to;
        st.why = why;
        st.required = true;
        body.push_back(st);
        changed = true;
    };

    // The files this uninstall acts on, from the install records: the chained graphics file and the runtime have their own restore rules
    // below; every other record is a candidate for removal (removalCandidates). A payload, when the caller gives one, only widens
    // that for a folder whose record was lost; an uninstall given none works from the records and the tool's receipts alone.
    std::vector<std::string> recordNotes;
    const std::vector<Candidate> doomed = removalCandidates(s, p, true, 0, &recordNotes);
    for (const std::string& note : recordNotes) plan.notes.push_back(note);
    if (!s.state.refusedRecords.empty())
        plan.notes.push_back("The install record has " + std::to_string(s.state.refusedRecords.size()) +
                             " entry(ies) it cannot use (a path outside its folder, or an unknown name); they were ignored.");
    std::vector<DllInfo> probed;
    // A record's path, or the default when there is no record or the record's path is refused.
    auto recordedPath = [&](const FileRecord* r, const std::wstring& fallback) {
        std::wstring path;
        if (r && recordPathOf(s, *r, &path)) return path;
        if (r) plan.notes.push_back("The install record names a path outside its folder for " + r->id + "; it was not touched.");
        return fallback;
    };

    // ---- the chained graphics file ------------------------------------
    const std::wstring d3d11Path = recordedPath(chainRecord, joinPath(s.game.dir, kD3d11));
    const DllInfo d3d11Info = targetInfoOf(s, d3d11Path, &probed);
    // EDVR's by what probeDll reads it as, or by the record's hash.
    const bool d3d11Ours = d3d11Info.kind == DllKind::Edvr ||
                           (chainRecord && ownedFile(s, d3d11Info, true, chainRecord->sha256, std::string()));
    if (d3d11Ours) {
        backup(d3d11Path, "ED-UP's d3d11.dll");
        remove(d3d11Path, "removes ED-UP's d3d11.dll", d3d11Info.sha256);

        // Whatever we moved aside goes back under the name it had. Without
        // this, uninstalling EDVR silently uninstalls the other mod too: its
        // file is still on disk but under a name nothing loads.
        std::wstring chain = safeSiblingName(s.state.chainTarget, kD3d11);
        // The value the runtime would chain through: the shared edvr.ini's, which the flat file may override (real_dll_choice.h). The
        // flat file counts for the scopes that install it; a VR install ignores a flat file lying beside it.
        // A `none` only switches the chain off for a profile; the mod it left parked still has to go back, so a name in either file
        // answers when the effective value does not.
        if (chain.empty()) {
            const std::string sharedValue = iniValue(s.iniText, kChainKey);
            const std::string flatValue = scope != "vr" ? iniValue(s.flatIniText, kChainKey) : std::string();
            const RealDllChoice c = chooseRealDll(scope != "vr", flatValue, sharedValue);
            std::string name = c.value;
            if (name.empty() && !realDllIsNone(sharedValue)) name = realDllTrimmed(sharedValue);
            if (name.empty() && !realDllIsNone(flatValue)) name = realDllTrimmed(flatValue);
            if (!name.empty()) chain = safeSiblingName(fromUtf8(name), kD3d11);
            if (name == realDllTrimmed(flatValue) && name != realDllTrimmed(sharedValue)) runtimeIniName = "edvr-flat.ini";
        }
        const DllInfo* target = chain.empty() ? nullptr : siblingNamed(s.otherD3d11, chain);
        if (target && target->kind != DllKind::Absent) {
            rename(target->path, d3d11Path,
                   "puts " + (s.state.chainMod.empty() ? std::string("the other mod")
                                                       : say(s.state.chainMod)) +
                       " back under its own name",
                   target->sha256);
            plan.notes.push_back("Removing ED-UP's d3d11.dll and restoring " + say(chain) +
                                 " as d3d11.dll.");
        } else {
            plan.notes.push_back("Removing ED-UP's d3d11.dll.");
            if (!chain.empty()) {
                plan.problems.push_back(
                    runtimeIniName + " pointed at " + say(chain) +
                    ", but that file is not there any more, so there is nothing to put back.");
            }
        }
    } else if (d3d11Info.kind == DllKind::Absent) {
        plan.notes.push_back("No d3d11.dll to remove.");
    } else {
        plan.notes.push_back("The d3d11.dll here is not ED-UP's (" + say(describeDll(d3d11Info)) +
                             ") -- left alone.");
    }

    // ---- the runtime --------------------------------------------------
    if (s.haveOpenvrDir) {
        const std::wstring dst = recordedPath(runtimeRecord, joinPath(s.game.openvrDir, kOpenvr));
        const DllInfo current = targetInfoOf(s, dst, &probed);
        const bool runtimeOurs = current.kind == DllKind::Edvr ||
                                 (runtimeRecord && ownedFile(s, current, true, runtimeRecord->sha256, std::string()));
        std::wstring origName = safeSiblingName(s.openvrOrigName, kOpenvr);
        if (origName.empty()) origName = kOpenvrOrig;
        const std::wstring origPath = joinPath(s.game.openvrDir, origName);
        if (runtimeOurs) {
            if (s.openvrOrig.kind == DllKind::OpenVrRuntime) {
                backup(dst, "ED-UP's openvr_api.dll");
                remove(dst, "removes ED-UP's openvr_api.dll", current.sha256);
                rename(origPath, dst, "puts the game's own openvr_api.dll back",
                       s.openvrOrig.sha256);
                plan.notes.push_back(
                    "Removing ED-UP's openvr_api.dll and renaming the game's original back.");
            } else if (!s.openvrOrigInBackups.empty()) {
                // The renamed original is gone, but this installer kept a copy
                // of it before it ever renamed anything. Uninstalling from that
                // copy is a great deal better than sending somebody to their
                // launcher's file verification.
                const std::wstring& copy = s.openvrOrigInBackups.front();
                backup(dst, "ED-UP's openvr_api.dll");
                restore(copy, dst, "puts the game's own openvr_api.dll back from an ED-UP backup");
                plan.notes.push_back(
                    "The renamed original is missing, so the game's own openvr_api.dll is being "
                    "restored from the backup this installer made before it first renamed it.");
            } else {
                // Deleting ours here would leave NO openvr_api.dll at all --
                // worse than leaving EDVR installed, and done in the name of
                // removing it. The file stays until there is something to put
                // in its place.
                plan.problems.push_back(
                    "ED-UP's openvr_api.dll has been left in place: the game's original is not "
                    "there to put back, and no backup of it was found, so removing ours would "
                    "leave that folder with no openvr_api.dll at all. Verify or repair the game "
                    "files in your launcher -- that restores the original -- and then uninstall "
                    "again.");
            }
        } else if (current.kind == DllKind::OpenVrRuntime &&
                   s.openvrOrig.kind == DllKind::OpenVrRuntime) {
            plan.notes.push_back(
                "The game's own openvr_api.dll is already in place; the spare copy at "
                "openvr_api_orig.dll was left where it is.");
        } else {
            plan.notes.push_back("No ED-UP openvr_api.dll to remove.");
        }
    }

    // ---- every other recorded file, and the startup config ------------
    // A file is removed only while it is still the copy the record names (or the tool placed); an edited copy is kept and the plan
    // says so. A replaced file is backed up first; a vendor runtime or notice (owned) is not: the payload already holds its bytes.
    for (const Candidate& c : doomed) {
        if (!c.owned) {
            plan.notes.push_back(keepNote(c));
            continue;
        }
        if (c.mode == ExistingMode::Replace) backup(c.path, "the ED-UP file being removed");
        remove(c.path, "removes a file ED-UP placed", c.info.sha256);
        const FileBlurb* blurb = blurbFor(c.id);
        if (blurb) plan.notes.push_back("Removing " + say(leafOf(c.path)) + " (" + blurb->what + ", which ED-UP placed).");
    }
    // The startup config is a fixed step with no record: EDVR's by its text, or by its hash and the record or the tool's receipt.
    if (s.nativeConfig.kind != DllKind::Absent) {
        if (startupConfigOwned(s)) {
            backup(s.nativeConfig.path, "the native OpenXR file being removed");
            remove(s.nativeConfig.path, "removes ED-UP's OpenXR dependency", s.nativeConfig.sha256);
        } else {
            plan.notes.push_back("Keeping a changed OpenXR file: " + say(leafOf(s.nativeConfig.path)));
        }
    }

    // ---- settings and record ------------------------------------------
    // The settings files of the scope being removed, and only those: the flat
    // profile's edvr-flat.ini and the VR profile's edvr.ini are separate people's
    // tuning, and an uninstall that took a file outside its scope destroyed
    // settings it had no business touching. edvr.ini for vr and both, edvr-flat.ini for flat and both.
    plan.settingsFile = flatEdition ? "edvr-flat.ini" : "edvr.ini";
    for (const bool flatFile : {false, true}) {
        if (flatFile ? scope == "vr" : scope == "flat") continue;
        const char* profile = flatFile ? "flat" : "vr";
        const std::wstring iniPath = settingsPathFor(s.game.dir, profile);
        const std::string iniName = toUtf8(settingsLeafFor(profile));
        const bool ownPresent = flatFile ? s.flatIniPresent : s.iniPresent;
        if (ownPresent) {
            if (o.removeSettings) {
                backup(iniPath, "your " + iniName);
                remove(iniPath, "removes " + iniName);
                plan.notes.push_back("Removing " + iniName + " (a copy is kept in the backup folder).");
            } else {
                plan.notes.push_back(
                    "Leaving " + iniName + " in place, so a reinstall finds your settings again.");
            }
        } else if (flatFile && flatEdition && s.iniPresent) {
            // A flat install that never got a file of its own -- one made before
            // edvr-flat.ini existed -- has been reading the shared edvr.ini, which the
            // VR profile uses too. It is not the flat edition's to remove.
            plan.notes.push_back(
                o.removeSettings
                    ? "This flat install has no edvr-flat.ini of its own: it was reading edvr.ini, "
                      "which the VR profile uses too, so edvr.ini was left in place."
                    : "Leaving edvr.ini in place, so a reinstall finds your settings again.");
        }
    }
    if (fileExists(statePath(s.game.dir))) remove(statePath(s.game.dir), "removes the install record");
    const std::wstring descriptorPath = joinPath(s.game.dir, kProfile);
    // Its bytes are one of EDVR's descriptor texts, or its hash is the one the record kept: a leftover from an install whose record is gone
    // says `scope = both` in exactly EDVR's bytes, and is EDVR's whatever the record says.
    if (s.descriptorPresent && ((!s.state.descriptorSha.empty() && s.descriptorSha == s.state.descriptorSha) ||
                                isCanonicalDescriptor(s.descriptorSha))) {
        remove(descriptorPath, "removes ED-UP's profile descriptor", s.descriptorSha);
    } else if (s.descriptorPresent) {
        plan.notes.push_back("Keeping an unverified edvr_profile.ini; its ownership could not be proved.");
    }
    if (fileExists(baseIniPath(s.game.dir)))
        remove(baseIniPath(s.game.dir), "removes the kept default edvr.ini");
    if (fileExists(flatBaseIniPath(s.game.dir)))
        remove(flatBaseIniPath(s.game.dir), "removes the kept default edvr-flat.ini");

    if (!changed) {
        plan.nothingToDo = true;
        plan.notes.push_back("ED-UP does not appear to be installed here.");
    } else {
        if (wantBackupDir) {
            Step mk;
            mk.action = Action::MakeDir;
            mk.to = plan.backupDir;
            mk.why = "keeps a copy of everything removed";
            plan.steps.push_back(mk);
        }
        plan.steps.insert(plan.steps.end(), body.begin(), body.end());
        plan.notes.push_back(
            "Logs in edvr_logs\\ and the backups in edvr_backup\\ are left where they are; delete "
            "them by hand if you want them gone.");
    }
    return plan;
}

std::string planSummary(const Plan& plan) {
    std::string out;
    for (const std::string& n : plan.notes) out += "  - " + n + "\r\n";
    if (!plan.problems.empty()) {
        out += "\r\n";
        for (const std::string& p : plan.problems) out += "  ! " + p + "\r\n";
    }
    return out;
}

}  // namespace edvr::installer
