#include "app/app.h"

#include <psapi.h>
#include <windows.h>
#include <windowsx.h>

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include "app/i18n.h"
#include "app/studio_ui.h"
#include "core/log.h"
#include "core/studio_engine.h"
#include "platform/paths.h"

namespace wasd::app {
using platform::narrow;
using platform::widen;
namespace fs = std::filesystem;

App::App() {
    // The visual-graph runner shares the exact same device sinks as MacroRunner.
    studio::GraphHost host;
    host.touch = [this](int a, int f, Vec2 n) { macroTouch(a, f, n); };
    host.key = [this](int vk, bool d) {
        std::lock_guard lk(inputMu_);
        input_.onKey(vk, d, nowMicros(), InputSource::Macro);
    };
    host.zonePresent = [this](int z) { return zonePresent(z); };
    host.imagePresent = [this](int i) { return imagePresent(i); };
    host.imageScan = [this](int i, const ImageQuery& q) { return imageScanQuery(i, q); };
    host.runMacro = [this](int i) { macros_.start(profile_.automation, i, nowMicros()); };
    host.pixelColor = [this](Vec2 pt) { return graphPixelColor(pt); };
    host.colorScan = [this](const Rect& r, const studio::ColorVal& t) { return graphColorScan(r, t); };
    host.objectScan = [this](const Rect& r, const std::vector<std::string>& classes, float conf) {
        return graphObjectScan(r, classes, conf);
    };
    host.log = nullptr;  // the studio console reads GraphRunner::events() directly
    graphRunner_ = std::make_unique<studio::GraphRunner>(std::move(host));
}
App::~App() = default;

static bool ImGuiWantsKeyboard() { return ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput; }
static bool ImGuiWantsMouse() { return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse; }

static std::string dataPath(const std::string& name) { return narrow(platform::appDataDir()) + "/" + name; }
static std::string profilePath(const std::string& name) { return narrow(platform::profilesDir()) + "/" + name + ".json"; }

bool App::init(HWND hwnd, std::string& err) {
    hwnd_ = hwnd;
    log::setFile(narrow(platform::logsDir()) + "/wasd.log");
    if (!renderer_.init(hwnd, err)) return false;
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    loadSettings();
    refreshProfiles();
    loadProfile(settings_.lastProfile);
    adb_ = std::make_unique<platform::Adb>(platform::runtimeDir() + L"/adb.exe");
    if (!adb_->available()) {
        trackerError_ = "adb.exe not found in runtime folder (run tools/fetch-runtime.ps1)";
        log::error(trackerError_);
        return true;
    }
    std::string e;
    if (!adb_->startServer(e)) trackerError_ = e;
    tracker_ = std::make_unique<platform::DeviceTracker>(*adb_, [this](const std::vector<platform::AdbDevice>& d) {
        std::lock_guard lk(devMu_);
        devices_ = d;
    });
    tracker_->start();
    startInputTick();
    startScanWorker();
    return true;
}

void App::shutdown() {
    macros_.cancel("closing");
    automationOn_ = false;
    stopInputTick();
    stopScanWorker();
    disconnect();
    if (tracker_) tracker_->stop();
    saveSettings();
    if (frameEvent_) CloseHandle(frameEvent_), frameEvent_ = nullptr;
    renderer_.shutdown();
}

// Dedicated input thread (ADR-006): owns a message-only window registered with
// RIDEV_INPUTSINK so raw mouse input arrives on THIS thread's message queue,
// independent of the UI/render thread. Each loop iteration drains WM_INPUT and
// runs InputEngine::tick() — a steady ~1 ms clock for recoil, smoothing,
// auto-fire, and camera auto-release. TIME_CRITICAL keeps it ahead of
// the UI thread under load.
void App::startInputTick() {
    if (inputTickThread_.joinable()) return;
    inputTickRunning_ = true;
    inputHwndReady_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    inputTickThread_ = std::thread([this] { inputThreadLoop(); });
}

void App::stopInputTick() {
    inputTickRunning_ = false;
    // Wake the thread out of MsgWaitForMultipleObjectsEx so it can exit.
    if (inputHwndReady_) {
        WaitForSingleObject(inputHwndReady_, 2000);
        if (inputHwnd_) PostMessageW(inputHwnd_, WM_NULL, 0, 0);
    }
    if (inputTickThread_.joinable()) inputTickThread_.join();
    if (inputHwndReady_) CloseHandle(inputHwndReady_), inputHwndReady_ = nullptr;
}

LRESULT CALLBACK App::inputWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && msg == WM_INPUT) {
        self->onInputRawInput(reinterpret_cast<HRAWINPUT>(lp));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void App::onInputRawInput(HRAWINPUT hraw) {
    if (!locked_) return;
    // Magnet mode: the algorithm owns the camera while an aim lock is alive —
    // drain the queue but drop hand deltas so they can't fight the pull.
    const bool swallow = aimSwallowMouse_.load(std::memory_order_relaxed);
    inputT0Us_ = nowMicros();
    // Drain the whole kernel raw-input queue in one syscall; the event that
    // posted this WM_INPUT is included. At 1000+ Hz this batches dozens of
    // deltas per call instead of one GetRawInputData per message.
    static thread_local std::vector<uint8_t> rawBuf(64 * 1024);  // ~2000 events
    UINT bufSize = static_cast<UINT>(rawBuf.size());
    const UINT n = GetRawInputBuffer(reinterpret_cast<PRAWINPUT>(rawBuf.data()), &bufSize, sizeof(RAWINPUTHEADER));
    {
        std::lock_guard lk(inputMu_);
        RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(rawBuf.data());
        for (UINT i = 0; i < n; ++i) {
            if (ri->header.dwType == RIM_TYPEMOUSE && !(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                const int dx = ri->data.mouse.lLastX, dy = ri->data.mouse.lLastY;
                if ((dx || dy) && !swallow) input_.onMouseDelta(dx, dy, inputT0Us_);
            }
            ri = reinterpret_cast<RAWINPUT*>(reinterpret_cast<BYTE*>(ri) +
                                             ((static_cast<SIZE_T>(ri->header.dwSize) + 7u) & ~static_cast<SIZE_T>(7u)));
        }
        if (n == 0) {
            // The posting event was already consumed; read it by handle so no
            // delta is ever dropped.
            RAWINPUT ri2;
            UINT size2 = sizeof ri2;
            if (GetRawInputData(hraw, RID_INPUT, &ri2, &size2, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                ri2.header.dwType == RIM_TYPEMOUSE && !(ri2.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                const int dx = ri2.data.mouse.lLastX, dy = ri2.data.mouse.lLastY;
                if ((dx || dy) && !swallow) input_.onMouseDelta(dx, dy, inputT0Us_);
            }
        }
    }
    inputT0Us_ = 0;
}

void App::inputThreadLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &App::inputWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WasdInputWindow";
    RegisterClassExW(&wc);
    inputHwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (inputHwnd_) {
        SetWindowLongPtrW(inputHwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_INPUTSINK, inputHwnd_};  // generic desktop / mouse
        RegisterRawInputDevices(&rid, 1, sizeof rid);
    }
    if (inputHwndReady_) SetEvent(inputHwndReady_);
    while (inputTickRunning_) {
        // 1 ms cadence: wakes on queued input or the timeout, whichever first.
        MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_INPUT, 0);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const int64_t now = nowMicros();
        { std::lock_guard lk(inputMu_); input_.tick(now); }
    }
    if (inputHwnd_) {
        RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&rid, 1, sizeof rid);
        DestroyWindow(inputHwnd_);
        inputHwnd_ = nullptr;
    }
}

void App::loadSettings() {
    std::ifstream f(fs::u8path(dataPath("settings.json")));
    if (!f) return;
    const auto j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    settings_.preset = std::clamp(j.value("preset", 0), 0, static_cast<int>(std::size(kPresets)) - 1);
    settings_.vsync = j.value("vsync", false);
    settings_.hardwareDecode = j.value("hardwareDecode", true);
    settings_.h265 = j.value("h265", false);
    settings_.autoReconnect = j.value("autoReconnect", true);
    settings_.showTouches = j.value("showTouches", false);
    settings_.lastProfile = j.value("lastProfile", std::string("Default"));
    settings_.language = j.value("language", std::string("en"));
    settings_.panelCollapsed = true;  // always launch with rail only; user expands on demand
    settings_.darkTheme = j.value("darkTheme", true);
    settings_.panelWidthPx = std::clamp(j.value("panelWidthPx", 440), 340, 680);
    const auto& pr = kPresets[settings_.preset];  // older settings files only stored the preset
    settings_.maxSize = std::min<uint32_t>(j.value("maxSize", pr.maxSize), 4096u);
    settings_.bitRate = std::clamp<uint32_t>(j.value("bitRate", pr.bitRate), 1000000u, 60000000u);
    settings_.maxFps = std::min<uint32_t>(j.value("maxFps", pr.maxFps), 240u);
    settings_.displayMode = std::clamp(j.value("displayMode", 0), 0, 1);
    settings_.queueStaleMs = std::clamp(j.value("queueStaleMs", 60), 10, 1000);
    settings_.showFps = j.value("showFps", false);
    settings_.overlayOpacity = std::clamp(j.value("overlayOpacity", 65), 0, 100);
    settings_.overlayLandscapeOnly = j.value("overlayLandscapeOnly", true);
    settings_.touchTrace = j.value("touchTrace", false);
}

void App::saveSettings() {
    nlohmann::json j = {{"preset", settings_.preset},          {"vsync", settings_.vsync},
                        {"hardwareDecode", settings_.hardwareDecode}, {"h265", settings_.h265},
                        {"autoReconnect", settings_.autoReconnect}, {"showTouches", settings_.showTouches},
                        {"lastProfile", profile_.name},             {"language", settings_.language},
                        {"panelCollapsed", settings_.panelCollapsed}, {"darkTheme", settings_.darkTheme},
                        {"panelWidthPx", settings_.panelWidthPx},
                        {"maxSize", settings_.maxSize},
                        {"bitRate", settings_.bitRate},             {"maxFps", settings_.maxFps},
                        {"displayMode", settings_.displayMode},     {"queueStaleMs", settings_.queueStaleMs},
                        {"showFps", settings_.showFps},             {"overlayOpacity", settings_.overlayOpacity},
                        {"overlayLandscapeOnly", settings_.overlayLandscapeOnly}, {"touchTrace", settings_.touchTrace}};
    std::ofstream(fs::u8path(dataPath("settings.json"))) << j.dump(2);
}

void App::refreshProfiles() {
    profileNames_.clear();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(platform::profilesDir(), ec))
        if (e.path().extension() == ".json") profileNames_.push_back(narrow(e.path().stem().wstring()));
    std::sort(profileNames_.begin(), profileNames_.end());
}

