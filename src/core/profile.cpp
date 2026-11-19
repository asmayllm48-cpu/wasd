#include "core/profile.h"
#include "core/log.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace wasd {
namespace fs = std::filesystem;
using nlohmann::json;

const char* bindingTypeName(BindingType t) {
    switch (t) {
        case BindingType::Tap: return "tap";
        case BindingType::Joystick: return "joystick";
        case BindingType::Camera: return "camera";
        case BindingType::Drag: return "drag";
        case BindingType::Fire: return "fire";
    }
    return "tap";
}

static bool parseType(const std::string& s, BindingType& t) {
    for (BindingType c : {BindingType::Tap, BindingType::Joystick, BindingType::Camera, BindingType::Drag, BindingType::Fire}) {
        if (s == bindingTypeName(c)) {
            t = c;
            return true;
        }
    }
    return false;
}

Profile makeDefaultProfile() {
    Profile p;
    p.name = "Default";
    Binding joy;
    joy.name = "Move";
    joy.type = BindingType::Joystick;
    joy.pos = {0.18f, 0.72f};
    joy.radius = 0.12f;
    p.bindings.push_back(joy);
    Binding cam;
    cam.name = "Camera";
    cam.type = BindingType::Camera;
    cam.pos = {0.70f, 0.45f};
    cam.radius = 0.25f;
    cam.sensitivity = 1.0f;
    p.bindings.push_back(cam);
    Binding fire;
    fire.name = "Fire";
    fire.type = BindingType::Tap;
    fire.key = 0x01;  // left mouse button
    fire.pos = {0.85f, 0.65f};
    p.bindings.push_back(fire);
    Binding jump;
    jump.name = "Jump";
    jump.type = BindingType::Tap;
    jump.key = 0x20;  // space
    jump.pos = {0.92f, 0.80f};
    p.bindings.push_back(jump);
    return p;
}

static bool inUnit(Vec2 v) { return v.x >= 0 && v.x <= 1 && v.y >= 0 && v.y <= 1; }

