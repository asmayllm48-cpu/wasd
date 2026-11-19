#include "core/automation.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <nlohmann/json.hpp>

namespace wasd {
using json = nlohmann::json;

// ---------------------------------------------------------------- color matching

static const std::array<float, 256>& srgbToLinear() {
    static const std::array<float, 256> lut = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = i / 255.0f;
            t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return lut;
}

Lab rgbToLab(uint8_t r8, uint8_t g8, uint8_t b8) {
    const auto& lin = srgbToLinear();
    const float r = lin[r8], g = lin[g8], b = lin[b8];
    // sRGB -> XYZ (D65), normalized by the white point.
    const float x = (0.4124564f * r + 0.3575761f * g + 0.1804375f * b) / 0.95047f;
    const float y = 0.2126729f * r + 0.7151522f * g + 0.0721750f * b;
    const float z = (0.0193339f * r + 0.1191920f * g + 0.9503041f * b) / 1.08883f;
    auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.0f / 116.0f; };
    const float fx = f(x), fy = f(y), fz = f(z);
    return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}

float deltaE(const Lab& p, const Lab& q) {
    const float dl = p.L - q.L, da = p.a - q.a, db = p.b - q.b;
    return std::sqrt(dl * dl + da * da + db * db);
}

ZoneMatch matchZone(const RgbView& img, float cx, float cy, float r, const Zone& z) {
    ZoneMatch m;
    m.perColor.assign(z.colors.size(), 0);
    if (!img.data || img.width <= 0 || img.height <= 0 || r <= 0 || z.colors.empty()) return m;
    std::vector<Lab> targets;
    targets.reserve(z.colors.size());
    for (const auto& c : z.colors) targets.push_back(rgbToLab(c.r, c.g, c.b));
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - r))), x1 = std::min(img.width - 1, static_cast<int>(std::ceil(cx + r)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - r))), y1 = std::min(img.height - 1, static_cast<int>(std::ceil(cy + r)));
    const float r2 = r * r;
    for (int y = y0; y <= y1; ++y) {
        const float dy = y + 0.5f - cy;
        const uint8_t* row = img.data + static_cast<size_t>(y) * img.stride;
        for (int x = x0; x <= x1; ++x) {
            const float dx = x + 0.5f - cx;
            if (dx * dx + dy * dy > r2) continue;  // circular mask
            ++m.total;
            const uint8_t* p = row + x * 3;
            const Lab lab = rgbToLab(p[0], p[1], p[2]);
            for (size_t i = 0; i < targets.size(); ++i)
                if (deltaE(lab, targets[i]) <= z.colors[i].tolerance) ++m.perColor[i];
        }
    }
    auto ok = [&](int n) { return n >= std::max(1, z.minPixels) && (z.minRatio <= 0 || (m.total && n >= z.minRatio * m.total)); };
    if (z.mode == ColorMatchMode::Any) m.present = std::any_of(m.perColor.begin(), m.perColor.end(), ok);
    else m.present = std::all_of(m.perColor.begin(), m.perColor.end(), ok);
    return m;
}

// ---------------------------------------------------------------- monitor

bool MonitorRuntime::onSample(bool detected, int64_t now, const Monitor& m, bool busy) {
    if (finished_ || !m.enabled) return false;
    if (detected) ++hits_, misses_ = 0;
    else ++misses_, hits_ = 0;
    const int need = std::clamp(m.confirm, 1, 20);
    const bool was = present_;
    if (!present_ && hits_ >= need) present_ = true;
    if (present_ && misses_ >= need) present_ = false;
    if (present_ && !was) lastDetect_ = now, pendingAt_ = now + int64_t(std::max(0, m.delayMs)) * 1000, absentSince_ = -1;
    if (!present_) {
        if (was) armed_ = true;
        pendingAt_ = -1;
        return false;
    }
    if (now < pendingAt_ || busy || now - lastFire_ < int64_t(std::max(0, m.cooldownMs)) * 1000) return false;
    switch (m.mode) {
        case TriggerMode::Once:
        case TriggerMode::Continuous: break;
        case TriggerMode::Rearm:
            if (!armed_) return false;
            armed_ = false;
            break;
        case TriggerMode::Limited:
            if (runs_ >= std::max(1, m.maxRuns)) return finished_ = true, false;
            break;
    }
    lastFire_ = now;
    ++runs_;
    if (m.mode == TriggerMode::Once || (m.mode == TriggerMode::Limited && runs_ >= std::max(1, m.maxRuns))) finished_ = true;
    return true;
}

bool MonitorRuntime::onAbsent(int64_t now, const Monitor& m) {
    if (present_ || finished_) return false;
    if (m.elseMacro < 0 && m.elseKey <= 0 && !m.elseStop) return false;
    if (absentSince_ < 0) absentSince_ = now;
    if (now - absentSince_ < int64_t(std::max(0, m.absentMs)) * 1000) return false;
    if (now - lastElseFire_ < int64_t(std::max(0, m.elseCooldownMs)) * 1000) return false;
    lastElseFire_ = now;
    return true;
}

// ---------------------------------------------------------------- macro runner

const char* runStateName(RunState s) {
    switch (s) {
        case RunState::Idle: return "Idle";
        case RunState::Running: return "Running";
        case RunState::Waiting: return "Waiting";
        case RunState::Completed: return "Completed";
        case RunState::Cancelled: return "Cancelled";
        case RunState::Failed: return "Failed";
    }
    return "";
}

bool MacroRunner::start(const Automation& a, int macro, int64_t now) {
    if (active() || macro < 0 || macro >= static_cast<int>(a.macros.size())) return false;
    ++epoch_;
    automation_ = a;
    macro_ = macro;
    stack_.clear();
    stack_.push_back({&automation_.macros[macro].steps});
    state_ = RunState::Running;
    executed_ = phase_ = 0;
    startUs_ = now;
    lastTickUs_ = now;
    maxDurationMs_ = std::clamp(automation_.macros[macro].maxDurationMs, 100, 600000);
    runMode_ = automation_.macros[macro].runMode;
    runCount_ = 0;
    lastFound_ = false;
    message_.clear();
    return true;
}

void MacroRunner::releaseAll() {
    for (int i = 0; i < kMaxFingers; ++i)
        if (fingerDown_[i]) {
            fingerDown_[i] = false;
            if (sinks_.touch) sinks_.touch(1, i, fingerPos_[i]);
        }
    for (int vk : keysDown_)
        if (sinks_.key) sinks_.key(vk, false);
    keysDown_.clear();
}

void MacroRunner::finish(RunState s, const std::string& msg) {
    ++epoch_;
    releaseAll();
    stack_.clear();
    current_ = nullptr;
    phase_ = 0;

    // Handle run modes: Continuous / UntilFound / UntilFail restart automatically.
    // Only restart on *completion* (not failure) for Until modes; Continuous
    // restarts on both. A failed Continuous macro also restarts, but with a
    // fresh start time and a reset lastFound_ so it cannot spin in a tight
    // loop after the max-duration check expires.
    const bool completed = (s == RunState::Completed);
    const bool failed = (s == RunState::Failed);
    if (runMode_ != MacroRunMode::Once && (completed || failed)) {
        ++runCount_;
        const bool shouldRestart =
            runMode_ == MacroRunMode::Continuous ||
            (runMode_ == MacroRunMode::UntilFound && !lastFound_) ||
            (runMode_ == MacroRunMode::UntilFail && lastFound_);
        if (shouldRestart) {
            // Restart with a fresh time window and reset found-state so the
            // max-duration check and UntilFail logic work correctly on each run.
            stack_.push_back({&automation_.macros[macro_].steps});
            state_ = RunState::Running;
            executed_ = phase_ = 0;
            startUs_ = lastTickUs_;  // reset the max-duration window
            lastFound_ = false;     // reset so UntilFail re-evaluates each run
            message_ = msg + " (restart #" + std::to_string(runCount_) + ")";
            return;
        }
    }
    state_ = s;
    message_ = msg;
}

