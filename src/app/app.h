// Windows application shell: owns the window-side renderer, device tracking,
// the active session, input routing and the ImGui user interface.
#pragma once
#include <windows.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <fstream>

#include "core/aim_control.h"
#include "core/bounded_queue.h"
#include "core/coords.h"
#include "core/detector.h"
#include "core/detect_tracker.h"
#include "core/editor.h"
#include "core/imgmatch.h"
#include "core/input_engine.h"
#include "core/profile.h"
#include "core/stats.h"
#include "core/target_manager.h"
#include "core/vision_frame.h"
#include "core/vision_fusion.h"
#include "platform/adb.h"
#include "render/renderer.h"
#include "session/session.h"

namespace wasd::studio { class GraphRunner; }  // visual-graph execution engine (studio_engine.h)

namespace wasd::app {

struct VideoPreset {
    const char* name;
    uint32_t maxSize, bitRate, maxFps;
};
inline constexpr VideoPreset kPresets[] = {
    {"Low latency (1280px, 6 Mbit/s)", 1280, 6000000, 0},
    {"Balanced (1920px, 10 Mbit/s)", 1920, 10000000, 0},
    {"High quality (native, 20 Mbit/s)", 0, 20000000, 0},
};

struct Settings {
    int preset = 0;
    bool vsync = false;
    bool hardwareDecode = true;
    bool h265 = false;
    bool autoReconnect = true;
    bool showTouches = false;
    std::string lastProfile = "Default";
    std::string language = "en";
    bool panelCollapsed = false;
    bool darkTheme = true;
    int panelWidthPx = 440;
    uint32_t maxSize = 1280, bitRate = 6000000, maxFps = 0;  // custom video quality (0 = native / unlimited)
    int displayMode = 0;           // 0 = Competitive (latest frame, aggressive stale-drop), 1 = Smooth (2-frame buffer)
    int queueStaleMs = 60;         // max age of queued touch MOVE commands before they are dropped as stale
    bool showFps = false;
    int overlayOpacity = 65;  // floating buttons, percent (visual only)
    bool overlayLandscapeOnly = true;
    bool touchTrace = false;
};

struct LatencyProbe {
    bool active = false;
    Vec2 point{0.5f, 0.5f};
    int target = 30;
    int phase = 0;  // 0 wait, 1 touching
    int64_t tDown = 0, nextAt = 0;
    float baseline = 0;
    int misses = 0;
    std::vector<double> resultsUs;
};

enum class Section { Home, Device, Mapping, Macros, Images, Colors, Detect, Settings };

class StudioEditor;  // NEXPLAY Automation Studio (studio_canvas.cpp / studio_panels.cpp)

// What the editor is waiting a key for. slot: 0 key, 1 up, 2 left, 3 down, 4 right, 5 mouse-lock key.
struct CaptureTarget {
    int binding = -1;
    int slot = -1;
    bool isNew = false;     // remove the binding if capture is cancelled
    bool sequence = false;  // joystick: continue with the next direction
    int previous = 0;
};

class App {
public:
    App();   // out-of-line ctor/dtor: unique_ptr<StudioEditor> needs the complete type
    ~App();
    bool init(HWND hwnd, std::string& err);
    void initUi();
    void shutdown();
    void frame();
    bool handleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
    ID3D11Device* rendererDevice() const { return renderer_.device(); }
    ID3D11DeviceContext* rendererContext() const { return renderer_.context(); }
    HANDLE frameEvent() const { return frameEvent_; }
    bool sessionActive() const { return session_ && session_->state() == SessionState::Running; }
    bool inputLocked() const { return locked_; }

private:
    void connect(const std::string& serial);
    void disconnect();
    // Closes the session view back to the launcher, then auto-connects the same
    // device - the "close window 2, reconnect, reopen window 2" flow.
    void reconnectTo(const std::string& serial);
    void superviseSession();
    void onFrameDisplayed(const video::DecodedFrame& f);
    void setLocked(bool on);
    void releaseInput();
    void sendTouch(const proto::TouchEvent& ev);
    void updateLayout();
    float panelWidth() const;
    bool overlayAllowed() const;
    bool keyEvent(int vk, bool down);
    void probeTick(int64_t now);
    void exportCsv();

