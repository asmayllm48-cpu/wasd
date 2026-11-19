// Turns keyboard/mouse state into multi-touch events according to a Profile.
// Pure logic: no OS calls, fully unit-testable with a fake sink.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "core/profile.h"
#include "core/protocol.h"

namespace wasd {

using TouchSink = std::function<void(const proto::TouchEvent&)>;

// Explicit identity for every input producer in NEXPLAY.
// Used for per-key and per-pointer reference counting / ownership so one source
// (e.g. a Macro or Monitor) never prematurely releases a key or button still held
// by another source (e.g. Physical Mouse/Keyboard).
enum class InputSource : uint8_t {
    Physical     = 1 << 0,  // Hardware keyboard & mouse buttons (WM_KEYDOWN / WM_LBUTTONDOWN)
    Macro        = 1 << 1,  // Visual macro engine (StepType::Key / StepType::Tap / Swipe)
    Monitor      = 1 << 2,  // Color / Image detection monitor auto-trigger
    ImageClick   = 1 << 3,  // Image recognition click action
    DirectMouse  = 1 << 4,  // Unlocked cursor direct screen touch
    Recoil       = 1 << 5,  // Continuous vertical/horizontal recoil compensation
};

enum class KeyLogicalState : uint8_t {
    Released = 0,
    Pressed = 1,
    ReleasePending = 2,
};

class InputEngine {
public:
    static constexpr uint64_t kBindingPointerBase = 0x1000;
    static constexpr uint64_t kCameraPointerId = 0x2000;
    static constexpr uint64_t kMacroPointerBase = 0x4000;
    static constexpr int kMaxMacroFingers = 8;
    static constexpr uint64_t kAutoFirePointerId = 0x3000;  // engine-owned auto-fire finger

    explicit InputEngine(TouchSink sink);

    void setProfile(const Profile& p);  // releases all active touches first
    const Profile& profile() const { return profile_; }
    void setFrameSize(uint32_t w, uint32_t h);  // releases all touches on change
    // Physical device display size in the frame's orientation. When set, camera
    // sensitivity means device pixels per mouse count (WASD+ parity): the delta
    // is scaled by frame/device so the same value moves the aim identically on
    // any stream resolution. 0/0 = legacy frame-pixel semantics.
    void setDeviceSize(uint32_t w, uint32_t h) { devW_ = w; devH_ = h; }

    // Multi-source key input with explicit source ownership.
    // A logical key transitions Released -> Pressed when the first source presses it,
    // and Pressed -> Released only when ALL sources holding it have released it.
    bool onKey(int vk, bool down, int64_t nowUs, InputSource src = InputSource::Physical);

    // Query logical state & active source bitmask for diagnostics and testing.
    KeyLogicalState keyState(int vk) const;
    uint8_t keySources(int vk) const;
    uint32_t activeSourcesMask() const;

    // Releases only synthetic states initiated by a specific source (e.g. when a macro stops),
    // preserving any keys/buttons still held by physical hardware or other active sources.
    void releaseSource(InputSource src, int64_t nowUs = 0);

    // Central macro / image-recognition touch routing (finger 0..kMaxMacroFingers-1).
    void onMacroTouch(proto::MotionAction action, int finger, Vec2 normalized, InputSource src = InputSource::Macro);

    // Locked mode: relative mouse motion drives the camera binding.
    void onMouseDelta(int dx, int dy, int64_t nowUs = 0);
    void setLocked(bool locked);
    bool locked() const { return locked_; }

    // Unlocked mode: mouse acts as a single finger at a normalized position.
    // The release is sent at the last down/move position (`normalized` is ignored on up).
    void directPointer(bool down, Vec2 normalized);
    void directMove(Vec2 normalized);

    void tick(int64_t nowUs);  // advances drag animations, recoil compensation, and rapid fire
    void releaseAll();
    std::size_t activeTouches() const;

