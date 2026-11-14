#pragma once

#include "../../src/common/mirror_location.h"
#include "../../src/openxr/seat_store.h"

// Included after the installer rig's scratch-file and assertion helpers.
static void testEpicDetection(const std::wstring& scratch) {
    printf("\nEpic manifests and a directly selected product\n");
    const std::wstring root = joinPath(scratch, L"epic-detection");
    removeTree(root);
    const std::wstring manifests = joinPath(root, L"manifests");
    makeTree(manifests);
    const std::wstring customRoot = joinPath(root, L"Archive 42");
    const std::wstring customProduct = joinPath(customRoot, L"Products\\elite-dangerous-odyssey-64");
    const std::wstring unicodeRoot = joinPath(root, L"Vacances \u00e9");
    const std::wstring unicodeProduct = joinPath(unicodeRoot, L"Products\\odyssey-64");
    makeTree(customProduct);
    makeTree(unicodeProduct);
    check(writeAll(joinPath(customProduct, L"EliteDangerous64.exe"), "fixture") &&
          writeAll(joinPath(unicodeProduct, L"EliteDangerous64.exe"), "fixture"),
          "the custom and Unicode product fixtures have the game executable");
    std::string unicodeJson = jsonString(unicodeRoot);
    const size_t accent = unicodeJson.find("\xc3\xa9");
    check(accent != std::string::npos, "the Unicode fixture contains the accented path character");
    if (accent == std::string::npos) return;
    unicodeJson.replace(accent, 2, "\\u00e9");
    check(writeAll(joinPath(manifests, L"custom.item"),
                   "\xef\xbb\xbf{\"DisplayName\":\"Space adventure\",\"InstallLocation\":" + jsonString(customRoot) + "}") &&
          writeAll(joinPath(manifests, L"unicode.item"),
                   "{\"InstallLocation\":" + unicodeJson + "}"),
          "a BOM-prefixed manifest and a Unicode-escaped manifest name the custom locations");

    // Invalid manifests point at a real executable, so accepting them would
    // visibly add another product. A stale record has no executable at all.
    const std::wstring invalidProduct = joinPath(root, L"invalid-record-game");
    makeTree(invalidProduct);
    check(writeAll(joinPath(invalidProduct, L"EliteDangerous64.exe"), "fixture"),
          "the invalid-manifest fixtures point at an existing game executable");
    const std::string invalidPath = jsonString(invalidProduct);
    check(writeAll(joinPath(manifests, L"malformed.item"), "{\"InstallLocation\":" + invalidPath) &&
          writeAll(joinPath(manifests, L"wrong-type.item"), "{\"InstallLocation\":7,\"other\":" + invalidPath + "}") &&
          writeAll(joinPath(manifests, L"stale.item"), "{\"InstallLocation\":" + jsonString(joinPath(root, L"gone")) + "}"),
          "malformed, wrong-type and stale manifests are present");
    const std::vector<GameInstall> installs = findEpicInstalls(manifests);
    check(installs.size() == 2, "only the two valid manifests with a game executable are discovered");
    bool customFound = false;
    bool unicodeFound = false;
    for (const GameInstall& install : installs) {
        if (install.dir == canonicalPath(customProduct) && install.source == L"Epic") customFound = true;
        if (install.dir == canonicalPath(unicodeProduct) && install.source == L"Epic") unicodeFound = true;
    }
    check(customFound, "an arbitrary install folder and display name still identify the Epic game");
    check(unicodeFound, "a Unicode JSON escape resolves to the actual Epic product folder");

    // The same inventory the scan offered must identify a product picked
    // directly, including equivalent Windows spellings of that exact folder.
    GameInstall selected;
    std::wstring alias = joinPath(customProduct, L".\\");
    for (wchar_t& c : alias) c = static_cast<wchar_t>(towupper(c));
    check(describeDirWithInstalls(alias, L"Folder you chose", installs, &selected) &&
          selected.source == L"Epic", "a directly selected equivalent product keeps its Epic source");
    const std::wstring mirrorRoot = joinPath(root, L"saved-settings");
    const std::wstring expectedMirror = joinPath(mirrorRoot, L"elite-dangerous-odyssey-64-epic");
    check(_wcsicmp(mirrorDirFor(selected, mirrorRoot).c_str(), expectedMirror.c_str()) == 0,
          "the direct product uses the Epic mirror used by the detected install");

    const std::wstring other = joinPath(root, L"other\\elite-dangerous-odyssey-64");
    makeTree(other);
    check(writeAll(joinPath(other, L"EliteDangerous64.exe"), "fixture"),
          "the unrelated folder is also a real game product");
    GameInstall unknown;
    check(describeDirWithInstalls(other, L"Folder you chose", installs, &unknown) &&
          unknown.source == L"Folder you chose",
          "another folder with the same product name cannot borrow the Epic source");
    check(describeDirWithInstalls(other, L"Frontier launcher", {}, &unknown) &&
          unknown.source == L"Frontier launcher", "an unmatched explicit store source remains the fallback");
    check(!describeDirWithInstalls(joinPath(root, L"gone"), L"Folder you chose", installs, &unknown),
          "a selected folder without the game executable is still refused");
    removeTree(root);
}