void MacroRunner::fail(const std::string& why) { finish(RunState::Failed, why); }

void MacroRunner::cancel(const char* reason) {
    if (active()) finish(RunState::Cancelled, reason);
}

std::optional<bool> MacroRunner::condition(const Step& s) {
    if (s.image >= 0) {
        if (!sinks_.imagePresent) return std::nullopt;
        const auto r = sinks_.imagePresent(s.image);
        if (!r) return std::nullopt;
        return s.negate ? !*r : *r;
    }
    if (!sinks_.zonePresent) return std::nullopt;
    const auto r = sinks_.zonePresent(s.zone);
    if (!r) return std::nullopt;
    return s.negate ? !*r : *r;
}

void MacroRunner::tick(int64_t now) {
    if (!active()) return;
    lastTickUs_ = now;
    if (now - startUs_ > int64_t(maxDurationMs_) * 1000) return fail("time limit reached");
    runEpoch_ = epoch_;
    for (int guard = 0; guard < 2000 && active(); ++guard) {
        if (state_ == RunState::Waiting) {
            if (now < wakeUs_) return;
            state_ = RunState::Running;
        }
        if (!stepOnce(now, runEpoch_)) return;
    }
}

bool MacroRunner::waitFor(int64_t t) {
    wakeUs_ = t;
    state_ = RunState::Waiting;
    return false;
}

bool MacroRunner::advance() {
    stack_.back().index++;
    phase_ = 0;
    ++executed_;
    return true;
}

void MacroRunner::emitTouch(int action, int fi, Vec2 p) {
    if (!active() || epoch_ != runEpoch_) return;
    fingerPos_[fi] = p;
    fingerDown_[fi] = action != 1;
    if (sinks_.touch) sinks_.touch(action, fi, p);
}

void MacroRunner::emitKey(int vk, bool down) {
    if (!active() || epoch_ != runEpoch_) return;
    if (sinks_.key) sinks_.key(vk, down);
}

bool MacroRunner::stepOnce(int64_t now, uint64_t runEpoch) {
    if (!active() || epoch_ != runEpoch) return false;
    runEpoch_ = runEpoch;
    if (stack_.empty()) return finish(RunState::Completed, "completed"), false;
    Frame& f = stack_.back();
    auto waitUntil = [&](int64_t t) { return waitFor(t); };
    if (f.index >= f.list->size()) {
        if (f.loop) {
            const Step& L = *f.loop;
            ++f.iteration;
            const int limit = std::clamp(L.count, 1, kMaxLoopCount);
            bool again = f.iteration < limit;
            if (again && L.loopMode != LoopMode::Count) {
                const auto c = condition(L);
                if (!c) return fail("no video frame for color check"), false;
                again = L.loopMode == LoopMode::While ? *c : !*c;
            }
            if (again) {
                f.index = 0;
                if (L.ms > 0) return waitUntil(now + int64_t(L.ms) * 1000);
                return true;
            }
        }
        stack_.pop_back();
        if (stack_.empty()) return finish(RunState::Completed, "completed"), false;
        return true;
    }
    const Step& s = (*f.list)[f.index];
    current_ = &s;
    auto next = [&] { return advance(); };
    if (!s.enabled || s.type == StepType::Comment) return next();
    const int fi = std::clamp(s.finger, 0, kMaxFingers - 1);
    auto touch = [&](int action, Vec2 p) { emitTouch(action, fi, p); };
    auto keyFn = [&](int vk, bool down) { emitKey(vk, down); };
    const int64_t ms = int64_t(std::clamp(s.ms, 0, 600000)) * 1000;
    switch (s.type) {
        case StepType::Wait:
            if (phase_ == 0) return phase_ = 1, waitUntil(now + ms);
            return next();
        case StepType::Tap:
        case StepType::LongPress:
            if (phase_ == 0) {
                touch(0, s.a);
                phase_ = 1;
                return waitUntil(now + (s.type == StepType::Tap ? 40000 : std::max<int64_t>(ms, 1000)));
            }
            touch(1, s.a);
            return next();
        case StepType::PressDown:
            touch(fingerDown_[fi] ? 2 : 0, s.a);
            return next();
        case StepType::Release:
            if (fingerDown_[fi]) touch(1, fingerPos_[fi]);
            return next();
        case StepType::Swipe: {
            if (phase_ == 0) {
                swipeOwnsFinger_ = !fingerDown_[fi];
                touch(swipeOwnsFinger_ ? 0 : 2, s.a);
                phaseStartUs_ = now;
                phase_ = 1;
                return waitUntil(now + 8000);
            }
            const float t = ms > 0 ? std::min(1.0f, float(now - phaseStartUs_) / float(ms)) : 1.0f;
            touch(2, {s.a.x + (s.b.x - s.a.x) * t, s.a.y + (s.b.y - s.a.y) * t});
            if (t < 1.0f) return waitUntil(now + 8000);
            if (swipeOwnsFinger_) touch(1, s.b);
            return next();
        }
        case StepType::Key:
            if (s.key <= 0 || s.key > 0xFE) return fail("key step without a key"), false;
            if (s.keyMode == KeyMode::Up) {
                keyFn(s.key, false);
                keysDown_.erase(std::remove(keysDown_.begin(), keysDown_.end(), s.key), keysDown_.end());
                return next();
            }
            if (phase_ == 0) {
                keyFn(s.key, true);
                keysDown_.push_back(s.key);
                if (s.keyMode == KeyMode::Down) return next();
                phase_ = 1;
                return waitUntil(now + std::max<int64_t>(ms, 40000));
            }
            keyFn(s.key, false);
            keysDown_.erase(std::remove(keysDown_.begin(), keysDown_.end(), s.key), keysDown_.end());
            return next();
        case StepType::RunMacro:
            if (s.macro < 0 || s.macro >= static_cast<int>(automation_.macros.size())) return fail("macro not found"), false;
            if (static_cast<int>(stack_.size()) >= kMaxDepth) return fail("too many nested steps"), false;
            next();
            stack_.push_back({&automation_.macros[s.macro].steps});
            return true;
        case StepType::If: {
            const auto c = condition(s);
            if (!c) return fail("no video frame for color check"), false;
            if (static_cast<int>(stack_.size()) >= kMaxDepth) return fail("too many nested steps"), false;
            next();
            stack_.push_back({*c ? &s.then : &s.otherwise});
            return true;
        }
        case StepType::Loop: {
            if (static_cast<int>(stack_.size()) >= kMaxDepth) return fail("too many nested steps"), false;
            if (s.loopMode != LoopMode::Count) {
                const auto c = condition(s);
                if (!c) return fail("no video frame for color check"), false;
                if ((s.loopMode == LoopMode::While) != *c) return next();
            }
            next();
            stack_.push_back({&s.then, 0, &s, 0});
            return true;
        }
        case StepType::Break:
            while (!stack_.empty()) {
                const bool wasLoop = stack_.back().loop != nullptr;
                stack_.pop_back();
                if (wasLoop) break;
            }
            phase_ = 0;
            ++executed_;
            if (stack_.empty()) return finish(RunState::Completed, "completed"), false;
            return true;
        case StepType::Stop:
            ++executed_;
            return finish(RunState::Completed, "stopped by step"), false;
        case StepType::ImageRecognize: {
            if (s.image < 0 || s.image >= static_cast<int>(automation_.images.size()))
                return fail("reference image not found"), false;
            // phase 0: begin the search; phase 1: waiting for a detection; phase 2:
            // running the click sub-machine; phase 3: continuous re-arm.
            if (phase_ == 0) {
                phaseStartUs_ = now;
                imgAttempts_ = 0, imgSub_ = 0, imgTap2_ = false;
                imgCandIdx_ = 0, imgCands_.clear(), imgMatch_ = ImageMatch{};
                phase_ = 1;
            }
            if (phase_ == 2) return clickTick(s, now);
            // Timeout: ms > 0 is the search timeout; ms == 0 means a single immediate check.
            const int64_t timeoutUs = ms;
            const bool timedOut = timeoutUs <= 0 || now - phaseStartUs_ > timeoutUs;
            const ImageScanResult r =
                sinks_.imageScan ? sinks_.imageScan(s.image, imgQuery(s)) : ImageScanResult{};
            const bool definitive = r.available && r.paramsMatch;
            const bool hit = definitive && (s.negate ? !r.found : r.found);
            if (phase_ == 3) {
                // Continuous mode: wait for the image to disappear before re-arming;
                // timeout is the exit condition and completes the step normally.
                if (timedOut) return next();
                if (definitive && !hit) phase_ = 1, imgAttempts_ = 0;
                return waitUntil(now + imgPollUs(s));
            }
            if (hit) {
                if (!s.negate) {
                    if (const ImageMatch* m = pickMatch(s, r)) imgMatch_ = *m;
                    if (s.matchPolicy == MatchPolicy::All) imgCands_ = r.matches, imgCandIdx_ = 0;
                }
                lastFound_ = true;
                return imageFoundBranch(f, s, now);
            }
            if (definitive) ++imgAttempts_;
            // On timeout, only declare not-found after at least one definitive scan.
            // With ms==0 (single-check) or a very short timeout, the first GPU
            // readback may not have completed yet; declaring not-found immediately
            // would fire false negatives on every "click image" step.
            if (timedOut && imgAttempts_ == 0) return waitUntil(now + imgPollUs(s));
            if (timedOut || (s.retryCount > 0 && imgAttempts_ >= s.retryCount))
                return imageNotFoundBranch(f, s, now);
            return waitUntil(now + imgPollUs(s));
        }
        case StepType::ImageSearch:
        case StepType::ClickImage:
            // Legacy step types are converted to ImageRecognize while loading and
            // must never reach the runner.
            return fail("unsupported legacy image step"), false;
        case StepType::Comment: return next();
    }
    return next();
}