void App::loadProfile(const std::string& name) {
    Profile p;
    std::string err;
    if (!loadProfileFile(profilePath(name), p, err)) {
        if (!err.empty() && name != "Default") status_ = i18n::trf("Layout load failed: %s", err.c_str());
        p = makeDefaultProfile();
        p.name = name.empty() ? "Default" : name;
    }
    releaseInput();
    endCapture();
    cancelPick();
    selectedZone_ = selectedMonitor_ = selectedMacro_ = selectedImage_ = -1;
    selectedStepPath_.clear();
    profile_ = p;
    saved_ = p;
    loadRefImageFiles();
    if (const auto problems = validateProfile(profile_); !problems.empty())
        status_ = i18n::trf("Layout load failed: %s", problems.front().c_str());
    engineProfile_ = p;
    history_.reset(p);
    { std::lock_guard lk(inputMu_); input_.setProfile(profile_); }
    selectedBinding_ = -1;
    dragBinding_ = -1;
    dragging_ = false;
}

bool App::saveProfile() {
    if (capTarget_.slot >= 0) cancelCapture();
    std::string err;
    const auto problems = validateProfile(profile_);
    if (!problems.empty()) {
        status_ = i18n::trf("Not saved: %s", problems.front().c_str());
        return false;
    }
    const bool ok = saveProfileFile(profilePath(profile_.name), profile_, err);
    if (ok) {
        saved_ = profile_;
        settings_.lastProfile = profile_.name;
        status_.clear();
        showToast(i18n::trf("Saved layout %s", profile_.name.c_str()));
    } else {
        status_ = i18n::trf("Save failed: %s", err.c_str());
    }
    { std::lock_guard lk(inputMu_); input_.setProfile(profile_); }
    engineProfile_ = profile_;
    refreshProfiles();
    return ok;
}

