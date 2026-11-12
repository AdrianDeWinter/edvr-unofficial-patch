// launcher_test -- the launcher's update core, over fixtures nobody can arrange by hand.
//
// What it proves, and how it would fail if the code under test never ran: every
// check below reads a value the code produced (bytes extracted from a zip, a
// verdict on a signed manifest, a file in a cache folder, a header the server
// received), so a stubbed or skipped function turns checks red, and the loops
// over fixture rows assert how many rows they saw.
//
//   json         the strict parser: depth, trailing bytes, duplicate keys, escapes
//   signing      Python-signed manifests accepted by BCrypt, every tampering refused
//   shapes       every shape tools\sign_manifest.py rejects is rejected here too
//   versions     compareVersions
//   zip          inflate over stored, fixed and dynamic blocks; corrupt and bomb input
//   cache        only missing assets fetched, tampering refused before the zip opens
//   state        launcher.ini
//   updater      GitHub scripted: 304, 403, 429, 5xx, bad signature, tag, issued, ...
//   http         WinHttpClient against a loopback server thread in this process
//   payload      usePayloadFiles
//   core         roots, --payload folders, the stored manifest, the developer-install check
//   register     the Start menu shortcut and the HKCU entry, and taking them away
//   selfupdate   downloading the launcher, the two renames and their rollback, on a running exe
//   planfile     the plan and result files, and the elevated copy's work (this rig is the copy)
//   route        the store addresses and EDLaunch's flags, recorded and never opened
//   watcher      the game watcher against fake programs: EDLaunch asked to close, left alone
//   session      the window's behaviour without a window: payload, check, update, self-update, Play
//   machine id   the machine ID Frontier is told, from fixed strings and a registry subkey this rig makes
//   layout dpi   the scale the window is laid out at when the real one would not fit the screen
//   direct       direct launch: Frontier scripted by hand, Steam a fake library the real loader loads,
//                every row of the fallback table, the order of the ticket, the sign-in and the start
//   dry run      --direct-dry-run's masked output
//   frontier login file       the stored Frontier-store login: DPAPI round trip, damage, entropy
//   frontier store: session   the three-call sign-in, a later launch, a denied login, a refused code, every
//                             cancel, every fallback to Frontier's own route, unticking
//   frontier dialogs          the two real windows, hidden and driven by posted keys
//   log bundle and the login  a bundle never holds the stored login or a launcher log
//   frontier store: dry run   the stored login only, never asked, never changed
//   args         the launcher's command line
//
// Nothing here touches the real %LOCALAPPDATA%\EDVR, a game folder or any host but
// 127.0.0.1: the updater sections use a scripted client, and the http section
// speaks only to a listener this process opens on the loopback address.
//
// Usage: launcher_test.exe <repo root> <fixtures dir> <scratch dir> <fakes dir>
//   fixtures are written by tools\launcher_fixtures.py; the fakes are build.bat's
//   (tools\launcher_test\fake_proc.cpp, built as ed_up_fake_game.exe and ed_up_fake_edlaunch.exe).
//   launcher_test.exe --apply-plan <file> --key <hex> is the elevated copy's work, run unelevated.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../src/installer/app.h"
#include "../../src/installer/detect.h"
#include "../../src/installer/direct_launch.h"
#include "../../src/installer/frontier_api.h"
#include "../../src/installer/frontier_login.h"
#include "../../src/installer/launch_route.h"
#include "../../src/installer/launcher_core.h"
#include "../../src/installer/launcher_login_dialog.h"
#include "../../src/installer/launcher_main.h"
#include "../../src/installer/launcher_plan.h"
#include "../../src/installer/launcher_register.h"
#include "../../src/installer/launcher_session.h"
#include "../../src/installer/launcher_state.h"
#include "../../src/installer/layout_dpi.h"
#include "../../src/installer/logbundle.h"
#include "../../src/installer/machine_id.h"
#include "../../src/installer/mirror.h"
#include "../../src/installer/selfupdate.h"
#include "../../src/installer/payload.h"
#include "../../src/installer/probe.h"
#include "../../src/installer/steam_ticket.h"
#include "../../src/installer/ui.h"
#include "../../src/installer/update_cache.h"
#include "../../src/installer/update_http.h"
#include "../../src/installer/update_json.h"
#include "../../src/installer/update_keys.h"
#include "../../src/installer/update_manifest.h"
#include "../../src/installer/update_util.h"
#include "../../src/installer/update_zip.h"
#include "../../src/installer/updater.h"

using namespace edvr::installer;

namespace {

int g_checks = 0;
int g_fails = 0;
std::wstring g_fixtures;
std::wstring g_scratch;
std::string g_testKey, g_otherKey;
const char* g_testKeys[2] = {nullptr, nullptr};

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

void section(const char* name) { std::printf("[launcher_test] %s\n", name); }

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// ---- files ---------------------------------------------------------------------

std::wstring P(const std::wstring& a, const std::wstring& b) { return joinPath(a, b); }
std::wstring P(const std::wstring& a, const std::string& b) { return joinPath(a, widenUtf8(b)); }

std::string readAll(const std::wstring& path) {
    std::string bytes;
    readFileBytes(path, &bytes, 1ull << 31, nullptr);
    return bytes;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool writeAll(const std::wstring& path, const std::string& data) {
    createDirectories(path.substr(0, path.find_last_of(L"\\/")));
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    while (done < data.size()) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 20));
        if (!WriteFile(f, data.data() + done, want, &wrote, nullptr) || wrote == 0) break;
        done += wrote;
    }
    CloseHandle(f);
    return done == data.size();
}

void rmtree(const std::wstring& dir) {
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(P(dir, L"*").c_str(), &find);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = find.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = P(dir, name);
            if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmtree(full);
            else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &find));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

std::vector<std::string> names(const std::wstring& dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(P(dir, L"*").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(narrowUtf8(find.cFileName));
    } while (FindNextFileW(h, &find));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::wstring fresh(const std::string& name) {
    const std::wstring dir = P(g_scratch, name);
    rmtree(dir);
    createDirectories(dir);
    return dir;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
        start = end + 1;
    }
    return out;
}

std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t end = line.find(sep, start);
        out.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

std::string sha(const std::string& bytes) { return sha256Bytes(bytes.data(), bytes.size()); }

const unsigned char* U(const std::string& s) { return reinterpret_cast<const unsigned char*>(s.data()); }

unsigned long u32At(const std::string& s, size_t pos) {
    return static_cast<unsigned long>(static_cast<unsigned char>(s[pos])) |
           (static_cast<unsigned long>(static_cast<unsigned char>(s[pos + 1])) << 8) |
           (static_cast<unsigned long>(static_cast<unsigned char>(s[pos + 2])) << 16) |
           (static_cast<unsigned long>(static_cast<unsigned char>(s[pos + 3])) << 24);
}

// ---- json ----------------------------------------------------------------------

void testJson() {
    section("json");
    JsonValue v;
    std::string err;
    check(parseJson("{\"a\": [1, -0, 2.5, true, null, \"x\"], \"b\": {\"c\": 1e2}}", &v, &err), "a plain document parses: " + err);
    check(v.isObject() && v.find("a") && v.find("a")->items.size() == 6, "the array has six elements");
    check(v.find("a")->items[0].isInt() && v.find("a")->items[0].integer == 1, "1 is an integer");
    check(v.find("a")->items[1].isInt() && v.find("a")->items[1].integer == 0, "-0 is the integer 0");
    check(v.find("a")->items[2].type == JsonValue::Type::Float, "2.5 is a float, not an integer");
    check(v.find("b")->find("c")->type == JsonValue::Type::Float, "1e2 is a float, not an integer");
    check(v.find("zz") == nullptr, "a missing key is null");

    check(parseJson("\"caf\\u00e9 \\ud83d\\ude00 \\\" \\\\ \\/\"", &v, &err), "escapes parse");
    check(v.text == "caf\xC3\xA9 \xF0\x9F\x98\x80 \" \\ /", "\\u escapes decode to UTF-8, pairs join");
    check(parseJson("\"\\ud800\"", &v, &err) && v.text == "\xED\xA0\x80", "a lone high surrogate is kept as its own three bytes, as Python does");
    check(parseJson("\"\\udc00x\"", &v, &err) && v.text == "\xED\xB0\x80x", "a lone low surrogate is kept as its own three bytes");
    check(parseJson("\"\\ud800\\u0041\"", &v, &err) && v.text == "\xED\xA0\x80" "A", "a high surrogate then a plain escape stay separate");
    check(!parseJson("\"\\ud800\\u00zz\"", &v, &err), "a bad escape after a high surrogate is still refused");
    check(!parseJson("\"\\x41\"", &v, &err), "an unknown escape is refused");
    check(!parseJson(std::string("\"a\tb\""), &v, &err), "a raw control character in a string is refused");
    check(!parseJson("\"\xC0\x80\"", &v, &err), "an overlong UTF-8 form is refused");
    check(!parseJson("\"\xED\xA0\x80\"", &v, &err), "an encoded surrogate is refused");
    check(!parseJson("\"\xC3\"", &v, &err), "a cut UTF-8 sequence is refused");
    check(parseJson("\"\xC3\xA9\"", &v, &err) && v.text == "\xC3\xA9", "valid UTF-8 passes through");

    check(!parseJson("{\"a\": 1, \"a\": 2}", &v, &err) && contains(err, "repeated"), "a repeated key is refused");
    check(!parseJson("{\"a\": 1, \"\\u0061\": 2}", &v, &err), "a repeated key written with an escape is refused");
    check(parseJson("[{\"a\":1},{\"a\":2}]", &v, &err), "the same key in two objects is fine");
    check(!parseJson("{} x", &v, &err) && contains(err, "follow"), "bytes after the value are refused");
    check(!parseJson("[] []", &v, &err), "a second value is refused");
    check(parseJson(" \t\r\n[ ] \n", &v, &err), "white space around the value is fine");
    check(!parseJson("", &v, &err), "empty input is refused");
    check(!parseJson("[1,]", &v, &err), "a trailing comma is refused");
    check(!parseJson("01", &v, &err), "a leading zero is refused");
    check(!parseJson("NaN", &v, &err), "NaN is refused");
    check(!parseJson("1.", &v, &err), "a number with no fraction digits is refused");
    check(!parseJson("-", &v, &err), "a bare minus is refused");
    check(!parseJson("9223372036854775808", &v, &err), "an integer past 64 bits is refused");
    check(parseJson("9223372036854775807", &v, &err) && v.integer == 9223372036854775807ll, "the largest integer is kept exactly");
    check(!parseJson("{'a': 1}", &v, &err), "single quotes are refused");
    check(!parseJson("[tru]", &v, &err), "a misspelt literal is refused");

    std::string deep32(32, '[');
    deep32 += std::string(32, ']');
    check(parseJson(deep32, &v, &err), "nesting of 32 is accepted");
    std::string deep33(33, '[');
    deep33 += std::string(33, ']');
    check(!parseJson(deep33, &v, &err) && contains(err, "deep"), "nesting of 33 is refused");
    std::string mixed;
    for (int i = 0; i < 33; ++i) mixed += (i % 2 ? "[" : "{\"k\":");
    mixed += "0";
    for (int i = 32; i >= 0; --i) mixed += (i % 2 ? "]" : "}");
    check(!parseJson(mixed, &v, &err), "33 levels of mixed arrays and objects are refused");
    check(v.type == JsonValue::Type::Null && v.items.empty(), "a refused parse leaves the output empty");
}

// ---- signing and keys ------------------------------------------------------------

bool loadWith(const char* const* keys, const std::string& bytes, const std::string& sig, Manifest* m, std::string* err) {
    return loadVerifiedManifest(bytes, sig, keys, m, err);
}

void testSigning() {
    section("signing");
    check(g_testKey.size() == 128 && g_otherKey.size() == 128, "the fixture keys are 128 hex characters");

    // The embedded keys: two, well formed, and never a test key.
    size_t embedded = 0;
    for (const char* const* k = kUpdatePublicKeys; *k; ++k) {
        ++embedded;
        std::string key = *k;
        check(key.size() == 128, "an embedded key is 128 characters");
        std::string lowerKey = key, lowerTest = g_testKey, lowerOther = g_otherKey;
        for (char& c : lowerKey) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        check(lowerKey != lowerTest, "an embedded key is the RFC 6979 test key");
        check(lowerKey != lowerOther, "an embedded key is the rig's other key");
    }
    check(embedded == 2, "update_keys.h holds the main and the recovery key");

    const std::wstring dir = P(g_fixtures, L"signing");
    const auto rows = lines(readAll(P(g_fixtures, L"signing.txt")));
    check(rows.size() >= 20, "the signing fixture lists its cases");
    int accepted = 0, refused = 0;
    for (const std::string& row : rows) {
        const auto f = split(row, '\t');
        if (f.size() != 4) { check(false, "a signing row has four fields: " + row); continue; }
        Manifest m;
        std::string err;
        const bool ok = loadWith(g_testKeys, readAll(P(dir, f[1])), readAll(P(dir, f[2])), &m, &err);
        check(ok == (f[3] == "accept"), "signing case " + f[0] + " expected " + f[3] + (ok ? ", accepted" : ", refused: " + err));
        (ok ? accepted : refused)++;
    }
    check(accepted >= 3 && refused >= 15, "the signing cases include accepts and refusals");

    const std::string good = readAll(P(dir, L"good.json")), goodSig = readAll(P(dir, L"good.sig"));
    Manifest m;
    std::string err;
    check(loadWith(g_testKeys, good, goodSig, &m, &err), "the Python-signed manifest is accepted by BCrypt");
    check(m.version == "1.0.0" && m.tag == "v1.0.0" && m.build == "v1.0.0" && m.issued == 1794000000 &&
              m.minLauncher == "0.1.0" && !m.hasLauncher && m.assets.size() == 1 && m.files.size() == 3,
          "the accepted manifest's fields are read");
    check(m.files[0].item == "native_graphics" && m.files[0].asset == m.assets[0].name && m.files[0].sha256.size() == 64,
          "a file entry is read whole");

    // Either listed key may sign; a list with neither refuses; the real keys refuse a test-signed manifest.
    const char* second[3] = {g_otherKey.c_str(), g_testKey.c_str(), nullptr};
    check(loadWith(second, good, goodSig, &m, &err), "the second listed key may be the one that signed");
    const char* none[1] = {nullptr};
    check(!loadWith(none, good, goodSig, &m, &err), "an empty key list refuses everything");
    check(!loadWith(nullptr, good, goodSig, &m, &err), "a null key list refuses everything");
    check(!loadWith(kUpdatePublicKeys, good, goodSig, &m, &err), "the embedded keys refuse a test-signed manifest");
    const char* wrong[2] = {g_otherKey.c_str(), nullptr};
    check(!loadWith(wrong, good, goodSig, &m, &err), "a stranger's key does not verify the signature");
    const std::string notAKey(128, 'z'), shortKey(100, 'a');
    const char* junkThenGood[4] = {notAKey.c_str(), shortKey.c_str(), g_testKey.c_str(), nullptr};
    check(loadWith(junkThenGood, good, goodSig, &m, &err), "a key that is not a point is skipped, the next is tried");
    const char* offCurve[2] = {"0000000000000000000000000000000000000000000000000000000000000001"
                               "0000000000000000000000000000000000000000000000000000000000000001", nullptr};
    check(!loadWith(offCurve, good, goodSig, &m, &err), "a key that is not on the curve verifies nothing");

    // The signature is checked before anything is parsed: a bad signature over
    // text that is not JSON reports the signature, and a good one reports the JSON.
    check(!loadWith(g_testKeys, "not json {", goodSig, &m, &err) && contains(err, "signature") && !contains(err, "JSON"),
          "a bad signature is reported before the parser runs: " + err);
    const auto shapes = lines(readAll(P(g_fixtures, L"shapes.txt")));
    for (const std::string& row : shapes) {
        const auto f = split(row, '\t');
        if (f.size() == 2 && f[0].size() > 4 && f[0].substr(4) == "not-json") {
            const std::wstring base = P(P(g_fixtures, L"shapes"), f[0]);
            check(!loadWith(g_testKeys, readAll(base + L".json"), readAll(base + L".sig"), &m, &err) && contains(err, "JSON"),
                  "a good signature over non-JSON reaches the parser: " + err);
        }
    }
    check(loadWith(g_testKeys, good, goodSig, &m, &err) && m.files.size() == 3, "a refused load does not disturb a later one");
    Manifest untouched;
    untouched.version = "keep";
    check(!loadWith(g_testKeys, good + " ", goodSig, &untouched, &err) && untouched.version == "keep", "a refused load leaves the output alone");
}

// ---- schema shapes -----------------------------------------------------------------

void testShapes() {
    section("shapes");
    const auto rows = lines(readAll(P(g_fixtures, L"shapes.txt")));
    check(rows.size() > 120, "the shape fixture lists its cases");
    std::map<std::string, std::string> verdictByName;
    int accepted = 0, refused = 0, mismatched = 0;
    for (const std::string& row : rows) {
        const auto f = split(row, '\t');
        if (f.size() != 2) { check(false, "a shape row has two fields: " + row); continue; }
        const std::wstring base = P(P(g_fixtures, L"shapes"), f[0]);
        Manifest m;
        std::string err;
        const bool ok = loadVerifiedManifest(readAll(base + L".json"), readAll(base + L".sig"), g_testKeys, &m, &err);
        const bool pythonAccepts = f[1] == "accept";
        if (ok != pythonAccepts) ++mismatched;
        check(ok == pythonAccepts, "shape " + f[0] + ": sign_manifest.py says " + f[1] + ", the launcher " + (ok ? "accepts" : "refuses: " + err));
        (ok ? accepted : refused)++;
        verdictByName[f[0].substr(4)] = ok ? "accept" : "refuse";
    }
    check(mismatched == 0, "the C++ and Python validators agree on every shape");
    check(accepted >= 14 && refused >= 100, "the shapes include both accepted and refused manifests");
    for (const char* name : {"path-6", "path-14", "path-2", "path-9", "file-sha-not-hex", "asset-sha-upper", "asset-name-5", "asset-name-6",
                             "launcher-asset-dots", "bom", "non-ascii-raw", "duplicate-key", "item-unknown", "item-alias-d3d11"}) {
        check(verdictByName.count(name) && verdictByName[name] == "refuse", std::string("the launcher refuses ") + name);
    }
    for (const char* name : {"valid", "valid-launcher", "valid-escaped-version", "valid-escaped-path", "valid-minus-zero-size",
                             "path-nested-ok", "item-ini", "item-openxr_license"}) {
        check(verdictByName.count(name) && verdictByName[name] == "accept", std::string("the launcher accepts ") + name);
    }
    for (const char* item : kManifestItems) {
        check(verdictByName.count(std::string("item-") + item) && verdictByName[std::string("item-") + item] == "accept",
              std::string("a manifest naming item ") + item + " is accepted");
    }

    // A parsed shape keeps what the file said.
    const std::wstring base = P(P(g_fixtures, L"shapes"), L"001-valid-launcher");
    Manifest m;
    std::string err;
    check(loadVerifiedManifest(readAll(base + L".json"), readAll(base + L".sig"), g_testKeys, &m, &err) && m.hasLauncher &&
              m.launcher.asset == "ed-up-launcher.exe" && m.launcher.size == 1 && m.launcher.version == "0.1.0",
          "the launcher entry is read");
}

// ---- versions -------------------------------------------------------------------

void testVersions() {
    section("versions");
    struct Case { const char* a; const char* b; int sign; };
    const Case cases[] = {
        {"0.1.0", "0.1.0", 0}, {"0.1.0", "0.2.0", -1}, {"0.10.0", "0.9.0", 1}, {"1.0.0-rc.1", "1.0.0", -1},
        {"1.0.0", "1.0.0-rc.7", 1}, {"0.17.0-rc.6", "0.17.0-rc.7", -1}, {"0.17.0-rc.10", "0.17.0-rc.9", 1},
        {"v1.2.3", "1.2.3", 0}, {"1.2", "1.2.0", 0}, {"1.0.0-rc.7", "0.99.9", 1}, {"2.0.0", "1.99.99", 1},
        {"1.0.0-alpha", "1.0.0-1", 1}, {"1.0.0-rc", "1.0.0-rc.1", -1}, {"0.1.0", "0.1.1", -1}, {"10.0.0", "9.99.99", 1},
    };
    for (const Case& c : cases) {
        const int got = compareVersions(c.a, c.b);
        const int back = compareVersions(c.b, c.a);
        check((got < 0 ? -1 : got > 0 ? 1 : 0) == c.sign, std::string("compareVersions(") + c.a + ", " + c.b + ")");
        check((back < 0 ? -1 : back > 0 ? 1 : 0) == -c.sign, std::string("compareVersions(") + c.b + ", " + c.a + ") is the mirror");
    }
}

// ---- zip and inflate ----------------------------------------------------------------

struct InflateRow {
    std::string name;
    unsigned long long size;
    std::string sha;
    std::string kind;
};

void testZip() {
    section("zip");
    std::vector<InflateRow> rows;
    for (const std::string& line : lines(readAll(P(g_fixtures, L"inflate\\entries.txt")))) {
        const auto f = split(line, '\t');
        if (f.size() == 4) rows.push_back({f[0], std::strtoull(f[1].c_str(), nullptr, 10), f[2], f[3]});
    }
    check(rows.size() == 8, "the inflate fixture has eight entries");
    const std::wstring zipPath = P(g_fixtures, L"inflate\\inflate.zip");
    ZipReader zip;
    std::string err;
    check(zip.openFile(zipPath, &err), "the fixture zip opens from a file: " + err);
    check(zip.entryCount() == 8, "the directory lists eight entries");
    InflateStats stats;
    for (const InflateRow& row : rows) {
        std::vector<unsigned char> bytes;
        const bool ok = zip.extract(row.name, row.size, &bytes, &err, &stats);
        check(ok, "extract " + row.name + ": " + err);
        check(ok && bytes.size() == row.size && sha256Bytes(bytes.data(), bytes.size()) == row.sha,
              "the bytes of " + row.name + " match the hash Python recorded");
    }
    check(stats.storedBlocks >= 1, "inflate met a stored block");
    check(stats.fixedBlocks >= 1, "inflate met a fixed-Huffman block");
    check(stats.dynamicBlocks >= 3, "inflate met dynamic-Huffman blocks");
    check(zip.has("stored.bin") && !zip.has("STORED.BIN") && !zip.has("stored.bi") && !zip.has("../stored.bin"), "entries are found by exact name only");

    std::vector<unsigned char> out;
    check(!zip.extract("no-such-entry", 1, &out, &err), "an unknown entry fails");
    check(!zip.extract("stored.bin", 299, &out, &err) && !zip.extract("stored.bin", 301, &out, &err), "a size other than the recorded one fails");
    check(!zip.extract("dynamic.txt", 5, &out, &err), "a smaller expected size than the entry's fails");
    check(zip.extract("empty-stored.bin", 0, &out, &err) && out.empty(), "an empty stored entry extracts");
    check(zip.extract("empty-deflate.bin", 0, &out, &err) && out.empty(), "an empty deflated entry extracts");

    // The same archive from memory, then damaged in ways a hostile server could.
    const std::string original = readAll(zipPath);
    check(original.size() > 5000000, "the fixture zip was read whole");
    {
        ZipReader memory;
        std::vector<unsigned char> bytes;
        check(memory.openMemory(U(original), original.size(), &err) && memory.extract("fixed.txt", rows[3].size, &bytes, &err),
              "the zip opens and extracts from memory");
    }
    auto mutated = [&](const std::function<void(std::string&)>& change) {
        std::string copy = original;
        change(copy);
        return copy;
    };
    auto opens = [&](const std::string& bytes, std::string* why) {
        ZipReader r;
        return r.openMemory(U(bytes), bytes.size(), why);
    };
    const size_t eocd = original.size() - 22;
    check(u32At(original, eocd) == 0x06054b50ul, "the fixture has no zip comment");
    std::string why;
    check(!opens(original.substr(0, original.size() / 2), &why), "a truncated zip does not open");
    check(!opens(original.substr(0, 10), &why) && !opens(std::string(), &why), "a tiny file does not open");
    check(!opens(mutated([&](std::string& b) { b[eocd + 10] = '\xff'; b[eocd + 11] = '\xff'; b[eocd + 8] = '\xff'; b[eocd + 9] = '\xff'; }), &why) &&
              contains(why, "zip64"), "a zip64 marker is refused: " + why);
    check(!opens(mutated([&](std::string& b) { b[eocd + 4] = 1; }), &why) && contains(why, "disks"), "a multi-disk archive is refused: " + why);
    check(!opens(mutated([&](std::string& b) { b[eocd + 16] = '\xff'; b[eocd + 17] = '\xff'; b[eocd + 18] = '\xff'; b[eocd + 19] = '\xff'; }), &why),
          "a directory offset of 0xffffffff is refused");
    check(!opens(mutated([&](std::string& b) { b[eocd + 12] = '\xff'; b[eocd + 13] = '\xff'; b[eocd + 14] = '\x7f'; }), &why),
          "a directory size past the file is refused");
    check(!opens(mutated([&](std::string& b) { b[eocd + 20] = 5; }), &why), "a comment length that does not reach the end is refused");

    // The first entry is stored.bin: its local header is at 0 and its central header opens the directory.
    const size_t storedLocal = 0;
    const size_t header = u32At(original, eocd + 16);
    check(u32At(original, storedLocal) == 0x04034b50ul && original.compare(storedLocal + 30, 10, "stored.bin") == 0, "the stored entry's local header is first");
    check(u32At(original, header) == 0x02014b50ul && original.compare(header + 46, 10, "stored.bin") == 0, "its central header opens the directory");
    {
        std::string encrypted = mutated([&](std::string& b) { b[header + 8] = static_cast<char>(b[header + 8] | 1); });
        ZipReader r;
        std::vector<unsigned char> bytes;
        check(r.openMemory(U(encrypted), encrypted.size(), &why), "an archive with one encrypted entry still opens");
        check(!r.extract("stored.bin", 300, &bytes, &why) && contains(why, "encrypted"), "the encrypted entry is refused: " + why);
        check(r.extract("fixed.txt", rows[3].size, &bytes, &why), "its neighbours still extract");
    }
    {
        std::string odd = mutated([&](std::string& b) { b[header + 10] = 9; });
        ZipReader r;
        std::vector<unsigned char> bytes;
        check(r.openMemory(U(odd), odd.size(), &why) && !r.extract("stored.bin", 300, &bytes, &why) && contains(why, "method"),
              "an unsupported method is refused: " + why);
    }
    {
        std::string renamed = mutated([&](std::string& b) { b[storedLocal + 32] = 'X'; });
        ZipReader r;
        std::vector<unsigned char> bytes;
        check(r.openMemory(U(renamed), renamed.size(), &why) && !r.extract("stored.bin", 300, &bytes, &why) && contains(why, "names another"),
              "a local header that names another file is refused: " + why);
    }
    {
        std::string outside = mutated([&](std::string& b) { b[header + 23] = '\x7f'; });
        ZipReader r;
        std::vector<unsigned char> bytes;
        check(r.openMemory(U(outside), outside.size(), &why) && !r.extract("stored.bin", 300, &bytes, &why),
              "a compressed size past the file is refused");
    }
    {
        std::string flippedCrc = mutated([&](std::string& b) { b[header + 16] ^= 1; });
        ZipReader r;
        std::vector<unsigned char> bytes;
        check(r.openMemory(U(flippedCrc), flippedCrc.size(), &why) && !r.extract("stored.bin", 300, &bytes, &why) && contains(why, "checksum"),
              "a wrong CRC is refused: " + why);
    }

    // Corrupt and truncated deflate streams fail cleanly, and nothing is written
    // past the output buffer.
    const InflateRow* dynamicRow = nullptr;
    for (const InflateRow& r : rows) if (r.name == "dynamic.txt") dynamicRow = &r;
    const size_t nameAt = original.find("dynamic.txt");
    const unsigned long compressedSize = u32At(original, nameAt - 30 + 18);
    const size_t dataAt = nameAt + 11;
    check(dynamicRow && compressedSize > 1000 && dataAt + compressedSize < original.size(), "the dynamic entry's stream is located");
    {
        std::vector<unsigned char> buffer(dynamicRow->size + 64, 0xA5);
        std::string e;
        check(inflateRaw(U(original) + dataAt, compressedSize, buffer.data(), dynamicRow->size, nullptr, nullptr, &e), "the stream inflates directly: " + e);
        check(buffer[dynamicRow->size] == 0xA5 && buffer[dynamicRow->size + 63] == 0xA5, "inflate wrote nothing past the output size");
        std::fill(buffer.begin(), buffer.end(), static_cast<unsigned char>(0xA5));
        check(!inflateRaw(U(original) + dataAt, compressedSize / 2, buffer.data(), dynamicRow->size, nullptr, nullptr, &e), "half a stream fails");
        check(!inflateRaw(U(original) + dataAt, 0, buffer.data(), dynamicRow->size, nullptr, nullptr, &e), "no stream fails");
        check(!inflateRaw(U(original) + dataAt, compressedSize, buffer.data(), dynamicRow->size - 1, nullptr, nullptr, &e) && contains(e, "larger"),
              "an output buffer one byte short stops the stream: " + e);
        check(!inflateRaw(U(original) + dataAt, compressedSize, buffer.data(), dynamicRow->size + 1, nullptr, nullptr, &e) && contains(e, "shorter"),
              "an output buffer one byte long is not filled: " + e);
        check(buffer[dynamicRow->size + 1] == 0xA5, "the failed runs left the sentinel alone");
    }
    int failedFlips = 0, flips = 0, wrongSuccesses = 0;
    for (int i = 0; i < 60; ++i) {
        std::string copy = original;
        copy[dataAt + static_cast<size_t>(i) * (compressedSize / 60)] ^= static_cast<char>(1 << (i % 8));
        ZipReader r;
        std::vector<unsigned char> bytes;
        if (!r.openMemory(U(copy), copy.size(), &why)) continue;
        ++flips;
        if (r.extract("dynamic.txt", dynamicRow->size, &bytes, &why)) {
            if (sha256Bytes(bytes.data(), bytes.size()) != dynamicRow->sha) ++wrongSuccesses;
        } else {
            ++failedFlips;
        }
    }
    check(flips == 60 && failedFlips >= 45, "single-bit flips in the stream fail the extract, not crash it");
    check(wrongSuccesses == 0, "no flip yields success with different bytes");
    unsigned seed = 12345;
    int garbageSurvived = 0;
    for (int trial = 0; trial < 300; ++trial) {
        std::string noise(200 + trial, '\0');
        for (char& c : noise) { seed = seed * 1664525u + 1013904223u; c = static_cast<char>(seed >> 24); }
        std::vector<unsigned char> buffer(1064, 0xCC);
        std::string e;
        inflateRaw(U(noise), noise.size(), buffer.data(), 1000, nullptr, nullptr, &e);
        bool sentinel = true;
        for (size_t i = 1000; i < buffer.size(); ++i) sentinel = sentinel && buffer[i] == 0xCC;
        garbageSurvived += sentinel ? 1 : 0;
    }
    check(garbageSurvived == 300, "300 streams of noise never wrote past the output buffer");

    // The bomb: the archive records 1000 bytes and the stream inflates to 100 MB.
    {
        ZipReader bomb;
        std::vector<unsigned char> bytes;
        check(bomb.openFile(P(g_fixtures, L"inflate\\bomb.zip"), &err), "the bomb archive opens: " + err);
        const DWORD before = GetTickCount();
        check(!bomb.extract("bomb.bin", 1000, &bytes, &why) && contains(why, "larger"), "the bomb stops at the recorded size: " + why);
        check(bytes.empty(), "no bomb output is handed back");
        check(GetTickCount() - before < 20000, "the bomb is refused without inflating its 100 MB");
        check(!bomb.extract("bomb.bin", 100u << 20, &bytes, &why), "asking for the bomb's real size fails: the archive records 1000");
    }
    // A hand-made stream: a stored block that claims more than it holds.
    {
        const unsigned char bad[] = {0x01, 0x10, 0x00, 0xef, 0xff, 'a', 'b'};
        unsigned char out16[16];
        std::string e;
        check(!inflateRaw(bad, sizeof(bad), out16, 16, nullptr, nullptr, &e), "a stored block longer than its input fails");
        const unsigned char badLen[] = {0x01, 0x02, 0x00, 0x00, 0x00, 'a', 'b'};
        check(!inflateRaw(badLen, sizeof(badLen), out16, 2, nullptr, nullptr, &e) && contains(e, "length check"), "a stored block with a bad length check fails");
        const unsigned char reserved[] = {0x07};
        check(!inflateRaw(reserved, sizeof(reserved), out16, 1, nullptr, nullptr, &e) && contains(e, "unknown type"), "block type 3 fails");
        const unsigned char tooFar[] = {0x03, 0x02};  // fixed block: a match with nothing before it
        check(!inflateRaw(tooFar, sizeof(tooFar), out16, 16, nullptr, nullptr, &e), "a back-reference before the start fails");
    }
}

// ---- the cache ---------------------------------------------------------------------

struct Rel {
    std::wstring dir;
    Manifest manifest;
    std::string manifestBytes, sigText;
    std::string name;
};

Rel loadRel(const char* name) {
    Rel r;
    r.name = name;
    r.dir = P(P(g_fixtures, L"releases"), name);
    r.manifestBytes = readAll(P(r.dir, L"ed-up-manifest.json"));
    r.sigText = readAll(P(r.dir, L"ed-up-manifest.json.sig"));
    std::string err;
    if (!loadVerifiedManifest(r.manifestBytes, r.sigText, g_testKeys, &r.manifest, &err)) {
        check(false, std::string("release fixture ") + name + " loads: " + err);
    }
    return r;
}

using Tamper = std::function<void(const std::string& assetName, std::string* bytes)>;

AssetFetcher serveFrom(const Rel& rel, std::vector<std::string>* log, Tamper tamper = nullptr) {
    return [dir = rel.dir, log, tamper](const ManifestAsset& asset, const std::wstring& dest, std::string* error) {
        log->push_back(asset.name);
        std::string bytes;
        if (!readFileBytes(P(dir, asset.name), &bytes, 1ull << 30, nullptr)) {
            *error = "the fixture has no such asset";
            return false;
        }
        if (tamper) tamper(asset.name, &bytes);
        return writeAll(dest, bytes);
    };
}

bool onlyHashNames(const std::wstring& dir, size_t expected) {
    const auto found = names(dir);
    if (found.size() != expected) return false;
    for (const std::string& n : found) if (!isLowerHex(n, 64)) return false;
    return true;
}