// ------------------------------------------------------------ ImageRecognize

ImageQuery imageQueryFor(const Step& s, const Automation& a) {
    ImageQuery q;
    if (s.image >= 0 && s.image < static_cast<int>(a.images.size())) {
        const ImageRef& r = a.images[s.image];
        q.region = s.regionMode == ImageRegionMode::Saved ? r.region
                   : s.regionMode == ImageRegionMode::FullScreen ? Rect{0.0f, 0.0f, 1.0f, 1.0f} : s.region;
        q.thresholdPct = s.thresholdPct > 0 ? s.thresholdPct : r.thresholdPct;
        q.intervalMs = s.intervalMs > 0 ? s.intervalMs : r.intervalMs;
    } else {
        q.region = s.regionMode == ImageRegionMode::Custom ? s.region : Rect{0.0f, 0.0f, 1.0f, 1.0f};
        q.thresholdPct = s.thresholdPct > 0 ? s.thresholdPct : 85;
        q.intervalMs = s.intervalMs > 0 ? s.intervalMs : 150;
    }
    q.wantMatches = s.matchPolicy != MatchPolicy::Best;
    return q;
}

ImageQuery MacroRunner::imgQuery(const Step& s) const { return imageQueryFor(s, automation_); }

int64_t MacroRunner::imgPollUs(const Step& s) const {
    int iv = s.retryIntervalMs;
    if (iv <= 0) {
        iv = s.intervalMs;
        if (iv <= 0 && s.image >= 0 && s.image < static_cast<int>(automation_.images.size()))
            iv = automation_.images[s.image].intervalMs;
        if (iv <= 0) iv = 150;
    }
    return int64_t(std::clamp(iv, 16, 60000)) * 1000;
}

const ImageMatch* MacroRunner::pickMatch(const Step& s, const ImageScanResult& r) const {
    if (r.matches.empty()) return r.match.found ? &r.match : nullptr;
    switch (s.matchPolicy) {
        case MatchPolicy::All:
        case MatchPolicy::Best: return &r.matches.front();
        case MatchPolicy::First: {
            // First in scan order: topmost, then leftmost.
            const ImageMatch* best = &r.matches.front();
            for (const ImageMatch& m : r.matches)
                if (m.center.y < best->center.y - 1e-4f ||
                    (std::abs(m.center.y - best->center.y) <= 1e-4f && m.center.x < best->center.x))
                    best = &m;
            return best;
        }
        case MatchPolicy::Nearest: {
            const ImageMatch* best = &r.matches.front();
            float bd = FLT_MAX;
            for (const ImageMatch& m : r.matches) {
                const float dx = m.center.x - s.nearTo.x, dy = m.center.y - s.nearTo.y;
                const float d = dx * dx + dy * dy;
                if (d < bd) bd = d, best = &m;
            }
            return best;
        }
    }
    return &r.matches.front();
}

Vec2 MacroRunner::clickPointFor(const Step& s) const {
    if (s.foundAction == ImageFoundAction::ClickPoint)
        return {std::clamp(s.foundClick.x, 0.0f, 1.0f), std::clamp(s.foundClick.y, 0.0f, 1.0f)};
    const ImageMatch& m =
        (s.matchPolicy == MatchPolicy::All && imgCandIdx_ < imgCands_.size()) ? imgCands_[imgCandIdx_] : imgMatch_;
    Vec2 p{m.rect.x + s.clickOfs.x * m.rect.w, m.rect.y + s.clickOfs.y * m.rect.h};
    return {std::clamp(p.x, 0.0f, 1.0f), std::clamp(p.y, 0.0f, 1.0f)};
}

bool MacroRunner::imageStepDone(const Step& s, int64_t now) {
    if (s.continuous) {
        phase_ = 3;  // re-arm: wait for the image to disappear, then search again
        return waitFor(now + imgPollUs(s));
    }
    // The optional `then` list runs right after the found action ("what also
    // happens after the image is found / clicked").
    const bool seqAfter = s.foundAction == ImageFoundAction::Continue ||
                          s.foundAction == ImageFoundAction::ClickMatch ||
                          s.foundAction == ImageFoundAction::ClickPoint;
    if (seqAfter && !s.then.empty()) {
        if (static_cast<int>(stack_.size()) >= kMaxDepth) return fail("too many nested steps"), false;
        advance();
        stack_.push_back({&s.then, 0, nullptr, 0});
        return true;
    }
    return advance();
}