void App::connect(const std::string& serial) {
    disconnect();
    autoConnectSerial_.clear();
    autoConnectAtUs_ = 0;
    serial_ = serial;
    wantSession_ = true;
    pendingSettings_ = false;  // this connection applies the current settings
    // Physical display size - needed so camera sensitivity means device pixels
    // per mouse count (WASD+ parity) instead of frame pixels, which made the
    // same value faster on smaller streams.
    adb_->deviceSize(serial, devW_, devH_);
    if (!devW_ || !devH_)
        showToast(i18n::tr("Could not read the screen size — camera speed may differ with the stream resolution"));
    reconnectAttempts_ = 0;
    SessionOptions o;
    o.serial = serial;
    o.video.maxSize = settings_.maxSize;
    o.video.bitRate = settings_.bitRate;
    o.video.maxFps = settings_.maxFps;
    o.video.h265 = settings_.h265;
    o.video.hardwareDecode = settings_.hardwareDecode;
    o.showTouches = settings_.showTouches;
    // Competitive: always present the newest decoded frame. Smooth: allow a 2-frame buffer.
    pending_.setMaxDepth(settings_.displayMode == 1 ? 2 : 1);
    // The input thread dereferences session_ (in sendTouch) under inputMu_, so the
    // pointer swap must be under the same lock. start() is non-blocking — it only
    // spawns the session thread — so the lock is held briefly.
    {
        std::lock_guard lk(inputMu_);
        session_ = std::make_unique<Session>(*adb_, platform::runtimeDir() + L"/scrcpy-server");
        session_->setControlStaleAgeUs(static_cast<int64_t>(settings_.queueStaleMs) * 1000);
        lastControlDropped_ = lastControlStale_ = 0;
        session_->start(
            o, renderer_.device(),
            [this](video::DecodedFrame&& f) {
                pending_.put(std::move(f));
                SetEvent(frameEvent_);
            },
            [this](uint32_t w, uint32_t h) {
                streamW_ = w;
                streamH_ = h;
                SetEvent(frameEvent_);
            });
    }
}