void testCache() {
    section("cache");
    const Rel a = loadRel("relA"), b = loadRel("relB");
    std::string err;
    std::vector<std::string> log;

    // Fetch only what is missing.
    {
        const std::wstring root = P(fresh("cache1"), L"payload");
        PayloadCache cache(root);
        std::vector<std::string> fetched;
        check(cache.ensureFiles(a.manifest, serveFrom(a, &log), &err, &fetched), "a fresh cache fills: " + err);
        check(fetched.size() == 2 && log.size() == 2, "both assets were fetched into an empty cache");
        check(cache.hasAll(a.manifest), "every file of release A is cached and verified");
        check(onlyHashNames(root, 4), "the cache folder holds four files, named by hash, and no temporary");
        for (const ManifestFile& f : a.manifest.files) {
            check(cache.hasVerified(f.sha256, f.size) && readAll(cache.pathFor(f.sha256)).size() == f.size, "cached file " + f.path + " is whole");
        }
        log.clear();
        fetched.clear();
        check(cache.ensureFiles(a.manifest, serveFrom(a, &log), &err, &fetched) && log.empty() && fetched.empty(), "a second fill fetches nothing");
        check(cache.ensureFiles(b.manifest, serveFrom(b, &log), &err, &fetched), "release B fills: " + err);
        check(log.size() == 1 && log[0] == "ed-up-core-1.1.0.zip", "only B's changed asset was fetched; the shared DLSS zip was not");
        check(cache.hasAll(b.manifest) && cache.hasAll(a.manifest), "both releases are now complete");
        check(onlyHashNames(root, 7), "the shared DLSS file is stored once");

        // Offline reads.
        check(cache.hasVerified(a.manifest.files[0].sha256, a.manifest.files[0].size) && !cache.hasVerified(a.manifest.files[0].sha256, a.manifest.files[0].size + 1),
              "hasVerified checks the size");
        check(cache.pathFor("../x").empty() && cache.pathFor("").empty() && cache.pathFor(std::string(64, 'G')).empty() &&
                  cache.pathFor(std::string(64, 'A')).empty() && !cache.pathFor(std::string(64, 'a')).empty(),
              "only a lower-case 64-digit hash names a cache file");
        // A file changed on disk no longer verifies, and the next fill repairs it.
        const std::wstring victim = cache.pathFor(a.manifest.files[0].sha256);
        std::string damaged = readAll(victim);
        damaged[0] ^= 1;
        check(writeAll(victim, damaged) && !cache.hasVerified(a.manifest.files[0].sha256, a.manifest.files[0].size), "a damaged cached file is not trusted");
        log.clear();
        check(cache.ensureFiles(a.manifest, serveFrom(a, &log), &err) && log.size() == 1 && cache.hasAll(a.manifest), "a damaged file is re-fetched and repaired: " + err);

        // Prune keeps what is kept.
        writeAll(P(root, L"junk.tmp"), "x");
        writeAll(P(root, L"notes.txt"), "keep me");
        writeAll(P(root, std::string(64, 'c')), "an unreferenced cached file");
        cache.prune({b.manifest});
        check(!exists(P(root, L"junk.tmp")), "prune removes a leftover temporary");
        check(!exists(P(root, std::string(64, 'c'))), "prune removes an unreferenced hash-named file");
        check(exists(P(root, L"notes.txt")), "prune leaves a file that is not a hash alone");
        check(cache.hasAll(b.manifest) && !cache.hasAll(a.manifest), "prune keeps B's files and drops A's alone");
        int stillA = 0;
        for (const ManifestFile& f : a.manifest.files) stillA += exists(cache.pathFor(f.sha256)) ? 1 : 0;
        check(stillA == 1, "of A's four files only the one B shares (the DLSS file) remains");
        cache.prune({});
        check(names(root).size() == 1, "pruning with nothing kept empties the cache of cached files");
    }

    // A leftover temporary is removed at the start of a fill.
    {
        const std::wstring root = P(fresh("cache2"), L"payload");
        createDirectories(root);
        writeAll(P(root, L"asset-1.tmp"), "half a download");
        writeAll(P(root, L"file-abc.tmp"), "half a file");
        PayloadCache cache(root);
        log.clear();
        check(cache.ensureFiles(a.manifest, serveFrom(a, &log), &err), "a fill over leftovers works: " + err);
        check(onlyHashNames(root, 4), "the leftover temporaries are gone");
    }

    // A tampered asset is refused before its zip is opened.
    {
        const std::wstring root = P(fresh("cache3"), L"payload");
        PayloadCache cache(root);
        log.clear();
        // One byte inside the compressed data: the zip itself would still open.
        Tamper flip = [](const std::string& name, std::string* bytes) {
            if (name.find("core") != std::string::npos) (*bytes)[60] ^= 1;
        };
        std::vector<std::string> fetched;
        check(!cache.ensureFiles(a.manifest, serveFrom(a, &log, flip), &err, &fetched), "a tampered asset fails the fill");
        check(contains(err, "hash") && !contains(err, "checksum") && !contains(err, "invalid"), "it is refused on the asset's hash, before the zip is read: " + err);
        check(names(root).empty(), "nothing is left in the cache, not even a temporary");

        // Same for a wrong size, even with a matching start.
        log.clear();
        Tamper cut = [](const std::string& name, std::string* bytes) {
            if (name.find("dlss") != std::string::npos) bytes->resize(bytes->size() - 3);
        };
        check(!cache.ensureFiles(a.manifest, serveFrom(a, &log, cut), &err) && contains(err, "size"), "a truncated asset is refused on its size: " + err);
        // The core zip came first and was fine: its files are whole, and no partial file exists.
        const auto left = names(root);
        bool allWhole = true;
        for (const std::string& n : left) allWhole = allWhole && isLowerHex(n, 64) && sha256File(P(root, n)) == n;
        check(allWhole && left.size() == 3, "after a failure on the second asset, only whole verified files remain (the first asset's three)");
        // And the next honest fill completes it without refetching what is cached.
        log.clear();
        check(cache.ensureFiles(a.manifest, serveFrom(a, &log), &err) && log.size() == 1 && cache.hasAll(a.manifest), "a later honest fill fetches only the missing asset: " + err);
    }

    // A file inside a good zip that does not match the manifest.
    {
        const std::wstring root = P(fresh("cache4"), L"payload");
        PayloadCache cache(root);
        Manifest lie = a.manifest;
        lie.files[1].sha256 = std::string(64, 'e');
        log.clear();
        check(!cache.ensureFiles(lie, serveFrom(a, &log), &err) && contains(err, lie.files[1].path), "a member that does not match its listed hash fails: " + err);
        check(!exists(cache.pathFor(std::string(64, 'e'))), "nothing is stored under the hash the manifest wrongly promised");
        bool noTemp = true;
        for (const std::string& n : names(root)) noTemp = noTemp && n.size() >= 4 && n.compare(n.size() - 4, 4, ".tmp") != 0;
        check(noTemp, "no temporary file remains");
        Manifest wrongPath = a.manifest;
        wrongPath.files[0].path = "no/such/entry.dll";
        PayloadCache second(P(fresh("cache4b"), L"payload"));
        check(!second.ensureFiles(wrongPath, serveFrom(a, &log), &err) && contains(err, "no entry"), "a manifest path the zip lacks fails: " + err);
        Manifest wrongSize = a.manifest;
        wrongSize.files[0].size += 1;
        PayloadCache third(P(fresh("cache4c"), L"payload"));
        check(!third.ensureFiles(wrongSize, serveFrom(a, &log), &err) && contains(err, "size"), "a manifest size the zip disagrees with fails: " + err);
    }

    // A fetcher that fails leaves nothing behind.
    {
        const std::wstring root = P(fresh("cache5"), L"payload");
        PayloadCache cache(root);
        const AssetFetcher failing = [](const ManifestAsset&, const std::wstring& dest, std::string* error) {
            writeAll(dest, "half");
            *error = "the network went away";
            return false;
        };
        check(!cache.ensureFiles(a.manifest, failing, &err) && contains(err, "network"), "a failing fetch fails the fill with its reason: " + err);
        check(names(root).empty(), "a failed fetch leaves no file");
    }
}

// ---- launcher.ini ---------------------------------------------------------------------

// Stand-ins for describeDir and eliteExeIsLegacy: a folder is a game when its path says Game or Legacy, one
// that says Launcher describes into the Game folder beside it, and Legacy in the path makes it legacy.
bool fakeDescribe(const std::wstring& dir, const std::wstring& source, GameInstall* out) {
    const size_t launcher = dir.find(L"Launcher");
    if (launcher != std::wstring::npos) {
        out->dir = dir.substr(0, launcher) + L"Game";
    } else if (dir.find(L"Game") != std::wstring::npos || dir.find(L"Legacy") != std::wstring::npos) {
        out->dir = dir;
    } else {
        return false;
    }
    out->source = source;
    return true;
}
bool fakeIsLegacy(const std::wstring& exe) { return exe.find(L"Legacy") != std::wstring::npos; }

void testRememberedChoice() {
    section("remembered install");
    std::vector<GameInstall> installs(2);
    installs[0].dir = L"C:\\Steam\\Game";
    installs[0].source = L"Steam";
    installs[1].dir = L"D:\\Frontier\\Game";
    installs[1].source = L"Frontier launcher";
    GameInstall restored;
    check(rememberedInstallChoice(installs, L"", fakeDescribe, fakeIsLegacy, &restored) == kNoRememberedInstall, "nothing remembered: no choice");
    check(rememberedInstallChoice(installs, L"d:/frontier/game/", fakeDescribe, fakeIsLegacy, &restored) == 1,
          "a listed install is chosen, whatever the case or separators");
    restored = GameInstall();
    check(rememberedInstallChoice(installs, L"E:\\Browsed\\Game", fakeDescribe, fakeIsLegacy, &restored) == 2 && restored.dir == L"E:\\Browsed\\Game" &&
              restored.source == L"Folder you chose",
          "a browsed folder that still holds a game comes back as a new entry, labelled as Browse labels it");
    check(rememberedInstallChoice(installs, L"E:\\Browsed\\Legacy", fakeDescribe, fakeIsLegacy, &restored) == kNoRememberedInstall,
          "a browsed folder holding the legacy game does not come back");
    check(rememberedInstallChoice(installs, L"E:\\Gone\\Nothing", fakeDescribe, fakeIsLegacy, &restored) == kNoRememberedInstall,
          "nor one that holds no game any more");
    check(rememberedInstallChoice(installs, L"D:\\Frontier\\Launcher", fakeDescribe, fakeIsLegacy, &restored) == 1,
          "a folder that describes into a listed install is that install, not a second entry");
    restored = GameInstall();
    check(rememberedInstallChoice({}, L"E:\\Browsed\\Game", fakeDescribe, fakeIsLegacy, &restored) == 0 && restored.dir == L"E:\\Browsed\\Game",
          "with nothing found by the scan, a browsed game is still restored");
    check(std::string(onlyLegacyInstallsText()).find("Odyssey only") != std::string::npos,
          "the scan's legacy-only line says what the planner says: Odyssey only");
}

void testState() {
    section("state");
    const std::wstring dir = fresh("state");
    const std::wstring path = P(dir, L"launcher.ini");
    LauncherState s;
    std::string err;
    check(s.load(path, &err) && s.channel() == "stable" && s.currentManifest().empty() && s.highestIssued() == 0, "a missing file is an empty state");
    s.setChannel("rc");
    s.setCurrentManifest(std::string(64, 'a'));
    s.raiseHighestIssued(1794000100);
    s.raiseHighestIssued(1794000000);
    check(s.highestIssued() == 1794000100, "highest_issued is never lowered");
    s.setMode(L"C:\\Games\\Elite", "flat");
    check(s.save(&err), "saving works: " + err);
    LauncherState t;
    check(t.load(path, &err) && t.channel() == "rc" && t.currentManifest() == std::string(64, 'a') && t.highestIssued() == 1794000100,
          "the keys round-trip");
    check(t.mode(L"C:\\Games\\Elite") == "flat" && t.mode(L"c:/games/elite/") == "flat" && t.mode(L"C:\\GAMES\\ELITE\\\\") == "flat",
          "a mode is found by folder, whatever the case or separators");
    check(t.mode(L"C:\\Games\\Other").empty(), "another folder has no mode");
    check(LauncherState::modeKey(L"C:\\Games\\Elite") == "mode.3d6b05c4d8ea02f1", "the mode key is the first 16 hex of the SHA-256 of the lower-cased folder");

    // last_install: the folder Play was last pressed for, kept as a path and matched as the mode keys are.
    {
        const std::wstring lastPath = P(fresh("state-last"), L"launcher.ini");
        LauncherState a;
        check(a.load(lastPath, &err) && a.lastInstall().empty(), "no install is remembered at first");
        const std::wstring folder = L"D:\\Spiele\\\x00c9lite \x5b89\\Products\\elite-dangerous-odyssey-64";
        a.setLastInstall(folder);
        check(a.save(&err), "saving last_install works: " + err);
        LauncherState b;
        check(b.load(lastPath, &err) && b.lastInstall() == folder, "last_install round-trips, non-ASCII included");
        const std::vector<std::wstring> folders = {L"C:\\Steam\\Products\\elite-dangerous-odyssey-64",
                                                   L"c:/users/me/appdata/local/frontier_developments/products/elite-dangerous-odyssey-64/"};
        check(rememberedInstallIndex(folders, L"C:\\Users\\Me\\AppData\\Local\\Frontier_Developments\\Products\\elite-dangerous-odyssey-64") == 1,
              "the remembered install is found in the list whatever the case or separators");
        check(rememberedInstallIndex(folders, L"C:\\Steam\\Products") == folders.size(), "a folder not in the list is not found");
        check(rememberedInstallIndex(folders, L"") == folders.size(), "nothing remembered finds nothing");
        check(rememberedInstallIndex({}, L"C:\\Steam") == 0, "an empty list finds nothing");
        check(!LauncherState::sameInstall(L"", L""), "two empty paths are not the same install");
    }

    // direct_launch: off unless the file says exactly "on".
    check(!t.directLaunch(), "direct launch is off when the key is missing");
    t.setDirectLaunch(true);
    check(t.directLaunch() && t.get("direct_launch") == "on", "setDirectLaunch(true) writes on");
    check(t.save(&err), "and saves: " + err);
    LauncherState direct;
    check(direct.load(path, &err) && direct.directLaunch() && direct.channel() == "rc", "on survives a reload, beside the other keys");
    direct.setDirectLaunch(false);
    check(!direct.directLaunch() && direct.get("direct_launch") == "off", "setDirectLaunch(false) writes off");
    int notOn = 0;
    for (const char* value : {"", "off", "ON", "On", "yes", "true", "1", " on", "on "}) {
        direct.set("direct_launch", value);
        notOn += direct.directLaunch() ? 0 : 1;
    }
    check(notOn == 9, "anything but on is off (" + std::to_string(notOn) + " of 9 values)");

    // Unknown lines survive, in place.
    check(writeAll(path, "# kept comment\nfuture_key=7\nchannel=stable\nweird line\nhighest_issued=5\n"), "the rig writes a hand-made file");
    LauncherState u;
    check(u.load(path, &err), "it loads");
    u.set("channel", "rc");
    u.raiseHighestIssued(9);
    u.set("new_key", "1");
    check(u.save(&err), "it saves");
    check(readAll(path) == "# kept comment\nfuture_key=7\nchannel=rc\nweird line\nhighest_issued=9\nnew_key=1\n", "unknown lines are preserved in place and new keys are appended: " + readAll(path));
    check(!contains(readAll(path), "\r"), "the file is written with plain line feeds");
    bool noTemp = true;
    for (const std::string& n : names(dir)) noTemp = noTemp && n == "launcher.ini";
    check(noTemp, "the atomic write left no temporary beside the file");
    LauncherState v;
    v.load(path, &err);
    v.set("channel", "nonsense");
    check(v.channel() == "stable", "an unknown channel reads as stable");
    v.setCurrentManifest("not-a-hash");
    check(v.currentManifest().empty(), "a malformed manifest hash reads as none");
    v.set("highest_issued", "12x");
    check(v.highestIssued() == 0, "a malformed highest_issued reads as zero");
    v.set("channel", "rc\nevil=1");
    check(v.get("evil").empty(), "a value cannot start a new line");
    LauncherState w;
    check(!w.load(dir, &err), "a path that is a folder does not load as a state file");
}

// ---- the scripted GitHub -----------------------------------------------------------------

struct Reply {
    bool transportOk = true;
    int status = 200;
    std::string body;
    std::string etag;
    std::string error;
};

class ScriptedHttp : public HttpClient {
public:
    std::map<std::string, Reply> replies;
    std::vector<HttpRequest> requests;

    HttpResponse get(const HttpRequest& request) override {
        requests.push_back(request);
        Reply reply;
        const auto found = replies.find(request.url);
        if (found != replies.end()) reply = found->second;
        else reply.status = 404;
        HttpResponse out;
        out.transportOk = reply.transportOk;
        out.status = reply.status;
        out.error = reply.error;
        out.etag = reply.etag;
        if (!reply.transportOk) return out;
        if (!reply.etag.empty() && reply.status == 200) {
            for (const auto& h : request.headers) {
                if (h.first == "If-None-Match" && h.second == reply.etag) out.status = 304;
            }
        }
        if (out.status != 200) return out;
        if (request.maxBytes && reply.body.size() > request.maxBytes) {
            out.transportOk = false;
            out.error = "the reply is larger than allowed";
            return out;
        }
        if (!request.toFile.empty()) {
            if (!writeAll(request.toFile, reply.body)) {
                out.transportOk = false;
                out.error = "cannot write";
            }
            if (request.progress) request.progress(reply.body.size(), reply.body.size());
        } else {
            out.body = reply.body;
        }
        out.bytes = reply.body.size();
        return out;
    }

    int count(const std::string& part) const {
        int n = 0;
        for (const auto& r : requests) n += contains(r.url, part) ? 1 : 0;
        return n;
    }
    bool sentHeader(size_t index, const std::string& name, const std::string& value) const {
        for (const auto& h : requests[index].headers) if (h.first == name && h.second == value) return true;
        return false;
    }
    bool hasHeader(size_t index, const std::string& name) const {
        for (const auto& h : requests[index].headers) if (h.first == name) return true;
        return false;
    }
};

const char* kApi = "https://api.example.invalid/repos/p";
const char* kDownloads = "https://dl.example.invalid/";

std::string q(const std::string& s) { return "\"" + s + "\""; }

struct ApiOptions {
    bool includeSig = true;
    bool digests = true;
    std::string tag;       // empty: the release's own tag
    bool draft = false;
    std::string odd;       // a digest string to use instead of "sha256:<hex>"
    bool onlyManifestAssets = false;
};

std::string releaseTag(const Rel& r) { return "v" + r.manifest.version; }

std::string releaseJson(const Rel& r, const ApiOptions& o = ApiOptions()) {
    std::string assets;
    auto add = [&](const std::string& name, const std::string& bytes, bool digest) {
        if (!assets.empty()) assets += ",";
        assets += "{\"name\":" + q(name) + ",\"browser_download_url\":" + q(kDownloads + r.name + "/" + name) +
                  ",\"size\":" + std::to_string(bytes.size());
        if (o.digests && digest) assets += ",\"digest\":" + q(o.odd.empty() ? "sha256:" + sha(bytes) : o.odd);
        else if (o.digests) assets += ",\"digest\":null";
        assets += "}";
    };
    add("ed-up-manifest.json", r.manifestBytes, true);
    if (o.includeSig) add("ed-up-manifest.json.sig", r.sigText, true);
    if (!o.onlyManifestAssets) {
        for (const ManifestAsset& a : r.manifest.assets) add(a.name, readAll(P(r.dir, a.name)), false);
        if (r.manifest.hasLauncher) add("ed-up-launcher.exe", readAll(P(r.dir, L"ed-up-launcher.exe")), false);
    }
    return "{\"url\":\"https://api.example.invalid/x\",\"tag_name\":" + q(o.tag.empty() ? releaseTag(r) : o.tag) +
           ",\"draft\":" + (o.draft ? "true" : "false") + ",\"prerelease\":false,\"body\":\"notes \\u00e9\",\"assets\":[" + assets + "]}";
}

void serveRelease(ScriptedHttp* http, const Rel& r) {
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(P(r.dir, L"*").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Reply reply;
        reply.body = readAll(P(r.dir, find.cFileName));
        http->replies[kDownloads + r.name + "/" + narrowUtf8(find.cFileName)] = reply;
    } while (FindNextFileW(h, &find));
    FindClose(h);
}

UpdaterSettings settingsFor(const std::wstring& base) {
    UpdaterSettings s;
    s.launcherDir = P(base, L"launcher");
    s.cacheDir = P(base, L"payload");
    s.apiBase = kApi;
    s.keys = g_testKeys;
    s.launcherVersion = "0.1.0";
    s.launcherSha256 = std::string(64, '1');
    s.channel = "stable";
    return s;
}

std::string latestUrl() { return std::string(kApi) + "/releases/latest"; }
std::string listUrl() { return std::string(kApi) + "/releases?per_page=20"; }

Reply ok(const std::string& body, const std::string& etag = std::string()) {
    Reply r;
    r.body = body;
    r.etag = etag;
    return r;
}

Reply failing(int status) {
    Reply r;
    r.status = status;
    return r;
}

bool adopt(Updater* u, const UpdateCheck& c, std::string* err) {
    return u->downloadUpdate(c, nullptr, nullptr, err) && u->commitManifest(c, err);
}

void testUpdater() {
    section("updater");
    const Rel a = loadRel("relA"), b = loadRel("relB"), old = loadRel("relOld"), minRel = loadRel("relMin"), minLauncher = loadRel("relMinLauncher");
    std::string err;

    // A first check, a download, an adoption, and an up-to-date answer that costs one request.
    {
        const std::wstring base = fresh("up1");
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(a), "\"etag-A\"");
        serveRelease(&http, a);
        Updater up(http, settingsFor(base));
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Available, "release A is offered: " + c.message);
        check(c.signatureVerified && c.tag == "v1.0.0" && c.manifest.version == "1.0.0" && c.manifestSha256 == sha(a.manifestBytes), "the check carries the verified manifest");
        check(contains(c.message, "1.0.0") && !c.launcherNeedsUpdate, "the message names the version; A has no launcher entry");
        check(http.requests.size() == 3 && http.requests[0].url == latestUrl(), "the check made the API request and fetched the manifest and signature, nothing more");
        check(http.sentHeader(0, "Accept", "application/vnd.github+json") && http.sentHeader(0, "X-GitHub-Api-Version", "2022-11-28") && !http.hasHeader(0, "If-None-Match"),
              "the first request carries the GitHub headers and no If-None-Match");
        check(!exists(P(settingsFor(base).cacheDir, L"x")) && names(settingsFor(base).cacheDir).empty(), "checking downloaded no payload file");
        check(http.requests[1].maxBytes == (1u << 20) && http.requests[2].maxBytes == (1u << 20), "the manifest and signature are capped at 1 MB each");

        std::string progressNames;
        check(up.downloadUpdate(c, nullptr, [&](const std::string& name, unsigned long long got, unsigned long long) { progressNames += name + (got ? "+" : "0") + ";"; }, &err),
              "the download fills the cache: " + err);
        check(contains(progressNames, "ed-up-core-1.0.0.zip"), "progress names the asset being fetched");
        check(http.count("ed-up-core-1.0.0.zip") == 1 && http.count("ed-up-dlss") == 1, "each missing asset was fetched once");
        LauncherState before;
        before.load(P(settingsFor(base).launcherDir, L"launcher.ini"), &err);
        check(before.currentManifest().empty() && before.highestIssued() == 0, "downloading adopts nothing");
        check(up.commitManifest(c, &err), "the commit succeeds: " + err);
        LauncherState after;
        after.load(P(settingsFor(base).launcherDir, L"launcher.ini"), &err);
        check(after.currentManifest() == c.manifestSha256 && after.highestIssued() == 1794000000, "the commit records current_manifest and highest_issued");
        check(exists(P(P(settingsFor(base).launcherDir, L"manifests"), widenUtf8(c.manifestSha256) + L".json")) &&
                  exists(P(P(settingsFor(base).launcherDir, L"manifests"), widenUtf8(c.manifestSha256) + L".sig")),
              "the manifest and its signature are stored under the manifest's hash");

        const size_t before2 = http.requests.size();
        const UpdateCheck again = up.check();
        check(again.outcome == UpdateOutcome::UpToDate, "the same digest is up to date: " + again.message);
        check(http.requests.size() == before2 + 1, "up to date cost one API request and no download");
        check(http.sentHeader(before2, "If-None-Match", "\"etag-A\""), "the stored ETag was sent as If-None-Match");

        // A reply without a digest is downloaded and still recognised as the adopted one.
        ScriptedHttp bare;
        bare.replies[latestUrl()] = ok(releaseJson(a, [] { ApiOptions o; o.digests = false; return o; }()));
        serveRelease(&bare, a);
        Updater up2(bare, settingsFor(base));
        const UpdateCheck noDigest = up2.check();
        check(noDigest.outcome == UpdateOutcome::UpToDate && bare.count("ed-up-manifest.json") == 2, "with no digest listed the manifest is downloaded and found to be the adopted one: " + noDigest.message);
        ScriptedHttp odd;
        odd.replies[latestUrl()] = ok(releaseJson(a, [] { ApiOptions o; o.odd = "md5:00ff"; return o; }()));
        serveRelease(&odd, a);
        Updater up3(odd, settingsFor(base));
        check(up3.check().outcome == UpdateOutcome::UpToDate && odd.count("ed-up-manifest.json") == 2, "an odd digest means download anyway");
    }
    // 304 with a pending update, and 304 with nothing stored.
    {
        const std::wstring base = fresh("up2");
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(a), "\"etag-A\"");
        serveRelease(&http, a);
        Updater up(http, settingsFor(base));
        check(up.check().outcome == UpdateOutcome::Available, "first check offers A");
        const size_t n = http.requests.size();
        const UpdateCheck second = up.check();
        check(http.sentHeader(n, "If-None-Match", "\"etag-A\""), "the second check sent the stored ETag");
        check(second.outcome == UpdateOutcome::Available && second.tag == "v1.0.0", "a 304 reuses the stored reply, so an update seen but not installed is still offered: " + second.message);
        check(exists(P(settingsFor(base).launcherDir, L"api-stable.cache")), "the reply is stored in the launcher folder");

        ScriptedHttp lone;
        lone.replies[latestUrl()] = failing(304);
        Updater up2(lone, settingsFor(fresh("up2b")));
        const UpdateCheck c = up2.check();
        check(c.outcome == UpdateOutcome::Skipped, "a 304 with no stored reply is skipped: " + c.message);
    }
    // Statuses and transport failures never block.
    for (int status : {403, 429, 500, 502, 503, 404, 418}) {
        ScriptedHttp http;
        http.replies[latestUrl()] = failing(status);
        const std::wstring base = fresh("up3");
        Updater up(http, settingsFor(base));
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Skipped && contains(c.message, std::to_string(status)), "status " + std::to_string(status) + " is skipped: " + c.message);
        check(!exists(P(settingsFor(base).launcherDir, L"launcher.ini")) && http.requests.size() == 1, "a skipped check writes no state and asks nothing more");
    }
    {
        ScriptedHttp http;
        Reply r;
        r.transportOk = false;
        r.error = "no route";
        http.replies[latestUrl()] = r;
        Updater up(http, settingsFor(fresh("up4")));
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Skipped && contains(c.message, "no route"), "a transport failure is skipped: " + c.message);
        ScriptedHttp junk;
        junk.replies[latestUrl()] = ok("<html>not json</html>");
        Updater up2(junk, settingsFor(fresh("up4b")));
        check(up2.check().outcome == UpdateOutcome::Skipped, "a reply that is not JSON is skipped");
        ScriptedHttp wrongShape;
        wrongShape.replies[latestUrl()] = ok("[]");
        Updater up3(wrongShape, settingsFor(fresh("up4c")));
        check(up3.check().outcome == UpdateOutcome::Skipped, "a list where a release was expected is skipped");
    }
    // A release without the signed manifest is not offered; a broken one is refused.
    {
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(a, [] { ApiOptions o; o.includeSig = false; return o; }()));
        serveRelease(&http, a);
        Updater up(http, settingsFor(fresh("up5")));
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::UpToDate && http.count("ed-up-manifest") == 0, "a release with no .sig is not offered and nothing is downloaded: " + c.message);
        ScriptedHttp bare;
        bare.replies[latestUrl()] = ok("{\"tag_name\":\"v0.0.1\",\"assets\":[]}");
        Updater up2(bare, settingsFor(fresh("up5b")));
        check(up2.check().outcome == UpdateOutcome::UpToDate, "stable with no manifest assets is up to date");

        ScriptedHttp gone;
        gone.replies[latestUrl()] = ok(releaseJson(a));
        serveRelease(&gone, a);
        gone.replies[std::string(kDownloads) + "relA/ed-up-manifest.json.sig"] = failing(404);
        Updater up3(gone, settingsFor(fresh("up5c")));
        check(up3.check().outcome == UpdateOutcome::Refused, "a .sig that is listed but cannot be fetched is refused");
        gone.replies[std::string(kDownloads) + "relA/ed-up-manifest.json.sig"] = failing(503);
        check(up3.check().outcome == UpdateOutcome::Skipped, "a .sig that fails with a server error is skipped");

        // The manifest of A with the signature of B, and no digest to tell them apart, so the
        // signature alone has to catch it.
        ScriptedHttp badSig;
        badSig.replies[latestUrl()] = ok(releaseJson(a, [] { ApiOptions o; o.digests = false; return o; }()));
        serveRelease(&badSig, a);
        badSig.replies[std::string(kDownloads) + "relA/ed-up-manifest.json.sig"] = ok(b.sigText);
        Updater up4(badSig, settingsFor(fresh("up5d")));
        const UpdateCheck c4 = up4.check();
        check(c4.outcome == UpdateOutcome::Refused, "a signature of another manifest is refused: " + c4.message);
        // A download that differs from GitHub's own listing is its cache serving an older copy after
        // a re-cut: seen on the first test release. Nothing is offered, and it is no verdict on the
        // release, so the next check looks again.
        ScriptedHttp staleSig;
        staleSig.replies[latestUrl()] = ok(releaseJson(a));
        serveRelease(&staleSig, a);
        staleSig.replies[std::string(kDownloads) + "relA/ed-up-manifest.json.sig"] = ok(b.sigText);
        Updater upStaleSig(staleSig, settingsFor(fresh("up5s")));
        const UpdateCheck cStaleSig = upStaleSig.check();
        check(cStaleSig.outcome == UpdateOutcome::Skipped && contains(cStaleSig.message, "older copy of the signature"), "a stale signature skips the check: " + cStaleSig.message);
        ScriptedHttp swapped;
        swapped.replies[latestUrl()] = ok(releaseJson(a));
        serveRelease(&swapped, a);
        swapped.replies[std::string(kDownloads) + "relA/ed-up-manifest.json"] = ok(b.manifestBytes);
        Updater up5(swapped, settingsFor(fresh("up5e")));
        const UpdateCheck c5 = up5.check();
        check(c5.outcome == UpdateOutcome::Skipped && contains(c5.message, "older copy of the manifest"), "a manifest that is not the one GitHub lists skips the check: " + c5.message);
        // Without a digest the signature alone refuses a flipped byte.
        std::string flipped = a.manifestBytes;
        flipped[flipped.find("1794000000") + 9] = '1';
        ScriptedHttp flippedHttp;
        flippedHttp.replies[latestUrl()] = ok(releaseJson(a, [] { ApiOptions o; o.digests = false; return o; }()));
        serveRelease(&flippedHttp, a);
        flippedHttp.replies[std::string(kDownloads) + "relA/ed-up-manifest.json"] = ok(flipped);
        Updater up6(flippedHttp, settingsFor(fresh("up5f")));
        const UpdateCheck c6 = up6.check();
        check(c6.outcome == UpdateOutcome::Refused && contains(c6.message, "signature"), "a flipped manifest byte is refused on the signature: " + c6.message);
        check(c6.manifest.version.empty() && !c6.signatureVerified, "a refused check carries no manifest");
        // A manifest signed by someone else.
        ScriptedHttp stranger;
        stranger.replies[latestUrl()] = ok(releaseJson(a));
        serveRelease(&stranger, a);
        UpdaterSettings other = settingsFor(fresh("up5g"));
        const char* otherKeys[2] = {g_otherKey.c_str(), nullptr};
        other.keys = otherKeys;
        Updater up7(stranger, other);
        check(up7.check().outcome == UpdateOutcome::Refused, "a manifest signed by a key the exe does not carry is refused");
        UpdaterSettings production = settingsFor(fresh("up5h"));
        production.keys = nullptr;
        Updater up8(stranger, production);
        check(up8.check().outcome == UpdateOutcome::Refused, "with the embedded keys a test-signed release is refused");
    }
    // The tag must match the release it was fetched from.
    {
        ScriptedHttp http;
        ApiOptions o;
        o.tag = "v9.9.9";
        http.replies[latestUrl()] = ok(releaseJson(a, o));
        serveRelease(&http, a);
        Updater up(http, settingsFor(fresh("up6")));
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Refused && contains(c.message, "tag"), "a manifest moved onto another release is refused: " + c.message);
    }
    // issued only goes up.
    {
        const std::wstring base = fresh("up7");
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(b));
        serveRelease(&http, b);
        serveRelease(&http, old);
        serveRelease(&http, a);
        Updater up(http, settingsFor(base));
        const UpdateCheck cb = up.check();
        check(cb.outcome == UpdateOutcome::Available && adopt(&up, cb, &err), "B is offered and adopted: " + err);
        http.replies[latestUrl()] = ok(releaseJson(old));
        const UpdateCheck co = up.check();
        check(co.outcome == UpdateOutcome::UpToDate && contains(co.message, "older"), "an older verified manifest is ignored with a note: " + co.message);
        http.replies[latestUrl()] = ok(releaseJson(a));
        check(up.check().outcome == UpdateOutcome::UpToDate, "release A, issued before B, is ignored too");
        LauncherState s;
        s.load(P(settingsFor(base).launcherDir, L"launcher.ini"), &err);
        check(s.highestIssued() == 1794000100, "highest_issued still reads B's");
    }
    // The rc channel walks the list.
    {
        ScriptedHttp http;
        ApiOptions draft;
        draft.draft = true;
        const std::string noManifest = "{\"tag_name\":\"v2.0.0-rc.1\",\"draft\":false,\"assets\":[{\"name\":\"ed-up-launcher.exe\",\"browser_download_url\":\"https://dl.example.invalid/x\",\"size\":1}]}";
        // newest first: a draft that holds both assets, a release with none, then B, then A.
        http.replies[listUrl()] = ok("[" + releaseJson(minRel, draft) + "," + noManifest + "," + releaseJson(b) + "," + releaseJson(a) + "]");
        serveRelease(&http, b);
        serveRelease(&http, a);
        UpdaterSettings s = settingsFor(fresh("up8"));
        s.channel = "rc";
        Updater up(http, s);
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Available && c.tag == "v1.1.0", "rc takes the first non-draft release holding both assets, walking past one without: " + c.message);
        check(http.requests[0].url == listUrl() && http.count("relMin") == 0, "rc asked for the release list and never touched the draft");
        check(c.launcherNeedsUpdate, "B carries a newer launcher");
        // Channel from launcher.ini when the settings leave it empty.
        const std::wstring base = fresh("up8b");
        createDirectories(settingsFor(base).launcherDir);
        writeAll(P(settingsFor(base).launcherDir, L"launcher.ini"), "channel=rc\n");
        UpdaterSettings fromState = settingsFor(base);
        fromState.channel.clear();
        ScriptedHttp http2;
        http2.replies[listUrl()] = ok("[" + noManifest + "]");
        Updater up2(http2, fromState);
        const UpdateCheck none = up2.check();
        check(none.outcome == UpdateOutcome::UpToDate && http2.requests[0].url == listUrl(), "with no release holding a manifest rc is up to date, and the channel came from launcher.ini: " + none.message);
        check(exists(P(fromState.launcherDir, L"api-rc.cache")) && !exists(P(fromState.launcherDir, L"api-stable.cache")), "each channel keeps its own reply cache");
    }
    // min_launcher and the launcher's digest.
    {
        ScriptedHttp http;
        serveRelease(&http, minRel);
        serveRelease(&http, minLauncher);
        serveRelease(&http, b);
        http.replies[latestUrl()] = ok(releaseJson(minRel));
        Updater up(http, settingsFor(fresh("up9")));
        const UpdateCheck refused = up.check();
        check(refused.outcome == UpdateOutcome::Refused && contains(refused.message, "newer launcher"), "min_launcher above ours with no launcher in the manifest is refused: " + refused.message);
        UpdaterSettings newer = settingsFor(fresh("up9b"));
        newer.launcherVersion = "0.3.0";
        Updater upNew(http, newer);
        check(upNew.check().outcome == UpdateOutcome::Available, "a launcher at min_launcher may take the release");
        http.replies[latestUrl()] = ok(releaseJson(minLauncher));
        const UpdateCheck viaNewLauncher = up.check();
        check(viaNewLauncher.outcome == UpdateOutcome::Available && viaNewLauncher.launcherNeedsUpdate, "min_launcher above ours with a newer launcher in the manifest is offered, launcher first");

        http.replies[latestUrl()] = ok(releaseJson(b));
        const std::string theirs = b.manifest.launcher.sha256;
        UpdaterSettings same = settingsFor(fresh("up9c"));
        same.launcherSha256 = theirs;
        Updater upSame(http, same);
        check(!upSame.check().launcherNeedsUpdate, "the same launcher digest needs no update");
        UpdaterSettings differ = settingsFor(fresh("up9d"));
        Updater upDiffer(http, differ);
        check(upDiffer.check().launcherNeedsUpdate, "a different digest with a higher version needs an update");
        UpdaterSettings higher = settingsFor(fresh("up9e"));
        higher.launcherVersion = "0.3.0";
        Updater upHigher(http, higher);
        const UpdateCheck lower = upHigher.check();
        check(lower.outcome == UpdateOutcome::Available && !lower.launcherNeedsUpdate, "a different digest with a lower version is not a launcher update");
        UpdaterSettings equal = settingsFor(fresh("up9f"));
        equal.launcherVersion = "0.2.0";
        Updater upEqual(http, equal);
        check(upEqual.check().launcherNeedsUpdate, "an equal version with a different digest is a launcher update");
    }
    // Commit needs the files; retention keeps three; offline load works; the payload follows.
    {
        const std::wstring base = fresh("up10");
        ScriptedHttp http;
        serveRelease(&http, a);
        http.replies[latestUrl()] = ok(releaseJson(a));
        UpdaterSettings s = settingsFor(base);
        s.launcherVersion = "0.5.0";
        Updater up(http, s);
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Available, "A is offered");
        check(!up.commitManifest(c, &err) && contains(err, "cache"), "committing before the files are cached is refused: " + err);
        LauncherState st;
        st.load(P(s.launcherDir, L"launcher.ini"), &err);
        check(st.currentManifest().empty() && !exists(P(s.launcherDir, L"manifests")), "the refused commit stored nothing");
        UpdateCheck notAvailable;
        check(!up.downloadUpdate(notAvailable, nullptr, nullptr, &err) && !up.commitManifest(notAvailable, &err), "nothing downloads or commits without an offered update");

        const Rel* sequence[] = {&a, &b, &minRel, &minLauncher};
        std::string lastSha;
        for (const Rel* rel : sequence) {
            http.replies[latestUrl()] = ok(releaseJson(*rel));
            serveRelease(&http, *rel);
            const UpdateCheck step = up.check();
            check(step.outcome == UpdateOutcome::Available, "release " + rel->name + " is offered in turn: " + step.message);
            check(adopt(&up, step, &err), "release " + rel->name + " is adopted: " + err);
            lastSha = step.manifestSha256;
            Sleep(30);
        }
        const std::wstring manifests = P(s.launcherDir, L"manifests");
        int json = 0, sig = 0;
        for (const std::string& n : names(manifests)) {
            json += n.size() == 69 && n.compare(64, 5, ".json") == 0;
            sig += n.size() == 68 && n.compare(64, 4, ".sig") == 0;
        }
        check(json == 3 && sig == 3, "only the newest three manifests are kept");
        check(!exists(P(manifests, widenUtf8(sha(a.manifestBytes)) + L".json")) && exists(P(manifests, widenUtf8(sha(b.manifestBytes)) + L".json")),
              "the oldest was dropped and the next newest kept");
        PayloadCache cache(s.cacheDir);
        check(!cache.hasAll(a.manifest) && cache.hasAll(b.manifest) && cache.hasAll(minRel.manifest) && cache.hasAll(minLauncher.manifest), "the cache holds exactly what the kept manifests list");
        check(!exists(cache.pathFor(a.manifest.files[0].sha256)), "a file only the dropped manifest used is gone");

        // Offline: a client that fails everything.
        ScriptedHttp offline;
        Updater off(offline, s);
        Manifest loaded;
        std::vector<PayloadFile> files;
        check(off.loadCurrentManifest(&loaded, &files, &err), "the current manifest loads offline: " + err);
        check(offline.requests.empty(), "loading it made no request");
        check(loaded.version == "1.3.0" && files.size() == 4, "it is the last adopted manifest, with four files");
        for (const PayloadFile& f : files) check(exists(f.path) && sha256File(f.path) == f.sha256, "payload file " + f.item + " is cached and verified");
        // Damage: a changed stored manifest, a changed signature, a missing cache file.
        const std::wstring storedJson = P(manifests, widenUtf8(lastSha) + L".json");
        const std::string original = readAll(storedJson);
        std::string changed = original;
        changed[changed.find("1.3.0")] = '9';
        writeAll(storedJson, changed);
        check(!off.loadCurrentManifest(&loaded, &files, &err) && contains(err, "changed"), "a stored manifest that no longer matches its hash is refused: " + err);
        writeAll(storedJson, original);
        const std::wstring storedSig = P(manifests, widenUtf8(lastSha) + L".sig");
        const std::string origSig = readAll(storedSig);
        writeAll(storedSig, std::string(128, '0') + "\n");
        check(!off.loadCurrentManifest(&loaded, &files, &err) && contains(err, "signature"), "a stored signature that no longer verifies is refused: " + err);
        writeAll(storedSig, origSig);
        check(off.loadCurrentManifest(&loaded, &files, &err), "restored, it loads again: " + err);
        DeleteFileW(cache.pathFor(minLauncher.manifest.files[0].sha256).c_str());
        check(!off.loadCurrentManifest(&loaded, &files, &err) && contains(err, "cache lacks"), "a missing cache file is named: " + err);
        std::vector<std::string> refetched;
        check(cache.ensureFiles(minLauncher.manifest, serveFrom(minLauncher, &refetched), &err) && refetched.size() == 1, "the cache repairs itself from the release: " + err);
        check(off.loadCurrentManifest(&loaded, &files, &err), "after the repair the current manifest loads again: " + err);
        // The hash is compared once, in usePayloadFiles: a same-size file with other bytes passes the
        // listing (size only) and is refused where the file is read.
        {
            const std::wstring victim = cache.pathFor(minLauncher.manifest.files[0].sha256);
            const std::string good = readAll(victim);
            std::string bad = good;
            bad[0] = static_cast<char>(bad[0] ^ 1);
            writeAll(victim, bad);
            check(off.loadCurrentManifest(&loaded, &files, &err), "a same-size tampered cache file passes the size listing: " + err);
            check(!usePayloadFiles(files, loaded.version, &err) && contains(err, "hash"), "usePayloadFiles refuses the tampered cache file: " + err);
            check(!cache.hasVerified(minLauncher.manifest.files[0].sha256, good.size()), "and the cache itself no longer calls it verified");
            writeAll(victim, good);
            check(off.loadCurrentManifest(&loaded, &files, &err), "the restored file lists again: " + err);
            // By path: the file name has to be the manifest's hash.
            Manifest byPath;
            std::vector<PayloadFile> byPathFiles;
            check(off.loadManifestFile(P(manifests, widenUtf8(lastSha) + L".json"), &byPath, &byPathFiles, &err) && byPath.version == "1.3.0" && byPathFiles.size() == 4,
                  "a stored manifest loads by its path: " + err);
            check(!off.loadManifestFile(P(manifests, L"renamed.json"), &byPath, &byPathFiles, &err) && contains(err, "not its hash"), "a manifest not named for its hash is refused: " + err);
        }
        check(usePayloadFiles(files, loaded.version, &err), "the payload switches to those files: " + err);
        const void* data = nullptr;
        size_t size = 0;
        check(payloadItem("native_graphics", &data, &size) && std::string(static_cast<const char*>(data), size) == "graphics four", "payloadItem now serves the cached graphics file");
        check(payloadInfo().version == "1.3.0" && payloadInfo().haveD3d11 && payloadInfo().haveNgx && payloadInfo().haveIni, "payloadInfo follows the manifest's version and files");
        resetPayloadFiles();

        const std::wstring emptyBase = fresh("up10b");
        Updater none(offline, settingsFor(emptyBase));
        check(!none.loadCurrentManifest(&loaded, &files, &err) && contains(err, "no update"), "with nothing adopted there is nothing to load: " + err);
    }
}

