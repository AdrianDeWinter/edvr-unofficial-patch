#pragma once

#include "../../src/common/mirror_location.h"
#include "../../src/openxr/seat_store.h"

static std::string mirrorFixtureTree(const std::wstring& dir) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW entry{};
    HANDLE handle = FindFirstFileW(joinPath(dir, L"*").c_str(), &entry);
    if (handle == INVALID_HANDLE_VALUE) return "missing";
    do {
        if (wcscmp(entry.cFileName, L".") != 0 && wcscmp(entry.cFileName, L"..") != 0)
            names.push_back(entry.cFileName);
    } while (FindNextFileW(handle, &entry));
    FindClose(handle);
    std::sort(names.begin(), names.end());
    std::string tree;
    for (const std::wstring& name : names) {
        const std::wstring path = joinPath(dir, name);
        const bool directory = dirExists(path);
        tree += (directory ? "dir " : "file ") + toUtf8(name) + "\n";
        tree += directory ? mirrorFixtureTree(path) : readAll(path);
        tree += "\n";
    }
    return tree;
}

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
    GameInstall duplicate;
    duplicate.dir = customProduct;
    duplicate.source = L"Steam";
    GameInstall epic = duplicate;
    epic.source = L"Epic";
    check(describeDirWithInstalls(alias, L"Folder you chose", {duplicate, epic}, &selected) &&
          selected.source == L"Steam",
          "duplicate inventory entries keep the first matching source");
    check(describeDirWithInstalls(alias, L"Folder you chose", {epic, duplicate}, &selected) &&
          selected.source == L"Epic",
          "reversing duplicate inventory entries reverses the selected source");
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

    // The current launcher's journal: a header, then one snapshot per line.
    // The last line for an id wins. A Linux row, an unfinished row and a
    // deletion are not installs, and two snapshots of one folder are one install.
    const std::wstring journal = joinPath(root, L"installations.jsonl");
    const std::string journalText =
        "{\"version\":1,\"writtenAt\":\"2026-11-19T00:00:00Z\",\"entries\":5}\n"
        "{\"installationId\":\"side\",\"complete\":false,\"dir\":" + jsonString(customRoot) + ",\"platform\":\"Windows\"}\n"
        "{\"installationId\":\"game\",\"complete\":true,\"dir\":" + jsonString(customRoot) + ",\"platform\":\"Windows\"}\n"
        "{\"installationId\":\"linux\",\"complete\":true,\"dir\":" + jsonString(unicodeRoot) + ",\"platform\":\"Linux\"}\n"
        "{\"installationId\":\"gone\",\"complete\":true,\"dir\":" + jsonString(invalidProduct) + ",\"platform\":\"Windows\"}\n"
        "{\"installationId\":\"gone\",\"deleted\":true}\n"
        "{\"installationId\":\"game\",\"complete\":true,\"dir\":" + jsonString(customRoot) + ",\"platform\":\"Windows\"}\n";
    check(writeAll(journal, journalText), "the journal fixture is written");
    const std::vector<GameInstall> fromJournal = findEpicJournal(journal);
    check(fromJournal.size() == 1 && fromJournal[0].dir == canonicalPath(customProduct) &&
              fromJournal[0].source == L"Epic",
          "the journal offers the one completed Windows install, once");
    check(findEpicJournal(joinPath(root, L"missing.jsonl")).empty(), "a missing journal is no installs");
    check(writeAll(journal, "{\"version\":2,\"entries\":0}\n") && findEpicJournal(journal).empty(),
          "a newer journal version is left unread");
    check(writeAll(journal, "{\"installationId\":\"game\",\"version\":1}\n") && findEpicJournal(journal).empty(),
          "a journal whose first line is an install is left unread");
    check(writeAll(journal,
                   "{\"version\":1}\n{\"installationId\":\"game\",\"complete\":true,\"dir\":" +
                       jsonString(customRoot) + ",\"platform\":\"Windows\"}\n{") &&
              findEpicJournal(journal).empty(),
          "a torn journal is not applied, so the earlier good line does not count");
    removeTree(root);
}

