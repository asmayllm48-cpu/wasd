// Per-game key mapping profile, stored as versioned JSON.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/automation.h"
#include "core/coords.h"

namespace wasd {

inline constexpr int kProfileSchemaVersion = 1;

// Virtual key codes for mouse-wheel directions. Windows has no real VK for the
// wheel, so these are unassigned codes we define; a wheel event is a pulse
// (down+up), never a held key.
inline constexpr int kVkWheelUp = 0x0A;
inline constexpr int kVkWheelDown = 0x0B;
inline constexpr int kVkWheelLeft = 0x0E;
inline constexpr int kVkWheelRight = 0x0F;
inline bool isWheelVk(int vk) { return vk == kVkWheelUp || vk == kVkWheelDown || vk == kVkWheelLeft || vk == kVkWheelRight; }

enum class BindingType { Tap, Joystick, Camera, Drag, Fire };

const char* bindingTypeName(BindingType t);

// Key codes are Windows virtual-key codes. Mouse buttons use VK_LBUTTON(1),
// VK_RBUTTON(2), VK_MBUTTON(4), VK_XBUTTON1(5), VK_XBUTTON2(6).
struct Binding {
    std::string name;
    BindingType type = BindingType::Tap;
    int key = 0;              // Tap / Drag / Fire primary trigger key
    int key2 = 0;             // Optional second key for Dual-Key Combo (e.g. H + T pressed together)
    bool disabled = false;    // Binding is ignored by the engine but kept in the profile
    bool quickTap = false;    // false = Hold while key is pressed, true = Quick Tap (auto-release after tapHoldMs)
    bool tapToggle = false;   // Tap: first press lands the finger, second press lifts it (Toggle Finger)
    int tapRepeat = 1;        // Tap: total number of taps emitted per activation (1 = single tap)
    int tapHoldMs = 45;       // Dwell time in ms when quickTap is true
    Vec2 pos{0.5f, 0.5f};     // normalized: Tap/Fire point, Joystick center, Camera anchor, Drag start
    Vec2 pos2{0.5f, 0.5f};    // Drag end
    int durationMs = 120;     // Drag duration
    int keyUp = 'W', keyDown = 'S', keyLeft = 'A', keyRight = 'D';  // Joystick
    float radius = 0.12f;     // Joystick reach / Camera recenter distance, fraction of frame height
    float radiusX = 0.0f;     // Camera: X half-extent; 0 = circle (mirrors radius). Lets the aim ring be an ellipse.
    // Advanced Joystick / Dpad parameters
    float joyUpOffset = 0.0f;       // 0.0 .. 0.50 (0% ~ 50% extra upward reach for sprint)
    float joyAngleDeg = 45.0f;      // 15 .. 75 degrees diagonal deflection angle
    int joyReleaseDelayMs = 0;      // 0 .. 200 ms delay before lifting finger when keys released
    int joyIntervalMs = 16;         // 16 .. 100 ms step update interval
    bool joyShiftEnabled = false;   // Enable Shift radius modifier (walk / sprint)
    int joyShiftKey = 0x10;         // VK_SHIFT default modifier key
    float joyShiftOffset = -0.12f;  // -0.45 .. +0.45 (-45% ~ +45% radius offset when Shift held)
    float joyStepLength = 0.10f;    // 0.01 .. 0.20 interpolation step fraction
    float joySpeed = 1.0f;          // 0.25 .. 4.00 human-like movement speed multiplier
    bool joyHumanize = false;       // legacy: same as joyProfile == 1 (kept for old layouts)
    bool joyShake = false;          // Enable periodic micro-shake while holding direction
    int joyShakePeriodMs = 100;     // 100 .. 500 ms shake period
    float joyShakeAmp = 0.10f;      // 0.1 .. 10.0 px shake amplitude
    float joyRandomOffset = 1.0f;   // 0.1 .. 1.0 random touch offset scale
    // Movement profile: 0 = Geometric (snap to target), 1 = Smooth spring (legacy
    // humanize), 2 = Natural (stepped waypoint path like typical mapper sticks).
    int joyProfile = 0;
    // Natural profile parameters. Down lands at the stick center; after
    // joyNatInitialMs the finger steps along the path at joyNatSegMs cadence and
    // converges to the target in ~joyNatDurMs. Timing and lateral variation are
    // bounded and deterministic (fixed rng seed per activation).
    int joyNatInitialMs = 12;       // 0 .. 60 ms: delay before the first step
    int joyNatSegMs = 14;           // 8 .. 60 ms: update cadence
    int joyNatDurMs = 90;           // 20 .. 400 ms: overall travel duration
    float joyNatSpacingPx = 0.0f;   // 0 (=auto from duration) .. 40 px: max per-step distance
    int joyNatTimeVarPct = 15;      // 0 .. 50: ±% cadence jitter
    float joyNatPathVarPx = 1.5f;   // 0 .. 10 px: max lateral offset mid-path
    int joyNatEase = 1;             // 0 = linear, 1 = ease-out, 2 = ease-in-out
    float sensitivity = 1.0f; // Camera: X sensitivity, frame pixels per mouse count
    float sensitivityY = 1.0f; // Camera: Y sensitivity (old profiles inherit `sensitivity`)
    bool invertY = false;     // Camera
    // Advanced mouse-look (Camera) options. All engine-backed; see InputEngine.
    bool camBoundsEnabled = false;  // clamp the aim pointer inside the bounds rectangle
    float camBoundsRange = 0.30f;   // bounds half-height, fraction of frame height (0.05..0.50)
    float camBoundsRangeX = 0.0f;   // bounds half-width, fraction of frame height (0 = same as camBoundsRange)
    int camAutoReleaseMs = 0;       // lift the aim finger after this much idle time (0 = never; WASD+ holds it)
    int camReturnType = 0;          // 0=None(edge snap only), 1=Instant, 2=Smooth, 3=Proportional(Type P)
    int camReturnDelayMs = 250;     // ms of mouse-idle before return-to-center starts
    int camAverageMs = 0;           // delta smoothing window in ms (0 = off, no added delay)
    bool camAutoFire = false;       // periodic fire on the Smart Fire anchor while aiming
    int camAutoFireReleaseMs = 50;  // auto-fire pulse period component: press hold & release gap (ms)
    std::string comment;            // optional user description for the binding
    // Smart Fire: Recoil compensation (0..100 fine-grained per direction) & Rapid Fire
    bool recoilEnabled = true;
    int recoilDown = 10;
    int recoilUp = 0;
    int recoilLeft = 0;
    int recoilRight = 0;
    int recoilDelayMs = 0;
    int recoilToggleKey = 0;
    bool rapidFire = false;
    int rapidFireDelayMs = 60;
    int rapidFireToggleKey = 0;  // Hotkey to switch between Continuous Fire (Recoil) and Rapid Fire (Pistol/DMR)
    bool operator==(const Binding&) const = default;
};

struct Profile {
    int schemaVersion = kProfileSchemaVersion;
    std::string name = "Default";
    std::string packageName;      // optional Android package the profile targets
    bool landscape = true;
    int lockToggleKey = 0xC0;     // VK_OEM_3 (`) toggles mouse-look lock
    int maxVirtualFingers = 16;   // Multi-touch budget: max simultaneous virtual fingers (1..16)
    std::vector<Binding> bindings;
    Automation automation;  // optional: color zones, monitors, macros
    bool operator==(const Profile&) const = default;
};

Profile makeDefaultProfile();

// Validation returns human readable problems; empty means valid.
std::vector<std::string> validateProfile(const Profile& p);

// JSON (de)serialization with migration from older schema versions.
bool profileFromJson(const std::string& text, Profile& out, std::string& err);
std::string profileToJson(const Profile& p);

// Writes atomically (temp file + rename) and keeps the previous file as .bak.
bool saveProfileFile(const std::string& path, const Profile& p, std::string& err);
bool loadProfileFile(const std::string& path, Profile& out, std::string& err);

std::string keyName(int vk);

}  // namespace wasd