    // Live multi-touch state for diagnostics & tests. Each active virtual finger is
    // reported with its stable pointer id, kind, position and owner-source mask.
    struct FingerInfo {
        uint64_t pointerId = 0;
        const char* kind = "";
        float x = 0, y = 0;
        uint8_t ownerMask = 0;
        int bindingIndex = -1;
    };
    std::vector<FingerInfo> fingerStates() const;
    int activeFingerCount() const;
    int maxFingers() const { return profile_.maxVirtualFingers; }
    bool consumeRecoilToggle(std::size_t& bindingIdx, bool& enabled);
    bool consumeRapidFireToggle(std::size_t& bindingIdx, bool& rapidFireOn);
    std::size_t sourceConflictsResolved() const { return conflictsResolved_; }
    // Camera-path diagnostics counters (aim-assist observability).
    uint64_t camTeleports() const { return camTeleports_; }
    uint64_t camBoundsClamps() const { return camBoundsClamps_; }
    // Presses rejected by the multi-touch finger budget; surfaced in the UI so a
    // silently dead key (or macro finger) is visible instead of looking stuck.
    std::size_t fingerRejections() const { return fingerRejections_; }
    // Diagnostic trace: logs joystick key-state/vector transitions to the app log.
    void setTrace(bool on) { trace_ = on; }

private:
    bool trace_ = false;
    struct DragState {
        bool active = false;
        int64_t startUs = 0;
        float x = 0, y = 0;
    };
    struct JoyState {
        bool active = false;
        float curX = 0, curY = 0;
        float targetX = 0, targetY = 0;   // ideal target (direction * radius)
        float wanderX = 0, wanderY = 0;    // slow drift added to ideal target
        float velX = 0, velY = 0;          // spring velocity for smooth accel/decel
        float randOx = 0, randOy = 0;      // instantaneous micro-jitter
        int64_t releaseAtUs = 0;
        int64_t nextStepUs = 0;
        int64_t bornUs = 0;                // touch-down time for ease-in
        int64_t lastEmitUs = 0;            // last Move emitted (non-humanized heartbeat)
        int64_t retryAtUs = 0;             // next retry when activation was budget-rejected
        uint32_t rng = 0x12345678u;
        // Natural profile waypoint path (joyProfile == 2): the finger steps along
        // a straight line pathSX->pathTX with eased progress, a short initial
        // delay, bounded timing jitter and bounded lateral offset. Progress along
        // the path is monotonic — the stick can never slide backwards.
        float pathSX = 0, pathSY = 0;      // path origin (center or retarget point)
        float pathTX = 0, pathTY = 0;      // path destination (ideal target)
        float latAmp = 0;                  // signed lateral amplitude for this path
        float latOff = 0;                  // smoothed lateral offset state
        float alongMax = 0;                // furthest along-path fraction emitted
        int64_t pathDurUs = 0;             // duration of the current path
        int64_t pathStartUs = 0;           // when stepping begins (bornUs + initial delay)
        bool converged = false;            // reached the exact target
    };
    struct FireState {
        bool held = false;
        bool fingerDown = false;
        int64_t startUs = 0;
        int64_t lastTickUs = 0;
        int64_t nextPulseUs = 0;
        float x = 0, y = 0;
    };
    struct MacroFingerState {
        bool active = false;
        uint8_t sources = 0;
        float x = 0, y = 0;
    };
    struct TapPulse {  // extra taps pending for a Tap binding with tapRepeat > 1
        int left = 0;
        int64_t nextUs = 0;
        bool downPhase = false;
    };
    void emit(proto::MotionAction a, uint64_t id, float fx, float fy, float pressure = 0.0f);
    void toFrame(Vec2 n, float& fx, float& fy) const;
    void updateJoystick(std::size_t idx, int64_t nowUs);
    void cameraDown(int64_t nowUs);
    // Presses a Fire binding's finger and, in locked mode, prepares the camera
    // finger for the recoil pull (land/recenter on the anchor + primed first step).
    void startFire(std::size_t i, int64_t nowUs);
    const Binding* findCamera() const;
    const Binding* findFire() const;
    void applyCameraPixelDelta(float fdx, float fdy, int64_t nowUs);
    void tickCamera(int64_t nowUs, const Binding* cam);   // smoothing drain, return-to-center
    void tickAutoFire(int64_t nowUs, const Binding* cam); // periodic fire pulses while aiming
    void stopAutoFire();                                  // ensures the auto-fire finger is lifted
    // Finger budget: counts every active virtual finger and rejects new presses
    // once Profile::maxVirtualFingers is reached (no partial Down/Up pairs).
    int usedFingerSlots() const;
    bool tryReserveFinger();
    // Slots kept free for essential producers (joystick bindings, camera, direct
    // pointer) so taps/macros/autofire can never starve movement or aiming.
    int reservedCoreSlots() const;
    bool tryReserveNonCoreFinger();
    bool reserveFireFinger();  // fire priority: evicts a non-essential finger when the budget is full
    void tapFingerDown(std::size_t i, int64_t nowUs);   // emits Down + arms repeat pulses
    void tapFingerUp(std::size_t i);                    // emits Up + cancels repeat pulses
    // Teleports the camera finger (Up at current pos, Down at target). Returning
    // to center MUST use this: a Move while the finger is down drags the camera.
    void camTeleport(float x, float y);
    // Lifts ONLY the finger of binding i (if active) - used by the surgical
    // setProfile that must not kill unrelated live fingers mid-game.
    void releaseBinding(std::size_t i);

