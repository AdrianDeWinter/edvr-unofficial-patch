// The launcher's part of the window. This is not a header: gui.cpp includes it, inside its own
// anonymous namespace and only when EDUP_LAUNCHER is defined, because it draws with that file's
// state (`g`, `dp`, `setReport`) and its helpers. The installer's target never sees it.
//
// What is here is window glue: a band under the report with the update line and the Play
// buttons, and the three or four places gui.cpp calls into it. What the launcher DOES lives in
// launcher_session.cpp and the files it uses, none of which touch a control.

// Defined later in gui.cpp.
void applyLayout();
void showSurvey();
void runAction(AppArgs::Act action);
const wchar_t* installButtonLabel();

enum : int {
    kIdUpdate = 1080,
    kIdRc = 1081,
    kIdPlay = 1082,
    kIdPlayVr = 1083,
    kIdPlayFlat = 1084,
    kIdDirect = 1085,
    kIdCloseAfter = 1086,
    kIdSteamFrontier = 1087,
};

// Posted by the session's worker threads: lParam is a heap std::function<void()> to run here.
const UINT kMsgLauncherJob = WM_APP + 2;

// Updates sits between Status and Install & repair; Launch sits between Log and the Discord link.
const RECT kUpdatesCard{kMargin, 372, kClientWidth - kMargin, 426};
const RECT kBandCard{kMargin, 656, kClientWidth - kMargin, 758};

struct LauncherGui {
    HWND update = nullptr;
    HWND rc = nullptr;
    HWND play = nullptr;
    HWND playVr = nullptr;
    HWND playFlat = nullptr;
    HWND direct = nullptr;           // "Start Elite directly", for Steam / Frontier / Epic installs
    HWND closeAfter = nullptr;       // "Close the launcher once Elite has started", saved in launcher.ini
    HWND steamFrontier = nullptr;    // "Use Frontier account login", for Steam direct launch
    std::unique_ptr<LauncherSession> session;
    std::wstring playCaption;        // beside or under the Play buttons, for the folder shown
    int directLeft = 0;              // where the "Start Elite directly" checkbox starts, in design units
    int updateTextRight = 332;       // right edge (design units) for the painted update status line
    bool restoreViaPlan = false;     // the user agreed to a settings restore the elevated copy does
    bool noPayloadShown = false;
    std::wstring developerNote;      // set per Install, read by confirmText
    // Cached route and remembered mode, invalidated on install/setting/play changes (#198).
    std::wstring cachedRouteSource;
    std::wstring cachedRouteDir;
    LaunchRoute  cachedRoute;
    bool         haveCachedRoute = false;
    std::wstring cachedModeDir;
    std::string  cachedMode;
    bool         haveCachedMode = false;
};
LauncherGui lg;

void invalidateLauncherCache() {
    lg.haveCachedRoute = false;
    lg.haveCachedMode = false;
}

bool launcherPayloadReady() { return lg.session && lg.session->payloadState() == PayloadState::Ready; }

bool launcherBusy() { return lg.session && (lg.session->updating() || lg.session->applying()); }

bool launcherFolderHasEdup() {
    return g.haveSurvey && (g.survey.d3d11.kind == DllKind::Edvr || g.survey.openvrCurrent.kind == DllKind::Edvr ||
                            g.survey.state.present);
}

const wchar_t* launcherNoPayloadText() {
    if (g.haveSurvey && g.survey.state.present) {
        return tr(L"ED-UP is installed in this folder, but this launcher has no local release package yet.\r\n\r\n"
                  L"Press Download when Updates shows a release to enable repair and updates, or start the launcher with "
                  L"--payload and the folder of an unpacked ed-up zip.");
    }
    return tr(L"There is nothing to install from yet.\r\n\r\n"
              L"Press Download when Updates shows a release, or start the launcher with "
              L"--payload and the folder of an unpacked ed-up zip.");
}

std::wstring launcherSubtitle() {
    std::wstring head;
    if (!lg.session || lg.session->payloadState() == PayloadState::Loading) {
        head = tr(L"loading");
    } else if (lg.session->payloadState() == PayloadState::Ready) {
        head = fromUtf8(lg.session->payload().version);
    } else if (g.haveSurvey && g.survey.state.present && !g.survey.state.edvrVersion.empty() &&
               !installRecordStale(g.survey)) {
        head = fromUtf8(g.survey.state.edvrVersion) + L" (" + tr(L"no local package") + L")";
    } else {
        head = tr(L"no release downloaded yet");
    }
    return head + L"  \x00b7  " + tr(L"launcher") + L" " + fromUtf8(EDUP_LAUNCHER_VERSION) +
           L"  \x00b7  " + tr(L"unofficial patch for Elite Dangerous in VR and flat");
}