static void testRelabelledLegacyMirror(const std::wstring& scratch) {
    printf("\na source-only relabel preserves recovery from the sole legacy mirror\n");
    for (const wchar_t* source : {L"Steam", L"Epic"}) {
        const std::wstring root = joinPath(scratch, std::wstring(L"relabel-mirror-") + source);
        removeTree(root);
        const std::wstring local = joinPath(root, L"AppData");
        const std::wstring dir = joinPath(root, L"Archive42\\Products\\elite-dangerous-odyssey-64");
        const std::wstring mirrorRoot = joinPath(local, L"EDVR");
        const std::wstring legacy = joinPath(mirrorRoot, L"elite-dangerous-odyssey-64-folder-you-chose");
        const std::wstring canonical = joinPath(mirrorRoot, mirrorFolderName(dir, source));
        makeTree(stateDirPath(dir));
        makeTree(local);
        const std::string tunedVr = "[fix]\r\nshare_exposure = 0\r\n";
        const std::string tunedFlat = "[fix]\r\ntemporal_aa = off\r\n";
        const PayloadInfo payload = flatPayloadFor(tunedFlat);
        Survey current = installedFlatSurvey(dir, payload);
        current.state.edvrVersion = payload.version;
        current.game.source = source;
        current.d3d11.sha256 = tsha(payload, "graphics");
        trecord(current.state, "graphics", tsha(payload, "graphics"));
        current.flatIniPresent = true;
        current.flatIniText = tunedFlat;
        current.iniPresent = true;
        current.iniText = tunedVr;
        const std::string savedSeat = "edvr-seat 1\nfixture headset\t0.125\t0.25\t-0.5\t0\n";
        check(writeAll(joinPath(dir, L"EliteDangerous64.exe"), "fixture") &&
              writeAll(joinPath(dir, L"d3d11.dll"), kFlatGraphicsBytes) &&
              writeAll(joinPath(dir, L"edvr_profile.ini"), "[install]\r\nschema = 2\r\nscope = flat\r\n") &&
              writeAll(joinPath(dir, L"edvr.ini"), tunedVr) &&
              writeAll(joinPath(dir, L"edvr-flat.ini"), tunedFlat) &&
              writeAll(flatBaseIniPath(dir), tunedFlat) &&
              writeAll(statePath(dir), serializeState(current.state)),
              "a current generic-folder installation has both tuned settings files and its legacy record");
        check(updateMirror(dir, L"", legacy).ok && writeAll(joinPath(legacy, L"seat.txt"), savedSeat),
              "only the generic-folder mirror holds the settings and remembered seat");
        const std::string oldRecord = readAll(statePath(dir));
        check(readState(dir).present && readState(dir).storeSource.empty(),
              "the legacy record is valid and predates store_source metadata");
        const std::string before = mirrorFixtureTree(root);
        Options options = testOptions();
        options.scope = "flat";
        const Plan relabel = planInstall(current, options, payload);
        check(!relabel.blocked && !relabel.nothingToDo &&
              !hasStep(relabel, Action::WritePayload, nullptr, nullptr) &&
              !hasStep(relabel, Action::WriteText, nullptr, L"edvr.ini") &&
              !hasStep(relabel, Action::WriteText, nullptr, L"edvr-flat.ini"),
              "discovering the store plans only an install-record update");
        planReport(relabel);
        mirrorDirFor(current.game, mirrorRoot);
        runtimeMirrorDirFor(local, dir, false);
        check(mirrorFixtureTree(root) == before && readAll(statePath(dir)) == oldRecord && !dirExists(canonical),
              "planning, reporting and read-only mirror lookup write nothing");
        check(applyPlan(relabel, provider(false)).ok && readState(dir).storeSource == source,
              "the record-only relabel persists the detected store");
        check(sha256File(joinPath(dir, L"d3d11.dll")) == tsha(payload, "graphics") &&
              readAll(joinPath(dir, L"edvr.ini")) == tunedVr &&
              readAll(joinPath(dir, L"edvr-flat.ini")) == tunedFlat,
              "the relabel leaves the current payload and both live settings untouched");

        const std::wstring menuMirror = runtimeMirrorDirFor(local, dir, true);
        check(menuMirror == legacy && mirrorDirFor(current.game, mirrorRoot) == legacy && !dirExists(canonical),
              "the menu and installer keep the sole generic mirror without creating an empty canonical folder");
        const std::wstring seatMirror = edvr::openxr::seatMirrorDirFor(local, dir, false);
        std::vector<edvr::openxr::SeatRecord> seats;
        check(edvr::openxr::seatBookLoadFile(joinPath(seatMirror, L"seat.txt"), seats) &&
              seats.size() == 1 && seats[0].key == "fixture headset" && seats[0].pose.x == 0.125f &&
              seats[0].pose.y == 0.25f && seats[0].pose.z == -0.5f,
              "seat memory reads the actual remembered position after the relabel");

        // No settings edit or full repair occurs before this update: only the
        // old generic mirror can recover the settings that were already saved.
        removeTree(dir);
        makeTree(dir);
        writeAll(joinPath(dir, L"EliteDangerous64.exe"), "fixture");
        GameInstall repaired;
        check(describeDirWithInstalls(dir, L"Folder you chose", {current.game}, &repaired) && repaired.source == source,
              "the wiped product is rediscovered in the same store inventory");
        std::vector<std::string> notes;
        check(restoreFromMirror(dir, readMirror(mirrorDirFor(repaired, mirrorRoot)), "both", &notes),
              "a wipe immediately after the relabel restores without a seeding install");
        expectEq(readAll(joinPath(dir, L"edvr.ini")), "[fix]\r\nshare_exposure = 0\r\n",
                 "the VR settings saved before the relabel survive the immediate game-folder wipe");
        expectEq(readAll(joinPath(dir, L"edvr-flat.ini")), "[fix]\r\ntemporal_aa = off\r\n",
                 "the flat settings saved before the relabel survive the immediate game-folder wipe");
        const std::string editedVr = "[fix]\r\nshare_exposure = 1\r\n";
        const std::string editedFlat = "[fix]\r\ntemporal_aa = on\r\n";
        check(writeAll(joinPath(dir, L"edvr.ini"), editedVr) &&
              writeAll(joinPath(dir, L"edvr-flat.ini"), editedFlat) &&
              writeGenerations(menuMirror, L"edvr.ini", editedVr, false) &&
              writeGenerations(menuMirror, L"edvr-flat.ini", editedFlat, false),
              "menu-style changes update both profiles through the runtime-selected mirror");

        removeTree(dir);
        makeTree(dir);
        writeAll(joinPath(dir, L"EliteDangerous64.exe"), "fixture");
        check(describeDirWithInstalls(dir, L"Folder you chose", {current.game}, &repaired) && repaired.source == source,
              "the wiped product is rediscovered in the same store inventory");
        notes.clear();
        check(restoreFromMirror(dir, readMirror(mirrorDirFor(repaired, mirrorRoot)), "both", &notes),
              "repair restores directly from the legacy mirror without a seeding install");
        expectEq(readAll(joinPath(dir, L"edvr.ini")), "[fix]\r\nshare_exposure = 1\r\n",
                 "the exact latest VR settings survive the relabel and whole game-folder wipe");
        expectEq(readAll(joinPath(dir, L"edvr-flat.ini")), "[fix]\r\ntemporal_aa = on\r\n",
                 "the exact latest flat settings survive the relabel and whole game-folder wipe");
        expectEq(readAll(joinPath(edvr::openxr::seatMirrorDirFor(local, dir, false), L"seat.txt")), savedSeat,
                 "the remembered seat remains available after the game-folder wipe");

        // A known store never adopts another store's mirror. Even a generic
        // mirror cannot be attributed safely when another candidate exists.
        GameInstall policy = current.game;
        policy.dir = joinPath(root, L"policy-product");
        makeTree(stateDirPath(policy.dir));
        check(writeAll(statePath(policy.dir), "[edvr]\r\nversion = test\r\nstore_source = " + toUtf8(source) + "\r\n"),
              "the ambiguity fixture has an explicit recorded store");
        const std::wstring policyLegacy = joinPath(mirrorRoot, L"policy-product-folder-you-chose");
        const std::wstring policyCanonical = joinPath(mirrorRoot, mirrorFolderName(policy.dir, source));
        const std::wstring foreign = joinPath(mirrorRoot, L"policy-product-frontier-launcher");
        makeTree(policyLegacy);
        makeTree(foreign);
        check(mirrorDirFor(policy, mirrorRoot) == policyCanonical &&
              runtimeMirrorDirFor(local, policy.dir, false).empty() && !dirExists(policyCanonical),
              "ambiguous generic and foreign mirrors are not adopted and a lookup creates nothing");
        removeTree(policyLegacy);
        check(mirrorDirFor(policy, mirrorRoot) == policyCanonical && runtimeMirrorDirFor(local, policy.dir, false).empty(),
              "a known store does not adopt a sole foreign-store mirror");
        makeTree(policyLegacy);
        makeTree(policyCanonical);
        writeAll(joinPath(policyCanonical, L"edvr.ini"), "canonical owner");
        check(mirrorDirFor(policy, mirrorRoot) == policyCanonical &&
              runtimeMirrorDirFor(local, policy.dir, true) == policyCanonical &&
              readAll(joinPath(policyCanonical, L"edvr.ini")) == "canonical owner",
              "an existing canonical mirror retains ownership even alongside legacy and foreign mirrors");
        removeTree(root);
    }
}