bool MacroRunner::imageFoundBranch(Frame& f, const Step& s, int64_t now) {
    switch (s.foundAction) {
        case ImageFoundAction::Continue: return imageStepDone(s, now);
        case ImageFoundAction::ClickPoint:
        case ImageFoundAction::ClickMatch:
            // Nothing to click when the image is absent (negated condition hit).
            if (s.foundAction == ImageFoundAction::ClickMatch && !imgMatch_.found) return imageStepDone(s, now);
            phase_ = 2, imgSub_ = 0, imgTap2_ = false, imgCandIdx_ = 0;
            return clickTick(s, now);
        case ImageFoundAction::RunMacro:
            if (s.foundMacro < 0 || s.foundMacro >= static_cast<int>(automation_.macros.size()))
                return fail("macro not found"), false;
            if (static_cast<int>(stack_.size()) + (s.then.empty() ? 0 : 1) >= kMaxDepth)
                return fail("too many nested steps"), false;
            advance();
            // `then` is pushed below the macro so it runs after the macro finishes.
            if (!s.then.empty()) stack_.push_back({&s.then, 0, nullptr, 0});
            stack_.push_back({&automation_.macros[s.foundMacro].steps, 0, nullptr, 0});
            return true;
        case ImageFoundAction::RunSteps:
            if (static_cast<int>(stack_.size()) >= kMaxDepth) return fail("too many nested steps"), false;
            advance();
            stack_.push_back({&s.then, 0, nullptr, 0});
            return true;
        case ImageFoundAction::JumpStep:
            if (s.jumpTo < 0 || s.jumpTo >= static_cast<int>(f.list->size()))
                return fail("jump target out of range"), false;
            f.index = s.jumpTo;
            phase_ = 0;
            ++executed_;
            return true;
        case ImageFoundAction::Stop: return finish(RunState::Completed, "image found, stopping"), false;
    }
    return imageStepDone(s, now);
}

bool MacroRunner::imageNotFoundBranch(Frame& f, const Step& s, int64_t now) {
    switch (s.notFoundAction) {
        case ImageNotFoundAction::Continue: return advance();
        case ImageNotFoundAction::RetrySearch:
            // Reset the attempt budget; the overall timeout still bounds the search.
            imgAttempts_ = 0;
            phase_ = 1;
            return waitFor(now + imgPollUs(s));
        case ImageNotFoundAction::RunSteps:
            if (static_cast<int>(stack_.size()) >= kMaxDepth) return fail("too many nested steps"), false;
            advance();
            stack_.push_back({&s.otherwise, 0, nullptr, 0});
            return true;
        case ImageNotFoundAction::JumpStep:
            if (s.jumpTo < 0 || s.jumpTo >= static_cast<int>(f.list->size()))
                return fail("jump target out of range"), false;
            f.index = s.jumpTo;
            phase_ = 0;
            ++executed_;
            return true;
        case ImageNotFoundAction::Stop: return finish(RunState::Completed, "image not found, stopping"), false;
        case ImageNotFoundAction::Fail: return fail("image not found"), false;
    }
    return advance();
}

bool MacroRunner::clickTick(const Step& s, int64_t now) {
    const int fi = std::clamp(s.finger, 0, kMaxFingers - 1);
    const int64_t holdUs = s.clickKind == ClickKind::Tap ? 40000 : int64_t(std::clamp(s.pressMs, 1, 600000)) * 1000;
    for (int guard = 0; guard < 8; ++guard) {
        switch (imgSub_) {
            case 0:  // optional delay before the click
                imgSub_ = 1;
                if (s.delayMs > 0) return waitFor(now + int64_t(std::clamp(s.delayMs, 0, 600000)) * 1000);
                continue;
            case 1:  // finger down
                emitTouch(0, fi, clickPointFor(s));
                imgSub_ = 2;
                return waitFor(now + holdUs);
            case 2:  // finger up
                emitTouch(1, fi, clickPointFor(s));
                if (s.clickKind == ClickKind::DoubleTap && !imgTap2_) {
                    imgTap2_ = true;
                    imgSub_ = 3;
                    return waitFor(now + 80000);
                }
                imgSub_ = 4;
                continue;
            case 3:  // second tap of a DoubleTap
                imgSub_ = 1;
                continue;
            case 4:  // next candidate for the All policy
                if (s.matchPolicy == MatchPolicy::All && imgCandIdx_ + 1 < imgCands_.size()) {
                    ++imgCandIdx_;
                    imgTap2_ = false;
                    imgSub_ = 1;
                    return waitFor(now + 80000);
                }
                return imageStepDone(s, now);
        }
    }
    return imageStepDone(s, now);
}

// ---------------------------------------------------------------- validation

static void validateSteps(const std::vector<Step>& steps, const Automation& a, int depth, int& total, const std::string& where,
                          std::vector<std::string>& errs) {
    if (depth > kMaxDepth) return errs.push_back(where + ": nesting deeper than " + std::to_string(kMaxDepth));
    auto in01 = [](Vec2 v) { return v.x >= 0 && v.x <= 1 && v.y >= 0 && v.y <= 1; };
    for (size_t i = 0; i < steps.size(); ++i) {
        const Step& s = steps[i];
        const std::string w = where + " step " + std::to_string(i + 1);
        if (++total > kMaxSteps) return errs.push_back(where + ": more than " + std::to_string(kMaxSteps) + " steps");
        if (s.ms < 0 || s.ms > 600000) errs.push_back(w + ": time out of range");
        if (!in01(s.a) || !in01(s.b)) errs.push_back(w + ": position out of range");
        if (s.finger < 0 || s.finger >= kMaxFingers) errs.push_back(w + ": invalid finger");
        if (s.key < 0 || s.key > 0xFE) errs.push_back(w + ": invalid key");
        if (s.type == StepType::RunMacro && (s.macro < 0 || s.macro >= static_cast<int>(a.macros.size())))
            errs.push_back(w + ": macro not found");
        if (s.type == StepType::ImageSearch || s.type == StepType::ClickImage)
            errs.push_back(w + ": legacy image step must be migrated to Image recognition");
        if (s.type == StepType::ImageRecognize) {
            if (s.image < 0 || s.image >= static_cast<int>(a.images.size()))
                errs.push_back(w + ": reference image not found");
            if (s.regionMode == ImageRegionMode::Custom &&
                (s.region.x < 0 || s.region.y < 0 || s.region.w <= 0 || s.region.h <= 0 ||
                 s.region.x + s.region.w > 1.001f || s.region.y + s.region.h > 1.001f))
                errs.push_back(w + ": search region out of the screen");
            if (s.thresholdPct < 0 || s.thresholdPct > 100 || s.retryCount < 0 || s.retryCount > 1000 ||
                s.retryIntervalMs < 0 || s.intervalMs < 0 || s.delayMs < 0 || s.pressMs < 0)
                errs.push_back(w + ": invalid recognition setting");
            if (s.continuous && s.ms <= 0)
                errs.push_back(w + ": continuous search needs a timeout as exit condition");
            if ((s.foundAction == ImageFoundAction::JumpStep || s.notFoundAction == ImageNotFoundAction::JumpStep) &&
                (s.jumpTo < 0 || s.jumpTo >= static_cast<int>(steps.size())))
                errs.push_back(w + ": jump target out of range");
        }
        const bool needsSource = s.type == StepType::If || (s.type == StepType::Loop && s.loopMode != LoopMode::Count);
        if (needsSource) {
            if (s.image >= 0) {
                if (s.image >= static_cast<int>(a.images.size())) errs.push_back(w + ": reference image not found");
            } else if (s.zone < 0 || s.zone >= static_cast<int>(a.zones.size())) {
                errs.push_back(w + ": condition source not found (zone or image)");
            }
        }
        if (s.type == StepType::Loop && (s.count < 1 || s.count > kMaxLoopCount)) errs.push_back(w + ": invalid loop count");
        validateSteps(s.then, a, depth + 1, total, w, errs);
        validateSteps(s.otherwise, a, depth + 1, total, w, errs);
    }
}