// ---- payload ---------------------------------------------------------------------------

void testPayload() {
    section("payload");
    const std::wstring dir = fresh("payload");
    const PayloadInfo embedded = payloadInfo();
    const void* data = nullptr;
    size_t size = 0;
    check(!payloadItem("ini", &data, &size) && !embedded.haveIni && !embedded.haveD3d11, "with no call made the rig's (resource-less) executable serves nothing, as before");
    const std::string baseVersion = embedded.version;
    const PayloadInfo* reference = &payloadInfo();

    auto make = [&](const wchar_t* leaf, const std::string& bytes) {
        const std::wstring path = P(dir, leaf);
        writeAll(path, bytes);
        return path;
    };
    const std::string graphics = "graphics bytes", ini = "[fix]\nx=1\n", ngx = "ngx bytes";
    std::string err;
    std::vector<PayloadFile> files = {{"d3d11", make(L"g.bin", graphics), sha(graphics)},
                                      {"ini", make(L"i.bin", ini), sha(ini)},
                                      {"ngx", make(L"n.bin", ngx), std::string()}};
    check(usePayloadFiles(files, "9.9.9", &err), "mapped files are accepted: " + err);
    check(payloadItem("native_graphics", &data, &size) && std::string(static_cast<const char*>(data), size) == graphics, "payloadItem serves the mapped file under its native name");
    const void* alias = nullptr;
    size_t aliasSize = 0;
    check(payloadItem("d3d11", &alias, &aliasSize) && alias == data && aliasSize == size, "the d3d11 alias serves the same bytes");
    check(!payloadItem("openvr", &data, &size) && !payloadItem("native_runtime", &data, &size), "an item the payload lacks is not served from anywhere else");
    check(!payloadItem("bogus", &data, &size), "an unknown item is not served");
    const PayloadInfo& info = payloadInfo();
    check(&info == reference, "payloadInfo hands back the same object, now updated");
    check(info.version == "9.9.9" && info.haveD3d11 && info.d3d11Sha == sha(graphics) && info.haveIni && info.iniText == ini && info.haveNgx && info.ngxSha == sha(ngx),
          "payloadInfo follows the files: version, hashes and the ini text");
    check(!info.haveOpenvr && !info.nativePairValid && !info.nativeGraphicsValid, "what is absent or not a valid native image reads absent or invalid");

    // The pointer handed out earlier outlives a replacement.
    check(payloadItem("ini", &data, &size), "the ini is served");
    const char* oldIni = static_cast<const char*>(data);
    const size_t oldIniSize = size;
    const std::string newIni = "[fix]\nx=2\n";
    check(usePayloadFiles({{"ini", make(L"i2.bin", newIni), sha(newIni)}}, "9.9.10", &err), "the payload can be replaced: " + err);
    check(std::string(oldIni, oldIniSize) == ini, "a pointer from before the replacement still reads the old bytes");
    check(payloadItem("ini", &data, &size) && std::string(static_cast<const char*>(data), size) == newIni && payloadInfo().iniText == newIni && payloadInfo().version == "9.9.10",
          "after the replacement the new bytes and version are served");
    check(!payloadItem("native_graphics", &data, &size) && !payloadInfo().haveD3d11, "the replaced payload no longer has the old items");

    // Refusals change nothing.
    check(!usePayloadFiles({{"ini", make(L"i3.bin", "other"), sha(ini)}}, "bad", &err) && contains(err, "hash"), "a wrong hash is refused: " + err);
    check(!usePayloadFiles({{"bogus", make(L"b.bin", "x"), std::string()}}, "bad", &err) && contains(err, "unknown"), "an unknown item is refused: " + err);
    check(!usePayloadFiles({{"d3d11", make(L"d1.bin", "x"), std::string()}, {"native_graphics", make(L"d2.bin", "y"), std::string()}}, "bad", &err) && contains(err, "twice"),
          "an alias and its native name together are refused as a repeat: " + err);
    check(!usePayloadFiles({{"ini", P(dir, L"missing.bin"), std::string()}}, "bad", &err) && contains(err, "cannot be opened"), "a missing file is refused: " + err);
    check(!usePayloadFiles({{"ini", make(L"empty.bin", ""), std::string()}}, "bad", &err), "an empty file is refused");
    check(payloadInfo().version == "9.9.10" && payloadItem("ini", &data, &size) && std::string(static_cast<const char*>(data), size) == newIni, "after the refusals the previous payload is intact");

    // Every item a manifest may name is one the payload knows.
    std::vector<PayloadFile> all;
    int index = 0;
    for (const char* item : kManifestItems) {
        const std::string bytes = std::string("bytes of ") + item;
        all.push_back({item, make((L"all" + std::to_wstring(index++) + L".bin").c_str(), bytes), sha(bytes)});
    }
    check(usePayloadFiles(all, "all", &err), "every manifest item is accepted by the payload: " + err);
    for (const char* item : kManifestItems) {
        check(payloadItem(item, &data, &size) && std::string(static_cast<const char*>(data), size) == std::string("bytes of ") + item, std::string("item ") + item + " is served");
    }
    check(payloadInfo().haveXess && payloadInfo().haveOpenxrLoader && payloadInfo().haveOpenxrLicense && payloadInfo().haveFlatIni && !payloadInfo().descriptorText.empty(),
          "payloadInfo reads the XeSS, loader, license, flat ini and descriptor items too");

    // No files is the empty payload; reset returns to the executable's own.
    check(usePayloadFiles({}, "none", &err), "an empty payload is allowed: " + err);
    check(!payloadItem("ini", &data, &size) && !payloadInfo().haveIni && payloadInfo().version == "none", "an empty payload serves nothing and says its version");
    resetPayloadFiles();
    check(payloadInfo().version == baseVersion && !payloadInfo().haveIni && &payloadInfo() == reference, "reset returns to the embedded behaviour");
}

// ---- WinHttpClient against a loopback server ----------------------------------------------------

std::string requestHeader(const std::string& request, const std::string& name) {
    std::string lowerReq = request, lowerName = name;
    for (char& c : lowerReq) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t at = lowerReq.find("\r\n" + lowerName + ":");
    if (at == std::string::npos) return std::string();
    size_t start = at + 2 + lowerName.size() + 1;
    while (start < request.size() && request[start] == ' ') ++start;
    const size_t end = request.find("\r\n", start);
    return request.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

class TestServer {
public:
    unsigned short port = 0;
    std::atomic<int> connections{0};
    std::atomic<bool> slowAborted{false};

    bool start() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) return false;
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        addr.sin_port = 0;
        if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        int len = sizeof(addr);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port = ntohs(addr.sin_port);
        if (listen(listener_, 16) != 0) return false;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void shutdown() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
        listener_ = INVALID_SOCKET;
        WSACleanup();
    }

    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }

    std::vector<std::string> requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    void run() {
        while (!stop_) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener_, &set);
            timeval wait = {0, 100000};
            if (select(0, &set, nullptr, nullptr, &wait) <= 0) continue;
            SOCKET s = accept(listener_, nullptr, nullptr);
            if (s == INVALID_SOCKET) continue;
            ++connections;
            DWORD timeout = 5000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            handle(s);
            shutdown_(s);
        }
    }

    static void shutdown_(SOCKET s) {
        ::shutdown(s, SD_SEND);
        closesocket(s);
    }

    static bool sendAll(SOCKET s, const char* data, size_t size) {
        size_t done = 0;
        while (done < size) {
            const int n = send(s, data + done, static_cast<int>(std::min<size_t>(size - done, 1 << 20)), 0);
            if (n <= 0) return false;
            done += static_cast<size_t>(n);
        }
        return true;
    }

    static bool reply(SOCKET s, int status, const char* text, const std::string& headers, const std::string& body, bool withLength = true) {
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\n" + headers;
        if (withLength) head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        head += "Connection: close\r\n\r\n";
        return sendAll(s, head.data(), head.size()) && sendAll(s, body.data(), body.size());
    }

    void handle(SOCKET s) {
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            const int n = recv(s, buffer, sizeof(buffer), 0);
            if (n <= 0) return;
            request.append(buffer, static_cast<size_t>(n));
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request);
        }
        const size_t sp1 = request.find(' ');
        const size_t sp2 = request.find(' ', sp1 + 1);
        std::string path = request.substr(sp1 + 1, sp2 - sp1 - 1);
        const size_t query = path.find('?');
        if (query != std::string::npos) path.erase(query);
        const std::string self = "http://127.0.0.1:" + std::to_string(port);

        if (path == "/ok") {
            if (requestHeader(request, "If-None-Match") == "\"v1\"") { reply(s, 304, "Not Modified", "ETag: \"v1\"\r\n", ""); return; }
            reply(s, 200, "OK", "ETag: \"v1\"\r\n", "hello");
        } else if (path == "/redir") {
            reply(s, 302, "Found", "Location: /ok\r\n", "");
        } else if (path == "/redir-rel") {
            reply(s, 302, "Found", "Location: ok\r\n", "");
        } else if (path == "/redir-abs") {
            reply(s, 301, "Moved", "Location: " + self + "/ok\r\n", "");
        } else if (path == "/redir-307") {
            reply(s, 307, "Temporary Redirect", "Location: /redir\r\n", "");
        } else if (path == "/redir-loop") {
            reply(s, 302, "Found", "Location: /redir-loop\r\n", "");
        } else if (path == "/redir-plain") {
            reply(s, 302, "Found", "Location: http://example.com/x\r\n", "");
        } else if (path == "/redir-localhost") {
            reply(s, 302, "Found", "Location: http://localhost:" + std::to_string(port) + "/ok\r\n", "");
        } else if (path == "/redir-none") {
            reply(s, 302, "Found", "", "");
        } else if (path == "/404") {
            reply(s, 404, "Not Found", "", "nothing here");
        } else if (path == "/big") {
            reply(s, 200, "OK", "", std::string(100000, 'x'));
        } else if (path == "/big-chunked") {
            reply(s, 200, "OK", "", std::string(100000, 'x'), false);
        } else if (path == "/short") {
            // Promises 5000 bytes, sends 100, and closes.
            std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\nConnection: close\r\n\r\n" + std::string(100, 'y');
            sendAll(s, head.data(), head.size());
        } else if (path == "/file") {
            std::string body(300000, '\0');
            for (size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>('a' + (i * 7) % 26);
            reply(s, 200, "OK", "", body);
        } else if (path == "/slow") {
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 4194304\r\nConnection: close\r\n\r\n";
            if (!sendAll(s, head.data(), head.size())) { slowAborted = true; return; }
            const std::string chunk(65536, 's');
            for (int i = 0; i < 64; ++i) {
                if (!sendAll(s, chunk.data(), chunk.size())) { slowAborted = true; return; }
                Sleep(20);
            }
        } else if (path == "/echo") {
            reply(s, 200, "OK", "", request);
        } else if (path == "/1.1/server/time") {
            reply(s, 200, "OK", "", "{\"unixTimestamp\":1800000000}");
        } else if (path == "/3.0/user/steam/auth") {
            reply(s, 302, "Found", "Location: /echo\r\n", "");
        } else if (path == "/3.0/user/purchases") {
            reply(s, 307, "Temporary Redirect", "Location: /echo\r\n", "");
        } else if (path == "/3.0/user/frontier/auth") {
            reply(s, 302, "Found", "Location: /echo\r\n", "");
        } else {
            reply(s, 404, "Not Found", "", "");
        }
    }

    SOCKET listener_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::vector<std::string> requests_;
};

void testHttp() {
    section("http");
    // The address rules, without a network.
    std::string why;
    check(urlAllowed("https://api.github.com/x", false, &why), "https is allowed");
    check(!urlAllowed("http://api.github.com/x", false, &why) && contains(why, "plain http"), "plain http is refused: " + why);
    check(!urlAllowed("http://api.github.com/x", true, &why), "plain http to another host is refused even for the rig's client");
    check(!urlAllowed("http://127.0.0.1:80/x", false, &why), "loopback http is refused by the product's client");
    check(urlAllowed("http://127.0.0.1:8080/x", true, &why), "loopback http is allowed for the rig's client");
    check(!urlAllowed("http://127.0.0.2/x", true, &why) && !urlAllowed("http://localhost/x", true, &why) && !urlAllowed("http://127.0.0.1.evil.com/x", true, &why) &&
              !urlAllowed("http://127.0.0.1@evil.com/x", true, &why),
          "only the literal 127.0.0.1 counts as loopback");
    check(!urlAllowed("ftp://x/y", true, &why) && !urlAllowed("file:///c:/x", true, &why) && !urlAllowed("//x/y", true, &why) && !urlAllowed("", true, &why), "other schemes are refused");
    check(!urlAllowed("https://user:pw@host/x", false, &why) && !urlAllowed("https://host:99999/x", false, &why) && !urlAllowed("https://ho st/x", false, &why),
          "user information, a bad port and a space are refused");
    check(redirectAllowed("https://a.example/x", "https://b.example/y", false, &why), "https to https may be followed");
    check(!redirectAllowed("https://a.example/x", "http://127.0.0.1:1/y", true, &why) && contains(why, "https to http"), "https down to http is refused, even to loopback: " + why);
    check(redirectAllowed("http://127.0.0.1:1/x", "https://b.example/y", true, &why), "http up to https may be followed");
    check(!redirectAllowed("http://127.0.0.1:1/x", "http://b.example/y", true, &why), "a redirect to plain http elsewhere is refused");
    check(resolveLocation("https://h.example/a/b?c=1", "/z") == "https://h.example/z", "an absolute path resolves against the origin");
    check(resolveLocation("https://h.example/a/b?c=1", "z") == "https://h.example/a/z", "a relative path resolves against the directory");
    check(resolveLocation("https://h.example/a/b", "http://o.example/q") == "http://o.example/q", "an absolute address stands");
    check(resolveLocation("https://h.example:8443/a/b", "//o.example/q") == "https://o.example/q", "a scheme-relative address takes the scheme");
    check(resolveLocation("https://h.example/a/b", "?x=1") == "https://h.example/a/b?x=1", "a query-only location keeps the path");

    TestServer server;
    check(server.start(), "the loopback server starts");
    if (server.port == 0) return;
    WinHttpClient client(true, "9.8.7");

    HttpRequest r;
    r.url = server.url("/ok");
    HttpResponse resp = client.get(r);
    check(resp.transportOk && resp.status == 200 && resp.body == "hello" && resp.etag == "\"v1\"" && resp.bytes == 5, "a 200 returns its body and ETag");
    r.headers = {{"If-None-Match", "\"v1\""}};
    resp = client.get(r);
    check(resp.transportOk && resp.status == 304 && resp.body.empty(), "a matching If-None-Match gets a 304 with no body");
    r.headers.clear();
    r.url = server.url("/404");
    resp = client.get(r);
    check(resp.transportOk && resp.status == 404 && resp.body.empty(), "a 404 is a status, not a transport failure");

    // Redirects.
    for (const char* path : {"/redir", "/redir-rel", "/redir-abs", "/redir-307"}) {
        r.url = server.url(path);
        resp = client.get(r);
        check(resp.transportOk && resp.status == 200 && resp.body == "hello", std::string("redirect ") + path + " is followed");
    }
    r.url = server.url("/redir-loop");
    resp = client.get(r);
    check(!resp.transportOk && contains(resp.error, "too many redirects"), "a redirect loop stops: " + resp.error);
    r.url = server.url("/redir-none");
    resp = client.get(r);
    check(!resp.transportOk && contains(resp.error, "no place"), "a redirect with no Location fails: " + resp.error);
    const int beforePlain = server.connections;
    r.url = server.url("/redir-plain");
    resp = client.get(r);
    check(!resp.transportOk && contains(resp.error, "plain http"), "a redirect to a plain http address elsewhere is refused: " + resp.error);
    r.url = server.url("/redir-localhost");
    resp = client.get(r);
    check(!resp.transportOk && contains(resp.error, "plain http"), "a redirect to localhost (not 127.0.0.1) is refused: " + resp.error);
    check(server.connections == beforePlain + 2, "each refused redirect cost only the one request that carried it");

    // Plain http is refused before any socket opens.
    {
        WinHttpClient strict(false);
        const int connectionsBefore = server.connections;
        HttpRequest direct;
        direct.url = server.url("/ok");
        resp = strict.get(direct);
        check(!resp.transportOk && contains(resp.error, "plain http") && server.connections == connectionsBefore, "the product's client refuses http to the loopback listener without connecting: " + resp.error);
        direct.url = "http://127.0.0.2:" + std::to_string(server.port) + "/ok";
        resp = client.get(direct);
        check(!resp.transportOk && server.connections == connectionsBefore, "the rig's client refuses http to another loopback address without connecting");
        direct.url = "http://example.com/";
        resp = client.get(direct);
        check(!resp.transportOk && contains(resp.error, "plain http"), "the rig's client refuses plain http to another host");
        direct.url = "ftp://example.com/";
        check(!client.get(direct).transportOk, "other schemes are refused");
    }

    // Size caps.
    r.url = server.url("/big");
    r.maxBytes = 1000;
    resp = client.get(r);
    check(!resp.transportOk && contains(resp.error, "larger") && resp.body.empty(), "a Content-Length past the cap fails early: " + resp.error);
    r.url = server.url("/big-chunked");
    resp = client.get(r);
    check(!resp.transportOk && contains(resp.error, "larger") && resp.body.empty(), "a body with no length is cut off at the cap: " + resp.error);
    r.maxBytes = 100000;
    r.url = server.url("/big");
    resp = client.get(r);
    check(resp.transportOk && resp.body.size() == 100000, "a body exactly at the cap is accepted");
    r.maxBytes = 0;
    r.url = server.url("/short");
    resp = client.get(r);
    check(!resp.transportOk, "a reply cut short of its Content-Length fails");

    // To a file, with progress.
    const std::wstring dir = fresh("http");
    {
        HttpRequest f;
        f.url = server.url("/file");
        f.toFile = P(dir, L"download.bin");
        unsigned long long lastGot = 0, lastTotal = 0;
        int calls = 0;
        bool monotonic = true;
        f.progress = [&](unsigned long long got, unsigned long long total) {
            monotonic = monotonic && got >= lastGot;
            lastGot = got;
            lastTotal = total;
            ++calls;
        };
        resp = client.get(f);
        const std::string saved = readAll(f.toFile);
        check(resp.transportOk && resp.status == 200 && resp.body.empty() && saved.size() == 300000 && resp.bytes == 300000, "a download lands in its file");
        check(saved[0] == 'a' && saved[1] == 'h' && saved[299999] == static_cast<char>('a' + (299999 * 7) % 26), "the file's bytes are the server's");
        check(calls >= 2 && monotonic && lastGot == 300000 && lastTotal == 300000, "progress was reported, rising to the total");
        f.maxBytes = 1000;
        f.toFile = P(dir, L"capped.bin");
        resp = client.get(f);
        check(!resp.transportOk && !exists(f.toFile), "a download past its cap leaves no file");
        f.maxBytes = 0;
        f.url = server.url("/404");
        f.toFile = P(dir, L"never.bin");
        resp = client.get(f);
        check(resp.transportOk && resp.status == 404 && !exists(f.toFile), "a 404 creates no file");
    }

    // Cancel mid-body.
    {
        std::atomic<bool> cancel{false};
        HttpRequest s;
        s.url = server.url("/slow");
        s.cancel = &cancel;
        s.toFile = P(dir, L"slow.bin");
        s.progress = [&](unsigned long long got, unsigned long long) { if (got >= 65536) cancel = true; };
        const DWORD began = GetTickCount();
        resp = client.get(s);
        check(!resp.transportOk && resp.cancelled && contains(resp.error, "cancel"), "a cancel stops the download: " + resp.error);
        check(GetTickCount() - began < 10000 && !exists(s.toFile), "it stopped well before the body ended and removed the partial file");
        for (int i = 0; i < 100 && !server.slowAborted; ++i) Sleep(50);
        check(server.slowAborted, "the server saw the connection close before the body was done");
        std::atomic<bool> already{true};
        HttpRequest pre;
        pre.url = server.url("/ok");
        pre.cancel = &already;
        const int connectionsBefore = server.connections;
        resp = client.get(pre);
        check(!resp.transportOk && resp.cancelled && server.connections == connectionsBefore, "a flag already set cancels before connecting");
    }

    // What a request says about the machine: only the user agent and the headers asked for.
    {
        HttpRequest e;
        e.url = server.url("/echo");
        e.headers = {{"Accept", "application/vnd.github+json"}};
        resp = client.get(e);
        const std::string seen = resp.body;
        check(resp.transportOk && contains(seen, "GET /echo HTTP/1.1"), "the echo returns the request as the server saw it");
        check(requestHeader(seen, "User-Agent") == "ed-up-launcher/9.8.7", "the user agent is ed-up-launcher/<version>: " + requestHeader(seen, "User-Agent"));
        check(requestHeader(seen, "Accept") == "application/vnd.github+json", "the caller's header went out");
        check(!contains(seen, "Authorization") && !contains(seen, "Cookie") && !contains(seen, "Referer") && !contains(seen, "Proxy-"), "no credential, cookie or referer is sent");
        char computer[256] = {};
        DWORD cn = sizeof(computer);
        if (GetComputerNameA(computer, &cn) && cn >= 4) {
            std::string lowerSeen = seen, lowerName = computer;
            for (char& c : lowerSeen) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            check(!contains(lowerSeen, lowerName), "the computer's name is not in the request");
        }
        char user[256] = {};
        DWORD un = sizeof(user);
        if (GetUserNameA(user, &un) && un >= 5) {
            std::string lowerSeen = seen, lowerUser = user;
            for (char& c : lowerSeen) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : lowerUser) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            check(!contains(lowerSeen, lowerUser.substr(0, un - 1)), "the user's name is not in the request");
        }
        WinHttpClient defaultAgent(true);
        resp = defaultAgent.get(e);
        check(requestHeader(resp.body, "User-Agent") == std::string("ed-up-launcher/") + EDUP_LAUNCHER_VERSION, "the default user agent carries the launcher's version");
    }

    // A request that asks not to follow redirects gets the redirect as its answer, with nothing else sent.
    {
        HttpRequest stay;
        stay.url = server.url("/redir");
        stay.headers = {{"Authorization", "bearer TOKEN-NOT-FOR-ELSEWHERE"}};
        stay.followRedirects = false;
        const size_t before = server.requests().size();
        resp = client.get(stay);
        check(resp.transportOk && resp.status == 302 && resp.body.empty(), "with followRedirects off a 302 is the answer, not a transport failure");
        check(server.requests().size() == before + 1, "and the Location was not requested: one request reached the server");
        for (const char* path : {"/redir-307", "/redir-abs", "/redir-plain"}) {
            stay.url = server.url(path);
            resp = client.get(stay);
            check(resp.transportOk && (resp.status == 307 || resp.status == 301 || resp.status == 302) && resp.body.empty(), std::string(path) + " is returned as it is, even to an address the redirect rules would refuse");
        }
        stay.followRedirects = true;
        stay.url = server.url("/redir");
        resp = client.get(stay);
        check(resp.transportOk && resp.status == 200 && resp.body == "hello", "and the default still follows");
    }

    // Frontier's calls through the real client: no redirect is followed, the user agent is honest, and the
    // wire shows what the rig's scripted replies assumed.
    {
        WinHttpClient wire(true, "9.8.7");
        FrontierApi api(wire, "http://127.0.0.1:" + std::to_string(server.port));
        const size_t before = server.requests().size();
        check(api.syncTime(), "the time call reaches the server and reads unixTimestamp");
        FrontierSession none;
        const ApiResult auth = api.steamAuth("0123456789abcdef", "AABB01", &none);
        check(auth.outcome == ApiOutcome::RegistrationRequired && auth.httpStatus == 302 && none.authToken.empty(), "a 302 to sign-in is registration required, with no tokens");
        FrontierSession held;
        held.authToken = "TOKEN-A";
        held.machineToken = "TOKEN-M";
        PurchaseList list;
        const ApiResult bought = api.purchases(held, &list);
        check(bought.outcome == ApiOutcome::LinkAvailable && bought.httpStatus == 307 && list.items.empty(), "a 307 on purchases is link available");
        const std::vector<std::string> seen = server.requests();
        check(seen.size() == before + 3, "three requests reached the server and no redirect target was fetched: " + std::to_string(seen.size() - before));
        if (seen.size() == before + 3) {
            const std::string& time = seen[before];
            const std::string& signIn = seen[before + 1];
            const std::string& purchases = seen[before + 2];
            check(time.find("GET /1.1/server/time HTTP/1.1") == 0, "the time request is a plain GET with no query");
            check(signIn.find("GET /3.0/user/steam/auth?steamTicket=AABB01&machineId=0123456789abcdef&fTime=") == 0, "the sign-in's query is steamTicket, machineId, fTime, in that order");
            check(requestHeader(signIn, "Authorization").empty() && !contains(signIn, "Authorization"), "the sign-in carries no Authorization header");
            check(requestHeader(purchases, "Authorization") == "bearer TOKEN-A", "purchases carries the bearer token: " + requestHeader(purchases, "Authorization"));
            for (const std::string* request : {&time, &signIn, &purchases})
                check(requestHeader(*request, "User-Agent") == "ed-up-launcher/9.8.7" && !contains(*request, "min-ed-launcher") && !contains(*request, "EDLaunch"), "the user agent is the launcher's own");
        }
        // A transport failure puts no address and no token in its message: these URLs carry tokens.
        FrontierApi down(wire, "http://127.0.0.1:1");
        FrontierSession tokens;
        const ApiResult failed = down.steamAuth("0123456789abcdef", "SENTINEL-TICKET-1f2e3d", &tokens);
        check(failed.outcome == ApiOutcome::Unavailable && !contains(failed.message, "127.0.0.1") && !contains(failed.message, "SENTINEL") && !contains(failed.message, "0123456789abcdef"),
              "a connection that fails says so without the address or any value: " + failed.message);
        HttpRequest direct;
        direct.url = "http://127.0.0.1:1/x?authToken=SENTINEL-TOKEN-9a8b7c";
        resp = client.get(direct);
        check(!resp.transportOk && !contains(resp.error, "SENTINEL") && !contains(resp.error, "127.0.0.1") && !contains(resp.error, "authToken"), "and neither does the HTTP client's own error: " + resp.error);
    }

    // POST, as Frontier's store sign-in makes it: no body, Content-Length: 0, and no redirect followed.
    {
        WinHttpClient wire(true, "9.8.7");
        HttpRequest post;
        post.method = "POST";
        post.url = server.url("/echo?a=1");
        const HttpResponse echoed = wire.get(post);
        check(echoed.transportOk && echoed.status == 200 && echoed.body.find("POST /echo?a=1 HTTP/1.1") == 0, "a POST goes out as one: " + echoed.body.substr(0, 40));
        check(requestHeader(echoed.body, "Content-Length") == "0" && requestHeader(echoed.body, "Authorization").empty() && requestHeader(echoed.body, "User-Agent") == "ed-up-launcher/9.8.7",
              "with an empty body declared as Content-Length: 0, and the honest user agent");
        post.url = server.url("/redir");
        post.followRedirects = true;
        const size_t before = server.requests().size();
        const HttpResponse redirected = wire.get(post);
        check(redirected.transportOk && redirected.status == 302 && server.requests().size() == before + 1, "a redirect to a POST is never followed, even when following is on");
        HttpRequest odd;
        odd.method = "DELETE";
        odd.url = server.url("/ok");
        check(!wire.get(odd).transportOk, "a method other than GET and POST is refused");

        // The Frontier-store sign-in's first call on the wire: POST, email and password as hex, in the port's order.
        FrontierApi api(wire, "http://127.0.0.1:" + std::to_string(server.port));
        std::string enc;
        const size_t beforeApi = server.requests().size();
        const ApiResult first = api.frontierFirstSignIn("a@b.c", "pw", "0123456789abcdef", &enc);
        check(first.outcome == ApiOutcome::RegistrationRequired && enc.empty(), "a 302 to the first sign-in is not followed and gives no code");
        const std::vector<std::string> seen = server.requests();
        check(seen.size() == beforeApi + 1 && seen.back().find("POST /3.0/user/frontier/auth?email=6140622E63&password=7077&machineId=0123456789abcdef&lang=en&fTime=") == 0,
              "its query is email, password, machineId, lang, fTime, with the first two as hex of their bytes: " + (seen.empty() ? std::string() : seen.back().substr(0, 120)));
        check(!seen.empty() && !contains(seen.back(), "Authorization") && !contains(first.message, "6140622E63") && !contains(first.message, "127.0.0.1"), "no Authorization header, and nothing sent is in the message");
    }
    server.shutdown();
}


// ===========================================================================================
// The launcher's exe: everything around the update core that has no window of its own.
//
// Programs are only ever the two fakes build.bat compiles into <fakes dir> (fake_proc.cpp), the
// store addresses are recorded by RouteOps and never opened, the registry is one subkey of
// HKCU this rig creates and removes, and every folder is under the scratch folder.
// ===========================================================================================

std::wstring g_fakes;  // build\launcher_test_fakes
std::wstring g_self;   // this program, started again as the stand-in for the elevated copy
const wchar_t* kFakeGame = L"ed_up_fake_game.exe";
const wchar_t* kFakeEd = L"ed_up_fake_edlaunch.exe";

HANDLE startProgram(const std::wstring& exe, const std::wstring& args) {
    std::wstring line = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), &line[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

bool stillRunning(HANDLE p) { return p && WaitForSingleObject(p, 0) == WAIT_TIMEOUT; }

void endProgram(HANDLE p) {
    if (!p) return;
    TerminateProcess(p, 0);
    WaitForSingleObject(p, 3000);
    CloseHandle(p);
}

// Anything an earlier run of this rig left behind under the fakes' names.
void endFakes() {
    for (const wchar_t* name : {kFakeGame, kFakeEd}) {
        std::vector<unsigned long> pids;
        pidsNamed(name, &pids);
        for (unsigned long pid : pids) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
            if (!h) continue;
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 3000);
            CloseHandle(h);
        }
    }
}

std::string logText(const std::wstring& path) { return readAll(path); }

// ---- core: roots, payload folders, the developer check ---------------------------------------

std::wstring copyOfSystemDll(const std::wstring& dest) {
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    CopyFileW(P(system, L"wsock32.dll").c_str(), dest.c_str(), FALSE);
    return dest;
}