static void testLegacyMirrorOwnership(const std::wstring& scratch) {
    printf("\nan adopted generic mirror retains its recorded store ownership\n");
    for (const wchar_t* source : {L"Steam", L"Epic"}) {
        const std::wstring root = joinPath(scratch, std::wstring(L"claimed-mirror-") + source);
        removeTree(root);
        const std::wstring local = joinPath(root, L"AppData");
        const std::wstring mirrorRoot = joinPath(local, L"EDVR");
        GameInstall first;
        first.source = source;
        first.dir = joinPath(root, first.source == L"Steam" ? L"first\\steamapps\\common\\product"
                                                            : L"first\\Epic Games\\product");
        GameInstall second;
        second.dir = joinPath(root, L"second\\Products\\product");
        second.source = first.source == L"Steam" ? L"Epic" : L"Steam";
        const std::wstring legacy = joinPath(mirrorRoot, L"product-folder-you-chose");
        const std::wstring secondCanonical = joinPath(mirrorRoot, mirrorFolderName(second.dir, second.source));
        makeTree(stateDirPath(first.dir));
        makeTree(stateDirPath(second.dir));
        makeTree(local);
        InstallState state;
        state.edvrVersion = "v1";
        state.storeSource = L"Folder you chose";
        check(writeAll(statePath(first.dir), serializeState(state)) &&
              writeAll(joinPath(first.dir, L"edvr.ini"), "[fix]\r\nshare_exposure = 0\r\n") &&
              writeAll(joinPath(first.dir, L"edvr-flat.ini"), "[fix]\r\ntemporal_aa = off\r\n") &&
              updateMirror(first.dir, L"", legacy).ok,
              "a genuinely generic legacy record and both settings files are mirrored");
        state.storeSource = first.source;
        check(writeFileAtomic(statePath(first.dir), serializeState(state)) &&
              mirrorDirFor(first, mirrorRoot) == legacy &&
              updateMirror(first.dir, L"", mirrorDirFor(first, mirrorRoot)).ok &&
              parseState(readAll(joinPath(legacy, L"state.ini"))).storeSource == first.source,
              "the first store adopts the generic mirror and a normal update records its canonical ownership there");
        state.storeSource = second.source;
        check(writeAll(statePath(second.dir), serializeState(state)),
              "the second install has its own canonical store identity");
        const std::string before = mirrorFixtureTree(root);
        check(mirrorDirFor(second, mirrorRoot) == secondCanonical &&
              runtimeMirrorDirFor(local, second.dir, false).empty() &&
              edvr::openxr::seatMirrorDirFor(local, second.dir, false).empty() &&
              mirrorFixtureTree(root) == before && !dirExists(secondCanonical),
              "the second store refuses another store's claimed generic mirror and read-only lookup writes nothing");
        check(mirrorDirFor(first, mirrorRoot) == legacy && runtimeMirrorDirFor(local, first.dir, false) == legacy,
              "the first store keeps access to its claimed generic mirror");
        check(writeAll(joinPath(second.dir, L"edvr.ini"), "[fix]\r\nshare_exposure = 1\r\n") &&
              updateMirror(second.dir, L"", mirrorDirFor(second, mirrorRoot)).ok &&
              parseState(readAll(joinPath(secondCanonical, L"state.ini"))).storeSource == second.source,
              "the second store creates and updates its separate canonical mirror");
        check(mirrorDirFor(first, mirrorRoot) == legacy && runtimeMirrorDirFor(local, first.dir, false) == legacy &&
              edvr::openxr::seatMirrorDirFor(local, first.dir, false) == legacy,
              "matching recorded ownership keeps the first generic mirror accessible beside the other canonical store");

        GameInstall unlabelled;
        unlabelled.dir = joinPath(root, L"chosen\\Products\\product");
        unlabelled.source = L"Folder you chose";
        makeTree(stateDirPath(unlabelled.dir));
        state.storeSource = L"Folder you chose";
        writeAll(statePath(unlabelled.dir), serializeState(state));
        const std::string beforeRefusal = mirrorFixtureTree(root);
        const MirrorInfo empty = readMirror(L"");
        std::vector<std::string> emptyNotes;
        check(!empty.hasIni && !empty.hasFlatIni && settingsToRestore(unlabelled.dir, empty, "both").empty() &&
              !restoreFromMirror(unlabelled.dir, empty, "both", &emptyNotes) &&
              !updateMirror(unlabelled.dir, L"", L"").ok && !updateMirrorIni(unlabelled.dir, L"").ok &&
              mirrorFixtureTree(root) == beforeRefusal,
              "installer mirror readers, restore and both writers safely refuse an empty mirror path without writes");
        check(mirrorDirFor(unlabelled, mirrorRoot).empty() &&
              runtimeMirrorDirFor(local, unlabelled.dir, false).empty() &&
              runtimeMirrorDirFor(local, unlabelled.dir, true).empty() &&
              edvr::openxr::seatMirrorDirFor(local, unlabelled.dir, false).empty() &&
              mirrorFixtureTree(root) == beforeRefusal,
              "Folder you chose cannot bypass a claimed generic mirror's owner through an exact-name match");

        // Issue #225: a browsed or --dir install with an unowned or generic-owned
        // -folder-you-chose mirror beside another store's canonical mirror keeps its own mirror.
        const std::wstring genericRoot = joinPath(root, L"generic-beside-store");
        const std::wstring genericLocal = joinPath(genericRoot, L"AppData");
        const std::wstring genericMirrorRoot = joinPath(genericLocal, L"EDVR");
        const std::wstring genericDir = joinPath(genericRoot, L"Custom\\Products\\product");
        const std::wstring genericMirror = joinPath(genericMirrorRoot, L"product-folder-you-chose");
        const std::wstring steamCanonical = joinPath(genericMirrorRoot, L"product-steam");
        makeTree(stateDirPath(genericDir));
        makeTree(genericMirror);
        makeTree(steamCanonical);
        writeAll(joinPath(genericDir, L"EliteDangerous64.exe"), "fixture");
        InstallState genericState;
        genericState.edvrVersion = "0.22.0";
        genericState.storeSource = L"Folder you chose";
        writeAll(statePath(genericDir), serializeState(genericState));
        GameInstall genericInstall;
        genericInstall.dir = genericDir;
        genericInstall.source = L"Folder you chose";
        // Pre-#219 unowned generic mirror beside a canonical Steam mirror.
        check(mirrorDirFor(genericInstall, genericMirrorRoot) == genericMirror &&
              runtimeMirrorDirFor(genericLocal, genericDir, false) == genericMirror &&
              runtimeMirrorDirFor(genericLocal, genericDir, true) == genericMirror &&
              edvr::openxr::seatMirrorDirFor(genericLocal, genericDir, false) == genericMirror,
              "generic install with unowned -folder-you-chose mirror keeps its mirror beside another store");
        // Generic-owned mirror beside a canonical Steam mirror.
        writeAll(joinPath(genericMirror, L"state.ini"), serializeState(genericState));
        check(mirrorDirFor(genericInstall, genericMirrorRoot) == genericMirror &&
              runtimeMirrorDirFor(genericLocal, genericDir, false) == genericMirror &&
              runtimeMirrorDirFor(genericLocal, genericDir, true) == genericMirror &&
              edvr::openxr::seatMirrorDirFor(genericLocal, genericDir, false) == genericMirror,
              "generic install with generic-owned -folder-you-chose mirror keeps its mirror beside another store");

        removeTree(first.dir);
        makeTree(first.dir);
        writeAll(joinPath(first.dir, L"EliteDangerous64.exe"), "fixture");
        std::vector<std::string> notes;
        check(!fileExists(statePath(first.dir)) && runtimeMirrorDirFor(local, first.dir, false) == legacy &&
              restoreFromMirror(first.dir, readMirror(mirrorDirFor(first, mirrorRoot)), "both", &notes),
              "the first store restores from its claimed generic mirror after a live state and settings wipe");
        expectEq(readAll(joinPath(first.dir, L"edvr.ini")), "[fix]\r\nshare_exposure = 0\r\n",
                 "the owning store recovers the exact VR settings");
        expectEq(readAll(joinPath(first.dir, L"edvr-flat.ini")), "[fix]\r\ntemporal_aa = off\r\n",
                 "the owning store recovers the exact flat settings");
        check(readState(first.dir).storeSource == first.source &&
              runtimeMirrorDirFor(local, first.dir, false) == legacy,
              "the recovered canonical record still resolves to its owning generic mirror");
        removeTree(root);
    }
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

    // A game update can remove state.ini as well as the settings. A path
    // hint cannot borrow a mirror authoritatively bound to another source.
    GameInstall wiped;
    wiped.dir = joinPath(root, L"steamapps\\common\\wiped-product");
    wiped.source = L"Steam";
    makeTree(wiped.dir);
    writeAll(joinPath(wiped.dir, L"EliteDangerous64.exe"), "fixture");
    const std::wstring wipedSteam = joinPath(mirrorRoot, L"wiped-product-steam");
    const std::wstring wipedEpic = joinPath(mirrorRoot, L"wiped-product-epic");
    const std::wstring wipedLegacy = joinPath(mirrorRoot, L"wiped-product-folder-you-chose");
    makeTree(wipedEpic);
    writeAll(joinPath(wipedEpic, L"edvr.ini"), "foreign Epic settings");
    // A slug alone does not bind older mirrors. This fixture represents an
    // owned foreign store, so its installer-authored metadata says so.
    writeAll(joinPath(wipedEpic, L"state.ini"), "[edvr]\r\nversion = test\r\nstore_source = Epic\r\n");
    const std::string beforeForeignRead = mirrorFixtureTree(root);
    check(!fileExists(statePath(wiped.dir)) && mirrorDirFor(wiped, mirrorRoot) == wipedSteam &&
          runtimeMirrorDirFor(local, wiped.dir, false).empty() &&
          edvr::openxr::seatMirrorDirFor(local, wiped.dir, false).empty() &&
          mirrorFixtureTree(root) == beforeForeignRead,
          "after a state wipe a Steam path hint refuses a sole Epic mirror and read-only lookup writes nothing");
    makeTree(wipedLegacy);
    const std::string beforeAmbiguousRead = mirrorFixtureTree(root);
    check(runtimeMirrorDirFor(local, wiped.dir, false).empty() &&
          mirrorDirFor(wiped, mirrorRoot) == wipedSteam && mirrorFixtureTree(root) == beforeAmbiguousRead,
          "an inferred source refuses ambiguous generic and foreign mirrors without creating a folder");
    removeTree(wipedLegacy);
    check(runtimeMirrorDirFor(local, wiped.dir, true) == wipedSteam && dirExists(wipedSteam) &&
          edvr::openxr::seatMirrorDirFor(local, wiped.dir, true) == wipedSteam &&
          readAll(joinPath(wipedEpic, L"edvr.ini")) == "foreign Epic settings",
          "an inferred Steam create makes its canonical folder and preserves the foreign mirror");
    removeTree(wipedSteam);
    removeTree(wipedEpic);
    makeTree(wipedLegacy);
    writeAll(joinPath(wipedLegacy, L"edvr.ini"), "legacy settings");
    const std::string beforeLegacyRead = mirrorFixtureTree(root);
    check(mirrorDirFor(wiped, mirrorRoot) == wipedLegacy &&
          runtimeMirrorDirFor(local, wiped.dir, false) == wipedLegacy &&
          runtimeMirrorDirFor(local, wiped.dir, true) == wipedLegacy &&
          edvr::openxr::seatMirrorDirFor(local, wiped.dir, false) == wipedLegacy &&
          mirrorFixtureTree(root) == beforeLegacyRead && !dirExists(wipedSteam),
          "an inferred Steam source reuses the sole generic mirror in place for reads and writes");
    makeTree(wipedSteam);
    writeAll(joinPath(wipedSteam, L"edvr.ini"), "canonical Steam settings");
    check(mirrorDirFor(wiped, mirrorRoot) == wipedSteam &&
          runtimeMirrorDirFor(local, wiped.dir, false) == wipedSteam &&
          runtimeMirrorDirFor(local, wiped.dir, true) == wipedSteam &&
          edvr::openxr::seatMirrorDirFor(local, wiped.dir, false) == wipedSteam &&
          readAll(joinPath(wipedSteam, L"edvr.ini")) == "canonical Steam settings" &&
          readAll(joinPath(wipedLegacy, L"edvr.ini")) == "legacy settings",
          "an inferred Steam source keeps canonical ownership when a legacy mirror also exists");
    removeTree(root);
}

