// Automation layer: color-detection zones, monitors and visual macros (IF/ELSE/LOOP).
// Pure logic, independent from the input engine; the app feeds frames, time and sinks.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/coords.h"
#include "core/studio_graph.h"

namespace wasd {

struct ColorTarget {
    std::string name;
    uint8_t r = 255, g = 0, b = 0;
    float tolerance = 12.0f;  // max CIELAB distance (Delta E 76)
    bool operator==(const ColorTarget&) const = default;
};

enum class ColorMatchMode { Any, All };

// Circular zone on the phone image: center normalized to the frame, radius as a fraction of frame height.
struct Zone {
    std::string name = "Zone";
    Vec2 center{0.5f, 0.5f};
    float radius = 0.04f;
    std::vector<ColorTarget> colors;
    ColorMatchMode mode = ColorMatchMode::Any;
    int minPixels = 6;      // per color
    float minRatio = 0.0f;  // fraction of zone pixels (0 = off)
    bool operator==(const Zone&) const = default;
};

// ImageSearch / ClickImage exist only so old profiles can be parsed; they are
// converted to ImageRecognize while loading (see automationFromJson) and are never
// created, serialized or executed by the current build.
enum class StepType { Wait, Tap, LongPress, PressDown, Release, Swipe, Key, RunMacro, If, Loop, Break, Stop, Comment,
                       ImageSearch, ClickImage, ImageRecognize };
enum class KeyMode { Press, Down, Up };
enum class LoopMode { Count, While, Until };

// Unified image-recognition action ------------------------------------------------
// What to search: `Step::image` picks the ImageRef; region/threshold/interval can be
// overridden per step (0 / Saved = inherit the image's own settings).
enum class ImageRegionMode { Saved, FullScreen, Custom };  // Saved = ImageRef::region
enum class MatchPolicy { Best, First, Nearest, All };      // which candidate is acted on
enum class ClickKind { Tap, LongPress, DoubleTap };
enum class ImageFoundAction {
    Continue,    // do nothing, go to the next step
    ClickMatch,  // click inside the detected bounding box (center or custom offset)
    ClickPoint,  // click a fixed frame point (Step::foundClick)
    RunMacro,    // run another macro (Step::foundMacro)
    RunSteps,    // execute the Step::then sequence
    JumpStep,    // jump to step Step::jumpTo in the same step list
    Stop         // finish the macro (completed)
};
enum class ImageNotFoundAction {
    Continue,     // go to the next step
    RetrySearch,  // reset the attempt counter and keep waiting (still bounded by timeout)
    RunSteps,     // execute the Step::otherwise sequence
    JumpStep,     // jump to step Step::jumpTo in the same step list
    Stop,         // finish the macro (completed)
    Fail          // fail the macro with an error
};

struct Step {
    StepType type = StepType::Wait;
    bool enabled = true;
    int ms = 100;             // wait / press / swipe duration, loop pause
    Vec2 a{0.5f, 0.5f}, b{0.6f, 0.5f};
    int finger = 0;           // 0..kMaxFingers-1
    int key = 0;
    KeyMode keyMode = KeyMode::Press;
    int macro = -1;           // RunMacro target
    int zone = -1;            // If / Loop condition
    int image = -1;           // If / Loop / ImageRecognize condition (overrides zone when >= 0)
    bool negate = false;      // condition: color/image NOT present
    LoopMode loopMode = LoopMode::Count;
    int count = 3;            // loop repetitions (Count) or max iterations (While/Until)
    std::string text;         // comment
    // ImageRecognize: `ms` is the search timeout (0 = a single immediate check),
    // `then`/`otherwise` are the ON FOUND / ON NOT FOUND action sequences.
    ImageFoundAction foundAction = ImageFoundAction::Continue;
    ImageNotFoundAction notFoundAction = ImageNotFoundAction::Continue;
    Vec2 foundClick{0.5f, 0.5f};  // fixed click point when foundAction == ClickPoint
    int foundMacro = -1;         // macro to run when foundAction == RunMacro
    int thresholdPct = 0;        // similarity override, 0 = ImageRef::thresholdPct
    ImageRegionMode regionMode = ImageRegionMode::Saved;
    Rect region{0.0f, 0.0f, 1.0f, 1.0f};  // search rect when regionMode == Custom
    int retryCount = 0;          // missed scans before ON NOT FOUND (0 = wait until timeout)
    int retryIntervalMs = 0;     // pause between search attempts (0 = scan cadence)
    bool continuous = false;     // keep re-firing the found action on reappearance until timeout
    int intervalMs = 0;          // scan cadence override, 0 = ImageRef::intervalMs
    MatchPolicy matchPolicy = MatchPolicy::Best;
    Vec2 nearTo{0.5f, 0.5f};     // anchor for the Nearest match policy
    ClickKind clickKind = ClickKind::Tap;
    Vec2 clickOfs{0.5f, 0.5f};   // offset inside the detected bbox when foundAction == ClickMatch
    int delayMs = 0;             // wait before clicking
    int pressMs = 60;            // hold duration (LongPress / each tap of DoubleTap)
    int jumpTo = -1;             // target step index for JumpStep actions
    std::vector<Step> then, otherwise;  // If branches; Loop body and image sequences
    bool operator==(const Step&) const = default;
};

enum class MacroRunMode { Once, Continuous, UntilFound, UntilFail };

struct Macro {
    std::string name = "Macro";
    int key = 0;              // trigger key (0 = only from monitors / other macros)
    int key2 = 0;             // optional second trigger key (dual-key combo, like bindings)
    int maxDurationMs = 60000;
    MacroRunMode runMode = MacroRunMode::Once;
    std::vector<Step> steps;
    bool operator==(const Macro&) const = default;
};

// One detection of a reference image inside a frame, normalized to the frame.
// Defined here (not in imgmatch.h) so the macro sinks can carry it.
struct ImageMatch {
    bool found = false;
    Vec2 center{};    // center of the best match, normalized to the frame
    Rect rect{};      // matched rectangle normalized to the frame
    float score = 0;  // NCC of the best match (0 when not found)
};

// A saved screen crop ("did this appear on the phone?"). The crop is stored in the
// data folder (w x h RGB24 pointed to by `file`); everything else is profile metadata.
// region / clickOffset are normalized to the phone frame, so resolution changes are safe.
inline constexpr int kMaxImages = 20;  // bounded by shared renderer readback slots (see app/automation_runtime.cpp)
struct ImageRef {
    std::string name = "Image";
    uint32_t w = 0, h = 0;      // crop size in pixels
    std::vector<uint8_t> rgb;   // w * h * 3 bytes (loaded from `file`)
    int thresholdPct = 85;      // gray NCC in percent (NOT a statistical probability)
    Rect region{0.0f, 0.0f, 1.0f, 1.0f};  // search area, normalized to the frame
    Vec2 clickOffset{0.5f, 0.5f};         // ClickImage point inside the matched crop
    int intervalMs = 150;                 // scan cadence when active
    std::string file;                     // file name inside the images data folder
    bool showOverlay = true;
    // Pixel data is excluded: `rgb` is a cache of the eagerly-written file, so
    // dirty-checking a Profile must not memcmp megabytes of pixels every frame.
    bool operator==(const ImageRef& o) const {
        return name == o.name && w == o.w && h == o.h && thresholdPct == o.thresholdPct && region == o.region &&
               clickOffset == o.clickOffset && intervalMs == o.intervalMs && file == o.file &&
               showOverlay == o.showOverlay;
    }
};

enum class TriggerMode { Once, Continuous, Rearm, Limited };

struct Monitor {
    std::string name = "Monitor";
    bool enabled = true;
    int zone = 0;
    int image = -1;           // image monitor overrides the color-zone source when >= 0
    int intervalMs = 50;
    int confirm = 2;          // consecutive samples before the state changes (debounce)
    int cooldownMs = 300;
    int delayMs = 0;          // wait after detection before acting
    TriggerMode mode = TriggerMode::Rearm;
    int maxRuns = 5;          // Limited
    int macro = -1;           // action: run macro...
    int key = 0;              // ...or press a key
    int toggleKey = 0;        // hotkey that arms/disarms this rule on its own (0 = none)
    // Absence branch ("when NOT found"): after the target stays missing for
    // absentMs, fire elseMacro/elseKey every elseCooldownMs, or disarm the rule.
    int elseMacro = -1, elseKey = 0;
    int absentMs = 2000, elseCooldownMs = 2000;
    bool elseStop = false;    // disarm instead of running an action
    bool operator==(const Monitor&) const = default;
};

// Standalone continuous object detection (the "Detect" section): the app re-scans
// the video on `intervalMs` and draws live bounding boxes. Whether it is watching
// right now is runtime state in the app — these are just the saved preferences.
struct DetectWatch {
    int startKey = 0;           // hotkey to start watching (0 = none)
    int stopKey = 0;            // hotkey to stop (0 = startKey toggles)
    int intervalMs = 80;        // rescan cadence; the model's own latency is the floor
    int confPct = 30;           // minimum confidence in percent
    Rect region{0.0f, 0.0f, 1.0f, 1.0f};  // search area, normalized to the frame
    std::vector<std::string> classes;     // class-name filter; empty = all classes
    bool drawBoxes = true;
    bool drawLabels = true;
    bool showRegion = false;    // draw the search-region frame on the video
    bool autostart = false;     // arm automatically once video is connected
    // Filters applied to raw detections before they reach the overlay/graphs.
    bool exclOn = true;                        // ignore detections inside `excl` (own character)
    Rect excl{0.05f, 0.20f, 0.42f, 0.80f};     // normalized frame rect, covers the player area
    bool showExcl = false;                     // draw the exclusion frame
    bool mateOn = true;                        // drop hits with a teammate marker above the box
    int mateSat = 60;                          // min channel spread to count as a colored marker
    int mateMinPct = 3;                        // % of marker-band pixels required to suppress
    bool redOn = true;                         // a red outline rescues sub-threshold hits
    int redMinPct = 2;                         // % of edge-ring pixels that must look red
    int redBoostPct = 20;                      // confidence bonus added to red-outlined hits
    // Tracker tuning (sticky boxes between scans).
    int trackLowPct = 55;        // % of confPct that keeps an EXISTING track alive (continuation floor)
    int trackPosBlendPct = 55;   // % weight of each fresh measurement — higher = snappier, lower = smoother
    int trackVelPct = 45;        // % weight of the new velocity sample — higher = reacts faster to motion
    int trackPredictMs = 350;    // cap on velocity glide between scans
    int trackMaxMissed = 2;      // scans a box may miss before it is dropped
    int trackMaxAgeMs = 600;     // absolute lifetime of an unmatched box
    // Aim assist: while aimKey is held, steer the camera toward the tracked
    // enemy via synthetic mouse deltas — the same sensitivity/smoothing path
    // as the user's own hand (only works while mouse look is locked).
    bool aimOn = false;
    int aimKey = 0;           // hold-to-assist VK (0 = disabled)
    int aimSpeedPct = 50;     // pull strength of each correction step
    int aimRangePct = 40;     // % of frame from the crosshair that acquires a target
    int aimLeadMs = 80;       // velocity lead on moving targets
    int aimBonePct = 30;      // aim point inside the box: 0 = top/head, 100 = feet
    int aimTargetMode = 0;    // 0 = nearest to crosshair, 1 = largest box
    // Vision pipeline v2: inference scheduling, fusion, selection, lock, point,
    // control limits. Defaults keep legacy behaviour identical.
    int inferFps = 0;         // analysis rate preset: 0 = Auto, else 15/20/30/45/60
    bool fusionOn = true;     // merge contained/duplicate hits into one character
    int selPreset = 0;        // 0 Center, 1 Size, 2 Confidence, 3 Balanced
    int selCenterW = 100, selSizeW = 0, selConfW = 25;   // custom weights (used when selPreset==4)
    int switchHystPct = 15;   // challenger must beat current score by this %
    int lockGraceMs = 150;    // TempLost grace before the lock identity is released
    int pointPreset = 2;      // 0 Center,1 Head,2 Chest,3 Upper body,4 Custom
    int pointX = 50, pointY = 32;   // custom rel point inside box, percent
    int ctrlDeadzonePx = 1;   // extra absolute deadband, px
    int ctrlInnerPct = 22;    // stop zone = character box inset by this % each side
    int ctrlMaxOut = 3000;    // pull speed cap, emitted counts/s
    int ctrlMaxRateHz = 200;  // control output rate limit
    int ctrlMaxAgeMs = 400;   // refuse measurements older than this
    int ctrlMinConfPct = 15;  // refuse measurements below this conf
    // Debug overlay (testing only): raw vs smoothed boxes, ids, perf panel.
    bool dbgRaw = false, dbgIds = false, dbgPerf = false, dbgRecord = false;
    bool operator==(const DetectWatch&) const = default;
};

struct Automation {
    std::vector<Zone> zones;
    std::vector<ImageRef> images;
    std::vector<Macro> macros;
    std::vector<Monitor> monitors;
    std::vector<studio::AutomationGraph> studioGraphs;  // NEXPLAY Automation Studio visual graphs
    DetectWatch detect;
    int toggleKey = 0x77;     // F8 starts/stops monitors and stops running macros
    bool operator==(const Automation&) const = default;
};

inline constexpr int kMaxFingers = 5;
inline constexpr int kMaxSteps = 500;       // per list
inline constexpr int kMaxDepth = 8;         // nested If/Loop/RunMacro
inline constexpr int kMaxLoopCount = 10000;

std::vector<std::string> validateAutomation(const Automation& a);
std::string automationToJson(const Automation& a);   // JSON object text
// Parses automation JSON. Legacy "Image search"/"Click image" steps are converted to
// the unified ImageRecognize action; when `migrated` is given, one human-readable note
// is appended per converted step so the change is reported, not silent.
bool automationFromJson(const std::string& text, Automation& out, std::string& err,
                        std::vector<std::string>* migrated = nullptr);

// ---- color matching ----
struct Lab {
    float L = 0, a = 0, b = 0;
};
Lab rgbToLab(uint8_t r, uint8_t g, uint8_t b);
float deltaE(const Lab& x, const Lab& y);

// RGB24 image (the zone's bounding box read from the video frame).
struct RgbView {
    const uint8_t* data = nullptr;
    int width = 0, height = 0, stride = 0;
};
struct ZoneMatch {
    int total = 0;                 // pixels inside the circle
    std::vector<int> perColor;     // matching pixels per target color
    bool present = false;
};
// cx, cy, r in pixels of the view. Only pixels inside the circle are counted.
ZoneMatch matchZone(const RgbView& img, float cx, float cy, float r, const Zone& z);

// Reference-image matching lives in core/imgmatch.h: matchImage(frameView, ImageRef).

// ---- monitor trigger logic (debounce, cooldown, modes) ----
class MonitorRuntime {
public:
    void reset() { *this = MonitorRuntime{}; }
    // Feed one sample; returns true when the action must start now.
    bool onSample(bool detected, int64_t nowUs, const Monitor& m, bool actionBusy);
    // After a sample reports absence: returns true when the else-branch must run.
    // elseStop is handled by the caller (it flips switchedOn).
    bool onAbsent(int64_t nowUs, const Monitor& m);
    bool present() const { return present_; }
    int runs() const { return runs_; }
    int64_t lastDetectUs() const { return lastDetect_; }
    bool finished() const { return finished_; }
    // User-facing switch driven by Monitor.toggleKey: independent of the global
    // automation toggle, so a single rule can run alone.
    bool switchedOn = false;

private:
    bool present_ = false, armed_ = true, finished_ = false;
    int hits_ = 0, misses_ = 0, runs_ = 0;
    int64_t lastFire_ = INT64_MIN / 2, pendingAt_ = -1, lastDetect_ = 0;
    int64_t absentSince_ = -1, lastElseFire_ = INT64_MIN / 2;
};

// ---- macro execution ----
enum class RunState { Idle, Running, Waiting, Completed, Cancelled, Failed };
const char* runStateName(RunState s);

// What an ImageRecognize step asks the scan engine for, and what it gets back.
// The query may override the ImageRef's own region/threshold/cadence; the app scans
// with the requested parameters and reports which parameters the last scan used.
struct ImageQuery {
    Rect region{0.0f, 0.0f, 1.0f, 1.0f};
    int thresholdPct = 85;
    int intervalMs = 150;
    bool wantMatches = false;  // also report the candidate list (Nearest / All policies)
};
struct ImageScanResult {
    bool available = false;    // a scan completed recently enough to trust
    bool paramsMatch = false;  // the scan ran with the requested region & threshold
    bool found = false;
    float score = 0;
    ImageMatch match;
    std::vector<ImageMatch> matches;  // best-first candidates (only when requested)
    int64_t scanUs = 0;        // duration of the last scan
};

// Effective search parameters of an ImageRecognize step: per-step overrides applied
// on top of the referenced ImageRef. Shared by the runner and the editor's test scan.
ImageQuery imageQueryFor(const Step& s, const Automation& a);

struct MacroSinks {
    std::function<void(int action /*0 down,1 up,2 move*/, int finger, Vec2 n)> touch;
    std::function<void(int vk, bool down)> key;
    std::function<std::optional<bool>(int zone)> zonePresent;  // nullopt: no frame available
    std::function<std::optional<bool>(int image)> imagePresent;
    std::function<ImageScanResult(int image, const ImageQuery&)> imageScan;
};

class MacroRunner {
public:
    explicit MacroRunner(MacroSinks s) : sinks_(std::move(s)) {}
    bool start(const Automation& a, int macro, int64_t nowUs);
    void tick(int64_t nowUs);
    void cancel(const char* reason = "cancelled");
    RunState state() const { return state_; }
    bool active() const { return state_ == RunState::Running || state_ == RunState::Waiting; }
    int macro() const { return macro_; }
    uint64_t epoch() const { return epoch_; }
    const std::string& message() const { return message_; }
    int executedSteps() const { return executed_; }
    const Step* currentStep() const { return current_; }

private:
    struct Frame {
        const std::vector<Step>* list = nullptr;
        size_t index = 0;
        const Step* loop = nullptr;  // owning loop step (body frames)
        int iteration = 0;
    };
    bool stepOnce(int64_t now, uint64_t runEpoch);  // returns false when it must wait
    void fail(const std::string& why);
    void releaseAll();
    void finish(RunState s, const std::string& msg);
    std::optional<bool> condition(const Step& s);
    // Shared step helpers (epoch-guarded like the old step-local lambdas).
    bool waitFor(int64_t t);     // -> RunState::Waiting until t
    bool advance();              // -> next step in the current frame
    void emitTouch(int action, int fi, Vec2 p);
    void emitKey(int vk, bool down);
    // ImageRecognize: query building, match selection, branch & click sub-machines.
    ImageQuery imgQuery(const Step& s) const;
    int64_t imgPollUs(const Step& s) const;
    const ImageMatch* pickMatch(const Step& s, const ImageScanResult& r) const;
    Vec2 clickPointFor(const Step& s) const;
    bool imageStepDone(const Step& s, int64_t now);   // after the found action finished
    bool imageFoundBranch(Frame& f, const Step& s, int64_t now);
    bool imageNotFoundBranch(Frame& f, const Step& s, int64_t now);
    bool clickTick(const Step& s, int64_t now);       // click sub-machine (phase 2)