void testCore() {
    section("core");
    const LauncherRoots real = launcherRoots(L"");
    check(real.canonical && !real.dataRoot.empty() && real.dataRoot.size() > 4 && real.dataRoot.substr(real.dataRoot.size() - 4) == L"EDVR",
          "with no --data-root the root is %LOCALAPPDATA%\\EDVR and the run is canonical");
    const LauncherRoots trial = launcherRoots(L"C:\\scratch\\root");
    check(!trial.canonical && trial.dataRoot == L"C:\\scratch\\root" && trial.launcherDir() == L"C:\\scratch\\root\\launcher" &&
              trial.cacheDir() == L"C:\\scratch\\root\\payload" && trial.exePath() == L"C:\\scratch\\root\\launcher\\ed-up-launcher.exe",
          "--data-root names the folder, marks the run as a trial, and the folders under it are launcher and payload");

    // quoteArg: what CommandLineToArgvW reads back is what went in.
    {
        const std::vector<std::wstring> awkward = {L"plain", L"with space", L"quote\"inside", L"trailing\\", L"C:\\Program Files\\Elite Dangerous\\", L"", L"two  spaces\\\\", L"a\\\"b"};
        std::wstring line = L"prog";
        for (const std::wstring& a : awkward) line += L" " + quoteArg(a);
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(line.c_str(), &argc);
        bool same = argv && argc == static_cast<int>(awkward.size()) + 1;
        for (size_t i = 0; same && i < awkward.size(); ++i) same = awkward[i] == argv[i + 1];
        if (argv) LocalFree(argv);
        check(same, "quoteArg survives CommandLineToArgvW for spaces, quotes and trailing backslashes");
    }

    // A payload folder: the table of paths, and the version from d3d11.dll's own stamp.
    {
        const std::wstring dir = fresh("core-folder");
        copyOfSystemDll(P(dir, L"d3d11.dll"));
        writeAll(P(dir, L"edvr.ini"), "[fix]\nx=1\n");
        writeAll(P(dir, L"Openvr\\openvr_api.dll"), "runtime");
        writeAll(P(dir, L"unrelated.txt"), "ignored");
        std::vector<PayloadFile> files;
        std::string version, err;
        check(listPayloadFolder(dir, &files, &version, &err), "an unpacked zip lists: " + err);
        check(files.size() == 3, "only the files the table names are listed: " + std::to_string(files.size()));
        check(version == narrowUtf8(fileVersionOf(P(dir, L"d3d11.dll"))) && !version.empty(), "the version is d3d11.dll's FileVersion: " + version);
        bool graphics = false, ini = false, runtime = false;
        for (const PayloadFile& f : files) {
            graphics |= f.item == "native_graphics" && f.sha256.empty();
            ini |= f.item == "ini";
            runtime |= f.item == "native_runtime" && _wcsicmp(f.path.c_str(), P(dir, L"Openvr\\openvr_api.dll").c_str()) == 0;
        }
        check(graphics && ini && runtime, "the items are the manifest's names, and no hash is claimed for the user's folder");

        const LauncherRoots roots = launcherRoots(L"");
        const PayloadLoad load = loadLauncherPayload(roots, dir, g_testKeys);
        check(load.ok && load.kind == PayloadKind::Folder && load.version == version, "loadLauncherPayload takes the folder: " + load.note);
        const void* data = nullptr;
        size_t size = 0;
        check(payloadItem("ini", &data, &size) && std::string(static_cast<const char*>(data), size) == "[fix]\nx=1\n", "payloadItem now serves the folder's edvr.ini");
        check(payloadPresent(payloadInfo()) && payloadInfo().version == version, "payloadPresent says there is a payload, with its version");
        resetPayloadFiles();
        check(!payloadPresent(payloadInfo()), "reset: a build with no embedded resources has no payload (the state the launcher starts in)");

        check(!loadLauncherPayload(roots, P(dir, L"nowhere"), g_testKeys).ok, "a folder that is not there is refused");
        const std::wstring empty = fresh("core-folder-empty");
        const PayloadLoad none = loadLauncherPayload(roots, empty, g_testKeys);
        check(!none.ok && contains(none.note, "d3d11.dll"), "a folder with no d3d11.dll is refused, and says what to point at: " + none.note);
        check(!payloadPresent(payloadInfo()), "and a refused load leaves the process with no payload");
        // (The folder above is still mapped by the retired payload, so this is another one.)
        const std::wstring plain = fresh("core-folder-plain");
        writeAll(P(plain, L"d3d11.dll"), "no version resource in these bytes");
        check(!listPayloadFolder(plain, &files, &version, &err) && contains(err, "version"), "a d3d11.dll without a stamp is refused: " + err);
    }

    // A stored manifest: the first run has none, and after an adoption it loads.
    {
        const Rel a = loadRel("relA");
        const std::wstring base = fresh("core-manifest");
        const LauncherRoots roots = launcherRoots(base);
        const PayloadLoad first = loadLauncherPayload(roots, L"", g_testKeys);
        check(!first.ok && first.firstRun && first.kind == PayloadKind::None, "nothing adopted yet is the first run, not an error");
        check(!payloadPresent(payloadInfo()), "and it leaves no payload");

        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(a), "\"etag-A\"");
        serveRelease(&http, a);
        UpdaterSettings s = settingsFor(base);
        Updater up(http, s);
        std::string err;
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Available && adopt(&up, c, &err), "release A is adopted: " + err);
        const PayloadLoad load = loadLauncherPayload(roots, L"", g_testKeys);
        check(load.ok && load.kind == PayloadKind::Manifest && load.version == "v1.0.0" && !load.firstRun, "the adopted manifest loads, with its build stamp as the version: " + load.note);
        check(load.manifestPath == P(P(roots.launcherDir(), L"manifests"), widenUtf8(sha(a.manifestBytes)) + L".json") && exists(load.manifestPath), "the load names the stored manifest it used");
        const void* data = nullptr;
        size_t size = 0;
        check(payloadItem("native_graphics", &data, &size) && std::string(static_cast<const char*>(data), size) == "graphics one", "payloadItem serves the cached graphics file");
        resetPayloadFiles();

        // The cache lost a file: named, and not a first run. (Another root: the files above are still mapped.)
        const std::wstring base2 = fresh("core-manifest-broken");
        const LauncherRoots roots2 = launcherRoots(base2);
        ScriptedHttp http2;
        http2.replies[latestUrl()] = ok(releaseJson(a), "\"etag-A\"");
        serveRelease(&http2, a);
        Updater up2(http2, settingsFor(base2));
        const UpdateCheck c2 = up2.check();
        check(c2.outcome == UpdateOutcome::Available && adopt(&up2, c2, &err), "release A is adopted into a second root: " + err);
        check(DeleteFileW(PayloadCache(roots2.cacheDir()).pathFor(a.manifest.files[0].sha256).c_str()) != 0, "and one of its cached files is deleted");
        const PayloadLoad broken = loadLauncherPayload(roots2, L"", g_testKeys);
        check(!broken.ok && !broken.firstRun && contains(broken.note, "cache lacks"), "a cache missing a file is reported: " + broken.note);
        check(!payloadPresent(payloadInfo()), "and the payload stays empty");
    }

    // Developer installs: a receipt newer than the install record, or a receipt with no record.
    {
        const std::wstring game = fresh("core-dev");
        check(!isDeveloperInstall(game), "a folder with no receipt and no record is not a developer's");
        writeAll(P(game, L"edvr_install\\state.ini"), "version=1\n");
        check(!isDeveloperInstall(game), "a record alone is not either");
        auto stamp = [](const std::wstring& path, int secondsFromNow) {
            HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER t;
            t.LowPart = now.dwLowDateTime;
            t.HighPart = now.dwHighDateTime;
            t.QuadPart += static_cast<ULONGLONG>(static_cast<LONGLONG>(secondsFromNow) * 10000000LL);
            FILETIME ft{t.LowPart, static_cast<DWORD>(t.HighPart)};
            SetFileTime(f, nullptr, nullptr, &ft);
            CloseHandle(f);
        };
        stamp(P(game, L"edvr_install\\state.ini"), -1000);
        writeAll(P(game, L"edvr_native_receipt.json"), "{}");
        stamp(P(game, L"edvr_native_receipt.json"), -2000);
        check(!isDeveloperInstall(game), "a receipt older than the record is the launcher's own past, not a developer's");
        writeAll(P(game, L"edvr_flat_receipt.json.pre-20260101-000000-1"), "{}");
        stamp(P(game, L"edvr_flat_receipt.json.pre-20260101-000000-1"), -500);
        check(isDeveloperInstall(game), "a backup-named receipt newer than the record counts");
        const std::wstring bare = fresh("core-dev-bare");
        writeAll(P(bare, L"edvr_flat_receipt.json"), "{}");
        check(isDeveloperInstall(bare), "a receipt with no install record at all counts");
    }
}

// ---- register and unregister ------------------------------------------------------------------

std::wstring shortcutTarget(const std::wstring& link) {
    std::wstring target;
    IShellLinkW* l = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l)))) {
        IPersistFile* f = nullptr;
        if (SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&f)))) {
            if (SUCCEEDED(f->Load(link.c_str(), STGM_READ))) {
                wchar_t path[MAX_PATH]{};
                l->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH);
                target = path;
            }
            f->Release();
        }
        l->Release();
    }
    return target;
}

std::wstring regString(const std::wstring& key, const wchar_t* value) {
    wchar_t buf[1024]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return std::wstring();
    return buf;
}

bool regDword(const std::wstring& key, const wchar_t* value, DWORD* out) {
    DWORD size = sizeof(*out);
    return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_DWORD, nullptr, out, &size) == ERROR_SUCCESS;
}

void testRegister() {
    section("register");
    const std::wstring root = fresh("register");
    const std::wstring key = L"Software\\EdUpLauncherRigTest-" + std::to_wstring(GetCurrentProcessId());
    RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
    RegisterTargets t;
    t.launcherDir = P(root, L"launcher");
    t.payloadDir = P(root, L"payload");
    t.exePath = P(t.launcherDir, L"ed-up-launcher.exe");
    t.startMenuDir = P(root, L"StartMenu");
    t.registryKey = key;
    t.version = "0.1.0";
    writeAll(t.exePath, "the launcher");
    writeAll(P(t.launcherDir, L"launcher.ini"), "channel=rc\n");
    writeAll(P(P(t.launcherDir, L"manifests"), L"x.json"), "{}");
    writeAll(P(t.payloadDir, L"abc"), "cached");
    writeAll(P(root, L"mirror-folder\\edvr.ini"), "settings");   // beside the launcher folder, as the real mirror is

    std::string err;
    check(registerLauncher(t, &err), "registering works: " + err);
    const std::wstring link = P(t.startMenuDir, kShortcutName);
    check(exists(link), "the Start menu shortcut is written");
    check(_wcsicmp(shortcutTarget(link).c_str(), t.exePath.c_str()) == 0, "and points at the launcher exe: " + narrowUtf8(shortcutTarget(link)));
    check(regString(key, L"DisplayName") == L"ED Unofficial Patch (ED-UP)" && regString(key, L"DisplayVersion") == L"0.1.0" &&
              regString(key, L"Publisher") == L"ED-UP (unofficial)" && regString(key, L"InstallLocation") == t.launcherDir,
          "DisplayName, DisplayVersion, Publisher and InstallLocation are set");
    check(regString(key, L"UninstallString") == L"\"" + t.exePath + L"\" --unregister", "UninstallString runs the launcher with --unregister: " + narrowUtf8(regString(key, L"UninstallString")));
    DWORD noModify = 0, noRepair = 0;
    check(regDword(key, L"NoModify", &noModify) && noModify == 1 && regDword(key, L"NoRepair", &noRepair) && noRepair == 1, "NoModify and NoRepair are 1");
    check(registerLauncher(t, &err), "registering again replaces rather than fails: " + err);

    // Refusals before anything is deleted.
    UnregisterResult result;
    RegisterTargets wrong = t;
    wrong.launcherDir = P(root, L"somewhere-else");
    writeAll(P(wrong.launcherDir, L"precious.txt"), "keep");
    check(!unregisterLauncher(wrong, std::wstring(), &result, &err) && contains(err, "launcher"), "a launcher folder not named launcher is refused: " + err);
    check(exists(P(wrong.launcherDir, L"precious.txt")) && exists(link), "and nothing was removed");
    wrong = t;
    wrong.payloadDir = P(root, L"mirror-folder");
    check(!unregisterLauncher(wrong, std::wstring(), &result, &err) && contains(err, "payload") && exists(P(root, L"mirror-folder\\edvr.ini")), "a cache folder not named payload is refused: " + err);

    // The running exe cannot be deleted: it is held open here without delete sharing, as Windows holds a running image.
    HANDLE hold = CreateFileW(t.exePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(unregisterLauncher(t, t.exePath, &result, &err), "unregistering works: " + err);
    CloseHandle(hold);
    check(!exists(link) && regString(key, L"DisplayName").empty(), "the shortcut and the registry entry are gone");
    check(!exists(t.payloadDir), "the downloaded files are gone");
    check(!exists(P(t.launcherDir, L"launcher.ini")) && !exists(P(t.launcherDir, L"manifests")), "the launcher folder's other contents are gone");
    check(result.exeRemains && exists(t.exePath), "the exe that is running stays, and the result says so");
    check(exists(P(root, L"mirror-folder\\edvr.ini")), "the settings copy beside the launcher folder is untouched");
    std::string said;
    for (const std::string& n : result.notes) said += n + "\n";
    check(contains(said, "game folders") && contains(said, "settings"), "the report says the game folders and the settings copy were left alone: " + said);
    // With nothing held it goes all the way.
    check(unregisterLauncher(t, std::wstring(), &result, &err) && !result.exeRemains && !exists(t.launcherDir), "unregistering again, with the exe free, removes the folder too: " + err);
    RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
}

// ---- self-update -------------------------------------------------------------------------------

void testSelfUpdate() {
    section("selfupdate");
    const Rel b = loadRel("relB");
    const std::string newBytes = readAll(P(b.dir, L"ed-up-launcher.exe"));
    check(!newBytes.empty() && b.manifest.hasLauncher, "release B carries a launcher");
    const std::wstring base = fresh("self");
    ScriptedHttp http;
    http.replies[latestUrl()] = ok(releaseJson(b), "\"etag-B\"");
    serveRelease(&http, b);
    UpdaterSettings s = settingsFor(base);
    Updater up(http, s);
    const UpdateCheck c = up.check();
    check(c.outcome == UpdateOutcome::Available && c.launcherNeedsUpdate, "B offers a newer launcher: " + c.message);

    // The installed launcher is a program that is running, so the rename really is of a running image.
    const std::wstring exe = P(P(base, L"launcher"), L"ed-up-launcher.exe");
    createDirectories(P(base, L"launcher"));
    CopyFileW(P(g_fakes, kFakeGame).c_str(), exe.c_str(), FALSE);
    const std::string oldBytes = readAll(exe);
    HANDLE running = startProgram(exe, L"--run-ms 20000");
    check(running != nullptr && stillRunning(running), "the stand-in launcher is running from its own path");

    std::string err;
    std::string names;
    check(downloadLauncher(http, c, exe, nullptr, [&](const std::string& n, unsigned long long, unsigned long long) { names = n; }, &err), "the launcher downloads: " + err);
    check(readAll(newLauncherPath(exe)) == newBytes && names == "ed-up-launcher.exe", "to <exe>.new, byte for byte, with progress under the asset's name");
    check(readAll(exe) == oldBytes, "the running exe is untouched by the download");

    // A same-size download with other bytes is refused, and leaves no .new behind.
    {
        ScriptedHttp bad = http;
        std::string tampered = newBytes;
        tampered[tampered.size() - 1] = static_cast<char>(tampered[tampered.size() - 1] ^ 1);
        bad.replies[std::string(kDownloads) + "relB/ed-up-launcher.exe"].body = tampered;
        std::string e2;
        check(!downloadLauncher(bad, c, exe, nullptr, nullptr, &e2) && contains(e2, "hash") && !exists(newLauncherPath(exe)), "a changed download is refused by hash and removed: " + e2);
        std::string longer = newBytes + "x";
        bad.replies[std::string(kDownloads) + "relB/ed-up-launcher.exe"].body = longer;
        check(!downloadLauncher(bad, c, exe, nullptr, nullptr, &e2) && !exists(newLauncherPath(exe)), "a longer download is refused, by the size cap or the size check: " + e2);
        UpdateCheck noAsset = c;
        noAsset.assets.clear();
        check(!downloadLauncher(http, noAsset, exe, nullptr, nullptr, &e2) && contains(e2, "no asset"), "a release without the asset is refused: " + e2);
        UpdateCheck noEntry = c;
        noEntry.manifest.hasLauncher = false;
        check(!downloadLauncher(http, noEntry, exe, nullptr, nullptr, &e2) && contains(e2, "no launcher"), "a manifest without a launcher entry is refused: " + e2);
        // Put the good download back for the swaps below.
    }
    check(downloadLauncher(http, c, exe, nullptr, nullptr, &err), "the good download is back: " + err);

    // A failing second rename: the original is restored, and the error says which step.
    {
        int calls = 0;
        FileMover failSecond = [&](const std::wstring& from, const std::wstring& to) {
            ++calls;
            if (calls == 2) return false;
            return moveReplacing(from, to);
        };
        std::string e2;
        check(!swapInNewLauncher(exe, failSecond, &e2) && contains(e2, "old one is back"), "when the new file cannot take the name, the old one is put back: " + e2);
        check(calls == 3 && readAll(exe) == oldBytes && !exists(oldLauncherPath(exe)) && exists(newLauncherPath(exe)), "three moves were tried, the exe is the old one again, and the download is kept for a retry");
        // The first rename failing changes nothing.
        calls = 0;
        FileMover failFirst = [&](const std::wstring&, const std::wstring&) {
            ++calls;
            return false;
        };
        check(!swapInNewLauncher(exe, failFirst, &e2) && calls == 1 && readAll(exe) == oldBytes, "when the running exe cannot be renamed nothing else is tried: " + e2);
        // The restore failing too says where the old exe is.
        calls = 0;
        FileMover failRestore = [&](const std::wstring& from, const std::wstring& to) {
            ++calls;
            if (calls >= 2) return false;
            return moveReplacing(from, to);
        };
        check(!swapInNewLauncher(exe, failRestore, &e2) && contains(e2, "could not be put back") && contains(e2, "ed-up-launcher.exe.old"), "when the restore fails too, the old exe's new name is given: " + e2);
        moveReplacing(oldLauncherPath(exe), exe);  // the rig puts it back itself
        check(readAll(exe) == oldBytes, "the rig restored the stand-in");
    }

    // The real swap, with the old program still running from its renamed file.
    std::string e3;
    check(swapInNewLauncher(exe, FileMover(moveReplacing), &e3), "the swap works on a running exe: " + e3);
    check(readAll(exe) == newBytes && readAll(oldLauncherPath(exe)) == oldBytes && !exists(newLauncherPath(exe)), "the new bytes are at the exe's path and the old ones at .old");
    check(stillRunning(running), "the old program is still running, from the .old file");
    check(!swapInNewLauncher(exe, FileMover(moveReplacing), &e3) && contains(e3, "no downloaded"), "with no download there is nothing to switch to: " + e3);
    endProgram(running);
    removeLauncherLeftovers(exe);
    check(!exists(oldLauncherPath(exe)) && readAll(exe) == newBytes, "the next start deletes .old and keeps the exe");
}

// ---- the plan file and the elevated copy ----------------------------------------------------------

bool sameBytesOf(const std::wstring& a, const std::wstring& b) { return readAll(a) == readAll(b); }

void testPlanFile() {
    section("planfile");
    const std::wstring base = fresh("plan");
    PlanFile p;
    p.action = "install";
    p.dir = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Elite Dangerous\\Products\\elite-dangerous-odyssey-64";
    p.source = L"Steam";
    p.scope = "flat";
    p.keepSettings = false;
    p.removeSettings = true;
    p.restoreMirror = true;
    p.manifestSha = std::string(64, 'a');
    p.dir += L" \xe9";
    const std::wstring file = P(base, L"plan.txt");
    std::string err;
    check(writePlanFile(file, p, &err), "a plan file is written: " + err);
    PlanFile q;
    check(readPlanFile(file, &q, &err), "and read: " + err);
    check(q.action == p.action && q.dir == p.dir && q.source == p.source && q.scope == p.scope && q.keepSettings == p.keepSettings && q.removeSettings == p.removeSettings &&
              q.restoreMirror == p.restoreMirror && q.manifestSha == p.manifestSha,
          "every field round-trips, a non-ASCII path included");
    const std::string text = readAll(file);
    check(contains(text, "version=2\n") && contains(text, "restore_mirror=1\n") && !contains(text, "\r"), "the file is plain key=value lines with a version first: " + text.substr(0, 40));
    // No path the elevated copy writes to or installs from is in the file at all.
    for (const char* gone : {"payload_dir=", "cache_dir=", "mirror_dir=", "result="})
        check(!contains(text, gone), std::string("the file names no ") + gone);

    PlanFile bad;
    p.dir = L"C:\\game\nrm";
    check(!writePlanFile(file, p, &err) && contains(err, "line break"), "a value with a line break is not written: " + err);
    p.dir = L"C:\\game";
    auto reads = [&](const std::string& body) {
        writeAll(file, body);
        return readPlanFile(file, &bad, &err);
    };
    const std::string good = text;
    check(!reads(good + "extra=1\n") && contains(err, "extra"), "a key the reader does not know is refused: " + err);
    // The keys that once named paths are refused outright: an old or forged plan cannot steer the copy.
    for (const char* old : {"payload_dir=D:\\x\n", "result=C:\\Windows\\x.txt\n", "mirror_dir=C:\\x\n", "cache_dir=C:\\x\n"})
        check(!reads(good + old) && contains(err, "does not know"), std::string("a plan with ") + old + " is refused: " + err);
    check(!reads(good + "action=uninstall\n") && contains(err, "twice"), "a repeated key is refused: " + err);
    check(!reads("version=1\n" + good.substr(good.find('\n') + 1)) && contains(err, "version"), "another plan version is refused: " + err);
    {
        std::string s = good;
        s.replace(s.find("keep_settings=0"), 15, "keep_settings=7");
        check(!reads(s) && contains(err, "keep_settings"), "a boolean that is not 0 or 1 is refused: " + err);
        s = good;
        s.replace(s.find("scope=flat"), 10, "scope=nonsense");
        check(!reads(s) && contains(err, "scope"), "an unknown scope is refused: " + err);
        s = good;
        s.replace(s.find("manifest=") + 9, 64, "..\\..\\evil");
        check(!reads(s) && contains(err, "manifest"), "a manifest that is not a 64-character hash is refused: " + err);
        check(!reads("not a plan at all\n"), "a file that is not key=value is refused: " + err);
    }

    // Where the copy works its paths out from: <data root>\launcher\plan-<n>-<n>.txt, and nothing else.
    {
        const std::wstring root = fresh("plan-root");
        const std::wstring launcher = P(root, L"launcher");
        createDirectories(launcher);
        const std::wstring planAt = P(launcher, L"plan-12-345.txt");
        writeAll(planAt, "x");
        PlanPaths paths;
        check(planPathsFor(planAt, &paths, &err) && paths.dataRoot == root && paths.resultPath == P(launcher, L"result-12-345.txt") &&
                  paths.manifestsDir == P(launcher, L"manifests") && paths.cacheDir == P(root, L"payload"),
              "the result goes beside the plan, and the manifest and the cache are under the same root: " + err);
        writeAll(P(root, L"plan-12-345.txt"), "x");
        check(!planPathsFor(P(root, L"plan-12-345.txt"), &paths, &err) && contains(err, "launcher folder"), "a plan outside a launcher folder is refused: " + err);
        for (const wchar_t* leaf : {L"plan.txt", L"plan-12.txt", L"plan-1a-2.txt", L"result-12-345.txt", L"plan-12-345.txt.exe"}) {
            writeAll(P(launcher, leaf), "x");
            check(!planPathsFor(P(launcher, leaf), &paths, &err), "a plan file named " + narrowUtf8(leaf) + " is refused");
        }
        check(!planPathsFor(P(launcher, L"plan-1-2.txt"), &paths, &err), "a plan file that is not there is refused");
        // A junction standing in for the launcher folder is refused, though everything behind it is in order.
        const std::wstring real = fresh("plan-real");
        createDirectories(P(real, L"launcher"));
        writeAll(P(P(real, L"launcher"), L"plan-1-2.txt"), "x");
        const std::wstring linkRoot = fresh("plan-link");
        wchar_t systemDir[MAX_PATH]{};
        GetSystemDirectoryW(systemDir, MAX_PATH);
        HANDLE mklink = startProgram(P(systemDir, L"cmd.exe"), L"/c mklink /J \"" + P(linkRoot, L"launcher") + L"\" \"" + P(real, L"launcher") + L"\"");
        if (mklink) {
            WaitForSingleObject(mklink, 15000);
            CloseHandle(mklink);
        }
        const DWORD linkAttributes = GetFileAttributesW(P(linkRoot, L"launcher").c_str());
        check(linkAttributes != INVALID_FILE_ATTRIBUTES && (linkAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0, "the rig made a junction to test with");
        check(!planPathsFor(P(P(linkRoot, L"launcher"), L"plan-1-2.txt"), &paths, &err) && contains(err, "link"),
              "a plan reached through a junction is refused: " + err);
        check(planPathsFor(P(P(real, L"launcher"), L"plan-1-2.txt"), &paths, &err), "and the same plan reached directly is accepted: " + err);
        check(manifestShaOf(P(P(launcher, L"manifests"), widenUtf8(std::string(64, 'b')) + L".json")) == std::string(64, 'b') &&
                  manifestShaOf(L"C:\\x\\notahash.json").empty(),
              "manifestShaOf takes the hash from a stored manifest's name, and nothing else");
    }

    // Result files.
    ApplyOutcome o;
    o.status = ApplyOutcome::Status::Blocked;
    o.lines = {"first = with equals", "second"};
    const std::wstring result = P(base, L"result-file.txt");
    check(writeResultFile(result, o, &err), "a result is written");
    ApplyOutcome r;
    check(readResultFile(result, &r, &err) && r.status == ApplyOutcome::Status::Blocked && r.lines == o.lines, "and read back with its lines intact: " + err);
    for (ApplyOutcome::Status st : {ApplyOutcome::Status::Done, ApplyOutcome::Status::NothingToDo, ApplyOutcome::Status::Failed}) {
        o.status = st;
        writeResultFile(result, o, nullptr);
        check(readResultFile(result, &r, &err) && r.status == st, "each status round-trips");
    }
    writeAll(result, "status=maybe\n");
    check(!readResultFile(result, &r, &err), "an unknown status is refused");
    writeAll(result, "line=only a line\n");
    check(!readResultFile(result, &r, &err) && contains(err, "no status"), "a result with no status is refused: " + err);
    o.status = ApplyOutcome::Status::Failed;
    check(contains(outcomeReport(o), "did not finish") && contains(outcomeReport(o), "first = with equals"), "a failed outcome reads as one, with its lines");
    o.status = ApplyOutcome::Status::Done;
    check(contains(outcomeReport(o), "Done."), "a done outcome says Done");
}

// A game folder that is not Elite: describeDir accepts it (the exe is there), the planner will not.
std::wstring fakeGameFolder(const std::string& name) {
    const std::wstring dir = fresh(name);
    writeAll(P(dir, L"EliteDangerous64.exe"), "not an executable");
    return dir;
}

void testElevatedApply() {
    section("elevated");
    const Rel a = loadRel("relA");
    const std::wstring base = fresh("elev");
    ScriptedHttp http;
    http.replies[latestUrl()] = ok(releaseJson(a), "\"etag-A\"");
    serveRelease(&http, a);
    Updater up(http, settingsFor(base));
    std::string err;
    const UpdateCheck c = up.check();
    check(c.outcome == UpdateOutcome::Available && adopt(&up, c, &err), "release A is adopted for the elevated copy to read: " + err);
    const LauncherRoots roots = launcherRoots(base);
    const std::wstring manifestPath = P(P(roots.launcherDir(), L"manifests"), widenUtf8(sha(a.manifestBytes)) + L".json");
    const std::wstring game = fakeGameFolder("elev-game");

    PlanFile plan;
    plan.action = "install";
    plan.dir = game;
    plan.source = L"Folder you chose";
    plan.manifestSha = manifestShaOf(manifestPath);
    check(plan.manifestSha == sha(a.manifestBytes), "the plan names the adopted manifest by its hash");
    // Where the launcher puts a plan, and so where the copy puts its result and finds everything else.
    const std::wstring planPath = P(roots.launcherDir(), L"plan-7-8.txt");
    const std::wstring resultPath = P(roots.launcherDir(), L"result-7-8.txt");
    GameInstall described;
    describeDir(game, plan.source, &described);
    const std::wstring mirrorDir = mirrorDirFor(described, base);
    auto run = [&](const PlanFile& p, const char* const* keys, ApplyOutcome* out) {
        DeleteFileW(resultPath.c_str());
        std::string e;
        writePlanFile(planPath, p, &e);
        const int code = applyPlanFile(planPath, keys);
        std::string readError;
        const bool have = readResultFile(resultPath, out, &readError);
        return std::make_pair(code, have);
    };

    // The copy checks the signature for itself: the other key is not the manifest's.
    {
        const char* other[2] = {g_otherKey.c_str(), nullptr};
        ApplyOutcome out;
        const auto r = run(plan, other, &out);
        check(r.second && r.first == 1 && out.status == ApplyOutcome::Status::Failed && !out.lines.empty() && contains(out.lines[0], "downloaded files cannot be used"),
              "a manifest the copy's keys do not verify is refused: " + (out.lines.empty() ? std::string("(no line)") : out.lines[0]));
        check(!exists(P(game, L"d3d11.dll")) && !exists(P(game, L"edvr_install")), "and the game folder was not touched");
    }
    // A cached file that no longer matches its hash is refused where it is read.
    {
        PayloadCache cache(roots.cacheDir());
        const std::wstring victim = cache.pathFor(a.manifest.files[0].sha256);
        const std::string good = readAll(victim);
        std::string tampered = good;
        tampered[0] = static_cast<char>(tampered[0] ^ 1);
        writeAll(victim, tampered);
        ApplyOutcome out;
        const auto r = run(plan, g_testKeys, &out);
        check(r.second && r.first == 1 && out.status == ApplyOutcome::Status::Failed && contains(out.lines.empty() ? "" : out.lines[0], "hash"),
              "a cache file changed since the window checked it is refused by the copy: " + (out.lines.empty() ? std::string("(no line)") : out.lines[0]));
        check(!exists(P(game, L"d3d11.dll")), "nothing was written");
        writeAll(victim, good);
    }
    // A plan naming a payload that is not there, and a folder that is not a game.
    {
        PlanFile p2 = plan;
        p2.manifestSha.clear();
        ApplyOutcome out;
        auto r = run(p2, g_testKeys, &out);
        check(r.second && out.status == ApplyOutcome::Status::Failed && contains(out.lines[0], "no payload"), "an install with no payload is refused: " + out.lines[0]);
        p2 = plan;
        p2.dir = fresh("elev-empty");
        r = run(p2, g_testKeys, &out);
        check(r.second && out.status == ApplyOutcome::Status::Failed && contains(out.lines[0], "EliteDangerous64.exe"), "a folder with no game in it is refused: " + out.lines[0]);
        p2 = plan;
        p2.action = "frobnicate";
        r = run(p2, g_testKeys, &out);
        check(r.second && out.status == ApplyOutcome::Status::Failed && contains(out.lines[0], "action"), "an unknown action is refused: " + out.lines[0]);
    }
    // The good case reaches the planner, which declines an executable that is not Elite: Blocked, and still nothing written.
    {
        ApplyOutcome out;
        const auto r = run(plan, g_testKeys, &out);
        check(r.second && r.first == 1 && out.status == ApplyOutcome::Status::Blocked && !out.lines.empty(), "a verified payload gets as far as the planner, which blocks a folder whose exe is not Elite");
        check(!exists(P(game, L"d3d11.dll")) && !exists(P(game, L"edvr_install")) && !exists(mirrorDir), "and the folder holds nothing new");
        std::string said;
        for (const std::string& l : out.lines) said += l + " | ";
        check(contains(outcomeReport(out), "blocked"), "the report says the plan is blocked: " + said);
    }
    // An unreadable plan has nowhere to report, and says so by its exit code.
    writeAll(planPath, "garbage");
    DeleteFileW(resultPath.c_str());
    check(applyPlanFile(planPath, g_testKeys) == 2 && !exists(resultPath), "an unreadable plan exits 2 and writes no result");
    // A good plan outside a launcher folder is not run, and writes no result anywhere.
    {
        const std::wstring stray = P(base, L"plan-7-8.txt");
        std::string e;
        writePlanFile(stray, plan, &e);
        check(applyPlanFile(stray, g_testKeys) == 2 && !exists(P(base, L"result-7-8.txt")) && !exists(resultPath) && !exists(P(game, L"edvr_install")),
              "a plan outside a launcher folder exits 2, writes no result and touches no game folder");
    }
    // The cases above leave their own plan in the launcher folder; applyElevated's leftover check below starts clean.
    DeleteFileW(planPath.c_str());
    DeleteFileW(resultPath.c_str());

    // The unelevated half, with this rig standing in for the elevated copy (started as itself, unelevated).
    {
        ElevatedRunner viaSelf = [](const std::wstring& exePath, const std::wstring& arguments) {
            ElevatedLaunch l;
            HANDLE h = startProgram(exePath, arguments + L" --key " + widenUtf8(g_testKey));
            if (!h) {
                l.error = "no start";
                return l;
            }
            l.started = true;
            WaitForSingleObject(h, 30000);
            DWORD code = 99;
            GetExitCodeProcess(h, &code);
            l.exitCode = code;
            CloseHandle(h);
            return l;
        };
        const std::wstring launcherDir = roots.launcherDir();
        const ElevatedResult result = applyElevated(g_self, launcherDir, plan, viaSelf);
        check(!result.declined && result.error.empty() && result.outcome.status == ApplyOutcome::Status::Blocked && !result.outcome.lines.empty(),
              "applyElevated ran the copy and read its result: " + result.error);
        bool leftovers = false;
        for (const std::string& n : names(launcherDir)) leftovers = leftovers || n.compare(0, 5, "plan-") == 0 || n.compare(0, 7, "result-") == 0;
        check(!leftovers, "and deleted the plan and the result afterwards");

        ElevatedRunner declined = [](const std::wstring&, const std::wstring&) {
            ElevatedLaunch l;
            l.declined = true;
            l.error = "the permission prompt was declined";
            return l;
        };
        const ElevatedResult no = applyElevated(g_self, launcherDir, plan, declined);
        check(no.declined && no.outcome.lines.empty(), "a declined prompt is reported as declined, with no outcome");
        bool leftovers2 = false;
        for (const std::string& n : names(launcherDir)) leftovers2 = leftovers2 || n.compare(0, 5, "plan-") == 0;
        check(!leftovers2 && !exists(P(game, L"d3d11.dll")), "and leaves no plan behind and the game folder alone");

        ElevatedRunner silent = [](const std::wstring&, const std::wstring&) {
            ElevatedLaunch l;
            l.started = true;
            l.exitCode = 5;
            return l;
        };
        const ElevatedResult mute = applyElevated(g_self, launcherDir, plan, silent);
        check(!mute.declined && contains(mute.error, "without reporting") && mute.outcome.status == ApplyOutcome::Status::Failed, "a copy that exits without a result is a failure that says so: " + mute.error);
    }
}

// ---- routes and the watcher -----------------------------------------------------------------------

struct Recorder {
    std::vector<std::wstring> uris;
    struct Spawn {
        std::wstring exe, args, dir;
    };
    std::vector<Spawn> spawns;
    RouteOps ops() {
        RouteOps o;
        o.openUri = [this](const std::wstring& uri) {
            uris.push_back(uri);
            return true;
        };
        o.spawn = [this](const std::wstring& exe, const std::wstring& args, const std::wstring& dir) {
            spawns.push_back({exe, args, dir});
            return true;
        };
        return o;
    }
};

void testRoute() {
    section("route");
    const std::wstring tree = fresh("route");
    const std::wstring frontier = P(tree, L"Frontier");
    const std::wstring products = P(frontier, L"Products\\elite-dangerous-odyssey-64");
    writeAll(P(products, L"EliteDangerous64.exe"), "x");
    writeAll(P(frontier, L"EDLaunch\\EDLaunch.exe"), "x");

    const LaunchRoute steam = routeFor(L"Steam", products, true);
    check(steam.kind == RouteKind::Steam && steam.uri == L"steam://rungameid/359320" && !steam.carriesMode, "Steam: the address is steam://rungameid/359320, and it carries no mode");
    const LaunchRoute epic = routeFor(L"Epic", products, false);
    check(epic.kind == RouteKind::Epic && epic.uri == L"com.epicgames.launcher://apps/9c203b6ed35846e8a4a9ff1e314f6593?action=launch&silent=true" && !epic.carriesMode,
          "Epic: the address is the launcher URI for app 9c203b6e..., launch and silent");
    const LaunchRoute vr = routeFor(L"Frontier launcher", products, true);
    check(vr.kind == RouteKind::Frontier && vr.exe == P(frontier, L"EDLaunch\\EDLaunch.exe") && vr.args == L"/autorun /autoquit /edo /vr" && vr.carriesMode && vr.workDir == P(frontier, L"EDLaunch"),
          "Frontier VR: EDLaunch.exe /autorun /autoquit /edo /vr, from the EDLaunch folder found above Products");
    const LaunchRoute flat = routeFor(L"Frontier launcher", products, false);
    check(flat.args == L"/autorun /autoquit /edo /novr", "Frontier flat: the same with /novr");
    const LaunchRoute chosen = routeFor(L"Folder you chose", products, true);
    check(chosen.kind == RouteKind::Frontier && chosen.exe == vr.exe, "a folder the user picked walks up to an EDLaunch.exe");
    writeAll(P(tree, L"Steam\\steamapps\\common\\Elite Dangerous\\EDLaunch.exe"), "x");
    const LaunchRoute steamWithEd = routeFor(L"Steam", P(tree, L"Steam\\steamapps\\common\\Elite Dangerous\\Products\\elite-dangerous-odyssey-64"), true);
    check(steamWithEd.kind == RouteKind::Steam && steamWithEd.exe.empty(), "a Steam install with an EDLaunch.exe beside it still goes through Steam");
    check(findEdLaunch(P(tree, L"Steam\\steamapps\\common\\Elite Dangerous\\Products\\elite-dangerous-odyssey-64")) == P(tree, L"Steam\\steamapps\\common\\Elite Dangerous\\EDLaunch.exe"), "findEdLaunch finds the one at the top of a Steam install");
    const std::wstring lone = P(tree, L"Games\\Elite");
    writeAll(P(lone, L"EliteDangerous64.exe"), "x");
    const LaunchRoute none = routeFor(L"Folder you chose", lone, true);
    check(none.kind == RouteKind::None && !none.why.empty() && contains(none.why, "EDLaunch.exe"), "a folder with no EDLaunch.exe above it has no route, and says why: " + none.why);

    // The Frontier store's own layout: the game under AppData, EDLaunch under Program Files, and
    // nothing above the game folder that leads to it.
    const std::wstring apart = P(tree, L"AppData\\Frontier_Developments\\Products\\elite-dangerous-odyssey-64");
    writeAll(P(apart, L"EliteDangerous64.exe"), "x");
    const std::wstring programFiles = P(tree, L"Program Files (x86)\\Frontier");
    writeAll(P(programFiles, L"EDLaunch\\EDLaunch.exe"), "x");
    check(findEdLaunch(apart).empty(), "the store's game folder has no EDLaunch.exe above it, so walking up cannot be enough");
    const std::vector<std::wstring> storeRoots = {L"", P(tree, L"NotInstalledHere"), programFiles};
    const std::vector<std::wstring> noRoots;
    const LaunchRoute apartRoute = routeFor(L"Frontier launcher", apart, true, &storeRoots);
    check(apartRoute.kind == RouteKind::Frontier && apartRoute.exe == P(programFiles, L"EDLaunch\\EDLaunch.exe") &&
              apartRoute.workDir == P(programFiles, L"EDLaunch") && apartRoute.args == L"/autorun /autoquit /edo /vr" && apartRoute.carriesMode,
          "Frontier with the game under AppData finds EDLaunch in the launcher's install folder");
    check(routeFor(L"Frontier launcher", apart, true, &noRoots).kind == RouteKind::None, "and has no route when no install folder holds one");
    check(routeFor(L"Folder you chose", apart, true, &storeRoots).kind == RouteKind::None,
          "a picked folder does not borrow the store's EDLaunch, which would start the store's own copy");
    wchar_t programFilesEnv[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramFiles", programFilesEnv, MAX_PATH)) {
        const std::wstring expected = P(programFilesEnv, L"Frontier");
        const std::vector<std::wstring> machineRoots = frontierLauncherRoots();
        check(std::find(machineRoots.begin(), machineRoots.end(), expected) != machineRoots.end(),
              "frontierLauncherRoots holds the default install folder under Program Files");
    }

    // Starting records and never opens anything.
    Recorder rec;
    std::string err;
    check(startRoute(steam, rec.ops(), false, &err) && rec.uris.size() == 1 && rec.uris[0] == L"steam://rungameid/359320" && rec.spawns.empty(), "Steam is started by opening exactly the Steam address, and spawns nothing: " + err);
    check(startRoute(epic, rec.ops(), false, &err) && rec.uris.size() == 2 && rec.uris[1] == epic.uri && rec.spawns.empty(), "Epic is started by opening exactly the Epic address");
    check(startRoute(vr, rec.ops(), false, &err) && rec.spawns.size() == 1 && rec.spawns[0].exe == vr.exe && rec.spawns[0].args == vr.args && rec.spawns[0].dir == vr.workDir && rec.uris.size() == 2,
          "Frontier is started by spawning EDLaunch.exe with its flags, and opens no address");
    Recorder refused;
    check(!startRoute(steam, refused.ops(), true, &err) && contains(err, "administrator") && refused.uris.empty() && refused.spawns.empty(), "an elevated launcher refuses to start Steam: " + err);
    check(!startRoute(epic, refused.ops(), true, &err) && !startRoute(vr, refused.ops(), true, &err) && refused.uris.empty() && refused.spawns.empty(), "and Epic and Frontier, with nothing opened or spawned");
    check(!startRoute(none, refused.ops(), false, &err) && contains(err, "EDLaunch.exe") && refused.uris.empty() && refused.spawns.empty(), "a route with none starts nothing: " + err);
    RouteOps failing;
    failing.openUri = [](const std::wstring&) { return false; };
    failing.spawn = [](const std::wstring&, const std::wstring&, const std::wstring&) { return false; };
    check(!startRoute(steam, failing, false, &err) && !startRoute(vr, failing, false, &err), "an address Windows will not open, or a program that will not start, is a failure");
    const RouteOps real = realRouteOps();
    check(real.openUri && real.spawn, "the real ops exist (and are not called by this rig)");
}

struct Events {
    std::vector<WatchEvent> list;
    bool has(WatchEvent e) const {
        for (WatchEvent x : list) if (x == e) return true;
        return false;
    }
};

void testWatcher() {
    section("watcher");
    endFakes();
    const std::wstring dir = fresh("watch");
    const std::wstring ed = P(g_fakes, kFakeEd), game = P(g_fakes, kFakeGame);
    check(exists(ed) && exists(game), "the fake programs were built");
    WatchNames names;
    names.game = kFakeGame;
    names.edlaunch = kFakeEd;
    WatchTiming timing;
    timing.pollMs = 40;
    timing.startupWaitMs = 1500;
    timing.closeWaitMs = 800;

    // The process list helper itself.
    {
        HANDLE g = startProgram(game, L"--run-ms 3000");
        Sleep(150);
        std::vector<unsigned long> pids;
        check(pidsNamed(kFakeGame, &pids) && pids.size() == 1 && pids[0] == GetProcessId(g), "pidsNamed finds the process by its exe name");
        check(pidsNamed(L"ED_UP_FAKE_GAME.EXE", &pids) && pids.size() == 1, "and the match ignores case");
        check(pidsNamed(kFakeEd, &pids) && pids.empty(), "and finds nothing for a name nobody runs");
        endProgram(g);
    }

    // The game runs and exits; EDLaunch is asked to close, and goes.
    {
        const std::wstring log = P(dir, L"a-ed.log");
        HANDLE e = startProgram(ed, L"--obey-close --log \"" + log + L"\"");
        HANDLE g = startProgram(game, L"--run-ms 700");
        Events ev;
        const WatchOutcome out = watchLaunch(names, timing, nullptr, [&](WatchEvent x) { ev.list.push_back(x); });
        check(out.kind == WatchOutcome::Kind::GameRan && out.askedEdlaunchToClose && out.edlaunchClosed, "game ran, EDLaunch was asked to close and left");
        check(ev.has(WatchEvent::GameSeen) && ev.has(WatchEvent::GameExited) && ev.list.size() == 2 && ev.list[0] == WatchEvent::GameSeen, "the events were: game seen, then game exited");
        check(contains(logText(log), "WM_CLOSE") && contains(logText(log), "exiting"), "the fake EDLaunch's own log shows the WM_CLOSE arriving: " + logText(log));
        check(WaitForSingleObject(e, 3000) == WAIT_OBJECT_0, "and the process is gone");
        CloseHandle(e);
        CloseHandle(g);
    }
    // EDLaunch leaves by itself and the game never starts.
    {
        const std::wstring log = P(dir, L"b-ed.log");
        HANDLE e = startProgram(ed, L"--run-ms 500 --log \"" + log + L"\"");
        Events ev;
        const WatchOutcome out = watchLaunch(names, timing, nullptr, [&](WatchEvent x) { ev.list.push_back(x); });
        check(out.kind == WatchOutcome::Kind::EdlaunchLeft && !out.askedEdlaunchToClose, "EDLaunch exited with no game: the watcher stops, and did not ask for anything to close");
        check(ev.has(WatchEvent::WaitingForGame) && !ev.has(WatchEvent::GameSeen), "having said it was waiting, and never having seen the game");
        check(!contains(logText(log), "WM_CLOSE"), "no WM_CLOSE reached it");
        CloseHandle(e);
    }
    // EDLaunch stays with no game: the watcher keeps waiting and leaves it alone.
    {
        const std::wstring log = P(dir, L"c-ed.log");
        HANDLE e = startProgram(ed, L"--log \"" + log + L"\"");
        std::atomic<bool> cancel{false};
        std::thread stopper([&]() {
            Sleep(900);
            cancel = true;
        });
        Events ev;
        const DWORD began = GetTickCount();
        const WatchOutcome out = watchLaunch(names, timing, &cancel, [&](WatchEvent x) { ev.list.push_back(x); });
        const DWORD took = GetTickCount() - began;
        stopper.join();
        check(out.kind == WatchOutcome::Kind::Cancelled && took >= 800, "with EDLaunch up and no game the watcher kept waiting until it was cancelled (" + std::to_string(took) + " ms)");
        check(ev.has(WatchEvent::WaitingForGame), "and said it was waiting");
        check(stillRunning(e) && !contains(logText(log), "WM_CLOSE"), "EDLaunch is still running and was never asked to close");
        endProgram(e);
    }
    // Nothing appears at all.
    {
        WatchTiming quick = timing;
        quick.startupWaitMs = 400;
        const DWORD began = GetTickCount();
        const WatchOutcome out = watchLaunch(names, quick, nullptr, nullptr);
        const DWORD took = GetTickCount() - began;
        // The allowance counts the watcher's sleeps, not its process-list reads, and on a loaded CI runner
        // those reads stretched this 400 ms allowance to 4.2 s. So the upper bound only shows that the short
        // allowance was used rather than the two-minute default; the lower bound shows it was waited out.
        check(out.kind == WatchOutcome::Kind::NeverStarted && took >= 350 && took < 60000, "neither EDLaunch nor the game: it waits out the allowance and says so (" + std::to_string(took) + " ms)");
    }
    // The game runs and exits, and EDLaunch ignores the request: it is left running.
    {
        const std::wstring log = P(dir, L"e-ed.log");
        HANDLE e = startProgram(ed, L"--log \"" + log + L"\"");
        HANDLE g = startProgram(game, L"--run-ms 500");
        WatchTiming shortClose = timing;
        shortClose.closeWaitMs = 500;
        const WatchOutcome out = watchLaunch(names, shortClose, nullptr, nullptr);
        check(out.kind == WatchOutcome::Kind::GameRan && out.askedEdlaunchToClose && !out.edlaunchClosed, "an EDLaunch that stays after being asked is reported as still open");
        check(contains(logText(log), "WM_CLOSE") && stillRunning(e), "it received the request and was not ended by the watcher");
        endProgram(e);
        CloseHandle(g);
    }
    // Cancelled while the game runs: everything is left running.
    {
        const std::wstring log = P(dir, L"f-ed.log");
        HANDLE e = startProgram(ed, L"--obey-close --log \"" + log + L"\"");
        HANDLE g = startProgram(game, L"--run-ms 20000");
        std::atomic<bool> cancel{false};
        std::thread stopper([&]() {
            Sleep(500);
            cancel = true;
        });
        Events ev;
        const WatchOutcome out = watchLaunch(names, timing, &cancel, [&](WatchEvent x) { ev.list.push_back(x); });
        stopper.join();
        check(out.kind == WatchOutcome::Kind::Cancelled && ev.has(WatchEvent::GameSeen) && !ev.has(WatchEvent::GameExited), "cancelled with the game running: it had seen the game and never saw it end");
        check(stillRunning(e) && stillRunning(g) && !contains(logText(log), "WM_CLOSE"), "the game and EDLaunch are both still running, and nothing was posted");
        endProgram(e);
        endProgram(g);
    }
    // A game that is already running when Play is pressed is seen at once.
    {
        HANDLE g = startProgram(game, L"--run-ms 400");
        Events ev;
        const WatchOutcome out = watchLaunch(names, timing, nullptr, [&](WatchEvent x) { ev.list.push_back(x); });
        check(out.kind == WatchOutcome::Kind::GameRan && !out.askedEdlaunchToClose && out.edlaunchClosed, "a game with no EDLaunch beside it: seen, ended, and nothing to close");
        CloseHandle(g);
    }
    endFakes();
}

// ---- the session: the window's behaviour without a window -----------------------------------------------

class Forward : public HttpClient {
public:
    explicit Forward(ScriptedHttp* target) : target_(target) {}
    HttpResponse get(const HttpRequest& request) override {
        static std::mutex lock;
        std::lock_guard<std::mutex> guard(lock);
        return target_->get(request);
    }

private:
    ScriptedHttp* target_;
};

struct Ui {
    std::mutex m;
    std::vector<std::function<void()>> queue;
    int changed = 0, switched = 0, closed = 0;
    std::vector<bool> aside;
    std::vector<std::wstring> restarted;

    void post(std::function<void()> f) {
        std::lock_guard<std::mutex> l(m);
        queue.push_back(std::move(f));
    }
    // The window's thread: runs what the workers posted until `done` or the time runs out.
    bool pump(const std::function<bool()>& done, unsigned timeoutMs = 20000) {
        const DWORD end = GetTickCount() + timeoutMs;
        for (;;) {
            std::vector<std::function<void()>> jobs;
            {
                std::lock_guard<std::mutex> l(m);
                jobs.swap(queue);
            }
            for (auto& j : jobs) j();
            if (done()) return true;
            if (static_cast<int>(GetTickCount() - end) > 0) return false;
            Sleep(5);
        }
    }
};

SessionEnv envFor(Ui* ui, ScriptedHttp* http, const std::wstring& base, bool canonical, const std::wstring& selfExe) {
    SessionEnv e;
    e.roots.dataRoot = base;
    e.roots.canonical = canonical;
    e.selfExe = selfExe;
    e.keys = g_testKeys;
    e.apiBase = kApi;
    e.makeHttp = [http]() { return std::unique_ptr<HttpClient>(new Forward(http)); };
    e.post = [ui](std::function<void()> f) { ui->post(std::move(f)); };
    e.changed = [ui]() { ++ui->changed; };
    e.payloadSwitched = [ui]() { ++ui->switched; };
    e.closeWindow = [ui]() { ++ui->closed; };
    e.windowAside = [ui](bool aside) { ui->aside.push_back(aside); };
    e.startNewLauncher = [ui](const std::wstring& exe) {
        ui->restarted.push_back(exe);
        return true;
    };
    return e;
}

void testSession() {
    section("session");
    const Rel a = loadRel("relA"), b = loadRel("relB"), minLauncher = loadRel("relMinLauncher");

    // A first start with a release out: the payload is none, the check offers A, and nothing downloads until Update.
    {
        const std::wstring base = fresh("sess-a");
        const std::wstring self = P(base, L"self.exe");
        writeAll(self, "the running launcher");
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(a), "\"etag-A\"");
        serveRelease(&http, a);
        Ui ui;
        {
            LauncherSession session(envFor(&ui, &http, base, true, self));
            check(session.payloadState() == PayloadState::Loading && session.checkState() == CheckState::NotYet, "before start the payload is loading and nothing has been asked");
            session.start();
            check(ui.pump([&]() { return session.payloadState() != PayloadState::Loading && session.checkState() == CheckState::Available; }), "the payload resolves and the check finishes");
            check(session.payloadState() == PayloadState::None && session.payload().firstRun, "with nothing adopted the payload is none, quietly");
            check(session.updateLine() == "ED-UP 1.0.0 is available." && !session.updating(), "the line says an update is available: " + session.updateLine());
            check(http.count("ed-up-core") == 0 && http.count("ed-up-dlss") == 0 && http.count("ed-up-launcher") == 0, "nothing was downloaded before Update was pressed");
            check(names(settingsFor(base).cacheDir).empty(), "and the cache is empty");
            check(ui.changed > 0 && ui.switched == 0, "the window was told about each change and the payload has not switched");

            session.beginUpdate();
            check(session.updating(), "Update marks the session busy at once");
            check(ui.pump([&]() { return ui.switched == 1; }), "the update fills the cache, adopts the manifest and switches the payload");
            check(!session.updating() && session.payloadState() == PayloadState::Ready && session.payload().version == "v1.0.0", "the payload is ready, at v1.0.0: " + session.updateLine());
            const void* data = nullptr;
            size_t size = 0;
            check(payloadItem("native_graphics", &data, &size) && std::string(static_cast<const char*>(data), size) == "graphics one", "payloadItem serves the update's files");
            LauncherState state;
            state.load(P(settingsFor(base).launcherDir, L"launcher.ini"), nullptr);
            check(state.currentManifest() == sha(a.manifestBytes) && state.highestIssued() == 1794000000, "launcher.ini names the adopted manifest");
            check(readAll(self) == "the running launcher" && ui.closed == 0 && ui.restarted.empty(), "release A has no launcher: the exe was not touched and nothing restarted");
            check(session.updateLine() == "ED-UP 1.0.0 is ready to install.", "the line says the update is ready: " + session.updateLine());
        }
        resetPayloadFiles();
    }

    // A failed check is one quiet line, and Update does nothing.
    {
        const std::wstring base = fresh("sess-fail");
        ScriptedHttp http;
        Reply down;
        down.transportOk = false;
        down.error = "no route";
        http.replies[latestUrl()] = down;
        Ui ui;
        LauncherSession session(envFor(&ui, &http, base, true, P(base, L"self.exe")));
        session.start();
        check(ui.pump([&]() { return session.checkState() == CheckState::Skipped; }), "an unreachable GitHub ends the check as skipped");
        check(contains(session.updateLine(), "could not be reached") && session.updateLine().back() == '.', "with one sentence: " + session.updateLine());
        // The payload loads on its own worker, which can finish after the check under load.
        check(ui.pump([&]() { return session.payloadState() != PayloadState::Loading; }), "the payload load finishes");
        session.beginUpdate();
        check(!session.updating() && http.count("ed-up-core") == 0, "Update does nothing when there is nothing to update");
        check(session.payloadState() == PayloadState::None, "and the payload stays none");
    }

    // Release candidates: saved, and asked again against the list.
    {
        const std::wstring base = fresh("sess-rc");
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(a), "\"etag-A\"");
        http.replies[listUrl()] = ok("[" + releaseJson(b) + "]", "\"etag-L\"");
        serveRelease(&http, a);
        serveRelease(&http, b);
        Ui ui;
        LauncherSession session(envFor(&ui, &http, base, false, P(base, L"self.exe")));
        session.start();
        check(ui.pump([&]() { return session.checkState() == CheckState::Available; }) && !session.includeReleaseCandidates() && session.check().tag == "v1.0.0", "stable reads /releases/latest");
        session.setIncludeReleaseCandidates(true);
        check(session.includeReleaseCandidates() && ui.pump([&]() { return session.checkState() == CheckState::Available && session.check().tag == "v1.1.0"; }), "ticking the box re-checks against the release list");
        LauncherState state;
        state.load(P(settingsFor(base).launcherDir, L"launcher.ini"), nullptr);
        check(state.channel() == "rc", "and the choice is saved in launcher.ini");
        LauncherSession again(envFor(&ui, &http, base, false, P(base, L"self.exe")));
        again.start();
        check(again.includeReleaseCandidates(), "a later start begins on the saved channel");
        ui.pump([&]() { return again.checkState() == CheckState::Available; });
        resetPayloadFiles();
    }

    // Self-update: the launcher first, then a restart that carries on without being asked again.
    {
        const std::wstring base = fresh("sess-self");
        const std::wstring exe = P(P(base, L"launcher"), L"ed-up-launcher.exe");
        writeAll(exe, "the old launcher");
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(b), "\"etag-B\"");
        serveRelease(&http, b);
        Ui ui;
        {
            LauncherSession session(envFor(&ui, &http, base, true, exe));
            session.start();
            check(ui.pump([&]() { return session.checkState() == CheckState::Available; }) && session.check().launcherNeedsUpdate, "B offers a newer launcher");
            session.beginUpdate();
            check(ui.pump([&]() { return ui.closed == 1; }), "the update replaces the launcher, starts the new one and closes this window");
            check(readAll(exe) == readAll(P(b.dir, L"ed-up-launcher.exe")) && readAll(oldLauncherPath(exe)) == "the old launcher", "the new launcher is at its path and the old one at .old");
            check(ui.restarted.size() == 1 && ui.restarted[0] == exe, "the replacement was started from the same path");
            check(http.count("ed-up-core") == 0 && ui.switched == 0, "and the payload download waits for the restarted launcher");
        }
        // The restarted launcher: --updated, so it continues into the update by itself.
        {
            Ui ui2;
            SessionEnv env = envFor(&ui2, &http, base, true, exe);
            env.updatedFlag = true;
            LauncherSession session(env);
            session.start();
            check(ui2.pump([&]() { return ui2.switched == 1; }), "after the restart the update continues with no further press");
            check(session.payloadState() == PayloadState::Ready && ui2.closed == 0 && ui2.restarted.empty(), "and does not replace the launcher a second time");
        }
        resetPayloadFiles();
    }
    // A launcher that is still not the manifest's after the restart does not loop.
    {
        const std::wstring base = fresh("sess-loop");
        const std::wstring exe = P(P(base, L"launcher"), L"ed-up-launcher.exe");
        writeAll(exe, "a launcher that differs");
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(b), "\"etag-B\"");
        serveRelease(&http, b);
        Ui ui;
        SessionEnv env = envFor(&ui, &http, base, true, exe);
        env.updatedFlag = true;
        LauncherSession session(env);
        session.start();
        check(ui.pump([&]() { return ui.switched == 1; }), "an --updated launcher that still differs carries on with the payload");
        check(readAll(exe) == "a launcher that differs" && ui.restarted.empty() && ui.closed == 0, "and does not swap or restart again");
        resetPayloadFiles();
    }
    // A download that does not match the signed manifest.
    {
        const std::wstring base = fresh("sess-bad");
        const std::wstring exe = P(P(base, L"launcher"), L"ed-up-launcher.exe");
        writeAll(exe, "the old launcher");
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(b), "\"etag-B\"");
        serveRelease(&http, b);
        std::string wrong = readAll(P(b.dir, L"ed-up-launcher.exe"));
        wrong[wrong.size() - 1] = static_cast<char>(wrong[wrong.size() - 1] ^ 1);
        http.replies[std::string(kDownloads) + "relB/ed-up-launcher.exe"].body = wrong;
        Ui ui;
        LauncherSession session(envFor(&ui, &http, base, true, exe));
        session.start();
        ui.pump([&]() { return session.checkState() == CheckState::Available; });
        session.beginUpdate();
        check(ui.pump([&]() { return !session.updating(); }) && contains(session.updateLine(), "Could not update the launcher"), "a launcher that does not match the manifest fails the update with a line: " + session.updateLine());
        check(readAll(exe) == "the old launcher" && !exists(newLauncherPath(exe)) && ui.closed == 0 && ui.restarted.empty(), "and the exe is unchanged, no .new is left, and nothing restarted");
    }
    // A trial run (--data-root) never replaces itself; a launcher too old for the release says so.
    {
        const std::wstring base = fresh("sess-trial");
        const std::wstring exe = P(base, L"trial.exe");
        writeAll(exe, "the trial launcher");
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(b), "\"etag-B\"");
        serveRelease(&http, b);
        Ui ui;
        {
            LauncherSession session(envFor(&ui, &http, base, false, exe));
            session.start();
            ui.pump([&]() { return session.checkState() == CheckState::Available; });
            session.beginUpdate();
            check(ui.pump([&]() { return ui.switched == 1; }) && readAll(exe) == "the trial launcher" && ui.restarted.empty(), "a trial run takes the payload and leaves its exe alone");
        }
        resetPayloadFiles();
        const std::wstring base2 = fresh("sess-old");
        ScriptedHttp http2;
        http2.replies[latestUrl()] = ok(releaseJson(minLauncher), "\"etag-M\"");
        serveRelease(&http2, minLauncher);
        Ui ui2;
        LauncherSession old(envFor(&ui2, &http2, base2, false, P(base2, L"trial.exe")));
        old.start();
        ui2.pump([&]() { return old.checkState() == CheckState::Available; });
        old.beginUpdate();
        check(!old.updating() && contains(old.updateLine(), "too old") && http2.count("ed-up-core") == 0, "a trial launcher older than min_launcher does not download: " + old.updateLine());
    }

    // Play: the Frontier route is started, remembered, watched, and the window steps aside and back.
    {
        endFakes();
        const std::wstring base = fresh("sess-play");
        const std::wstring tree = P(base, L"Frontier");
        const std::wstring products = P(tree, L"Products\\elite-dangerous-odyssey-64");
        writeAll(P(products, L"EliteDangerous64.exe"), "x");
        writeAll(P(tree, L"EDLaunch\\EDLaunch.exe"), "x");
        ScriptedHttp http;
        Reply down;
        down.transportOk = false;
        http.replies[latestUrl()] = down;
        Ui ui;
        LauncherSession session(envFor(&ui, &http, base, false, P(base, L"self.exe")));
        std::vector<HANDLE> started;
        RouteOps ops;
        std::vector<std::wstring> spawned;
        ops.spawn = [&](const std::wstring& exe, const std::wstring& args, const std::wstring&) {
            spawned.push_back(exe + L" " + args);
            started.push_back(startProgram(P(g_fakes, kFakeEd), L"--obey-close"));
            started.push_back(startProgram(P(g_fakes, kFakeGame), L"--run-ms 700"));
            return true;
        };
        ops.openUri = [&](const std::wstring& uri) {
            spawned.push_back(uri);
            return true;
        };
        WatchNames names;
        names.game = kFakeGame;
        names.edlaunch = kFakeEd;
        WatchTiming timing;
        timing.pollMs = 40;
        timing.startupWaitMs = 1500;
        timing.closeWaitMs = 800;
        session.beginPlay(L"Frontier launcher", products, false, false, ops, names, timing);
        check(session.playing() && spawned.size() == 1 && contains(narrowUtf8(spawned[0]), "/autorun /autoquit /edo /novr"), "Play flat starts EDLaunch with /novr");
        check(ui.pump([&]() { return !session.playing(); }), "the watcher finishes when the game has been and gone");
        check(ui.aside.size() == 2 && ui.aside[0] == true && ui.aside[1] == false, "the window stepped aside when the game started and came back when it ended");
        check(contains(session.playLine(), "Elite has closed"), "the line says so: " + session.playLine());
        LauncherState state;
        state.load(P(settingsFor(base).launcherDir, L"launcher.ini"), nullptr);
        check(state.mode(products) == "flat", "the flat choice is remembered for that folder");
        check(state.lastInstall() == products, "and that folder is the install the window opens on next");
        for (HANDLE h : started) endProgram(h);

        // Steam: the address only, and no mode remembered.
        session.beginPlay(L"Steam", P(base, L"SteamGame"), true, false, ops, names, timing);
        check(spawned.size() == 2 && spawned[1] == L"steam://rungameid/359320", "Steam is started by its address");
        state.load(P(settingsFor(base).launcherDir, L"launcher.ini"), nullptr);
        check(state.mode(P(base, L"SteamGame")).empty(), "and a store that cannot carry a mode remembers none");
        check(state.lastInstall() == P(base, L"SteamGame") && state.mode(products) == "flat",
              "but it is now the install the window opens on next, and the Frontier folder keeps its mode");
        session.shutdown();   // the watcher for it would wait out its 1.5 s otherwise
        // An elevated launcher refuses, starting nothing.
        Ui ui3;
        LauncherSession elevated(envFor(&ui3, &http, base, false, P(base, L"self.exe")));
        const size_t before = spawned.size();
        elevated.beginPlay(L"Steam", P(base, L"SteamGame"), true, true, ops, names, timing);
        check(!elevated.playing() && spawned.size() == before && contains(elevated.playLine(), "administrator"), "an elevated launcher says why it will not start Elite and starts nothing: " + elevated.playLine());
        endFakes();
    }
}