    TouchSink sink_;
    Profile profile_;
    uint32_t frameW_ = 0, frameH_ = 0;
    uint32_t devW_ = 0, devH_ = 0;  // physical display dims (frame orientation)
    std::array<bool, 256> keys_{};
    std::array<uint8_t, 256> keySources_{};  // bitmask of InputSource holding each virtual key
    std::vector<bool> tapActive_;
    std::vector<TapPulse> tapPulses_;
    std::vector<int64_t> tapReleaseAtUs_;
    std::vector<int64_t> singleDeferUs_;  // >0 = single-key binding deferred (waiting for combo window)
    std::vector<bool> joyActive_;
    std::vector<JoyState> joys_;
    std::vector<DragState> drags_;
    std::vector<FireState> fires_;
    std::array<MacroFingerState, kMaxMacroFingers> macroFingers_{};
    std::size_t conflictsResolved_ = 0;
    std::size_t fingerRejections_ = 0;
    bool recoilToggled_ = false;
    std::size_t recoilToggledIdx_ = 0;
    bool rapidToggled_ = false;
    std::size_t rapidToggledIdx_ = 0;
    bool locked_ = false;
    bool camActive_ = false;
    float camX_ = 0, camY_ = 0;
    float camFracX_ = 0, camFracY_ = 0;  // sub-pixel accumulator for smooth recoil & mouse delta
    // Advanced camera feature state
    float camAvgX_ = 0, camAvgY_ = 0;        // pending smoothed deltas (leaky integrator)
    int64_t camAvgLastUs_ = 0;               // last raw delta timestamp feeding the integrator
    int64_t camAvgTickUs_ = 0;               // last tick that drained the integrator
    int64_t camLastMoveUs_ = 0;              // last real mouse motion (return-to-center delay)
    bool camRetActive_ = false;              // smooth return animation running
    int64_t camRetT0Us_ = 0;
    float camRetX_ = 0, camRetY_ = 0;
    // Camera-path diagnostics: teleports (ring recenter + return steps) and
    // hard-bounds clamps — a high count during aim assist means the pull is
    // fighting the finger's travel limits.
    uint64_t camTeleports_ = 0;
    uint64_t camBoundsClamps_ = 0;
    bool camFireDown_ = false;               // auto-fire finger currently held
    int64_t camFireStartUs_ = 0;
    int64_t camFireNextUs_ = 0;
    float camFireX_ = 0, camFireY_ = 0;
    bool directActive_ = false;
    float directX_ = 0, directY_ = 0;
    // The largest timestamp tick() has ever seen. The input thread and the UI
    // thread can both call tick(); a stale timestamp must clamp, never rewind
    // recoil/animation timers.
    int64_t lastTickNowUs_ = 0;
};

}  // namespace wasd
