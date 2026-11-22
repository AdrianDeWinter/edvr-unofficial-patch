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
    kIdPlayVr = 1083,
    kIdPlayFlat = 1084,
    kIdDirect = 1085,
    kIdCloseAfter = 1086,
    kIdSteamFrontier = 1087,
    kIdVrMode = 1088,
    kIdVrRefresh = 1089,
    kIdThemeToggle = 1090,
};

constexpr UINT_PTR kTimerVrProbe = 2001;
constexpr UINT kVrProbeIntervalMs = 5000;

struct VrModeChoice {
    edvr::VrHeadsetMode mode;
    const wchar_t* label;
};
// Elite Dangerous's in-game Options > Graphics > 3D headset modes, in dropdown order.
const VrModeChoice kVrModeChoices[] = {
    {edvr::VrHeadsetMode::NotSet, L"Not Set"},
    {edvr::VrHeadsetMode::HmdHeadphones, L"HMD (Headphones)"},
    {edvr::VrHeadsetMode::HmdSpeakers, L"HMD (Speakers)"},
    {edvr::VrHeadsetMode::HmdCinemaHeadphones, L"HMD Cinema (Headphones)"},
    {edvr::VrHeadsetMode::HmdCinemaSpeakers, L"HMD Cinema (Speakers)"},
};

// Posted by the session's worker threads: lParam is a heap std::function<void()> to run here.
const UINT kMsgLauncherJob = WM_APP + 2;

// Launch hero card geometry (96-dpi units) on Screen::Play.
constexpr int kLaunchTop = kContentTop;
constexpr int kLaunchBtnY = kLaunchTop + 38;
constexpr int kLaunchBtnH = 38;
constexpr int kLaunchDividerY = kLaunchBtnY + kLaunchBtnH + 12;
constexpr int kLaunchDirectY = kLaunchDividerY + 10;
constexpr int kLaunchOptWithDirectY = kLaunchDirectY + 30;
constexpr int kLaunchOptNoDirectY = kLaunchDividerY + 12;
constexpr int kLaunchBottomWithDirect = kLaunchOptWithDirectY + 42;
constexpr int kLaunchBottomNoDirect = kLaunchOptNoDirectY + 42;
constexpr int kPlayBannerH = 46;
constexpr int kVrCardH = 92;

struct LauncherGui {
    HWND update = nullptr;
    HWND rc = nullptr;
    HWND playVr = nullptr;
    HWND playFlat = nullptr;
    HWND direct = nullptr;           // "Start Elite directly", for Steam / Frontier / Epic installs
    HWND closeAfter = nullptr;       // "Close the launcher once Elite has started", saved in launcher.ini
    HWND steamFrontier = nullptr;    // "Use Frontier account login", for Steam direct launch
    HWND vrMode = nullptr;           // preferred VR headset mode (NotSet or HMD modes) for Settings.xml routes
    HWND vrRefresh = nullptr;        // manual Refresh button in the VR headset readout card
    HWND themeToggle = nullptr;      // bottom-left sun/moon light/dark theme toggle pill
    RECT vrModeLabelPx{};            // painted "VR 3D mode" label beside vrMode
    std::unique_ptr<LauncherSession> session;
    VrHeadsetReport vrReport;
    std::wstring vrReportGameDir;    // install folder `vrReport` belongs to
    std::thread vrProbeThread;
    HANDLE vrProbeDoneEvent = nullptr;
    std::atomic<bool> vrShuttingDown{false};
    bool vrProbing = false;
    bool vrProbePending = false;
    bool vrHaveReport = false;
    uint64_t vrProbeGen = 0;
    ULONGLONG vrLastProbeMs = 0;
    std::wstring playCaption;        // beside the Play buttons on Screen::Play
    int playCaptionLeft = 320;       // right of visible Play buttons, in design units
    int launchBottom = kLaunchBottomWithDirect;
    bool playBannerShown = false;    // update available strip under Launch on Screen::Play
    int updateTextRight = 460;       // right edge (design units) for the painted update status line
    bool restoreViaPlan = false;     // the user agreed to a settings restore the elevated copy does
    bool noPayloadShown = false;
    bool edhmRecovering = false;     // guard against re-entrant AV-interference recovery
    std::wstring edhmInterferenceAskedDir;  // avoids re-prompting on passive missing-DLL detection for this folder
    std::wstring developerNote;      // set per Install, read by confirmText
    // Cached route, remembered mode, and layout signature (#198).
    std::wstring cachedRouteSource;
    std::wstring cachedRouteDir;
    LaunchRoute  cachedRoute;
    bool         haveCachedRoute = false;
    std::wstring cachedModeDir;
    std::string  cachedMode;
    bool         haveCachedMode = false;
    bool         layoutDirty = true;
    Screen       lastLayoutScreen = Screen::Install;
    UINT         lastLayoutDpi = 0;
    bool         lastAvailable = false;
    bool         lastPlayBanner = false;
    bool         lastShowVr = false;
    bool         lastShowFlat = false;
    bool         lastShowDirect = false;
    std::wstring lastUpdateLabel;
    std::wstring lastInstallLabel;
    std::wstring lastDirectLabel;
};
LauncherGui lg;

void invalidateLauncherCache() {
    lg.haveCachedRoute = false;
    lg.haveCachedMode = false;
    lg.layoutDirty = true;
}

int launcherVrTop() {
    return lg.playBannerShown ? (lg.launchBottom + 10 + kPlayBannerH + 10) : (lg.launchBottom + 12);
}

int launcherStatusTop() {
    return launcherVrTop() + kVrCardH + (lg.playBannerShown ? 10 : 12);
}

// Resolves a cryptographically verified openxr_loader.dll from the signed
// release manifest (matching either the installed copy or the signed cache).
// Never returns an unverified DLL path.
void resolveVerifiedOpenXrLoader(const std::wstring& gameDir, std::wstring* pathOut, std::string* shaOut) {
    if (pathOut) pathOut->clear();
    if (shaOut) shaOut->clear();
    if (!lg.session || lg.session->payloadState() != PayloadState::Ready ||
        lg.session->payload().kind != PayloadKind::Manifest) {
        return;
    }
    const PayloadFileEntry* loader = payloadInfo().find("openxr_loader");
    if (!loader || !loader->valid || loader->sha.size() != 64) return;

    if (!gameDir.empty()) {
        const std::wstring gameLoader = joinPath(gameDir, L"Openvr\\win64\\openxr_loader.dll");
        if (fileExists(gameLoader)) {
            const DllInfo info = probeDll(gameLoader);
            if (info.is64 && info.hasXrGetInstanceProcAddr && info.sha256 == loader->sha) {
                if (pathOut) *pathOut = gameLoader;
                if (shaOut) *shaOut = loader->sha;
                return;
            }
        }
    }
    const PayloadCache cache(launcherRootsFor(g.args).cacheDir());
    const std::wstring cached = cache.pathFor(loader->sha);
    if (!cached.empty() && fileExists(cached)) {
        if (pathOut) *pathOut = cached;
        if (shaOut) *shaOut = loader->sha;
    }
}

void startVrHeadsetProbe(bool force) {
    if (!g.window || lg.vrShuttingDown.load()) return;
    const std::wstring gameDir = g.haveSurvey ? g.survey.game.dir : std::wstring();
    const bool installChanged =
        lg.vrHaveReport && _wcsicmp(lg.vrReportGameDir.c_str(), gameDir.c_str()) != 0;
    if (installChanged) {
        lg.vrHaveReport = false;
        lg.vrReportGameDir.clear();
        lg.vrReport = VrHeadsetReport{};
        lg.vrReport.status = VrStatusKind::Checking;
    }
    if (lg.vrProbing) {
        if (force || installChanged) lg.vrProbePending = true;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!force && !installChanged && lg.vrHaveReport && now - lg.vrLastProbeMs < 3000) return;

    if (lg.vrProbeThread.joinable()) lg.vrProbeThread.join();
    if (!lg.vrProbeDoneEvent) lg.vrProbeDoneEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (lg.vrProbeDoneEvent) ResetEvent(lg.vrProbeDoneEvent);

    lg.vrProbing = true;
    lg.vrProbePending = false;
    const uint64_t gen = ++lg.vrProbeGen;
    if (!lg.vrHaveReport) {
        lg.vrReport = VrHeadsetReport{};
        lg.vrReport.status = VrStatusKind::Checking;
    }
    if (lg.vrRefresh) {
        SetWindowTextW(lg.vrRefresh, tr(L"Checking..."));
        EnableWindow(lg.vrRefresh, FALSE);
    }
    InvalidateRect(g.window, nullptr, FALSE);

    VrProbeOptions opts;
    opts.gameDir = gameDir;
    resolveVerifiedOpenXrLoader(gameDir, &opts.verifiedLoaderPath, &opts.verifiedLoaderSha256);
    opts.allowLiveProbe = true;
    opts.liveProbeTimeoutMs = 2500;
    const HWND window = g.window;
    HANDLE doneEvent = lg.vrProbeDoneEvent;

    lg.vrProbeThread = std::thread([window, opts, gen, doneEvent]() {
        VrHeadsetReport report = probeVrHeadset(opts);
        if (doneEvent) SetEvent(doneEvent);
        if (lg.vrShuttingDown.load()) return;
        std::function<void()>* job = new std::function<void()>([report = std::move(report),
                                                                probedDir = opts.gameDir, gen]() {
            if (lg.vrShuttingDown.load() || gen != lg.vrProbeGen) return;
            lg.vrProbing = false;
            lg.vrHaveReport = true;
            lg.vrReportGameDir = probedDir;
            lg.vrLastProbeMs = GetTickCount64();
            lg.vrReport = report;
            if (lg.vrRefresh) {
                SetWindowTextW(lg.vrRefresh, tr(L"Refresh"));
                EnableWindow(lg.vrRefresh, TRUE);
            }
            InvalidateRect(g.window, nullptr, FALSE);
            if (lg.vrProbePending) startVrHeadsetProbe(true);
        });
        if (!PostMessageW(window, kMsgLauncherJob, 0, reinterpret_cast<LPARAM>(job))) delete job;
    });
}