void App::disconnect() {
    launcherSkip_ = false;  // back to the device picker after every session ends
    releaseInput();
    setLocked(false);
    cancelPick();
    // Hold inputMu_ while stopping and freeing the session: sendTouch() on the
    // input tick thread dereferences session_ under the same lock. Without this,
    // a tick-thread emit could race with session_.reset() and use a freed pointer.
    std::lock_guard lk(inputMu_);
    if (session_) session_->stop();
    session_.reset();
    pending_.take();
    wantSession_ = false;
    streamW_ = streamH_ = 0;
    appliedW_ = appliedH_ = 0;
    devW_ = devH_ = 0;
    pendingSettings_ = false;
    reconnectPrompt_ = false;
    reconnectAction_ = {};
}

// Reconnect = close the session window (disconnect returns to the launcher),
// let the launcher show briefly, then dial the same device again. Splitting the
// teardown and the new session across frames mirrors a fresh Connect click.
void App::reconnectTo(const std::string& serial) {
    autoConnectSerial_ = serial;
    autoConnectAtUs_ = nowMicros() + 700000;  // ~0.7 s of launcher before dialing
    disconnect();
}

// Forces a fresh `adb devices -l` outside the tracker's event stream - the
// launcher's Refresh button. The tracker still pushes live changes on its own.
void App::refreshDevices() {
    if (!adb_ || !adb_->available()) return;
    std::string e;
    std::vector<platform::AdbDevice> devs = adb_->devices(e);
    if (!e.empty()) {
        showToast(i18n::trf("Refresh failed: %s", e.c_str()));
        return;
    }
    {
        std::lock_guard lk(devMu_);
        devices_ = std::move(devs);
    }
    showToast(i18n::tr("Device list updated"));
}

// Restarts a failed session with backoff while the device is still listed by adb.
void App::superviseSession() {
    if (!session_ || !wantSession_) return;
    const SessionState s = session_->state();
    if (s == SessionState::Running) {
        reconnectAttempts_ = 0;
        return;
    }
    if (s != SessionState::Failed) return;
    releaseInput();
    setLocked(false);
    if (!settings_.autoReconnect || reconnectAttempts_ >= 5) return;
    bool present = false;
    {
        std::lock_guard lk(devMu_);
        for (auto& d : devices_) present |= d.serial == serial_ && d.usable();
    }
    const int64_t now = nowMicros();
    if (!present) return;  // tracker will report it again when plugged back in
    if (!reconnectAt_) {
        reconnectAt_ = now + (500000LL << std::min(reconnectAttempts_, 3));
        return;
    }
    if (now < reconnectAt_) return;
    reconnectAt_ = 0;
    const int attempts = reconnectAttempts_ + 1;
    const std::string serial = serial_;
    log::info("reconnecting (attempt " + std::to_string(attempts) + ")");
    connect(serial);
    reconnectAttempts_ = attempts;
}

void App::sendTouch(const proto::TouchEvent& ev) {
    // -------- touch trace: the full click path, mouse -> window -> phone --------
    if (settings_.touchTrace) {
        static const char* names[] = {"DOWN", "UP", "MOVE"};
        static thread_local std::unordered_map<uint64_t, int64_t> pendingDown;  // per-thread: sendTouch runs on the input thread too
        const int64_t nowMs = nowMicros() / 1000;
        const int a = static_cast<int>(ev.action);
        std::string line = std::string("trace touch ") + (a >= 0 && a < 3 ? names[a] : "?") + " id=0x" + [&] {
            char b[24];
            std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(ev.pointerId));
            return std::string(b);
        }() + " frame=(" + std::to_string(ev.x) + "," + std::to_string(ev.y) + ") of " + std::to_string(ev.frameWidth) + "x" +
                   std::to_string(ev.frameHeight) + " t=" + std::to_string(nowMs) + "ms";
        if (a == 0) pendingDown[ev.pointerId] = nowMs;
        if (a == 1) {
            const auto it = pendingDown.find(ev.pointerId);
            if (it != pendingDown.end()) line += " dwell=" + std::to_string(nowMs - it->second) + "ms", pendingDown.erase(it);
        }
        if (!session_) line += " DROPPED: no session";
        else if (editMode_) line += " DROPPED: edit mode";
        log::info(line);
    }
    // Ups always pass: a release must reach the phone even in edit mode or a
    // ghost finger stays held down there.
    if (!session_ || (editMode_ && ev.action != proto::MotionAction::Up)) return;
    const int64_t t1 = nowMicros();
    // T0->T1 processing latency only for real input events (inputT0Us_ is set by
    // handleMessage/keyEvent/WM_INPUT/direct-pointer paths). Synthetic events from
    // macros/probe keep whatever t0 they carried and are still measured end-to-end.
    const int64_t t0 = inputT0Us_.load();
    if (t0 > 0 && t1 - t0 < 1000000) inputProcUs_.add(static_cast<double>(t1 - t0));
    session_->sendTouch(ev, t0 ? t0 : t1);
    if (ev.action == proto::MotionAction::Down || ev.action == proto::MotionAction::Up) {
        flight_.record(ev.action == proto::MotionAction::Down ? FlightEventType::TouchDown : FlightEventType::TouchUp,
                       ev.pointerId, ev.x, ev.y, 0, input_.activeSourcesMask(), t1);
    }
}

