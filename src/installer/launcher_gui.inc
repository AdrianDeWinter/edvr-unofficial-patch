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

enum : int {
    kIdUpdate = 1080,
    kIdRc = 1081,
    kIdPlay = 1082,
    kIdPlayVr = 1083,
    kIdPlayFlat = 1084,
    kIdDirect = 1085,
    kIdCloseAfter = 1086,
};

// Posted by the session's worker threads: lParam is a heap std::function<void()> to run here.
const UINT kMsgLauncherJob = WM_APP + 2;

// The band sits between the report card (ends at 650) and the bottom of the window. Its third row, under
// the Play row, holds the close-after-start checkbox.
const RECT kBandCard{kMargin, 658, kClientWidth - kMargin, 776};

struct LauncherGui {
    HWND update = nullptr;
    HWND rc = nullptr;
    HWND play = nullptr;
    HWND playVr = nullptr;
    HWND playFlat = nullptr;
    HWND direct = nullptr;           // "Start Elite directly", for a Steam install
    HWND closeAfter = nullptr;       // "Close the launcher once Elite has started", saved in launcher.ini
    std::unique_ptr<LauncherSession> session;
    std::wstring playCaption;        // beside or under the Play buttons, for the folder shown
    int directLeft = 0;              // where the "Start Elite directly" checkbox starts, in design units
    bool restoreViaPlan = false;     // the user agreed to a settings restore the elevated copy does
    bool noPayloadShown = false;
    std::wstring developerNote;      // set per Install, read by confirmText
};
LauncherGui lg;

bool launcherPayloadReady() { return lg.session && lg.session->payloadState() == PayloadState::Ready; }

bool launcherBusy() { return lg.session && (lg.session->updating() || lg.session->applying()); }

const wchar_t* launcherNoPayloadText() {
    return L"There is nothing to install from yet.\r\n\r\n"
           L"Press Download when the line below says an update is available, or start the launcher with "
           L"--payload and the folder of an unpacked ed-up zip.";
}

std::wstring launcherSubtitle() {
    std::wstring head;
    if (!lg.session || lg.session->payloadState() == PayloadState::Loading) {
        head = L"loading";
    } else if (lg.session->payloadState() == PayloadState::Ready) {
        head = fromUtf8(lg.session->payload().version);
    } else {
        head = L"no release downloaded yet";
    }
    return head + L"  \x00b7  launcher " + fromUtf8(EDUP_LAUNCHER_VERSION) +
           L"  \x00b7  unofficial patch for Elite Dangerous in VR and flat";
}