    void loadSettings();
    void saveSettings();
    void refreshProfiles();
    void loadProfile(const std::string& name);
    bool saveProfile();
    void saveProfileAs(const std::string& name);
    void renameProfile(const std::string& name);
    void newProfile(const std::string& name);
    void resetProfileToDefault();
    bool hasUnsavedChanges() const { return !(profile_ == saved_); }
    // (auto-save removed: profiles persist only via an explicit Save action)
    bool confirmDiscard();
    // In-app styled discard dialog: when the layout is dirty it opens a NEXPLAY-themed
    // modal (Save / Don't save / Cancel) and runs `action` only after the user decides.
    // Replaces the legacy MessageBoxW which could trap the user in a loop when the
    // save failed validation without any visible reason.
    void requestDiscard(std::function<void()> action);
    void discardChanges();  // reverts profile_ to the last saved state (Don't save path)
    // Shared name-input modal: Save as / Rename / New layout all open the same
    // dialog instead of the old always-visible "New layout name" field.
    void beginNamePrompt(const char* title, std::function<void(const std::string&)> action);
    void drawNamePrompt();
    struct NamePrompt {
        bool open = false;
        const char* title = nullptr;
        std::function<void(const std::string&)> action;
        char buf[64] = {};
    } namePrompt_;
    void drawDiscardPrompt();
    std::function<void()> discardAction_;
    bool discardPrompt_ = false;

    // Settings marked dirty by a change that only applies on the next
    // connection. Navigating away while dirty prompts for a reconnect.
    void requestNav(std::function<void()> action);
    void drawReconnectPrompt();
    std::function<void()> reconnectAction_;
    bool reconnectPrompt_ = false;
    bool pendingSettings_ = false;

    // key-mapping editor (editor.cpp)
    void setEditMode(bool on);
    void setLanguage(const std::string& code);
    void addBindingAt(BindingType type, Vec2 n);
    void duplicateBinding(int i);
    void deleteBinding(int i);
    void undo();
    void redo();
    void beginCapture(int binding, int slot, bool isNew, bool sequence = false);
    void finishCapture(int vk);
    void cancelCapture();
    void endCapture();
    bool captureMessage(UINT msg, WPARAM wp, LPARAM lp);
    int* captureSlot(const CaptureTarget& t);
    std::string keyOwner(int vk, int binding, int slot) const;
    void showToast(const std::string& text);

    // automation (automation_runtime.cpp)
    void automationTick(int64_t now);
    bool automationKey(int vk, bool down);
    void setAutomation(bool on, const std::string& reason);
    void macroTouch(int action, int finger, Vec2 n);
    std::optional<bool> zonePresent(int zone) const;
    std::optional<bool> imagePresent(int image) const;
    // Unified image-recognition sink: registers the step's region/threshold/cadence as
    // a temporary scan override and returns the latest scan plus which params it used.
    ImageScanResult imageScanQuery(int image, const ImageQuery& q);
    void loadRefImageFiles();          // fills ImageRef.rgb from the images data folder
    std::string refImagePath(const ImageRef& r) const;
    std::string imagesDirFor(const std::string& profileName) const;  // <APPDATA>/images/<profile>
    bool saveRefImageFile(const ImageRef& r, std::string& err);
    void deleteRefImageFile(const ImageRef& r);
    void copyImagesDir(const std::string& fromProfile, const std::string& toProfile);  // for save-as
    void moveImagesDir(const std::string& fromProfile, const std::string& toProfile);  // for rename

    // UI (ui.cpp)
    void applyTheme();
    void drawUi();
    void handleShortcuts();
    // Startup launcher (WASD+ style): a compact device picker shown while no
    // session exists. Connect restores the window and reveals the full UI.
    bool launcherMode() const { return !session_ && !launcherSkip_; }
    void drawLauncher();
    void applyLauncherWindow(bool on);
    void fitWindowToVideo();
    void refreshDevices();
    void drawEmptyState();
    void drawOverlay();
    void drawToolPalette();
    void drawToast();
    void drawFpsOverlay();
    // automation & panels (ui.cpp / automation_ui.cpp)
    void drawPanel();
    // NEXPLAY Automation Studio workspace (fullscreen takeover while studioOpen_).
    void openStudio();
    void drawStudio();
    void panelHeader();
    void panelHome();
    void panelDevice();
    void panelGeneral();
    void panelMapping();
    void panelSelected();
    void panelPerformance();
    void panelLogs();
    void panelHelp();
    void panelSettings();
    void panelMacros();
    void panelImages();
    void panelColors();
    void panelDetect();
    void panelAutomation();
    void automationUiTick();
    void cancelPick();  // disarm any active screen pick and clear its targets
    void drawZonesOverlay();
    void drawImagesOverlay();   // reference-image search regions + last-match markers
    void stepList(std::vector<Step>& list, int depth, std::vector<int>& pathPrefix);
    void drawStepInspector(Step& s, int depth, const std::vector<Step>& siblings);
    void drawImageStepInspector(Step& s, const std::vector<Step>& siblings);  // unified image step editor
    void deleteRefImage(int idx);                            // removes an ImageRef + fixes all references
    void beginExternalCapture(int* p, int slot, int* p2 = nullptr);
    void imagePixelPreview(const ImageRef& r, float box, const Vec2* mark = nullptr);  // magnified crop preview