bool launcherTimer(WPARAM timerId) {
    if (timerId != kTimerVrProbe) return false;
    if (g.screen == Screen::Play && g.window && !IsIconic(g.window) && !lg.vrProbing) {
        startVrHeadsetProbe(false);
    }
    return true;
}

bool launcherPayloadReady() { return lg.session && lg.session->payloadState() == PayloadState::Ready; }

bool launcherBusy() { return lg.session && (lg.session->updating() || lg.session->applying()); }

bool launcherEdhmBusy() { return launcherBusy() || (lg.session && lg.session->playing()); }

bool launcherFolderHasEdup() {
    return g.haveSurvey && (g.survey.d3d11.kind == DllKind::Edvr || g.survey.openvrCurrent.kind == DllKind::Edvr ||
                            g.survey.state.present);
}

void launcherUpdateSettingsToggle();
bool launcherCheckEdhmInterference(bool refreshDisk = false);

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
    const bool onPlay = g.screen == Screen::Play;
    const bool onSetup = g.screen == Screen::Install;
    const bool payloadOk = launcherPayloadReady();
    const bool payloadLoading = lg.session->payloadState() == PayloadState::Loading;
    const bool busy = launcherBusy();
    const bool gameHere = g.haveSurvey && g.survey.gameRunningHere;

    // Install and Repair need the payload; Uninstall works without one (the planner then only keeps more).
    const bool survey = g.haveSurvey;
    const wchar_t* installLabel = installButtonLabel();
    if (g.install && lg.lastInstallLabel != installLabel) {
        lg.lastInstallLabel = installLabel;
        SetWindowTextW(g.install, installLabel);
    }
    EnableWindow(g.install, survey && payloadOk && !busy && !gameHere);
    EnableWindow(g.repair, survey && payloadOk && !busy && !gameHere);
    EnableWindow(g.uninstall, survey && !payloadLoading && !busy && !gameHere);

    const bool available = lg.session->checkState() == CheckState::Available && !lg.session->updating();
    const wchar_t* updateLabel = launcherFolderHasEdup() ? tr(L"Download & update") : tr(L"Download & install");
    lg.playBannerShown = onPlay && (available || lg.session->updating());

    // Play route and options (shown on Screen::Play).
    lg.playCaption.clear();
    bool showPair = false, showDirect = false, showSteamFrontier = false, showVrMode = false,
         playEnabled = false;
    bool showVrButton = false, showFlatButton = false;  // within the pair: only the modes this folder has installed
    const wchar_t* directLabel = tr(L"Start Elite directly (no Frontier launcher)");
    if (survey) {
        const LaunchRoute& route = launcherRouteFor(g.survey.game.source, g.survey.game.dir);
        const bool anyGame = g.survey.gameRunningHere || g.survey.gameRunningElsewhere;
        playEnabled = !lg.session->playing() && !busy && !anyGame && !g.survey.gameRunStateUnknown;
        const bool frontierStore = route.kind == RouteKind::Frontier && _wcsicmp(g.survey.game.source.c_str(), L"Frontier launcher") == 0;
        const bool epic = route.kind == RouteKind::Epic;
        showDirect = (route.kind == RouteKind::Steam || frontierStore || epic) && lg.session->directLaunchOffered();
        const bool directOn = showDirect && lg.session->directLaunch();
        showSteamFrontier = route.kind == RouteKind::Steam && directOn;
        directLabel = epic ? tr(L"Skip the Epic launcher") : tr(L"Start Elite directly (no Frontier launcher)");
        if (route.kind != RouteKind::None) {
            // A button only for a mode this folder has installed: a VR-only install gets no flat button, and the reverse.
            // An empty scope (no install the survey can name) keeps both, as before.
            // Every valid route (Frontier, Steam, Epic, direct or store) offers Play VR / Play flat;
            // non-direct Steam/Epic write StereoscopicMode in Settings.xml before opening the store URI.
            const std::string scope = installedScope(g.survey);
            const bool hasVr = scope.empty() || scope == "both" || scope == "vr";
            const bool hasFlat = scope.empty() || scope == "both" || scope == "flat";
            const bool bothModes = hasVr && hasFlat;
            showPair = true;
            showVrButton = hasVr;
            showFlatButton = hasFlat;
            showVrMode = !directOn && route.kind != RouteKind::Frontier && hasVr;
            const bool steamFrontierOn = showSteamFrontier && lg.session->steamFrontierLogin();
            if (route.kind == RouteKind::Frontier || directOn) {
                lg.playCaption = directOn ? (epic ? tr(L"Signs in to Epic and Frontier, starts Elite itself, and exits once Elite is running.")
                                              : frontierStore ? tr(L"Signs in itself. If that fails, it starts EDLaunch.")
                                              : steamFrontierOn ? tr(L"Signs in with your Frontier account. If that fails, it starts through Steam.")
                                                                : tr(L"Signs in itself. If that fails, it starts through Steam."))
                                 : bothModes ? tr(L"Starts EDLaunch with the mode you pick, then closes it when Elite exits.")
                                 : hasVr ? tr(L"Starts EDLaunch in VR, then closes it when Elite exits.")
                                         : tr(L"Starts EDLaunch in flat, then closes it when Elite exits.");
            } else {
                lg.playCaption = route.kind == RouteKind::Steam
                                     ? tr(L"Sets Elite's 3D mode in Settings.xml, then starts Elite through Steam.")
                                     : tr(L"Sets Elite's 3D mode in Settings.xml, then starts Elite through Epic.");
            }
            // With a choice, the remembered mode is the primary button; a lone button is the primary one.
            const bool flat = launcherRememberedMode() == "flat";
            ui::setButtonStyle(lg.playVr, (bothModes ? !flat : hasVr) ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary);
            ui::setButtonStyle(lg.playFlat, (bothModes ? flat : hasFlat) ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary);
        } else {
            lg.playCaption = fromUtf8(route.why);
        }
        if (gameHere || g.survey.gameRunningElsewhere) lg.playCaption = tr(L"Elite is already running.");
    }
    if (lg.session->playing() || !lg.session->playLine().empty()) lg.playCaption = fromUtf8(lg.session->playLine());

    if (lg.direct && lg.lastDirectLabel != directLabel) {
        lg.lastDirectLabel = directLabel;
        SetWindowTextW(lg.direct, directLabel);
    }

    lg.launchBottom = showDirect ? kLaunchBottomWithDirect : kLaunchBottomNoDirect;

    // Re-measure and move controls only when screen, scale, labels or button visibility actually changed,
    // avoiding repeated GetDC/textWidth/MoveWindow work on every download progress tick (#198).
    const bool needRelayout = lg.layoutDirty || lg.lastLayoutScreen != g.screen || lg.lastLayoutDpi != g.dpi ||
                              lg.lastAvailable != available || lg.lastPlayBanner != lg.playBannerShown ||
                              lg.lastShowVr != (showPair && showVrButton) ||
                              lg.lastShowFlat != (showPair && showFlatButton) || lg.lastShowDirect != showDirect ||
                              lg.lastUpdateLabel != updateLabel;
    if (needRelayout) {
        lg.layoutDirty = false;
        lg.lastLayoutScreen = g.screen;
        lg.lastLayoutDpi = g.dpi;
        lg.lastAvailable = available;
        lg.lastPlayBanner = lg.playBannerShown;
        lg.lastShowVr = showPair && showVrButton;
        lg.lastShowFlat = showPair && showFlatButton;
        lg.lastShowDirect = showDirect;
        lg.lastUpdateLabel = updateLabel;

        const int innerLeft = kMargin + kCardPad;
        const int innerRight = kClientWidth - kMargin - kCardPad;
        HDC dc = GetDC(g.window);
        const HFONT font = ui::fonts().body;

        // Update button (shared HWND: sits in Play's update banner or Setup's Updates card).
        if (lg.update) {
            SetWindowTextW(lg.update, updateLabel);
            const int labelPx = ui::textWidth(dc, updateLabel, font) + dp(28);
            int btnW = g.dpi > 0 ? MulDiv(labelPx, 96, static_cast<int>(g.dpi)) : 140;
            if (btnW < 126) btnW = 126;
            if (btnW > 210) btnW = 210;
            const int btnLeft = innerRight - btnW;
            const int btnY = onPlay ? (lg.launchBottom + 10 + (kPlayBannerH - 28) / 2) : (kUpdatesTop + 34);
            lg.updateTextRight = available ? (btnLeft - 12) : innerRight;
            MoveWindow(lg.update, dp(btnLeft), dp(btnY), dp(btnW), dp(28), TRUE);
        }
        if (lg.rc) {
            const int rcW = 248;
            MoveWindow(lg.rc, dp(innerRight - rcW), dp(kUpdatesTop + 8), dp(rcW), dp(22), TRUE);
        }

        // Play VR / Play flat buttons sized to localized text so German/Russian labels never clip.
        int vrW = 126, flatW = 126;
        if (lg.playVr) {
            wchar_t lbl[128]{};
            GetWindowTextW(lg.playVr, lbl, 128);
            const int px = ui::textWidth(dc, lbl, font) + dp(34);
            vrW = g.dpi > 0 ? MulDiv(px, 96, static_cast<int>(g.dpi)) : 126;
            if (vrW < 122) vrW = 122;
            if (vrW > 180) vrW = 180;
        }
        if (lg.playFlat) {
            wchar_t lbl[128]{};
            GetWindowTextW(lg.playFlat, lbl, 128);
            const int px = ui::textWidth(dc, lbl, font) + dp(34);
            flatW = g.dpi > 0 ? MulDiv(px, 96, static_cast<int>(g.dpi)) : 126;
            if (flatW < 122) flatW = 122;
            if (flatW > 190) flatW = 190;
        }
        int curX = innerLeft;
        if (showPair && showVrButton) {
            MoveWindow(lg.playVr, dp(curX), dp(kLaunchBtnY), dp(vrW), dp(kLaunchBtnH), TRUE);
            curX += vrW + 10;
        }
        if (showPair && showFlatButton) {
            MoveWindow(lg.playFlat, dp(curX), dp(kLaunchBtnY), dp(flatW), dp(kLaunchBtnH), TRUE);
            curX += flatW + 10;
        }
        lg.playCaptionLeft = (showPair && (showVrButton || showFlatButton)) ? (curX + 6) : innerLeft;

        // Direct launch checkbox gets the full interior width on its own clean row.
        if (lg.direct)
            MoveWindow(lg.direct, dp(innerLeft), dp(kLaunchDirectY), dp(innerRight - innerLeft), dp(22), TRUE);

        // Bottom options row inside Launch card.
        const int optY = showDirect ? kLaunchOptWithDirectY : kLaunchOptNoDirectY;
        if (lg.closeAfter) MoveWindow(lg.closeAfter, dp(innerLeft), dp(optY + 3), dp(340), dp(22), TRUE);
        if (lg.steamFrontier)
            MoveWindow(lg.steamFrontier, dp(innerLeft + 352), dp(optY + 3), dp(innerRight - (innerLeft + 352)), dp(22), TRUE);
        if (lg.vrMode) {
            const int comboW = dp(212);
            const int comboRight = dp(innerRight);
            const int comboLeft = comboRight - comboW;
            const int comboTop = dp(optY);
            MoveWindow(lg.vrMode, comboLeft, comboTop, comboW, dp(28), TRUE);
            SendMessageW(lg.vrMode, CB_SETITEMHEIGHT, 0, dp(24));
            SendMessageW(lg.vrMode, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), dp(20));
            RECT rc{};
            GetWindowRect(lg.vrMode, &rc);
            const int frame = (rc.bottom - rc.top) - dp(20);
            const int fieldItem = dp(28) - frame;
            if (fieldItem > 0) SendMessageW(lg.vrMode, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), fieldItem);
            lg.vrModeLabelPx = RECT{dp(innerLeft + 344), comboTop, comboLeft - dp(8), comboTop + dp(28)};
        }

        if (lg.vrRefresh) {
            const int wRefresh = ui::textWidth(dc, tr(L"Refresh"), ui::fonts().caption);
            const int wChecking = ui::textWidth(dc, tr(L"Checking..."), ui::fonts().caption);
            const int labelPx = (wRefresh > wChecking ? wRefresh : wChecking) + dp(22);
            int btnW = g.dpi > 0 ? MulDiv(labelPx, 96, static_cast<int>(g.dpi)) : 82;
            if (btnW < 76) btnW = 76;
            if (btnW > 136) btnW = 136;
            const int vrTop = launcherVrTop();
            MoveWindow(lg.vrRefresh, dp(innerRight - btnW), dp(vrTop + 7), dp(btnW), dp(22), TRUE);
        }

        if (lg.themeToggle) {
            MoveWindow(lg.themeToggle, dp(kMargin), dp(kTipTop - 2), dp(48), dp(22), TRUE);
        }

        ReleaseDC(g.window, dc);
    }

    ShowWindow(lg.update, (onPlay || onSetup) && available ? SW_SHOW : SW_HIDE);
    EnableWindow(lg.update, !lg.session->applying());
    ShowWindow(lg.rc, onSetup ? SW_SHOW : SW_HIDE);
    if (lg.vrRefresh) {
        ShowWindow(lg.vrRefresh, onPlay ? SW_SHOW : SW_HIDE);
        EnableWindow(lg.vrRefresh, !lg.vrProbing);
    }

    ShowWindow(lg.playVr, onPlay && showPair && showVrButton ? SW_SHOW : SW_HIDE);
    ShowWindow(lg.playFlat, onPlay && showPair && showFlatButton ? SW_SHOW : SW_HIDE);
    for (HWND button : {lg.playVr, lg.playFlat}) EnableWindow(button, playEnabled);

    ShowWindow(lg.direct, onPlay && showDirect ? SW_SHOW : SW_HIDE);
    EnableWindow(lg.direct, !lg.session->playing() && !busy);
    if (ui::checkboxChecked(lg.direct) != lg.session->directLaunch()) ui::toggleCheckbox(lg.direct);

    ShowWindow(lg.closeAfter, onPlay ? SW_SHOW : SW_HIDE);
    EnableWindow(lg.closeAfter, !busy);
    if (ui::checkboxChecked(lg.closeAfter) != lg.session->closeAfterStartSetting()) ui::toggleCheckbox(lg.closeAfter);

    ShowWindow(lg.steamFrontier, onPlay && showSteamFrontier ? SW_SHOW : SW_HIDE);
    EnableWindow(lg.steamFrontier, !lg.session->playing() && !busy);
    if (lg.steamFrontier && ui::checkboxChecked(lg.steamFrontier) != lg.session->steamFrontierLoginSetting())
        ui::toggleCheckbox(lg.steamFrontier);

    if (lg.vrMode) {
        ShowWindow(lg.vrMode, onPlay && showVrMode ? SW_SHOW : SW_HIDE);
        EnableWindow(lg.vrMode, !lg.session->playing() && !busy);
        const int wantMode = edvr::vrHeadsetModeToken(lg.session->vrStereoMode());
        LRESULT match = 0;
        const LRESULT count = SendMessageW(lg.vrMode, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < count; ++i) {
            if (static_cast<int>(SendMessageW(lg.vrMode, CB_GETITEMDATA, static_cast<WPARAM>(i), 0)) == wantMode) {
                match = i;
                break;
            }
        }
        if (SendMessageW(lg.vrMode, CB_GETCURSEL, 0, 0) != match)
            SendMessageW(lg.vrMode, CB_SETCURSEL, static_cast<WPARAM>(match), 0);
    }
    launcherUpdateSettingsToggle();
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
    for (HWND control :
         {lg.update, lg.rc, lg.playVr, lg.playFlat, lg.direct, lg.closeAfter, lg.steamFrontier, lg.vrMode})
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui::fonts().body), TRUE);
    if (lg.vrRefresh)
        SendMessageW(lg.vrRefresh, WM_SETFONT, reinterpret_cast<WPARAM>(ui::fonts().caption), TRUE);
    if (lg.themeToggle) {
        MoveWindow(lg.themeToggle, dp(kMargin), dp(kTipTop - 2), dp(48), dp(22), TRUE);
    }
    lg.layoutDirty = true;
    updateLauncherControls();
}