// ---- direct launch -------------------------------------------------------------------------------------
//
// Frontier's servers are never contacted and the real Steam is never loaded. Every reply below is written
// by hand from what min-ed-launcher's source reads; the Steam library is build\launcher_test_fakes\steam_api64.dll
// (fake_steam_api.cpp), loaded by the launcher's own loader and export binding; and the game that "starts"
// is the fake program. The fake library and this rig write what happens, in order, to one log, so the
// order is proved from what the code did, not from what it says.

const char* kFrontier = "https://frontier.example.invalid";
const char* kAuthToken = "SENTINEL-AUTH-7f3a91c2";
const char* kMachineToken = "SENTINEL-MACHINE-c20e55d1";
// Frontier-store sign-in, all made up. None of them may reach a message, a band line, console output or a file
// (the login file holds the email in the clear and nothing else of these).
const char* kEmail = "sentinel.cmdr.4c1e@example.invalid";
const char* kPassword = "SENTINEL-PASSWORD-9d2c41";
const char* kCode = "SENTINEL-CODE-7731";
const char* kBadCode = "SENTINEL-BADCODE-2208";
const char* kEncCode = "SENTINEL-ENC-5b8e";
const char* kServerMachineToken = "SENTINEL-SERVERSIDE-MACHINE-e1f0";  // what the server's sign-in reply says; the port ignores it
const char* kFakeTicket = "14000000ABCDEF010203";               // the fake library's canned ticket, as hex
const char* kMachineGuid = "6f4e2c1a-9b3d-4e8f-a1c2-0d5b7e9f3a11";
const char* kFrontierGuid = "c0ffee00-1234-4abc-8def-0123456789ab";
const char* kExpectedMachineId = "be7640f19fe2be22";             // sha1(kMachineGuid + kFrontierGuid), first 16 hex
std::wstring g_steamLog;

void appendFile(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    CloseHandle(f);
}

void eventLog(const std::string& line) { appendFile(g_steamLog, line + "\n"); }
void resetEvents() { DeleteFileW(g_steamLog.c_str()); }

// The events as tags: the fake library's lines by their first word, the rig's own whole.
std::vector<std::string> eventTags() {
    std::vector<std::string> out;
    for (const std::string& line : lines(readAll(g_steamLog))) {
        if (line.compare(0, 5, "HTTP ") == 0 || line == "START") out.push_back(line);
        else out.push_back(line.substr(0, line.find(' ')));
    }
    return out;
}

int eventIndex(const std::vector<std::string>& tags, const std::string& tag) {
    for (size_t i = 0; i < tags.size(); ++i) {
        if (tags[i] == tag) return static_cast<int>(i);
    }
    return -1;
}

std::string joined(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : " | ") + p;
    return out;
}

// Text that must never hold a token, the ticket or the machine ID.
bool leaks(const std::string& text) {
    for (const char* secret : {kAuthToken, kMachineToken, kFakeTicket, kExpectedMachineId, kEmail, kPassword, kCode, kBadCode, kEncCode, kServerMachineToken}) {
        if (contains(text, secret)) return true;
    }
    return false;
}

// treeHas, leaving one file name out: the stored login holds the email in the clear and must hold nothing else.
bool treeHasSkipping(const std::wstring& dir, const std::vector<std::string>& needles, const std::wstring& skipName) {
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(P(dir, L"*").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        const std::wstring name = find.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring full = P(dir, name);
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            found = found || treeHasSkipping(full, needles, skipName);
            continue;
        }
        if (_wcsicmp(name.c_str(), skipName.c_str()) == 0) continue;
        const std::string bytes = readAll(full);
        for (const std::string& n : needles) found = found || contains(bytes, n);
    } while (FindNextFileW(h, &find));
    FindClose(h);
    return found;
}

// Every sentinel but the email, for a scan that includes the login file; the email too for one that skips it.
std::vector<std::string> frontierNeedles(bool withEmail) {
    std::vector<std::string> out = {kAuthToken, kMachineToken, kFakeTicket, kExpectedMachineId, kPassword, kCode, kBadCode, kEncCode, kServerMachineToken};
    if (withEmail) out.push_back(kEmail);
    return out;
}

bool treeHas(const std::wstring& dir, const std::vector<std::string>& needles) {
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(P(dir, L"*").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        const std::wstring name = find.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring full = P(dir, name);
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            found = found || treeHas(full, needles);
            continue;
        }
        const std::string bytes = readAll(full);
        for (const std::string& n : needles) found = found || contains(bytes, n);
    } while (FindNextFileW(h, &find));
    FindClose(h);
    return found;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string pathOf(const std::string& url) {
    const size_t scheme = url.find("://");
    const size_t slash = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    if (slash == std::string::npos) return std::string();
    const size_t query = url.find('?', slash);
    return url.substr(slash, query == std::string::npos ? std::string::npos : query - slash);
}

std::vector<std::pair<std::string, std::string>> queryOf(const std::string& url) {
    std::vector<std::pair<std::string, std::string>> out;
    const size_t q = url.find('?');
    if (q == std::string::npos) return out;
    for (const std::string& pair : split(url.substr(q + 1), '&')) {
        const size_t eq = pair.find('=');
        out.emplace_back(percentDecode(pair.substr(0, eq)), eq == std::string::npos ? std::string() : percentDecode(pair.substr(eq + 1)));
    }
    return out;
}

std::string queryNames(const std::string& url) {
    std::string out;
    for (const auto& kv : queryOf(url)) out += (out.empty() ? "" : ",") + kv.first;
    return out;
}

std::string queryValue(const std::string& url, const std::string& name) {
    for (const auto& kv : queryOf(url)) {
        if (kv.first == name) return kv.second;
    }
    return "<missing>";
}

std::string headerOf(const HttpRequest& r, const std::string& name) {
    for (const auto& h : r.headers) {
        if (h.first == name) return h.second;
    }
    return "<none>";
}

// Frontier, scripted by path. A path's replies are served in order and the last one repeats. Every request
// is also written to the event log, so it sits in its place among the Steam library's events.
class FrontierHttp : public HttpClient {
public:
    std::map<std::string, std::vector<Reply>> script;
    std::vector<HttpRequest> requests;

    HttpResponse get(const HttpRequest& request) override {
        std::lock_guard<std::mutex> guard(lock_);
        requests.push_back(request);
        const std::string path = pathOf(request.url);
        eventLog("HTTP " + path);
        Reply reply;
        reply.status = 404;
        const auto found = script.find(path);
        if (found != script.end() && !found->second.empty()) {
            reply = found->second.front();
            if (found->second.size() > 1) found->second.erase(found->second.begin());
        }
        HttpResponse out;
        out.transportOk = reply.transportOk;
        out.status = reply.status;
        out.error = reply.error;
        if (!out.transportOk) return out;
        if (request.maxBytes && reply.body.size() > request.maxBytes) {
            out.transportOk = false;
            out.error = "the reply is larger than allowed";
            return out;
        }
        if (out.status == 200) {
            out.body = reply.body;
            out.bytes = reply.body.size();
        }
        return out;
    }

    int count(const std::string& path) {
        std::lock_guard<std::mutex> guard(lock_);
        int n = 0;
        for (const HttpRequest& r : requests) n += pathOf(r.url) == path ? 1 : 0;
        return n;
    }
    // The nth request for `path`, or null.
    const HttpRequest* nth(const std::string& path, int index = 0) {
        std::lock_guard<std::mutex> guard(lock_);
        for (const HttpRequest& r : requests) {
            if (pathOf(r.url) == path && index-- == 0) return &r;
        }
        return nullptr;
    }

private:
    std::mutex lock_;
};

// One client the session can own, over the rig's.
class Share : public HttpClient {
public:
    explicit Share(HttpClient* target) : target_(target) {}
    HttpResponse get(const HttpRequest& request) override { return target_->get(request); }

private:
    HttpClient* target_;
};

const std::string kTimePath = "/1.1/server/time";
const std::string kAuthPath = "/3.0/user/steam/auth";
const std::string kPurchasesPath = "/3.0/user/purchases";
const std::string kInstallerPath = "/3.0/user/installer";

Reply jsonReply(const std::string& body) {
    Reply r;
    r.body = body;
    return r;
}
Reply statusReply(int code) {
    Reply r;
    r.status = code;
    return r;
}
Reply htmlReply() { return jsonReply("<!DOCTYPE html><html><head><title>Sign in</title></head><body>?</body></html>"); }
Reply noAnswer(const std::string& why) {
    Reply r;
    r.transportOk = false;
    r.error = why;
    return r;
}

std::string purchasesJson(const std::string& gameArgs, const std::string& serverArgs, bool testApi = false) {
    return "{\"purchases\":[{\"product_sku\":\"FORC-FDEV-D-1010\",\"product_name\":\"Elite Dangerous: Horizons\",\"directory\":\"elite-dangerous-64\",\"filter\":\"edh\","
           "\"gameargs\":\"\",\"serverargs\":\"\",\"testapi\":false,\"sortkey\":10},"
           "{\"product_sku\":\"FORC-FDEV-DO-38-IBE\",\"product_name\":\"Elite Dangerous: Odyssey\",\"directory\":\"elite-dangerous-odyssey-64\",\"filter\":\"edo\","
           "\"gameargs\":\"" + gameArgs + "\",\"serverargs\":\"" + serverArgs + "\",\"testapi\":" + (testApi ? "true" : "false") + ",\"sortkey\":20}]}";
}

void scriptHappy(FrontierHttp* http, const std::string& installerVersion = "4.2.1.0") {
    http->script[kTimePath] = {jsonReply("{\"unixTimestamp\":1800000000}")};
    http->script[kAuthPath] = {jsonReply(std::string("{\"authToken\":\"") + kAuthToken + "\",\"machineToken\":\"" + kMachineToken +
                                         "\",\"fid\":\"F1\",\"registeredName\":\"CMDR Test\"}")};
    http->script[kPurchasesPath] = {jsonReply(purchasesJson("/edo --foo", "/Zz"))};
    http->script[kInstallerPath] = {jsonReply("{\"version\":\"" + installerVersion + "\",\"remotePath\":\"68747470\",\"localFile\":\"x.zip\",\"md5\":\"00\",\"size\":1}")};
}

struct GameTree {
    std::wstring root, product, edlaunch, dll;
};

void writeVersionInfo(const std::wstring& product, const std::string& version, bool steamAware = true, bool offline = false, bool bom = false) {
    writeAll(P(product, L"VersionInfo.txt"),
             std::string(bom ? "\xEF\xBB\xBF" : "") + "{\"name\":\"Elite Dangerous Odyssey\",\"Version\":\"" + version + "\",\"executable\":\"EliteDangerous64.exe\","
             "\"useWatchDog64\":true,\"steamaware\":" + (steamAware ? "true" : "false") + ",\"offline\":" + (offline ? "true" : "false") + "}");
}

// <base>\Steam\steamapps\common\Elite Dangerous: EDLaunch.exe and steam_api64.dll at the top, the Odyssey product inside.
GameTree makeGame(const std::wstring& base, bool withDll = true) {
    GameTree g;
    g.root = P(base, L"Steam\\steamapps\\common\\Elite Dangerous");
    g.product = P(g.root, L"Products\\elite-dangerous-odyssey-64");
    g.edlaunch = P(g.root, L"EDLaunch.exe");
    g.dll = P(g.root, L"steam_api64.dll");
    writeAll(g.edlaunch, "x");
    writeAll(P(g.product, L"EliteDangerous64.exe"), "x");
    writeVersionInfo(g.product, "4.2.1.0");
    if (withDll && !CopyFileW(P(g_fakes, L"steam_api64.dll").c_str(), g.dll.c_str(), FALSE))
        check(false, "the fake steam_api64.dll could not be copied into " + narrowUtf8(g.root) + ": Windows error " + std::to_string(GetLastError()));
    return g;
}

// <base>\FrontierStore\Elite Dangerous: EDLaunch.exe at the top and the Odyssey product inside, and no Steam library
// anywhere: an install from Frontier's own store. Its path has no steamapps\common, which is how --direct-dry-run tells.
GameTree makeFrontierStore(const std::wstring& base) {
    GameTree g;
    g.root = P(base, L"FrontierStore\\Elite Dangerous");
    g.product = P(g.root, L"Products\\elite-dangerous-odyssey-64");
    g.edlaunch = P(g.root, L"EDLaunch.exe");
    writeAll(g.edlaunch, "x");
    writeAll(P(g.product, L"EliteDangerous64.exe"), "x");
    writeVersionInfo(g.product, "4.2.1.0");
    return g;
}

const std::string kFAuthPath = "/3.0/user/frontier/auth";
const std::string kFTokenPath = "/3.0/user/frontier/token";

Reply encReply() { return jsonReply(std::string("{\"encCode\":\"") + kEncCode + "\"}"); }
Reply tokenReply() { return jsonReply(std::string("{\"machineToken\":\"") + kMachineToken + "\"}"); }
// The server's reply names its own machine token; the port uses the one it sent.
Reply frontierAuthReply() { return jsonReply(std::string("{\"authToken\":\"") + kAuthToken + "\",\"machineToken\":\"" + kServerMachineToken + "\"}"); }

// Frontier's store, scripted: with `firstTime` the auth path answers the first call with a code and the second
// with the token, as it does for a PC it has not seen; otherwise every auth call is the sign-in proper.
void scriptStore(FrontierHttp* http, bool firstTime) {
    scriptHappy(http);
    http->script[kFAuthPath] = firstTime ? std::vector<Reply>{encReply(), frontierAuthReply()} : std::vector<Reply>{frontierAuthReply()};
    http->script[kFTokenPath] = {tokenReply()};
}

bool saveLogin(const std::wstring& path, const char* password, const char* token) {
    StoredLogin login;
    login.email = kEmail;
    login.password = password ? password : "";
    login.machineToken = token ? token : "";
    return saveFrontierLogin(path, login, nullptr);
}

struct RigKeys {
    MachineIdKeys keys;
    std::wstring root;
};

bool regSet(const std::wstring& path, const wchar_t* name, const std::string& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const std::wstring wide = widenUtf8(value);
    const LONG status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(wide.c_str()), static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::string regGet(const std::wstring& path, const wchar_t* name) {
    wchar_t buffer[260]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer, &size) != ERROR_SUCCESS) return "<none>";
    return narrowUtf8(buffer);
}