std::vector<std::string> validateAutomation(const Automation& a) {
    std::vector<std::string> errs;
    for (size_t i = 0; i < a.zones.size(); ++i) {
        const Zone& z = a.zones[i];
        const std::string w = "zone " + std::to_string(i + 1);
        if (z.center.x < 0 || z.center.x > 1 || z.center.y < 0 || z.center.y > 1) errs.push_back(w + ": center out of range");
        if (!(z.radius > 0 && z.radius <= 0.5f)) errs.push_back(w + ": invalid radius");
        if (z.colors.size() > 48) errs.push_back(w + ": too many colors");
        for (const auto& c : z.colors)
            if (!(c.tolerance >= 0 && c.tolerance <= 100)) errs.push_back(w + ": invalid tolerance");
        if (z.minPixels < 1 || z.minRatio < 0 || z.minRatio > 1) errs.push_back(w + ": invalid match threshold");
    }
    for (size_t i = 0; i < a.macros.size(); ++i) {
        int total = 0;
        validateSteps(a.macros[i].steps, a, 1, total, "macro " + std::to_string(i + 1), errs);
        if (a.macros[i].key < 0 || a.macros[i].key > 0xFE) errs.push_back("macro " + std::to_string(i + 1) + ": invalid key");
    }
    for (size_t i = 0; i < a.monitors.size(); ++i) {
        const Monitor& m = a.monitors[i];
        const std::string w = "monitor " + std::to_string(i + 1);
        if (m.image >= 0) {
            if (m.image >= static_cast<int>(a.images.size())) errs.push_back(w + ": reference image not found");
        } else if (m.zone < 0 || m.zone >= static_cast<int>(a.zones.size())) {
            errs.push_back(w + ": color zone not found");
        }
        if (m.intervalMs < 10 || m.intervalMs > 5000) errs.push_back(w + ": interval must be 10..5000 ms");
        if (m.confirm < 1 || m.confirm > 20 || m.cooldownMs < 0 || m.delayMs < 0 || m.maxRuns < 1) errs.push_back(w + ": invalid timing");
        if (m.macro >= static_cast<int>(a.macros.size())) errs.push_back(w + ": macro not found");
        if (m.elseMacro >= static_cast<int>(a.macros.size())) errs.push_back(w + ": else macro not found");
        if (m.elseMacro == m.macro && m.elseMacro >= 0) errs.push_back(w + ": else action must differ from the action");
        if ((m.elseMacro >= 0 || m.elseKey > 0 || m.elseStop) && (m.absentMs < 0 || m.elseCooldownMs < 50))
            errs.push_back(w + ": invalid else timing");
    }
    for (size_t i = 0; i < a.images.size(); ++i) {
        const ImageRef& r = a.images[i];
        const std::string w = "image " + std::to_string(i + 1);
        if (i >= kMaxImages) errs.push_back(w + ": too many images");
        // `rgb` is file-backed (loaded by the app after the metadata), so an empty crop is
        // not an error here; only metadata sanity is checked at parse time.
        if (r.w == 0 || r.h == 0 || (!r.rgb.empty() && r.rgb.size() != size_t(r.w) * r.h * 3))
            errs.push_back(w + ": corrupt crop metadata");
        if (r.thresholdPct < 1 || r.thresholdPct > 100) errs.push_back(w + ": threshold must be 1..100");
        if (r.intervalMs < 10 || r.intervalMs > 5000) errs.push_back(w + ": interval must be 10..5000 ms");
        if (r.region.x < 0 || r.region.y < 0 || r.region.w <= 0 || r.region.h <= 0 ||
            r.region.x + r.region.w > 1.001f || r.region.y + r.region.h > 1.001f)
            errs.push_back(w + ": search region out of the screen");
        if (r.clickOffset.x < 0 || r.clickOffset.x > 1 || r.clickOffset.y < 0 || r.clickOffset.y > 1)
            errs.push_back(w + ": click point out of the image");
    }
    for (size_t i = 0; i < a.studioGraphs.size(); ++i) {
        for (const auto& ge : studio::validateGraph(a.studioGraphs[i]))
            errs.push_back("studio graph '" + a.studioGraphs[i].name + "': " + ge);
    }
    return errs;
}

// ---------------------------------------------------------------- JSON

static json vec(Vec2 v) { return json::array({v.x, v.y}); }
static Vec2 vec(const json& j, Vec2 def) {
    if (!j.is_array() || j.size() != 2) return def;
    return {j[0].get<float>(), j[1].get<float>()};
}

static json rectOf(const Rect& r) { return json::array({r.x, r.y, r.w, r.h}); }
static Rect rectOf(const json& j, Rect def) {
    if (!j.is_array() || j.size() != 4) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
}

static json stepToJson(const Step& s) {
    json o{{"type", static_cast<int>(s.type)}, {"enabled", s.enabled}, {"ms", s.ms},       {"a", vec(s.a)},
           {"b", vec(s.b)},                    {"finger", s.finger},   {"key", s.key},     {"keyMode", static_cast<int>(s.keyMode)},
           {"macro", s.macro},                 {"zone", s.zone},       {"image", s.image}, {"negate", s.negate},
           {"loopMode", static_cast<int>(s.loopMode)},
           {"count", s.count},                 {"text", s.text},
           {"foundAction", static_cast<int>(s.foundAction)},
           {"notFoundAction", static_cast<int>(s.notFoundAction)},
           {"foundClick", vec(s.foundClick)}, {"foundMacro", s.foundMacro}};
    // Image recognition settings: written only when they differ from the defaults.
    if (s.thresholdPct > 0) o["thrPct"] = s.thresholdPct;
    if (s.regionMode != ImageRegionMode::Saved) {
        o["regMode"] = static_cast<int>(s.regionMode);
        if (s.regionMode == ImageRegionMode::Custom) o["reg"] = rectOf(s.region);
    }
    if (s.retryCount > 0) o["retryCount"] = s.retryCount;
    if (s.retryIntervalMs > 0) o["retryMs"] = s.retryIntervalMs;
    if (s.continuous) o["continuous"] = true;
    if (s.intervalMs > 0) o["scanMs"] = s.intervalMs;
    if (s.matchPolicy != MatchPolicy::Best) {
        o["matchPolicy"] = static_cast<int>(s.matchPolicy);
        if (s.matchPolicy == MatchPolicy::Nearest) o["nearTo"] = vec(s.nearTo);
    }
    if (s.clickKind != ClickKind::Tap) o["clickKind"] = static_cast<int>(s.clickKind);
    if (s.clickOfs.x != 0.5f || s.clickOfs.y != 0.5f) o["clickOfs"] = vec(s.clickOfs);
    if (s.delayMs > 0) o["delayMs"] = s.delayMs;
    if (s.pressMs != 60) o["pressMs"] = s.pressMs;
    if (s.jumpTo >= 0) o["jumpTo"] = s.jumpTo;
    if (!s.then.empty()) {
        json t = json::array();
        for (const auto& c : s.then) t.push_back(stepToJson(c));
        o["then"] = std::move(t);
    }
    if (!s.otherwise.empty()) {
        json t = json::array();
        for (const auto& c : s.otherwise) t.push_back(stepToJson(c));
        o["else"] = std::move(t);
    }
    return o;
}