std::wstring fillVrPlaceholders(const wchar_t* pattern, const std::wstring& a,
                                const std::wstring& b = L"", const std::wstring& c = L"") {
    std::wstring out = pattern ? pattern : L"";
    const std::wstring* vals[3] = {&a, &b, &c};
    for (int i = 0; i < 3; ++i) {
        const size_t at = out.find(L"%s");
        if (at == std::wstring::npos) break;
        out.replace(at, 2, *vals[i]);
    }
    return out;
}

void paintLauncherVrCard(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    const int top = launcherVrTop();
    const int bottom = top + kVrCardH;

    const RECT card{dp(kMargin), dp(top), dp(kClientWidth - kMargin), dp(bottom)};
    ui::paintCard(dc, card, g.dpi);

    const VrHeadsetReport& vr = lg.vrReport;
    const wchar_t* badgeText = tr(L"Checking...");
    COLORREF badgeColour = t.muted;
    switch (vr.status) {
        case VrStatusKind::Ready:
            badgeText = tr(L"Ready");
            badgeColour = t.good;
            break;
        case VrStatusKind::Standby:
            badgeText = tr(L"Standby");
            badgeColour = t.warn;
            break;
        case VrStatusKind::RuntimeInactive:
            badgeText = tr(L"Runtime off");
            badgeColour = (vr.headsetFromUsb || vr.runtimeMismatch) ? t.warn : t.muted;
            break;
        case VrStatusKind::NotConfigured:
            badgeText = tr(L"Not configured");
            badgeColour = t.warn;
            break;
        case VrStatusKind::Error:
            badgeText = tr(L"Check failed");
            badgeColour = t.bad;
            break;
        case VrStatusKind::Checking:
        default:
            badgeText = tr(L"Checking...");
            badgeColour = t.muted;
            break;
    }

    const int innerLeftPx = dp(kMargin + kCardPad);
    const int innerRightPx = dp(kClientWidth - kMargin - kCardPad);

    const std::wstring heading = tr(L"VR headset");
    const int headW = ui::textWidth(dc, heading, f.heading);
    RECT headRect{innerLeftPx, dp(top + 7), innerLeftPx + headW + dp(4), dp(top + 29)};
    ui::drawText(dc, heading, headRect, f.heading, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    ui::paintStatusPill(dc, innerLeftPx + headW + dp(10), dp(top + 8), dp(20), badgeColour, badgeText,
                        f.caption, g.dpi);

    // Primary readout line: detected headset (left, bold) + active VR runtime (right, caption).
    std::wstring headsetLine;
    bool headsetDim = false;
    if (vr.status == VrStatusKind::Checking) {
        headsetLine = tr(L"Checking VR headset and runtime...");
        headsetDim = true;
    } else if (!vr.headsetName.empty()) {
        headsetLine = vr.headsetName;
    } else if (vr.status == VrStatusKind::Ready) {
        headsetLine = tr(L"VR headset detected");
    } else if (vr.status == VrStatusKind::Standby) {
        headsetLine = tr(L"Waiting for headset");
        headsetDim = true;
    } else if (vr.status == VrStatusKind::NotConfigured) {
        headsetLine = tr(L"No VR runtime found");
        headsetDim = true;
    } else {
        headsetLine = tr(L"No headset detected");
        headsetDim = true;
    }

    std::wstring runtimeValue = vr.runtimeDisplayName.empty() ? std::wstring(tr(L"None")) : vr.runtimeDisplayName;
    if (!vr.runtimeVersion.empty() && !vr.runtimeDisplayName.empty()) {
        runtimeValue += L" " + vr.runtimeVersion;
    }
    const std::wstring runtimeLine =
        vr.status == VrStatusKind::Checking ? std::wstring()
                                            : (std::wstring(tr(L"VR runtime")) + L": " + runtimeValue);

    int headsetRightPx = innerRightPx;
    if (!runtimeLine.empty()) {
        const int maxRuntimeW = (innerRightPx - innerLeftPx) * 46 / 100;
        int rtW = ui::textWidth(dc, runtimeLine, f.caption) + dp(4);
        if (rtW > maxRuntimeW) rtW = maxRuntimeW;
        const int rtLeftPx = innerRightPx - rtW;
        RECT rtRect{rtLeftPx, dp(top + 35), innerRightPx, dp(top + 54)};
        ui::drawText(dc, runtimeLine, rtRect, f.caption, t.subtext,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        headsetRightPx = rtLeftPx - dp(12);
    }

    RECT headsetRect{innerLeftPx, dp(top + 34), headsetRightPx, dp(top + 54)};
    ui::drawText(dc, headsetLine, headsetRect, headsetDim ? f.body : f.bodyBold,
                 headsetDim ? t.subtext : t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Detail & guidance line (up to 2 lines).
    std::wstring detailLine;
    COLORREF detailColour = t.subtext;
    const std::wstring rtName = vr.runtimeDisplayName.empty() ? std::wstring(L"OpenXR") : vr.runtimeDisplayName;
    switch (vr.status) {
        case VrStatusKind::Checking:
            detailLine = tr(L"Inspecting the active OpenXR runtime, running VR services, and connected hardware.");
            break;
        case VrStatusKind::Ready:
            if (vr.openCompositeWarning) {
                detailLine = tr(L"OpenComposite is active. ED-UP already includes its own OpenXR bridge, so stacking both can break VR.");
                detailColour = t.warn;
            } else if (!vr.transportName.empty()) {
                detailLine = fillVrPlaceholders(tr(L"Connected via %s and ready for VR launch."), vr.transportName);
            } else {
                detailLine = tr(L"Connected and ready for VR launch.");
            }
            break;
        case VrStatusKind::Standby:
            if (vr.openCompositeWarning) {
                detailLine = tr(L"OpenComposite is active. ED-UP already includes its own OpenXR bridge, so stacking both can break VR.");
                detailColour = t.warn;
            } else {
                detailLine = fillVrPlaceholders(
                    tr(L"%s is running, but no active headset responded. Wake or connect your headset, then refresh."),
                    rtName);
            }
            break;
        case VrStatusKind::RuntimeInactive:
            if (vr.openCompositeWarning) {
                detailLine = tr(L"OpenComposite is active. ED-UP already includes its own OpenXR bridge, so stacking both can break VR.");
                detailColour = t.warn;
            } else if (vr.runtimeMismatch && !vr.mismatchRuntimeName.empty()) {
                detailLine = fillVrPlaceholders(
                    tr(L"%s is running while %s is set as your active OpenXR runtime. Switch OpenXR or start %s."),
                    vr.mismatchRuntimeName, rtName, rtName);
                detailColour = t.warn;
            } else if (vr.headsetFromUsb) {
                detailLine = fillVrPlaceholders(
                    tr(L"Detected over USB. Start %s to connect before launching Elite in VR."), rtName);
            } else {
                detailLine = fillVrPlaceholders(
                    tr(L"%s is your active OpenXR runtime, but is not running yet. Start it or connect your headset."),
                    rtName);
            }
            break;
        case VrStatusKind::NotConfigured:
            if (vr.activeRuntimeMissingFile) {
                detailLine = tr(L"Active OpenXR runtime file is missing. Open SteamVR, Meta Horizon Link, Virtual Desktop, or Pimax Play to restore it.");
                detailColour = t.warn;
            } else if (vr.headsetFromUsb) {
                detailLine = tr(L"Detected over USB, but no active OpenXR runtime is registered yet.");
                detailColour = t.warn;
            } else {
                detailLine = tr(L"No active OpenXR runtime is registered. Start SteamVR, Meta Horizon Link, Virtual Desktop, Pimax Play, or WiVRn to enable VR.");
            }
            break;
        case VrStatusKind::Error:
            if (vr.liveProbeTimedOut) {
                detailLine = fillVrPlaceholders(
                    tr(L"%s did not respond in time. Restart it and press Refresh."), rtName);
            } else {
                detailLine = fillVrPlaceholders(
                    tr(L"The active OpenXR runtime could not be queried. Restart %s and refresh."), rtName);
            }
            detailColour = t.warn;
            break;
    }

    RECT detailRect{innerLeftPx, dp(top + 56), innerRightPx, dp(bottom - 6)};
    ui::drawText(dc, detailLine, detailRect, f.caption, detailColour,
                 DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
}

std::wstring launcherFormattedUpdateLine(COLORREF* colourOut) {
    const ui::Theme& t = ui::theme();
    COLORREF colour = t.text;
    if (lg.session->checkState() == CheckState::Refused) colour = t.warn;
    else if (lg.session->checkState() == CheckState::Skipped) colour = t.subtext;
    if (colourOut) *colourOut = colour;
    std::string statusLine = lg.session->updateLine();
    if (lg.session->checkState() == CheckState::Available && g.haveSurvey && g.survey.state.present &&
        !g.survey.state.edvrVersion.empty() && !installRecordStale(g.survey) &&
        statusLine == ("ED-UP " + g.survey.state.edvrVersion + " is available.")) {
        statusLine = "ED-UP " + g.survey.state.edvrVersion + " package available for repair and updates.";
    }
    return trLine(statusLine);
}

// Paints the Launch hero card and optional inline update notification strip on Screen::Play.
void paintLauncherPlay(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();

    const RECT hero{dp(kMargin), dp(kLaunchTop), dp(kClientWidth - kMargin), dp(lg.launchBottom)};
    ui::paintAccentCard(dc, hero, g.dpi);

    RECT launchHead{dp(kMargin + kCardPad), dp(kLaunchTop + 10), dp(kClientWidth - kMargin - kCardPad),
                    dp(kLaunchTop + 28)};
    ui::drawText(dc, tr(L"Launch"), launchHead, f.heading, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT divider{dp(kMargin + kCardPad), dp(kLaunchDividerY), dp(kClientWidth - kMargin - kCardPad),
                 dp(kLaunchDividerY) + 1};
    ui::fillRect(dc, divider, t.cardBorder);

    if (!lg.session) return;

    RECT caption{dp(lg.playCaptionLeft), dp(kLaunchBtnY - 2), dp(kClientWidth - kMargin - kCardPad),
                 dp(kLaunchBtnY + kLaunchBtnH + 2)};
    ui::drawText(dc, lg.playCaption, caption, f.caption, t.subtext, DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_END_ELLIPSIS);

    if (lg.vrMode && IsWindowVisible(lg.vrMode)) {
        ui::drawText(dc, tr(L"VR 3D mode"), lg.vrModeLabelPx, f.caption, t.subtext,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (lg.playBannerShown) {
        const int bannerTop = lg.launchBottom + 10;
        const RECT banner{dp(kMargin), dp(bannerTop), dp(kClientWidth - kMargin), dp(bannerTop + kPlayBannerH)};
        ui::fillRounded(dc, banner, dp(8), t.cardBg);
        ui::strokeRounded(dc, banner, dp(8), t.accent);
        COLORREF colour = t.text;
        const std::wstring text = launcherFormattedUpdateLine(&colour);
        RECT line{dp(kMargin + kCardPad), dp(bannerTop + 8), dp(lg.updateTextRight), dp(bannerTop + kPlayBannerH - 8)};
        ui::drawText(dc, text, line, f.body, colour, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    paintLauncherVrCard(dc);
}

// Paints the Updates card at the top of Screen::Install (Setup tab).
void paintLauncherUpdatesCard(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();

    const RECT updatesCard{dp(kMargin), dp(kUpdatesTop), dp(kClientWidth - kMargin), dp(kUpdatesBottom)};
    ui::paintCard(dc, updatesCard, g.dpi);
    RECT updatesHead{dp(kMargin + kCardPad), dp(kUpdatesTop + 8), dp(kMargin + kCardPad + 200),
                     dp(kUpdatesTop + 26)};
    ui::drawText(dc, tr(L"Updates"), updatesHead, f.heading, t.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!lg.session) return;

    COLORREF colour = t.text;
    const std::wstring text = launcherFormattedUpdateLine(&colour);
    RECT line{dp(kMargin + kCardPad), dp(kUpdatesTop + 34), dp(lg.updateTextRight), dp(kUpdatesTop + 62)};
    ui::drawText(dc, text, line, f.body, colour, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void refillVrModeChoices() {
    if (!lg.vrMode) return;
    int selectedMode = lg.session ? edvr::vrHeadsetModeToken(lg.session->vrStereoMode()) : 0;
    const LRESULT cur = SendMessageW(lg.vrMode, CB_GETCURSEL, 0, 0);
    if (cur != CB_ERR)
        selectedMode = static_cast<int>(SendMessageW(lg.vrMode, CB_GETITEMDATA, static_cast<WPARAM>(cur), 0));
    SendMessageW(lg.vrMode, CB_RESETCONTENT, 0, 0);
    LRESULT match = 0;
    for (const VrModeChoice& choice : kVrModeChoices) {
        const LRESULT idx = SendMessageW(lg.vrMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(choice.label)));
        if (idx >= 0) {
            const int token = edvr::vrHeadsetModeToken(choice.mode);
            SendMessageW(lg.vrMode, CB_SETITEMDATA, static_cast<WPARAM>(idx), static_cast<LPARAM>(token));
            if (token == selectedMode) match = idx;
        }
    }
    SendMessageW(lg.vrMode, CB_SETCURSEL, static_cast<WPARAM>(match), 0);
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

void syncThemeToggleLabel() {
    if (!lg.themeToggle) return;
    SetWindowTextW(lg.themeToggle, ui::theme().dark ? tr(L"Theme: dark (switch to light)")
                                                    : tr(L"Theme: light (switch to dark)"));
}

// The words on the window, in the language `code` names. Does not write launcher.ini.
void applyLauncherLanguage(const std::string& code) {
    l10nSet(code.empty() ? systemGameLanguageCode() : canonicalGameLanguage(code));
    invalidateLauncherCache();
    setControlText(kIdTabPlay, L"Play");
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
    setControlText(kIdPlayVr, L"Play VR");
    setControlText(kIdPlayFlat, L"Play flat");
    setControlText(kIdDirect, L"Start Elite directly (no Frontier launcher)");
    setControlText(kIdCloseAfter, L"Close the launcher once Elite has started");
    setControlText(kIdSteamFrontier, L"Use Frontier account login");
    setControlText(kIdDiscord, L"Ask on Discord");
    setControlText(kIdCopyPath, L"Copy path");
    setControlText(kIdShowFolder, L"Show folder");
    if (lg.vrRefresh) SetWindowTextW(lg.vrRefresh, lg.vrProbing ? tr(L"Checking...") : tr(L"Refresh"));
    syncThemeToggleLabel();
    if (g.search) SendMessageW(g.search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(L"Search settings")));
    refillScopeChoices();
    refillVrModeChoices();
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

void applyNativeControlThemes() {
    const bool dark = ui::theme().dark;
    const wchar_t* cfd = dark ? L"DarkMode_CFD" : L"";
    const wchar_t* exp = dark ? L"DarkMode_Explorer" : L"";
    if (g.combo) SetWindowTheme(g.combo, cfd, nullptr);
    if (g.scopeCombo) SetWindowTheme(g.scopeCombo, cfd, nullptr);
    if (g.languageCombo) SetWindowTheme(g.languageCombo, cfd, nullptr);
    if (lg.vrMode) SetWindowTheme(lg.vrMode, cfd, nullptr);
    if (g.search) SetWindowTheme(g.search, cfd, nullptr);
    if (g.report) SetWindowTheme(g.report, exp, nullptr);
    if (g.settingsList) SetWindowTheme(g.settingsList, exp, nullptr);
}

void applyLauncherTheme(const std::string& mode) {
    if (mode == "dark") ui::setThemeMode(ui::ThemeMode::Dark);
    else if (mode == "light") ui::setThemeMode(ui::ThemeMode::Light);
    else ui::setThemeMode(ui::ThemeMode::System);
    syncThemeToggleLabel();
    if (g.window) {
        ui::applyWindowChrome(g.window);
        applyNativeControlThemes();
        RedrawWindow(g.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE | RDW_UPDATENOW);
    }
}

void createLauncherControls(HWND window) {
    const ui::Fonts& f = ui::fonts();
    lg.update = ui::makeButton(window, L"Download & install", kIdUpdate, ui::ButtonStyle::Primary, f.body);
    lg.rc = ui::makeCheckbox(window, L"Include release candidates", kIdRc, false, f.body);
    lg.playVr = ui::makeButton(window, L"Play VR", kIdPlayVr, ui::ButtonStyle::Primary, f.body);
    lg.playFlat = ui::makeButton(window, L"Play flat", kIdPlayFlat, ui::ButtonStyle::Secondary, f.body);
    lg.direct = ui::makeCheckbox(window, L"Start Elite directly (no Frontier launcher)", kIdDirect, false, f.body);
    lg.closeAfter = ui::makeCheckbox(window, L"Close the launcher once Elite has started", kIdCloseAfter, false, f.body);
    lg.steamFrontier = ui::makeCheckbox(window, L"Use Frontier account login", kIdSteamFrontier, false, f.body);
    lg.vrMode = CreateWindowExW(0, L"COMBOBOX", L"",
                                WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                0, 0, 10, 10, window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdVrMode)), nullptr, nullptr);
    SendMessageW(lg.vrMode, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    if (ui::theme().dark) SetWindowTheme(lg.vrMode, L"DarkMode_CFD", nullptr);
    refillVrModeChoices();
    lg.vrRefresh = ui::makeButton(window, L"Refresh", kIdVrRefresh, ui::ButtonStyle::Secondary, f.caption);
    lg.themeToggle = ui::makeButton(window, L"Theme: light (switch to dark)", kIdThemeToggle,
                                    ui::ButtonStyle::ThemeToggle, f.caption);
    syncThemeToggleLabel();
    for (HWND control :
         {lg.update, lg.rc, lg.playVr, lg.playFlat, lg.direct, lg.closeAfter, lg.steamFrontier, lg.vrMode, lg.vrRefresh})
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
        g.survey = surveyTarget(g.survey.game, mirrorDirOfGame(g.survey.game));
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
    const std::wstring settingsBackupDir = joinPath(env.roots.launcherDir(), L"backups");
    auto lastSettingsSnapshot = std::make_shared<SettingsXmlSnapshot>();
    env.detectVrMode = [](const std::wstring& gameDir) {
        return detectVrHeadsetModeFromSettingsXml(defaultEliteSettingsXmlPath(gameDir));
    };
    env.prepareLaunchMode = [settingsBackupDir, lastSettingsSnapshot](
                                bool vr, edvr::VrHeadsetMode preferredVrMode, std::string* error) {
        const std::wstring gameDir = g.haveSurvey ? g.survey.game.dir : std::wstring();
        return prepareSettingsXmlForLaunch(defaultEliteSettingsXmlPath(gameDir), settingsBackupDir, vr,
                                             preferredVrMode, error, L"EliteDangerous64.exe",
                                             lastSettingsSnapshot.get());
    };
    env.restoreLaunchMode = [lastSettingsSnapshot]() {
        restoreSettingsXmlFromSnapshot(*lastSettingsSnapshot);
        *lastSettingsSnapshot = SettingsXmlSnapshot();
    };
    lg.session.reset(new LauncherSession(env));
    lg.session->start();
    if (lg.session->includeReleaseCandidates()) ui::toggleCheckbox(lg.rc);
    if (g.window) SetTimer(g.window, kTimerVrProbe, kVrProbeIntervalMs, nullptr);
    launcherRefresh();
}

void launcherAfterScan() {
    if (lg.session && g.haveSurvey) {
        if (lg.session->seedVrStereoModeFromSettings(g.survey.game.dir) != edvr::VrHeadsetMode::NotSet) {
            updateLauncherControls();
        }
    }
    startVrHeadsetProbe(false);
    const std::string& note = launcherStartupNote();
    if (!note.empty() && g.haveSurvey) setReport(note);
    // Open on Play when ED-UP is already installed (the everyday workflow); stay on Setup
    // for first-run installs or explicit --autorun actions so Updates, Install and Log are up front.
    if (!g.args.autorun && g.args.action == AppArgs::Act::None && launcherFolderHasEdup()) {
        showScreen(Screen::Play);
    }
    launcherCheckEdhmInterference();
}

void launcherShutdown() {
    // A worker stuck connecting can hold the join for up to a network timeout, so the window goes first.
    if (g.window) {
        KillTimer(g.window, kTimerVrProbe);
        ShowWindow(g.window, SW_HIDE);
    }
    lg.vrShuttingDown.store(true);
    if (lg.vrProbeThread.joinable()) {
        // Bound shutdown wait so a stalled third-party OpenXR runtime cannot wedge exit.
        const bool finished = !lg.vrProbeDoneEvent ||
                              WaitForSingleObject(lg.vrProbeDoneEvent, 300) == WAIT_OBJECT_0;
        if (finished) lg.vrProbeThread.join();
        else lg.vrProbeThread.detach();
    }
    if (lg.vrProbeDoneEvent) {
        CloseHandle(lg.vrProbeDoneEvent);
        lg.vrProbeDoneEvent = nullptr;
    }
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
    if (id == kIdThemeToggle && code == BN_CLICKED) {
        const std::string next = ui::theme().dark ? "light" : "dark";
        if (lg.session) {
            lg.session->setTheme(next);
        } else {
            LauncherState state;
            const std::wstring path = joinPath(launcherRootsFor(g.args).launcherDir(), L"launcher.ini");
            state.load(path, nullptr);
            state.setTheme(next);
            state.save(nullptr);
        }
        applyLauncherTheme(next);
        return true;
    }
    if (!lg.session) return false;
    if (id == kIdVrMode && code == CBN_SELCHANGE) {
        const LRESULT index = SendMessageW(lg.vrMode, CB_GETCURSEL, 0, 0);
        if (index != CB_ERR) {
            const int token = static_cast<int>(SendMessageW(lg.vrMode, CB_GETITEMDATA, static_cast<WPARAM>(index), 0));
            lg.session->setVrStereoMode(edvr::vrHeadsetModeFromInt(token));
        }
        return true;
    }
    if (code != BN_CLICKED) return false;
    switch (id) {
        case kIdUpdate:
            lg.session->beginUpdate();
            return true;
        case kIdVrRefresh:
            startVrHeadsetProbe(true);
            return true;
        case kIdCloseAfter:
            ui::toggleCheckbox(lg.closeAfter);
            lg.session->setCloseAfterStart(ui::checkboxChecked(lg.closeAfter));
            return true;
        case kIdSteamFrontier: {
            ui::toggleCheckbox(lg.steamFrontier);
            const bool isSteam = g.haveSurvey && _wcsicmp(g.survey.game.source.c_str(), L"Steam") == 0;
            bool hasFrontierInstall = false;
            for (const GameInstall& inst : g.installs) {
                if (_wcsicmp(inst.source.c_str(), L"Frontier launcher") == 0) {
                    hasFrontierInstall = true;
                    break;
                }
            }
            lg.session->setSteamFrontierLogin(ui::checkboxChecked(lg.steamFrontier), isSteam && !hasFrontierInstall);
            updateLauncherControls();
            InvalidateRect(g.window, nullptr, FALSE);
            return true;
        }
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
        case kIdPlayVr:
        case kIdPlayFlat: {
            if (!g.haveSurvey) return true;
            if (launcherCheckEdhmInterference(true) || launcherBusy()) {
                // Antivirus recovery (synchronous or elevated) ran or is still running; do not launch Elite until it finishes.
                return true;
            }
            const bool vr = id != kIdPlayFlat;
            if (vr && !isElevatedForPlay() && lg.vrMode && IsWindowVisible(lg.vrMode)) {
                // Seed from an existing 3..6 value in Settings.xml before treating the user as
                // unset; NotSet is reserved for a new VR user who has never chosen one yet.
                lg.session->seedVrStereoModeFromSettings(g.survey.game.dir);
                if (lg.session->vrStereoMode() == edvr::VrHeadsetMode::NotSet) {
                    MessageBoxW(g.window, tr(L"Choose your preferred VR 3D mode before launching in VR."), kAppTitle,
                                MB_OK | MB_ICONINFORMATION);
                    SetFocus(lg.vrMode);
                    SendMessageW(lg.vrMode, CB_SHOWDROPDOWN, TRUE, 0);
                    return true;
                }
            }
            if (vr) {
                // Always evaluate a fresh report bound to the currently selected install:
                // reuse the background report only when it matches this folder and is recent;
                // otherwise run a fast static-only probe (<10 ms, never blocks on a live DLL).
                const ULONGLONG now = GetTickCount64();
                const bool cacheFresh =
                    lg.vrHaveReport && !lg.vrProbing &&
                    _wcsicmp(lg.vrReportGameDir.c_str(), g.survey.game.dir.c_str()) == 0 &&
                    (now - lg.vrLastProbeMs <= 5000);
                VrHeadsetReport effectiveReport;
                if (cacheFresh) {
                    effectiveReport = lg.vrReport;
                } else {
                    VrProbeOptions opts;
                    opts.gameDir = g.survey.game.dir;
                    opts.allowLiveProbe = false;
                    effectiveReport = probeVrHeadset(opts);
                }
                const VrPreflightWarning warn = evaluateVrLaunchPreflight(effectiveReport);
                if (warn != VrPreflightWarning::None) {
                    const std::wstring rtName =
                        effectiveReport.runtimeDisplayName.empty() ? std::wstring(L"OpenXR")
                                                                   : effectiveReport.runtimeDisplayName;
                    std::wstring prompt;
                    switch (warn) {
                        case VrPreflightWarning::NoRuntime:
                            prompt = tr(L"No active OpenXR runtime was found. Elite may fail to start in VR.\n\nLaunch in VR anyway?");
                            break;
                        case VrPreflightWarning::MissingRuntimeFile:
                            prompt = tr(L"Your active OpenXR runtime file is missing. Open SteamVR, Meta Horizon Link, Virtual Desktop, or Pimax Play to restore it.\n\nLaunch in VR anyway?");
                            break;
                        case VrPreflightWarning::RuntimeMismatch:
                            prompt = fillVrPlaceholders(
                                tr(L"%s is running while %s is set as your active OpenXR runtime. Elite may connect to the wrong runtime or fail to start in VR.\n\nLaunch in VR anyway?"),
                                effectiveReport.mismatchRuntimeName, rtName);
                            break;
                        case VrPreflightWarning::OpenComposite:
                            prompt = tr(L"OpenComposite is set as your active runtime. ED-UP already includes its own OpenXR bridge, and stacking both can break VR.\n\nLaunch in VR anyway?");
                            break;
                        case VrPreflightWarning::RuntimeError:
                            prompt = fillVrPlaceholders(
                                tr(L"Your active OpenXR runtime (%s) reported an error and may not start cleanly.\n\nLaunch in VR anyway?"),
                                rtName);
                            break;
                        case VrPreflightWarning::None:
                        default:
                            break;
                    }
                    if (!prompt.empty() &&
                        MessageBoxW(g.window, prompt.c_str(), kAppTitle,
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                        return true;
                    }
                }
            }
            invalidateLauncherCache();
            lg.session->beginPlay(g.survey.game.source, g.survey.game.dir, vr, isElevatedForPlay(), realRouteOps(), WatchNames(),
                                  WatchTiming());
            g.mode = eliteGameMode();
            rebuildStatus();
            InvalidateRect(g.window, nullptr, FALSE);
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
            L"This folder was last changed by a branch build (install_edup.py), not by this launcher. Going on "
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
    file.edhm2dMigoto = args.edhm2dMigoto;
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
        g.survey = surveyTarget(g.survey.game, mirrorDirOfGame(g.survey.game));
        loadSettings(g.settingsEdition);
        showSurvey();
    });
}

const DllInfo* launcherEdhmDllOnDisk() {
    if (!g.haveSurvey) return nullptr;
    for (const DllInfo& d : g.survey.otherD3d11) {
        if (_wcsicmp(leafOf(d.path).c_str(), L"d3d11_edhm.dll") == 0) return &d;
    }
    return nullptr;
}

bool launcherEdhm2dMigotoActiveOnDisk() {
    if (!g.haveSurvey) return false;
    if (!g.survey.state.chainTarget.empty() &&
        _wcsicmp(g.survey.state.chainTarget.c_str(), L"d3d11_edhm.dll") != 0)
        return false;
    const FileRecord* rec = g.survey.state.file("edhm_2dmigoto");
    if (!rec || rec->existing != existingName(ExistingMode::OptIn) || rec->sha256.empty()) return false;
    const DllInfo* disk = launcherEdhmDllOnDisk();
    return disk && disk->kind != DllKind::Absent && disk->kind != DllKind::Unreadable &&
           _stricmp(disk->sha256.c_str(), rec->sha256.c_str()) == 0;
}

bool launcherEdhmPresentOnDisk() {
    if (!g.haveSurvey) return false;
    const DllInfo* disk = launcherEdhmDllOnDisk();
    if (disk && disk->kind != DllKind::Absent) return true;
    if (g.survey.edhmConfigPresent) return true;
    if (_wcsicmp(g.survey.state.chainTarget.c_str(), L"d3d11_edhm.dll") == 0) return true;
    if (g.survey.d3d11.kind != DllKind::Absent && g.survey.d3d11.kind != DllKind::Edvr &&
        modNameOf(g.survey.d3d11) == L"EDHM")
        return true;
    return false;
}

void launcherApplyEdhmPlan(bool wantOn, bool isFallbackRestore);

bool launcherDismissMissingEdhm() {
    if (!lg.session || !g.haveSurvey || launcherEdhmBusy()) return false;
    const std::wstring mirrorDir = mirrorDirOfGame(g.survey.game);
    g.survey = surveyTarget(g.survey.game, mirrorDir);
    Options options;
    options.nowUtc = utcNow();
    options.settingsDir = mirrorDir;
    const Plan plan = planEdhmDismiss(g.survey, options);
    if (plan.blocked || plan.nothingToDo) {
        showSurvey();
        return false;
    }
    if (launcherNeedsElevatedCopy()) {
        const wchar_t* prompt =
            L"This game folder can only be written to by an administrator. Windows will ask for permission "
            L"to clear the saved EDHM recovery record so later updates do not put d3d11_edhm.dll back.\n\nContinue?";
        if (MessageBoxW(g.window, prompt, kAppTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            showSurvey();
            return false;
        }
        PlanFile file;
        file.action = "edhm_dismiss";
        file.dir = g.survey.game.dir;
        file.source = g.survey.game.source;
        setReport("Clearing EDHM recovery state...\r\n----\r\nWaiting for Windows' permission prompt...\r\n");
        lg.session->beginElevatedApply(file, runWithUac, [](const ElevatedResult& result) {
            if (result.declined) {
                setReport("The permission prompt was declined, so the EDHM recovery record was not changed.\r\n");
            } else if (!result.error.empty() && result.outcome.lines.empty()) {
                setReport(result.error + "\r\n");
            } else {
                setReport(outcomeReport(result.outcome));
            }
            g.survey = surveyTarget(g.survey.game, mirrorDirOfGame(g.survey.game));
            showSurvey();
        });
        return true;
    }
    applyPlan(plan, PayloadProvider{});
    g.survey = surveyTarget(g.survey.game, mirrorDir);
    showSurvey();
    return false;
}

bool launcherTriggerEdhmInterferenceFallback(bool askFirst = false) {
    if (!lg.session || lg.edhmRecovering || launcherEdhmBusy()) return false;
    if (askFirst) {
        if (g.haveSurvey && _wcsicmp(lg.edhmInterferenceAskedDir.c_str(), g.survey.game.dir.c_str()) == 0) {
            return false;
        }
        if (g.haveSurvey) lg.edhmInterferenceAskedDir = g.survey.game.dir;
        const int answer = MessageBoxW(
            g.window,
            L"d3d11_edhm.dll is missing from your game folder.\n\n"
            L"If antivirus removed the tailored 2DMigoto DLL, choose Yes to restore your backed-up "
            L"standard d3d11_edhm.dll and d3dx.ini settings.\n\n"
            L"If you uninstalled EDHM yourself, choose No to turn the option off without putting "
            L"d3d11_edhm.dll back.\n\n"
            L"Restore your backed-up EDHM files?",
            kAppTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (answer != IDYES) {
            lg.session->setEdhm2dMigoto(false, nullptr);
            lg.session->setEdhmWarning(std::string());
            return launcherDismissMissingEdhm();
        }
    }
    lg.edhmRecovering = true;
    lg.session->setEdhm2dMigoto(false, nullptr);
    lg.session->setEdhmWarning(kEdhmInterferenceWarning);
    launcherApplyEdhmPlan(false, true);
    return true;
}

void launcherVerifyEdhmAfterEnable() {
    if (!g.haveSurvey || !lg.session) return;
    g.survey = surveyTarget(g.survey.game, mirrorDirOfGame(g.survey.game));
    const FileRecord* rec = g.survey.state.file("edhm_2dmigoto");
    if (!rec || rec->existing != existingName(ExistingMode::OptIn)) return;
    const DllInfo* disk = launcherEdhmDllOnDisk();
    const bool ok = disk && disk->kind != DllKind::Absent && disk->kind != DllKind::Unreadable &&
                    _stricmp(disk->sha256.c_str(), rec->sha256.c_str()) == 0;
    if (!ok) launcherTriggerEdhmInterferenceFallback(false);
}

bool launcherCheckEdhmInterference(bool refreshDisk) {
    if (!lg.session || !lg.session->edhm2dMigoto() || !g.haveSurvey || g.survey.gameRunningHere ||
        launcherEdhmBusy() || lg.edhmRecovering)
        return false;
    const FileRecord* rec = g.survey.state.file("edhm_2dmigoto");
    if (!rec || rec->existing != existingName(ExistingMode::OptIn)) return false;
    if (refreshDisk) {
        const DllInfo fresh = probeDll(joinPath(g.survey.game.dir, L"d3d11_edhm.dll"));
        if (fresh.kind == DllKind::Absent) {
            return launcherTriggerEdhmInterferenceFallback(true);
        }
        return false;
    }
    const DllInfo* disk = launcherEdhmDllOnDisk();
    if (!disk || disk->kind == DllKind::Absent) {
        return launcherTriggerEdhmInterferenceFallback(true);
    }
    return false;
}

void launcherApplyEdhmPlan(bool wantOn, bool isFallbackRestore) {
    if (!lg.session || !g.haveSurvey || launcherEdhmBusy()) {
        lg.edhmRecovering = false;
        launcherUpdateSettingsToggle();
        return;
    }
    const std::wstring mirrorDir = mirrorDirOfGame(g.survey.game);
    g.survey = surveyTarget(g.survey.game, mirrorDir);

    if (wantOn) {
        // Only apply EDHM 2DMigoto immediately when ED-UP is already installed in this folder;
        // otherwise keep the saved preference as "Pending install" until normal Install runs.
        if (!launcherFolderHasEdup()) {
            lg.edhmRecovering = false;
            launcherUpdateSettingsToggle();
            return;
        }
        if (!launcherPayloadReady()) {
            lg.edhmRecovering = false;
            launcherUpdateSettingsToggle();
            return;
        }
        const PayloadFileEntry* entry = payloadInfo().find("edhm_2dmigoto");
        if (!entry) {
            lg.edhmRecovering = false;
            launcherUpdateSettingsToggle();
            return;
        }
        // Check that antivirus has not removed the cached payload file before placement.
        if (lg.session->payload().kind == PayloadKind::Manifest) {
            const PayloadCache cache(launcherRootsFor(g.args).cacheDir());
            const std::wstring cached = cache.pathFor(entry->sha);
            if (!fileExists(cached) || _stricmp(sha256File(cached).c_str(), entry->sha.c_str()) != 0) {
                launcherTriggerEdhmInterferenceFallback();
                return;
            }
        }
    }

    Options options;
    options.edhm2dMigoto = wantOn;
    options.backupStamp = timestampName();
    options.nowUtc = utcNow();
    options.settingsDir = mirrorDir;
    const Plan plan = wantOn ? planEdhmToggle(g.survey, options, payloadInfo())
                             : planEdhmRestore(g.survey, options);
    if (plan.blocked) {
        lg.edhmRecovering = false;
        if (wantOn) lg.session->setEdhm2dMigoto(false, nullptr);
        setReport(planReport(plan) + "\r\n----\r\n" + planSummary(plan) + "\r\n");
        if (!isFallbackRestore && !plan.problems.empty()) {
            std::wstring text;
            for (const std::string& p : plan.problems) text += fromUtf8(p) + L"\n\n";
            MessageBoxW(g.window, text.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
        }
        showSurvey();
        return;
    }
    if (plan.nothingToDo) {
        lg.edhmRecovering = false;
        showSurvey();
        return;
    }

    if (launcherNeedsElevatedCopy()) {
        const PayloadLoad& payload = lg.session->payload();
        if (wantOn && payload.kind != PayloadKind::Manifest) {
            lg.edhmRecovering = false;
            lg.session->setEdhm2dMigoto(false, nullptr);
            if (!isFallbackRestore) {
                MessageBoxW(g.window,
                            L"This game folder needs administrator rights, and the elevated helper installs only from a signed release download.",
                            kAppTitle, MB_OK | MB_ICONWARNING);
            }
            showSurvey();
            return;
        }
        const wchar_t* prompt =
            isFallbackRestore
                ? L"d3d11_edhm.dll went missing (possibly removed by antivirus). Windows will ask for administrator permission to restore your backed-up standard d3d11_edhm.dll and d3dx.ini settings.\n\nContinue?"
                : L"This game folder can only be written to by an administrator (it is usually under Program Files).\n\nWindows will ask for permission, and a short-lived copy of the launcher with administrator rights will update your EDHM compatibility files.\n\nContinue?";
        if (MessageBoxW(g.window, prompt, kAppTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            lg.edhmRecovering = false;
            if (wantOn) lg.session->setEdhm2dMigoto(false, nullptr);
            showSurvey();
            return;
        }
        PlanFile file;
        file.action = wantOn ? "edhm_apply" : "edhm_restore";
        file.dir = g.survey.game.dir;
        file.source = g.survey.game.source;
        file.edhm2dMigoto = wantOn;
        if (wantOn) file.manifestSha = manifestShaOf(payload.manifestPath);
        const std::string shown = planReport(plan);
        setReport(shown + "\r\n----\r\nWaiting for Windows' permission prompt...\r\n");
        lg.session->beginElevatedApply(file, runWithUac, [shown, wantOn](const ElevatedResult& result) {
            lg.edhmRecovering = false;
            if (result.declined) {
                setReport(shown + "\r\n----\r\nThe permission prompt was declined, so nothing was changed.\r\n");
                if (wantOn && lg.session) lg.session->setEdhm2dMigoto(false, nullptr);
            } else if (!result.error.empty() && result.outcome.lines.empty()) {
                setReport(shown + "\r\n----\r\n" + result.error + "\r\n");
                if (wantOn && lg.session) lg.session->setEdhm2dMigoto(false, nullptr);
            } else {
                setReport(shown + "\r\n----\r\n" + outcomeReport(result.outcome));
                const bool applied = result.outcome.status == ApplyOutcome::Status::Done ||
                                     result.outcome.status == ApplyOutcome::Status::NothingToDo;
                if (!applied && wantOn && lg.session) {
                    lg.session->setEdhm2dMigoto(false, nullptr);
                } else if (applied && wantOn) {
                    launcherVerifyEdhmAfterEnable();
                }
            }
            g.survey = surveyTarget(g.survey.game, mirrorDirOfGame(g.survey.game));
            loadSettings(g.settingsEdition);
            showSurvey();
        });
        return;
    }

    const ApplyResult result = applyPlan(plan, wantOn ? PayloadProvider{payloadItem} : PayloadProvider{});
    lg.edhmRecovering = false;
    if (!result.ok) {
        setReport(planReport(plan) + "\r\n----\r\n" + result.error + "\r\n");
        if (wantOn) {
            lg.session->setEdhm2dMigoto(false, nullptr);
            lg.session->setEdhmWarning(kEdhmInterferenceWarning);
        }
        g.survey = surveyTarget(g.survey.game, mirrorDir);
        showSurvey();
        return;
    }
    setReport(planReport(plan) + "\r\n----\r\nDone.\r\n");
    if (!plan.problems.empty() && !isFallbackRestore) {
        std::wstring text;
        for (const std::string& p : plan.problems) text += fromUtf8(p) + L"\n\n";
        MessageBoxW(g.window, text.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
    }
    if (wantOn) {
        launcherVerifyEdhmAfterEnable();
    }
    g.survey = surveyTarget(g.survey.game, mirrorDir);
    loadSettings(g.settingsEdition);
    showSurvey();
}

void launcherSetEdhmLoggingFlag(std::string key, bool wantHide) {
    if (!lg.session || !g.haveSurvey || launcherEdhmBusy()) {
        launcherUpdateSettingsToggle();
        return;
    }
    const std::wstring mirrorDir = mirrorDirOfGame(g.survey.game);
    g.survey = surveyTarget(g.survey.game, mirrorDir);
    Options options;
    options.nowUtc = utcNow();
    options.settingsDir = mirrorDir;
    const Plan plan = planEdhmLogging(g.survey, options, key, wantHide);
    if (plan.blocked) {
        if (!plan.problems.empty()) {
            std::wstring text;
            for (const std::string& p : plan.problems) text += fromUtf8(p) + L"\n\n";
            MessageBoxW(g.window, text.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
        }
        launcherUpdateSettingsToggle();
        return;
    }
    if (plan.nothingToDo) {
        launcherUpdateSettingsToggle();
        return;
    }

    if (launcherNeedsElevatedCopy()) {
        const wchar_t* prompt =
            L"This game folder can only be written to by an administrator (it is usually under Program Files).\n\n"
            L"Windows will ask for permission, and a short-lived copy of the launcher with administrator rights will update d3dx.ini.\n\n"
            L"Continue?";
        if (MessageBoxW(g.window, prompt, kAppTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            launcherUpdateSettingsToggle();
            return;
        }
        PlanFile file;
        file.action = "edhm_logging";
        file.dir = g.survey.game.dir;
        file.source = g.survey.game.source;
        file.d3dxLoggingKey = key;
        file.d3dxHideFlag = wantHide;
        setReport("Updating d3dx.ini...\r\n----\r\nWaiting for Windows' permission prompt...\r\n");
        lg.session->beginElevatedApply(file, runWithUac, [](const ElevatedResult& result) {
            if (result.declined) {
                setReport("The permission prompt was declined, so d3dx.ini was not changed.\r\n");
            } else if (!result.error.empty() && result.outcome.lines.empty()) {
                setReport(result.error + "\r\n");
            } else {
                setReport(outcomeReport(result.outcome));
            }
            g.survey = surveyTarget(g.survey.game, mirrorDirOfGame(g.survey.game));
            showSurvey();
        });
        return;
    }

    const ApplyResult result = applyPlan(plan, PayloadProvider{});
    if (!result.ok) {
        std::wstring msg = L"Could not update d3dx.ini.";
        if (!result.error.empty()) msg += L"\n\n" + fromUtf8(result.error);
        MessageBoxW(g.window, msg.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
    }
    g.survey = surveyTarget(g.survey.game, mirrorDir);
    launcherUpdateSettingsToggle();
}

void launcherUpdateSettingsToggle() {
    if (!g.settingsList) return;
    if (!lg.session) {
        settingsListSetLauncherToggles(g.settingsList, {});
        return;
    }
    std::vector<LauncherToggleRow> rows;
    const std::wstring edhmGroup = tr(L"EDHM compatibility");
    const bool busy = launcherEdhmBusy();

    LauncherToggleRow migotoRow;
    migotoRow.visible = true;
    migotoRow.control = LauncherControlKind::Toggle;
    migotoRow.enabled = true;
    migotoRow.group = edhmGroup;
    migotoRow.label = tr(L"(Experimental) Use tailored 2DMigoto dll for EDHM compatibility improvements.");
    migotoRow.description =
        tr(L"Backs up your standard d3d11_edhm.dll and d3dx.ini settings, installs the signed 2DMigoto build, and applies ED-UP compatibility settings. Turning off restores them.");
    migotoRow.searchTerms = L"edhm 2dmigoto 3dmigoto d3d11_edhm d3dx.ini experimental hud compatibility allow_platform_update upscaling";
    migotoRow.on = lg.session->edhm2dMigoto();
    migotoRow.busy = busy;
    migotoRow.warning = trLine(lg.session->edhmWarning());

    const bool active2d = launcherEdhm2dMigotoActiveOnDisk();
    const bool edhmPresent = launcherEdhmPresentOnDisk();
    const DllInfo* diskDll = launcherEdhmDllOnDisk();
    const bool hasEdhmDll = diskDll && diskDll->kind != DllKind::Absent;
    const bool hasPendingDllRestore = active2d || (hasEdhmDll && g.survey.state.file("edhm_2dmigoto") != nullptr);
    const bool hasPendingIniRestore = g.survey.state.edhmD3dxBackup.anySaved();
    const bool hasPendingRestore = g.haveSurvey && (hasPendingDllRestore || hasPendingIniRestore);
    const bool externalEdhmKept =
        hasEdhmDll && !active2d && !g.survey.state.edhmExternalSha.empty();
    const bool payloadReady = launcherPayloadReady();
    const bool hasPayloadAsset = payloadReady && payloadInfo().find("edhm_2dmigoto") != nullptr;
    const bool hasOptInAsset = lg.session->payload().hasOptInEdhmAsset;

    if (!g.haveSurvey) {
        migotoRow.statusText = tr(L"No install selected");
        migotoRow.statusTone = LauncherRowTone::Muted;
    } else if (migotoRow.busy) {
        migotoRow.statusText = tr(L"Applying...");
        migotoRow.statusTone = LauncherRowTone::Accent;
    } else if (active2d && migotoRow.on) {
        migotoRow.statusText = tr(L"2DMigoto active");
        migotoRow.statusTone = LauncherRowTone::Good;
    } else if (hasPendingRestore && !migotoRow.on) {
        migotoRow.statusText = hasPendingDllRestore ? tr(L"2DMigoto still on disk") : tr(L"d3dx.ini restore pending");
        migotoRow.statusTone = LauncherRowTone::Warn;
        migotoRow.actionLink = tr(L"Restore standard");
    } else if (migotoRow.on && externalEdhmKept) {
        migotoRow.statusText = tr(L"EDHM updated externally");
        migotoRow.statusTone = LauncherRowTone::Muted;
        if (hasPayloadAsset && launcherFolderHasEdup()) migotoRow.actionLink = tr(L"Apply now");
    } else if (migotoRow.on && !payloadReady) {
        migotoRow.statusText = tr(L"Download a release first");
        migotoRow.statusTone = LauncherRowTone::Warn;
    } else if (migotoRow.on && !hasPayloadAsset && hasOptInAsset) {
        if (lg.session->checkState() == CheckState::Checking) {
            migotoRow.statusText = tr(L"Checking...");
            migotoRow.statusTone = LauncherRowTone::Accent;
        } else {
            migotoRow.statusText = tr(L"Download required");
            migotoRow.statusTone = LauncherRowTone::Warn;
            if (lg.session->checkState() == CheckState::Available) migotoRow.actionLink = tr(L"Download now");
        }
    } else if (migotoRow.on && !hasPayloadAsset) {
        migotoRow.statusText = tr(L"Not in this release");
        migotoRow.statusTone = LauncherRowTone::Warn;
    } else if (migotoRow.on && !edhmPresent) {
        migotoRow.statusText = tr(L"EDHM not detected");
        migotoRow.statusTone = LauncherRowTone::Muted;
    } else if (migotoRow.on) {
        migotoRow.statusText = tr(L"Pending install");
        migotoRow.statusTone = LauncherRowTone::Accent;
        if (launcherFolderHasEdup()) migotoRow.actionLink = tr(L"Apply now");
    } else if (hasEdhmDll) {
        migotoRow.statusText = tr(L"Standard EDHM");
        migotoRow.statusTone = LauncherRowTone::Muted;
    } else {
        migotoRow.statusText = tr(L"Off");
        migotoRow.statusTone = LauncherRowTone::Muted;
    }

    migotoRow.onToggle = [](bool want) {
        if (!lg.session || launcherEdhmBusy()) return;
        if (want) {
            const bool accepted = lg.session->setEdhm2dMigoto(true, []() {
                return MessageBoxW(g.window, fromUtf8(kEdhm2dMigotoQuestion).c_str(), kAppTitle,
                                   MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
            });
            if (!accepted) {
                launcherUpdateSettingsToggle();
                return;
            }
            lg.edhmInterferenceAskedDir.clear();
            launcherApplyEdhmPlan(true, false);
        } else {
            lg.session->setEdhmWarning(std::string());
            lg.session->setEdhm2dMigoto(false, nullptr);
            launcherApplyEdhmPlan(false, false);
        }
    };
    migotoRow.onAction = []() {
        if (!lg.session || launcherEdhmBusy()) return;
        if (lg.session->edhm2dMigoto() && launcherPayloadReady() &&
            payloadInfo().find("edhm_2dmigoto") == nullptr &&
            lg.session->payload().hasOptInEdhmAsset) {
            if (lg.session->checkState() == CheckState::Available) {
                lg.session->beginUpdate();
            }
            return;
        }
        launcherApplyEdhmPlan(lg.session->edhm2dMigoto(), false);
    };
    rows.push_back(std::move(migotoRow));

    const bool managed2d = active2d && migotoRow.on;
    const bool d3dxReadable = g.haveSurvey && g.survey.d3dxIniPresent && !g.survey.d3dxIniUnreadable && !g.survey.d3dxIniUtf16;
    const bool d3dxUsable = managed2d && d3dxReadable && !g.survey.d3dxIniReadOnly && !g.survey.gameRunningHere;
    auto buildLoggingRow = [&](std::wstring labelText, std::wstring descText, const wchar_t* search,
                               const char* keyName, bool hiddenNow) {
        LauncherToggleRow r;
        r.visible = true;
        r.control = LauncherControlKind::Checkbox;
        r.enabled = d3dxUsable;
        r.group = edhmGroup;
        r.label = std::move(labelText);
        r.description = std::move(descText);
        r.searchTerms = search;
        r.on = d3dxReadable && hiddenNow;
        r.busy = busy;
        if (!g.haveSurvey) {
            r.statusText = tr(L"No install selected");
            r.statusTone = LauncherRowTone::Muted;
        } else if (!managed2d) {
            r.statusText = tr(L"Requires 2DMigoto");
            r.statusTone = LauncherRowTone::Muted;
        } else if (!g.survey.d3dxIniPresent) {
            r.statusText = tr(L"d3dx.ini not found");
            r.statusTone = LauncherRowTone::Muted;
        } else if (g.survey.d3dxIniUnreadable) {
            r.statusText = tr(L"d3dx.ini unreadable");
            r.statusTone = LauncherRowTone::Warn;
        } else if (g.survey.d3dxIniUtf16) {
            r.statusText = tr(L"d3dx.ini unsupported encoding");
            r.statusTone = LauncherRowTone::Warn;
        } else if (g.survey.d3dxIniReadOnly) {
            r.statusText = tr(L"d3dx.ini read-only");
            r.statusTone = LauncherRowTone::Warn;
        } else if (g.survey.gameRunningHere) {
            r.statusText = tr(L"Close Elite first");
            r.statusTone = LauncherRowTone::Warn;
        } else if (busy) {
            r.statusText = tr(L"Applying...");
            r.statusTone = LauncherRowTone::Accent;
        } else if (r.on) {
            r.statusText = tr(L"Hidden");
            r.statusTone = LauncherRowTone::Good;
        } else {
            r.statusText = tr(L"Shown");
            r.statusTone = LauncherRowTone::Muted;
        }
        const std::string keyCopy(keyName);
        r.onToggle = [keyCopy](bool wantHide) { launcherSetEdhmLoggingFlag(keyCopy, wantHide); };
        return r;
    };

    rows.push_back(buildLoggingRow(
        tr(L"Hide Refresh/Reload messages from EDHM"),
        tr(L"Sets [Logging] show_reload_message = 0 in d3dx.ini while 2DMigoto is active; unchecking or turning 2DMigoto off restores your original setting."),
        L"edhm d3dx.ini logging show_reload_message refresh reload messages banners hide",
        "show_reload_message",
        d3dxReadable && readEdhmHideReloadMessages(g.survey.d3dxIniText)));

    rows.push_back(buildLoggingRow(
        tr(L"Hide Warnings from EDHM"),
        tr(L"Sets [Logging] show_warnings = 0 in d3dx.ini while 2DMigoto is active; unchecking or turning 2DMigoto off restores your original setting."),
        L"edhm d3dx.ini logging show_warnings warnings banners hide",
        "show_warnings",
        d3dxReadable && readEdhmHideWarnings(g.survey.d3dxIniText)));

    settingsListSetLauncherToggles(g.settingsList, std::move(rows));
}