void App::releaseInput() {
    if (macros_.active()) macros_.cancel("input released");
    std::lock_guard lk(inputMu_);
    input_.releaseAll();
    if (directDown_) input_.directPointer(false, {0, 0});
    directDown_ = false;
}

void App::setLocked(bool on) {
    if (on == locked_) return;
    locked_ = on;
    std::lock_guard lk(inputMu_);
    input_.setLocked(on);
    if (on) {
        // Collapse the content panel and recompute the actual game viewport
        // before clipping, so the cursor cannot wander onto the side controls.
        panelCollapsedBeforeLock_ = settings_.panelCollapsed;
        settings_.panelCollapsed = true;
        selectedBinding_ = -1;
        updateLayout();
        RECT r{static_cast<LONG>(std::floor(videoRect_.x)),
               static_cast<LONG>(std::floor(videoRect_.y)),
               static_cast<LONG>(std::ceil(videoRect_.x + videoRect_.w)),
               static_cast<LONG>(std::ceil(videoRect_.y + videoRect_.h))};
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ClipCursor(&r);
        while (ShowCursor(FALSE) >= 0) {}
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
        // Restore the panel state from before the lock - without this the side
        // settings stayed collapsed (looked "hidden") after leaving fullscreen.
        settings_.panelCollapsed = panelCollapsedBeforeLock_;
        updateLayout();
    }
}

void App::updateLayout() {
    const float panel = showPanel_ && !launcherMode() ? panelWidth() : 0.0f;
    const float cw = std::max(1.0f, renderer_.width() - panel), ch = static_cast<float>(renderer_.height());
    gameArea_ = {i18n::rtl() ? 0.0f : panel, 0.0f, cw, ch};
    const uint32_t fw = appliedW_ ? appliedW_ : 1080, fh = appliedH_ ? appliedH_ : 2400;
    mapper_.update(cw, ch, fw, fh);
    videoRect_ = fitViewport(cw, ch, fw, fh);
    videoRect_.x += gameArea_.x;
}

bool App::keyEvent(int vk, bool down) {
    if (capture_.active()) return false;
    if (down && vk == profile_.lockToggleKey && sessionActive() && !editMode_) {
        setLocked(!locked_);
        return true;
    }
    if (!sessionActive() || editMode_) return false;
    inputT0Us_ = nowMicros();
    { std::lock_guard lk(inputMu_);
        flight_.record(down ? FlightEventType::KeyDown : FlightEventType::KeyUp, static_cast<uint64_t>(vk), 0, 0, 0,
                       input_.activeSourcesMask(), inputT0Us_);
    }
    if (automationKey(vk, down)) return true;
    std::lock_guard lk(inputMu_);
    const bool used = input_.onKey(vk, down, inputT0Us_, InputSource::Physical);
    std::size_t toggledIdx = 0;
    bool recoilOn = false;
    if (input_.consumeRecoilToggle(toggledIdx, recoilOn) && toggledIdx < profile_.bindings.size()) {
        profile_.bindings[toggledIdx].recoilEnabled = recoilOn;
        // In-game toggles are deliberate state, not draft edits — mirror them into
        // the saved baseline so they never raise the unsaved-changes prompt.
        if (toggledIdx < saved_.bindings.size()) saved_.bindings[toggledIdx].recoilEnabled = recoilOn;
        if (toggledIdx < engineProfile_.bindings.size()) engineProfile_.bindings[toggledIdx].recoilEnabled = recoilOn;
        showToast(i18n::tr(recoilOn ? "Recoil control: ON" : "Recoil control: OFF"));
    }
    bool rapidOn = false;
    if (input_.consumeRapidFireToggle(toggledIdx, rapidOn) && toggledIdx < profile_.bindings.size()) {
        profile_.bindings[toggledIdx].rapidFire = rapidOn;
        if (!rapidOn) profile_.bindings[toggledIdx].recoilEnabled = true;
        if (toggledIdx < saved_.bindings.size()) {
            saved_.bindings[toggledIdx].rapidFire = rapidOn;
            if (!rapidOn) saved_.bindings[toggledIdx].recoilEnabled = true;
        }
        if (toggledIdx < engineProfile_.bindings.size()) {
            engineProfile_.bindings[toggledIdx].rapidFire = rapidOn;
            if (!rapidOn) engineProfile_.bindings[toggledIdx].recoilEnabled = true;
        }
        showToast(i18n::tr(rapidOn ? "Mode: Rapid Fire (Pistol & DMR)" : "Mode: Continuous Fire (Recoil control)"));
    }
    return used;
}