    MacroSinks sinks_;
    Automation automation_;
    std::vector<Frame> stack_;
    RunState state_ = RunState::Idle;
    uint64_t epoch_ = 0;
    int macro_ = -1, executed_ = 0, phase_ = 0;
    int64_t startUs_ = 0, wakeUs_ = 0, phaseStartUs_ = 0, lastTickUs_ = 0;
    int maxDurationMs_ = 60000;
    MacroRunMode runMode_ = MacroRunMode::Once;
    int runCount_ = 0;
    bool lastFound_ = false;
    const Step* current_ = nullptr;
    // ImageRecognize runtime state (reset with phase_)
    uint64_t runEpoch_ = 0;      // epoch of the currently executing tick
    int imgAttempts_ = 0;        // missed scans so far in this search
    int imgSub_ = 0;             // click sub-machine state
    bool imgTap2_ = false;       // second tap of a DoubleTap pending
    size_t imgCandIdx_ = 0;      // next candidate for the All match policy
    std::vector<ImageMatch> imgCands_;  // candidates queued for the All policy
    ImageMatch imgMatch_;        // last positive detection (drives the click point)
    bool fingerDown_[kMaxFingers] = {};
    Vec2 fingerPos_[kMaxFingers] = {};
    bool swipeOwnsFinger_ = false;
    std::vector<int> keysDown_;
    std::string message_;
};

}  // namespace wasd