static Step stepFromJson(const json& o, int depth) {
    if (depth > kMaxDepth) throw std::runtime_error("steps nested too deeply");
    Step s;
    const int type = o.value("type", 0);
    if (type < 0 || type > static_cast<int>(StepType::ImageRecognize)) throw std::runtime_error("unknown step type");
    s.type = static_cast<StepType>(type);
    const bool legacyImg = s.type == StepType::ImageSearch || s.type == StepType::ClickImage;
    s.enabled = o.value("enabled", true);
    s.ms = o.value("ms", 100);
    s.a = vec(o.value("a", json()), s.a);
    s.b = vec(o.value("b", json()), s.b);
    s.finger = o.value("finger", 0);
    s.key = o.value("key", 0);
    s.keyMode = static_cast<KeyMode>(std::clamp(o.value("keyMode", 0), 0, 2));
    s.macro = o.value("macro", -1);
    s.zone = o.value("zone", -1);
    s.image = o.value("image", -1);
    s.negate = o.value("negate", false);
    s.loopMode = static_cast<LoopMode>(std::clamp(o.value("loopMode", 0), 0, 2));
    s.count = o.value("count", 3);
    s.text = o.value("text", std::string());
    // Legacy steps store indexes into the OLD action enums; they are remapped by
    // migrateImageSteps after parsing (ClickImage had no action fields at all).
    s.foundAction = static_cast<ImageFoundAction>(std::clamp(o.value("foundAction", 0), 0, legacyImg ? 3 : 6));
    s.notFoundAction = static_cast<ImageNotFoundAction>(std::clamp(o.value("notFoundAction", 0), 0, legacyImg ? 2 : 5));
    s.foundClick = vec(o.value("foundClick", json()), s.foundClick);
    s.foundMacro = o.value("foundMacro", -1);
    s.thresholdPct = std::clamp(o.value("thrPct", 0), 0, 100);
    s.regionMode = static_cast<ImageRegionMode>(std::clamp(o.value("regMode", 0), 0, 2));
    s.region = rectOf(o.value("reg", json()), s.region);
    s.retryCount = std::clamp(o.value("retryCount", 0), 0, 1000);
    s.retryIntervalMs = std::clamp(o.value("retryMs", 0), 0, 600000);
    s.continuous = o.value("continuous", false);
    s.intervalMs = std::clamp(o.value("scanMs", 0), 0, 60000);
    s.matchPolicy = static_cast<MatchPolicy>(std::clamp(o.value("matchPolicy", 0), 0, 3));
    s.nearTo = vec(o.value("nearTo", json()), s.nearTo);
    s.clickKind = static_cast<ClickKind>(std::clamp(o.value("clickKind", 0), 0, 2));
    s.clickOfs = vec(o.value("clickOfs", json()), s.clickOfs);
    s.delayMs = std::clamp(o.value("delayMs", 0), 0, 600000);
    s.pressMs = std::clamp(o.value("pressMs", 60), 0, 600000);
    s.jumpTo = o.value("jumpTo", -1);
    if (o.contains("then"))
        for (const auto& c : o["then"]) s.then.push_back(stepFromJson(c, depth + 1));
    if (o.contains("else"))
        for (const auto& c : o["else"]) s.otherwise.push_back(stepFromJson(c, depth + 1));
    return s;
}

std::string automationToJson(const Automation& a) {
    json zones = json::array(), images = json::array(), macros = json::array(), monitors = json::array();
    for (const Zone& z : a.zones) {
        json colors = json::array();
        for (const auto& c : z.colors) colors.push_back({{"name", c.name}, {"rgb", {c.r, c.g, c.b}}, {"tolerance", c.tolerance}});
        zones.push_back({{"name", z.name},
                         {"center", vec(z.center)},
                         {"radius", z.radius},
                         {"colors", colors},
                         {"mode", z.mode == ColorMatchMode::All ? "all" : "any"},
                         {"minPixels", z.minPixels},
                         {"minRatio", z.minRatio}});
    }
    for (const ImageRef& r : a.images)
        images.push_back({{"name", r.name},        {"w", r.w},             {"h", r.h},
                          {"threshold", r.thresholdPct}, {"region", rectOf(r.region)}, {"clickOffset", vec(r.clickOffset)},
                          {"intervalMs", r.intervalMs},  {"file", r.file},       {"showOverlay", r.showOverlay}});
    for (const Macro& m : a.macros) {
        json steps = json::array();
        for (const auto& s : m.steps) steps.push_back(stepToJson(s));
        json mo{{"name", m.name}, {"key", m.key}, {"maxDurationMs", m.maxDurationMs}, {"runMode", static_cast<int>(m.runMode)}, {"steps", steps}};
        if (m.key2) mo["key2"] = m.key2;
        macros.push_back(mo);
    }
    for (const Monitor& m : a.monitors)
        monitors.push_back({{"name", m.name},       {"enabled", m.enabled},     {"zone", m.zone},
                            {"image", m.image},
                            {"intervalMs", m.intervalMs}, {"confirm", m.confirm}, {"cooldownMs", m.cooldownMs},
                            {"delayMs", m.delayMs}, {"mode", static_cast<int>(m.mode)}, {"maxRuns", m.maxRuns},
                            {"macro", m.macro},     {"key", m.key},           {"toggleKey", m.toggleKey},
                            {"elseMacro", m.elseMacro}, {"elseKey", m.elseKey}, {"absentMs", m.absentMs},
                            {"elseCooldownMs", m.elseCooldownMs}, {"elseStop", m.elseStop}});
    json root{{"toggleKey", a.toggleKey}, {"zones", zones}, {"images", images}, {"macros", macros}, {"monitors", monitors}};
    if (!(a.detect == DetectWatch{})) {
        root["detect"] = {{"startKey", a.detect.startKey},       {"stopKey", a.detect.stopKey},
                          {"intervalMs", a.detect.intervalMs},   {"confPct", a.detect.confPct},
                          {"region", rectOf(a.detect.region)},   {"classes", a.detect.classes},
                          {"drawBoxes", a.detect.drawBoxes},     {"drawLabels", a.detect.drawLabels},
                          {"showRegion", a.detect.showRegion},   {"autostart", a.detect.autostart},
                          {"exclOn", a.detect.exclOn},           {"excl", rectOf(a.detect.excl)},
                          {"showExcl", a.detect.showExcl},       {"mateOn", a.detect.mateOn},
                          {"mateSat", a.detect.mateSat},         {"mateMinPct", a.detect.mateMinPct},
                          {"redOn", a.detect.redOn},             {"redMinPct", a.detect.redMinPct},
                          {"redBoostPct", a.detect.redBoostPct},
                          {"trackLowPct", a.detect.trackLowPct},   {"trackPosBlendPct", a.detect.trackPosBlendPct},
                          {"trackVelPct", a.detect.trackVelPct},   {"trackPredictMs", a.detect.trackPredictMs},
                          {"trackMaxMissed", a.detect.trackMaxMissed},
                          {"trackMaxAgeMs", a.detect.trackMaxAgeMs},
                          {"aimOn", a.detect.aimOn},             {"aimKey", a.detect.aimKey},
                          {"aimSpeedPct", a.detect.aimSpeedPct}, {"aimRangePct", a.detect.aimRangePct},
                          {"aimLeadMs", a.detect.aimLeadMs},     {"aimBonePct", a.detect.aimBonePct},
                          {"aimTargetMode", a.detect.aimTargetMode},
                          {"inferFps", a.detect.inferFps},           {"fusionOn", a.detect.fusionOn},
                          {"selPreset", a.detect.selPreset},         {"selCenterW", a.detect.selCenterW},
                          {"selSizeW", a.detect.selSizeW},           {"selConfW", a.detect.selConfW},
                          {"switchHystPct", a.detect.switchHystPct}, {"lockGraceMs", a.detect.lockGraceMs},
                          {"pointPreset", a.detect.pointPreset},     {"pointX", a.detect.pointX},
                          {"pointY", a.detect.pointY},               {"ctrlDeadzonePx", a.detect.ctrlDeadzonePx},
                          {"ctrlInnerPct", a.detect.ctrlInnerPct},
                          {"ctrlMaxOut", a.detect.ctrlMaxOut},       {"ctrlMaxRateHz", a.detect.ctrlMaxRateHz},
                          {"ctrlMaxAgeMs", a.detect.ctrlMaxAgeMs},   {"ctrlMinConfPct", a.detect.ctrlMinConfPct},
                          {"dbgRaw", a.detect.dbgRaw},               {"dbgIds", a.detect.dbgIds},
                          {"dbgPerf", a.detect.dbgPerf},             {"dbgRecord", a.detect.dbgRecord}};
    }
    if (!a.studioGraphs.empty()) {
        json sg = json::array();
        for (const auto& g : a.studioGraphs) sg.push_back(json::parse(studio::graphToJson(g, -1)));
        root["studioGraphs"] = std::move(sg);
    }
    return root.dump();
}