    HWND hwnd_ = nullptr;
    render::Renderer renderer_;
    std::unique_ptr<platform::Adb> adb_;
    std::unique_ptr<platform::DeviceTracker> tracker_;
    std::mutex devMu_;
    std::vector<platform::AdbDevice> devices_;
    std::string trackerError_;

    std::unique_ptr<Session> session_;
    std::string serial_;
    bool wantSession_ = false;
    int64_t reconnectAt_ = 0;
    int reconnectAttempts_ = 0;
    LatestSlot<video::DecodedFrame> pending_;
    HANDLE frameEvent_ = nullptr;
    std::atomic<uint32_t> streamW_{0}, streamH_{0};
    uint32_t appliedW_ = 0, appliedH_ = 0;
    uint32_t devW_ = 0, devH_ = 0;  // physical display size queried at connect

    // Dedicated input thread (ADR-006): owns a message-only window that receives
    // raw mouse input via RIDEV_INPUTSINK, batches deltas with GetRawInputBuffer,
    // and drives InputEngine::tick() at ~1 ms — all independent of the render
    // loop. All input_ calls are serialized via inputMu_ between the two threads.
    std::mutex inputMu_;
    std::thread inputTickThread_;
    std::atomic<bool> inputTickRunning_{false};
    HWND inputHwnd_ = nullptr;             // message-only raw-input sink window
    HANDLE inputHwndReady_ = nullptr;      // signaled once inputHwnd_ exists
    void startInputTick();
    void stopInputTick();
    void inputThreadLoop();
    void onInputRawInput(HRAWINPUT hraw);
    static LRESULT CALLBACK inputWndProc(HWND, UINT, WPARAM, LPARAM);

    InputEngine input_{[this](const proto::TouchEvent& ev) { sendTouch(ev); }};
    CoordinateMapper mapper_;
    Rect videoRect_;
    Rect gameArea_;  // client area left of/right of the side panel
    std::atomic<bool> locked_{false}, editMode_{false}, showPanel_{true}, directDown_{false};
    bool panelCollapsedBeforeLock_ = false;  // restored when leaving locked mode
    int selectedBinding_ = -1;

    // editor state
    Section section_ = Section::Mapping;
    Section autoSec_ = Section::Macros;  // last automation sub-tab (Macros / Images / Colors)
    BindingType tool_ = BindingType::Tap;
    KeyCapture capture_;
    CaptureTarget capTarget_;
    int comboFirstVk_ = 0;  // first key held during dual-key combo capture on slot 0
    std::vector<Rect> captureRects_, captureRectsNext_;  // regions where a click feeds the capture
    EditHistory history_;
    Profile saved_, engineProfile_;
    int dragBinding_ = -1, dragPart_ = 0, hoveredListBinding_ = -1;
    int camDragHandle_ = -1;  // camera bounds resize handle being dragged: 0-3 corners, 4-5 X edges, 6-7 Y edges
    bool dragging_ = false, suppressMouse_ = false;
    Vec2 dragStart_, menuPos_;
    std::string launcherSerial_;               // device row selected in the launcher
    bool launcherSkip_ = false;                // gear icon opened the full app without a session
    std::string autoConnectSerial_;            // reconnectTo: device to dial once back at the launcher
    int64_t autoConnectAtUs_ = 0;              // and when to fire it
    bool launcherUi_ = false;                  // launcher was drawn on the previous frame
    std::string toast_;
    int64_t toastUntil_ = 0;