std::vector<std::string> validateProfile(const Profile& p) {
    std::vector<std::string> errs;
    if (p.name.empty()) errs.push_back("profile name is empty");
    int cameras = 0;
    std::vector<int> usedKeys;
    auto useKey = [&](int k, const std::string& who) {
        if (k <= 0 || k > 0xFE) {
            errs.push_back(who + ": invalid key code " + std::to_string(k));
            return;
        }
        for (int u : usedKeys)
            if (u == k) errs.push_back(who + ": key " + keyName(k) + " is bound more than once");
        usedKeys.push_back(k);
    };
    if (p.lockToggleKey) useKey(p.lockToggleKey, "lock toggle");
    if (!(p.automation == Automation{})) {
        if (p.automation.toggleKey) useKey(p.automation.toggleKey, "automation toggle");
        for (const Macro& m : p.automation.macros)
            if (m.key) useKey(m.key, "macro " + m.name);
        for (auto& e : validateAutomation(p.automation)) errs.push_back(e);
    }
    for (std::size_t i = 0; i < p.bindings.size(); ++i) {
        const Binding& b = p.bindings[i];
        const std::string who = "binding " + std::to_string(i) + " (" + b.name + ")";
        if (!inUnit(b.pos)) errs.push_back(who + ": position outside [0,1]");
        auto checkComboOrSingle = [&]() {
            if (b.key2 != 0) {
                if (b.key <= 0 || b.key > 0xFE || b.key2 <= 0 || b.key2 > 0xFE || b.key == b.key2)
                    errs.push_back(who + ": invalid combo keys");
            } else {
                useKey(b.key, who);
            }
        };
        switch (b.type) {
            case BindingType::Tap:
                checkComboOrSingle();
                break;
            case BindingType::Drag:
                checkComboOrSingle();
                if (!inUnit(b.pos2)) errs.push_back(who + ": end position outside [0,1]");
                if (b.durationMs < 0 || b.durationMs > 5000) errs.push_back(who + ": duration must be 0..5000 ms");
                break;
            case BindingType::Joystick:
                for (int k : {b.keyUp, b.keyDown, b.keyLeft, b.keyRight}) useKey(k, who);
                if (b.radius <= 0 || b.radius > 0.5f) errs.push_back(who + ": radius must be in (0, 0.5]");
                break;
            case BindingType::Camera:
                ++cameras;
                if (b.radius <= 0 || b.radius > 0.5f) errs.push_back(who + ": radius must be in (0, 0.5]");
                if (b.sensitivity <= 0 || b.sensitivity > 50) errs.push_back(who + ": sensitivity must be in (0, 50]");
                if (b.sensitivityY <= 0 || b.sensitivityY > 50) errs.push_back(who + ": sensitivityY must be in (0, 50]");
                if (b.camBoundsRange <= 0 || b.camBoundsRange > 0.5f) errs.push_back(who + ": camBoundsRange must be in (0, 0.5]");
                if (b.camBoundsRangeX < 0 || b.camBoundsRangeX > 0.5f)
                    errs.push_back(who + ": camBoundsRangeX must be in [0, 0.5]");
                if (b.camAutoReleaseMs < 0 || b.camAutoReleaseMs > 5000)
                    errs.push_back(who + ": camAutoReleaseMs must be in [0, 5000]");
                if (b.camReturnType < 0 || b.camReturnType > 3) errs.push_back(who + ": camReturnType must be 0..3");
                break;
            case BindingType::Fire:
                checkComboOrSingle();
                if (b.recoilToggleKey) useKey(b.recoilToggleKey, who + " recoil toggle");
                if (b.rapidFireToggleKey) useKey(b.rapidFireToggleKey, who + " rapid fire toggle");
                if (b.recoilDown < 0 || b.recoilDown > 100 || b.recoilUp < 0 || b.recoilUp > 100 ||
                    b.recoilLeft < 0 || b.recoilLeft > 100 || b.recoilRight < 0 || b.recoilRight > 100)
                    errs.push_back(who + ": recoil speeds must be 0..100");
                if (b.recoilDelayMs < 0 || b.recoilDelayMs > 5000)
                    errs.push_back(who + ": recoil delay must be 0..5000 ms");
                if (b.rapidFireDelayMs < 10 || b.rapidFireDelayMs > 2000)
                    errs.push_back(who + ": rapid fire delay must be 10..2000 ms");
                break;
        }
    }
    if (cameras > 1) errs.push_back("only one camera binding is supported");
    if (p.maxVirtualFingers < 1 || p.maxVirtualFingers > 16) errs.push_back("maxVirtualFingers must be in 1..16");
    return errs;
}

static json vecToJson(Vec2 v) { return json::array({v.x, v.y}); }
static Vec2 vecFromJson(const json& j) { return Vec2{j.at(0).get<float>(), j.at(1).get<float>()}; }

