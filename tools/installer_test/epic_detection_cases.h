#pragma once

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

    // Simulate the update that removed ED-UP's files while leaving Elite.
    const std::string tuned = "[fix]\r\nshare_exposure = 0\r\n";
    writeAll(joinPath(customProduct, L"edvr.ini"), tuned);
    check(updateMirrorIni(customProduct, expectedMirror).ok, "the Epic settings are saved outside the game");
    DeleteFileW(joinPath(customProduct, L"edvr.ini").c_str());
    GameInstall repaired;
    check(describeDirWithInstalls(customProduct, L"Folder you chose", installs, &repaired),
          "the product can be selected again after the game update");
    std::vector<std::string> notes;
    check(restoreFromMirror(customProduct, readMirror(mirrorDirFor(repaired, mirrorRoot)), "vr", &notes),
          "the direct repair finds and restores the previous Epic settings");
    expectEq(readAll(joinPath(customProduct, L"edvr.ini")), "[fix]\r\nshare_exposure = 0\r\n",
             "the repaired product gets the player's exact saved setting");

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