    // NEXPLAY Automation Studio: lazy-created editor editing profile_.automation.studioGraphs.
    std::unique_ptr<StudioEditor> studio_;
    bool studioOpen_ = false;
    bool studioResumeAfterPick_ = false;  // reopen the studio once a pickMode_ finishes
    // Visual-graph execution (studio_engine): shares the macro sinks; ticked from automationTick.
    std::unique_ptr<studio::GraphRunner> graphRunner_;
    std::string studioRunningGraphId_;  // id of the graph the runner is executing
    void studioRunGraph(const std::string& graphId, bool debug, uint32_t entryNodeId = 0);
    void studioStopGraph();
    // Async pixel/region color probes for Get Pixel Color / Find Color nodes
    // (readback on kGraphProbeSlot; serviced from automationTick).
    struct RegionProbe {
        bool active = false;
        bool wantsScan = false;          // false: single-point pixel sample
        Vec2 point{0.5f, 0.5f};
        Rect region{0.0f, 0.0f, 0.05f, 0.05f};
        studio::ColorVal target{};
        bool requested = false;
        int64_t requestedAt = 0;
        bool ready = false;
        int64_t readyAt = 0;
        studio::ColorVal color{};
        Vec2 center{0.5f, 0.5f};
        float similarity = 0.0f;
        bool found = false;
    } regionProbe_;
    std::optional<studio::ColorVal> graphPixelColor(Vec2 pt);
    std::optional<studio::ColorScanResult> graphColorScan(const Rect& region,
                                                        const studio::ColorVal& target);
    void serviceRegionProbe(int64_t now);
    // Async object-detection probes (YOLO/ONNX): readback on kDetectProbeSlot,
    // inference on the scan worker. `detectProbe_` serves Find Object nodes;
    // `detectPreview_` serves the inspector's live-test overlay; `detectWatch_`
    // is the standalone continuous watcher armed from the Detect section.
    bool detectWatchOn_ = false;
    bool detectAutoArmed_ = false;  // autostart fired once for this video session
    void setDetectWatch(bool on);
    struct DetectProbe {
        bool active = false;
        Rect region{0.0f, 0.0f, 1.0f, 1.0f};
        std::vector<std::string> classFilter;  // model class names; empty = all
        float conf = 0.5f;
        // Post-filters (used by the standalone watcher; off for graph/preview).
        bool exclOn = false;
        Rect exclRegion{};      // frame-normalized exclusion rect (own character)
        bool mateOn = false;
        float mateSat = 60.0f;      // min channel spread for a colored marker pixel
        float mateMinRatio = 0.03f;
        bool redOn = false;
        float redMinRatio = 0.02f;  // edge-ring pixels that must look red
        float redBoost = 0.20f;     // confidence bonus for red-outlined hits
        bool requested = false;     // readback queued
        int64_t requestedAt = 0;
        int64_t readbackAtUs = 0;   // pollRegion success (readback wait probe)
        uint64_t pendingReqId = 0;  // inference in flight on the worker
        bool ready = false;
        int64_t readyAt = 0;
        int64_t lastInferUs = 0;    // duration of the last completed inference
        studio::ObjectScanResult result{};
        std::vector<Detection> hits;  // frame-normalized, for the live overlay
        std::vector<Detection> rawHits; // pre-fusion hits (debug overlay)
        uint64_t frameId = 0;         // stamp of the analyzed source frame
        int64_t captureUs = 0;        // readback timestamp of that frame
    } detectProbe_, detectPreview_, detectWatch_;
    // Sticky tracker for the standalone watcher: explicit lifecycle state
    // machine (Confirmed / TempLost / Reacquiring / Expired) that keeps the
    // last REAL measurement separate from the predicted glide, decays stale
    // velocity on missed scans, requires fresh consistent evidence to
    // re-confirm, and logs every transition for diagnostics.
    DetectTracker detectTracker_;
    int64_t lastWatchResultUs_ = 0;
    // Aim assist: while the hold key is down, proportional corrections steer
    // the camera (through onMouseDelta, so user sensitivity applies) toward a
    // Vision pipeline v2: scheduler (rate presets/Auto), fusion cache, lock FSM,
    // controller. aimTrackId_ mirrors targetLock_.trackId for overlay/protectId.
    InferScheduler inferSched_;
    TargetLock targetLock_;
    AimController aimCtrl_;
    uint64_t detectFrameSeq_ = 0;    // monotonic analyzed-frame counter
    std::vector<CharacterDet> fusedChars_;  // last fused detections (debug/raw view)
    // Magnet mode: while the aim key is held AND a lock is alive, user mouse
    // deltas are swallowed on the input thread so the hand can't fight the
    // pull. Written on the UI thread, read on the input thread -> atomic.
    std::atomic<bool> aimSwallowMouse_{false};
    // sticky-locked track; aimTrackId_ = 0 means "acquire on next tick".
    bool aimHeld_ = false;
    uint32_t aimTrackId_ = 0;
    bool aimInside_ = false;           // hysteresis: crosshair reached the target box
    Vec2 aimLastPt_{0.5f, 0.5f};       // last locked aim point — resume identity near it
    int64_t aimLastPtAtUs_ = 0;
    float aimAccX_ = 0, aimAccY_ = 0;  // fractional mouse-count accumulators
    float aimLockErr_ = 0;             // error magnitude when the lock was taken
    int64_t aimLockAtUs_ = 0;          // lock timestamp for the divergence guard
    // Plant-gain calibration: counts emitted between scans vs the locked
    // box's measured drift -> kPxPerCount (frame-px of travel per count).
    float aimEmitX_ = 0, aimEmitY_ = 0;    // signed counts emitted this scan window
    Vec2 aimPrevMeasC_{-1.0f, -1.0f};      // locked box center at the previous scan
    float aimKx_ = -1.0f, aimKy_ = -1.0f;  // per-axis EMA of px-per-count
    // Saturation diagnostics: how often the speed cap actually binds.
    uint64_t aimSteerTicks_ = 0, aimSatTicks_ = 0;
    // Diagnostics session recording (dbgRecord): bounded JSONL in the profile dir.
    std::ofstream detRec_;
    int detRecLines_ = 0;
    void tickAimAssist(int64_t now, const DetectWatch& cfg);
    std::optional<studio::ObjectScanResult> graphObjectScan(
        const Rect& region, const std::vector<std::string>& classFilter, float conf);
    void serviceDetectProbe(DetectProbe& p, int slot, int64_t now);
    std::vector<std::string> detectorClassNames();        // for inspector checkboxes (lazy-loads)
    std::string detectorStatus() const;                   // "loading"/"ready"/error text