// The remembered mode of the folder on screen, "vr" or "flat", else "vr".
std::string launcherRememberedMode() {
    if (!g.haveSurvey) return "vr";
    LauncherState state;
    state.load(joinPath(launcherRootsFor(g.args).launcherDir(), L"launcher.ini"), nullptr);
    return state.mode(g.survey.game.dir) == "flat" ? "flat" : "vr";
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
    EnableWindow(g.install, survey && payloadOk && !busy && !gameHere);
    EnableWindow(g.repair, survey && payloadOk && !busy && !gameHere);
    EnableWindow(g.uninstall, survey && !payloadLoading && !busy && !gameHere);

    const bool available = lg.session->checkState() == CheckState::Available && !lg.session->updating();
    ShowWindow(lg.update, onScreen && available ? SW_SHOW : SW_HIDE);
    EnableWindow(lg.update, !lg.session->applying());
    ShowWindow(lg.rc, onScreen ? SW_SHOW : SW_HIDE);

    // Play. Only a route that carries VR or flat gets two buttons: Frontier's, and Steam's once the
    // launcher starts Elite itself (direct launch, which is off unless the user turned it on). The
    // setting is offered for a Steam install and for one from Frontier's own store.
    lg.playCaption.clear();
    bool showPlay = false, showPair = false, showDirect = false, playEnabled = false;
    if (survey) {
        const LaunchRoute route = routeFor(g.survey.game.source, g.survey.game.dir, true);
        const bool anyGame = g.survey.gameRunningHere || g.survey.gameRunningElsewhere;
        playEnabled = !lg.session->playing() && !busy && !anyGame && !g.survey.gameRunStateUnknown;
        const bool frontierStore = route.kind == RouteKind::Frontier && _wcsicmp(g.survey.game.source.c_str(), L"Frontier launcher") == 0;
        showDirect = (route.kind == RouteKind::Steam || frontierStore) && lg.session->directLaunchOffered();
        const bool directOn = showDirect && lg.session->directLaunch();
        if (route.kind == RouteKind::Frontier || directOn) {
            showPair = true;
            lg.playCaption = directOn ? (frontierStore ? L"Signs in itself. If that fails, it starts EDLaunch."
                                                       : L"Signs in itself. If that fails, it starts through Steam.")
                                      : L"Starts EDLaunch with the mode you pick, then closes it when Elite exits.";
            const bool flat = launcherRememberedMode() == "flat";
            ui::setButtonStyle(lg.playVr, flat ? ui::ButtonStyle::Secondary : ui::ButtonStyle::Primary);
            ui::setButtonStyle(lg.playFlat, flat ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary);
        } else if (route.kind != RouteKind::None) {
            showPlay = true;
            const wchar_t* store = route.kind == RouteKind::Steam ? L"Steam" : L"Epic";
            std::wstring mode = L"Elite's 3D mode is unknown.";
            if (g.mode.known)
                mode = g.mode.mode == edvr::GameMode::Flat ? L"Elite is set to flat (3D off)." : L"Elite is set to VR (3D on).";
            lg.playCaption = std::wstring(L"Starts Elite through ") + store + L", which uses the mode Elite is set to. " + mode;
        } else {
            lg.playCaption = fromUtf8(route.why);
        }
        if (gameHere || g.survey.gameRunningElsewhere) lg.playCaption = L"Elite is already running.";
    }
    if (lg.session->playing() || !lg.session->playLine().empty()) lg.playCaption = fromUtf8(lg.session->playLine());
    ShowWindow(lg.play, onScreen && showPlay ? SW_SHOW : SW_HIDE);
    ShowWindow(lg.playVr, onScreen && showPair ? SW_SHOW : SW_HIDE);
    ShowWindow(lg.playFlat, onScreen && showPair ? SW_SHOW : SW_HIDE);
    for (HWND button : {lg.play, lg.playVr, lg.playFlat}) EnableWindow(button, playEnabled);

    // The checkbox is on the second row, right of whichever Play buttons are showing, and what is under
    // it is the caption. It follows the saved setting, which is the session's to say.
    ShowWindow(lg.direct, onScreen && showDirect ? SW_SHOW : SW_HIDE);
    EnableWindow(lg.direct, !lg.session->playing() && !busy);
    if (ui::checkboxChecked(lg.direct) != lg.session->directLaunch()) ui::toggleCheckbox(lg.direct);
    lg.directLeft = kMargin + kCardPad + (showPair ? 262 : 134);
    MoveWindow(lg.direct, dp(lg.directLeft), dp(696), dp(kClientWidth - kMargin - kCardPad - lg.directLeft), dp(22), TRUE);

    // Close-after-start: its own row, under the Play row, always offered on the install screen.
    ShowWindow(lg.closeAfter, onScreen ? SW_SHOW : SW_HIDE);
    EnableWindow(lg.closeAfter, !busy);
    if (ui::checkboxChecked(lg.closeAfter) != lg.session->closeAfterStartSetting()) ui::toggleCheckbox(lg.closeAfter);
}

void launcherRefresh() {
    updateLauncherControls();
    if (lg.session && lg.session->payloadState() == PayloadState::None && !lg.noPayloadShown && g.report) {
        lg.noPayloadShown = true;
        setReport(toUtf8(launcherNoPayloadText()));
    }
    InvalidateRect(g.window, nullptr, FALSE);
}

// Positions the controls the band owns. Called from applyLayout, after its own loop, because their
// visibility is the session's call and not the screen's alone.
void layoutLauncherControls() {
    if (!lg.update) return;
    // These are not in `places`, so a change of scale does not reach their font on its own.
    for (HWND control : {lg.update, lg.rc, lg.play, lg.playVr, lg.playFlat, lg.direct, lg.closeAfter})
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui::fonts().body), TRUE);
    MoveWindow(lg.update, dp(574), dp(666), dp(102), dp(26), TRUE);
    MoveWindow(lg.rc, dp(336), dp(666), dp(226), dp(26), TRUE);
    MoveWindow(lg.playVr, dp(kMargin + kCardPad), dp(700), dp(118), dp(34), TRUE);
    MoveWindow(lg.playFlat, dp(kMargin + kCardPad + 126), dp(700), dp(118), dp(34), TRUE);
    MoveWindow(lg.play, dp(kMargin + kCardPad), dp(700), dp(124), dp(34), TRUE);
    // Stops short of the Discord link, which sits at the right of the same rows (kTipTop).
    MoveWindow(lg.closeAfter, dp(kMargin + kCardPad), dp(752), dp(538), dp(22), TRUE);
    updateLauncherControls();
}