static void testLegacyRuntimeHint(const std::wstring& scratch) {
    printf("\na path hint preserves recovery from the sole unbound legacy mirror\n");
    const std::wstring root = joinPath(scratch, L"legacy-runtime-hint");
    removeTree(root);
    const std::wstring local = joinPath(root, L"AppData");
    const std::wstring game = joinPath(root, L"Epic Games\\legacy-game");
    const std::wstring mirrorRoot = joinPath(local, L"EDVR");
    const std::wstring legacy = joinPath(mirrorRoot, L"legacy-game-steam");
    const std::wstring canonical = joinPath(mirrorRoot, L"legacy-game-epic");
    makeTree(game);
    makeTree(legacy);
    check(writeAll(joinPath(legacy, L"state.ini"), "[edvr]\r\nversion = v0.22.0\r\n") &&
          writeAll(joinPath(legacy, L"edvr.ini"), "[fix]\r\nshare_exposure = 0\r\n") &&
          writeAll(joinPath(legacy, L"edvr-flat.ini"), "[fix]\r\ntemporal_aa = off\r\n") &&
          writeAll(joinPath(legacy, L"seat.txt"), "edvr-seat 1\nlegacy headset\t0.125\t0.25\t-0.5\t0\n"),
          "a pre-store_source mirror retains actual VR, flat and remembered seat settings under its old slug");
    const std::string before = mirrorFixtureTree(root);
    check(!fileExists(statePath(game)) && runtimeMirrorDirFor(local, game, false) == legacy &&
          runtimeMirrorDirFor(local, game, true) == legacy &&
          edvr::openxr::seatMirrorDirFor(local, game, false) == legacy &&
          mirrorFixtureTree(root) == before && !dirExists(canonical),
          "a store path hint preserves the sole unbound legacy mirror without creating or changing anything");
    const std::wstring seat = edvr::openxr::seatMirrorDirFor(local, game, false);
    std::vector<edvr::openxr::SeatRecord> seats;
    check(!seat.empty() && edvr::openxr::seatBookLoadFile(joinPath(seat, L"seat.txt"), seats) &&
          seats.size() == 1 && seats[0].key == "legacy headset" && seats[0].pose.x == 0.125f &&
          seats[0].pose.y == 0.25f && seats[0].pose.z == -0.5f,
          "the legacy fallback reads the exact remembered seat despite the conflicting store path hint");
    std::vector<std::string> notes;
    check(restoreFromMirror(game, readMirror(runtimeMirrorDirFor(local, game, false)), "both", &notes),
          "the legacy runtime selection can recover both missing settings files");
    expectEq(readAll(joinPath(game, L"edvr.ini")), "[fix]\r\nshare_exposure = 0\r\n",
             "legacy recovery retains the exact VR setting");
    expectEq(readAll(joinPath(game, L"edvr-flat.ini")), "[fix]\r\ntemporal_aa = off\r\n",
             "legacy recovery retains the exact flat setting");
    makeTree(canonical);
    writeAll(joinPath(canonical, L"edvr.ini"), "canonical settings");
    check(runtimeMirrorDirFor(local, game, false) == canonical && runtimeMirrorDirFor(local, game, true) == canonical &&
          readAll(joinPath(canonical, L"edvr.ini")) == "canonical settings",
          "an existing exact canonical mirror takes precedence over the unbound legacy fallback");
    removeTree(canonical);
    writeFileAtomic(joinPath(legacy, L"state.ini"), "[edvr]\r\nversion = test\r\nstore_source = Epic\r\n");
    const std::string beforeMatchingRead = mirrorFixtureTree(root);
    check(runtimeMirrorDirFor(local, game, false) == legacy &&
          runtimeMirrorDirFor(local, game, true) == legacy &&
          edvr::openxr::seatMirrorDirFor(local, game, false) == legacy &&
          mirrorFixtureTree(root) == beforeMatchingRead && !dirExists(canonical),
          "a sole legacy candidate with the matching recorded owner remains recoverable despite its old slug");
    writeFileAtomic(joinPath(legacy, L"state.ini"), "[edvr]\r\nversion = test\r\nstore_source = Steam\r\n");
    const std::string beforeClaimedRead = mirrorFixtureTree(root);
    check(runtimeMirrorDirFor(local, game, false).empty() &&
          edvr::openxr::seatMirrorDirFor(local, game, false).empty() &&
          mirrorFixtureTree(root) == beforeClaimedRead && !dirExists(canonical),
          "the same candidate is refused without writes once authoritative ownership binds it to the other store");
    removeTree(root);
}
