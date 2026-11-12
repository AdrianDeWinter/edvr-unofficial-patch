#include "install_manifest.h"

#include <initializer_list>
#include <set>

#include "update_util.h"

namespace edvr::installer {
namespace {

using Json = JsonValue;

bool failWith(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
}

bool isText(const Json* v) { return v && v->isString() && !v->text.empty(); }
bool isCount(const Json* v) { return v && v->isInt() && v->integer >= 0; }
bool isHash(const Json* v) { return v && v->isString() && isLowerHex(v->text, 64); }

// [A-Za-z0-9._-]+ and not made only of dots.
bool isAssetName(const Json* v) {
    if (!v || !v->isString() || v->text.empty()) return false;
    bool onlyDots = true;
    for (char c : v->text) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        if (!ok) return false;
        if (c != '.') onlyDots = false;
    }
    return !onlyDots;
}

// A relative path inside a zip: no backslash, colon or control or non-ASCII
// character, and no empty, "." or ".." segment.
bool isZipPath(const Json* v) {
    if (!v || !v->isString() || v->text.empty()) return false;
    for (char c : v->text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\\' || c == ':' || u < 0x20 || u > 0x7e) return false;
    }
    size_t start = 0;
    for (;;) {
        const size_t slash = v->text.find('/', start);
        const std::string part = v->text.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

// [a-z0-9_]{1,32}
bool isFileId(const Json* v) {
    if (!v || !v->isString() || v->text.empty() || v->text.size() > 32) return false;
    for (char c : v->text) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

// The object has every key in `required`, no key outside `required` and `optional`.
bool shapeOk(const Json& obj, std::initializer_list<const char*> required,
             std::initializer_list<const char*> optional, std::string* error, const std::string& where) {
    if (!obj.isObject()) return failWith(error, where + " must be an object");
    for (const char* key : required) {
        if (!obj.find(key)) return failWith(error, where + " lacks " + key);
    }
    for (const std::string& key : obj.keys) {
        bool known = false;
        for (const char* k : required) known = known || key == k;
        for (const char* k : optional) known = known || key == k;
        if (!known) return failWith(error, where + " has an unknown key");
    }
    return true;
}

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string foldAscii(const std::string& text) {
    std::string out = text;
    for (char& c : out) c = lowerAscii(c);
    return out;
}

bool isReservedDeviceName(const std::string& segment) {
    // The stem is what precedes the first dot; Windows also ignores spaces before that dot.
    std::string stem = segment.substr(0, segment.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    stem = foldAscii(stem);
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return true;
    return stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
           stem[3] >= '0' && stem[3] <= '9';
}

bool parseWhen(const Json& obj, ManifestWhen* out, std::string* error, const std::string& where) {
    if (!shapeOk(obj, {}, {"profile", "adapter"}, error, where)) return false;
    if (obj.keys.empty()) return failWith(error, where + " is empty");
    ManifestWhen when;
    when.present = true;
    if (const Json* profile = obj.find("profile")) {
        if (!profile->isArray() || profile->items.empty())
            return failWith(error, where + ".profile must be a non-empty list");
        for (const Json& entry : profile->items) {
            unsigned bit = 0;
            if (entry.isString() && entry.text == "vr") bit = kProfileVr;
            else if (entry.isString() && entry.text == "flat") bit = kProfileFlat;
            if (bit == 0) return failWith(error, where + ".profile holds a value that is not vr or flat");
            if (when.profiles & bit) return failWith(error, where + ".profile repeats a value");
            when.profiles |= bit;
        }
    }
    if (const Json* adapter = obj.find("adapter")) {
        if (!adapter->isString() || adapter->text != "nvidia")
            return failWith(error, where + ".adapter must be nvidia");
        when.adapterNvidia = true;
    }
    *out = when;
    return true;
}

bool parseExisting(const Json* v, ExistingMode* out) {
    if (!v || !v->isString()) return false;
    if (v->text == "replace") *out = ExistingMode::Replace;
    else if (v->text == "owned") *out = ExistingMode::Owned;
    else if (v->text == "chain") *out = ExistingMode::Chain;
    else if (v->text == "runtime") *out = ExistingMode::Runtime;
    else if (v->text == "settings") *out = ExistingMode::Settings;
    else return false;
    return true;
}

bool parseFile(const Json& file, size_t index, const std::set<std::string>& assetNames, ManifestFile* out,
               std::string* error) {
    const std::string where = "files[" + std::to_string(index) + "]";
    if (!shapeOk(file, {"id", "asset", "path", "sha256", "size", "target", "existing"}, {"when"}, error, where))
        return false;
    if (!isFileId(file.find("id"))) return failWith(error, where + ".id must match [a-z0-9_]{1,32}");
    ManifestFile f;
    f.id = file.find("id")->text;
    const std::string label = "file " + f.id;
    const Json* asset = file.find("asset");
    if (!asset->isString() || assetNames.find(asset->text) == assetNames.end())
        return failWith(error, label + " names no listed asset");
    if (!isZipPath(file.find("path"))) return failWith(error, label + " has a path that is not a plain relative path");
    if (!isHash(file.find("sha256"))) return failWith(error, label + " sha256 must be 64 lower-case hex characters");
    if (!isCount(file.find("size"))) return failWith(error, label + " size must be a non-negative integer");
    f.asset = asset->text;
    f.path = file.find("path")->text;
    f.sha256 = file.find("sha256")->text;
    f.size = static_cast<unsigned long long>(file.find("size")->integer);

    const Json& target = *file.find("target");
    if (!shapeOk(target, {"root", "path"}, {}, error, label + " target")) return false;
    const Json* root = target.find("root");
    if (root->isString() && root->text == "game") f.target.root = TargetRoot::Game;
    else if (root->isString() && root->text == "openvr") f.target.root = TargetRoot::Openvr;
    else return failWith(error, label + " target.root must be game or openvr");
    const Json* path = target.find("path");
    std::string why;
    if (!path->isString() || !isTargetPath(path->text, &why))
        return failWith(error, label + " target.path is refused" + (why.empty() ? std::string() : ": " + why));
    f.target.path = path->text;

    if (const Json* when = file.find("when")) {
        if (!parseWhen(*when, &f.when, error, label + " when")) return false;
    }
    if (!parseExisting(file.find("existing"), &f.existing))
        return failWith(error, label + " existing must be replace, owned, chain, runtime or settings");
    *out = std::move(f);
    return true;
}

bool startsWithFolded(const std::string& folded, const char* prefix) {
    const std::string p = prefix;
    return folded.size() >= p.size() && folded.compare(0, p.size(), p) == 0;
}

// Why a target may not be a payload file: it is a name or a folder the installer itself writes (the fixed steps), or, under the game
// root, it reaches into the default Openvr folder where it would alias the openvr root. Names are compared case-folded, the way
// Windows compares them. Empty when the target is free to use. sign_manifest.py's _reserved_target_problem says the same.
std::string reservedTargetReason(const ManifestTarget& target) {
    const std::string path = foldAscii(target.path);
    const size_t slash = path.rfind('/');
    const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    const bool topLevel = slash == std::string::npos;
    if (target.root == TargetRoot::Game && startsWithFolded(path, "openvr/"))
        return "is under the openvr folder, which the openvr root names";
    if (startsWithFolded(path, "edvr_install/") || startsWithFolded(path, "edvr_backup/"))
        return "is inside a folder the installer keeps its record and backups in";
    if (topLevel && (path == "edvr_profile.ini" || path == "edvr_openxr.ini" || path == "openvr_api_orig.dll"))
        return "is a file the installer writes itself";
    if (leaf.size() >= 5 && leaf.compare(leaf.size() - 5, 5, ".base") == 0)
        return "is a .base copy the installer keeps";
    if (topLevel && path.size() >= 10 && startsWithFolded(path, "d3d11_") && path.compare(path.size() - 4, 4, ".dll") == 0)
        return "is a name a chained graphics DLL is parked under";
    return std::string();
}

// The cross-file rules: behaviour constraints and overlapping targets.
bool checkFiles(const std::vector<ManifestFile>& files, std::string* error) {
    const ManifestFile* chain = nullptr;
    const ManifestFile* runtime = nullptr;
    const ManifestFile* settings[2] = {nullptr, nullptr};  // vr, flat
    for (const ManifestFile& f : files) {
        switch (f.existing) {
            case ExistingMode::Chain:
                if (chain) return failWith(error, "file " + f.id + " is a second chain file (after " + chain->id + ")");
                if (f.target.root != TargetRoot::Game || foldAscii(f.target.path) != "d3d11.dll")
                    return failWith(error, "file " + f.id + " is a chain file and must target game d3d11.dll");
                chain = &f;
                break;
            case ExistingMode::Runtime:
                if (runtime) return failWith(error, "file " + f.id + " is a second runtime file (after " + runtime->id + ")");
                if (f.target.root != TargetRoot::Openvr || foldAscii(f.target.path) != "openvr_api.dll")
                    return failWith(error, "file " + f.id + " is a runtime file and must target openvr openvr_api.dll");
                runtime = &f;
                break;
            case ExistingMode::Settings: {
                if (f.when.profiles != kProfileVr && f.when.profiles != kProfileFlat)
                    return failWith(error, "file " + f.id + " is a settings file and needs when.profile exactly [vr] or [flat]");
                const ManifestFile*& slot = settings[f.when.profiles == kProfileVr ? 0 : 1];
                if (slot) return failWith(error, "file " + f.id + " is a second settings file for its profile (after " + slot->id + ")");
                slot = &f;
                break;
            }
            default:
                break;
        }
    }
    for (const ManifestFile& f : files) {
        const std::string reason = reservedTargetReason(f.target);
        if (!reason.empty()) return failWith(error, "file " + f.id + " target " + reason);
    }
    for (size_t i = 0; i < files.size(); ++i) {
        for (size_t j = i + 1; j < files.size(); ++j) {
            const ManifestFile& a = files[i];
            const ManifestFile& b = files[j];
            if (a.target.root != b.target.root || foldAscii(a.target.path) != foldAscii(b.target.path)) continue;
            if (!whensCanOverlap(a.when, b.when)) continue;
            return failWith(error, "files " + a.id + " and " + b.id + " have the same target");
        }
    }
    return true;
}

}  // namespace

bool isTargetPath(const std::string& path, std::string* why) {
    auto refuse = [&](const char* text) {
        if (why) *why = text;
        return false;
    };
    if (path.empty()) return refuse("it is empty");
    for (char c : path) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\\' || c == ':') return refuse("it has a backslash or a colon");
        if (u < 0x20 || u > 0x7e) return refuse("it has a control or non-ASCII character");
        if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
            return refuse("it has a character Windows bars from a name");
    }
    size_t start = 0;
    for (;;) {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") return refuse("it has an empty, . or .. segment");
        if (part.back() == '.' || part.back() == ' ') return refuse("a segment ends in a dot or a space");
        if (isReservedDeviceName(part)) return refuse("a segment is a reserved device name");
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool whenHolds(const ManifestWhen& when, unsigned installedProfiles, bool nvidiaAdapter) {
    if (when.profiles != 0 && (when.profiles & installedProfiles) == 0) return false;
    if (when.adapterNvidia && !nvidiaAdapter) return false;
    return true;
}

bool whensCanOverlap(const ManifestWhen& a, const ManifestWhen& b) {
    if (a.profiles == 0 || b.profiles == 0) return true;
    return (a.profiles & b.profiles) != 0;
}

const char* targetRootName(TargetRoot root) { return root == TargetRoot::Game ? "game" : "openvr"; }

const char* existingName(ExistingMode mode) {
    switch (mode) {
        case ExistingMode::Replace: return "replace";
        case ExistingMode::Owned: return "owned";
        case ExistingMode::Chain: return "chain";
        case ExistingMode::Runtime: return "runtime";
        default: return "settings";
    }
}

ManifestParse parseManifest(const JsonValue& root, Manifest* out, std::string* error, long long* schemaSeen) {
    auto malformed = [&](const std::string& text) {
        if (error) *error = text;
        return ManifestParse::Malformed;
    };
    if (!root.isObject()) return malformed("manifest must be an object");
    // The schema is read alone, before any shape check: a manifest from a newer
    // schema is not malformed, this launcher just cannot read it.
    const Json* schema = root.find("schema");
    if (!schema || !schema->isInt()) return malformed("schema must be a whole number");
    if (schemaSeen) *schemaSeen = schema->integer;
    if (schema->integer > kManifestSchema) {
        if (error) *error = "manifest schema " + std::to_string(schema->integer) + ", launcher reads " +
                            std::to_string(kManifestSchema);
        return ManifestParse::SchemaTooNew;
    }
    if (schema->integer != kManifestSchema)
        return malformed("schema " + std::to_string(schema->integer) + " is no longer read; this launcher reads " +
                         std::to_string(kManifestSchema));

    std::string why;
    auto fail = [&](bool ok) { return ok ? ManifestParse::Ok : malformed(why); };
    if (!shapeOk(root, {"schema", "version", "build", "tag", "issued", "assets", "files"}, {"launcher"}, &why, "manifest"))
        return fail(false);
    for (const char* key : {"version", "build", "tag"}) {
        if (!isText(root.find(key))) return malformed(std::string(key) + " must be a non-empty string");
    }
    const Json* issued = root.find("issued");
    if (!issued->isInt() || issued->integer <= 0) return malformed("issued must be a positive integer");

    Manifest m;
    m.version = root.find("version")->text;
    m.build = root.find("build")->text;
    m.tag = root.find("tag")->text;
    m.issued = issued->integer;

    if (const Json* launcher = root.find("launcher")) {
        if (!shapeOk(*launcher, {"version", "asset", "sha256", "size"}, {}, &why, "launcher")) return fail(false);
        if (!isText(launcher->find("version"))) return malformed("launcher.version must be a non-empty string");
        if (!isAssetName(launcher->find("asset"))) return malformed("launcher.asset is not a plain file name");
        if (!isHash(launcher->find("sha256"))) return malformed("launcher.sha256 must be 64 lower-case hex characters");
        if (!isCount(launcher->find("size"))) return malformed("launcher.size must be a non-negative integer");
        m.hasLauncher = true;
        m.launcher.version = launcher->find("version")->text;
        m.launcher.asset = launcher->find("asset")->text;
        m.launcher.sha256 = launcher->find("sha256")->text;
        m.launcher.size = static_cast<unsigned long long>(launcher->find("size")->integer);
    }

    const Json* assets = root.find("assets");
    if (!assets->isArray() || assets->items.empty()) return malformed("assets must be a non-empty list");
    std::set<std::string> assetNames;
    for (const Json& asset : assets->items) {
        if (!shapeOk(asset, {"name", "sha256", "size"}, {}, &why, "an asset")) return fail(false);
        if (!isAssetName(asset.find("name"))) return malformed("an asset name is not a plain file name");
        if (!assetNames.insert(asset.find("name")->text).second) return malformed("an asset is listed twice");
        if (!isHash(asset.find("sha256"))) return malformed("an asset sha256 must be 64 lower-case hex characters");
        if (!isCount(asset.find("size"))) return malformed("an asset size must be a non-negative integer");
        ManifestAsset a;
        a.name = asset.find("name")->text;
        a.sha256 = asset.find("sha256")->text;
        a.size = static_cast<unsigned long long>(asset.find("size")->integer);
        m.assets.push_back(std::move(a));
    }

    const Json* files = root.find("files");
    if (!files->isArray() || files->items.empty()) return malformed("files must be a non-empty list");
    std::set<std::string> ids;
    for (size_t i = 0; i < files->items.size(); ++i) {
        ManifestFile f;
        if (!parseFile(files->items[i], i, assetNames, &f, &why)) return fail(false);
        if (!ids.insert(f.id).second) return malformed("file " + f.id + " is listed twice");
        m.files.push_back(std::move(f));
    }
    if (!checkFiles(m.files, &why)) return fail(false);
    *out = std::move(m);
    return ManifestParse::Ok;
}

}  // namespace edvr::installer