static void testEpicRuntimeMirrors(const std::wstring& scratch) {
    printf("\nthe runtime and repair share the recorded Epic mirror\n");
    const std::wstring root = joinPath(scratch, L"epic-runtime-mirror");
    removeTree(root);
    const std::wstring local = joinPath(root, L"AppData");
    const std::wstring dir = joinPath(root, L"Archive42\\Products\\elite-dangerous-odyssey-64");
    makeTree(local);
    makeTree(stateDirPath(dir));
    const PayloadInfo payload = flatPayloadFor(kFlatTemplate);
    Survey current = installedFlatSurvey(dir, payload);
    current.game.source = L"Epic";
    current.d3d11.sha256 = tsha(payload, "graphics");
    trecord(current.state, "graphics", tsha(payload, "graphics"));
    current.flatIniPresent = true;
    current.flatIniText = kFlatTemplate;
    current.iniPresent = true;
    current.iniText = kSharedIni;
    const std::string oldSettings = "[fix]\r\nshare_exposure = 1\r\n";
    check(writeAll(joinPath(dir, L"EliteDangerous64.exe"), "fixture") &&
          writeAll(joinPath(dir, L"edvr.ini"), oldSettings) &&
          writeAll(statePath(dir), serializeState(current.state)), "the legacy direct-folder install has settings and its old record");
    const std::wstring mirrorRoot = joinPath(local, L"EDVR");
    const std::wstring legacyMirror = joinPath(mirrorRoot, L"elite-dangerous-odyssey-64-folder-you-chose");
    const std::wstring expectedMirror = joinPath(mirrorRoot, L"elite-dangerous-odyssey-64-epic");
    const std::wstring steamMirror = joinPath(mirrorRoot, L"elite-dangerous-odyssey-64-steam");
    check(updateMirrorIni(dir, legacyMirror).ok, "the old generic backup remains available");
    makeTree(steamMirror);
    writeAll(joinPath(steamMirror, L"edvr.ini"), "another install's settings");

    Options options = testOptions();
    options.scope = "flat";
    const std::string beforeRecord = readAll(statePath(dir));
    check(beforeRecord.find("store_source") == std::string::npos,
          "the actual legacy record has no stored source metadata");
    const Plan recordPlan = planInstall(current, options, payload);
    check(!recordPlan.blocked && !recordPlan.nothingToDo,
          "an otherwise current install plans to persist its discovered Epic source");
    planReport(recordPlan);
    check(readAll(statePath(dir)) == beforeRecord && !dirExists(expectedMirror),
          "dry-run planning leaves the install record and mirror directories untouched");
    check(applyPlan(recordPlan, provider(false)).ok, "the source-only install record update applies");
    check(readState(dir).storeSource == L"Epic", "the recorded source is the canonical Epic store");
    check(!fileExists(joinPath(dir, L"d3d11.dll")) && readAll(joinPath(dir, L"edvr.ini")) == oldSettings,
          "the record-only update writes neither a payload nor the live settings");
    current.state = readState(dir);
    check(planInstall(current, options, payload).nothingToDo, "the recorded source needs no further update");
    check(updateMirror(dir, L"", mirrorDirFor(current.game, mirrorRoot)).ok,
          "repair seeds the Epic mirror without deleting the older generic copy");

    const std::wstring runtimeMirror = runtimeMirrorDirFor(local, dir, true);
    check(runtimeMirror == expectedMirror &&
          edvr::openxr::seatMirrorDirFor(local, dir, false) == expectedMirror &&
          edvr::openxr::seatMirrorDirFor(local, dir, true) == expectedMirror,
          "menu writes and seat reads and writes choose the recorded Epic mirror among all three stores");
    MergeReport report;
    const std::string changed = mergeIni(oldSettings, oldSettings, &oldSettings,
                                         {{"fix.share_exposure", "0"}}, &report);
    check(writeFileAtomic(joinPath(dir, L"edvr.ini"), changed) &&
          writeGenerations(runtimeMirror, L"edvr.ini", changed, false),
          "the F8-style edit saves through the actual runtime-selected mirror");
    expectEq(readAll(joinPath(legacyMirror, L"edvr.ini")), "[fix]\r\nshare_exposure = 1\r\n",
             "the previous generic backup is preserved");
    expectEq(readAll(joinPath(steamMirror, L"edvr.ini")), "another install's settings",
             "the Steam backup is preserved");

    removeTree(dir);
    makeTree(dir);
    writeAll(joinPath(dir, L"EliteDangerous64.exe"), "fixture");
    GameInstall repaired;
    check(describeDirWithInstalls(dir, L"Folder you chose", {current.game}, &repaired),
          "the wiped product still resolves to its discovered store");
    std::vector<std::string> notes;
    check(restoreFromMirror(dir, readMirror(mirrorDirFor(repaired, mirrorRoot)), "vr", &notes),
          "repair restores from that same Epic mirror after the game update");
    expectEq(readAll(joinPath(dir, L"edvr.ini")), "[fix]\r\nshare_exposure = 0\r\n",
             "repair recovers the exact latest runtime setting");
    check(readState(dir).storeSource == L"Epic", "the restored record keeps the runtime mirror identity");

    const std::wstring hinted = joinPath(root, L"steamapps\\common\\recorded-product");
    makeTree(stateDirPath(hinted));
    const std::string recorded = "[edvr]\r\nversion = test\r\nstore_source = Epic\r\n";
    writeAll(statePath(hinted), recorded);
    const std::wstring hintedSteam = joinPath(mirrorRoot, L"recorded-product-steam");
    const std::wstring recordedEpic = joinPath(mirrorRoot, L"recorded-product-epic");
    makeTree(hintedSteam);
    check(runtimeMirrorDirFor(local, hinted, false).empty() &&
          edvr::openxr::seatMirrorDirFor(local, hinted, false).empty() &&
          !dirExists(recordedEpic) && readAll(statePath(hinted)) == recorded,
          "a read-only recorded Epic lookup neither borrows the hinted Steam mirror nor creates its missing folder");
    check(runtimeMirrorDirFor(local, hinted, true) == recordedEpic && dirExists(hintedSteam),
          "a recorded Epic store beats a conflicting Steam path hint when creating its exact mirror");
    removeTree(root);
}