const LaunchRoute& launcherRouteFor(const std::wstring& source, const std::wstring& dir) {
    if (!lg.haveCachedRoute || lg.cachedRouteSource != source || lg.cachedRouteDir != dir) {
        lg.cachedRoute = routeFor(source, dir, true);
        lg.cachedRouteSource = source;
        lg.cachedRouteDir = dir;
        lg.haveCachedRoute = true;
    }
    return lg.cachedRoute;
}

// The remembered mode of the folder on screen, "vr" or "flat", else "vr".
std::string launcherRememberedMode() {
    if (!g.haveSurvey) return "vr";
    if (!lg.haveCachedMode || lg.cachedModeDir != g.survey.game.dir) {
        LauncherState state;
        state.load(joinPath(launcherRootsFor(g.args).launcherDir(), L"launcher.ini"), nullptr);
        lg.cachedMode = state.mode(g.survey.game.dir) == "flat" ? "flat" : "vr";
        lg.cachedModeDir = g.survey.game.dir;
        lg.haveCachedMode = true;
    }
    return lg.cachedMode;
}

// The install Play was last pressed for, as an index into g.installs, else g.installs.size(). A folder
// that was browsed to is put back in the list while it still holds a game the window would take.
size_t launcherRememberedInstall() {
    LauncherState state;
    state.load(joinPath(launcherRootsFor(g.args).launcherDir(), L"launcher.ini"), nullptr);
    GameInstall restored;
    const size_t choice = rememberedInstallChoice(g.installs, state.lastInstall(), describeDir, eliteExeIsLegacy, &restored);
    if (choice == kNoRememberedInstall) return g.installs.size();
    if (choice == g.installs.size()) {
        g.installs.push_back(restored);
        const std::wstring label = restored.source + L"  \x00b7  " + restored.dir;
        SendMessageW(g.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    return choice;
}

// Shows, hides and enables everything the launcher owns, from the session and the survey.
void updateLauncherControls() {
    if (!lg.session) return;
    const bool onScreen = g.screen == Screen::Install;
    const bool payloadOk = launcherPayloadReady();
    const bool payloadLoading = lg.session->payloadState() == PayloadState::Loading;
    const bool busy = launcherBusy();
    const bool gameHere = g.haveSurvey && g.survey.gameRunningHere;

    // Install and Repair need the payload; Uninstall works without one (the planner then only keeps more).
    const bool survey = g.haveSurvey;
    if (g.install) SetWindowTextW(g.install, installButtonLabel());
    EnableWindow(g.install, survey && payloadOk && !busy && !gameHere);
    EnableWindow(g.repair, survey && payloadOk && !busy && !gameHere);
    EnableWindow(g.uninstall, survey && !payloadLoading && !busy && !gameHere);

    const bool available = lg.session->checkState() == CheckState::Available && !lg.session->updating();
    if (lg.update) {
        const wchar_t* updateLabel = launcherFolderHasEdup() ? tr(L"Download & update") : tr(L"Download & install");
        SetWindowTextW(lg.update, updateLabel);
        HDC dc = GetDC(g.window);
        const int labelPx = ui::textWidth(dc, updateLabel, ui::fonts().body) + dp(24);
        ReleaseDC(g.window, dc);
        int btnW = g.dpi > 0 ? MulDiv(labelPx, 96, static_cast<int>(g.dpi)) : 124;
        if (btnW < 118) btnW = 118;
        if (btnW > 186) btnW = 186;
        const int cardRight = kClientWidth - kMargin - kCardPad;
        const int btnLeft = cardRight - btnW;
        const int rcW = 196;
        const int rcLeft = available ? (btnLeft - 8 - rcW) : (cardRight - rcW);
        lg.updateTextRight = rcLeft - 8;
        MoveWindow(lg.update, dp(btnLeft), dp(394), dp(btnW), dp(26), TRUE);
        MoveWindow(lg.rc, dp(rcLeft), dp(394), dp(rcW), dp(26), TRUE);
    }
    ShowWindow(lg.update, onScreen && available ? SW_SHOW : SW_HIDE);
    EnableWindow(lg.update, !lg.session->applying());
    ShowWindow(lg.rc, onScreen ? SW_SHOW : SW_HIDE);

    // Play. Only a route that carries VR or flat gets two buttons: Frontier's, Steam's once the
    // launcher starts Elite itself, and Epic's once the skip-the-launcher checkbox is on. The
    // checkbox is offered for Steam, for Frontier's own store, and for Epic.
    lg.playCaption.clear();
    bool showPlay = false, showPair = false, showDirect = false, showSteamFrontier = false, playEnabled = false;
    bool showVrButton = false, showFlatButton = false;  // within the pair: only the modes this folder has installed
    if (survey) {
        const LaunchRoute& route = launcherRouteFor(g.survey.game.source, g.survey.game.dir);
        const bool anyGame = g.survey.gameRunningHere || g.survey.gameRunningElsewhere;
        playEnabled = !lg.session->playing() && !busy && !anyGame && !g.survey.gameRunStateUnknown;
        const bool frontierStore = route.kind == RouteKind::Frontier && _wcsicmp(g.survey.game.source.c_str(), L"Frontier launcher") == 0;
        const bool epic = route.kind == RouteKind::Epic;
        showDirect = (route.kind == RouteKind::Steam || frontierStore || epic) && lg.session->directLaunchOffered();
        const bool directOn = showDirect && lg.session->directLaunch();
        showSteamFrontier = route.kind == RouteKind::Steam && directOn;
        if (lg.direct)
            SetWindowTextW(lg.direct, epic ? tr(L"Skip the Epic launcher") : tr(L"Start Elite directly (no Frontier launcher)"));
        if (route.kind == RouteKind::Frontier || directOn) {
            // A button only for a mode this folder has installed: a VR-only install gets no flat button, and the reverse.
            // An empty scope (no install the survey can name) keeps both, as before.
            const std::string scope = installedScope(g.survey);
            const bool hasVr = scope.empty() || scope == "both" || scope == "vr";
            const bool hasFlat = scope.empty() || scope == "both" || scope == "flat";
            const bool bothModes = hasVr && hasFlat;
            showPair = true;
            showVrButton = hasVr;
            showFlatButton = hasFlat;
            const bool steamFrontierOn = showSteamFrontier && lg.session->steamFrontierLogin();
            lg.playCaption = directOn ? (epic ? tr(L"Signs in to Epic and Frontier, starts Elite itself, and exits once Elite is running.")
                                          : frontierStore ? tr(L"Signs in itself. If that fails, it starts EDLaunch.")
                                          : steamFrontierOn ? tr(L"Signs in with your Frontier account. If that fails, it starts through Steam.")
                                                            : tr(L"Signs in itself. If that fails, it starts through Steam."))
                             : bothModes ? tr(L"Starts EDLaunch with the mode you pick, then closes it when Elite exits.")
                             : hasVr ? tr(L"Starts EDLaunch in VR, then closes it when Elite exits.")
                                     : tr(L"Starts EDLaunch in flat, then closes it when Elite exits.");
            // With a choice, the remembered mode is the primary button; a lone button is the primary one.
            const bool flat = launcherRememberedMode() == "flat";
            ui::setButtonStyle(lg.playVr, (bothModes ? !flat : hasVr) ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary);
            ui::setButtonStyle(lg.playFlat, (bothModes ? flat : hasFlat) ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary);
        } else if (route.kind != RouteKind::None) {
            showPlay = true;
            const wchar_t* store = route.kind == RouteKind::Steam ? L"Steam" : L"Epic";
            std::wstring mode = tr(L"Elite's 3D mode is unknown.");
            if (g.mode.known)
                mode = g.mode.mode == edvr::GameMode::Flat ? tr(L"Elite is set to flat (3D off).") : tr(L"Elite is set to VR (3D on).");
            lg.playCaption = std::wstring(tr(L"Starts Elite through ")) + store + tr(L", which uses the mode Elite is set to. ") + mode;
        } else {
            lg.playCaption = fromUtf8(route.why);
        }
        if (gameHere || g.survey.gameRunningElsewhere) lg.playCaption = tr(L"Elite is already running.");
    }
    if (lg.session->playing() || !lg.session->playLine().empty()) lg.playCaption = fromUtf8(lg.session->playLine());
    ShowWindow(lg.play, onScreen && showPlay ? SW_SHOW : SW_HIDE);
    ShowWindow(lg.playVr, onScreen && showPair && showVrButton ? SW_SHOW : SW_HIDE);
    ShowWindow(lg.playFlat, onScreen && showPair && showFlatButton ? SW_SHOW : SW_HIDE);
    // Left to right: Play VR, then Play flat. A lone pair button takes the first slot.
    MoveWindow(lg.playVr, dp(kMargin + kCardPad), dp(682), dp(114), dp(34), TRUE);
    MoveWindow(lg.playFlat, dp(kMargin + kCardPad + (showVrButton ? 122 : 0)), dp(682), dp(114), dp(34), TRUE);
    for (HWND button : {lg.play, lg.playVr, lg.playFlat}) EnableWindow(button, playEnabled);

    // The checkbox sits right of whichever Play buttons are showing, and what is under
    // it is the caption. It follows the saved setting, which is the session's to say.
    ShowWindow(lg.direct, onScreen && showDirect ? SW_SHOW : SW_HIDE);
    EnableWindow(lg.direct, !lg.session->playing() && !busy);
    if (ui::checkboxChecked(lg.direct) != lg.session->directLaunch()) ui::toggleCheckbox(lg.direct);
    lg.directLeft = kMargin + kCardPad + (showPair ? 248 : 134);
    MoveWindow(lg.direct, dp(lg.directLeft), dp(680), dp(kClientWidth - kMargin - kCardPad - lg.directLeft), dp(20), TRUE);

    // Bottom options row inside the Launch card: close-after-start and (for Steam direct launch) Frontier login.
    ShowWindow(lg.closeAfter, onScreen ? SW_SHOW : SW_HIDE);
    EnableWindow(lg.closeAfter, !busy);
    if (ui::checkboxChecked(lg.closeAfter) != lg.session->closeAfterStartSetting()) ui::toggleCheckbox(lg.closeAfter);
    ShowWindow(lg.steamFrontier, onScreen && showSteamFrontier ? SW_SHOW : SW_HIDE);
    EnableWindow(lg.steamFrontier, !lg.session->playing() && !busy);
    if (lg.steamFrontier && ui::checkboxChecked(lg.steamFrontier) != lg.session->steamFrontierLoginSetting())
        ui::toggleCheckbox(lg.steamFrontier);
}

void launcherRefresh() {
    updateLauncherControls();
    if (lg.session && lg.session->payloadState() == PayloadState::None && !lg.noPayloadShown && g.report) {
        lg.noPayloadShown = true;
        setReport(toUtf8(launcherNoPayloadText()));
    }
    InvalidateRect(g.window, nullptr, FALSE);
}

// Positions the controls the launcher cards own. Called from applyLayout, after its own loop, because their
// visibility is the session's call and not the screen's alone.
void layoutLauncherControls() {
    if (!lg.update) return;
    // These are not in `places`, so a change of scale does not reach their font on its own.
    for (HWND control : {lg.update, lg.rc, lg.play, lg.playVr, lg.playFlat, lg.direct, lg.closeAfter, lg.steamFrontier})
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui::fonts().body), TRUE);
    MoveWindow(lg.playVr, dp(kMargin + kCardPad), dp(682), dp(114), dp(34), TRUE);
    MoveWindow(lg.playFlat, dp(kMargin + kCardPad + 122), dp(682), dp(114), dp(34), TRUE);
    MoveWindow(lg.play, dp(kMargin + kCardPad), dp(682), dp(124), dp(34), TRUE);
    MoveWindow(lg.closeAfter, dp(kMargin + kCardPad), dp(732), dp(344), dp(22), TRUE);
    if (lg.steamFrontier) MoveWindow(lg.steamFrontier, dp(396), dp(732), dp(kClientWidth - kMargin - kCardPad - 396), dp(22), TRUE);
    updateLauncherControls();
}