void App::onFrameDisplayed(const video::DecodedFrame& f) {
    const int64_t now = nowMicros();
    ++presented_;
    presentFps_.tick(now);
    presentLatencyUs_.add(static_cast<double>(now - f.rxUs));
}

// Visual touch probe: measures touch sent -> first decoded frame whose luma at the probe
// point changes. Needs a visible response at that point (e.g. "show touches" enabled).
void App::probeTick(int64_t now) {
    if (!probe_.active || !sessionActive() || !renderer_.hasVideo()) return;
    const auto luma = renderer_.sampleLuma(probe_.point);
    if (!luma) return;
    const uint32_t fw = session_->frameWidth(), fh = session_->frameHeight();
    proto::TouchEvent ev;
    ev.pointerId = 0x3000;
    ev.frameWidth = static_cast<uint16_t>(fw);
    ev.frameHeight = static_cast<uint16_t>(fh);
    ev.x = static_cast<int32_t>(probe_.point.x * fw);
    ev.y = static_cast<int32_t>(probe_.point.y * fh);
    if (probe_.phase == 0 && now >= probe_.nextAt) {
        probe_.baseline = *luma;
        ev.action = proto::MotionAction::Down;
        probe_.tDown = now;
        session_->sendTouch(ev, now);
        probe_.phase = 1;
    } else if (probe_.phase == 1) {
        const bool changed = std::abs(*luma - probe_.baseline) > 24.0f;
        const bool timeout = now - probe_.tDown > 1000000;
        if (changed) probe_.resultsUs.push_back(static_cast<double>(now - probe_.tDown));
        if (timeout) ++probe_.misses;
        if (changed || timeout) {
            ev.action = proto::MotionAction::Up;
            session_->sendTouch(ev, now);
            probe_.phase = 0;
            probe_.nextAt = now + 400000;
            if (static_cast<int>(probe_.resultsUs.size()) + probe_.misses >= probe_.target) probe_.active = false;
        }
    }
}

void App::exportCsv() {
    if (!session_) return;
    std::vector<CsvRow> rows;
    const std::string run = std::to_string(nowMicros());
    const std::string mode = std::to_string(session_->frameWidth()) + "x" + std::to_string(session_->frameHeight()) + " " +
                             session_->decoderDescription() + (settings_.vsync ? " vsync" : " novsync");
    auto add = [&](const char* metric, const std::vector<double>& v) {
        for (std::size_t i = 0; i < v.size(); ++i) rows.push_back({run, serial_, mode, metric, "internal", i, v[i]});
    };
    add("decode_us", session_->decodeLatencyUs.snapshot());
    add("packet_interval_us", session_->frameIntervalUs.snapshot());
    add("control_send_us", session_->sendLatencyUs.snapshot());
    add("queue_wait_us", session_->queueWaitUs.snapshot());
    add("socket_write_us", session_->sendWriteUs.snapshot());
    add("input_proc_us", inputProcUs_.snapshot());
    add("det_readback_us", detReadbackUs_.snapshot());
    add("det_infer_us", detInferUs_.snapshot());
    add("det_mail_us", detMailUs_.snapshot());
    add("det_track_us", detTrackUs_.snapshot());
    add("det_aim_us", detAimUs_.snapshot());
    add("det_endtoend_us", detEndToEndUs_.snapshot());
    add("present_us", renderUs_.snapshot());
    add("rx_to_present_us", presentLatencyUs_.snapshot());
    add("probe_touch_to_frame_us", probe_.resultsUs);
    const std::string path = narrow(platform::logsDir()) + "/measure-" + run + ".csv";
    std::string err;
    status_ = writeCsv(path, rows, &err) ? "CSV written: " + path : "CSV failed: " + err;
}