    static constexpr uint64_t kMacroPointerBase = 0x4000;
    // Readback slots: zones scan via slots 0..9, reference images 10..13, color pick 14, image capture 15.
    static constexpr int kZoneSlots = 10, kImageSlotBase = 10, kPickColorSlot = 30, kPickImageSlot = 31;
    static constexpr int kGraphProbeSlot = 32;  // studio graph pixel/region color probe
    static constexpr int kDetectProbeSlot = 33; // Find Object detection region
    static constexpr int kDetectPreviewSlot = 34; // inspector live-test detection
    static constexpr int kDetectWatchSlot = 35;   // standalone continuous watcher
    struct ZoneScan {
        int64_t nextAt = 0, requestedAt = 0, lastUs = 0;
        bool requested = false;
        std::optional<bool> present;
        ZoneMatch match;
    };
    struct ImageScan {
        int64_t nextAt = 0, requestedAt = 0, lastUs = 0;
        bool requested = false;
        bool found = false;
        ImageMatch match;   // last positive match (normalized to the frame)
        std::vector<ImageMatch> matches;  // best-first candidates of the last scan
        float score = 0;
        bool tested = false;    // has a scan completed at least once
        int64_t scanCostUs = 0; // duration of the last match run
        // Parameters of the last issued request (req*) vs the last completed scan
        // (done*) — a macro step may temporarily override them; consumers must
        // check `done*`/defaultParams before trusting `found`.
        Rect reqRegion{0.0f, 0.0f, 1.0f, 1.0f};
        int reqThreshold = 0;
        bool reqWantMatches = false;  // whether the caller needs the candidate list
        Rect doneRegion{0.0f, 0.0f, 1.0f, 1.0f};
        int doneThreshold = 0;
        bool defaultParams = true;  // last scan used the ImageRef's own region+threshold
        // Worker-handoff: a scan job is in flight on the match worker thread.
        uint64_t pendingReqId = 0;
    };
    // Live scan override requested by a running ImageRecognize step or the UI test
    // button. Refreshed by every imageScanQuery call; considered stale after ~300 ms.
    struct ImgOverride {
        int image = -1;
        Rect region{0.0f, 0.0f, 1.0f, 1.0f};
        int thresholdPct = 0;
        int intervalMs = 0;
        bool wantMatches = false;  // mirror of the last ImageQuery.wantMatches
        int64_t lastQueryUs = 0;
    } imgOverride_{};