// A pair of HKCU subkeys standing in for the two MachineGuid keys; the machine one always holds kMachineGuid.
RigKeys rigKeys(const std::wstring& tag, bool presetFrontier) {
    RigKeys k;
    k.root = L"Software\\EdUpLauncherRigDirect-" + std::to_wstring(GetCurrentProcessId()) + L"-" + tag;
    RegDeleteTreeW(HKEY_CURRENT_USER, k.root.c_str());
    k.keys.machineRoot = HKEY_CURRENT_USER;
    k.keys.machinePath = k.root + L"\\Machine";
    k.keys.frontierRoot = HKEY_CURRENT_USER;
    k.keys.frontierPath = k.root + L"\\Frontier";
    regSet(k.keys.machinePath, L"MachineGuid", kMachineGuid);
    if (presetFrontier) regSet(k.keys.frontierPath, L"MachineGuid", kFrontierGuid);
    return k;
}
void dropKeys(const RigKeys& k) { RegDeleteTreeW(HKEY_CURRENT_USER, k.root.c_str()); }

DirectEnv directEnv(FrontierHttp* http, const MachineIdKeys& keys, std::vector<std::string>* progress) {
    DirectEnv e;
    e.http = http;
    e.apiBase = kFrontier;
    e.machineKeys = keys;
    e.clock.unixNow = []() { return 1700000000ll; };
    // 2 s per reading: the running time then moves by whole, countable seconds.
    const std::shared_ptr<unsigned long long> tick = std::make_shared<unsigned long long>(0);
    e.clock.tickMs = [tick]() { return *tick += 2000; };
    if (progress) e.progress = [progress](const std::string& line) { progress->push_back(line); };
    return e;
}

DirectRequest requestFor(const std::wstring& product, bool vr) {
    DirectRequest r;
    r.productDir = product;
    r.vr = vr;
    return r;
}

void setSteamMode(const char* mode) { SetEnvironmentVariableW(L"EDUP_FAKE_STEAM_MODE", mode && *mode ? widenUtf8(mode).c_str() : nullptr); }

std::vector<std::string> cat(std::initializer_list<std::vector<std::string>> parts) {
    std::vector<std::string> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

void testMachineId() {
    section("machine id");
    // Worked out from MachineId.fs: Trim each, join with nothing, SHA-1, hex, 16 characters, lower case.
    check(makeMachineId(kMachineGuid, kFrontierGuid) == kExpectedMachineId, "the ID of two fixed strings is sha1 of their concatenation, first 16 hex characters: " + makeMachineId(kMachineGuid, kFrontierGuid));
    check(makeMachineId(std::string("  ") + kMachineGuid + "\r\n", std::string("\t") + kFrontierGuid + " \n") == kExpectedMachineId, "each value is trimmed before it is joined");
    check(makeMachineId(kFrontierGuid, kMachineGuid) == "aff75376bbb5009e" && makeMachineId(kFrontierGuid, kMachineGuid) != kExpectedMachineId, "the machine's value comes first: swapping them is another ID");
    check(makeMachineId("", "") == "da39a3ee5e6b4b0d", "the empty input hashes to the SHA-1 of nothing, shortened: " + makeMachineId("", ""));

    // Through the registry: a subkey this rig makes and removes.
    {
        RigKeys keys = rigKeys(L"id-preset", true);
        std::string id, err;
        check(readMachineId(keys.keys, &id, &err) && id == kExpectedMachineId, "the ID is read from the two keys: " + err + id);
        check(regGet(keys.keys.frontierPath, L"MachineGuid") == kFrontierGuid, "and a Frontier value that is there is kept as it is");
        dropKeys(keys);
    }
    {
        RigKeys keys = rigKeys(L"id-made", false);
        check(regGet(keys.keys.frontierPath, L"MachineGuid") == "<none>", "the Frontier key starts missing");
        std::string first, second, err;
        check(readMachineId(keys.keys, &first, &err) && first.size() == 16, "a missing Frontier value is made: " + err);
        const std::string made = regGet(keys.keys.frontierPath, L"MachineGuid");
        bool guidShaped = made.size() == 36 && made[8] == '-' && made[13] == '-' && made[18] == '-' && made[23] == '-';
        for (char c : made) guidShaped = guidShaped && (c == '-' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        check(guidShaped, "it holds a random GUID, lower case with hyphens: " + made);
        check(readMachineId(keys.keys, &second, &err) && second == first && regGet(keys.keys.frontierPath, L"MachineGuid") == made, "and a later read keeps it, giving the same ID");
        check(first == makeMachineId(kMachineGuid, made), "the ID is made from the two values");
        dropKeys(keys);
    }
    {
        RigKeys keys = rigKeys(L"id-nomachine", true);
        RegDeleteTreeW(HKEY_CURRENT_USER, keys.keys.machinePath.c_str());
        std::string id, err;
        check(!readMachineId(keys.keys, &id, &err) && !err.empty() && !contains(err, kFrontierGuid), "no machine value is an error that names no value: " + err);
        dropKeys(keys);
    }
}

void testLayoutDpi() {
    section("layout dpi");
    // 1080p at 150%: a 1040 pixel work area, and a 70 pixel frame and title bar at 144 dpi.
    const unsigned fhd = layoutDpiFor(144, 1040, 786, 70);
    check(fhd < 144 && fhd >= 72 && 786 * fhd / 96 + 70 <= 1040, "1080p at 144 dpi is laid out smaller, and fits: " + std::to_string(fhd));
    check(fhd == 118, "and the largest dpi that fits is 118, not less: " + std::to_string(fhd));
    // The Steam Deck: 800 pixels, about 760 of them work area, at 96 dpi.
    const unsigned deck = layoutDpiFor(96, 760, 786, 39);
    check(deck < 96 && deck >= 72 && 786 * deck / 96 + 39 <= 760, "800p at 96 dpi is laid out smaller, and fits: " + std::to_string(deck));
    check(deck == 88, "and the largest dpi that fits is 88: " + std::to_string(deck));
    check(layoutDpiFor(144, 1400, 786, 70) == 144, "a 1440p screen at 144 dpi is left alone");
    check(layoutDpiFor(192, 2120, 786, 94) == 192, "a 4K screen at 192 dpi is left alone");
    check(layoutDpiFor(96, 1040, 786, 39) == 96, "1080p at 100% is left alone");
    check(layoutDpiFor(96, 400, 786, 39) == 72 && layoutDpiFor(144, 300, 786, 70) == 72, "it never goes below 72 dpi");
    check(layoutDpiFor(60, 400, 786, 39) == 60, "a system dpi already below 72 is not raised");
    check(layoutDpiFor(144, 1040, 0, 70) == 144 && layoutDpiFor(144, 60, 786, 70) == 144, "an unusable height or work area changes nothing");
    int rows = 0;
    for (int work = 500; work <= 2200; work += 50) {
        for (unsigned system : {96u, 120u, 144u, 192u}) {
            const unsigned got = layoutDpiFor(system, work, 786, 40 + static_cast<int>(system) / 4);
            ++rows;
            const bool fits = 786 * static_cast<int>(got) / 96 + 40 + static_cast<int>(system) / 4 <= work;
            check(got <= system && (got == 72 || fits), "work area " + std::to_string(work) + " at " + std::to_string(system) + " dpi gives " + std::to_string(got) + ", which fits or is the floor");
            if (got < system && got > 72) check(786ll * (got + 1) > static_cast<long long>(work - 40 - static_cast<int>(system) / 4) * 96, "and one dpi more would not fit");
        }
    }
    check(rows == 35 * 4, "the sweep saw every row: " + std::to_string(rows));
}

void testDirectPlan() {
    section("direct: the sequence");
    g_steamLog = P(g_scratch, L"steam_events.log");
    SetEnvironmentVariableW(L"EDUP_FAKE_STEAM_LOG", g_steamLog.c_str());
    setSteamMode("");

    // The pure parts.
    check(directCommandLine(true, "MT", "AT", "/Zz", true, true, "/edo --foo") == "\"ServerToken MT AT /Zz\" /steam /vr /edo --foo", "VR with game arguments and /steam: " + directCommandLine(true, "MT", "AT", "/Zz", true, true, "/edo --foo"));
    check(directCommandLine(true, "MT", "AT", "/Zz", true, false, "/edo --foo") == "\"ServerToken MT AT /Zz\" /steam /novr /edo --foo", "flat is /novr, and exactly one of the two is there");
    check(directCommandLine(true, "MT", "AT", "", true, true, "") == "\"ServerToken MT AT \" /steam /vr", "no server or game arguments: the quote closes after one space, as in Product.fs");
    check(directCommandLine(true, "MT", "AT", "", false, false, "") == "\"ServerToken MT AT \" /novr", "a product that is not steamaware gets no /steam");
    check(directCommandLine(false, "MT", "AT", "/Zz", true, true, "/g") == "\"\" /steam /vr /g", "an offline product has an empty quoted token");
    check(sameGameVersion("4.2.1.0", "4.2.1.0") && !sameGameVersion("4.2.1.0", "4.2.2.0") && !sameGameVersion("4.2.1", "4.2.1.0") && !sameGameVersion("a.b", "a.b") && sameGameVersion("4.2", "4.2"),
          "game versions are equal as System.Version is: the same numbers in the same count");
    {
        // Odyssey and live Horizons share a folder, and only the filter tells them apart.
        const auto purchase = [](const char* sku, const char* directory, const char* filter, long long sortKey) {
            Purchase p;
            p.sku = sku;
            p.directory = directory;
            p.filter = filter;
            p.sortKey = sortKey;
            return p;
        };
        const std::string live = "elite-dangerous-odyssey-64";
        const std::vector<Purchase> both = {purchase("OLD", "elite-dangerous-64", "edh", 1), purchase("H4", live.c_str(), "edh4", 5), purchase("ODY", live.c_str(), "edh4, edo", 9)};
        const Purchase* pick = chooseProduct(both, live);
        check(pick && pick->sku == "H4", "a filter piece is matched whole and untrimmed, as App.fs's split on commas does, so ' edo' does not count");
        const std::vector<Purchase> withEdo = {purchase("H4", live.c_str(), "edh4", 5), purchase("ODY", live.c_str(), "edh4,EDO", 9)};
        pick = chooseProduct(withEdo, live);
        check(pick && pick->sku == "ODY", "the edo product wins over a lower sortkey in the same folder, whatever the filter's case");
        const std::vector<Purchase> twoEdo = {purchase("B", live.c_str(), "edo", 9), purchase("A", live.c_str(), "edo", 3)};
        pick = chooseProduct(twoEdo, live);
        check(pick && pick->sku == "A", "among edo products the lowest sortkey wins, wherever it is listed");
        const std::vector<Purchase> onlyHorizons = {purchase("H4B", live.c_str(), "edh4", 7), purchase("H4A", live.c_str(), "edh4", 2)};
        pick = chooseProduct(onlyHorizons, live);
        check(pick && pick->sku == "H4A", "with no edo product in the folder the lowest sortkey is used");
        const std::vector<Purchase> bySku = {purchase("FORC-FDEV-DO-38", "elsewhere", "edo", 1)};
        pick = chooseProduct(bySku, "forc-fdev-do-38");
        check(pick && pick->sku == "FORC-FDEV-DO-38", "a folder named by the SKU matches, whatever its case");
        check(chooseProduct({purchase("OLD", "elite-dangerous-64", "edh", 1)}, live) == nullptr, "a purchase for another folder is not chosen");
        check(chooseProduct({purchase("", live.c_str(), "edo", 1)}, live) == nullptr, "a purchase with no SKU is not chosen, since the version check needs one");
    }
    check(maskSecrets("a TOKEN b TOKEN c ID", {"TOKEN", "ID", ""}) == "a <masked> b <masked> c <masked>", "maskSecrets removes every occurrence of each secret, and skips an empty one");
    check(contains(fallbackLine("Frontier refused the request (HTTP 403)"), "Frontier refused the request (HTTP 403)") && contains(fallbackLine("x"), "Steam") && contains(fallbackLine("x"), "mode it is set to"),
          "a fallback line gives the reason, names Steam, and says the game uses the mode it is set to: " + fallbackLine("x"));

    // The happy path, in full.
    {
        const std::wstring base = fresh("direct-happy");
        const GameTree game = makeGame(base);
        RigKeys keys = rigKeys(L"happy", true);
        FrontierHttp http;
        scriptHappy(&http);
        std::vector<std::string> progress;
        resetEvents();
        SetEnvironmentVariableW(L"SteamAppId", nullptr);
        const DirectPlan plan = planDirectLaunch(directEnv(&http, keys.keys, &progress), requestFor(game.product, true));
        check(plan.start && plan.reason.empty(), "everything answers: the plan is to start the game: " + plan.reason);
        check(plan.exe == P(game.product, L"EliteDangerous64.exe") && plan.workDir == game.product, "from the product's folder, the executable VersionInfo.txt names");
        check(narrowUtf8(plan.args) == std::string("\"ServerToken ") + kMachineToken + " " + kAuthToken + " /Zz\" /steam /vr /edo --foo", "the command line is the port's, in full");

        check(http.requests.size() == 4 && pathOf(http.requests[0].url) == kTimePath && pathOf(http.requests[1].url) == kAuthPath && pathOf(http.requests[2].url) == kPurchasesPath &&
                  pathOf(http.requests[3].url) == kInstallerPath,
              "four calls, in order: time, steam/auth, purchases, installer");
        if (http.requests.size() == 4) {
            for (const HttpRequest& r : http.requests) {
                check(!r.followRedirects && r.maxBytes == kFrontierMaxBytes && r.url.compare(0, std::strlen(kFrontier), kFrontier) == 0, "no redirect is followed, replies are capped at 4 MiB, and the host is the one given: " + pathOf(r.url));
            }
            const HttpRequest& time = http.requests[0];
            const HttpRequest& auth = http.requests[1];
            const HttpRequest& purchases = http.requests[2];
            const HttpRequest& installer = http.requests[3];
            check(time.url == std::string(kFrontier) + kTimePath && time.headers.empty(), "time has no query and no header");
            check(queryNames(auth.url) == "steamTicket,machineId,fTime" && queryValue(auth.url, "steamTicket") == kFakeTicket && queryValue(auth.url, "machineId") == kExpectedMachineId, "auth: steamTicket, machineId, fTime, with the ticket as hex and the machine ID");
            check(auth.headers.empty() && headerOf(auth, "Authorization") == "<none>", "and no Authorization header at all");
            check(queryNames(purchases.url) == "fTime" && headerOf(purchases, "Authorization") == std::string("bearer ") + kAuthToken, "purchases: fTime only, with the bearer token");
            check(queryNames(installer.url) == "machineToken,authToken,machineId,sku,os,fTime" && queryValue(installer.url, "machineToken") == kMachineToken &&
                      queryValue(installer.url, "authToken") == kAuthToken && queryValue(installer.url, "machineId") == kExpectedMachineId && queryValue(installer.url, "sku") == "FORC-FDEV-DO-38-IBE" &&
                      queryValue(installer.url, "os") == "win" && headerOf(installer, "Authorization") == std::string("bearer ") + kAuthToken,
                  "installer: machineToken, authToken, machineId, sku, os=win, fTime, with the same header, for the Odyssey sku");
            // The server said 1800000000; this machine's clock said 1700000000 and is not used. Each reading
            // of the fake clock is two seconds on.
            check(queryValue(auth.url, "fTime") == "1800000002.00000" && queryValue(purchases.url, "fTime") == "1800000004.00000" && queryValue(installer.url, "fTime") == "1800000006.00000",
                  "fTime is the server's time plus the seconds since, not the local clock: " + queryValue(auth.url, "fTime") + ", " + queryValue(purchases.url, "fTime") + ", " + queryValue(installer.url, "fTime"));
        }

        // Steam: the library beside EDLaunch.exe, in this order, and shut down before the game could start.
        const std::vector<std::string> tags = eventTags();
        const std::vector<std::string> expected = cat({{"HTTP " + kTimePath}, {"load", "init", "client", "pipe", "global_user", "user", "steamid", "ticket"}, {"HTTP " + kAuthPath},
                                                        {"cancel", "shutdown", "unload"}, {"HTTP " + kPurchasesPath, "HTTP " + kInstallerPath}});
        check(tags == expected, "events: " + joined(tags));
        const std::vector<std::string> raw = lines(readAll(g_steamLog));
        bool loadedFromGame = false, appId = false, version = false, cancelled = false;
        for (const std::string& line : raw) {
            std::string lowerLine = line, lowerPath = narrowUtf8(game.dll);
            for (char& c : lowerLine) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : lowerPath) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            loadedFromGame = loadedFromGame || lowerLine == "load path=" + lowerPath;
            appId = appId || line == "init appid=359320";
            version = version || line == "user version=SteamUser019";
            cancelled = cancelled || line == "cancel ticket=4242";
        }
        check(loadedFromGame, "steam_api64.dll was loaded by its absolute path, from beside EDLaunch.exe");
        check(appId && version && cancelled, "SteamAppId was 359320 when Steam started, the user interface asked for is SteamUser019, and the ticket handle is the one cancelled");
        wchar_t appIdNow[32]{};
        check(GetEnvironmentVariableW(L"SteamAppId", appIdNow, 32) == 0 && plan.steamAppId,
              "once Steam is released SteamAppId is gone from this process again, and the plan says the game's start sets it");
        check(!leaks(plan.reason) && !leaks(plan.detail), "the reason and the detail hold no token");
        bool progressLeaks = false;
        for (const std::string& line : progress) progressLeaks = progressLeaks || leaks(line);
        check(progress.size() >= 6 && !progressLeaks, "the window was told each step (" + std::to_string(progress.size()) + " lines) and none holds a token");
        check(plan.secrets.size() == 4 && !leaks(maskSecrets(narrowUtf8(plan.args), plan.secrets)) && contains(maskSecrets(narrowUtf8(plan.args), plan.secrets), "<masked>"),
              "masking the command line takes out both tokens and leaves the rest: " + maskSecrets(narrowUtf8(plan.args), plan.secrets));
        check(contains(plan.detail, "Version on disk: 4.2.1.0, Frontier's: 4.2.1.0"), "the detail says the two versions: " + plan.detail);

        // Flat is /novr, the same sign-in.
        FrontierHttp again;
        scriptHappy(&again);
        resetEvents();
        const DirectPlan flat = planDirectLaunch(directEnv(&again, keys.keys, nullptr), requestFor(game.product, false));
        check(flat.start && narrowUtf8(flat.args) == std::string("\"ServerToken ") + kMachineToken + " " + kAuthToken + " /Zz\" /steam /novr /edo --foo", "flat is /novr");

        // The product's own arguments.
        FrontierHttp bare;
        scriptHappy(&bare);
        bare.script[kPurchasesPath] = {jsonReply(purchasesJson("", ""))};
        const DirectPlan none = planDirectLaunch(directEnv(&bare, keys.keys, nullptr), requestFor(game.product, true));
        check(none.start && narrowUtf8(none.args) == std::string("\"ServerToken ") + kMachineToken + " " + kAuthToken + " \" /steam /vr", "with no arguments of its own: " + narrowUtf8(none.args));
        FrontierHttp test;
        scriptHappy(&test);
        test.script[kPurchasesPath] = {jsonReply(purchasesJson("", "/Zz", true))};
        const DirectPlan testApi = planDirectLaunch(directEnv(&test, keys.keys, nullptr), requestFor(game.product, true));
        check(testApi.start && contains(narrowUtf8(testApi.args), " /Test /Zz\" /steam /vr"), "a test-API product adds /Test before its server arguments: " + narrowUtf8(testApi.args));

        // VersionInfo.txt: steamaware, offline, a byte order mark.
        writeVersionInfo(game.product, "4.2.1.0", false, false, true);
        FrontierHttp unaware;
        scriptHappy(&unaware);
        const DirectPlan notSteam = planDirectLaunch(directEnv(&unaware, keys.keys, nullptr), requestFor(game.product, true));
        check(notSteam.start && !contains(narrowUtf8(notSteam.args), "/steam") && contains(narrowUtf8(notSteam.args), "/vr"), "steamaware false drops /steam (and a byte order mark in VersionInfo.txt is read past)");
        writeVersionInfo(game.product, "4.2.1.0", true, true);
        FrontierHttp offline;
        scriptHappy(&offline);
        const DirectPlan off = planDirectLaunch(directEnv(&offline, keys.keys, nullptr), requestFor(game.product, true));
        check(off.start && narrowUtf8(off.args).compare(0, 6, "\"\" /st") == 0, "an offline product is given an empty server token: " + std::string(contains(narrowUtf8(off.args), kAuthToken) ? "(tokens present)" : narrowUtf8(off.args)));
        dropKeys(keys);
    }

    // A time call that fails is not a failure: the local clock stands in.
    {
        int rows = 0;
        for (const Reply& timeReply : {noAnswer("no route"), statusReply(500), jsonReply("not json"), jsonReply("{\"unixTimestamp\":\"soon\"}")}) {
            ++rows;
            const std::wstring base = fresh("direct-time");
            const GameTree game = makeGame(base);
            RigKeys keys = rigKeys(L"time", true);
            FrontierHttp http;
            scriptHappy(&http);
            http.script[kTimePath] = {timeReply};
            resetEvents();
            const DirectPlan plan = planDirectLaunch(directEnv(&http, keys.keys, nullptr), requestFor(game.product, true));
            check(plan.start, "a time call that fails does not stop the launch (row " + std::to_string(rows) + "): " + plan.reason);
            check(http.requests.size() == 4 && queryValue(http.requests[1].url, "fTime") == "1700000002.00000", "and fTime is the local UTC clock plus the seconds since: " + (http.requests.size() > 1 ? queryValue(http.requests[1].url, "fTime") : std::string("?")));
            dropKeys(keys);
        }
        check(rows == 4, "the time rows were all run");
    }

    // Falling back: every row of the table, with Steam closed down each time and nothing started.
    {
        const std::string T = "HTTP " + kTimePath, A = "HTTP " + kAuthPath, Pu = "HTTP " + kPurchasesPath, I = "HTTP " + kInstallerPath;
        const std::vector<std::string> up = {"load", "init", "client", "pipe", "global_user", "user", "steamid", "ticket"};
        const std::vector<std::string> down = {"cancel", "shutdown", "unload"};
        struct Row {
            std::string name;
            std::function<void(FrontierHttp&, const GameTree&)> arrange;
            std::string steamMode;
            std::vector<std::string> events;
            std::string reason;
        };
        auto authReplies = [](std::vector<Reply> replies) {
            return [replies](FrontierHttp& h, const GameTree&) { h.script[kAuthPath] = replies; };
        };
        auto purchaseReplies = [](std::vector<Reply> replies) {
            return [replies](FrontierHttp& h, const GameTree&) { h.script[kPurchasesPath] = replies; };
        };
        auto installerReplies = [](std::vector<Reply> replies) {
            return [replies](FrontierHttp& h, const GameTree&) { h.script[kInstallerPath] = replies; };
        };
        auto nothing = [](FrontierHttp&, const GameTree&) {};
        const std::vector<std::string> afterAuth1 = cat({{T}, up, {A}, down});
        Reply bigBody;
        bigBody.body = "{\"purchases\":[],\"pad\":\"" + std::string(5u << 20, 'x') + "\"}";
        std::vector<Row> rows = {
            // transport failure, 5xx, or 400, 401, 403: say so, EDLaunch route
            {"auth: no answer", authReplies({noAnswer("could not connect (Windows error 12029)")}), "", afterAuth1, "could not be reached"},
            {"auth: 503", authReplies({statusReply(503)}), "", afterAuth1, "unavailable"},
            {"auth: 500", authReplies({statusReply(500)}), "", afterAuth1, "unavailable"},
            {"auth: 400", authReplies({statusReply(400)}), "", afterAuth1, "refused the request (HTTP 400)"},
            {"auth: 401", authReplies({statusReply(401)}), "", afterAuth1, "refused the request (HTTP 401)"},
            {"auth: 403", authReplies({statusReply(403)}), "", afterAuth1, "refused the request (HTTP 403)"},
            // the ownership page: once more, and a second page is the answer
            {"auth: the ownership page twice", authReplies({htmlReply(), htmlReply()}), "", cat({{T}, up, {A, A}, down}), "confirm that this account owns the game"},
            // 302 and 307: the accounts need linking
            {"auth: 302", authReplies({statusReply(302)}), "", afterAuth1, "Steam and Frontier accounts need linking (HTTP 302)"},
            {"auth: 307", authReplies({statusReply(307)}), "", afterAuth1, "Steam and Frontier accounts need linking (HTTP 307)"},
            // an unexpected reply
            {"auth: 404", authReplies({statusReply(404)}), "", afterAuth1, "does not know (HTTP 404)"},
            {"auth: no tokens", authReplies({jsonReply("{\"error_enum\":\"x\",\"message\":\"y\"}")}), "", afterAuth1, "carried no tokens"},
            {"auth: one token only", authReplies({jsonReply(std::string("{\"authToken\":\"") + kAuthToken + "\"}")}), "", afterAuth1, "carried no tokens"},
            {"auth: not JSON", authReplies({jsonReply("nonsense")}), "", afterAuth1, "not in the form"},
            {"auth: too large", authReplies({bigBody}), "", afterAuth1, "larger than allowed"},
            // the purchases
            {"purchases: 403", purchaseReplies({statusReply(403)}), "", cat({afterAuth1, {Pu}}), "refused the request (HTTP 403)"},
            {"purchases: 503", purchaseReplies({statusReply(503)}), "", cat({afterAuth1, {Pu}}), "unavailable"},
            {"purchases: the ownership page twice", purchaseReplies({htmlReply(), htmlReply()}), "", cat({afterAuth1, {Pu, Pu}}), "confirm that this account owns the game"},
            {"purchases: 307", purchaseReplies({statusReply(307)}), "", cat({afterAuth1, {Pu}}), "need linking"},
            {"purchases: not the product", purchaseReplies({jsonReply("{\"purchases\":[{\"product_sku\":\"X\",\"directory\":\"elite-dangerous-64\"}]}")}), "", cat({afterAuth1, {Pu}}), "not among the products"},
            {"purchases: none", purchaseReplies({jsonReply("{\"purchases\":[]}")}), "", cat({afterAuth1, {Pu}}), "not among the products"},
            {"purchases: not a list", purchaseReplies({jsonReply("{\"purchases\":5}")}), "", cat({afterAuth1, {Pu}}), "not in the form"},
            {"purchases: too large", purchaseReplies({bigBody}), "", cat({afterAuth1, {Pu}}), "larger than allowed"},
            // the game's version
            {"installer: 503", installerReplies({statusReply(503)}), "", cat({afterAuth1, {Pu, I}}), "unavailable"},
            {"installer: a different version", installerReplies({jsonReply("{\"version\":\"4.2.2.0\"}")}), "", cat({afterAuth1, {Pu, I}}), "game needs patching (installed 4.2.1.0, current 4.2.2.0)"},
            {"installer: no version", installerReplies({jsonReply("{\"remotePath\":\"x\"}")}), "", cat({afterAuth1, {Pu, I}}), "carried no version"},
            {"installer: the ownership page twice", installerReplies({htmlReply(), htmlReply()}), "", cat({afterAuth1, {Pu, I, I}}), "confirm that this account owns the game"},
            // Steam: no library, a library that will not load or is not Steam's, Steam not running, no user, no id, no ticket
            {"steam: the library is missing", [](FrontierHttp&, const GameTree& g) { DeleteFileW(g.dll.c_str()); }, "", {},"steam_api64.dll was not found beside Elite's launcher"},
            {"steam: the library will not load", [](FrontierHttp&, const GameTree& g) { writeAll(g.dll, "this is not a library"); }, "", {T}, "could not be loaded"},
            {"steam: the library is not Steam's", [](FrontierHttp&, const GameTree& g) { copyOfSystemDll(g.dll); }, "", {T}, "not a version the launcher can use"},
            {"steam: not running", nothing, "init_fails", {T, "load", "init", "unload"}, "Make sure the Steam client is running"},
            {"steam: no client", nothing, "no_client", {T, "load", "init", "client", "shutdown", "unload"}, "would not give the launcher your account"},
            {"steam: no pipe", nothing, "no_pipe", {T, "load", "init", "client", "pipe", "shutdown", "unload"}, "would not give the launcher your account"},
            {"steam: no global user", nothing, "no_global_user", {T, "load", "init", "client", "pipe", "global_user", "shutdown", "unload"}, "would not give the launcher your account"},
            {"steam: no user interface", nothing, "no_user", {T, "load", "init", "client", "pipe", "global_user", "user", "shutdown", "unload"}, "would not give the launcher your account"},
            {"steam: a zero Steam ID", nothing, "zero_id", {T, "load", "init", "client", "pipe", "global_user", "user", "steamid", "shutdown", "unload"}, "would not give the launcher your account"},
            {"steam: no ticket", nothing, "no_ticket", {T, "load", "init", "client", "pipe", "global_user", "user", "steamid", "ticket", "shutdown", "unload"}, "would not give the launcher your account"},
            // the folder
            {"folder: no VersionInfo.txt", [](FrontierHttp&, const GameTree& g) { DeleteFileW(P(g.product, L"VersionInfo.txt").c_str()); }, "", {}, "VersionInfo.txt could not be read"},
            {"folder: a version nobody can read", [](FrontierHttp&, const GameTree& g) { writeVersionInfo(g.product, "four"); }, "", {}, "cannot read"},
            {"folder: no executable", [](FrontierHttp&, const GameTree& g) { DeleteFileW(P(g.product, L"EliteDangerous64.exe").c_str()); }, "", {}, "program was not found"},
            {"folder: no EDLaunch.exe above it", [](FrontierHttp&, const GameTree& g) { DeleteFileW(g.edlaunch.c_str()); }, "", {}, "steam_api64.dll was not found"},
        };
        int ran = 0;
        for (const Row& row : rows) {
            ++ran;
            const std::wstring base = fresh("direct-row");
            const GameTree game = makeGame(base);
            RigKeys keys = rigKeys(L"row", true);
            FrontierHttp http;
            scriptHappy(&http);
            row.arrange(http, game);
            setSteamMode(row.steamMode.c_str());
            resetEvents();
            std::vector<std::string> progress;
            const DirectPlan plan = planDirectLaunch(directEnv(&http, keys.keys, &progress), requestFor(game.product, true));
            setSteamMode("");
            check(!plan.start && plan.exe.empty() && plan.args.empty(), row.name + ": the plan is to fall back, with nothing to start");
            check(contains(plan.reason, row.reason), row.name + ": the reason says so (" + row.reason + "): " + plan.reason);
            const std::string line = fallbackLine(plan.reason);
            check(contains(line, "Steam instead") && contains(line, "mode it is set to") && contains(line, plan.reason), row.name + ": the band's line gives the reason and the way on: " + line);
            check(eventTags() == row.events, row.name + ": events " + joined(eventTags()) + "   expected " + joined(row.events));
            bool progressLeaks = leaks(plan.reason) || leaks(plan.detail) || leaks(line);
            for (const std::string& p : progress) progressLeaks = progressLeaks || leaks(p);
            check(!progressLeaks, row.name + ": no token in the reason, the detail, the line or the progress");
            dropKeys(keys);
        }
        check(ran == 40 && static_cast<size_t>(ran) == rows.size(), "every row of the fallback table was run: " + std::to_string(ran));
    }

    // The ownership page that recovers on the retry: the second answer is taken, and Steam was only asked once.
    {
        const std::wstring base = fresh("direct-retry");
        const GameTree game = makeGame(base);
        RigKeys keys = rigKeys(L"retry", true);
        const std::string T = "HTTP " + kTimePath, A = "HTTP " + kAuthPath, Pu = "HTTP " + kPurchasesPath, I = "HTTP " + kInstallerPath;
        for (int which = 0; which < 3; ++which) {
            FrontierHttp http;
            scriptHappy(&http);
            const Reply good = http.script[which == 0 ? kAuthPath : which == 1 ? kPurchasesPath : kInstallerPath].front();
            http.script[which == 0 ? kAuthPath : which == 1 ? kPurchasesPath : kInstallerPath] = {htmlReply(), good};
            resetEvents();
            std::vector<std::string> progress;
            const DirectPlan plan = planDirectLaunch(directEnv(&http, keys.keys, &progress), requestFor(game.product, true));
            check(plan.start, "an ownership page that goes away on the retry is not the end (call " + std::to_string(which + 1) + "): " + plan.reason);
            const std::vector<std::string> expected =
                cat({{T}, {"load", "init", "client", "pipe", "global_user", "user", "steamid", "ticket"}, which == 0 ? std::vector<std::string>{A, A} : std::vector<std::string>{A},
                     {"cancel", "shutdown", "unload"}, which == 1 ? std::vector<std::string>{Pu, Pu} : std::vector<std::string>{Pu}, which == 2 ? std::vector<std::string>{I, I} : std::vector<std::string>{I}});
            check(eventTags() == expected, "the call was made twice and nothing else was repeated: " + joined(eventTags()));
            bool said = false;
            for (const std::string& p : progress) said = said || contains(p, "once more");
            check(said, "and the window was told it was trying again");
        }
        dropKeys(keys);
    }

    // The reply cap and the tokens on the wire, through the real client, in testHttp. A stop in the middle:
    {
        const std::wstring base = fresh("direct-cancel");
        const GameTree game = makeGame(base);
        RigKeys keys = rigKeys(L"cancel", true);
        FrontierHttp http;
        scriptHappy(&http);
        std::atomic<bool> stop{true};
        DirectEnv env = directEnv(&http, keys.keys, nullptr);
        env.cancel = &stop;
        resetEvents();
        const DirectPlan plan = planDirectLaunch(env, requestFor(game.product, true));
        check(!plan.start && http.requests.empty() && eventTags().empty(), "a launch stopped before it began contacts nothing and loads nothing");
        dropKeys(keys);
    }
}

// The session: the band's behaviour without a window.
struct DirectRig {
    Ui ui;
    FrontierHttp http;
    RigKeys keys;
    std::wstring base;
    GameTree game;
    std::vector<std::string> seen;  // the line at every change
    struct Start {
        std::wstring exe, args, dir;
        std::wstring appId;  // SteamAppId as the game would have inherited it, or "(none)"
    };
    std::vector<Start> started;
    std::vector<HANDLE> processes;
    int minimised = 0, restored = 0;  // windowAside(true) and windowAside(false)
    Recorder rec;
    std::unique_ptr<LauncherSession> session;

    // A Frontier-store install's questions, answered from these lists (an empty list is a Cancel). They are
    // called on the window's thread, which is this one: the rig checks that, and what each was told.
    struct LoginAnswer {
        bool ok;
        std::string email, password;
        bool remember;
    };
    struct CodeAnswer {
        bool ok;
        std::string code;
    };
    std::vector<LoginAnswer> loginAnswers;
    std::vector<CodeAnswer> codeAnswers;
    std::vector<std::string> loginNotes, loginEmails, codeNotes, codeEmails;
    std::vector<bool> loginRemembers, askedOnWindowThread;
    std::vector<StoredLogin> fileWhenAsked;  // what frontier-login.dat held when the login was asked for
    std::thread::id windowThread = std::this_thread::get_id();
    std::wstring loginPath;

    explicit DirectRig(const std::string& name, const std::string& ini, bool frontierStore = false, bool directOffered = true) {
        base = fresh(name);
        game = frontierStore ? makeFrontierStore(base) : makeGame(base);
        loginPath = P(base, L"launcher\\frontier-login.dat");
        keys = rigKeys(widenUtf8(name), true);
        scriptHappy(&http);
        if (!ini.empty()) writeAll(P(base, L"launcher\\launcher.ini"), ini);
        resetEvents();
        SessionEnv e;
        e.roots.dataRoot = base;
        e.roots.canonical = false;
        e.selfExe = P(base, L"self.exe");
        e.directLaunchOffered = directOffered;
        e.keys = g_testKeys;
        e.makeHttp = [this]() { return std::unique_ptr<HttpClient>(new Share(&http)); };
        e.post = [this](std::function<void()> f) { ui.post(std::move(f)); };
        e.changed = [this]() {
            ++ui.changed;
            seen.push_back(session ? session->playLine() : std::string());
        };
        e.closeWindow = [this]() { ++ui.closed; };
        e.windowAside = [this](bool aside) {
            ui.aside.push_back(aside);
            ++(aside ? minimised : restored);
        };
        e.frontierBase = kFrontier;
        e.machineKeys = keys.keys;
        e.clock.unixNow = []() { return 1700000000ll; };
        e.askLogin = [this](const std::string& note, LoginForm* form) {
            askedOnWindowThread.push_back(std::this_thread::get_id() == windowThread);
            loginNotes.push_back(note);
            loginEmails.push_back(form->email);
            loginRemembers.push_back(form->remember);
            StoredLogin onDisk;
            loadFrontierLogin(loginPath, &onDisk);
            fileWhenAsked.push_back(onDisk);
            const size_t n = loginNotes.size() - 1;
            if (n >= loginAnswers.size() || !loginAnswers[n].ok) return false;
            form->email = loginAnswers[n].email;
            form->password = loginAnswers[n].password;
            form->remember = loginAnswers[n].remember;
            return true;
        };
        e.askCode = [this](const std::string& email, const std::string& note, std::string* code) {
            askedOnWindowThread.push_back(std::this_thread::get_id() == windowThread);
            codeEmails.push_back(email);
            codeNotes.push_back(note);
            const size_t n = codeNotes.size() - 1;
            if (n >= codeAnswers.size() || !codeAnswers[n].ok) return false;
            *code = codeAnswers[n].code;
            return true;
        };
        e.startGame = [this](const std::wstring& exe, const std::wstring& args, const std::wstring& dir) -> HANDLE {
            wchar_t appId[32]{};
            const std::wstring inherited = GetEnvironmentVariableW(L"SteamAppId", appId, 32) ? std::wstring(appId) : std::wstring(L"(none)");
            started.push_back({exe, args, dir, inherited});
            eventLog("START");
            return startProgram(P(g_fakes, kFakeGame), L"--run-ms 700");
        };
        session.reset(new LauncherSession(e));
    }
    ~DirectRig() {
        session.reset();
        dropKeys(keys);
    }
    // Play, with the fake programs' names and quick timing for the Steam route that follows a fallback.
    void play(const std::wstring& source, bool vr, bool elevated) {
        WatchNames names;
        names.game = kFakeGame;
        names.edlaunch = kFakeEd;
        WatchTiming timing;
        timing.pollMs = 40;
        timing.startupWaitMs = 300;
        timing.closeWaitMs = 100;
        session->beginPlay(source, game.product, vr, elevated, rec.ops(), names, timing);
    }
    bool anyLineLeaks() const {
        for (const std::string& s : seen) {
            if (leaks(s)) return true;
        }
        return leaks(session->playLine());
    }
};

void testDirectSession() {
    section("direct: the session");
    endFakes();
    const std::string steamUri = "steam://rungameid/359320";
    auto uris = [](const Recorder& r) {
        std::vector<std::string> out;
        for (const std::wstring& u : r.uris) out.push_back(narrowUtf8(u));
        return out;
    };

    // Off by default: no key, the Steam route as before, and nothing contacted or loaded.
    {
        DirectRig rig("direct-off", std::string());
        check(!rig.session->directLaunch(), "with no key the setting is off");
        rig.play(L"Steam", true, false);
        check(rig.ui.pump([&]() { return !rig.session->playing(); }), "the Steam route's watcher ends");
        check(uris(rig.rec) == std::vector<std::string>{steamUri} && rig.started.empty() && rig.http.requests.empty() && eventTags().empty(),
              "Play on Steam opens steam://rungameid/359320 and contacts nothing, starts nothing and loads nothing");
        // Other values are off too.
        int others = 0;
        for (const char* value : {"off", "ON", "yes", "1", "true", "on "}) {
            DirectRig other("direct-off-" + std::to_string(++others), std::string("direct_launch=") + value + "\n");
            check(!other.session->directLaunch(), std::string("direct_launch=") + value + " is off");
        }
        check(others == 6, "every other value was tried");
    }

    // Not offered (the launcher under Wine): direct_launch=on in launcher.ini changes nothing. The setting
    // reads false, Play on a Steam install opens steam://rungameid/359320 and starts no game directly, and
    // the window has no checkbox to show (directLaunchOffered() is what it asks). The saved key is untouched.
    {
        DirectRig rig("direct-not-offered", "direct_launch=on\n", false, false);
        check(!rig.session->directLaunchOffered(), "the session reports direct launch as not offered");
        check(!rig.session->directLaunch(), "with direct launch not offered, direct_launch=on reads as off");
        rig.play(L"Steam", true, false);
        check(rig.ui.pump([&]() { return !rig.session->playing(); }), "the Steam route's watcher ends when direct launch is not offered");
        check(uris(rig.rec) == std::vector<std::string>{steamUri} && rig.started.empty() && rig.http.requests.empty() && eventTags().empty(),
              "Play on Steam opens steam://rungameid/359320 and starts no game directly, contacts nothing, loads nothing");
        LauncherState state;
        state.load(P(P(rig.base, L"launcher"), L"launcher.ini"), nullptr);
        check(state.directLaunch(), "the saved direct_launch=on is left as it was");
        DirectRig offered("direct-offered", "direct_launch=on\n");
        check(offered.session->directLaunchOffered() && offered.session->directLaunch(), "offered is the default, and then the saved setting stands");
    }

    // On: direct. The game is started by the launcher, the window steps aside and comes back and stays open,
    // and nothing else is opened.
    {
        DirectRig rig("direct-on", "direct_launch=on\n");
        check(rig.session->directLaunch(), "direct_launch=on is on");
        rig.play(L"Steam", false, false);
        check(rig.session->playing() && contains(rig.session->playLine(), "Signing in"), "Play marks the session busy at once, with the first line: " + rig.session->playLine());
        check(rig.ui.pump([&]() { return rig.restored == 1; }), "the window comes back when the game exits");
        check(rig.started.size() == 1 && rig.started[0].exe == P(rig.game.product, L"EliteDangerous64.exe") && rig.started[0].dir == rig.game.product, "the game is started once, from its own folder");
        check(!rig.started.empty() && narrowUtf8(rig.started[0].args) == std::string("\"ServerToken ") + kMachineToken + " " + kAuthToken + " /Zz\" /steam /novr /edo --foo", "with the port's command line, /novr for flat");
        check(rig.rec.uris.empty() && rig.rec.spawns.empty(), "no store address is opened and nothing is spawned");
        check(rig.ui.aside == std::vector<bool>{true, false} && rig.ui.closed == 0,
              "the window stepped aside once when the game started, came back when it ended, and was not closed");
        check(!rig.session->playing() && rig.session->playLine() == "Elite has closed.", "and the line ends: " + rig.session->playLine());
        const std::vector<std::string> tags = eventTags();
        const int start = eventIndex(tags, "START");
        check(start >= 0 && eventIndex(tags, "cancel") >= 0 && eventIndex(tags, "cancel") < start && eventIndex(tags, "shutdown") < start && eventIndex(tags, "unload") < start && start == static_cast<int>(tags.size()) - 1,
              "the ticket was cancelled, Steam shut down and its library unloaded before the game was started: " + joined(tags));
        LauncherState state;
        state.load(P(P(rig.base, L"launcher"), L"launcher.ini"), nullptr);
        check(state.mode(rig.game.product) == "flat" && state.directLaunch(), "flat is remembered for the install, and the setting is still on");
        check(state.lastInstall() == rig.game.product, "a direct launch remembers its install for the next start");
        check(!rig.anyLineLeaks(), "no line the band showed (" + std::to_string(rig.seen.size()) + ") holds a token");
        check(!treeHas(rig.base, {kAuthToken, kMachineToken, kFakeTicket, kExpectedMachineId}), "no token, ticket or machine ID is in any file under the data root");
    }

    // VR: the same, with /vr.
    {
        DirectRig rig("direct-vr", "direct_launch=on\n");
        rig.play(L"Steam", true, false);
        check(rig.ui.pump([&]() { return rig.restored == 1; }) && rig.started.size() == 1 && contains(narrowUtf8(rig.started[0].args), " /steam /vr /edo --foo"), "Play VR starts the game with /vr");
    }

    // One window, a Steam launch and then a Frontier-store one (the window stays open after a game now):
    // only the Steam game gets Steam's app id, and the process's environment is as it was in between.
    {
        SetEnvironmentVariableW(L"SteamAppId", nullptr);
        {
            DirectRig rig("appid-steam", "direct_launch=on\n");
            rig.play(L"Steam", true, false);
            check(rig.ui.pump([&]() { return rig.restored == 1; }) && rig.started.size() == 1 && rig.started[0].appId == L"359320",
                  "the Steam game was started with SteamAppId=359320: " + (rig.started.empty() ? std::string("(not started)") : narrowUtf8(rig.started[0].appId)));
        }
        wchar_t between[32]{};
        check(GetEnvironmentVariableW(L"SteamAppId", between, 32) == 0, "after it, the launcher's own environment has no SteamAppId: " + narrowUtf8(between));
        {
            DirectRig rig("appid-store", "direct_launch=on\n", true);
            scriptStore(&rig.http, false);
            saveLogin(rig.loginPath, kPassword, kMachineToken);
            rig.play(L"Frontier launcher", true, false);
            check(rig.ui.pump([&]() { return rig.restored == 1; }) && rig.started.size() == 1 && rig.started[0].appId == L"(none)",
                  "the Frontier-store game started next inherits no SteamAppId: " + (rig.started.empty() ? std::string("(not started)") : narrowUtf8(rig.started[0].appId)));
        }
        // A value the user's own environment had is put back as it was, not removed.
        SetEnvironmentVariableW(L"SteamAppId", L"12345");
        {
            DirectRig rig("appid-kept", "direct_launch=on\n");
            rig.play(L"Steam", true, false);
            check(rig.ui.pump([&]() { return rig.restored == 1; }) && rig.started.size() == 1 && rig.started[0].appId == L"359320", "a Steam start still gets 359320 for itself");
        }
        wchar_t kept[32]{};
        GetEnvironmentVariableW(L"SteamAppId", kept, 32);
        check(std::wstring(kept) == L"12345", "and the value that was there before is back afterwards: " + narrowUtf8(kept));
        SetEnvironmentVariableW(L"SteamAppId", nullptr);
    }

    // Elevated: refused before anything is contacted.
    {
        DirectRig rig("direct-elevated", "direct_launch=on\n");
        rig.play(L"Steam", true, true);
        check(!rig.session->playing() && contains(rig.session->playLine(), "administrator") && rig.session->playLine() == elevatedRefusal(), "an elevated launcher says why it will not start Elite: " + rig.session->playLine());
        check(rig.http.requests.empty() && rig.started.empty() && rig.rec.uris.empty() && rig.rec.spawns.empty() && eventTags().empty(), "and contacts nothing, loads nothing and starts nothing");
    }

    // Fallbacks, through the session: the reason in the line, then the Steam route, and no game started.
    {
        struct Case {
            const char* name;
            std::function<void(DirectRig&)> arrange;
            const char* mode;
            const char* reason;
        };
        const std::vector<Case> cases = {
            {"a refusal", [](DirectRig& r) { r.http.script[kAuthPath] = {statusReply(403)}; }, "", "refused the request (HTTP 403)"},
            {"no answer", [](DirectRig& r) { r.http.script[kAuthPath] = {noAnswer("no route")}; }, "", "could not be reached"},
            {"the ownership page twice", [](DirectRig& r) { r.http.script[kAuthPath] = {htmlReply(), htmlReply()}; }, "", "confirm that this account owns the game"},
            {"accounts to link", [](DirectRig& r) { r.http.script[kAuthPath] = {statusReply(302)}; }, "", "need linking"},
            {"a game that needs patching", [](DirectRig& r) { r.http.script[kInstallerPath] = {jsonReply("{\"version\":\"9.9.9.9\"}")}; }, "", "game needs patching"},
            {"Steam not running", [](DirectRig&) {}, "init_fails", "Steam client is running"},
        };
        int ran = 0;
        for (const Case& c : cases) {
            ++ran;
            DirectRig rig(std::string("direct-fallback-") + std::to_string(ran), "direct_launch=on\n");
            c.arrange(rig);
            setSteamMode(c.mode);
            rig.play(L"Steam", true, false);
            check(rig.ui.pump([&]() { return rig.rec.uris.size() == 1 && !rig.session->playing(); }), std::string(c.name) + ": the Steam route is taken and its watcher ends");
            setSteamMode("");
            check(uris(rig.rec) == std::vector<std::string>{steamUri} && rig.started.empty() && rig.ui.closed == 0, std::string(c.name) + ": Steam's address is opened and no game is started");
            bool said = false;
            for (const std::string& line : rig.seen) said = said || (contains(line, "Direct launch did not work") && contains(line, c.reason) && contains(line, "mode it is set to"));
            check(said, std::string(c.name) + ": the band's line said why and that the game uses the mode it is set to");
            check(eventIndex(eventTags(), "START") < 0, std::string(c.name) + ": nothing was started");
            check(!rig.anyLineLeaks() && !treeHas(rig.base, {kAuthToken, kMachineToken, kFakeTicket, kExpectedMachineId}), std::string(c.name) + ": no token in a line or a file");
        }
        check(ran == 6, "every fallback case was run through the session");
    }

    // Only a Steam install goes direct, even with the key on.
    {
        DirectRig rig("direct-other-stores", "direct_launch=on\n");
        writeAll(P(rig.game.root, L"EDLaunch\\EDLaunch.exe"), "x");
        WatchNames names;
        names.game = kFakeGame;
        names.edlaunch = kFakeEd;
        WatchTiming timing;
        timing.pollMs = 40;
        timing.startupWaitMs = 200;
        timing.closeWaitMs = 100;
        rig.session->beginPlay(L"Epic", rig.game.product, true, false, rig.rec.ops(), names, timing);
        rig.session->shutdown();
        check(rig.rec.uris.size() == 1 && rig.rec.uris[0].find(L"com.epicgames.launcher://") == 0 && rig.http.requests.empty() && rig.started.empty() && eventTags().empty(), "an Epic install takes its own route and contacts nothing");
        DirectRig frontier("direct-frontier-store", "direct_launch=on\n");
        writeAll(P(frontier.game.root, L"EDLaunch\\EDLaunch.exe"), "x");
        frontier.session->beginPlay(L"Folder you chose", frontier.game.product, true, false, frontier.rec.ops(), names, timing);
        frontier.session->shutdown();
        check(frontier.rec.spawns.size() == 1 && frontier.http.requests.empty() && frontier.started.empty(), "and so does a folder started through EDLaunch");
    }

    // The checkbox's rules: asked first, a No leaves it off, turning it off does not ask.
    {
        DirectRig rig("direct-dialog", std::string());
        int asked = 0;
        check(!rig.session->setDirectLaunch(true, [&]() { ++asked; return false; }) && asked == 1 && !rig.session->directLaunch(), "turning it on asks, and a No leaves it off");
        check(!exists(P(P(rig.base, L"launcher"), L"launcher.ini")) || !contains(readAll(P(P(rig.base, L"launcher"), L"launcher.ini")), "direct_launch"), "and writes nothing");
        rig.play(L"Steam", true, false);
        rig.ui.pump([&]() { return !rig.session->playing() && rig.rec.uris.size() == 1; });
        check(rig.http.requests.empty() && rig.started.empty(), "so Play after a No is the Steam route");
        check(!rig.session->setDirectLaunch(true, nullptr), "with no way to ask, it stays off");
        check(rig.session->setDirectLaunch(true, [&]() { ++asked; return true; }) && asked == 2 && rig.session->directLaunch(), "a Yes turns it on");
        LauncherState state;
        state.load(P(P(rig.base, L"launcher"), L"launcher.ini"), nullptr);
        check(state.get("direct_launch") == "on" && state.directLaunch(), "and launcher.ini says on");
        check(rig.session->setDirectLaunch(true, [&]() { ++asked; return false; }) && asked == 2, "turning it on when it is on does not ask again");
        check(!rig.session->setDirectLaunch(false, [&]() { ++asked; return false; }) && asked == 2 && !rig.session->directLaunch(), "turning it off does not ask");
        state.load(P(P(rig.base, L"launcher"), L"launcher.ini"), nullptr);
        check(state.get("direct_launch") == "off" && !state.directLaunch(), "and launcher.ini says off");
        const std::string question = kDirectLaunchQuestion;
        check(question.find("Start Elite directly instead of through Frontier's launcher?") == 0 && contains(question, "No password or token is stored.") && contains(question, "Frontier has not approved this.") &&
                  contains(question, "Untick this at any time to go back to Frontier's launcher.") && question.size() > 11 && question.substr(question.size() - 11) == "Turn it on?",
              "the question says what the setting does and that Frontier has not approved it");
    }
}

// --direct-dry-run: the sign-in as far as the command line, masked, with nothing started.
void testDryRun() {
    section("direct: --direct-dry-run");
    const std::wstring base = fresh("direct-dry");
    const GameTree game = makeGame(base);
    RigKeys keys = rigKeys(L"dry", true);
    AppArgs a;
    a.directDryRun = true;
    a.dir = game.product;
    a.vr = true;

    {
        FrontierHttp http;
        scriptHappy(&http);
        // The machine ID turns up in the game's own arguments here, so the mask has something of each kind to take out.
        http.script[kPurchasesPath] = {jsonReply(purchasesJson(std::string("--id ") + kExpectedMachineId, "/Zz"))};
        std::vector<std::string> out;
        resetEvents();
        const int code = directDryRunLines(a, directEnv(&http, keys.keys, nullptr), &out);
        const std::string all = joined(out);
        check(code == 0 && !out.empty(), "a good sign-in gives a plan and exit code 0: " + std::to_string(code));
        check(!leaks(all), "no token, ticket or machine ID is anywhere in what is printed");
        check(!out.empty() && out.back() == "    " + narrowUtf8(P(game.product, L"EliteDangerous64.exe")) + " \"ServerToken <masked> <masked> /Zz\" /steam /vr --id <masked>",
              "the last line is the program and its arguments, every token and the machine ID as <masked>: " + (out.empty() ? std::string() : out.back()));
        check(contains(all, "Purchases: 2, fields: product_sku, product_name, directory") && contains(all, "Version on disk: 4.2.1.0, Frontier's: 4.2.1.0"), "it says what it found, as a person comparing it with min-ed-launcher needs");
        const std::vector<std::string> tags = eventTags();
        check(eventIndex(tags, "ticket") >= 0 && eventIndex(tags, "unload") >= 0 && eventIndex(tags, "START") < 0 && http.requests.size() == 4, "the whole sign-in ran, Steam was closed down, and nothing was started: " + joined(tags));
        a.vr = false;
        a.novr = true;
        out.clear();
        FrontierHttp again;
        scriptHappy(&again);
        check(directDryRunLines(a, directEnv(&again, keys.keys, nullptr), &out) == 0 && !out.empty() && contains(out.back(), "/steam /novr /edo --foo") && !contains(out.back(), "/steam /vr"), "--novr prints /novr");
        a.vr = true;
        a.novr = false;
    }
    {
        FrontierHttp http;
        scriptHappy(&http);
        http.script[kAuthPath] = {statusReply(403)};
        std::vector<std::string> out;
        const int code = directDryRunLines(a, directEnv(&http, keys.keys, nullptr), &out);
        check(code == 3 && !out.empty() && contains(out.back(), "Direct launch did not work: Frontier refused the request (HTTP 403)") && !leaks(joined(out)), "a fallback prints its reason and exits 3: " + (out.empty() ? std::string() : out.back()));
    }
    {
        int refused = 0;
        for (int which = 0; which < 3; ++which) {
            AppArgs bad = a;
            if (which == 0) bad.dir.clear();
            if (which == 1) bad.vr = bad.novr = false;
            if (which == 2) bad.vr = bad.novr = true;
            FrontierHttp http;
            scriptHappy(&http);
            std::vector<std::string> out;
            if (directDryRunLines(bad, directEnv(&http, keys.keys, nullptr), &out) == 2 && http.requests.empty() && out.size() == 1 && contains(out[0], "--direct-dry-run needs --dir")) ++refused;
        }
        check(refused == 3, "no folder, neither of --vr and --novr, or both: exit 2, with one line, and nothing contacted (" + std::to_string(refused) + " of 3)");
    }
    dropKeys(keys);
}

// ---- Frontier's own store: the stored login, the sign-in, the dialogs, the dry run --------------------------

std::string pathsOf(FrontierHttp& http) {
    std::string out;
    for (const HttpRequest& r : http.requests) out += (out.empty() ? "" : " ") + pathOf(r.url);
    return out;
}

// The sign-in's pieces, from the production code's own hex so a flight's URL and the rig agree.
std::string hexed(const char* text) { return hexOfBytes(text); }

void testLoginFile() {
    section("frontier login file");
    const std::wstring dir = fresh("login-file");
    const std::wstring path = P(dir, L"launcher\\frontier-login.dat");

    check(saveLogin(path, kPassword, kMachineToken), "a login is written, creating the folder");
    StoredLogin got;
    check(loadFrontierLogin(path, &got) && got.email == kEmail && got.password == kPassword && got.machineToken == kMachineToken, "and reads back whole: DPAPI round trip for the password and the machine token");
    const std::string bytes = readAll(path);
    check(bytes.find("EDUP-FRONTIER-LOGIN 1\n") == 0 && contains(bytes, std::string("email=") + kEmail), "the file says what it is and holds the email in the clear");
    check(!contains(bytes, kPassword) && !contains(bytes, kMachineToken) && !contains(bytes, hexed(kPassword)) && !contains(bytes, hexed(kMachineToken)), "and neither the password nor the machine token, in the clear or as hex, is anywhere in its bytes");
    check(bytes.size() > 200, "the blobs are there: the file is not just a header and an email (" + std::to_string(bytes.size()) + " bytes)");

    // Not remembering the password leaves it out altogether.
    check(saveLogin(path, nullptr, kMachineToken) && !contains(readAll(path), "password=") && contains(readAll(path), "machine_token="), "no password to remember: no password line is written");
    check(loadFrontierLogin(path, &got) && got.email == kEmail && got.password.empty() && got.machineToken == kMachineToken, "and it reads back as an email and a token");
    check(saveLogin(path, kPassword, nullptr) && loadFrontierLogin(path, &got) && got.password == kPassword && got.machineToken.empty(), "a login with no token yet reads back as one");

    // Damage reads as no login at all, and leaves nothing in the result.
    saveLogin(path, kPassword, kMachineToken);
    const std::string whole = readAll(path);
    {
        // A digit near the end of the blob, where DPAPI keeps its signature.
        std::string tampered = whole;
        const size_t at = tampered.find('\n', tampered.find("password=")) - 4;
        tampered[at] = tampered[at] == 'a' ? 'b' : 'a';
        writeAll(path, tampered);
        StoredLogin dirty;
        dirty.email = "left over";
        check(!loadFrontierLogin(path, &dirty) && dirty.email.empty() && dirty.password.empty() && dirty.machineToken.empty(), "a changed byte inside the encrypted password is no login");
    }
    {
        std::string tampered = whole;
        const size_t at = tampered.find('\n', tampered.find("machine_token=")) - 4;
        tampered[at] = tampered[at] == '0' ? '1' : '0';
        writeAll(path, tampered);
        check(!loadFrontierLogin(path, &got), "nor is one inside the encrypted token");
    }
    writeAll(path, whole.substr(0, whole.size() - 12));
    check(!loadFrontierLogin(path, &got), "a file cut short is no login");
    writeAll(path, "not a login file\nemail=x\n");
    check(!loadFrontierLogin(path, &got), "a file with another header is no login");
    writeAll(path, whole + "extra=1\n");
    check(!loadFrontierLogin(path, &got), "a line this version did not write is no login");
    writeAll(path, std::string("EDUP-FRONTIER-LOGIN 1\n") + "password=00\n");
    check(!loadFrontierLogin(path, &got), "a header and a password that does not decrypt, with no email, is no login");
    writeAll(path, std::string());
    check(!loadFrontierLogin(path, &got), "an empty file is no login");
    check(!loadFrontierLogin(P(dir, L"launcher\\missing.dat"), &got), "a missing file is no login");

    // Encrypted without this program's entropy: the same user, another program's blob.
    {
        const std::string secret = "SENTINEL-NO-ENTROPY";
        DATA_BLOB in;
        in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()));
        in.cbData = static_cast<DWORD>(secret.size());
        DATA_BLOB out{};
        check(CryptProtectData(&in, L"rig", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) != FALSE, "the rig can protect a blob of its own");
        std::string hex;
        static const char digits[] = "0123456789abcdef";
        for (DWORD i = 0; i < out.cbData; ++i) {
            hex.push_back(digits[out.pbData[i] >> 4]);
            hex.push_back(digits[out.pbData[i] & 15]);
        }
        LocalFree(out.pbData);
        writeAll(path, std::string("EDUP-FRONTIER-LOGIN 1\nemail=") + kEmail + "\npassword=" + hex + "\n");
        check(!loadFrontierLogin(path, &got), "a blob protected without the launcher's entropy string does not open");
    }

    // Writing and forgetting.
    check(!saveFrontierLogin(path, StoredLogin(), nullptr), "an empty login is not written");
    StoredLogin breaking;
    breaking.email = "a@b\nc@d";
    check(!saveFrontierLogin(path, breaking, nullptr), "an email with a line break is not written");
    saveLogin(path, kPassword, kMachineToken);
    check(forgetFrontierSecrets(path) && loadFrontierLogin(path, &got) && got.email == kEmail && got.password.empty() && got.machineToken.empty() && !contains(readAll(path), "password=") && !contains(readAll(path), "machine_token="),
          "forgetting keeps the email and drops the password and the token");
    check(deleteFrontierLogin(path) && !exists(path) && deleteFrontierLogin(path), "deleting removes the file, and is fine when it is already gone");
    std::string error;
    check(!saveFrontierLogin(L"", StoredLogin{kEmail, "", ""}, &error) && !contains(error, kEmail), "a login that cannot be written says so without a value: " + error);
    std::string wiped = "secret";
    wipeString(&wiped);
    check(wiped.empty(), "wipeString leaves an empty string");

    check(contains(loginPrivacyLine(false), "stored encrypted for this Windows user") && !contains(loginPrivacyLine(false), "Wine"), "the line under the form says the password is stored encrypted for this Windows user");
    check(contains(loginPrivacyLine(true), "Wine or Proton") && contains(loginPrivacyLine(true), "not strongly protected") && !contains(loginPrivacyLine(true), "encrypted for this Windows user"), "and under Wine or Proton that the saved password is not strongly protected");
    check(contains(loginPrivacyLine(false), "only to Frontier's servers") && contains(loginPrivacyLine(true), "only to Frontier's servers"), "both say the password goes only to Frontier's servers");
    LoginForm form;
    check(!loginFormComplete(form) && !loginFormComplete(LoginForm{"a@b", "", true}) && !loginFormComplete(LoginForm{"", "pw", true}) && loginFormComplete(LoginForm{"a@b", "pw", false}), "a form is complete only with an email and a password");
}