void paintLauncherBand(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();

    // Updates card (between Status and Install & repair).
    const RECT updatesCard{dp(kUpdatesCard.left), dp(kUpdatesCard.top), dp(kUpdatesCard.right), dp(kUpdatesCard.bottom)};
    ui::paintCard(dc, updatesCard, g.dpi);
    RECT updatesHead{dp(kMargin + kCardPad), dp(kUpdatesCard.top + 6), dp(kClientWidth - kMargin - kCardPad),
                     dp(kUpdatesCard.top + 22)};
    ui::drawText(dc, tr(L"Updates"), updatesHead, f.heading, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Launch card (below Log).
    const RECT card{dp(kBandCard.left), dp(kBandCard.top), dp(kBandCard.right), dp(kBandCard.bottom)};
    ui::paintCard(dc, card, g.dpi);
    RECT launchHead{dp(kMargin + kCardPad), dp(kBandCard.top + 6), dp(kClientWidth - kMargin - kCardPad),
                    dp(kBandCard.top + 22)};
    ui::drawText(dc, tr(L"Launch"), launchHead, f.heading, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!lg.session) return;

    COLORREF colour = t.text;
    if (lg.session->checkState() == CheckState::Refused) colour = t.warn;
    else if (lg.session->checkState() == CheckState::Skipped) colour = t.subtext;
    std::string statusLine = lg.session->updateLine();
    if (lg.session->checkState() == CheckState::Available && g.haveSurvey && g.survey.state.present &&
        !g.survey.state.edvrVersion.empty() && !installRecordStale(g.survey) &&
        statusLine == ("ED-UP " + g.survey.state.edvrVersion + " is available.")) {
        statusLine = "ED-UP " + g.survey.state.edvrVersion + " package available for repair and updates.";
    }
    RECT line{dp(kMargin + kCardPad), dp(394), dp(lg.updateTextRight), dp(420)};
    ui::drawText(dc, trLine(statusLine), line, f.body, colour, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Under the "Start Elite directly" checkbox when that is showing; otherwise beside the Play buttons.
    const bool pair = lg.playVr && IsWindowVisible(lg.playVr);
    const bool single = lg.play && IsWindowVisible(lg.play);
    const bool underCheckbox = lg.direct && IsWindowVisible(lg.direct);
    const int captionLeft = underCheckbox ? lg.directLeft : pair ? kMargin + kCardPad + 244 : single ? kMargin + kCardPad + 134 : kMargin + kCardPad;
    RECT caption{dp(captionLeft), dp(underCheckbox ? 701 : 682), dp(kClientWidth - kMargin - kCardPad), dp(730)};
    ui::drawText(dc, lg.playCaption, caption, f.caption, t.subtext, DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
}

void refillScopeChoices() {
    if (!g.scopeCombo) return;
    const LRESULT sel = SendMessageW(g.scopeCombo, CB_GETCURSEL, 0, 0);
    SendMessageW(g.scopeCombo, CB_RESETCONTENT, 0, 0);
    for (const wchar_t* choice : kScopeChoices) {
        SendMessageW(g.scopeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(choice)));
    }
    SendMessageW(g.scopeCombo, CB_SETCURSEL, sel == CB_ERR ? 0 : sel, 0);
}

void setControlText(int id, const wchar_t* english) {
    if (HWND control = GetDlgItem(g.window, id)) SetWindowTextW(control, tr(english));
}

// The words on the window, in the language `code` names. Does not write launcher.ini.
void applyLauncherLanguage(const std::string& code) {
    l10nSet(code.empty() ? systemGameLanguageCode() : canonicalGameLanguage(code));
    invalidateLauncherCache();
    setControlText(kIdTabInstall, L"Setup");
    setControlText(kIdTabSettings, L"Settings");
    setControlText(kIdBrowse, L"Browse...");
    setControlText(kIdChkKeep, L"Keep the settings I have changed");
    setControlText(kIdChkPurge, L"Clean purge on uninstall");
    setControlText(kIdInstall, L"Install");
    if (g.haveSurvey && g.install) SetWindowTextW(g.install, installButtonLabel());
    setControlText(kIdRepair, L"Repair");
    setControlText(kIdUninstall, L"Uninstall");
    setControlText(kIdCollectLogs, L"Save logs");
    setControlText(kIdSettingsVr, L"VR");
    setControlText(kIdSettingsFlat, L"Flat");
    setControlText(kIdUpdate, L"Download & install");
    setControlText(kIdRc, L"Include release candidates");
    setControlText(kIdPlay, L"Play");
    setControlText(kIdPlayVr, L"Play VR");
    setControlText(kIdPlayFlat, L"Play flat");
    setControlText(kIdDirect, L"Start Elite directly (no Frontier launcher)");
    setControlText(kIdCloseAfter, L"Close the launcher once Elite has started");
    setControlText(kIdSteamFrontier, L"Use Frontier account login");
    setControlText(kIdDiscord, L"Ask on Discord");
    setControlText(kIdCopyPath, L"Copy path");
    setControlText(kIdShowFolder, L"Show folder");
    if (g.search) SendMessageW(g.search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(L"Search settings")));
    refillScopeChoices();
    if (g.languageCombo) {
        const LRESULT count = SendMessageW(g.languageCombo, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < count; ++i) {
            const auto stored = reinterpret_cast<const char*>(SendMessageW(g.languageCombo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0));
            if (stored && l10nCode() == stored) {
                SendMessageW(g.languageCombo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
                break;
            }
        }
    }
    if (g.haveSurvey) rebuildStatus();
    if (lg.session) updateLauncherControls();
    if (lg.session && lg.session->payloadState() == PayloadState::None)
        setReport(toUtf8(launcherNoPayloadText()));
    applyLayout();
}

// CB_SETITEMDATA keeps the code. The pointer is a string literal, so it outlives the control.
void fillLanguageCombo() {
    if (!g.languageCombo) return;
    SendMessageW(g.languageCombo, CB_RESETCONTENT, 0, 0);
    for (const char* code : kLanguageCodes) {
        const LRESULT index = SendMessageW(g.languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(gameLanguageName(code)));
        if (index >= 0) SendMessageW(g.languageCombo, CB_SETITEMDATA, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(code));
    }
}

void createLauncherControls(HWND window) {
    const ui::Fonts& f = ui::fonts();
    lg.update = ui::makeButton(window, L"Download & install", kIdUpdate, ui::ButtonStyle::Primary, f.body);
    lg.rc = ui::makeCheckbox(window, L"Include release candidates", kIdRc, false, f.body);
    lg.play = ui::makeButton(window, L"Play", kIdPlay, ui::ButtonStyle::Primary, f.body);
    lg.playVr = ui::makeButton(window, L"Play VR", kIdPlayVr, ui::ButtonStyle::Primary, f.body);
    lg.playFlat = ui::makeButton(window, L"Play flat", kIdPlayFlat, ui::ButtonStyle::Secondary, f.body);
    lg.direct = ui::makeCheckbox(window, L"Start Elite directly (no Frontier launcher)", kIdDirect, false, f.body);
    lg.closeAfter = ui::makeCheckbox(window, L"Close the launcher once Elite has started", kIdCloseAfter, false, f.body);
    lg.steamFrontier = ui::makeCheckbox(window, L"Use Frontier account login", kIdSteamFrontier, false, f.body);
    for (HWND control : {lg.update, lg.rc, lg.play, lg.playVr, lg.playFlat, lg.direct, lg.closeAfter, lg.steamFrontier})
        ShowWindow(control, SW_HIDE);
}

// The window is up: load the payload, ask GitHub, and say what start-up had to say.
void launcherBegin() {
    SessionEnv env;
    env.roots = launcherRootsFor(g.args);
    env.payloadDir = g.args.payloadDir;
    env.channelOverride = g.args.channel;
    env.closeAfterStartArg = g.args.closeAfterStart;
    env.frontierLoginArg = g.args.frontierLogin;
    env.updatedFlag = g.args.updated;
    wchar_t self[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(sizeof(self) / sizeof(self[0])));
    env.selfExe.assign(self, n);
    const HWND window = g.window;
    env.post = [window](std::function<void()> job) {
        std::function<void()>* heap = new std::function<void()>(std::move(job));
        if (!PostMessageW(window, kMsgLauncherJob, 0, reinterpret_cast<LPARAM>(heap))) delete heap;
    };
    env.changed = []() { launcherRefresh(); };
    env.payloadSwitched = []() {
        // Switched: look at the folder again, then the ordinary install, with its plan and its confirmation.
        if (!g.haveSurvey) return;
        g.survey = surveyTarget(g.survey.game);
        showSurvey();
        runAction(AppArgs::Act::Install);
    };
    // A Frontier-store direct launch asks for the login, and for the code Frontier emails, in modal windows
    // of the launcher's own. Both run here, on the window's thread, while the session's worker waits.
    env.askLogin = [window](const std::string& note, LoginForm* form) {
        FrontierDialogOptions options;
        options.owner = window;
        options.dpi = g.dpi;
        options.underWine = runningUnderWine();
        return showFrontierLogin(options, note, form);
    };
    env.askCode = [window](const std::string& email, const std::string& note, std::string* code) {
        FrontierDialogOptions options;
        options.owner = window;
        options.dpi = g.dpi;
        options.underWine = runningUnderWine();
        return showFrontierCode(options, email, note, code);
    };
    env.closeWindow = [window]() { PostMessageW(window, WM_CLOSE, 0, 0); };
    env.windowAside = [window](bool aside) {
        ShowWindow(window, aside ? SW_MINIMIZE : SW_RESTORE);
        if (!aside) SetForegroundWindow(window);
    };
    // Direct launch is not offered under Wine yet: the Steam ticket and the game start have not been shown to
    // work inside a Proton prefix. --direct-dry-run stays, as the way to find out.
    env.directLaunchOffered = !runningUnderWine();
    env.startNewLauncher = [](const std::wstring& exe) {
        // Under Wine there is no restart: the session then shows "The launcher was updated. Close this window and
        // start ED-UP again", as it does when a restart fails. A process the launcher starts may be ended with
        // Steam's runtime container when the first one exits (unverified).
        if (runningUnderWine()) return false;
        releaseInstanceMutex();  // the replacement waits for it, but not forever
        const bool started = startLauncherProcess(exe, restartArguments(true));
        if (!started) takeInstanceMutex(0);
        return started;
    };
    lg.session.reset(new LauncherSession(env));
    lg.session->start();
    if (lg.session->includeReleaseCandidates()) ui::toggleCheckbox(lg.rc);
    launcherRefresh();
}

void launcherAfterScan() {
    const std::string& note = launcherStartupNote();
    if (!note.empty() && g.haveSurvey) setReport(note);
}

void launcherShutdown() {
    // A worker stuck connecting can hold the join for up to a network timeout, so the window goes first.
    if (g.window) ShowWindow(g.window, SW_HIDE);
    if (lg.session) lg.session->shutdown();
}

// Closing is refused while the elevated copy is writing: it cannot be called back, and walking
// away from it would leave the user not knowing what it did.
bool launcherMayClose() {
    if (!lg.session || !lg.session->applying()) return true;
    setReport("An install is in progress. Wait for it to finish; this window closes when you press close again.\r\n");
    return false;
}

// Runs a job a worker thread posted.
void runLauncherJob(LPARAM param) {
    std::function<void()>* job = reinterpret_cast<std::function<void()>*>(param);
    if (!job) return;
    (*job)();
    delete job;
}

// WM_COMMAND for the launcher's own controls. True when it was one of them.
bool launcherCommand(int id, int code) {
    if (code != BN_CLICKED || !lg.session) return false;
    switch (id) {
        case kIdUpdate:
            lg.session->beginUpdate();
            return true;
        case kIdCloseAfter:
            ui::toggleCheckbox(lg.closeAfter);
            lg.session->setCloseAfterStart(ui::checkboxChecked(lg.closeAfter));
            return true;
        case kIdSteamFrontier:
            ui::toggleCheckbox(lg.steamFrontier);
            lg.session->setSteamFrontierLogin(ui::checkboxChecked(lg.steamFrontier));
            updateLauncherControls();
            InvalidateRect(g.window, nullptr, FALSE);
            return true;
        case kIdRc:
            ui::toggleCheckbox(lg.rc);
            lg.session->setIncludeReleaseCandidates(ui::checkboxChecked(lg.rc));
            return true;
        case kIdDirect: {
            // Turning it on asks first, with No as the default; a No leaves it off. Turning it off just does.
            const bool want = !ui::checkboxChecked(lg.direct);
            // A Steam install is asked with Steam's words; one from Frontier's store is told about the login.
            const bool frontierStore = g.haveSurvey && _wcsicmp(g.survey.game.source.c_str(), L"Frontier launcher") == 0;
            const bool epic = g.haveSurvey && _wcsicmp(g.survey.game.source.c_str(), L"Epic") == 0;
            const char* question = epic ? kDirectLaunchQuestionEpic
                                        : frontierStore ? kDirectLaunchQuestionFrontier : kDirectLaunchQuestion;
            const bool was = lg.session->directLaunch();
            lg.session->setDirectLaunch(want, [question]() {
                return MessageBoxW(g.window, fromUtf8(question).c_str(), kAppTitle,
                                   MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
            });
            if (epic && lg.session->directLaunch() != was) {
                const std::string scope = installedScope(g.survey);
                bool vr = launcherRememberedMode() != "flat";
                if (scope == "flat") vr = false;
                if (scope == "vr") vr = true;
                std::string error;
                if (!lg.session->standInForEdLaunch(g.survey.game.dir, lg.session->directLaunch(), vr, &error)) {
                    // A failed turn-on can already have written the stand-in (the mode file failed after it).
                    // Turning the setting off has to take that file back, and must not touch a folder that has none.
                    if (lg.session->directLaunch()) {
                        const std::wstring live = findEdLaunch(g.survey.game.dir);
                        std::wstring marker;
                        const size_t slash = live.find_last_of(L"\\/");
                        if (!live.empty() && slash != std::wstring::npos)
                            marker = joinPath(live.substr(0, slash), kEdLaunchMarkerName);
                        lg.session->setDirectLaunch(false, nullptr);
                        if (!marker.empty() && fileExists(marker)) {
                            std::string restoreError;
                            if (!removeEdLaunchShim(g.survey.game.dir, &restoreError) && !restoreError.empty()) {
                                if (!error.empty()) error += " ";
                                error += restoreError;
                            }
                        }
                    }
                    MessageBoxW(g.window, fromUtf8(error).c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
                }
            }
            invalidateLauncherCache();
            updateLauncherControls();
            return true;
        }
        case kIdPlay:
        case kIdPlayVr:
        case kIdPlayFlat: {
            if (!g.haveSurvey) return true;
            const bool vr = id != kIdPlayFlat;
            invalidateLauncherCache();
            lg.session->beginPlay(g.survey.game.source, g.survey.game.dir, vr, isElevatedForPlay(), realRouteOps(), WatchNames(),
                                  WatchTiming());
            return true;
        }
        default:
            return false;
    }
}

// What the elevated copy needs: true when this run goes through it, which is when the folder
// needs administrator rights and this process has none.
bool launcherNeedsElevatedCopy() { return needsElevationFor(g.survey) && !isElevated(); }

// The settings restore the user agreed to is done by the elevated copy when the folder needs it.
bool launcherRestoreHere(bool agreed) {
    lg.restoreViaPlan = false;
    if (agreed && launcherNeedsElevatedCopy()) {
        lg.restoreViaPlan = true;
        return false;
    }
    return true;
}

// Per Install, Repair or Uninstall: forget what the last one agreed to, and note whether the folder
// belongs to a developer (the confirmation then says a branch build would be replaced).
void launcherBeginAction() {
    lg.restoreViaPlan = false;
    lg.developerNote.clear();
    if (g.haveSurvey && isDeveloperInstall(g.survey.game.dir))
        lg.developerNote =
            L"This folder was last changed by a branch build (install_edvr.py), not by this launcher. Going on "
            L"replaces that build with this release.";
}

// Asks, then hands the confirmed install to the elevated copy and reports what it says.
void launcherApplyElevated(const AppArgs& args, const Plan& plan) {
    // The copy with administrator rights installs only a signed download (launcher_plan.h says why).
    const PayloadLoad& payload = lg.session->payload();
    if (payload.kind != PayloadKind::Manifest && args.action != AppArgs::Act::Uninstall) {
        setReport(planReport(plan) +
                  "\r\n----\r\nNothing was changed. This game folder needs administrator rights, and the copy of the launcher "
                  "that has them installs only a signed download, never a --payload folder. Start the launcher without "
                  "--payload and press Download, or run it as administrator.\r\n");
        return;
    }
    const int answer = MessageBoxW(
        g.window,
        L"This game folder can only be written to by an administrator (it is usually under Program Files).\n\n"
        L"Windows will ask for permission, and a short-lived copy of the launcher with administrator rights "
        L"makes the change. The launcher itself stays as it is.\n\nContinue?",
        kAppTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (answer != IDYES) return;

    PlanFile file;
    file.action = args.action == AppArgs::Act::Uninstall ? "uninstall" : args.action == AppArgs::Act::Repair ? "repair" : "install";
    file.dir = g.survey.game.dir;
    file.source = g.survey.game.source;
    file.scope = args.scope;
    file.keepSettings = args.keepSettings;
    file.removeSettings = args.removeSettings;
    file.purge = args.purge;
    file.restoreMirror = lg.restoreViaPlan;
    // The copy finds the manifest, the cache and the mirror under the data root the plan file sits in.
    if (payload.kind == PayloadKind::Manifest) file.manifestSha = manifestShaOf(payload.manifestPath);
    lg.restoreViaPlan = false;

    const std::string shown = planReport(plan);
    setReport(shown + "\r\n----\r\nWaiting for Windows' permission prompt...\r\n");
    lg.session->beginElevatedApply(file, runWithUac, [shown](const ElevatedResult& result) {
        if (result.declined) {
            setReport(shown + "\r\n----\r\nThe permission prompt was declined, so nothing was changed.\r\n");
        } else if (!result.error.empty() && result.outcome.lines.empty()) {
            setReport(shown + "\r\n----\r\n" + result.error + "\r\n");
        } else {
            setReport(shown + "\r\n----\r\n" + outcomeReport(result.outcome));
        }
        g.survey = surveyTarget(g.survey.game);
        showSurvey();
    });
}