// Converts legacy "Image search" (type 13) and "Click image" (type 14) steps into
// the unified ImageRecognize action. Behavior is preserved: the search keeps its
// timeout/actions, the click becomes ON FOUND = click inside the detected bbox at
// the image's configured offset, with the old hold duration kept as a long press
// (or a tap when it was <= 40 ms). Ambiguous cases get safe defaults and a note.
static void migrateImageSteps(std::vector<Step>& list, const Automation& a, const std::string& where,
                              std::vector<std::string>* notes) {
    for (size_t i = 0; i < list.size(); ++i) {
        Step& s = list[i];
        const std::string w = where + " step " + std::to_string(i + 1);
        migrateImageSteps(s.then, a, w, notes);
        migrateImageSteps(s.otherwise, a, w, notes);
        if (s.type == StepType::ImageSearch) {
            const int fa = static_cast<int>(s.foundAction);    // legacy enum index
            const int nfa = static_cast<int>(s.notFoundAction);
            s.type = StepType::ImageRecognize;
            s.foundAction = fa == 1   ? ImageFoundAction::ClickPoint
                            : fa == 2 ? ImageFoundAction::RunMacro
                            : fa == 3 ? ImageFoundAction::Stop
                                      : ImageFoundAction::Continue;
            s.notFoundAction = nfa == 1   ? ImageNotFoundAction::Continue
                               : nfa == 2 ? ImageNotFoundAction::Stop
                                          : ImageNotFoundAction::Fail;  // legacy "Retry" failed on timeout
            if (s.ms <= 0) s.ms = 10000;  // legacy default search window was 10 s
            if (notes) notes->push_back(w + ": 'Image search' migrated to 'Image recognition'");
        } else if (s.type == StepType::ClickImage) {
            s.type = StepType::ImageRecognize;
            const int holdMs = std::max(s.ms, 40);  // `ms` was the finger hold time
            s.ms = 0;                               // it checked the last scan immediately, never waited
            s.foundAction = ImageFoundAction::ClickMatch;
            s.notFoundAction = ImageNotFoundAction::Fail;
            s.clickKind = holdMs > 40 ? ClickKind::LongPress : ClickKind::Tap;
            s.pressMs = holdMs;
            s.clickOfs = s.image >= 0 && s.image < static_cast<int>(a.images.size())
                             ? a.images[s.image].clickOffset
                             : Vec2{0.5f, 0.5f};
            if (notes)
                notes->push_back(w + ": 'Click image' migrated to 'Image recognition' (click at offset, hold " +
                                 std::to_string(holdMs) + " ms)");
        }
    }
}