void testStoreSession() {
    section("frontier store: the session");
    endFakes();
    setSteamMode("");
    const std::string flagVr = "/autorun /autoquit /edo /vr", flagFlat = "/autorun /autoquit /edo /novr";

    // The first sign-in on a PC: auth, then a code, then the token, then auth again with it.
    {
        DirectRig rig("store-first", "direct_launch=on\n", true);
        scriptStore(&rig.http, true);
        rig.loginAnswers = {{true, kEmail, kPassword, true}};
        rig.codeAnswers = {{true, kCode}};
        rig.play(L"Frontier launcher", true, false);
        check(rig.session->playing() && contains(rig.session->playLine(), "Signing in"), "Play marks the session busy at once: " + rig.session->playLine());
        check(rig.ui.pump([&]() { return rig.restored == 1; }), "the window comes back when the game exits");
        check(pathsOf(rig.http) == kTimePath + " " + kFAuthPath + " " + kFTokenPath + " " + kFAuthPath + " " + kPurchasesPath + " " + kInstallerPath, "six calls, in order: time, auth, token, auth, purchases, installer: " + pathsOf(rig.http));
        if (rig.http.requests.size() == 6) {
            const HttpRequest& first = rig.http.requests[1];
            const HttpRequest& token = rig.http.requests[2];
            const HttpRequest& second = rig.http.requests[3];
            const HttpRequest& purchases = rig.http.requests[4];
            const HttpRequest& installer = rig.http.requests[5];
            for (int i = 1; i <= 3; ++i) {
                const HttpRequest& r = rig.http.requests[static_cast<size_t>(i)];
                check(r.method == "POST" && r.headers.empty() && headerOf(r, "Authorization") == "<none>" && !r.followRedirects && r.maxBytes == kFrontierMaxBytes && r.url.compare(0, std::strlen(kFrontier), kFrontier) == 0,
                      "call " + std::to_string(i) + " is a POST with no Authorization header, no redirect followed and a 4 MiB cap: " + pathOf(r.url));
            }
            check(queryNames(first.url) == "email,password,machineId,lang,fTime" && queryValue(first.url, "email") == hexed(kEmail) && queryValue(first.url, "password") == hexed(kPassword) && queryValue(first.url, "machineId") == kExpectedMachineId &&
                      queryValue(first.url, "lang") == "en",
                  "first auth: email, password (both as hex of their bytes), machineId, lang, fTime: " + queryNames(first.url));
            check(queryNames(token.url) == "machineId,plainCode,encCode,lang" && queryValue(token.url, "plainCode") == kCode && queryValue(token.url, "encCode") == kEncCode && queryValue(token.url, "machineId") == kExpectedMachineId,
                  "token: machineId, plainCode (as typed), encCode (as Frontier sent it), lang: " + queryNames(token.url));
            check(queryNames(second.url) == "email,password,machineId,machineToken,lang,fTime" && queryValue(second.url, "machineToken") == kMachineToken && queryValue(second.url, "email") == hexed(kEmail) && queryValue(second.url, "password") == hexed(kPassword),
                  "second auth: email, password, machineId, machineToken, lang, fTime, with the token the code earned: " + queryNames(second.url));
            check(purchases.method == "GET" && queryNames(purchases.url) == "fTime,authToken,machineToken" && queryValue(purchases.url, "authToken") == kAuthToken && queryValue(purchases.url, "machineToken") == kMachineToken && headerOf(purchases, "Authorization") == "<none>",
                  "purchases: fTime, authToken, machineToken in the query and no Authorization header: " + queryNames(purchases.url));
            check(installer.method == "GET" && queryNames(installer.url) == "machineToken,authToken,machineId,sku,os,fTime" && queryValue(installer.url, "sku") == "FORC-FDEV-DO-38-IBE" && headerOf(installer, "Authorization") == "<none>",
                  "installer: the same query as on Steam, and no header: " + queryNames(installer.url));
        }
        check(rig.started.size() == 1 && rig.started[0].exe == P(rig.game.product, L"EliteDangerous64.exe") && rig.started[0].dir == rig.game.product, "the game is started once, from its own folder");
        const std::string args = rig.started.empty() ? std::string() : narrowUtf8(rig.started[0].args);
        check(args == std::string("\"ServerToken ") + kMachineToken + " " + kAuthToken + " /Zz\" /vr /edo --foo", "the command line has no /steam, the token the code earned (not the one the server's reply names) and the auth token: " + args);
        check(!contains(args, "/steam") && !contains(args, kServerMachineToken), "no /steam, and the server's own machine token is not used");
        check(eventIndex(eventTags(), "load") < 0 && eventIndex(eventTags(), "init") < 0 && eventIndex(eventTags(), "START") >= 0, "Steam's library was never loaded, and the game was started: " + joined(eventTags()));
        check(rig.rec.uris.empty() && rig.rec.spawns.empty() && rig.minimised == 1 && rig.ui.closed == 0, "no store address and no EDLaunch, and the window stepped aside once and was not closed");
        check(rig.loginNotes.size() == 1 && rig.codeNotes.size() == 1 && rig.loginNotes[0].empty() && rig.codeNotes[0].empty() && rig.loginEmails[0].empty() && rig.codeEmails[0] == kEmail,
              "the login was asked for once, with nothing known, and the code once, naming the email it went to");
        check(rig.askedOnWindowThread.size() == 2 && rig.askedOnWindowThread[0] && rig.askedOnWindowThread[1], "both questions were asked on the window's thread, not the worker's");
        StoredLogin saved;
        check(loadFrontierLogin(rig.loginPath, &saved) && saved.email == kEmail && saved.password == kPassword && saved.machineToken == kMachineToken, "frontier-login.dat holds the email, the password and the token");
        const std::string bytes = readAll(rig.loginPath);
        check(!contains(bytes, kPassword) && !contains(bytes, kMachineToken) && !contains(bytes, kAuthToken) && !contains(bytes, kCode) && !contains(bytes, kEncCode), "and none of them, nor the code, is readable in its bytes");
        check(!rig.anyLineLeaks() && !treeHas(rig.base, frontierNeedles(false)) && !treeHasSkipping(rig.base, frontierNeedles(true), L"frontier-login.dat"),
              "no line the band showed (" + std::to_string(rig.seen.size()) + ") and no file under the data root holds a sentinel, the login file holding only the email");
        check(treeHas(rig.base, {kEmail}), "the control: the scan does see the email where it is meant to be, in the login file");
        LauncherState state;
        state.load(P(P(rig.base, L"launcher"), L"launcher.ini"), nullptr);
        check(state.mode(rig.game.product) == "vr" && state.directLaunch(), "VR is remembered for the install, and the setting is still on");
    }

    // A later launch: the stored login, one call, and nothing asked.
    {
        DirectRig rig("store-later", "direct_launch=on\n", true);
        scriptStore(&rig.http, false);
        saveLogin(rig.loginPath, kPassword, kMachineToken);
        rig.play(L"Frontier launcher", false, false);
        check(rig.ui.pump([&]() { return rig.restored == 1; }), "a later launch starts the game");
        check(pathsOf(rig.http) == kTimePath + " " + kFAuthPath + " " + kPurchasesPath + " " + kInstallerPath, "four calls: one sign-in, as the password is sent every time: " + pathsOf(rig.http));
        const HttpRequest* auth = rig.http.nth(kFAuthPath);
        check(auth && auth->method == "POST" && queryNames(auth->url) == "email,password,machineId,machineToken,lang,fTime" && queryValue(auth->url, "password") == hexed(kPassword) && queryValue(auth->url, "machineToken") == kMachineToken,
              "it is the call with the machine token, and the password goes with it");
        check(rig.loginNotes.empty() && rig.codeNotes.empty(), "nothing was asked");
        check(!rig.started.empty() && narrowUtf8(rig.started[0].args) == std::string("\"ServerToken ") + kMachineToken + " " + kAuthToken + " /Zz\" /novr /edo --foo", "flat is /novr, and still no /steam");
        StoredLogin saved;
        check(loadFrontierLogin(rig.loginPath, &saved) && saved.password == kPassword && saved.machineToken == kMachineToken, "the stored login is kept");
        check(!rig.anyLineLeaks() && !treeHasSkipping(rig.base, frontierNeedles(true), L"frontier-login.dat"), "no sentinel in a line or another file");
    }

    // The password was not remembered: the token is, so the window asks for the password only, and no code is needed.
    {
        DirectRig rig("store-forget-password", "direct_launch=on\n", true);
        scriptStore(&rig.http, true);
        rig.loginAnswers = {{true, kEmail, kPassword, false}};
        rig.codeAnswers = {{true, kCode}};
        rig.play(L"Frontier launcher", true, false);
        check(rig.ui.pump([&]() { return rig.restored == 1; }), "a first run that does not remember the password still starts the game");
        check(rig.http.count(kFAuthPath) == 2 && rig.http.count(kFTokenPath) == 1 && rig.started.size() == 1, "after the whole first-time sequence: " + pathsOf(rig.http));
        StoredLogin saved;
        check(loadFrontierLogin(rig.loginPath, &saved) && saved.email == kEmail && saved.password.empty() && saved.machineToken == kMachineToken && !contains(readAll(rig.loginPath), "password="), "only the email and the token are stored");
    }
    {
        DirectRig rig("store-token-only", "direct_launch=on\n", true);
        scriptStore(&rig.http, false);
        saveLogin(rig.loginPath, nullptr, kMachineToken);
        rig.loginAnswers = {{true, kEmail, kPassword, true}};
        rig.play(L"Frontier launcher", true, false);
        check(rig.ui.pump([&]() { return rig.restored == 1; }), "a stored token with no password asks for the password and starts the game");
        check(rig.loginNotes.size() == 1 && rig.loginEmails[0] == kEmail && rig.loginRemembers[0] == false && rig.codeNotes.empty(), "the form came up with the email filled in, the box as the user left it, and no code asked");
        check(pathsOf(rig.http) == kTimePath + " " + kFAuthPath + " " + kPurchasesPath + " " + kInstallerPath, "the token was used with the typed password: one sign-in call: " + pathsOf(rig.http));
        StoredLogin saved;
        check(loadFrontierLogin(rig.loginPath, &saved) && saved.password == kPassword && saved.machineToken == kMachineToken, "and ticking the box this time stored the password");
    }

    // A saved login Frontier denies: the password and token are forgotten first, then the window asks again.
    {
        DirectRig rig("store-denied", "direct_launch=on\n", true);
        scriptStore(&rig.http, false);
        rig.http.script[kFAuthPath] = {statusReply(403), encReply(), frontierAuthReply()};
        saveLogin(rig.loginPath, "SENTINEL-OLD-PASSWORD-0001", "SENTINEL-OLD-TOKEN-0002");
        rig.loginAnswers = {{true, kEmail, kPassword, true}};
        rig.codeAnswers = {{true, kCode}};
        rig.play(L"Frontier launcher", true, false);
        check(rig.ui.pump([&]() { return rig.restored == 1; }), "after a denied saved login, a new sign-in starts the game");
        check(pathsOf(rig.http) == kTimePath + " " + kFAuthPath + " " + kFAuthPath + " " + kFTokenPath + " " + kFAuthPath + " " + kPurchasesPath + " " + kInstallerPath, "the denied call, then the whole first-time sequence: " + pathsOf(rig.http));
        check(rig.loginNotes.size() == 1 && contains(rig.loginNotes[0], "did not accept the saved sign-in") && rig.loginEmails[0] == kEmail, "the window said the saved sign-in was refused, with the email it had");
        check(rig.fileWhenAsked.size() == 1 && rig.fileWhenAsked[0].email == kEmail && rig.fileWhenAsked[0].password.empty() && rig.fileWhenAsked[0].machineToken.empty(), "and by then the stored password and token were gone and the email was kept");
        const HttpRequest* denied = rig.http.nth(kFAuthPath, 0);
        check(denied && queryValue(denied->url, "machineToken") == "SENTINEL-OLD-TOKEN-0002" && queryValue(denied->url, "password") == hexed("SENTINEL-OLD-PASSWORD-0001"), "the denied call carried the saved password and token");
        const HttpRequest* again = rig.http.nth(kFAuthPath, 1);
        check(again && queryNames(again->url) == "email,password,machineId,lang,fTime" && queryValue(again->url, "password") == hexed(kPassword), "the next call is a first sign-in with the new password");
        StoredLogin saved;
        check(loadFrontierLogin(rig.loginPath, &saved) && saved.password == kPassword && saved.machineToken == kMachineToken, "and the new login is what is stored");
    }

    // A password Frontier refuses on a first sign-in: asked again, with a note.
    {
        DirectRig rig("store-bad-password", "direct_launch=on\n", true);
        scriptStore(&rig.http, true);
        rig.http.script[kFAuthPath] = {statusReply(400), encReply(), frontierAuthReply()};
        rig.loginAnswers = {{true, kEmail, "SENTINEL-WRONG-PASSWORD", true}, {true, kEmail, kPassword, true}};
        rig.codeAnswers = {{true, kCode}};
        rig.play(L"Frontier launcher", true, false);
        check(rig.ui.pump([&]() { return rig.restored == 1; }), "a second try after a refused password starts the game");
        check(rig.loginNotes.size() == 2 && rig.loginNotes[0].empty() && contains(rig.loginNotes[1], "did not accept that email and password") && rig.loginEmails[1] == kEmail, "the second form carried a note and the email: " + (rig.loginNotes.size() > 1 ? rig.loginNotes[1] : std::string()));
        check(rig.codeNotes.size() == 1, "only the good password led to a code");
    }

    // A refused code, then a good one: the same encCode is used for both.
    {
        DirectRig rig("store-bad-code", "direct_launch=on\n", true);
        scriptStore(&rig.http, true);
        rig.http.script[kFTokenPath] = {statusReply(403), tokenReply()};
        rig.loginAnswers = {{true, kEmail, kPassword, true}};
        rig.codeAnswers = {{true, kBadCode}, {true, kCode}};
        rig.play(L"Frontier launcher", true, false);
        check(rig.ui.pump([&]() { return rig.restored == 1; }), "a refused code and then a good one start the game");
        check(rig.codeNotes.size() == 2 && rig.codeNotes[0].empty() && contains(rig.codeNotes[1], "did not accept that code"), "the second code window said the first was refused: " + (rig.codeNotes.size() > 1 ? rig.codeNotes[1] : std::string()));
        check(rig.http.count(kFTokenPath) == 2 && queryValue(rig.http.nth(kFTokenPath, 0)->url, "plainCode") == kBadCode && queryValue(rig.http.nth(kFTokenPath, 1)->url, "plainCode") == kCode &&
                  queryValue(rig.http.nth(kFTokenPath, 1)->url, "encCode") == kEncCode && rig.http.count(kFAuthPath) == 2,
              "the token call was made twice with the two codes and one encCode, and the first auth call was not repeated");
        check(rig.started.size() == 1 && !rig.anyLineLeaks(), "the game started once and no line leaks");
    }

    // Cancel at each prompt: nothing is started, and nothing else is tried.
    {
        DirectRig rig("store-cancel-login", "direct_launch=on\n", true);
        scriptStore(&rig.http, true);
        rig.play(L"Frontier launcher", true, false);
        check(rig.ui.pump([&]() { return !rig.session->playing(); }), "cancelling the login ends the launch");
        check(rig.session->playLine() == "Sign-in cancelled." && rig.started.empty() && rig.rec.uris.empty() && rig.rec.spawns.empty() && rig.minimised == 0 && rig.ui.closed == 0, "the line says so, and nothing is started or opened: " + rig.session->playLine());
        check(rig.http.count(kFAuthPath) == 0 && rig.http.count(kFTokenPath) == 0 && rig.http.count(kPurchasesPath) == 0 && !exists(rig.loginPath), "no sign-in call was made and nothing was stored");
    }
    {
        DirectRig rig("store-cancel-code", "direct_launch=on\n", true);
        scriptStore(&rig.http, true);
        rig.loginAnswers = {{true, kEmail, kPassword, true}};
        rig.play(L"Frontier launcher", true, false);
        check(rig.ui.pump([&]() { return !rig.session->playing(); }), "cancelling the code ends the launch");
        check(rig.session->playLine() == "Sign-in cancelled." && rig.started.empty() && rig.rec.uris.empty() && rig.rec.spawns.empty(), "the line says so, and nothing is started or opened");
        check(rig.http.count(kFAuthPath) == 1 && rig.http.count(kFTokenPath) == 0 && rig.http.count(kPurchasesPath) == 0 && !exists(rig.loginPath), "only the first sign-in call was made, no token was asked for, and nothing was stored");
    }
    {
        DirectRig rig("store-cancel-reask", "direct_launch=on\n", true);
        scriptStore(&rig.http, false);
        rig.http.script[kFAuthPath] = {statusReply(401)};
        saveLogin(rig.loginPath, kPassword, kMachineToken);
        rig.play(L"Frontier launcher", true, false);
        check(rig.ui.pump([&]() { return !rig.session->playing(); }), "cancelling the window that follows a denied saved login ends the launch");
        StoredLogin saved;
        check(rig.session->playLine() == "Sign-in cancelled." && rig.started.empty() && rig.rec.spawns.empty() && loadFrontierLogin(rig.loginPath, &saved) && saved.email == kEmail && saved.password.empty() && saved.machineToken.empty(),
              "the line says so, nothing is started, and the stored password and token stay forgotten");
    }

    // On the code step only a refusal asks for the code again. Anything else ends the sign-in once, with
    // its own reason, and Frontier's route follows: no user is told a good code was wrong.
    {
        struct CodeCase {
            const char* name;
            Reply reply;
            const char* reason;
        };
        const std::vector<CodeCase> cases = {
            {"a 404", statusReply(404), "does not know"},
            {"a redirect", statusReply(302), "redirect"},
            {"an unavailable server", statusReply(503), "unavailable"},
            {"a reply with no token", jsonReply("{\"nothing\":1}"), "carried no machine token"},
            {"a web page", htmlReply(), "web page"},
        };
        int ran = 0;
        for (const CodeCase& c : cases) {
            DirectRig rig(std::string("store-code-fails-") + std::to_string(++ran), "direct_launch=on\n", true);
            scriptStore(&rig.http, true);
            rig.http.script[kFTokenPath] = {c.reply, tokenReply()};
            rig.loginAnswers = {{true, kEmail, kPassword, true}};
            rig.codeAnswers = {{true, kCode}, {true, kCode}};
            rig.play(L"Frontier launcher", true, false);
            check(rig.ui.pump([&]() { return rig.rec.spawns.size() == 1 && !rig.session->playing(); }), std::string(c.name) + " on the code: Frontier's route is taken");
            check(rig.codeNotes.size() == 1 && rig.http.count(kFTokenPath) == 1 && rig.started.empty(),
                  std::string(c.name) + " on the code: the code was asked for once and sent once, and no game was started directly (asked " +
                      std::to_string(rig.codeNotes.size()) + ")");
            bool said = false;
            for (const std::string& line : rig.seen) said = said || (contains(line, "Direct launch did not work") && contains(line, c.reason));
            check(said, std::string(c.name) + " on the code: the band's line gives that reason, not a wrong code");
        }
    }

    // Every fallback lands on Frontier's own route, which carries the VR or flat flag.
    {
        struct Case {
            const char* name;
            std::function<void(DirectRig&)> arrange;
            const char* reason;
        };
        const std::vector<Case> cases = {
            {"no answer", [](DirectRig& r) { r.http.script[kFAuthPath] = {noAnswer("no route")}; }, "could not be reached"},
            {"an unavailable server", [](DirectRig& r) { r.http.script[kPurchasesPath] = {statusReply(503)}; }, "unavailable"},
            {"a refused purchases call", [](DirectRig& r) { r.http.script[kPurchasesPath] = {statusReply(403)}; }, "refused the request (HTTP 403)"},
            {"the web page twice", [](DirectRig& r) { r.http.script[kFAuthPath] = {htmlReply(), htmlReply()}; }, "web page instead of a sign-in reply"},
            {"a redirect on sign-in", [](DirectRig& r) { r.http.script[kFAuthPath] = {statusReply(302)}; }, "redirect"},
            {"a redirect on the version", [](DirectRig& r) { r.http.script[kInstallerPath] = {statusReply(307)}; }, "redirect"},
            {"a game that needs patching", [](DirectRig& r) { r.http.script[kInstallerPath] = {jsonReply("{\"version\":\"9.9.9.9\"}")}; }, "game needs patching"},
            {"a reply with no token", [](DirectRig& r) { r.http.script[kFAuthPath] = {jsonReply("{\"nothing\":1}")}; }, "carried no token"},
            {"a product that is not listed", [](DirectRig& r) { r.http.script[kPurchasesPath] = {jsonReply("{\"purchases\":[]}")}; }, "not among the products"},
        };
        int ran = 0;
        for (const Case& c : cases) {
            const bool vr = ran++ % 2 == 0;
            DirectRig rig(std::string("store-fallback-") + std::to_string(ran), "direct_launch=on\n", true);
            scriptStore(&rig.http, false);
            saveLogin(rig.loginPath, kPassword, kMachineToken);
            c.arrange(rig);
            rig.play(L"Frontier launcher", vr, false);
            check(rig.ui.pump([&]() { return rig.rec.spawns.size() == 1 && !rig.session->playing(); }), std::string(c.name) + ": Frontier's route is taken and its watcher ends");
            check(rig.rec.spawns.size() == 1 && rig.rec.spawns[0].exe == rig.game.edlaunch && narrowUtf8(rig.rec.spawns[0].args) == (vr ? flagVr : flagFlat) && rig.rec.uris.empty(),
                  std::string(c.name) + ": EDLaunch is started with the " + (vr ? "VR" : "flat") + " flag: " + (rig.rec.spawns.empty() ? std::string() : narrowUtf8(rig.rec.spawns[0].args)));
            check(rig.started.empty() && rig.ui.closed == 0 && eventIndex(eventTags(), "START") < 0, std::string(c.name) + ": no game is started directly");
            bool said = false, steamNamed = false;
            for (const std::string& line : rig.seen) {
                if (!contains(line, "Direct launch did not work")) continue;  // the watcher's later lines name Steam among the stores
                said = said || (contains(line, c.reason) && contains(line, "Frontier's launcher") && contains(line, "VR or flat choice"));
                steamNamed = steamNamed || contains(line, "Steam");
            }
            check(said && !steamNamed, std::string(c.name) + ": the band's line said why, and that Frontier's launcher follows with the choice, without a word about Steam");
            // A sign-in that worked is written again (the encryption differs each time); what it holds must not change.
            StoredLogin kept;
            check(rig.loginNotes.empty() && rig.codeNotes.empty() && loadFrontierLogin(rig.loginPath, &kept) && kept.email == kEmail && kept.password == kPassword && kept.machineToken == kMachineToken,
                  std::string(c.name) + ": nothing was asked and the stored login still holds what it did");
            LauncherState state;
            state.load(P(P(rig.base, L"launcher"), L"launcher.ini"), nullptr);
            check(state.mode(rig.game.product) == (vr ? "vr" : "flat"), std::string(c.name) + ": the choice is remembered for the install");
            check(!rig.anyLineLeaks() && !treeHasSkipping(rig.base, frontierNeedles(true), L"frontier-login.dat"), std::string(c.name) + ": no sentinel in a line or a file");
        }
        check(ran == 9, "every fallback case was run through the session (" + std::to_string(ran) + ")");
    }

    // A session that cannot ask: a Frontier-store install with no window stops where it would have asked.
    {
        DirectRig rig("store-no-window", "direct_launch=on\n", true);
        scriptStore(&rig.http, true);
        // The rig's session was built with questions; one built without them cannot ask.
        SessionEnv e;
        e.roots.dataRoot = rig.base;
        e.roots.canonical = false;
        e.selfExe = P(rig.base, L"self.exe");
        e.keys = g_testKeys;
        e.makeHttp = [&rig]() { return std::unique_ptr<HttpClient>(new Share(&rig.http)); };
        std::mutex m;
        std::vector<std::function<void()>> queue;
        e.post = [&](std::function<void()> f) {
            std::lock_guard<std::mutex> l(m);
            queue.push_back(std::move(f));
        };
        e.frontierBase = kFrontier;
        e.machineKeys = rig.keys.keys;
        e.clock.unixNow = []() { return 1700000000ll; };
        LauncherSession quiet(e);
        quiet.beginPlay(L"Frontier launcher", rig.game.product, true, false, rig.rec.ops(), WatchNames(), WatchTiming());
        const DWORD end = GetTickCount() + 10000;
        while (quiet.playing() && static_cast<int>(GetTickCount() - end) < 0) {
            std::vector<std::function<void()>> jobs;
            {
                std::lock_guard<std::mutex> l(m);
                jobs.swap(queue);
            }
            for (auto& j : jobs) j();
            Sleep(5);
        }
        check(!quiet.playing() && contains(quiet.playLine(), "Sign in once from the launcher's window"), "with no window to ask in, the launch stops and says to sign in from the window: " + quiet.playLine());
        check(rig.http.count(kFAuthPath) == 0 && rig.rec.spawns.empty() && rig.rec.uris.empty() && rig.started.empty(), "and no sign-in call is made and nothing is started");
        quiet.shutdown();
    }

    // Only the Steam and Frontier-store sources go direct; Epic and a picked folder do not, even with the key on.
    {
        DirectRig rig("store-other-sources", "direct_launch=on\n", true);
        scriptStore(&rig.http, false);
        WatchNames names;
        names.game = kFakeGame;
        names.edlaunch = kFakeEd;
        WatchTiming timing;
        timing.pollMs = 40;
        timing.startupWaitMs = 200;
        timing.closeWaitMs = 100;
        rig.session->beginPlay(L"Folder you chose", rig.game.product, true, false, rig.rec.ops(), names, timing);
        rig.session->shutdown();
        check(rig.rec.spawns.size() == 1 && rig.http.requests.empty() && rig.started.empty(), "a folder the user picked takes EDLaunch's route and contacts nothing");
    }

    // Elevated: refused before anything is contacted.
    {
        DirectRig rig("store-elevated", "direct_launch=on\n", true);
        scriptStore(&rig.http, true);
        rig.loginAnswers = {{true, kEmail, kPassword, true}};
        rig.play(L"Frontier launcher", true, true);
        check(!rig.session->playing() && rig.session->playLine() == elevatedRefusal() && rig.http.requests.empty() && rig.loginNotes.empty() && rig.started.empty() && rig.rec.spawns.empty(), "an elevated launcher refuses before it contacts or asks anything");
    }

    // Unticking deletes the login; the question for a Frontier install says so and what it stores.
    {
        DirectRig rig("store-untick", "direct_launch=on\n", true);
        saveLogin(rig.loginPath, kPassword, kMachineToken);
        check(exists(rig.loginPath) && rig.session->directLaunch(), "the login is stored and the setting is on");
        check(!rig.session->setDirectLaunch(false, nullptr) && !exists(rig.loginPath), "unticking turns it off and deletes frontier-login.dat");
        LauncherState state;
        state.load(P(P(rig.base, L"launcher"), L"launcher.ini"), nullptr);
        check(state.get("direct_launch") == "off", "and launcher.ini says off");
        int asked = 0;
        check(rig.session->setDirectLaunch(true, [&]() { ++asked; return true; }) && asked == 1 && !exists(rig.loginPath), "turning it on again asks and stores nothing");
        saveLogin(rig.loginPath, kPassword, nullptr);
        check(!rig.session->setDirectLaunch(false, nullptr) && !exists(rig.loginPath), "and unticking deletes the file again");
        const std::string q = kDirectLaunchQuestionFrontier;
        check(q.find("Start Elite directly instead of through Frontier's launcher?") == 0 && contains(q, "ask for your Frontier email and password") && contains(q, "code Frontier emails you, once on this PC") &&
                  contains(q, "encrypted for this Windows user") && contains(q, "Frontier has not approved this.") && contains(q, "unticking also deletes the stored login") && q.substr(q.size() - 11) == "Turn it on?",
              "the Frontier question says it will ask for the login, may need an emailed code once, stores it encrypted, that Frontier has not approved this, and that unticking deletes it");
        check(!contains(std::string(kDirectLaunchQuestion), "email and password") && contains(std::string(kDirectLaunchQuestion), "No password or token is stored."), "the Steam question is unchanged");
    }
}

