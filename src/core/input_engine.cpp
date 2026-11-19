#include "core/input_engine.h"

#include "core/log.h"
#include "core/stats.h"  // nowMicros()

#include <algorithm>
#include <cmath>

namespace wasd {
using proto::MotionAction;

InputEngine::InputEngine(TouchSink sink) : sink_(std::move(sink)) {}

void InputEngine::setProfile(const Profile& p) {
    // Fields the engine itself mutates at runtime (hotkey toggles). A change in
    // ONLY these must never lift a live finger: toggling recoil/rapid mid-spray
    // used to releaseAll() and kill the joystick, fire and camera fingers.
    const auto sameExceptRuntime = [](Binding a, Binding b) {
        a.recoilEnabled = b.recoilEnabled = false;
        a.rapidFire = b.rapidFire = false;
        return a == b;
    };
    const std::size_t oldN = profile_.bindings.size(), newN = p.bindings.size();
    std::vector<char> keep(std::min(oldN, newN), false);
    for (std::size_t i = 0; i < keep.size(); ++i) keep[i] = sameExceptRuntime(profile_.bindings[i], p.bindings[i]);
    for (std::size_t i = 0; i < oldN; ++i)
        if (i >= keep.size() || !keep[i]) releaseBinding(i);

    // Resize the per-binding state vectors, preserving slots whose binding survived.
    const auto resizeKeep = [&](auto& vec) {
        std::decay_t<decltype(vec)> fresh(newN);
        for (std::size_t i = 0; i < std::min(vec.size(), newN); ++i)
            if (keep[i]) fresh[i] = vec[i];
        vec = std::move(fresh);
    };
    resizeKeep(tapActive_);
    resizeKeep(tapPulses_);
    resizeKeep(tapReleaseAtUs_);
    resizeKeep(singleDeferUs_);
    resizeKeep(joyActive_);
    resizeKeep(joys_);
    resizeKeep(drags_);
    resizeKeep(fires_);
    profile_ = p;
}

bool InputEngine::consumeRecoilToggle(std::size_t& bindingIdx, bool& enabled) {
    if (!recoilToggled_ || recoilToggledIdx_ >= profile_.bindings.size()) return false;
    recoilToggled_ = false;
    bindingIdx = recoilToggledIdx_;
    enabled = profile_.bindings[recoilToggledIdx_].recoilEnabled;
    return true;
}

bool InputEngine::consumeRapidFireToggle(std::size_t& bindingIdx, bool& rapidFireOn) {
    if (!rapidToggled_ || rapidToggledIdx_ >= profile_.bindings.size()) return false;
    rapidToggled_ = false;
    bindingIdx = rapidToggledIdx_;
    rapidFireOn = profile_.bindings[rapidToggledIdx_].rapidFire;
    return true;
}

void InputEngine::setFrameSize(uint32_t w, uint32_t h) {
    if (w == frameW_ && h == frameH_) return;
    // Update the dims BEFORE releasing: the server ignores touch events whose
    // frame size does not match the current video size, so Ups stamped with the
    // old dims were dropped and left ghost fingers held on the device.
    // Keys are preserved across the release: the user is still physically
    // holding them, and the joystick retry in tick() re-lands the finger at the
    // new size instead of leaving movement dead until the next key press.
    const auto savedKeys = keys_;
    const auto savedSources = keySources_;
    frameW_ = w;
    frameH_ = h;
    releaseAll();
    keys_ = savedKeys;
    keySources_ = savedSources;
}

void InputEngine::toFrame(Vec2 n, float& fx, float& fy) const {
    fx = std::clamp(n.x, 0.0f, 1.0f) * static_cast<float>(frameW_);
    fy = std::clamp(n.y, 0.0f, 1.0f) * static_cast<float>(frameH_);
}

void InputEngine::emit(MotionAction a, uint64_t id, float fx, float fy, float pressure) {
    if (!frameW_ || !frameH_) return;
    proto::TouchEvent ev;
    ev.action = a;
    ev.pointerId = id;
    ev.x = std::clamp(static_cast<int32_t>(std::lround(fx)), 0, static_cast<int32_t>(frameW_) - 1);
    ev.y = std::clamp(static_cast<int32_t>(std::lround(fy)), 0, static_cast<int32_t>(frameH_) - 1);
    ev.frameWidth = static_cast<uint16_t>(frameW_);
    ev.frameHeight = static_cast<uint16_t>(frameH_);
    ev.pressure = pressure;
    sink_(ev);
}

bool InputEngine::onKey(int vk, bool down, int64_t nowUs, InputSource src) {
    if (vk <= 0 || vk > 255) return false;
    const uint8_t mask = static_cast<uint8_t>(src);
    const uint8_t prevSources = keySources_[vk];
    const bool wasDown = keys_[vk];

    if (down) {
        if (prevSources != 0 && (prevSources & mask) == 0) {
            ++conflictsResolved_;  // overlapping press from a second source coordinated cleanly
        }
        keySources_[vk] |= mask;
    } else {
        if ((prevSources & mask) != 0 && (prevSources & ~mask) != 0) {
            ++conflictsResolved_;  // premature release prevented because another source still holds the key
        }
        keySources_[vk] &= static_cast<uint8_t>(~mask);
    }
    const bool nowDown = (keySources_[vk] != 0);
    keys_[vk] = nowDown;

    if (vk == profile_.lockToggleKey) {
        if (nowDown && !wasDown) setLocked(!locked_);
        return true;
    }
    // If another source is still holding the key down, or if this was a redundant press from an
    // additional source while already down, do not emit duplicate Down or premature Up!
    if (nowDown == wasDown) {
        // Still report whether the key is bound in the profile
        for (const Binding& b : profile_.bindings) {
            if (vk == b.key || (b.key2 != 0 && vk == b.key2) || vk == b.recoilToggleKey || vk == b.rapidFireToggleKey ||
                (b.type == BindingType::Joystick &&
                 (vk == b.keyUp || vk == b.keyDown || vk == b.keyLeft || vk == b.keyRight || (b.joyShiftEnabled && vk == b.joyShiftKey))))
                return true;
        }
        return false;
    }
    bool used = false;
    for (std::size_t i = 0; i < profile_.bindings.size(); ++i) {
        Binding& b = profile_.bindings[i];
        if (b.type == BindingType::Fire && b.recoilToggleKey != 0 && vk == b.recoilToggleKey) {
            if (down && !wasDown) {
                b.recoilEnabled = !b.recoilEnabled;
                recoilToggled_ = true;
                recoilToggledIdx_ = i;
            }
            used = true;
            continue;
        }
        if (b.type == BindingType::Fire && b.rapidFireToggleKey != 0 && vk == b.rapidFireToggleKey) {
            if (down && !wasDown) {
                b.rapidFire = !b.rapidFire;
                if (!b.rapidFire) b.recoilEnabled = true;  // switching back to continuous fire ensures recoil is active
                rapidToggled_ = true;
                rapidToggledIdx_ = i;
            }
            used = true;
            continue;
        }
        const uint64_t id = kBindingPointerBase + i;
        const bool isKeyMatch = (vk == b.key) || (b.key2 != 0 && vk == b.key2);
        bool wantDown = (b.key > 0 && b.key < 256 && keys_[b.key]) &&
                        (b.key2 == 0 || (b.key2 > 0 && b.key2 < 256 && keys_[b.key2]));
        // Combo suppression: if a combo binding (key+key2) shares this single key and both combo keys are held, suppress the single binding.
        if (wantDown && b.key2 == 0) {
            for (std::size_t j = 0; j < profile_.bindings.size(); ++j) {
                const Binding& o = profile_.bindings[j];
                if (o.key2 != 0 && (o.key == b.key || o.key2 == b.key) &&
                    o.key > 0 && o.key < 256 && o.key2 > 0 && o.key2 < 256 && keys_[o.key] && keys_[o.key2]) {
                    wantDown = false;
                    break;
                }
            }
        }
        // Combo deferral: if this single-key binding's key is the primary of a combo, defer activation by 50ms
        // so the combo has a chance to fire first (prevents spurious single-key Down when pressing both keys together).
        // Fire bindings are exempt: the trigger must respond instantly, always.
        const bool isComboPrimary = (b.key2 == 0 && b.key > 0 && b.key < 256 && down && !wasDown &&
                                     b.type != BindingType::Fire);
        bool deferred = false;
        if (isComboPrimary && wantDown) {
            for (std::size_t j = 0; j < profile_.bindings.size(); ++j) {
                const Binding& o = profile_.bindings[j];
                if (j != i && o.key2 != 0 && o.key == b.key && o.key2 > 0 && o.key2 < 256) {
                    singleDeferUs_[i] = nowUs + 50000;  // 50ms combo window
                    deferred = true;
                    break;
                }
            }
        }
        // Cancel deferral if key released or combo is now active
        if (!down && (vk == b.key || (b.key2 != 0 && vk == b.key2))) {
            singleDeferUs_[i] = 0;
        }
        if (deferred) continue;  // skip activation this event; tick() will fire it if the combo window expires
        // When a combo activates, cancel deferral of single-key bindings sharing the primary or secondary key
        if (wantDown && b.key2 != 0 && down) {
            for (std::size_t j = 0; j < profile_.bindings.size(); ++j) {
                if (j != i && profile_.bindings[j].key2 == 0 &&
                    (profile_.bindings[j].key == b.key || profile_.bindings[j].key == b.key2))
                    singleDeferUs_[j] = 0;
            }
        }
        if (b.disabled) continue;
        switch (b.type) {
            case BindingType::Tap:
                if (!isKeyMatch && !(tapActive_[i] && !wantDown)) break;
                used = true;
                if (b.tapToggle) {
                    // Toggle Finger: a key-press edge flips the finger on/off; key-up is ignored.
                    if (down && !wasDown && vk == b.key) {
                        if (tapActive_[i]) tapFingerUp(i);
                        else if (tryReserveNonCoreFinger()) tapFingerDown(i, nowUs);
                    }
                } else if (wantDown && !tapActive_[i]) {
                    if (tryReserveNonCoreFinger()) tapFingerDown(i, nowUs);
                } else if (!wantDown && tapActive_[i] && tapReleaseAtUs_[i] == 0 && tapPulses_[i].left == 0 &&
                           !tapPulses_[i].downPhase) {
                    // A pending quickTap dwell or a repeat-pulse train owns its
                    // own release; lifting here would cut the tap short.
                    tapFingerUp(i);
                }
                break;
            case BindingType::Drag:
                if (!isKeyMatch) break;
                used = true;
                if (wantDown && !drags_[i].active) {
                    if (!tryReserveNonCoreFinger()) break;
                    DragState& d = drags_[i];
                    d.active = true;
                    d.startUs = nowUs;
                    toFrame(b.pos, d.x, d.y);
                    emit(MotionAction::Down, id, d.x, d.y);
                } else if (!wantDown && drags_[i].active) {
                    drags_[i].active = false;
                    emit(MotionAction::Up, id, drags_[i].x, drags_[i].y);
                }
                break;
            case BindingType::Fire:
                if (!isKeyMatch) break;
                used = true;
                if (wantDown && !fires_[i].held) {
                    if (!reserveFireFinger()) break;
                    startFire(i, nowUs);
                } else if (!wantDown && fires_[i].held) {
                    FireState& f = fires_[i];
                    f.held = false;
                    if (f.fingerDown) {
                        f.fingerDown = false;
                        emit(MotionAction::Up, id, f.x, f.y);
                    }
                }
                break;
            case BindingType::Joystick:
                if (vk == b.keyUp || vk == b.keyDown || vk == b.keyLeft || vk == b.keyRight ||
                    (b.joyShiftEnabled && vk == b.joyShiftKey)) {
                    used = true;
                    updateJoystick(i, nowUs);
                }
                break;
            case BindingType::Camera: break;
        }
    }
    return used;
}

static float nextSignedRand(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (static_cast<int32_t>(s & 0xFFFFu) - 32768) / 32768.0f;
}

void InputEngine::updateJoystick(std::size_t i, int64_t nowUs) {
    const Binding& b = profile_.bindings[i];
    const uint64_t id = kBindingPointerBase + i;
    JoyState& js = joys_[i];
    float dx = static_cast<float>((keys_[b.keyRight] ? 1 : 0) - (keys_[b.keyLeft] ? 1 : 0));
    float dy = static_cast<float>((keys_[b.keyDown] ? 1 : 0) - (keys_[b.keyUp] ? 1 : 0));
    float cx, cy;
    toFrame(b.pos, cx, cy);
    if (dx == 0 && dy == 0) {
        if (joyActive_[i]) {
            if (b.joyReleaseDelayMs > 0) {
                if (js.releaseAtUs == 0) js.releaseAtUs = nowUs + static_cast<int64_t>(b.joyReleaseDelayMs) * 1000;
            } else {
                joyActive_[i] = false;
                js.active = false;
                js.releaseAtUs = 0;
                emit(MotionAction::Up, id, cx, cy);
            }
        }
        return;
    }
    js.releaseAtUs = 0;

    // Direction unit vector with configurable diagonal deflection angle (15..75 deg from vertical)
    float ux = 0.0f, uy = 0.0f;
    if (dx != 0.0f && dy != 0.0f) {
        // Deflection angle from the forward axis, fully user-controlled (15..75).
        const float rad = std::clamp(b.joyAngleDeg, 15.0f, 75.0f) * (3.14159265f / 180.0f);
        ux = std::copysign(std::sin(rad), dx);
        uy = std::copysign(std::cos(rad), dy);
    } else {
        const float len = std::sqrt(dx * dx + dy * dy);
        ux = dx / len;
        uy = dy / len;
    }

    // Effective radius with optional Shift walk/sprint offset and Upward sprint offset
    float effRadius = b.radius;
    if (b.joyShiftEnabled && b.joyShiftKey > 0 && b.joyShiftKey < 256 && keys_[b.joyShiftKey]) {
        effRadius = std::clamp(effRadius + b.joyShiftOffset, 0.02f, 0.48f);
    }
    const float r = effRadius * static_cast<float>(frameH_);
    const float upExtra = (dy < 0.0f && b.joyUpOffset > 0.0f) ? (b.joyUpOffset * static_cast<float>(frameH_) * (-uy)) : 0.0f;

    // Ideal target position (without humanization noise)
    const float idealX = cx + ux * r;
    const float idealY = cy + uy * r - upExtra;
    if (trace_)
        log::info("joy " + std::to_string(i) + " keys(U=" + std::string(keys_[b.keyUp] ? "1" : "0") +
                  " D=" + std::string(keys_[b.keyDown] ? "1" : "0") + " L=" + std::string(keys_[b.keyLeft] ? "1" : "0") +
                  " R=" + std::string(keys_[b.keyRight] ? "1" : "0") + ") vec=(" + std::to_string(dx) + "," +
                  std::to_string(dy) + ") dir=(" + std::to_string(ux) + "," + std::to_string(uy) + ") pos=(" +
                  std::to_string(static_cast<int>(idealX)) + "," + std::to_string(static_cast<int>(idealY)) + ") ptr=0x" +
                  std::to_string(id));
    js.targetX = idealX;
    js.targetY = idealY;

    if (!joyActive_[i]) {
        if (!tryReserveFinger()) {
            js.retryAtUs = nowUs + 100000;  // retry in tick(): the finger must not stay dead
            return;
        }
        joyActive_[i] = true;
        js.active = true;
        js.bornUs = nowUs;
        js.velX = 0.0f;
        js.velY = 0.0f;
        js.wanderX = 0.0f;
        js.wanderY = 0.0f;
        if (b.joyProfile == 2) {
            // Natural profile: Down at center, then stepped waypoints in tick().
            // The rng is re-seeded deterministically so identical configurations
            // produce identical paths (repeatable test mode).
            js.curX = cx;
            js.curY = cy;
            js.pathSX = cx;
            js.pathSY = cy;
            js.pathTX = idealX;
            js.pathTY = idealY;
            js.pathDurUs = static_cast<int64_t>(std::clamp(b.joyNatDurMs, 20, 400)) * 1000;
            js.pathStartUs = nowUs + static_cast<int64_t>(std::clamp(b.joyNatInitialMs, 0, 60)) * 1000;
            js.nextStepUs = js.pathStartUs;
            js.rng = 0x9E3779B9u ^ (static_cast<uint32_t>(i) * 2654435761u);
            js.latAmp = nextSignedRand(js.rng) * std::clamp(b.joyNatPathVarPx, 0.0f, 10.0f);
            js.latOff = 0.0f;
            js.alongMax = 0.0f;
            js.converged = false;
            emit(MotionAction::Down, id, js.curX, js.curY);
            return;
        }
        if (b.joyHumanize) {
            // Touch-down at center (like a real finger landing on the stick)
            js.curX = cx;
            js.curY = cy;
            js.nextStepUs = nowUs + static_cast<int64_t>(std::clamp(b.joyIntervalMs, 16, 100)) * 1000;
            emit(MotionAction::Down, id, js.curX, js.curY);
            return;  // first Move happens in tick() for smooth acceleration
        } else {
            // Non-humanized: Down at center then Move to target (original behavior)
            js.curX = cx;
            js.curY = cy;
            emit(MotionAction::Down, id, js.curX, js.curY);
            js.curX = idealX;
            js.curY = idealY;
            js.lastEmitUs = nowUs;
            emit(MotionAction::Move, id, js.curX, js.curY);
            return;
        }
    }

    // Already active: natural profile restarts a path from the current position
    // toward the moved target; its duration scales with the remaining distance.
    if (b.joyProfile == 2) {
        const float rx = idealX - js.pathTX, ry = idealY - js.pathTY;
        if (rx * rx + ry * ry > 1.0f) {
            js.pathSX = js.curX;
            js.pathSY = js.curY;
            js.pathTX = idealX;
            js.pathTY = idealY;
            const float reach = std::max(1.0f, effRadius * static_cast<float>(frameH_));
            const float dist = std::sqrt(rx * rx + ry * ry);
            const float frac = std::clamp(dist / reach, 0.10f, 1.0f);
            js.pathDurUs = static_cast<int64_t>(std::clamp(b.joyNatDurMs, 20, 400) * 1000.0f * frac);
            js.pathStartUs = nowUs;
            js.alongMax = 0.0f;
            js.converged = false;
            js.latAmp = nextSignedRand(js.rng) * std::clamp(b.joyNatPathVarPx, 0.0f, 10.0f);
            js.latOff = 0.0f;
            js.nextStepUs = nowUs + static_cast<int64_t>(std::clamp(b.joyNatSegMs, 8, 60)) * 1000;
        }
    } else if (!b.joyHumanize) {
        // Geometric: snap immediately to the new target on direction change.
        js.curX = idealX;
        js.curY = idealY;
        js.lastEmitUs = nowUs;
        emit(MotionAction::Move, id, js.curX, js.curY);
    }
    // For humanized mode, the spring in tick() smoothly redirects toward the new target.
}

const Binding* InputEngine::findCamera() const {
    const Binding* cam = nullptr;
    for (const Binding& b : profile_.bindings)
        if (b.type == BindingType::Camera) cam = &b;
    return cam;
}

const Binding* InputEngine::findFire() const {
    for (const Binding& b : profile_.bindings)
        if (b.type == BindingType::Fire) return &b;
    return nullptr;
}

int InputEngine::usedFingerSlots() const {
    int n = 0;
    for (bool a : tapActive_) n += a;
    for (bool a : joyActive_) n += a;
    for (const DragState& d : drags_) n += d.active;
    for (const FireState& f : fires_) n += f.fingerDown;
    for (const MacroFingerState& f : macroFingers_) n += f.active;
    n += camActive_ + directActive_ + camFireDown_;
    return n;
}

int InputEngine::activeFingerCount() const { return usedFingerSlots(); }

// Rejects a new finger once the profile's multi-touch budget is exhausted.
// The rejection is counted as a resolved conflict and, crucially, produces NO
// partial events: a press that is refused here never gets a Down, so nothing
// stays stuck and no spurious Up is emitted later.
bool InputEngine::tryReserveFinger() {
    if (usedFingerSlots() >= profile_.maxVirtualFingers) {
        ++conflictsResolved_;
        ++fingerRejections_;
        return false;
    }
    return true;
}

int InputEngine::reservedCoreSlots() const {
    // Inactive essential producers whose next activation must always succeed:
    // every joystick binding, the camera finger, and the direct mouse finger.
    int n = 0;
    for (std::size_t i = 0; i < profile_.bindings.size(); ++i)
        if (profile_.bindings[i].type == BindingType::Joystick && (i >= joyActive_.size() || !joyActive_[i])) ++n;
    if (findCamera() && !camActive_) ++n;
    if (!directActive_) ++n;
    return n;
}

// Non-essential fingers (taps, drags, fires, macro fingers, autofire) must leave
// the reserved core slots free on top of the global cap. The rejection produces
// no partial Down/Up pair, same as tryReserveFinger().
bool InputEngine::tryReserveNonCoreFinger() {
    if (usedFingerSlots() + reservedCoreSlots() >= profile_.maxVirtualFingers) {
        ++conflictsResolved_;
        ++fingerRejections_;
        return false;
    }
    return true;
}

// Fire presses get absolute priority: when the budget is full, evict the
// lowest-priority non-essential finger (active taps first, then drags) instead
// of silently dropping the trigger.
bool InputEngine::reserveFireFinger() {
    if (tryReserveNonCoreFinger()) return true;
    for (std::size_t j = 0; j < tapActive_.size(); ++j)
        if (tapActive_[j]) {
            tapFingerUp(j);
            ++conflictsResolved_;
            return true;
        }
    for (std::size_t j = 0; j < drags_.size(); ++j)
        if (drags_[j].active) {
            drags_[j].active = false;
            emit(MotionAction::Up, kBindingPointerBase + j, drags_[j].x, drags_[j].y);
            ++conflictsResolved_;
            return true;
        }
    return false;
}

void InputEngine::tapFingerDown(std::size_t i, int64_t nowUs) {
    const Binding& b = profile_.bindings[i];
    float x, y;
    toFrame(b.pos, x, y);
    tapActive_[i] = true;
    tapReleaseAtUs_[i] = (b.quickTap || b.tapRepeat > 1)
                             ? (nowUs + static_cast<int64_t>(std::max(15, b.tapHoldMs)) * 1000)
                             : 0;
    emit(MotionAction::Down, kBindingPointerBase + i, x, y);
    if (b.tapRepeat > 1) tapPulses_[i] = {b.tapRepeat - 1, tapReleaseAtUs_[i], true};
}

void InputEngine::tapFingerUp(std::size_t i) {
    const Binding& b = profile_.bindings[i];
    float x, y;
    toFrame(b.pos, x, y);
    tapActive_[i] = false;
    tapPulses_[i] = {};
    tapReleaseAtUs_[i] = 0;
    emit(MotionAction::Up, kBindingPointerBase + i, x, y);
}

std::vector<InputEngine::FingerInfo> InputEngine::fingerStates() const {
    std::vector<FingerInfo> out;
    out.reserve(static_cast<std::size_t>(usedFingerSlots()));
    const char* kinds[] = {"tap", "joystick", "camera", "drag", "fire"};
    for (std::size_t i = 0; i < profile_.bindings.size(); ++i) {
        const Binding& b = profile_.bindings[i];
        uint8_t src = 0;
        if (b.key > 0 && b.key < 256) src |= keySources_[b.key];
        if (b.key2 > 0 && b.key2 < 256) src |= keySources_[b.key2];
        if (b.type == BindingType::Joystick) {
            for (int k : {b.keyUp, b.keyDown, b.keyLeft, b.keyRight})
                if (k > 0 && k < 256) src |= keySources_[k];
        }
        auto push = [&](bool active, float x, float y, const char* kind) {
            if (active) out.push_back({kBindingPointerBase + i, kind, x, y, src, static_cast<int>(i)});
        };
        const int ti = static_cast<int>(b.type);
        const char* kind = kinds[ti < 5 ? ti : 0];
        float x = 0, y = 0;
        toFrame(b.pos, x, y);
        switch (b.type) {
            case BindingType::Tap: push(tapActive_[i], x, y, kind); break;
            case BindingType::Joystick: push(joyActive_[i], joys_[i].curX, joys_[i].curY, kind); break;
            case BindingType::Drag: push(drags_[i].active, drags_[i].x, drags_[i].y, kind); break;
            case BindingType::Fire: push(fires_[i].fingerDown, fires_[i].x, fires_[i].y, kind); break;
            case BindingType::Camera: break;
        }
    }
    if (camActive_) out.push_back({kCameraPointerId, "camera", camX_, camY_, static_cast<uint8_t>(InputSource::Physical), -1});
    if (directActive_) out.push_back({proto::kPointerIdGenericFinger, "direct", directX_, directY_, static_cast<uint8_t>(InputSource::Physical), -1});
    if (camFireDown_) out.push_back({kAutoFirePointerId, "autofire", camFireX_, camFireY_, 0, -1});
    for (int f = 0; f < kMaxMacroFingers; ++f)
        if (macroFingers_[f].active)
            out.push_back({kMacroPointerBase + static_cast<uint64_t>(f), "macro", macroFingers_[f].x, macroFingers_[f].y,
                           macroFingers_[f].sources, -1});
    return out;
}

void InputEngine::cameraDown(int64_t nowUs) {
    if (const Binding* b = findCamera()) {
        if (!tryReserveFinger()) return;
        toFrame(b->pos, camX_, camY_);
        camFracX_ = camFracY_ = 0.0f;
        camAvgX_ = camAvgY_ = 0.0f;
        camRetActive_ = false;
        camActive_ = true;
        camLastMoveUs_ = nowUs;
        // Pressure 0 like WASD+: the touch is real (ACTION_DOWN) but reports
        // Prs 0.0 on the device, matching the reference mapper's behaviour.
        emit(MotionAction::Down, kCameraPointerId, camX_, camY_, 0.0f);
    }
}

void InputEngine::applyCameraPixelDelta(float fdx, float fdy, int64_t nowUs) {
    const Binding* cam = findCamera();
    if (!cam || !frameW_ || (fdx == 0.0f && fdy == 0.0f)) return;
    if (!camActive_) cameraDown(nowUs);
    if (!camActive_) return;  // finger budget exhausted

    // Accumulate sub-pixel deltas so fine vertical recoil and high-DPI mouse motion
    // never lose fractional pixels or emit redundant identical integer coordinates.
    // Deltas arrive in device pixels (WASD+ sensitivity semantics); convert them
    // to frame pixels so the same sensitivity moves the aim identically on any
    // stream resolution - a smaller frame used to make the camera much faster.
    const float sx = devW_ ? static_cast<float>(frameW_) / static_cast<float>(devW_) : 1.0f;
    const float sy = devH_ ? static_cast<float>(frameH_) / static_cast<float>(devH_) : 1.0f;
    camFracX_ += fdx * sx;
    camFracY_ += fdy * sy;
    const float stepX = std::trunc(camFracX_);
    const float stepY = std::trunc(camFracY_);
    if (stepX == 0.0f && stepY == 0.0f) return;
    camFracX_ -= stepX;
    camFracY_ -= stepY;

    float ax, ay;
    toFrame(cam->pos, ax, ay);
    float nx = camX_ + stepX;
    float ny = camY_ + stepY;
    const float fw = static_cast<float>(frameW_) - 1.0f, fh = static_cast<float>(frameH_) - 1.0f;
    if (cam->camBoundsEnabled) {
        // Rectangular hard bounds with independent X/Y half-extents (X mirrors Y
        // when camBoundsRangeX is 0 -> square). The aim pointer can never leave.
        const float rx = (cam->camBoundsRangeX > 0.0f ? cam->camBoundsRangeX : cam->camBoundsRange) *
                         static_cast<float>(frameH_);
        const float ry = cam->camBoundsRange * static_cast<float>(frameH_);
        const float cx = std::clamp(std::clamp(nx, ax - rx, ax + rx), 0.0f, fw);
        const float cy = std::clamp(std::clamp(ny, ay - ry, ay + ry), 0.0f, fh);
        if (cx != nx || cy != ny) ++camBoundsClamps_;
        nx = cx; ny = cy;
    } else {
        // Roam inside the aim ring (ellipse): when the pointer crosses the
        // configured ring it recenters seamlessly — lift + land at the anchor
        // and keep the same delta flowing. The touch never stops or clamps.
        const float rx = (cam->radiusX > 0.0f ? cam->radiusX : cam->radius) * static_cast<float>(frameH_);
        const float ry = cam->radius * static_cast<float>(frameH_);
        const float ex = (nx - ax) / rx, ey = (ny - ay) / ry;
        if (ex * ex + ey * ey > 1.0f) {
            camTeleport(ax, ay);
            nx = std::clamp(ax + stepX, 0.0f, fw);
            ny = std::clamp(ay + stepY, 0.0f, fh);
        }
    }
    // Immediate emit: the control queue already coalesces back-to-back Moves
    // for one pointer, so buffering deltas here only added latency — that hold
    // was the "watery" feel versus a direct 1:1 mapper.
    camX_ = nx;
    camY_ = ny;
    emit(MotionAction::Move, kCameraPointerId, nx, ny, 0.0f);
}

void InputEngine::startFire(std::size_t i, int64_t nowUs) {
    const Binding& b = profile_.bindings[i];
    FireState& f = fires_[i];
    f.held = true;
    f.fingerDown = true;
    f.startUs = nowUs;
    f.lastTickUs = nowUs;
    f.nextPulseUs = nowUs + static_cast<int64_t>(std::max(10, b.rapidFireDelayMs)) * 500;
    toFrame(b.pos, f.x, f.y);
    emit(MotionAction::Down, kBindingPointerBase + i, f.x, f.y);

    // Land the aim finger if it is absent so mouse-look and recoil keep working
    // during the spray. The finger is NEVER re-anchored here — recentering only
    // happens when it crosses the configured aim ring, so the fire/throw key
    // never causes a mid-flick jump on the touch trace.
    const Binding* cam = findCamera();
    if (!locked_ || !cam) return;
    if (!camActive_) cameraDown(nowUs);
    // Prime the sub-pixel accumulator toward the recoil direction so the very
    // next tick (~1 ms) already emits the first compensation step. Without
    // this, the first integer pixel only materializes once a full pixel worth
    // of recoil accumulated (~100 ms at low stream heights), letting the first
    // shots climb before compensation starts.
    const int netX = b.recoilRight - b.recoilLeft;
    const int netY = b.recoilDown - b.recoilUp;
    if (camActive_ && b.recoilEnabled && b.recoilDelayMs == 0) {
        if (netX != 0) camFracX_ += netX > 0 ? 0.95f : -0.95f;
        if (netY != 0) camFracY_ += netY > 0 ? 0.95f : -0.95f;
    }
}

KeyLogicalState InputEngine::keyState(int vk) const {
    if (vk <= 0 || vk > 255) return KeyLogicalState::Released;
    if (keys_[vk]) return KeyLogicalState::Pressed;
    // Check if any joystick or quick-tap binding using this key has a pending delayed release
    for (std::size_t i = 0; i < profile_.bindings.size(); ++i) {
        const Binding& b = profile_.bindings[i];
        if (b.type == BindingType::Tap && b.key == vk && i < tapActive_.size() && tapActive_[i] && tapReleaseAtUs_[i] > 0)
            return KeyLogicalState::ReleasePending;
        if (b.type == BindingType::Joystick && (vk == b.keyUp || vk == b.keyDown || vk == b.keyLeft || vk == b.keyRight) &&
            i < joys_.size() && joyActive_[i] && joys_[i].releaseAtUs > 0)
            return KeyLogicalState::ReleasePending;
    }
    return KeyLogicalState::Released;
}

uint8_t InputEngine::keySources(int vk) const {
    if (vk <= 0 || vk > 255) return 0;
    return keySources_[vk];
}

uint32_t InputEngine::activeSourcesMask() const {
    uint32_t mask = 0;
    for (uint8_t s : keySources_) mask |= s;
    for (const auto& mf : macroFingers_)
        if (mf.active) mask |= mf.sources;
    if (directActive_) mask |= static_cast<uint32_t>(InputSource::DirectMouse);
    if (camActive_) mask |= static_cast<uint32_t>(InputSource::Physical);
    for (std::size_t i = 0; i < fires_.size(); ++i) {
        if (fires_[i].held && i < profile_.bindings.size() && profile_.bindings[i].recoilEnabled)
            mask |= static_cast<uint32_t>(InputSource::Recoil);
    }
    return mask;
}

void InputEngine::releaseSource(InputSource src, int64_t nowUs) {
    const uint8_t mask = static_cast<uint8_t>(src);
    for (int vk = 1; vk < 256; ++vk) {
        if (keySources_[vk] & mask) {
            onKey(vk, false, nowUs, src);
        }
    }
    for (int f = 0; f < kMaxMacroFingers; ++f) {
        MacroFingerState& mf = macroFingers_[f];
        if (mf.active && (mf.sources & mask)) {
            mf.sources &= static_cast<uint8_t>(~mask);
            if (mf.sources == 0) {
                mf.active = false;
                emit(MotionAction::Up, kMacroPointerBase + static_cast<uint64_t>(f), mf.x, mf.y);
            } else {
                ++conflictsResolved_;
            }
        }
    }
}

void InputEngine::onMacroTouch(MotionAction action, int finger, Vec2 normalized, InputSource src) {
    if (finger < 0 || finger >= kMaxMacroFingers || !frameW_ || !frameH_) return;
    MacroFingerState& mf = macroFingers_[finger];
    const uint8_t mask = static_cast<uint8_t>(src);
    const uint64_t id = kMacroPointerBase + static_cast<uint64_t>(finger);
    float fx = 0, fy = 0;
    toFrame(normalized, fx, fy);

    if (action == MotionAction::Down) {
        const bool wasActive = mf.active;
        if (wasActive && (mf.sources & ~mask) != 0) ++conflictsResolved_;
        if (!wasActive && !tryReserveNonCoreFinger()) return;  // budget full: no partial finger
        mf.sources |= mask;
        mf.active = true;
        mf.x = fx;
        mf.y = fy;
        emit(wasActive ? MotionAction::Move : MotionAction::Down, id, mf.x, mf.y);
    } else if (action == MotionAction::Move) {
        mf.x = fx;
        mf.y = fy;
        if (!mf.active) {
            if (!tryReserveNonCoreFinger()) return;
            mf.sources |= mask;
            mf.active = true;
            emit(MotionAction::Down, id, mf.x, mf.y);
        } else {
            emit(MotionAction::Move, id, mf.x, mf.y);
        }
    } else if (action == MotionAction::Up || action == MotionAction::Cancel) {
        if (!mf.active) return;
        mf.sources &= static_cast<uint8_t>(~mask);
        if (mf.sources == 0) {
            mf.active = false;
            emit(MotionAction::Up, id, mf.x, mf.y);
        } else {
            ++conflictsResolved_;  // another source still holds this macro finger
        }
    }
}

void InputEngine::onMouseDelta(int dx, int dy, int64_t nowUs) {
    if (!locked_ || (dx == 0 && dy == 0)) return;
    const Binding* cam = findCamera();
    if (!cam || !frameW_) return;
    if (nowUs == 0) nowUs = nowMicros();
    camLastMoveUs_ = nowUs;
    camRetActive_ = false;  // real motion cancels any running return animation
    if (!camActive_) cameraDown(nowUs);  // finger down immediately; never wait for the smoothing window
    const float fdx = static_cast<float>(dx) * cam->sensitivity;
    const float fdy = static_cast<float>(dy) * cam->sensitivityY * (cam->invertY ? -1.0f : 1.0f);
    if (cam->camAverageMs > 0) {
        // Leaky integrator: deltas accumulate and drain inside tick() over the
        // configured window. Total motion is preserved; nothing is stored beyond
        // the window, so no stale movement is replayed after the user stops.
        camAvgX_ += fdx;
        camAvgY_ += fdy;
        camAvgLastUs_ = nowUs;
        if (camAvgTickUs_ == 0) camAvgTickUs_ = nowUs;
        return;
    }
    applyCameraPixelDelta(fdx, fdy, nowUs);
}

void InputEngine::camTeleport(float x, float y) {
    ++camTeleports_;
    emit(MotionAction::Up, kCameraPointerId, camX_, camY_);
    camX_ = x;
    camY_ = y;
    emit(MotionAction::Down, kCameraPointerId, camX_, camY_, 0.0f);
}

void InputEngine::tickCamera(int64_t nowUs, const Binding* cam) {
    // Drain the averaging integrator exponentially; flush the remainder once the
    // window elapsed since the last raw delta so queued motion never goes stale.
    const int64_t tickDt = camAvgTickUs_ ? nowUs - camAvgTickUs_ : 16000;
    camAvgTickUs_ = nowUs;
    if (camAvgX_ != 0.0f || camAvgY_ != 0.0f) {
        const float windowUs = std::max(1, cam->camAverageMs) * 1000.0f;
        float f = 1.0f - std::exp(-static_cast<float>(tickDt) / windowUs);
        if (static_cast<float>(nowUs - camAvgLastUs_) >= windowUs) f = 1.0f;
        const float ox = camAvgX_ * f, oy = camAvgY_ * f;
        camAvgX_ -= ox;
        camAvgY_ -= oy;
        applyCameraPixelDelta(ox, oy, nowUs);
        camLastMoveUs_ = nowUs;  // drained motion counts as activity
    }

    // Auto-release (WASD+ parity): once the pointer has been idle for
    // camAutoReleaseMs and no Smart Fire is held, lift the camera finger. On
    // the device this shows Prs 0.0 and resets the game's velocity tracker; the
    // next delta or recoil step re-lands the finger on the anchor, so every
    // motion burst starts centered instead of wherever the last one stopped.
    // Suppressed while camAutoFire is enabled (auto-fire needs an active aim
    // finger) and while any Fire binding is held (recoil owns the finger).
    if (camActive_ && cam->camAutoReleaseMs > 0 && !cam->camAutoFire && camLastMoveUs_ > 0 &&
        nowUs - camLastMoveUs_ >= static_cast<int64_t>(cam->camAutoReleaseMs) * 1000) {
        bool fireHeld = false;
        for (const FireState& f : fires_) fireHeld |= f.held;
        if (!fireHeld) {
            camActive_ = false;
            camRetActive_ = false;
            camAvgX_ = camAvgY_ = 0.0f;
            emit(MotionAction::Up, kCameraPointerId, camX_, camY_);
            return;
        }
    }

    // Return-to-center after camReturnDelayMs of pointer idle.
    // Suppressed while any Fire binding is held: recoil compensation IS activity -
    // teleporting to the anchor mid-spray would fight the recoil pull and produce
    // the random camera jumps seen in the field.
    if (!camActive_ || cam->camReturnType == 0 || camLastMoveUs_ == 0) {
        camRetActive_ = false;
        return;
    }
    for (const FireState& f : fires_) {
        if (f.held) {
            camRetActive_ = false;
            return;
        }
    }
    const int64_t idleUs = nowUs - camLastMoveUs_;
    if (idleUs < static_cast<int64_t>(cam->camReturnDelayMs) * 1000) return;
    float ax, ay;
    toFrame(cam->pos, ax, ay);
    const float dx = ax - camX_, dy = ay - camY_;
    if (std::hypot(dx, dy) <= 1.0f) {
        camRetActive_ = false;
        return;
    }
    switch (cam->camReturnType) {
        case 1:  // Instant: teleport to the anchor in one step
            camTeleport(ax, ay);
            camRetActive_ = false;
            break;
        case 3: {  // Type P: proportional teleport steps (fast when far, settles when near)
            const float k = std::clamp(static_cast<float>(tickDt) / 60000.0f, 0.0f, 1.0f);
            float nx = camX_ + dx * k, ny = camY_ + dy * k;
            if (std::hypot(ax - nx, ay - ny) <= 1.0f) nx = ax, ny = ay;
            camTeleport(nx, ny);
            camRetActive_ = false;
            break;
        }
        default: {  // 2 = Smooth: ease-out teleport steps over ~120 ms
            if (!camRetActive_) {
                camRetActive_ = true;
                camRetT0Us_ = nowUs;
                camRetX_ = camX_;
                camRetY_ = camY_;
            }
            const float t = std::clamp(static_cast<float>(nowUs - camRetT0Us_) / 120000.0f, 0.0f, 1.0f);
            const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
            float nx = camRetX_ + (ax - camRetX_) * e;
            float ny = camRetY_ + (ay - camRetY_) * e;
            if (t >= 1.0f) nx = ax, ny = ay, camRetActive_ = false;
            camTeleport(nx, ny);
            break;
        }
    }
}

void InputEngine::stopAutoFire() {
    if (camFireDown_) {
        camFireDown_ = false;
        emit(MotionAction::Up, kAutoFirePointerId, camFireX_, camFireY_);
    }
    camFireNextUs_ = 0;
}

void InputEngine::tickAutoFire(int64_t nowUs, const Binding* cam) {
    const Binding* fire = findFire();
    if (!cam->camAutoFire || !camActive_ || !fire) {
        if (camFireDown_ || camFireNextUs_) stopAutoFire();
        return;
    }
    const int64_t holdUs = static_cast<int64_t>(std::clamp(cam->camAutoFireReleaseMs, 10, 2000)) * 1000;
    if (camFireDown_) {
        if (nowUs - camFireStartUs_ >= holdUs) {
            camFireDown_ = false;
            emit(MotionAction::Up, kAutoFirePointerId, camFireX_, camFireY_);
            camFireNextUs_ = nowUs + holdUs;  // release gap = same duration -> symmetric pulses
        }
    } else if (nowUs >= camFireNextUs_) {
        if (!tryReserveNonCoreFinger()) {
            camFireNextUs_ = nowUs + holdUs;  // budget full: retry next pulse
            return;
        }
        toFrame(fire->pos, camFireX_, camFireY_);
        camFireDown_ = true;
        camFireStartUs_ = nowUs;
        emit(MotionAction::Down, kAutoFirePointerId, camFireX_, camFireY_);
    }
}

void InputEngine::setLocked(bool locked) {
    if (locked == locked_) return;
    locked_ = locked;
    if (!locked) {
        if (camActive_) {
            camActive_ = false;
            emit(MotionAction::Up, kCameraPointerId, camX_, camY_);
        }
        // Safety: never leave auto-fire or smoothing/return state running unlocked.
        stopAutoFire();
        camAvgX_ = camAvgY_ = 0.0f;
        camAvgTickUs_ = camAvgLastUs_ = 0;
        camLastMoveUs_ = 0;
        camRetActive_ = false;
    }
    if (locked && directActive_) directPointer(false, {});
}

void InputEngine::directPointer(bool down, Vec2 n) {
    if (down && !directActive_) {
        if (!tryReserveFinger()) return;
        toFrame(n, directX_, directY_);
        directActive_ = true;
        emit(MotionAction::Down, proto::kPointerIdGenericFinger, directX_, directY_);
    } else if (!down && directActive_) {
        directActive_ = false;
        emit(MotionAction::Up, proto::kPointerIdGenericFinger, directX_, directY_);
    }
}

void InputEngine::directMove(Vec2 n) {
    if (!directActive_) return;
    toFrame(n, directX_, directY_);
    emit(MotionAction::Move, proto::kPointerIdGenericFinger, directX_, directY_);
}
void InputEngine::tick(int64_t nowUs) {
    // Both the ~1 ms input thread and the UI frame loop call tick(); a caller
    // that read its clock before waiting on inputMu_ can arrive with a stale
    // timestamp. Clamp to monotonic so recoil/animation dt never goes negative.
    if (nowUs < lastTickNowUs_) nowUs = lastTickNowUs_;
    else lastTickNowUs_ = nowUs;

    // Camera pipeline: smoothing drain, auto-release, return-to-center, auto-fire.
    if (const Binding* cam = findCamera()) {
        tickCamera(nowUs, cam);
        tickAutoFire(nowUs, cam);
    } else if (camFireDown_ || camFireNextUs_) {
        stopAutoFire();
    }

    // Combo deferral: fire single-key bindings whose combo window expired (no second key was pressed)
    for (std::size_t i = 0; i < singleDeferUs_.size(); ++i) {
        if (singleDeferUs_[i] == 0) continue;
        if (nowUs < singleDeferUs_[i]) continue;
        singleDeferUs_[i] = 0;
        const Binding& b = profile_.bindings[i];
        if (b.key2 != 0) continue;  // safety: only single-key bindings
        // Re-check: key still held and no combo currently active
        bool wantDown = (b.key > 0 && b.key < 256 && keys_[b.key]);
        if (wantDown) {
            for (std::size_t j = 0; j < profile_.bindings.size(); ++j) {
                const Binding& o = profile_.bindings[j];
                if (o.key2 != 0 && (o.key == b.key || o.key2 == b.key) &&
                    o.key > 0 && o.key < 256 && o.key2 > 0 && o.key2 < 256 && keys_[o.key] && keys_[o.key2]) {
                    wantDown = false;
                    break;
                }
            }
        }
        if (!wantDown || b.disabled) continue;
        const uint64_t id = kBindingPointerBase + i;
        float x, y;
        toFrame(b.pos, x, y);
        switch (b.type) {
            case BindingType::Tap:
                if (!tapActive_[i] && tryReserveNonCoreFinger()) tapFingerDown(i, nowUs);
                break;
            case BindingType::Drag:
                if (!drags_[i].active && tryReserveNonCoreFinger()) {
                    drags_[i].active = true;
                    drags_[i].startUs = nowUs;
                    toFrame(b.pos, drags_[i].x, drags_[i].y);
                    emit(MotionAction::Down, id, drags_[i].x, drags_[i].y);
                }
                break;
            case BindingType::Fire:
                if (!fires_[i].held && reserveFireFinger()) startFire(i, nowUs);
                break;
            default: break;
        }
    }

    // Quick-tap auto-release timer + tap-repeat pulse trains
    for (std::size_t i = 0; i < tapActive_.size(); ++i) {
        TapPulse& tp = tapPulses_[i];
        if (tp.downPhase && nowUs >= tp.nextUs) {
            // End of a repeat pulse's dwell: lift the finger, then re-press after a gap.
            const Binding& b = profile_.bindings[i];
            float x, y;
            toFrame(b.pos, x, y);
            tp.downPhase = false;
            emit(MotionAction::Up, kBindingPointerBase + i, x, y);
            if (tp.left <= 0) {
                tapActive_[i] = false;
                tapReleaseAtUs_[i] = 0;
            } else {
                tp.nextUs = nowUs + static_cast<int64_t>(std::max(15, b.tapHoldMs)) * 1000;
            }
        } else if (!tp.downPhase && tp.left > 0 && tapActive_[i] && nowUs >= tp.nextUs) {
            const Binding& b = profile_.bindings[i];
            float x, y;
            toFrame(b.pos, x, y);
            --tp.left;
            tp.downPhase = true;
            tp.nextUs = nowUs + static_cast<int64_t>(std::max(15, b.tapHoldMs)) * 1000;
            emit(MotionAction::Down, kBindingPointerBase + i, x, y);
        }
        if (tapActive_[i] && tapPulses_[i].left == 0 && !tapPulses_[i].downPhase && tapReleaseAtUs_[i] > 0 &&
            nowUs >= tapReleaseAtUs_[i]) {
            tapFingerUp(i);
        }
    }

    // Joystick release delay, smooth human-like step interpolation, and micro-shake
    for (std::size_t i = 0; i < joys_.size(); ++i) {
        JoyState& js = joys_[i];
        const Binding& b = profile_.bindings[i];
        const uint64_t id = kBindingPointerBase + i;
        if (!joyActive_[i]) {
            // Direction keys are held but the finger never landed: the finger
            // budget was exhausted at press time, or a frame-size change lifted
            // it. Retry instead of waiting for the next direction key edge.
            if (b.type == BindingType::Joystick && nowUs >= js.retryAtUs) {
                const float rdx = static_cast<float>((keys_[b.keyRight] ? 1 : 0) - (keys_[b.keyLeft] ? 1 : 0));
                const float rdy = static_cast<float>((keys_[b.keyDown] ? 1 : 0) - (keys_[b.keyUp] ? 1 : 0));
                if (rdx != 0.0f || rdy != 0.0f) {
                    js.retryAtUs = nowUs + 100000;
                    updateJoystick(i, nowUs);
                }
            }
            continue;
        }
        if (js.releaseAtUs > 0) {
            if (nowUs >= js.releaseAtUs) {
                float cx, cy;
                toFrame(b.pos, cx, cy);
                joyActive_[i] = false;
                js.active = false;
                js.releaseAtUs = 0;
                emit(MotionAction::Up, id, cx, cy);
            }
            continue;
        }
        if (b.joyProfile == 2) {
            if (nowUs >= js.nextStepUs) {
                // Cadence with bounded timing jitter (±joyNatTimeVarPct).
                const int baseMs = std::clamp(b.joyNatSegMs, 8, 60);
                const int varPct = std::clamp(b.joyNatTimeVarPct, 0, 50);
                js.nextStepUs = nowUs + static_cast<int64_t>(
                    baseMs * 1000.0f * (1.0f + nextSignedRand(js.rng) * varPct * 0.01f));

                if (!js.converged && nowUs >= js.pathStartUs) {
                    const float t =
                        std::clamp(static_cast<float>(nowUs - js.pathStartUs) / static_cast<float>(js.pathDurUs),
                                   0.0f, 1.0f);
                    float p;
                    switch (std::clamp(b.joyNatEase, 0, 2)) {
                        case 0: p = t; break;
                        case 2: p = t * t * (3.0f - 2.0f * t); break;      // ease-in-out
                        default: p = 1.0f - (1.0f - t) * (1.0f - t); break; // ease-out
                    }
                    const float pathDx = js.pathTX - js.pathSX, pathDy = js.pathTY - js.pathSY;
                    const float pathLen = std::sqrt(pathDx * pathDx + pathDy * pathDy);
                    // Segment spacing cap: a single emit may advance at most
                    // joyNatSpacingPx along the path, producing the "several small
                    // consecutive transitions" seen on real mapper sticks.
                    if (b.joyNatSpacingPx > 0.5f && pathLen > 0.5f)
                        p = std::min(p, js.alongMax + b.joyNatSpacingPx / pathLen);
                    // Monotonic: the stick can never slide back along its direction.
                    p = std::clamp(std::max(p, js.alongMax), 0.0f, 1.0f);
                    js.alongMax = p;
                    // Bounded lateral offset, smoothed and zero at both endpoints.
                    js.latOff += (js.latAmp * nextSignedRand(js.rng) - js.latOff) * 0.35f;
                    const float lat = js.latOff * std::sin(p * 3.14159265f);
                    float outX = js.pathTX, outY = js.pathTY;
                    if (pathLen > 0.5f) {
                        const float dirx = pathDx / pathLen, diry = pathDy / pathLen;
                        outX = js.pathSX + dirx * pathLen * p - diry * lat;
                        outY = js.pathSY + diry * pathLen * p + dirx * lat;
                    }
                    if (p >= 1.0f) {
                        outX = js.pathTX;
                        outY = js.pathTY;
                        js.converged = true;
                    }
                    js.curX = outX;
                    js.curY = outY;
                    js.lastEmitUs = nowUs;
                    if (trace_)
                        log::info("joy " + std::to_string(i) + " natural p=" + std::to_string(p) + " -> (" +
                                  std::to_string(static_cast<int>(outX)) + "," + std::to_string(static_cast<int>(outY)) +
                                  ")");
                    emit(MotionAction::Move, id, outX, outY);
                } else {
                    // Converged (or still in the initial delay): refresh the held
                    // position at cadence so a stale-dropped Move can never freeze
                    // the game stick.
                    js.lastEmitUs = nowUs;
                    emit(MotionAction::Move, id, js.curX, js.curY);
                }
            }
        } else if (b.joyHumanize && nowUs >= js.nextStepUs) {
            // Slight timing jitter so steps don't fall on a perfect grid
            const int baseMs = std::clamp(b.joyIntervalMs, 16, 100);
            const int jitter = static_cast<int>((nextSignedRand(js.rng) + 1.0f) * 2.0f);  // 0..4 ms
            js.nextStepUs = nowUs + static_cast<int64_t>(baseMs + jitter) * 1000;

            // --- Target wandering: slow drift around the ideal target ---
            const float wanderAmp = std::clamp(b.joyRandomOffset, 0.1f, 1.0f) * 2.0f;
            const float tSec = static_cast<float>(nowUs) * 1e-6f;
            // Two incommensurate low-frequency sines => organic drift, never repeats exactly
            js.wanderX = (std::sin(tSec * 0.7f * 6.2831f) * 0.6f + std::sin(tSec * 1.3f * 6.2831f) * 0.4f) * wanderAmp;
            js.wanderY = (std::cos(tSec * 0.5f * 6.2831f) * 0.6f + std::sin(tSec * 1.1f * 6.2831f) * 0.4f) * wanderAmp;

            const float goalX = js.targetX + js.wanderX;
            const float goalY = js.targetY + js.wanderY;

            // --- Critically-damped spring: smooth accel from center, smooth decel near target ---
            // omega is fast (~12 Hz) so the finger reaches target in ~80-120 ms like a real finger.
            // step length controls extra smoothing: larger step = snappier, smaller = softer.
            const float step = std::clamp(b.joyStepLength, 0.01f, 0.50f);
            const float dt = static_cast<float>(baseMs + jitter) * 0.001f;  // seconds
            const float omega = (12.0f + step * 16.0f) * std::clamp(b.joySpeed, 0.25f, 4.0f);
            const float k = omega * omega;             // spring constant
            const float c = 2.0f * omega;              // critical damping coefficient
            const float ax = (goalX - js.curX) * k - js.velX * c;
            const float ay = (goalY - js.curY) * k - js.velY * c;
            js.velX += ax * dt;
            js.velY += ay * dt;
            js.curX += js.velX * dt;
            js.curY += js.velY * dt;

            // --- Continuous micro-jitter (multi-frequency noise, not a single sine) ---
            float outX = js.curX, outY = js.curY;
            if (b.joyShake) {
                const float amp = std::clamp(b.joyShakeAmp, 0.1f, 10.0f);
                // Three incommensurate frequencies => irregular, non-circular jitter
                const float p1 = tSec * (1000.0f / std::clamp(b.joyShakePeriodMs, 100, 500)) * 6.2831f;
                const float p2 = tSec * 2.7f * 6.2831f;
                const float p3 = tSec * 4.13f * 6.2831f;
                outX += (std::sin(p1) * 0.5f + std::sin(p2) * 0.3f + std::sin(p3) * 0.2f) * amp;
                outY += (std::cos(p1 * 1.1f) * 0.5f + std::cos(p2 * 0.9f) * 0.3f + std::cos(p3 * 1.3f) * 0.2f) * amp;
            }
            js.lastEmitUs = nowUs;
            emit(MotionAction::Move, id, outX, outY);
        } else if (!b.joyHumanize && b.joyProfile != 2 && nowUs - js.lastEmitUs > 30000) {
            // Heartbeat: non-humanized joysticks only emit on direction change, so
            // re-send the held position periodically. A stale-dropped or lost Move
            // can then never freeze the game stick at an old direction.
            js.lastEmitUs = nowUs;
            emit(MotionAction::Move, id, js.curX, js.curY);
        }
    }

    for (std::size_t i = 0; i < drags_.size(); ++i) {
        DragState& d = drags_[i];
        if (!d.active) continue;
        const Binding& b = profile_.bindings[i];
        float x0, y0, x1, y1;
        toFrame(b.pos, x0, y0);
        toFrame(b.pos2, x1, y1);
        if (d.x == x1 && d.y == y1) continue;
        const float t = b.durationMs <= 0 ? 1.0f
                                          : std::clamp(static_cast<float>(nowUs - d.startUs) / (b.durationMs * 1000.0f), 0.0f, 1.0f);
        d.x = x0 + (x1 - x0) * t;
        d.y = y0 + (y1 - y0) * t;
        emit(MotionAction::Move, kBindingPointerBase + i, d.x, d.y);
    }

    for (std::size_t i = 0; i < fires_.size(); ++i) {
        FireState& f = fires_[i];
        if (!f.held) continue;
        const Binding& b = profile_.bindings[i];
        const uint64_t id = kBindingPointerBase + i;

        // Rapid fire (Pistol & DMR mode): pulses the fire finger while held
        if (b.rapidFire && nowUs >= f.nextPulseUs) {
            const int64_t halfUs = std::max<int64_t>(10000, static_cast<int64_t>(b.rapidFireDelayMs) * 500);
            if (f.fingerDown) {
                f.fingerDown = false;
                emit(MotionAction::Up, id, f.x, f.y);
            } else {
                toFrame(b.pos, f.x, f.y);
                f.fingerDown = true;
                emit(MotionAction::Down, id, f.x, f.y);
            }
            f.nextPulseUs = nowUs + halfUs;
        }

        // Smooth sub-pixel recoil compensation while holding the fire button
        const int64_t dtUs = std::clamp<int64_t>(nowUs - f.lastTickUs, 0, 100000);
        f.lastTickUs = nowUs;
        const int netX = b.recoilRight - b.recoilLeft;
        const int netY = b.recoilDown - b.recoilUp;
        if (b.recoilEnabled && (netX != 0 || netY != 0) && dtUs > 0 && frameH_ > 0 &&
            (nowUs - f.startUs >= static_cast<int64_t>(b.recoilDelayMs) * 1000)) {
            const float dtSec = static_cast<float>(dtUs) / 1000000.0f;
            // Two spaces: the camera path consumes DEVICE pixels (applyCameraPixelDelta
            // scales them into the frame), the fire-finger path works in frame pixels
            // directly. Feeding frame-sized deltas into the device-pixel path used to
            // shrink recoil by frameW/devW — roughly half at 480p.
            const float scaleDev = devH_ ? 0.00275f * static_cast<float>(devH_) * dtSec
                                        : 0.00275f * static_cast<float>(frameH_) * dtSec;
            const float scale = 0.00275f * static_cast<float>(frameH_) * dtSec;
            const float fdx = static_cast<float>(netX) * scale;
            const float fdy = static_cast<float>(netY) * scale;
            if (locked_ && findCamera() != nullptr) {
                applyCameraPixelDelta(static_cast<float>(netX) * scaleDev, static_cast<float>(netY) * scaleDev, nowUs);
            } else if (f.fingerDown) {
                float ax, ay;
                toFrame(b.pos, ax, ay);
                const float maxR = 0.35f * static_cast<float>(frameH_);
                float nx = f.x + fdx, ny = f.y + fdy;
                if (std::hypot(nx - ax, ny - ay) > maxR || nx < 2 || ny < 2 ||
                    nx >= static_cast<float>(frameW_) - 2 || ny >= static_cast<float>(frameH_) - 2) {
                    emit(MotionAction::Up, id, f.x, f.y);
                    f.x = ax, f.y = ay;
                    emit(MotionAction::Down, id, f.x, f.y);
                    nx = f.x + fdx, ny = f.y + fdy;
                }
                f.x = nx, f.y = ny;
                emit(MotionAction::Move, id, f.x, f.y);
            }
        }
    }
}

void InputEngine::releaseBinding(std::size_t i) {
    if (i >= profile_.bindings.size()) return;
    const Binding& b = profile_.bindings[i];
    const uint64_t id = kBindingPointerBase + i;
    float x, y;
    toFrame(b.pos, x, y);
    if (i < tapActive_.size() && tapActive_[i]) {
        tapActive_[i] = false;
        emit(MotionAction::Up, id, x, y);
    }
    if (i < joyActive_.size() && joyActive_[i]) {
        joyActive_[i] = false;
        emit(MotionAction::Up, id, x, y);
    }
    if (i < drags_.size() && drags_[i].active) {
        drags_[i].active = false;
        emit(MotionAction::Up, id, drags_[i].x, drags_[i].y);
    }
    if (i < fires_.size() && fires_[i].held) {
        fires_[i].held = false;
        if (fires_[i].fingerDown) {
            fires_[i].fingerDown = false;
            emit(MotionAction::Up, id, fires_[i].x, fires_[i].y);
        }
    }
    if (b.type == BindingType::Camera) {
        if (camActive_) {
            camActive_ = false;
            camFracX_ = camFracY_ = 0.0f;
            emit(MotionAction::Up, kCameraPointerId, camX_, camY_);
        }
        stopAutoFire();
        camAvgX_ = camAvgY_ = 0.0f;
        camAvgTickUs_ = camAvgLastUs_ = 0;
        camLastMoveUs_ = 0;
        camRetActive_ = false;
    }
}

void InputEngine::releaseAll() {
    for (std::size_t i = 0; i < profile_.bindings.size(); ++i) releaseBinding(i);
    for (int f = 0; f < kMaxMacroFingers; ++f) {
        if (macroFingers_[f].active) {
            macroFingers_[f].active = false;
            macroFingers_[f].sources = 0;
            emit(MotionAction::Up, kMacroPointerBase + static_cast<uint64_t>(f), macroFingers_[f].x, macroFingers_[f].y);
        }
    }
    if (camActive_) {
        camActive_ = false;
        camFracX_ = camFracY_ = 0.0f;
        emit(MotionAction::Up, kCameraPointerId, camX_, camY_);
    }
    // Always lift the engine-owned auto-fire finger so no key stays stuck.
    stopAutoFire();
    camAvgX_ = camAvgY_ = 0.0f;
    camAvgTickUs_ = camAvgLastUs_ = 0;
    camLastMoveUs_ = 0;
    camRetActive_ = false;
    if (directActive_) {
        directActive_ = false;
        emit(MotionAction::Up, proto::kPointerIdGenericFinger, directX_, directY_);
    }
    keys_.fill(false);
    keySources_.fill(0);
    for (auto& d : singleDeferUs_) d = 0;
    for (auto& tp : tapPulses_) tp = {};
    for (auto& r : tapReleaseAtUs_) r = 0;
    for (auto& j : joys_) j.releaseAtUs = 0, j.active = false;
}

std::size_t InputEngine::activeTouches() const {
    std::size_t n = (camActive_ ? 1 : 0) + (directActive_ ? 1 : 0);
    for (bool b : tapActive_) n += b;
    for (bool b : joyActive_) n += b;
    for (const auto& d : drags_) n += d.active;
    for (const auto& f : fires_) n += f.fingerDown;
    for (const auto& mf : macroFingers_) n += mf.active;
    return n;
}

}  // namespace wasd