std::string profileToJson(const Profile& p) {
    json j;
    j["schemaVersion"] = kProfileSchemaVersion;
    j["name"] = p.name;
    j["packageName"] = p.packageName;
    j["landscape"] = p.landscape;
    j["lockToggleKey"] = p.lockToggleKey;
    if (p.maxVirtualFingers != 16) j["maxVirtualFingers"] = p.maxVirtualFingers;
    json arr = json::array();
    for (const Binding& b : p.bindings) {
        json o;
        o["name"] = b.name;
        o["type"] = bindingTypeName(b.type);
        o["pos"] = vecToJson(b.pos);
        if (!b.comment.empty()) o["comment"] = b.comment;
        if (b.disabled) o["disabled"] = b.disabled;
        switch (b.type) {
            case BindingType::Tap:
                o["key"] = b.key;
                if (b.key2) o["key2"] = b.key2;
                if (b.quickTap) o["quickTap"] = b.quickTap, o["tapHoldMs"] = b.tapHoldMs;
                if (b.tapToggle) o["tapToggle"] = b.tapToggle;
                if (b.tapRepeat != 1) o["tapRepeat"] = b.tapRepeat;
                break;
            case BindingType::Drag:
                o["key"] = b.key;
                if (b.key2) o["key2"] = b.key2;
                o["pos2"] = vecToJson(b.pos2);
                o["durationMs"] = b.durationMs;
                break;
            case BindingType::Joystick:
                o["keys"] = {{"up", b.keyUp}, {"down", b.keyDown}, {"left", b.keyLeft}, {"right", b.keyRight}};
                o["radius"] = b.radius;
                if (b.joyUpOffset != 0.0f) o["joyUpOffset"] = b.joyUpOffset;
                if (b.joyAngleDeg != 45.0f) o["joyAngleDeg"] = b.joyAngleDeg;
                if (b.joyReleaseDelayMs != 0) o["joyReleaseDelayMs"] = b.joyReleaseDelayMs;
                if (b.joyIntervalMs != 16) o["joyIntervalMs"] = b.joyIntervalMs;
                if (b.joyShiftEnabled) {
                    o["joyShiftEnabled"] = b.joyShiftEnabled;
                    o["joyShiftKey"] = b.joyShiftKey;
                    o["joyShiftOffset"] = b.joyShiftOffset;
                }
                if (b.joyStepLength != 0.10f) o["joyStepLength"] = b.joyStepLength;
                if (b.joySpeed != 1.0f) o["joySpeed"] = b.joySpeed;
                if (b.joyProfile != 0) o["joyProfile"] = b.joyProfile;
                if (b.joyProfile == 1 || (b.joyProfile == 0 && b.joyHumanize)) {
                    o["joyHumanize"] = b.joyHumanize || b.joyProfile == 1;
                    o["joyShake"] = b.joyShake;
                    o["joyShakePeriodMs"] = b.joyShakePeriodMs;
                    o["joyShakeAmp"] = b.joyShakeAmp;
                    o["joyRandomOffset"] = b.joyRandomOffset;
                }
                if (b.joyProfile == 2) {
                    o["joyNatInitialMs"] = b.joyNatInitialMs;
                    o["joyNatSegMs"] = b.joyNatSegMs;
                    o["joyNatDurMs"] = b.joyNatDurMs;
                    o["joyNatSpacingPx"] = b.joyNatSpacingPx;
                    o["joyNatTimeVarPct"] = b.joyNatTimeVarPct;
                    o["joyNatPathVarPx"] = b.joyNatPathVarPx;
                    o["joyNatEase"] = b.joyNatEase;
                }
                break;
            case BindingType::Camera:
                o["radius"] = b.radius;
                if (b.radiusX > 0.0f) o["radiusX"] = b.radiusX;
                o["sensitivity"] = b.sensitivity;
                o["sensitivityY"] = b.sensitivityY;
                o["invertY"] = b.invertY;
                if (b.camBoundsEnabled) {
                    o["camBoundsEnabled"] = true;
                    o["camBoundsRange"] = b.camBoundsRange;
                    if (b.camBoundsRangeX > 0.0f) o["camBoundsRangeX"] = b.camBoundsRangeX;
                }
                if (b.camAutoReleaseMs != 0) o["camAutoReleaseMs"] = b.camAutoReleaseMs;
                if (b.camReturnType != 0) {
                    o["camReturnType"] = b.camReturnType;
                    o["camReturnDelayMs"] = b.camReturnDelayMs;
                }
                if (b.camAverageMs != 0) o["camAverageMs"] = b.camAverageMs;
                if (b.camAutoFire) {
                    o["camAutoFire"] = true;
                    o["camAutoFireReleaseMs"] = b.camAutoFireReleaseMs;
                }
                break;
            case BindingType::Fire:
                o["key"] = b.key;
                if (b.key2) o["key2"] = b.key2;
                o["recoilEnabled"] = b.recoilEnabled;
                o["recoilDown"] = b.recoilDown;
                o["recoilUp"] = b.recoilUp;
                o["recoilLeft"] = b.recoilLeft;
                o["recoilRight"] = b.recoilRight;
                o["recoilDelayMs"] = b.recoilDelayMs;
                o["recoilToggleKey"] = b.recoilToggleKey;
                o["rapidFire"] = b.rapidFire;
                o["rapidFireDelayMs"] = b.rapidFireDelayMs;
                o["rapidFireToggleKey"] = b.rapidFireToggleKey;
                break;
        }
        arr.push_back(std::move(o));
    }
    j["bindings"] = std::move(arr);
    if (!(p.automation == Automation{})) j["automation"] = json::parse(automationToJson(p.automation));
    return j.dump(2);
}