bool automationFromJson(const std::string& text, Automation& out, std::string& err,
                        std::vector<std::string>* migrated) {
    try {
        const json j = json::parse(text);
        Automation a;
        a.toggleKey = j.value("toggleKey", 0x77);
        for (const auto& o : j.value("zones", json::array())) {
            Zone z;
            z.name = o.value("name", std::string("Zone"));
            z.center = vec(o.value("center", json()), z.center);
            z.radius = o.value("radius", 0.04f);
            for (const auto& c : o.value("colors", json::array())) {
                ColorTarget t;
                t.name = c.value("name", std::string());
                const json rgb = c.value("rgb", json::array({255, 0, 0}));
                t.r = static_cast<uint8_t>(std::clamp(rgb.at(0).get<int>(), 0, 255));
                t.g = static_cast<uint8_t>(std::clamp(rgb.at(1).get<int>(), 0, 255));
                t.b = static_cast<uint8_t>(std::clamp(rgb.at(2).get<int>(), 0, 255));
                t.tolerance = c.value("tolerance", 12.0f);
                z.colors.push_back(t);
            }
            z.mode = o.value("mode", std::string("any")) == "all" ? ColorMatchMode::All : ColorMatchMode::Any;
            z.minPixels = o.value("minPixels", 6);
            z.minRatio = o.value("minRatio", 0.0f);
            a.zones.push_back(std::move(z));
        }
        for (const auto& o : j.value("images", json::array())) {
            ImageRef r;
            r.name = o.value("name", std::string("Image"));
            r.w = std::min<uint32_t>(o.value("w", 0u), 4096u);
            r.h = std::min<uint32_t>(o.value("h", 0u), 4096u);
            r.thresholdPct = std::clamp(o.value("threshold", 85), 1, 100);
            r.region = rectOf(o.value("region", json()), r.region);
            r.clickOffset = vec(o.value("clickOffset", json()), r.clickOffset);
            r.intervalMs = std::clamp(o.value("intervalMs", 150), 10, 5000);
            r.file = o.value("file", std::string());
            r.showOverlay = o.value("showOverlay", true);
            a.images.push_back(std::move(r));
        }
        for (const auto& o : j.value("macros", json::array())) {
            Macro m;
            m.name = o.value("name", std::string("Macro"));
            m.key = o.value("key", 0);
            m.key2 = o.value("key2", 0);
            m.maxDurationMs = o.value("maxDurationMs", 60000);
            m.runMode = static_cast<MacroRunMode>(std::clamp(o.value("runMode", 0), 0, 3));
            for (const auto& s : o.value("steps", json::array())) m.steps.push_back(stepFromJson(s, 1));
            a.macros.push_back(std::move(m));
        }
        for (size_t i = 0; i < a.macros.size(); ++i)
            migrateImageSteps(a.macros[i].steps, a, "macro '" + a.macros[i].name + "'", migrated);
        for (const auto& o : j.value("monitors", json::array())) {
            Monitor m;
            m.name = o.value("name", std::string("Monitor"));
            m.enabled = o.value("enabled", true);
            m.zone = o.value("zone", 0);
            m.image = o.value("image", -1);
            m.intervalMs = o.value("intervalMs", 50);
            m.confirm = o.value("confirm", 2);
            m.cooldownMs = o.value("cooldownMs", 300);
            m.delayMs = o.value("delayMs", 0);
            m.mode = static_cast<TriggerMode>(std::clamp(o.value("mode", 2), 0, 3));
            m.maxRuns = o.value("maxRuns", 5);
            m.macro = o.value("macro", -1);
            m.key = o.value("key", 0);
            m.toggleKey = o.value("toggleKey", 0);
            m.elseMacro = o.value("elseMacro", -1);
            m.elseKey = o.value("elseKey", 0);
            m.absentMs = std::clamp(o.value("absentMs", 2000), 0, 600000);
            m.elseCooldownMs = std::clamp(o.value("elseCooldownMs", 2000), 50, 600000);
            m.elseStop = o.value("elseStop", false);
            a.monitors.push_back(std::move(m));
        }
        if (const json d = j.value("detect", json()); d.is_object()) {
            a.detect.startKey = d.value("startKey", 0);
            a.detect.stopKey = d.value("stopKey", 0);
            a.detect.intervalMs = std::clamp(d.value("intervalMs", 80), 20, 5000);
            a.detect.confPct = std::clamp(d.value("confPct", 30), 1, 99);
            a.detect.region = rectOf(d.value("region", json()), a.detect.region);
            for (const auto& c : d.value("classes", json::array()))
                if (c.is_string()) a.detect.classes.push_back(c.get<std::string>());
            a.detect.drawBoxes = d.value("drawBoxes", true);
            a.detect.drawLabels = d.value("drawLabels", true);
            a.detect.showRegion = d.value("showRegion", false);
            a.detect.autostart = d.value("autostart", false);
            a.detect.exclOn = d.value("exclOn", true);
            a.detect.excl = rectOf(d.value("excl", json()), a.detect.excl);
            a.detect.showExcl = d.value("showExcl", false);
            a.detect.mateOn = d.value("mateOn", true);
            a.detect.mateSat = std::clamp(d.value("mateSat", 60), 10, 200);
            a.detect.mateMinPct = std::clamp(d.value("mateMinPct", 3), 1, 50);
            a.detect.redOn = d.value("redOn", true);
            a.detect.redMinPct = std::clamp(d.value("redMinPct", 2), 1, 30);
            a.detect.redBoostPct = std::clamp(d.value("redBoostPct", 20), 0, 50);
            a.detect.trackLowPct = std::clamp(d.value("trackLowPct", 55), 10, 95);
            a.detect.trackPosBlendPct = std::clamp(d.value("trackPosBlendPct", 55), 10, 100);
            a.detect.trackVelPct = std::clamp(d.value("trackVelPct", 45), 0, 100);
            a.detect.trackPredictMs = std::clamp(d.value("trackPredictMs", 350), 0, 2000);
            a.detect.trackMaxMissed = std::clamp(d.value("trackMaxMissed", 2), 0, 10);
            a.detect.trackMaxAgeMs = std::clamp(d.value("trackMaxAgeMs", 600), 50, 5000);
            a.detect.aimOn = d.value("aimOn", false);
            a.detect.aimKey = d.value("aimKey", 0);
            a.detect.aimSpeedPct = std::clamp(d.value("aimSpeedPct", 50), 10, 100);
            a.detect.aimRangePct = std::clamp(d.value("aimRangePct", 40), 5, 100);
            a.detect.aimLeadMs = std::clamp(d.value("aimLeadMs", 80), 0, 500);
            a.detect.aimBonePct = std::clamp(d.value("aimBonePct", 30), 0, 100);
            a.detect.aimTargetMode = std::clamp(d.value("aimTargetMode", 0), 0, 1);
            a.detect.inferFps = std::clamp(d.value("inferFps", 0), 0, 240);
            a.detect.fusionOn = d.value("fusionOn", true);
            // Legacy migration: the old "Target mode = largest" folds into the
            // size-priority selection preset; the new key wins when present.
            a.detect.selPreset = std::clamp(
                d.value("selPreset", a.detect.aimTargetMode == 1 ? 1 : 0), 0, 4);
            a.detect.selCenterW = std::clamp(d.value("selCenterW", 100), 0, 200);
            a.detect.selSizeW = std::clamp(d.value("selSizeW", 0), 0, 200);
            a.detect.selConfW = std::clamp(d.value("selConfW", 25), 0, 200);
            a.detect.switchHystPct = std::clamp(d.value("switchHystPct", 15), 0, 100);
            a.detect.lockGraceMs = std::clamp(d.value("lockGraceMs", 150), 0, 2000);
            a.detect.pointPreset = std::clamp(d.value("pointPreset", 2), 0, 4);
            a.detect.pointX = std::clamp(d.value("pointX", 50), 0, 100);
            a.detect.pointY = std::clamp(d.value("pointY", 32), 0, 100);
            // Legacy migration: old "Aim height" (bonePct) becomes a custom
            // point when the profile predates the point config.
            if (!d.contains("pointPreset") && d.contains("aimBonePct")) {
                a.detect.pointPreset = 4;  // Custom
                a.detect.pointX = 50;
                a.detect.pointY = std::clamp(a.detect.aimBonePct, 0, 100);
            }
            a.detect.ctrlDeadzonePx = std::clamp(d.value("ctrlDeadzonePx", 1), 0, 50);
            a.detect.ctrlInnerPct = std::clamp(d.value("ctrlInnerPct", 22), 0, 45);
            // ctrlMaxOut migrated: per-update px cap -> emitted counts/s cap.
            // Legacy values (<500) scale by ~25x (the old cap bound at ~120px
            // per ~40ms scan ≈ 3000 counts/s of actual throughput).
            {
                int mo = d.value("ctrlMaxOut", 3000);
                if (mo < 500) mo *= 25;
                a.detect.ctrlMaxOut = std::clamp(mo, 100, 20000);
            }
            a.detect.ctrlMaxRateHz = std::clamp(d.value("ctrlMaxRateHz", 200), 10, 1000);
            a.detect.ctrlMaxAgeMs = std::clamp(d.value("ctrlMaxAgeMs", 400), 0, 2000);
            a.detect.ctrlMinConfPct = std::clamp(d.value("ctrlMinConfPct", 15), 0, 100);
            a.detect.dbgRaw = d.value("dbgRaw", false);
            a.detect.dbgIds = d.value("dbgIds", false);
            a.detect.dbgPerf = d.value("dbgPerf", false);
            a.detect.dbgRecord = d.value("dbgRecord", false);
        }
        for (const auto& go : j.value("studioGraphs", json::array())) {
            studio::AutomationGraph g;
            std::string gerr;
            if (!studio::graphFromJson(go.dump(), g, gerr, migrated)) {
                err = gerr;
                return false;
            }
            a.studioGraphs.push_back(std::move(g));
        }
        if (auto problems = validateAutomation(a); !problems.empty()) {
            err = problems.front();
            return false;
        }
        out = std::move(a);
        return true;
    } catch (const std::exception& e) {
        err = std::string("invalid automation data: ") + e.what();
        return false;
    }
}

}  // namespace wasd