// The two real windows, hidden, driven from a helper thread by posted keys and messages.
struct DialogDrive {
    std::atomic<bool> returned{false};
};

// FindWindowW sees the dialog from inside its WM_CREATE, before createControls has made its boxes, and a helper
// that went on then typed into null windows (four failures in a loaded build, 2026-11-08). A message sent from
// this thread is answered only once the dialog's thread is back in its message loop, when every control exists.
HWND waitForDialog() {
    const DWORD end = GetTickCount() + 20000;
    while (static_cast<int>(GetTickCount() - end) < 0) {
        HWND w = FindWindowW(kFrontierDialogClass, nullptr);
        if (w) {
            DWORD_PTR ignored = 0;
            return SendMessageTimeoutW(w, WM_NULL, 0, 0, SMTO_NORMAL, 20000, &ignored) ? w : nullptr;
        }
        Sleep(5);
    }
    return nullptr;
}

// The boxes by their control IDs (10 email, 11 password, 12 code), not by where they sit in the window's list.
HWND nthEdit(HWND dialog, int n) { return GetDlgItem(dialog, n == 2 ? 12 : 10 + n); }

// A test that waits for a dialog must not wait forever: once the section has run a minute, every dialog is
// closed. Ten seconds was once within reach of a healthy run in a loaded build, and it closed dialogs under test.
struct DialogWatchdog {
    std::atomic<bool> stop{false};
    std::thread thread;
    DialogWatchdog() {
        thread = std::thread([this]() {
            const DWORD end = GetTickCount() + 60000;
            while (!stop && static_cast<int>(GetTickCount() - end) < 0) Sleep(20);
            while (!stop) {
                HWND w = FindWindowW(kFrontierDialogClass, nullptr);
                if (w) PostMessageW(w, WM_CLOSE, 0, 0);
                Sleep(50);
            }
        });
    }
    ~DialogWatchdog() {
        stop = true;
        thread.join();
    }
};

void typeInto(HWND edit, const wchar_t* text) { SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text)); }
void pressKey(HWND to, WPARAM key) { PostMessageW(to, WM_KEYDOWN, key, 0); }

void testLoginDialog() {
    section("frontier dialogs");
    ui::buildFonts(96);
    DialogWatchdog watchdog;
    FrontierDialogOptions options;
    options.visible = false;  // nobody's desktop is disturbed: the window exists and takes messages, hidden

    // Return on an empty form does nothing; so does Sign in; Return on a complete one signs in.
    {
        DialogDrive drive;
        int styleOk = 0;
        bool stillThereAfterEmptyReturn = false, stillThereAfterEmptyClick = false, stillThereAfterOneField = false, okDisabledWhenEmpty = false, okEnabledWhenComplete = false;
        std::thread helper([&]() {
            HWND dialog = waitForDialog();
            if (!dialog) return;
            HWND email = nthEdit(dialog, 0), password = nthEdit(dialog, 1);
            styleOk = (!(GetWindowLongPtrW(email, GWL_STYLE) & ES_PASSWORD) ? 1 : 0) + ((GetWindowLongPtrW(password, GWL_STYLE) & ES_PASSWORD) ? 1 : 0);
            HWND ok = FindWindowExW(dialog, nullptr, L"BUTTON", L"Sign in");
            okDisabledWhenEmpty = ok && !IsWindowEnabled(ok);
            pressKey(email, VK_RETURN);
            Sleep(250);
            stillThereAfterEmptyReturn = IsWindow(dialog) && !drive.returned;
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(1, BN_CLICKED), reinterpret_cast<LPARAM>(ok));
            Sleep(250);
            stillThereAfterEmptyClick = IsWindow(dialog) && !drive.returned;
            typeInto(email, L"  cmdr@example.invalid ");
            pressKey(password, VK_RETURN);
            Sleep(250);
            stillThereAfterOneField = IsWindow(dialog) && !drive.returned;
            typeInto(password, L"made-up-password");
            Sleep(50);
            okEnabledWhenComplete = ok && IsWindowEnabled(ok);
            pressKey(password, VK_RETURN);
        });
        LoginForm form;
        form.remember = true;
        const bool signedIn = showFrontierLogin(options, std::string(), &form);
        drive.returned = true;
        helper.join();
        check(styleOk == 2, "the email box is plain and the password box is masked");
        check(okDisabledWhenEmpty && okEnabledWhenComplete, "Sign in is unavailable until both boxes have something in them");
        check(stillThereAfterEmptyReturn && stillThereAfterEmptyClick && stillThereAfterOneField, "Return on an empty form, Sign in on one, and Return with only the email do not submit it");
        check(signedIn && form.email == "cmdr@example.invalid" && form.password == "made-up-password" && form.remember, "Return on a complete form signs in, with the email trimmed and Remember ticked by default");
    }
    // Escape cancels, and the form is left as it was; the email it started with is in the box.
    {
        std::wstring shown;
        std::thread helper([&]() {
            HWND dialog = waitForDialog();
            if (!dialog) return;
            HWND email = nthEdit(dialog, 0);
            wchar_t text[128]{};
            GetWindowTextW(email, text, 128);
            shown = text;
            typeInto(nthEdit(dialog, 1), L"typed-then-abandoned");
            pressKey(email, VK_ESCAPE);
        });
        LoginForm form;
        form.email = "known@example.invalid";
        const bool signedIn = showFrontierLogin(options, "Frontier did not accept the saved sign-in.", &form);
        helper.join();
        check(!signedIn && form.password.empty() && form.email == "known@example.invalid", "Escape cancels, and nothing typed is handed back");
        check(shown == L"known@example.invalid", "the email the launcher knew was already in the box");
    }
    // Remember can be unticked; a window closed with its close box is a cancel.
    {
        std::thread helper([&]() {
            HWND dialog = waitForDialog();
            if (!dialog) return;
            typeInto(nthEdit(dialog, 0), L"a@b.invalid");
            typeInto(nthEdit(dialog, 1), L"pw");
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(3, BN_CLICKED), 0);
            pressKey(nthEdit(dialog, 1), VK_RETURN);
        });
        LoginForm form;
        const bool signedIn = showFrontierLogin(options, std::string(), &form);
        helper.join();
        check(signedIn && !form.remember && form.email == "a@b.invalid" && form.password == "pw", "unticking Remember is handed back");
        std::thread closer([&]() {
            HWND dialog = waitForDialog();
            if (dialog) PostMessageW(dialog, WM_CLOSE, 0, 0);
        });
        LoginForm other;
        check(!showFrontierLogin(options, std::string(), &other), "closing the window is a cancel");
        closer.join();
    }
    // The code window.
    {
        bool stillThere = false;
        std::atomic<bool> returned{false};
        std::thread helper([&]() {
            HWND dialog = waitForDialog();
            if (!dialog) return;
            HWND code = nthEdit(dialog, 2);
            pressKey(code, VK_RETURN);
            Sleep(250);
            stillThere = IsWindow(dialog) && !returned;
            typeInto(code, L"  123456 ");
            pressKey(code, VK_RETURN);
        });
        std::string code;
        const bool accepted = showFrontierCode(options, "cmdr@example.invalid", std::string(), &code);
        returned = true;
        helper.join();
        check(stillThere && accepted && code == "123456", "an empty code is not submitted, and a typed one comes back trimmed");
        std::thread escaper([&]() {
            HWND dialog = waitForDialog();
            if (dialog) pressKey(nthEdit(dialog, 2), VK_ESCAPE);
        });
        std::string none = "unchanged";
        check(!showFrontierCode(options, "x@y.invalid", "Frontier did not accept that code.", &none) && none == "unchanged", "Escape cancels the code window, and hands back nothing");
        escaper.join();
    }
    check(FindWindowW(kFrontierDialogClass, nullptr) == nullptr, "no dialog window is left behind");
}

// A bundle built from a folder layout that holds the stored login and a launcher log never contains either.
void testBundleLeavesLoginOut() {
    section("log bundle and the login");
    const std::wstring base = fresh("bundle-login");
    const std::wstring launcher = P(base, L"EDVR\\launcher");
    const std::wstring game = P(base, L"Game");
    saveLogin(P(launcher, L"frontier-login.dat"), kPassword, kMachineToken);
    writeAll(P(launcher, L"ed-up-launcher.log"), std::string("a launcher log that names ") + kEmail + " and " + kAuthToken + "\n");
    writeAll(P(game, L"EliteDangerous64.exe"), "x");
    writeAll(P(game, L"edvr_logs\\edvr_vr_20260827_140003.log"), "SENTINEL-CONTROL-IN-A-REAL-LOG\n");
    // The login and the launcher log in the places a bundle looks: the game folder and its log folder.
    const std::string loginBytes = readAll(P(launcher, L"frontier-login.dat")), logBytes = readAll(P(launcher, L"ed-up-launcher.log"));
    for (const wchar_t* where : {L"frontier-login.dat", L"edvr_logs\\frontier-login.dat"}) writeAll(P(game, where), loginBytes);
    for (const wchar_t* where : {L"ed-up-launcher.log", L"edvr_logs\\ed-up-launcher.log"}) writeAll(P(game, where), logBytes);
    const DumpSearch noDumps{P(base, L"nodumps_local"), P(base, L"nodumps_temp")};
    const std::wstring out = P(base, L"out");
    createDirectories(out);
    const LogBundle bundle = collectLogs(game, out, &noDumps);
    check(bundle.ok && exists(bundle.zipPath), "the bundle is written: " + bundle.error);
    std::string joinedNames;
    for (const std::wstring& n : bundle.included) joinedNames += narrowUtf8(n) + "\n";
    const std::string zip = bundle.ok ? readAll(bundle.zipPath) : std::string();
    check(contains(zip, "SENTINEL-CONTROL-IN-A-REAL-LOG"), "the control: a sentinel in a log the bundle does take is readable in the zip, so the scan below can see");
    check(contains(joinedNames, "edvr_vr_20260827_140003.log"), "and the session's log is in it");
    check(!contains(joinedNames, "frontier-login") && !contains(joinedNames, "ed-up-launcher.log") && !contains(joinedNames, "launcher"), "no entry is the stored login or a launcher log: " + joinedNames);
    check(!contains(zip, kEmail) && !contains(zip, kAuthToken) && !contains(zip, "EDUP-FRONTIER-LOGIN"), "and neither file's contents are anywhere in the zip");
    // The settings can send logs to any folder, the launcher's own included.
    const std::wstring game2 = P(base, L"Game2");
    writeAll(P(game2, L"EliteDangerous64.exe"), "x");
    writeAll(P(game2, L"edvr.ini"), "[log]\r\ndir = " + narrowUtf8(launcher) + "\r\n");
    writeAll(P(launcher, L"edvr_vr_20260827_140003.log"), "SENTINEL-CONTROL-IN-THE-LAUNCHER-FOLDER\n");
    const LogBundle second = collectLogs(game2, out, &noDumps);
    std::string names2;
    for (const std::wstring& n : second.included) names2 += narrowUtf8(n) + "\n";
    const std::string zip2 = second.ok ? readAll(second.zipPath) : std::string();
    check(second.ok && contains(zip2, "SENTINEL-CONTROL-IN-THE-LAUNCHER-FOLDER"), "with the logs sent to the launcher's folder its log is taken, as the control");
    check(!contains(names2, "frontier-login") && !contains(names2, "ed-up-launcher.log") && !contains(zip2, kEmail) && !contains(zip2, kAuthToken) && !contains(zip2, "EDUP-FRONTIER-LOGIN"), "and still the stored login and the launcher log are not: " + names2);
}

// --direct-dry-run on a Frontier-store folder: the stored login only, never asked, never changed.
void testStoreDryRun() {
    section("frontier store: --direct-dry-run");
    const std::wstring base = fresh("store-dry");
    const GameTree game = makeFrontierStore(base);
    RigKeys keys = rigKeys(L"store-dry", true);
    const std::wstring loginPath = P(base, L"launcher\\frontier-login.dat");
    AppArgs a;
    a.directDryRun = true;
    a.dir = game.product;
    a.vr = true;
    int asked = 0;
    auto env = [&](FrontierHttp* http) {
        DirectEnv e = directEnv(http, keys.keys, nullptr);
        e.loginPath = loginPath;
        // A caller that could answer. A dry run must never reach them.
        e.prompts.askLogin = [&asked](const std::string&, LoginForm* form) {
            ++asked;
            form->email = kEmail;
            form->password = kPassword;
            return true;
        };
        e.prompts.askCode = [&asked](const std::string&, const std::string&, std::string* code) {
            ++asked;
            *code = kCode;
            return true;
        };
        return e;
    };

    {
        FrontierHttp http;
        scriptStore(&http, false);
        http.script[kPurchasesPath] = {jsonReply(purchasesJson(std::string("--id ") + kExpectedMachineId + " --mail " + kEmail, "/Zz"))};
        saveLogin(loginPath, kPassword, kMachineToken);
        const std::string before = readAll(loginPath);
        std::vector<std::string> out;
        resetEvents();
        const int code = directDryRunLines(a, env(&http), &out);
        const std::string all = joined(out);
        check(code == 0 && out.size() >= 3, "a stored login gives a plan and exit code 0: " + std::to_string(code) + " " + all);
        check(!out.empty() && contains(out[0], "Frontier's own store"), "the first line says which kind of install it took this for: " + (out.empty() ? std::string() : out[0]));
        check(!leaks(all), "no token, machine ID, email or password is anywhere in what is printed");
        check(!out.empty() && out.back() == "    " + narrowUtf8(P(game.product, L"EliteDangerous64.exe")) + " \"ServerToken <masked> <masked> /Zz\" /vr --id <masked> --mail <masked>",
              "the last line is the program and its arguments, with no /steam and every token, the machine ID and the email as <masked>: " + (out.empty() ? std::string() : out.back()));
        check(pathsOf(http) == kTimePath + " " + kFAuthPath + " " + kPurchasesPath + " " + kInstallerPath && eventTags().size() == 4 && eventIndex(eventTags(), "START") < 0 && eventIndex(eventTags(), "load") < 0,
              "one sign-in call, Steam never loaded, nothing started: " + pathsOf(http));
        check(asked == 0 && readAll(loginPath) == before, "nothing was asked and the stored login was not touched");
    }
    {
        // Flat.
        FrontierHttp http;
        scriptStore(&http, false);
        saveLogin(loginPath, kPassword, kMachineToken);
        AppArgs flat = a;
        flat.vr = false;
        flat.novr = true;
        std::vector<std::string> out;
        check(directDryRunLines(flat, env(&http), &out) == 0 && !out.empty() && contains(out.back(), " /novr /edo --foo") && !contains(out.back(), "/steam"), "--novr prints /novr");
    }
    {
        // No stored login at all: it says so, asks nothing, contacts nothing beyond the clock.
        FrontierHttp http;
        scriptStore(&http, true);
        deleteFrontierLogin(loginPath);
        std::vector<std::string> out;
        const int code = directDryRunLines(a, env(&http), &out);
        check(code == 3 && !out.empty() && contains(out.back(), "Sign in once from the launcher's window") && !contains(out.back(), "Direct launch did not work") && !leaks(joined(out)),
              "with no stored login it stops and says to sign in once from the window: " + (out.empty() ? std::string() : out.back()));
        check(asked == 0 && http.count(kFAuthPath) == 0 && http.count(kFTokenPath) == 0 && !exists(loginPath), "and asks nothing, makes no sign-in call and stores nothing");
    }
    {
        // A login with no password (not remembered): the same stop.
        FrontierHttp http;
        scriptStore(&http, false);
        saveLogin(loginPath, nullptr, kMachineToken);
        std::vector<std::string> out;
        check(directDryRunLines(a, env(&http), &out) == 3 && !out.empty() && contains(out.back(), "Sign in once from the launcher's window") && http.count(kFAuthPath) == 0 && asked == 0, "a stored login with no password stops the same way");
    }
    {
        // A password but no token yet: a dry run must not make Frontier email a code.
        FrontierHttp http;
        scriptStore(&http, true);
        saveLogin(loginPath, kPassword, nullptr);
        std::vector<std::string> out;
        const int code = directDryRunLines(a, env(&http), &out);
        check(code == 3 && !out.empty() && contains(out.back(), "emailed code") && http.count(kFAuthPath) == 0 && http.count(kFTokenPath) == 0 && asked == 0, "a login with no token stops before the call that would email a code: " + (out.empty() ? std::string() : out.back()));
    }
    {
        // Denied: the dry run reports it and leaves the stored login alone.
        FrontierHttp http;
        scriptStore(&http, false);
        http.script[kFAuthPath] = {statusReply(403)};
        saveLogin(loginPath, kPassword, kMachineToken);
        const std::string before = readAll(loginPath);
        std::vector<std::string> out;
        const int code = directDryRunLines(a, env(&http), &out);
        check(code == 3 && !out.empty() && contains(out.back(), "did not accept the saved login") && !leaks(joined(out)) && asked == 0 && readAll(loginPath) == before && http.count(kFAuthPath) == 1,
              "a denied login exits 3 with one call, asks nothing, and the stored login is unchanged: " + (out.empty() ? std::string() : out.back()));
    }
    {
        // Any other failure is a fallback line for Frontier's launcher, not Steam.
        FrontierHttp http;
        scriptStore(&http, false);
        http.script[kPurchasesPath] = {statusReply(503)};
        saveLogin(loginPath, kPassword, kMachineToken);
        std::vector<std::string> out;
        const int code = directDryRunLines(a, env(&http), &out);
        check(code == 3 && !out.empty() && contains(out.back(), "Direct launch did not work: Frontier's servers are unavailable (HTTP 503)") && contains(out.back(), "Frontier's launcher") && !contains(out.back(), "Steam"), "an unavailable server is a fallback to Frontier's launcher: " + (out.empty() ? std::string() : out.back()));
    }
    dropKeys(keys);
}

// ---- the command line ---------------------------------------------------------------------------

AppArgs parse(std::vector<std::wstring> words) {
    std::vector<wchar_t*> argv;
    std::wstring prog = L"ed-up-launcher.exe";
    argv.push_back(&prog[0]);
    for (std::wstring& w : words) argv.push_back(&w[0]);
    return parseArgs(static_cast<int>(argv.size()), argv.data());
}

void testArgs() {
    section("args");
    AppArgs a = parse({L"--payload", L"D:\\x", L"--data-root", L"E:\\y", L"--channel", L"rc", L"--check-update", L"--update", L"--unregister", L"--updated", L"--apply-plan", L"F:\\plan.txt"});
    check(!a.badArg && a.payloadDir == L"D:\\x" && a.dataRoot == L"E:\\y" && a.channel == "rc" && a.checkUpdate && a.update && a.unregister && a.updated && a.planFile == L"F:\\plan.txt",
          "every launcher flag is read");
    check(launcherHasCommand(parse({L"--check-update"})) && launcherHasCommand(parse({L"--update"})) && launcherHasCommand(parse({L"--unregister"})) && !launcherHasCommand(parse({L"--install"})) &&
              !launcherHasCommand(parse({})),
          "the commands that need no window are --check-update, --update and --unregister");
    const AppArgs dry = parse({L"--direct-dry-run", L"--dir", L"D:\\Elite\\Products\\p", L"--vr"});
    const AppArgs dryFlat = parse({L"--direct-dry-run", L"--dir", L"D:\\Elite\\Products\\p", L"--novr"});
    check(!dry.badArg && dry.directDryRun && dry.vr && !dry.novr && dry.dir == L"D:\\Elite\\Products\\p" && dryFlat.novr && !dryFlat.vr && launcherHasCommand(dry),
          "--direct-dry-run, --dir and --vr or --novr are read, and the dry run needs no window");
    const AppArgs licences = parse({L"--licenses"});
    check(!licences.badArg && licences.licenses && launcherHasCommand(licences) && !parse({}).licenses && !parse({}).directDryRun, "--licenses is a command with no window");
    check(parse({L"--channel", L"nightly"}).badArg && parse({L"--payload"}).badArg && parse({L"--data-root"}).badArg, "a bad channel and flags missing their value are refused");
    check(parse({L"--channel", L"stable"}).channel == "stable", "stable is accepted");
    const std::string usage = usageText();
    for (const char* flag : {"--check-update", "--update", "--channel", "--payload", "--data-root", "--unregister", "--direct-dry-run", "--licenses", "<masked>", "ed-up-launcher.exe"})
        check(contains(usage, flag), std::string("the help names ") + flag);
    check(!contains(usage, "edvr-installer.exe"), "and speaks of the launcher, not the installer");
    check(launcherRootsFor(parse({})).canonical && !launcherRootsFor(parse({L"--data-root", L"E:\\y"})).canonical && launcherMirrorRoot(parse({L"--data-root", L"E:\\y"})) == L"E:\\y",
          "--data-root moves the roots, the mirror included, and marks a trial");
    std::string note;
    check(!launcherPreparePayload(parse({L"--payload", L"Z:\\nowhere"}), &note) && !note.empty(), "a payload folder that is not there gives no payload and a note: " + note);
}

}  // namespace

int main(int argc, char** argv) {
    // The stand-in for the elevated copy: applyElevated starts this program with --apply-plan.
    if (argc == 5 && std::strcmp(argv[1], "--apply-plan") == 0 && std::strcmp(argv[3], "--key") == 0) {
        const char* keys[2] = {argv[4], nullptr};
        return applyPlanFile(widenUtf8(argv[2]), keys);
    }
    if (argc < 5) {
        std::printf("usage: launcher_test <repo root> <fixtures dir> <scratch dir> <fakes dir>\n");
        return 2;
    }
    g_fixtures = widenUtf8(argv[2]);
    g_scratch = widenUtf8(argv[3]);
    g_fakes = widenUtf8(argv[4]);
    {
        wchar_t self[MAX_PATH * 2]{};
        g_self.assign(self, GetModuleFileNameW(nullptr, self, MAX_PATH * 2));
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    rmtree(g_scratch);
    if (!createDirectories(g_scratch)) {
        std::printf("launcher_test: cannot create the scratch folder\n");
        return 2;
    }
    {
        std::string key = readAll(P(g_fixtures, L"test_key.txt"));
        g_testKey = key;
        g_otherKey = readAll(P(g_fixtures, L"other_key.txt"));
        g_testKeys[0] = g_testKey.c_str();
        if (g_testKey.size() != 128 || g_otherKey.size() != 128) {
            std::printf("launcher_test: the fixtures are missing; run tools\\launcher_fixtures.py first\n");
            return 2;
        }
    }
    std::printf("launcher_test: fixtures %ls\n", g_fixtures.c_str());

    testJson();
    testSigning();
    testShapes();
    testVersions();
    testZip();
    testCache();
    testState();
    testRememberedChoice();
    testUpdater();
    testPayload();
    testHttp();
    testCore();
    testRegister();
    testSelfUpdate();
    testPlanFile();
    testElevatedApply();
    testRoute();
    testWatcher();
    testSession();
    testMachineId();
    testLayoutDpi();
    testDirectPlan();
    testDirectSession();
    testDryRun();
    testLoginFile();
    testStoreSession();
    testLoginDialog();
    testBundleLeavesLoginOut();
    testStoreDryRun();
    testArgs();
    endFakes();
    SetEnvironmentVariableW(L"EDUP_FAKE_STEAM_LOG", nullptr);

    std::printf("\nlauncher_test: %d checks, %d failed\n", g_checks, g_fails);
    std::printf("%s\n", g_fails == 0 ? "launcher_test: all good" : "launcher_test: FAILURES");
    return g_fails == 0 ? 0 : 1;
}