void paintLauncherBand(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    const RECT card{dp(kBandCard.left), dp(kBandCard.top), dp(kBandCard.right), dp(kBandCard.bottom)};
    ui::paintCard(dc, card, g.dpi);
    if (!lg.session) return;

    COLORREF colour = t.text;
    if (lg.session->checkState() == CheckState::Refused) colour = t.warn;
    else if (lg.session->checkState() == CheckState::Skipped) colour = t.subtext;
    RECT line{dp(kMargin + kCardPad), dp(666), dp(330), dp(692)};
    ui::drawText(dc, fromUtf8(lg.session->updateLine()), line, f.body, colour, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Under the "Start Elite directly" checkbox when that is showing, which is a line shorter than the
    // card's second row; otherwise beside the pair of buttons, or beside the single one.
    const bool pair = lg.playVr && IsWindowVisible(lg.playVr);
    const bool single = lg.play && IsWindowVisible(lg.play);
    const bool underCheckbox = lg.direct && IsWindowVisible(lg.direct);
    const int captionLeft = underCheckbox ? lg.directLeft : pair ? kMargin + kCardPad + 252 : single ? kMargin + kCardPad + 134 : kMargin + kCardPad;
    RECT caption{dp(captionLeft), dp(underCheckbox ? 719 : 698), dp(kClientWidth - kMargin - kCardPad), dp(underCheckbox ? 749 : 742)};
    ui::drawText(dc, lg.playCaption, caption, f.caption, t.subtext, DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
}

void createLauncherControls(HWND window) {
    const ui::Fonts& f = ui::fonts();
    // Not "Update": that is what the Setup card's button says, and it installs into this folder.
    lg.update = ui::makeButton(window, L"Download", kIdUpdate, ui::ButtonStyle::Primary, f.body);
    lg.rc = ui::makeCheckbox(window, L"Include release candidates", kIdRc, false, f.body);
    lg.play = ui::makeButton(window, L"Play", kIdPlay, ui::ButtonStyle::Primary, f.body);
    lg.playVr = ui::makeButton(window, L"Play VR", kIdPlayVr, ui::ButtonStyle::Primary, f.body);
    lg.playFlat = ui::makeButton(window, L"Play flat", kIdPlayFlat, ui::ButtonStyle::Secondary, f.body);
    lg.direct = ui::makeCheckbox(window, L"Start Elite directly (no Frontier launcher)", kIdDirect, false, f.body);
    lg.closeAfter = ui::makeCheckbox(window, L"Close the launcher once Elite has started", kIdCloseAfter, false, f.body);
    for (HWND control : {lg.update, lg.rc, lg.play, lg.playVr, lg.playFlat, lg.direct, lg.closeAfter}) ShowWindow(control, SW_HIDE);
}

// The window is up: load the payload, ask GitHub, and say what start-up had to say.
void launcherBegin() {
    SessionEnv env;
    env.roots = launcherRootsFor(g.args);
    env.payloadDir = g.args.payloadDir;
    env.channelOverride = g.args.channel;
    env.closeAfterStartArg = g.args.closeAfterStart;
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
        case kIdRc:
            ui::toggleCheckbox(lg.rc);
            lg.session->setIncludeReleaseCandidates(ui::checkboxChecked(lg.rc));
            return true;
        case kIdDirect: {
            // Turning it on asks first, with No as the default; a No leaves it off. Turning it off just does.
            const bool want = !ui::checkboxChecked(lg.direct);
            // A Steam install is asked with Steam's words; one from Frontier's store is told about the login.
            const bool frontierStore = g.haveSurvey && _wcsicmp(g.survey.game.source.c_str(), L"Frontier launcher") == 0;
            const char* question = frontierStore ? kDirectLaunchQuestionFrontier : kDirectLaunchQuestion;
            lg.session->setDirectLaunch(want, [question]() {
                return MessageBoxW(g.window, fromUtf8(question).c_str(), kAppTitle,
                                   MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
            });
            updateLauncherControls();
            return true;
        }
        case kIdPlay:
        case kIdPlayVr:
        case kIdPlayFlat: {
            if (!g.haveSurvey) return true;
            const bool vr = id != kIdPlayFlat;
            lg.session->beginPlay(g.survey.game.source, g.survey.game.dir, vr, isElevated(), realRouteOps(), WatchNames(),
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