// v0 (pre-release drafts) stored bindings under "keys" and had no schemaVersion.
static void migrate(json& j) {
    int v = j.value("schemaVersion", 0);
    if (v == 0) {
        if (j.contains("keys") && !j.contains("bindings")) {
            j["bindings"] = j["keys"];
            j.erase("keys");
        }
        j["schemaVersion"] = 1;
    }
}

bool profileFromJson(const std::string& text, Profile& out, std::string& err) {
    try {
        json j = json::parse(text);
        if (!j.is_object()) {
            err = "profile must be a JSON object";
            return false;
        }
        migrate(j);
        const int v = j.at("schemaVersion").get<int>();
        if (v > kProfileSchemaVersion) {
            err = "profile schemaVersion " + std::to_string(v) + " is newer than supported (" +
                  std::to_string(kProfileSchemaVersion) + ")";
            return false;
        }
        Profile p;
        p.name = j.value("name", std::string("Unnamed"));
        p.packageName = j.value("packageName", std::string());
        p.landscape = j.value("landscape", true);
        p.lockToggleKey = j.value("lockToggleKey", 0xC0);
        p.maxVirtualFingers = std::clamp(j.value("maxVirtualFingers", 16), 1, 16);
        for (const json& o : j.value("bindings", json::array())) {
            Binding b;
            b.name = o.value("name", std::string());
            if (!parseType(o.at("type").get<std::string>(), b.type)) {
                err = "unknown binding type: " + o.at("type").get<std::string>();
                return false;
            }
            b.pos = vecFromJson(o.at("pos"));
            b.key = o.value("key", 0);
            b.key2 = o.value("key2", 0);
            b.disabled = o.value("disabled", false);
            b.quickTap = o.value("quickTap", false);
            b.tapToggle = o.value("tapToggle", false);
            b.tapRepeat = std::clamp(o.value("tapRepeat", 1), 1, 20);
            b.tapHoldMs = std::clamp(o.value("tapHoldMs", 45), 15, 1000);
            if (o.contains("pos2")) b.pos2 = vecFromJson(o["pos2"]);
            b.durationMs = o.value("durationMs", 120);
            if (o.contains("keys")) {
                const json& k = o["keys"];
                b.keyUp = k.value("up", int('W'));
                b.keyDown = k.value("down", int('S'));
                b.keyLeft = k.value("left", int('A'));
                b.keyRight = k.value("right", int('D'));
            }
            b.radius = o.value("radius", 0.12f);
            b.radiusX = std::clamp(o.value("radiusX", 0.0f), 0.0f, 0.50f);
            b.joyUpOffset = std::clamp(o.value("joyUpOffset", 0.0f), 0.0f, 0.50f);
            b.joyAngleDeg = std::clamp(o.value("joyAngleDeg", 45.0f), 15.0f, 75.0f);
            b.joyReleaseDelayMs = std::clamp(o.value("joyReleaseDelayMs", 0), 0, 200);
            b.joyIntervalMs = std::clamp(o.value("joyIntervalMs", 16), 16, 100);
            b.joyShiftEnabled = o.value("joyShiftEnabled", false);
            b.joyShiftKey = o.value("joyShiftKey", 0x10);
            b.joyShiftOffset = std::clamp(o.value("joyShiftOffset", -0.12f), -0.45f, 0.45f);
            b.joyStepLength = std::clamp(o.value("joyStepLength", 0.10f), 0.01f, 0.20f);
            b.joySpeed = std::clamp(o.value("joySpeed", 1.0f), 0.25f, 4.0f);
            b.joyHumanize = o.value("joyHumanize", false);
            b.joyShake = o.value("joyShake", false);
            b.joyShakePeriodMs = std::clamp(o.value("joyShakePeriodMs", 100), 100, 500);
            b.joyShakeAmp = std::clamp(o.value("joyShakeAmp", 0.10f), 0.10f, 10.0f);
            b.joyRandomOffset = std::clamp(o.value("joyRandomOffset", 1.0f), 0.10f, 1.0f);
            // Backward compat: an old layout only had the humanize bool, which maps
            // to profile 1 (smooth spring). joyProfile wins when present.
            b.joyProfile = std::clamp(o.value("joyProfile", b.joyHumanize ? 1 : 0), 0, 2);
            if (b.joyProfile != 0) b.joyHumanize = b.joyProfile == 1;
            b.joyNatInitialMs = std::clamp(o.value("joyNatInitialMs", 12), 0, 60);
            b.joyNatSegMs = std::clamp(o.value("joyNatSegMs", 14), 8, 60);
            b.joyNatDurMs = std::clamp(o.value("joyNatDurMs", 90), 20, 400);
            b.joyNatSpacingPx = std::clamp(o.value("joyNatSpacingPx", 0.0f), 0.0f, 40.0f);
            b.joyNatTimeVarPct = std::clamp(o.value("joyNatTimeVarPct", 15), 0, 50);
            b.joyNatPathVarPx = std::clamp(o.value("joyNatPathVarPx", 1.5f), 0.0f, 10.0f);
            b.joyNatEase = std::clamp(o.value("joyNatEase", 1), 0, 2);
            b.sensitivity = std::clamp(o.value("sensitivity", 1.0f), 0.01f, 50.0f);
            // Old profiles have a single sensitivity: Y inherits X when the key is absent.
            b.sensitivityY = std::clamp(o.value("sensitivityY", b.sensitivity), 0.01f, 50.0f);
            b.invertY = o.value("invertY", false);
            b.camBoundsEnabled = o.value("camBoundsEnabled", false);
            b.camBoundsRange = std::clamp(o.value("camBoundsRange", 0.30f), 0.05f, 0.50f);
            b.camBoundsRangeX = std::clamp(o.value("camBoundsRangeX", 0.0f), 0.0f, 0.50f);
            b.camAutoReleaseMs = std::clamp(o.value("camAutoReleaseMs", 0), 0, 5000);
            b.camReturnType = std::clamp(o.value("camReturnType", 0), 0, 3);
            b.camReturnDelayMs = std::clamp(o.value("camReturnDelayMs", 250), 0, 5000);
            b.camAverageMs = std::clamp(o.value("camAverageMs", 0), 0, 500);
            b.camAutoFire = o.value("camAutoFire", false);
            b.camAutoFireReleaseMs = std::clamp(o.value("camAutoFireReleaseMs", 50), 10, 2000);
            b.comment = o.value("comment", std::string());
            b.recoilEnabled = o.value("recoilEnabled", true);
            b.recoilDown = std::clamp(o.value("recoilDown", 10), 0, 100);
            b.recoilUp = std::clamp(o.value("recoilUp", 0), 0, 100);
            b.recoilLeft = std::clamp(o.value("recoilLeft", 0), 0, 100);
            b.recoilRight = std::clamp(o.value("recoilRight", 0), 0, 100);
            b.recoilDelayMs = std::clamp(o.value("recoilDelayMs", 0), 0, 5000);
            b.recoilToggleKey = o.value("recoilToggleKey", 0);
            b.rapidFire = o.value("rapidFire", false);
            b.rapidFireDelayMs = std::clamp(o.value("rapidFireDelayMs", 60), 10, 2000);
            b.rapidFireToggleKey = o.value("rapidFireToggleKey", 0);
            p.bindings.push_back(std::move(b));
        }
        if (j.contains("automation")) {
            std::vector<std::string> migrated;
            if (!automationFromJson(j["automation"].dump(), p.automation, err, &migrated)) return false;
            for (const auto& n : migrated) log::info("profile migration: " + n);
        }
        auto problems = validateProfile(p);
        if (!problems.empty()) {
            err = problems.front();
            return false;
        }
        out = std::move(p);
        return true;
    } catch (const std::exception& e) {
        err = std::string("invalid profile JSON: ") + e.what();
        return false;
    }
}
}  // namespace wasd