void App::frame() {
    const int64_t now = nowMicros();
    if (tracker_) superviseSession();
    if (!autoConnectSerial_.empty() && !session_ && now >= autoConnectAtUs_)
        connect(autoConnectSerial_);
    if (streamW_ != appliedW_ || streamH_ != appliedH_) {
        appliedW_ = streamW_;
        appliedH_ = streamH_;
        { std::lock_guard lk(inputMu_);
            input_.setFrameSize(appliedW_, appliedH_);
            // Device dims arrive portrait from `wm size`; orient them to match
            // the (landscape) video frame before the engine scales deltas.
            if (devW_ && devH_) {
                const bool land = appliedW_ >= appliedH_;
                input_.setDeviceSize(land ? std::max(devW_, devH_) : std::min(devW_, devH_),
                                     land ? std::min(devW_, devH_) : std::max(devW_, devH_));
            }
        }
        fitWindowToVideo();  // size the window to the new stream aspect (no dead margins)
    }
    if (!sessionActive() && locked_) setLocked(false);
    updateLayout();
    { std::lock_guard lk(inputMu_); input_.setTrace(settings_.touchTrace); }
    inputT0Us_ = now;  // events emitted by tick()/automation carry this frame's T0
    // Read the clock inside the lock: `now` was sampled before the mutex and the
    // 1 ms input thread may have ticked since; a stale timestamp would warp dtUs.
    { std::lock_guard lk(inputMu_); input_.tick(nowMicros()); }
    automationTick(now);
    // Hardware reconciliation: if the OS says a physical key/mouse button is up
    // but our state still has a Physical hold on it (a lost WM_KEYUP path: edit
    // mode entry, capture, ImGui focus, etc.), force the release. This makes a
    // permanently stuck direction key / joystick impossible.
    std::array<uint8_t, 256> heldSrc{};
    { std::lock_guard lk(inputMu_);
        for (int vk = 0; vk < 256; ++vk) heldSrc[vk] = input_.keySources(vk);
    }
    for (int vk = 1; vk < 256; ++vk) {
        if (!(heldSrc[vk] & static_cast<uint8_t>(InputSource::Physical))) continue;
        if (!isWheelVk(vk) && (GetAsyncKeyState(vk) & 0x8000) == 0)
            { std::lock_guard lk(inputMu_); input_.onKey(vk, false, now, InputSource::Physical); }
    }
    inputT0Us_ = 0;
    // Surface silent finger-budget rejections instead of letting keys look dead.
    std::size_t rej;
    { std::lock_guard lk(inputMu_); rej = input_.fingerRejections(); }
    if (rej != lastFingerReject_) {
        lastFingerReject_ = rej;
        if (now - rejectToastUs_ > 3000000) {
            rejectToastUs_ = now;
            showToast(i18n::tr("Touch limit reached: raise Max fingers or free a held binding"));
        }
    }

    // Profiles are only persisted by an explicit Save. The dirty flag drives the
    // unsaved-changes prompt on navigation/close; nothing auto-writes the file.

    // Non-blocking GPU readiness check: if the swap chain is still presenting the
    // previous frame, skip rendering entirely this iteration. Input was already
    // processed above, so skipping the render keeps the mouse pipeline running at
    // full speed instead of stalling on a 16 ms GPU wait.
    if (!renderer_.waitForNextFrame()) return;
    // While a screen pick is active (color / zone / crop / region), freeze the
    // displayed frame: the video behaves like a snapshot, and region readbacks
    // sample exactly what the user sees.
    std::optional<video::DecodedFrame> f;
    if (pickMode_ == 0) f = pending_.take();
    int64_t t7 = 0;
    if (f) {
        renderer_.upload(*f);
        t7 = nowMicros();  // T7: frame submitted to the render pipeline
    }
    if (f) probeTick(now);

    if (now - lastCpuWall_ > 1000000) {
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        const uint64_t t = ((uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime) + ((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime);
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        if (lastCpuWall_) cpuPercent_ = (t - lastCpuTime_) / 10.0 / ((now - lastCpuWall_) * si.dwNumberOfProcessors) * 100.0;
        lastCpuTime_ = t;
        lastCpuWall_ = now;
        PROCESS_MEMORY_COUNTERS pmc{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) workingSetMb_ = pmc.WorkingSetSize / 1048576.0;
    }

    renderer_.beginFrame();
    renderer_.drawVideo(videoRect_);
    drawUi();
    renderer_.present(settings_.vsync);
    if (t7) renderUs_.add(static_cast<double>(nowMicros() - t7));  // T7->T8
    if (f) onFrameDisplayed(*f);

    // Detect control-queue backpressure & latency spikes for the flight recorder.
    if (session_) {
        uint32_t srcMask;
        { std::lock_guard lk(inputMu_); srcMask = input_.activeSourcesMask(); }
        const uint64_t dropDelta = session_->controlDropped() - lastControlDropped_;
        const uint64_t staleDelta = session_->controlStaleDropped() - lastControlStale_;
        lastControlDropped_ = session_->controlDropped();
        lastControlStale_ = session_->controlStaleDropped();
        if (dropDelta || staleDelta)
            flight_.record(dropDelta ? FlightEventType::QueueDrop : FlightEventType::QueueStaleDrop,
                           static_cast<uint64_t>(dropDelta + staleDelta), 0, 0,
                           static_cast<int32_t>(session_->controlOldestAgeUs() / 1000), srcMask, now);
        const double lastSend = session_->sendLatencyUs.latest();
        if (lastSend > 25000)  // >25 ms end-to-end send latency is a visible hitch
            flight_.record(FlightEventType::LatencySpike, 0, 0, 0, static_cast<int32_t>(lastSend),
                           srcMask, now);
        const int64_t qAge = session_->controlOldestAgeUs();
        if (qAge > 80000)
            flight_.record(FlightEventType::LatencySpike, session_->controlQueueSize(), 0, 0,
                           static_cast<int32_t>(qAge), srcMask, now);
    }
}

bool App::handleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    result = 0;
    const bool uiKeyboard = ImGuiWantsKeyboard() && !locked_;
    const bool uiMouse = ImGuiWantsMouse() && !locked_;
    auto mouseVk = [&](UINT m) {
        switch (m) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: return VK_LBUTTON;
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: return VK_RBUTTON;
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: return VK_MBUTTON;
            default: return GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        }
    };
    if (captureMessage(msg, wp, lp)) return true;
    switch (msg) {
        case WM_CLOSE:
            // Closing the session window returns to the launcher - the app only
            // exits when the launcher itself is closed.
            if (!launcherMode()) {
                requestDiscard([this] { disconnect(); });
            } else {
                requestDiscard([] { PostQuitMessage(0); });
            }
            return true;  // consume; the app exits only after the user decides
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) renderer_.resize(LOWORD(lp), HIWORD(lp));
            return false;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) cancelCapture(), releaseInput(), setLocked(false);
            return false;
        case WM_KILLFOCUS:
            if (automationOn_) setAutomation(false, i18n::tr("Stopped: window lost focus"));
            cancelCapture();
            dragBinding_ = -1, dragging_ = false;
            releaseInput();
            setLocked(false);
            return false;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const int vk = static_cast<int>(wp);
            if (vk == VK_F1 && !(lp & 0x40000000)) {
                showPanel_ = !showPanel_;
                fitWindowToVideo();
                return true;
            }
            if (uiKeyboard) return false;
            if (lp & 0x40000000) { std::lock_guard lk(inputMu_); return input_.activeTouches() > 0; }  // ignore auto-repeat
            return keyEvent(vk, true);
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            bool touching;
            { std::lock_guard lk(inputMu_); touching = input_.activeTouches() > 0; }
            if (uiKeyboard && !touching) return false;
            return keyEvent(static_cast<int>(wp), false);
        }
        case WM_INPUT:
            // Raw mouse input is delivered to the dedicated input thread's
            // RIDEV_INPUTSINK window, not here. Nothing to do.
            return false;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: {
            if (uiMouse || editMode_ || !sessionActive()) return false;
            SetCapture(hwnd_);
            if (!locked_ && msg == WM_LBUTTONDOWN) {
                inputT0Us_ = nowMicros();
                const Vec2 p{static_cast<float>(GET_X_LPARAM(lp)) - gameArea_.x, static_cast<float>(GET_Y_LPARAM(lp)) - gameArea_.y};
                const auto n = mapper_.windowToNormalized(p);
                if (n) { std::lock_guard lk(inputMu_); input_.directPointer(true, *n); directDown_ = true; }
                inputT0Us_ = 0;
                if (settings_.touchTrace)
                    log::info("trace mouse down client=(" + std::to_string(GET_X_LPARAM(lp)) + "," + std::to_string(GET_Y_LPARAM(lp)) +
                              ") game=(" + std::to_string(static_cast<int>(p.x)) + "," + std::to_string(static_cast<int>(p.y)) + ") " +
                              (n ? "norm=(" + std::to_string(n->x) + "," + std::to_string(n->y) + ")" : std::string("outside the video: ignored")));
                return true;
            }
            keyEvent(mouseVk(msg), true);
            return true;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            // The wheel is a pulse device: emit an instant down+up so it works with
            // Tap / Toggle / macro triggers exactly like any other key.
            if (uiMouse || editMode_ || !sessionActive()) return false;
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            const int vk = msg == WM_MOUSEWHEEL ? (delta > 0 ? kVkWheelUp : kVkWheelDown)
                                                : (delta > 0 ? kVkWheelRight : kVkWheelLeft);
            keyEvent(vk, true);
            keyEvent(vk, false);
            return true;
        }
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            // Drop the capture only when no mouse buttons remain held: releasing
            // it unconditionally while another button is still down can lose that
            // button's WM_*BUTTONUP outside the window and stick its finger.
            if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON) |
                 GetAsyncKeyState(VK_XBUTTON1) | GetAsyncKeyState(VK_XBUTTON2)) >= 0)
                ReleaseCapture();
            if (directDown_ && msg == WM_LBUTTONUP) {
                inputT0Us_ = nowMicros();
                { std::lock_guard lk(inputMu_); input_.directPointer(false, {0, 0}); }
                inputT0Us_ = 0;
                directDown_ = false;
                return true;
            }
            return keyEvent(mouseVk(msg), false);
        case WM_MOUSEMOVE:
            if (directDown_) {
                inputT0Us_ = nowMicros();
                const Vec2 p{static_cast<float>(GET_X_LPARAM(lp)) - gameArea_.x, static_cast<float>(GET_Y_LPARAM(lp)) - gameArea_.y};
                if (auto n = mapper_.windowToNormalized(p)) { std::lock_guard lk(inputMu_); input_.directMove(*n); }
                inputT0Us_ = 0;
            }
            return false;
    }
    return false;
}

}  // namespace wasd::app