    // ---- image-match worker thread -------------------------------------------
    // The NCC match used to run on the UI thread inside automationTick — a
    // full-screen scan can take hundreds of ms and stall mouse input. Jobs are
    // queued here once the GPU readback lands; the worker runs the match and
    // posts a result the UI thread picks up next tick.
    struct ScanJob {
        int imageIndex = -1;
        render::Renderer::RegionPixels px;
        ImageRef ref;
        int thresholdPct = 0;
        bool wantMatches = false;
        uint32_t vw = 0, vh = 0;
        uint64_t requestId = 0;
        // Object detection job (isDetect -> runs the ONNX model instead of NCC).
        bool isDetect = false;
        std::vector<int> detectClasses;  // model ids; empty = all classes
        float detectConf = 0.5f;
        int detectTarget = 0;  // result mailbox: 0 = detectProbe_, 1 = preview, 2 = watch
        uint64_t frameId = 0;  // vision pipeline stamp: which captured frame
        int64_t captureUs = 0; // readback timestamp = source-frame stamp
        Rect detectRegion{};   // the scanned frame region (for exclusion math)
        float detectFloor = 0.5f;  // raw threshold passed to the model (<= detectConf)
        bool detectExclOn = false;
        Rect detectExcl{};
        bool detectMateOn = false;
        float mateSat = 60.0f;
        float mateMinRatio = 0.03f;
        bool detectRedOn = false;
        float redMinRatio = 0.02f;
        float redBoost = 0.20f;
        int selfIdx = -1;         // model class id of "self" (-1 = model has none)
        bool selfAutoAdded = false;  // self was included for suppression only, never emitted
    };
    struct ScanWorkerResult {
        std::atomic<bool> ready{false};
        uint64_t requestId = 0;
        bool found = false;
        float score = 0;
        ImageMatch match;
        std::vector<ImageMatch> matches;
        int64_t scanCostUs = 0;
    };
    struct DetectWorkerResult {
        std::atomic<bool> ready{false};
        uint64_t requestId = 0;
        std::vector<Detection> hits;  // normalized to the scanned REGION
        int64_t inferUs = 0;
        int64_t inferStartUs = 0;  // inference begin (worker clock)
        int64_t doneAtUs = 0;   // worker completion timestamp (mailbox wait probe)
        uint64_t frameId = 0;
        int64_t captureUs = 0;
        bool preview = false;
    };
    std::thread scanWorkerThread_;
    std::atomic<bool> scanWorkerRunning_{false};
    std::mutex scanMu_;
    std::condition_variable scanCv_;
    std::queue<ScanJob> scanQueue_;
    std::array<ScanWorkerResult, kMaxImages> scanResults_;
    std::atomic<uint64_t> scanReqId_{0};
    // Detection mailboxes (one per probe so the worker never overwrites a result
    // the UI is still draining): worker fills, UI drains by requestId match.
    DetectWorkerResult detectResult_, detectPreviewResult_, detectWatchResult_;
    std::atomic<uint64_t> detectReqId_{0};
    bool detectWarmupQueued_ = false;  // lazy model load kicked once the inspector asks for classes
    Detector detector_;  // owned+used ONLY on the scan worker thread
    mutable std::mutex detectStatusMu_;
    std::string detectStatusText_ = "not loaded";
    std::vector<std::string> detectClassNames_;
    void startScanWorker();
    void stopScanWorker();
    void scanWorkerLoop();
    MacroRunner macros_{{[this](int a, int f, Vec2 n) { macroTouch(a, f, n); },
                         [this](int vk, bool d) { std::lock_guard lk(inputMu_); input_.onKey(vk, d, nowMicros(), InputSource::Macro); },
                         [this](int z) { return zonePresent(z); },
                         [this](int i) { return imagePresent(i); },
                         [this](int i, const ImageQuery& q) { return imageScanQuery(i, q); }}};
    std::vector<MonitorRuntime> monitorRt_;
    std::vector<ZoneScan> zoneScan_;
    std::vector<ImageScan> imgScan_;
    std::vector<std::pair<int, int64_t>> pendingKeyUps_;
    bool automationOn_ = false, zoneTest_ = false, imgTest_ = false;
    std::string automationStatus_;
    int selectedZone_ = -1, selectedMonitor_ = -1, selectedMacro_ = -1, selectedImage_ = -1;
    std::vector<int> selectedStepPath_;   // path to the single open step in the selected macro
    int macroEditorTab_ = 0;              // 0 = steps, 1 = macro settings
    int settingsTab_ = 0;                 // 0 = video, 1 = performance, 2 = logs, 3 = help
    int colorsTab_ = 0;                   // 0 = color zones, 1 = monitors
    char macroSearch_[64] = {};
    bool bindingAdvOpen_ = false;
    bool camSepSensY_ = false;  // camera inspector: separate X/Y sliders (auto-on when they differ)
    bool stepAdvOpen_ = false;
    bool imageAdvOpen_ = false;
    bool zoneAdvOpen_ = false;
    bool monitorAdvOpen_ = false;
    bool resizingPanel_ = false;
    int pickMode_ = 0, pickZone_ = -1, pickColor_ = -1, pickImage_ = -1;  // 1 color, 2 new zone, 3 step point, 4 capture image, 5 saved search region, 6 step search region, 7 swipe path (from->to)
    Vec2* pickTarget_ = nullptr;
    Vec2* pickTarget2_ = nullptr;  // pickMode 7: the swipe's end point
    Vec2 pickSaved1_{}, pickSaved2_{};  // pickMode 7 originals, restored on Esc
    int pickSwipeHandle_ = 0;           // pickMode 7: 1 = dragging start, 2 = dragging end
    studio::ColorVal* pickColorTarget_ = nullptr;   // pickMode 8: node color prop to overwrite
    std::string* pickColorList_ = nullptr;          // pickMode 8: "r,g,b;..." list to append to
    Rect* pickRectTarget_ = nullptr;   // pickMode 6: a step's custom search region
    int imgTestImage_ = -1;            // image the editor's "Test search" is watching
    bool pickPending_ = false, pickPendingImage_ = false;
    bool pickMulti_ = false;  // continuous color picking: keep listening until Esc / zone full
    Rect pickRect_{}, pickDragOrigin_{};  // drag-in-progress rect (window-normalized to the video)
    bool pickDragging_ = false;
    int imgHandle_ = 0;                   // 1 = drag search region, 2 = resize it
    Rect imgRegionStart_{};               // search region snapshot while dragging
    int* capExternal_ = nullptr;
    int* capExternal2_ = nullptr;  // second target for combo captures (macro trigger key2)  // capture slots 6 (step/monitor key) and 7 (trigger keys)
    uint64_t detections_ = 0;
    SampleWindow scanUs_{500};
    // Detection pipeline stage probes (audit): readback wait, worker inference,
    // worker-done -> UI drain, tracker cost, aim-math cost, and the measurement
    // age of a fresh hit set (readback request -> coords drained).
    SampleWindow detReadbackUs_{500};
    SampleWindow detInferUs_{500};
    SampleWindow detMailUs_{500};
    SampleWindow detTrackUs_{2000};
    SampleWindow detAimUs_{2000};
    SampleWindow detEndToEndUs_{500};
    RateCounter scanRate_;

    Settings settings_;
    Profile profile_;
    std::vector<std::string> profileNames_;
    char newProfileName_[64] = {};  // unused; superseded by namePrompt_.buf
    std::string status_;

    SampleWindow presentLatencyUs_{2000};  // packet received -> frame presented
    SampleWindow inputProcUs_{2000};       // T0 OS input arrival -> T2 control command queued
    SampleWindow renderUs_{2000};          // T7 render submit -> T8 present complete
    FlightRecorder flight_;                // bounded ring of the last input/queue events
    std::atomic<int64_t> inputT0Us_{0};    // T0 of the input event currently being processed
    uint64_t lastControlDropped_ = 0, lastControlStale_ = 0;
    std::size_t lastFingerReject_ = 0;     // last seen finger-budget rejection count
    int64_t rejectToastUs_ = 0;            // throttle for the touch-limit toast
    RateCounter presentFps_;
    uint64_t presented_ = 0;
    LatencyProbe probe_;
    int64_t lastCpuWall_ = 0;
    uint64_t lastCpuTime_ = 0;
    double cpuPercent_ = 0;
    double workingSetMb_ = 0;
};

}  // namespace wasd::app
