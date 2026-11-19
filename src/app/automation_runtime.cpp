// Runs color-zone scans, reference-image scans, monitors and macros against the live
// session. Uses only existing interfaces: Renderer region readback, Session::sendTouch
// and InputEngine::onKey.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>

#include "app/app.h"
#include "app/i18n.h"
#include "core/imgmatch.h"
#include "core/log.h"
#include "core/studio_engine.h"
#include "platform/paths.h"
#include "platform/process.h"

#include <cstring>

namespace wasd::app {
using platform::narrow;

void App::macroTouch(int action, int finger, Vec2 n) {
    // Route through the central input engine: macro fingers get their own pointer
    // ids with per-source ownership, so a macro can never release a finger that the
    // user or another automation source is still holding.
    const auto a = action == 0 ? proto::MotionAction::Down : action == 1 ? proto::MotionAction::Up : proto::MotionAction::Move;
    inputT0Us_ = nowMicros();
    { std::lock_guard lk(inputMu_); input_.onMacroTouch(a, finger, n, InputSource::Macro); }
    inputT0Us_ = 0;
}

// ---- image-match worker ------------------------------------------------------
// A full-screen NCC scan can take hundreds of ms; running it inside
// automationTick stalled the UI thread and caused visible input hitches during
// image-recognition macros. The GPU readback is already async — once pixels
// land, the match itself is handed to this worker. The UI thread picks the
// result up on a later tick, so a scan costs the UI thread nothing.
void App::startScanWorker() {
    if (scanWorkerThread_.joinable()) return;
    scanWorkerRunning_ = true;
    scanWorkerThread_ = std::thread([this] { scanWorkerLoop(); });
}

void App::stopScanWorker() {
    scanWorkerRunning_ = false;
    scanCv_.notify_all();
    if (scanWorkerThread_.joinable()) scanWorkerThread_.join();
}

void App::scanWorkerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (scanWorkerRunning_) {
        ScanJob job;
        {
            std::unique_lock lk(scanMu_);
            scanCv_.wait(lk, [this] { return !scanQueue_.empty() || !scanWorkerRunning_; });
            if (!scanWorkerRunning_) return;
            job = std::move(scanQueue_.front());
            scanQueue_.pop();
        }
        const int64_t t0 = nowMicros();
        if (job.isDetect) {
            // Lazily load the ONNX model on this worker thread (first job pays
            // the ~200ms load; the UI thread never stalls).
            if (!detector_.loaded()) {
                // Any .onnx dropped into runtime/models works — "model.onnx"
                // wins if present, else the first file alphabetically.
                std::string mp;
                const std::string dir = narrow(platform::runtimeDir()) + "/models";
                std::error_code ec;
                for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                    if (!e.is_regular_file() || e.path().extension() != ".onnx") continue;
                    mp = e.path().string();
                    if (e.path().filename() == "model.onnx") break;
                }
                if (mp.empty()) {
                    std::lock_guard lk(detectStatusMu_);
                    detectStatusText_ = "no .onnx in runtime/models";
                } else {
                    std::string err;
                    {
                        std::lock_guard lk(detectStatusMu_);
                        detectStatusText_ = "loading model...";
                    }
                    if (detector_.load(mp, err)) {
                        std::lock_guard lk(detectStatusMu_);
                        detectClassNames_ = detector_.classNames();
                        detectStatusText_ = "ready: " + std::filesystem::path(mp).filename().string() +
                                            (detector_.usingGpu() ? " (GPU)" : " (CPU)");
                    } else {
                        std::lock_guard lk(detectStatusMu_);
                        detectStatusText_ = "load failed: " + err;
                    }
                }
            }
            DetectWorkerResult& out = job.detectTarget == 2   ? detectWatchResult_
                                      : job.detectTarget == 1 ? detectPreviewResult_
                                                              : detectResult_;
            out.hits.clear();
            if (detector_.loaded() && !job.px.rgb.empty()) {
                DetectResult r = detector_.detect(job.px.rgb.data(), (int)job.px.w, (int)job.px.h,
                                                  (int)job.px.w * 3, job.detectFloor,
                                                  job.detectClasses, 0.45f);
                const uint32_t pw = job.px.w, ph = job.px.h;
                const auto& px = job.px.rgb;
                auto isRed = [&](size_t o) {
                    return px[o] > 120 && px[o] > int(px[o + 1] * 1.35f) && px[o] > int(px[o + 2] * 1.35f);
                };
                // "self" boxes suppress overlapping "enemy" boxes: the model
                // sometimes emits both for the player's own avatar. Collected
                // before the filters so a weak self box still shields.
                std::vector<Rect> selfBoxes;
                for (const Detection& d : r.hits)
                    if (job.selfIdx >= 0 && d.classId == job.selfIdx) selfBoxes.push_back(d.box);
                // Map the frame-normalized exclusion rect into region space.
                float ex0 = 0, ey0 = 0, ex1 = 0, ey1 = 0;
                if (job.detectExclOn && job.detectRegion.w > 1e-6f && job.detectRegion.h > 1e-6f) {
                    ex0 = (job.detectExcl.x - job.detectRegion.x) / job.detectRegion.w;
                    ey0 = (job.detectExcl.y - job.detectRegion.y) / job.detectRegion.h;
                    ex1 = (job.detectExcl.x + job.detectExcl.w - job.detectRegion.x) / job.detectRegion.w;
                    ey1 = (job.detectExcl.y + job.detectExcl.h - job.detectRegion.y) / job.detectRegion.h;
                }
                for (Detection d : r.hits) {
                    if (job.selfAutoAdded && d.classId == job.selfIdx) continue;  // suppression-only class
                    // Exclusion rect (own character): drop hits that are mostly
                    // INSIDE the zone — enemies passing through its edge keep
                    // tracking instead of dying at the worst moment.
                    if (job.detectExclOn) {
                        const float ix = std::max(0.0f, std::min(d.box.x + d.box.w, ex1) - std::max(d.box.x, ex0));
                        const float iy = std::max(0.0f, std::min(d.box.y + d.box.h, ey1) - std::max(d.box.y, ey0));
                        const float area = d.box.w * d.box.h;
                        if (area > 1e-6f && (ix * iy) / area > 0.6f) continue;
                    }
                    // Self overlap: drop enemy boxes sitting on a self box.
                    if (d.classId == 0 && !selfBoxes.empty()) {
                        bool suppressed = false;
                        for (const Rect& s : selfBoxes) {
                            const float ix = std::max(0.0f, std::min(d.box.x + d.box.w, s.x + s.w) - std::max(d.box.x, s.x));
                            const float iy = std::max(0.0f, std::min(d.box.y + d.box.h, s.y + s.h) - std::max(d.box.y, s.y));
                            const float inter = ix * iy;
                            const float uni = d.box.w * d.box.h + s.w * s.h - inter;
                            const bool centerInSelf =
                                d.box.x + d.box.w * 0.5f >= s.x && d.box.x + d.box.w * 0.5f <= s.x + s.w &&
                                d.box.y + d.box.h * 0.5f >= s.y && d.box.y + d.box.h * 0.5f <= s.y + s.h;
                            if (centerInSelf || (uni > 1e-6f && inter / uni > 0.25f)) { suppressed = true; break; }
                        }
                        if (suppressed) continue;
                    }
                    // Teammate marker: a colored icon floats above the head. Markers
                    // are strongly saturated (green/blue/yellow) and NEVER red —
                    // red means enemy highlight, so it must not count as a marker.
                    if (job.detectMateOn) {
                        const int bx0 = std::clamp(int(d.box.x * pw), 0, (int)pw - 1);
                        const int bx1 = std::clamp(int((d.box.x + d.box.w) * pw), bx0, (int)pw);
                        const int by1 = std::clamp(int(d.box.y * ph), 0, (int)ph);
                        const int by0 = std::clamp(by1 - int(d.box.h * ph * 1.1f), 0, by1);
                        int tot = 0, hit = 0;
                        for (int y = by0; y < by1; ++y)
                            for (int x = bx0; x < bx1; ++x) {
                                const size_t o = (size_t(y) * pw + x) * 3;
                                ++tot;
                                const int mx = std::max({px[o], px[o + 1], px[o + 2]});
                                const int mn = std::min({px[o], px[o + 1], px[o + 2]});
                                if (mx - mn > job.mateSat && !isRed(o)) ++hit;
                            }
                        if (tot > 0 && float(hit) / tot >= job.mateMinRatio) continue;
                    }
                    // Red-outline rescue: enemies often glow red. A weak hit with a
                    // red edge ring gets a confidence boost instead of being dropped.
                    if (job.detectRedOn && d.confidence < job.detectConf) {
                        const float m = 0.06f;  // ring thickness, fraction of box
                        const int ox0 = std::clamp(int((d.box.x - d.box.w * m) * pw), 0, (int)pw);
                        const int oy0 = std::clamp(int((d.box.y - d.box.h * m) * ph), 0, (int)ph);
                        const int ox1 = std::clamp(int((d.box.x + d.box.w * (1 + m)) * pw), ox0, (int)pw);
                        const int oy1 = std::clamp(int((d.box.y + d.box.h * (1 + m)) * ph), oy0, (int)ph);
                        const int ix0 = std::clamp(int((d.box.x + d.box.w * m) * pw), ox0, (int)pw);
                        const int iy0 = std::clamp(int((d.box.y + d.box.h * m) * ph), oy0, (int)ph);
                        const int ix1 = std::clamp(int((d.box.x + d.box.w * (1 - m)) * pw), ix0, (int)pw);
                        const int iy1 = std::clamp(int((d.box.y + d.box.h * (1 - m)) * ph), iy0, (int)ph);
                        int tot = 0, hit = 0;
                        for (int y = oy0; y < oy1; ++y)
                            for (int x = ox0; x < ox1; ++x) {
                                if (x >= ix0 && x < ix1 && y >= iy0 && y < iy1) continue;  // ring only
                                ++tot;
                                if (isRed(size_t(y) * pw * 3 + x * 3)) ++hit;
                            }
                        if (tot > 0 && float(hit) / tot >= job.redMinRatio) {
                            d.confidence = std::min(0.99f, d.confidence + job.redBoost);
                            d.boosted = true;
                        }
                    }
                    if (d.confidence < job.detectConf) continue;
                    out.hits.push_back(d);
                }
            }
            out.inferUs = nowMicros() - t0;
            out.inferStartUs = t0;
            out.doneAtUs = nowMicros();
            out.requestId = job.requestId;
            out.frameId = job.frameId;
            out.captureUs = job.captureUs;
            out.preview = job.detectTarget != 0;
            out.ready.store(true, std::memory_order_release);
            continue;
        }
        const FrameView view{job.px.rgb.data(), static_cast<int>(job.px.w), static_cast<int>(job.px.h),
                             static_cast<int>(job.px.w) * 3};
        ImageRef qr = job.ref;
        qr.thresholdPct = job.thresholdPct;
        ScanWorkerResult& out = scanResults_[job.imageIndex];
        if (job.wantMatches) {
            const auto found = matchImageInRectAll(view, qr, 0, 0, static_cast<int>(job.px.w), static_cast<int>(job.px.h), 4);
            out.matches.clear();
            out.found = false;
            out.score = 0;
            for (const ImageMatch& m : found) {
                ImageMatch fm = toFrameCoords(m, static_cast<int>(job.px.x), static_cast<int>(job.px.y),
                                            static_cast<int>(job.px.w), static_cast<int>(job.px.h),
                                            static_cast<int>(job.vw), static_cast<int>(job.vh));
                out.score = std::max(out.score, m.score);
                if (!fm.found) continue;
                out.matches.push_back(fm);
                if (!out.found) out.match = fm;
                out.found = true;
            }
        } else {
            const ImageMatch m = matchImageInRect(view, qr, 0, 0, static_cast<int>(job.px.w), static_cast<int>(job.px.h));
            out.matches.clear();
            out.found = m.found;
            out.score = m.score;
            if (m.found) {
                out.match = toFrameCoords(m, static_cast<int>(job.px.x), static_cast<int>(job.px.y),
                                        static_cast<int>(job.px.w), static_cast<int>(job.px.h),
                                        static_cast<int>(job.vw), static_cast<int>(job.vh));
                out.matches.push_back(out.match);
            } else {
                out.match = ImageMatch{};
            }
        }
        out.scanCostUs = nowMicros() - t0;
        out.requestId = job.requestId;
        out.ready.store(true, std::memory_order_release);
    }
}

std::optional<bool> App::zonePresent(int zone) const {
    if (zone < 0 || zone >= static_cast<int>(zoneScan_.size())) return std::nullopt;
    const ZoneScan& z = zoneScan_[zone];
    if (!z.present || nowMicros() - z.lastUs > 500000) return std::nullopt;
    return z.present;
}

std::optional<bool> App::imagePresent(int image) const {
    if (image < 0 || image >= static_cast<int>(imgScan_.size())) return std::nullopt;
    const ImageScan& s = imgScan_[image];
    // Only answer when the last scan ran with the image's own parameters: a macro
    // step's temporary region/threshold override must not leak into conditions.
    if (!s.tested || !s.defaultParams || nowMicros() - s.lastUs > 500000) return std::nullopt;
    return s.found;
}

ImageScanResult App::imageScanQuery(int image, const ImageQuery& q) {
    ImageScanResult r;
    if (image < 0 || image >= static_cast<int>(imgScan_.size()) ||
        image >= static_cast<int>(profile_.automation.images.size()))
        return r;
    const int64_t now = nowMicros();
    imgOverride_.image = image;
    imgOverride_.region = q.region;
    imgOverride_.thresholdPct = q.thresholdPct;
    imgOverride_.intervalMs = q.intervalMs;
    imgOverride_.wantMatches = q.wantMatches;
    imgOverride_.lastQueryUs = now;
    const ImageScan& s = imgScan_[image];
    r.available = s.tested && now - s.lastUs <= 500000;
    r.paramsMatch = s.doneRegion == q.region && s.doneThreshold == q.thresholdPct;
    // If the caller wants the candidate list but the last scan used the fast
    // single-match path, the params don't match the request.
    if (q.wantMatches && !s.matches.size() && s.found) r.paramsMatch = false;
    r.found = s.found;
    r.score = s.score;
    r.match = s.match;
    r.matches = s.matches;
    r.scanUs = s.scanCostUs;
    return r;
}

void App::loadRefImageFiles() {
    for (ImageRef& r : profile_.automation.images) {
        r.rgb.clear();
        std::ifstream f(std::filesystem::u8path(refImagePath(r)), std::ios::binary);
        if (!f) continue;
        char magic[4];
        uint32_t w = 0, h = 0;
        f.read(magic, 4);
        f.read(reinterpret_cast<char*>(&w), 4);
        f.read(reinterpret_cast<char*>(&h), 4);
        if (!f || std::memcmp(magic, "WIMG", 4) != 0 || w != r.w || h != r.h || w * h > 4096u * 4096u) {
            log::error("image load failed: " + r.file);
            continue;
        }
        r.rgb.resize(size_t(w) * h * 3);
        f.read(reinterpret_cast<char*>(r.rgb.data()), std::streamsize(r.rgb.size()));
        if (!f) r.rgb.clear();
    }
}

std::string App::refImagePath(const ImageRef& r) const {
    return imagesDirFor(profile_.name) + "/" + (r.file.empty() ? "unnamed.wasdimg" : r.file);
}

bool App::saveRefImageFile(const ImageRef& r, std::string& err) {
    auto dir = std::filesystem::u8path(narrow(platform::appDataDir())) / "images";
    std::error_code ec;
    std::filesystem::create_directories(dir / std::filesystem::u8path(profile_.name), ec);    if (ec) {
        err = ec.message();
        return false;
    }
    std::ofstream f(std::filesystem::u8path(refImagePath(r)), std::ios::binary);
    if (!f) {
        err = "cannot open the image file for writing";
        return false;
    }
    f.write("WIMG", 4);
    f.write(reinterpret_cast<const char*>(&r.w), 4);
    f.write(reinterpret_cast<const char*>(&r.h), 4);
    f.write(reinterpret_cast<const char*>(r.rgb.data()), std::streamsize(r.rgb.size()));
    err.clear();
    return bool(f);
}

void App::deleteRefImageFile(const ImageRef& r) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(refImagePath(r)), ec);
}

std::string App::imagesDirFor(const std::string& profileName) const {
    return narrow(platform::appDataDir()) + "/images/" + profileName;
}

void App::copyImagesDir(const std::string& fromProfile, const std::string& toProfile) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path src = fs::u8path(imagesDirFor(fromProfile));
    if (!fs::exists(src, ec)) return;
    const fs::path dst = fs::u8path(imagesDirFor(toProfile));
    fs::create_directories(dst, ec);
    for (auto& entry : fs::directory_iterator(src, ec)) {
        if (!entry.is_regular_file()) continue;
        const auto target = dst / entry.path().filename();
        fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
    }
}

void App::moveImagesDir(const std::string& fromProfile, const std::string& toProfile) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path src = fs::u8path(imagesDirFor(fromProfile));
    if (!fs::exists(src, ec)) return;
    const fs::path dst = fs::u8path(imagesDirFor(toProfile));
    fs::create_directories(dst, ec);
    for (auto& entry : fs::directory_iterator(src, ec)) {
        if (!entry.is_regular_file()) continue;
        const auto target = dst / entry.path().filename();
        fs::rename(entry.path(), target, ec);
        if (ec) fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
    }
    fs::remove(src, ec);  // remove now-empty source directory
}

void App::setAutomation(bool on, const std::string& reason) {
    if (on && !sessionActive()) {
        automationStatus_ = i18n::tr("Connect a device first");
        on = false;
    }
    automationOn_ = on;
    for (auto& r : monitorRt_) r.reset();
    if (!on) {
        if (macros_.active()) macros_.cancel("stopped");
        automationStatus_ = reason;
    } else {
        automationStatus_.clear();
    }
}

// Trigger keys for macros and the automation toggle; returns true when consumed.
bool App::automationKey(int vk, bool down) {
    const Automation& a = profile_.automation;
    if (a == Automation{}) return false;
    // Keys that drive an active binding (WASD directions, fire, ...) belong to
    // the input engine. Swallowing their press or release for a trigger would
    // drop a direction component (walk sideways instead of diagonally) or leave
    // a key stuck held, so the binding always wins and the trigger is ignored.
    for (const Binding& b : profile_.bindings) {
        if (b.disabled) continue;
        if (vk == b.key || (b.key2 != 0 && vk == b.key2) || vk == b.recoilToggleKey || vk == b.rapidFireToggleKey ||
            (b.type == BindingType::Joystick &&
             (vk == b.keyUp || vk == b.keyDown || vk == b.keyLeft || vk == b.keyRight ||
              (b.joyShiftEnabled && vk == b.joyShiftKey))))
            return false;
    }
    if (vk == a.toggleKey) {
        if (down) {
            if (macros_.active()) macros_.cancel("emergency stop");
            setAutomation(!automationOn_, i18n::tr("Stopped by the toggle key"));
        }
        return true;
    }
    // Standalone object-detection watch: startKey arms it (toggles when no
    // separate stopKey is set), stopKey always disarms.
    const DetectWatch& dw = a.detect;
    if (dw.startKey > 0 && vk == dw.startKey) {
        if (!down) return true;
        setDetectWatch(dw.stopKey <= 0 ? !detectWatchOn_ : true);
        return true;
    }
    if (dw.stopKey > 0 && vk == dw.stopKey) {
        if (!down) return true;
        setDetectWatch(false);
        return true;
    }
    // Aim assist: hold-to-assist. Both edges are consumed so the key can never
    // leak into the game as a stray input; releasing drops the target lock so
    // the next hold reacquires the best target.
    if (dw.aimOn && dw.aimKey > 0 && vk == dw.aimKey) {
        aimHeld_ = down;
        if (!down) {
            aimTrackId_ = 0; aimInside_ = false; aimAccX_ = aimAccY_ = 0.0f;
            aimLockAtUs_ = 0; aimSwallowMouse_ = false;
            targetLock_.reset(nowMicros()); aimCtrl_.reset();
        }
        return true;
    }
    // Per-rule hotkeys: each monitor can be armed/disarmed on its own, whether or
    // not the global automation toggle is on. A re-arm starts from a clean
    // runtime so old hit counters cannot fire the action instantly.
    for (int i = 0; i < static_cast<int>(a.monitors.size()) && i < static_cast<int>(monitorRt_.size()); ++i) {
        const Monitor& m = a.monitors[i];
        if (m.toggleKey <= 0 || m.toggleKey != vk) continue;
        if (!down) return true;  // swallow the release so the key never leaks into the game
        MonitorRuntime& rt = monitorRt_[i];
        rt.switchedOn = !rt.switchedOn;
        if (rt.switchedOn) {
            rt.reset();
            rt.switchedOn = true;
        }
        showToast(i18n::trf("%s: %s", m.name.c_str(), i18n::tr(rt.switchedOn ? "ON" : "OFF")));
        return true;
    }
    for (int i = 0; i < static_cast<int>(a.macros.size()); ++i) {
        const Macro& m = a.macros[i];
        const bool isTrigger = (m.key == vk) || (m.key2 != 0 && m.key2 == vk);
        if (!isTrigger) continue;
        const int other = (m.key == vk) ? m.key2 : m.key;
        if (m.key2 != 0) {
            // Dual-key combo: the current key counts as pressed on `down` events, so
            // only the OTHER member's held state gates the trigger. A lone press of
            // either member passes through untouched (e.g. G still fires its binding).
            { std::lock_guard lk(inputMu_);
                if (input_.keyState(other) != KeyLogicalState::Pressed) continue; }
            if (!down) return true;  // swallow the release while the combo was held
            if (macros_.active() && macros_.macro() == i) macros_.cancel("stopped by its key");
            else if (!macros_.active()) macros_.start(a, i, nowMicros());
            return true;
        }
        if (!down) return true;
        if (macros_.active() && macros_.macro() == i) macros_.cancel("stopped by its key");
        else if (!macros_.active()) macros_.start(a, i, nowMicros());
        return true;
    }
    // Studio graph trigger keys: each Start node carries its own hotkey so a
    // single graph can host several independent flows. The graph-level
    // metadata key still applies to the FIRST Start (older profiles). Pressing
    // the key again while that graph runs cancels it.
    for (const studio::AutomationGraph& g : a.studioGraphs) {
        bool firstStart = true;
        for (const studio::GraphNode& n : g.nodes) {
            if (n.type != studio::NodeType::Start) continue;
            const int k1 = n.triggerKey ? n.triggerKey : (firstStart ? g.metadata.triggerKey : 0);
            const int k2 = n.triggerKey2 ? n.triggerKey2 : (firstStart ? g.metadata.triggerKey2 : 0);
            firstStart = false;
            if (k1 <= 0) continue;
            const bool isTrigger = (k1 == vk) || (k2 != 0 && k2 == vk);
            if (!isTrigger) continue;
            if (k2 != 0) {
                const int other = (k1 == vk) ? k2 : k1;
                { std::lock_guard lk(inputMu_);
                    if (input_.keyState(other) != KeyLogicalState::Pressed) continue; }
                if (!down) return true;  // swallow the release while the combo was held
            } else if (!down) {
                return true;
            }
            if (graphRunner_ && graphRunner_->active() && studioRunningGraphId_ == g.id)
                graphRunner_->cancel("stopped by its key");
            else if (!graphRunner_ || !graphRunner_->active())
                studioRunGraph(g.id, false, n.id);
            return true;
        }
    }
    return false;
}

void App::automationTick(int64_t now) {
    const Automation& a = profile_.automation;
    if (zoneScan_.size() != a.zones.size()) zoneScan_.assign(a.zones.size(), ZoneScan{});
    if (imgScan_.size() != a.images.size()) imgScan_.assign(a.images.size(), ImageScan{});
    if (monitorRt_.size() != a.monitors.size()) monitorRt_.assign(a.monitors.size(), MonitorRuntime{});
    for (auto it = pendingKeyUps_.begin(); it != pendingKeyUps_.end();) {
        if (now >= it->second) { std::lock_guard lk(inputMu_); input_.onKey(it->first, false, now, InputSource::Monitor); it = pendingKeyUps_.erase(it); }
        else ++it;
    }
    const bool video = sessionActive() && renderer_.hasVideo();
    if (!video) {
        if (macros_.active()) macros_.cancel("phone disconnected");
        if (graphRunner_ && graphRunner_->active()) graphRunner_->cancel("phone disconnected");
        if (automationOn_) setAutomation(false, i18n::tr("Stopped: no video from the phone"));
        detectAutoArmed_ = false;
        for (auto& z : zoneScan_) z.present.reset(), z.requested = false;
        for (auto& s : imgScan_) s.requested = false, s.tested = false;
        return;
    }
    // Rules armed by their own toggleKey keep scanning even when the global
    // automation switch is off — the per-monitor gate below decides what fires.
    bool anyZoneRule = false, anyImageRule = false;
    for (int i = 0; i < static_cast<int>(a.monitors.size()) && i < static_cast<int>(monitorRt_.size()); ++i) {
        if (!monitorRt_[i].switchedOn || !a.monitors[i].enabled) continue;
        if (a.monitors[i].image >= 0) anyImageRule = true;
        else anyZoneRule = true;
    }
    const bool graphActive = graphRunner_ && graphRunner_->active();
    const bool scanAll = automationOn_ || macros_.active() || graphActive || zoneTest_ || anyZoneRule;
    const bool scanImages = automationOn_ || macros_.active() || graphActive || imgTest_ || anyImageRule;
    const uint32_t vw = renderer_.videoWidth(), vh = renderer_.videoHeight();
    for (int zi = 0; zi < static_cast<int>(a.zones.size()) && zi < kZoneSlots; ++zi) {
        const Zone& zone = a.zones[zi];
        ZoneScan& sc = zoneScan_[zi];
        int interval = 100;
        for (const Monitor& m : a.monitors)
            if (m.zone == zi && m.enabled) interval = std::min(interval, std::clamp(m.intervalMs, 10, 5000));
        bool fresh = false;
        if (sc.requested) {
            render::Renderer::RegionPixels px;
            if (renderer_.pollRegion(zi, px)) {
                const int64_t t0 = nowMicros();
                const RgbView view{px.rgb.data(), static_cast<int>(px.w), static_cast<int>(px.h), static_cast<int>(px.w) * 3};
                sc.match = matchZone(view, zone.center.x * vw - px.x, zone.center.y * vh - px.y, zone.radius * vh, zone);
                sc.present = sc.match.present;
                sc.lastUs = now;
                sc.requested = false;
                scanUs_.add(static_cast<double>(nowMicros() - t0));
                scanRate_.tick(now);
                fresh = true;
            } else if (now - sc.requestedAt > 250000) {
                sc.requested = false;  // dropped; never queue stale scans
            }
        }
        if (scanAll && !sc.requested && now >= sc.nextAt && zi < kZoneSlots) {
            const float cx = zone.center.x * vw, cy = zone.center.y * vh, r = zone.radius * vh;
            const float x0 = std::max(0.0f, cx - r - 1), y0 = std::max(0.0f, cy - r - 1);
            if (renderer_.requestRegion(zi, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), static_cast<uint32_t>(2 * r + 3),
                                        static_cast<uint32_t>(2 * r + 3)))
                sc.requested = true, sc.requestedAt = now;
            sc.nextAt = now + int64_t(interval) * 1000;
        }
        if (!fresh) continue;
        for (int mi = 0; mi < static_cast<int>(a.monitors.size()); ++mi) {
            const Monitor& m = a.monitors[mi];
            if (m.image >= 0 || m.zone != zi || (m.macro < 0 && m.key <= 0)) continue;
            if (mi >= static_cast<int>(monitorRt_.size()) || !(automationOn_ || monitorRt_[mi].switchedOn)) continue;
            if (!monitorRt_[mi].onSample(*sc.present, now, m, macros_.active())) {
                if (!*sc.present && monitorRt_[mi].onAbsent(now, m)) {
                    if (m.elseStop) monitorRt_[mi].switchedOn = false;
                    else if (m.elseMacro >= 0) macros_.start(a, m.elseMacro, now);
                    else if (m.elseKey > 0) { std::lock_guard lk(inputMu_); input_.onKey(m.elseKey, true, now, InputSource::Monitor); pendingKeyUps_.push_back({m.elseKey, now + 40000}); }
                }
                continue;
            }
            ++detections_;
            if (m.macro >= 0) macros_.start(a, m.macro, now);
            else { std::lock_guard lk(inputMu_); input_.onKey(m.key, true, now, InputSource::Monitor); pendingKeyUps_.push_back({m.key, now + 40000}); }
        }
    }

    // ---- reference-image scans ------------------------------------------------
    // A running ImageRecognize step (or the UI test) may override the search
    // region/threshold/cadence of one image at a time.
    const bool ovActive = imgOverride_.image >= 0 && imgOverride_.image < static_cast<int>(a.images.size()) &&
                          now - imgOverride_.lastQueryUs <= 300000;
    for (int ii = 0; ii < static_cast<int>(a.images.size()) && ii < kMaxImages; ++ii) {
        const ImageRef& r = a.images[ii];
        ImageScan& sc = imgScan_[ii];
        const int slot = kImageSlotBase + ii;
        const bool ov = ovActive && imgOverride_.image == ii;
        const Rect effRegion = ov ? imgOverride_.region : r.region;
        const int effThreshold = ov ? imgOverride_.thresholdPct : r.thresholdPct;
        int interval = std::clamp(ov && imgOverride_.intervalMs > 0 ? imgOverride_.intervalMs : r.intervalMs, 30, 5000);
        for (const Monitor& m : a.monitors)
            if (m.image == ii && m.enabled) interval = std::min(interval, std::clamp(m.intervalMs, 10, 5000));
        if (!r.rgb.empty()) {
            // Pick up a finished worker result before issuing a new request.
            if (sc.pendingReqId && scanResults_[ii].ready.load(std::memory_order_acquire) &&
                scanResults_[ii].requestId == sc.pendingReqId) {
                const ScanWorkerResult& res = scanResults_[ii];
                sc.matches = res.matches;
                sc.match = res.match;
                sc.found = res.found;
                sc.score = res.score;
                sc.tested = true;
                sc.lastUs = now;
                sc.scanCostUs = res.scanCostUs;
                sc.doneRegion = sc.reqRegion;
                sc.doneThreshold = sc.reqThreshold;
                sc.defaultParams = sc.doneRegion == r.region && sc.doneThreshold == r.thresholdPct;
                scanUs_.add(static_cast<double>(sc.scanCostUs));
                scanRate_.tick(now);
                sc.pendingReqId = 0;
            }
            if (sc.requested) {
                render::Renderer::RegionPixels px;
                if (renderer_.pollRegion(slot, px)) {
                    // Pixels are in — hand the expensive NCC match to the worker
                    // thread instead of running it on the UI thread.
                    ScanJob job;
                    job.imageIndex = ii;
                    job.px = std::move(px);
                    job.ref = r;
                    job.thresholdPct = sc.reqThreshold;
                    job.wantMatches = sc.reqWantMatches;
                    job.vw = vw;
                    job.vh = vh;
                    job.requestId = ++scanReqId_;
                    scanResults_[ii].ready.store(false, std::memory_order_release);
                    {
                        std::lock_guard lk(scanMu_);
                        scanQueue_.push(std::move(job));
                    }
                    scanCv_.notify_one();
                    sc.pendingReqId = job.requestId;
                    sc.requested = false;
                } else if (now - sc.requestedAt > 250000) {
                    sc.requested = false;
                }
            }
            // Rescan promptly whenever the effective parameters changed.
            if (sc.reqRegion != effRegion || sc.reqThreshold != effThreshold) sc.nextAt = std::min(sc.nextAt, now);
            // Don't issue a new GPU request while a worker job is still in flight.
            if (scanImages && !sc.requested && !sc.pendingReqId && now >= sc.nextAt) {
                const uint32_t x0 = std::clamp(uint32_t(std::lround(effRegion.x * vw)), 0u, vw > 2 ? vw - 2 : 0u);
                const uint32_t y0 = std::clamp(uint32_t(std::lround(effRegion.y * vh)), 0u, vh > 2 ? vh - 2 : 0u);
                const uint32_t w = std::max(2u, uint32_t(std::lround(effRegion.w * vw)) & ~1u);
                const uint32_t h = std::max(2u, uint32_t(std::lround(effRegion.h * vh)) & ~1u);
                sc.reqRegion = effRegion;
                sc.reqThreshold = effThreshold;
                // Determine whether the caller needs the candidate list. Monitors
                // and presence checks only need a single best match; the UI test and
                // Nearest/All policies need the full candidate list.
                sc.reqWantMatches = ov && imgOverride_.wantMatches;
                if (renderer_.requestRegion(slot, x0, y0, w, h))
                    sc.requested = true, sc.requestedAt = now;
                else
                    sc.tested = true, sc.found = false, sc.score = 0, sc.lastUs = now;  // definitive negative
                sc.nextAt = now + int64_t(interval) * 1000;
            }
        }
        // image monitors consume image scans — but only ones taken with the image's
        // own parameters, so a macro step's override can never corrupt them.
        if (sc.tested && sc.defaultParams && nowMicros() - sc.lastUs <= 500000) {
            for (int mi = 0; mi < static_cast<int>(a.monitors.size()); ++mi) {
                const Monitor& m = a.monitors[mi];
                if (m.image != ii || (m.macro < 0 && m.key <= 0)) continue;
                if (mi >= static_cast<int>(monitorRt_.size()) || !(automationOn_ || monitorRt_[mi].switchedOn)) continue;
                if (!monitorRt_[mi].onSample(sc.found, now, m, macros_.active())) {
                    if (!sc.found && monitorRt_[mi].onAbsent(now, m)) {
                        if (m.elseStop) monitorRt_[mi].switchedOn = false;
                        else if (m.elseMacro >= 0) macros_.start(a, m.elseMacro, now);
                        else if (m.elseKey > 0) { std::lock_guard lk(inputMu_); input_.onKey(m.elseKey, true, now, InputSource::Monitor); pendingKeyUps_.push_back({m.elseKey, now + 40000}); }
                    }
                    continue;
                }
                ++detections_;
                if (m.macro >= 0) macros_.start(a, m.macro, now);
                else { std::lock_guard lk(inputMu_); input_.onKey(m.key, true, now, InputSource::Monitor); pendingKeyUps_.push_back({m.key, now + 40000}); }
            }
        }
    }
    if (regionProbe_.active && !regionProbe_.ready) serviceRegionProbe(now);
    if (detectProbe_.active && !detectProbe_.ready) serviceDetectProbe(detectProbe_, kDetectProbeSlot, now);
    if (detectPreview_.active) {
        // Live preview: as soon as one inference lands, re-arm the next frame —
        // boxes track the video at ~the model's inference rate.
        if (detectPreview_.ready && now - detectPreview_.readyAt >= 80000) {
            detectPreview_.ready = false;
            detectPreview_.requested = false;
        }
        serviceDetectProbe(detectPreview_, kDetectPreviewSlot, now);
    }
    // Standalone watcher: autostart on first video, then re-scan on the
    // configured cadence so boxes track moving targets continuously.
    if (a.detect.autostart && !detectWatchOn_ && !detectAutoArmed_) {
        detectAutoArmed_ = true;
        setDetectWatch(true);
    }
    if (detectWatchOn_) {
        DetectProbe& p = detectWatch_;
        p.active = true;
        p.region = a.detect.region;
        p.classFilter = a.detect.classes;
        p.conf = std::clamp(a.detect.confPct, 1, 99) / 100.0f;
        p.exclOn = a.detect.exclOn;
        p.exclRegion = a.detect.excl;
        p.mateOn = a.detect.mateOn;
        p.mateSat = float(a.detect.mateSat);
        p.mateMinRatio = a.detect.mateMinPct / 100.0f;
        p.redOn = a.detect.redOn;
        p.redMinRatio = a.detect.redMinPct / 100.0f;
        p.redBoost = a.detect.redBoostPct / 100.0f;
        // Diagnostics session recording (opt-in): bounded JSONL, no frames.
        if (a.detect.dbgRecord && !detRec_.is_open()) {
            detRec_.open(std::filesystem::u8path(narrow(platform::appDataDir()) + "/vision_log.jsonl"),
                         std::ios::trunc);
            detRecLines_ = 0;
        } else if (!a.detect.dbgRecord && detRec_.is_open()) {
            detRec_.close();
        }
        // Hysteresis: the worker emits hits down to trackLowPct of the user
        // threshold so weak detections keep EXISTING tracks alive through blur
        // dips; the tracker still requires full confidence (or a red-outline
        // rescue) to spawn a new box.
        const float spawnConf = std::clamp(a.detect.confPct, 1, 99) / 100.0f;
        const float lowFrac = std::clamp(a.detect.trackLowPct, 10, 95) / 100.0f;
        p.conf = std::max(0.03f, spawnConf * lowFrac);
        // Rate governor is the SINGLE cadence control: `inferFps` presets
        // (15/20/30/45/60) or Auto (adapts to measured p95 with hysteresis).
        // The legacy intervalMs field stays in the file but no longer gates —
        // two cadence knobs that could contradict each other were worse.
        inferSched_.setMode(a.detect.inferFps);
        const bool scanBusy = p.requested || p.pendingReqId != 0;
        if (p.ready && inferSched_.due(now, scanBusy)) {
            p.ready = false;
            p.requested = false;
        }
        serviceDetectProbe(p, kDetectWatchSlot, now);
        if (p.readyAt != lastWatchResultUs_) {
            lastWatchResultUs_ = p.readyAt;
            const int64_t tt = nowMicros();
            // Detection Fusion: contained/duplicate hits collapse into one
            // character record before tracking — a face-box inside a body-box
            // must never count as two targets. Raw hits stay in p.rawHits.
            std::vector<Detection> feed;
            if (a.detect.fusionOn) {
                fusedChars_ = fuseDetections(p.hits, FusionCfg{});
                feed.reserve(fusedChars_.size());
                for (const CharacterDet& c : fusedChars_) {
                    Detection d;
                    d.box = c.box; d.confidence = c.conf; d.classId = c.classId;
                    feed.push_back(d);
                }
            } else {
                fusedChars_.clear();
                feed = p.hits;
            }
            detectTracker_.update(feed, spawnConf, a.detect, p.readyAt, aimHeld_ ? aimTrackId_ : 0);
            detTrackUs_.add(static_cast<double>(nowMicros() - tt));
            // Plant-gain calibration: the locked box's measured drift since the
            // previous scan vs the counts we emitted in between estimates
            // frame-px of crosshair travel per count (kPxPerCount). Accepted
            // only when the drift OPPOSES our emission (camera-induced) and
            // both magnitudes clear the noise floor — target self-motion
            // contaminates single samples, but the sign gate + EMA keep the
            // estimate honest.
            const uint32_t cw = renderer_.videoWidth(), ch = renderer_.videoHeight();
            if (aimTrackId_ && cw && ch) {
                const DetectTrack* lt = nullptr;
                for (const DetectTrack& t : detectTracker_.tracks)
                    if (t.id == aimTrackId_) { lt = &t; break; }
                if (lt) {
                    const Vec2 mc{lt->measBox.x + lt->measBox.w * 0.5f,
                                  lt->measBox.y + lt->measBox.h * 0.5f};
                    if (aimPrevMeasC_.x >= 0.0f) {
                        auto learn = [](float driftPx, float counts, float& ema) {
                            if (counts * driftPx >= 0.0f || std::abs(counts) < 40.0f ||
                                std::abs(driftPx) < 6.0f) return;
                            const float k = std::abs(driftPx) / std::abs(counts);
                            if (k < 0.02f || k > 20.0f) return;
                            ema = (ema <= 0.0f) ? k : ema * 0.85f + k * 0.15f;
                        };
                        learn((mc.x - aimPrevMeasC_.x) * float(cw), aimEmitX_, aimKx_);
                        learn((mc.y - aimPrevMeasC_.y) * float(ch), aimEmitY_, aimKy_);
                        if (aimKx_ > 0.0f && aimKy_ > 0.0f)
                            aimCtrl_.kPxPerCount = (aimKx_ + aimKy_) * 0.5f;
                        else if (aimKx_ > 0.0f) aimCtrl_.kPxPerCount = aimKx_;
                        else if (aimKy_ > 0.0f) aimCtrl_.kPxPerCount = aimKy_;
                    }
                    aimPrevMeasC_ = mc;
                }
            } else aimPrevMeasC_ = {-1.0f, -1.0f};
            aimEmitX_ = aimEmitY_ = 0.0f;
        }
    }
    const int64_t ta = nowMicros();
    tickAimAssist(now, a.detect);
    detAimUs_.add(static_cast<double>(nowMicros() - ta));
    macros_.tick(now);
    if (graphRunner_) graphRunner_->tick(now);
}

// ---- NEXPLAY Automation Studio graph runner ---------------------------------

void App::studioRunGraph(const std::string& graphId, bool debug, uint32_t entryNodeId) {
    if (!graphRunner_) return;
    Automation& a = profile_.automation;
    for (const studio::AutomationGraph& g : a.studioGraphs) {
        if (g.id != graphId) continue;
        if (!sessionActive() || !renderer_.hasVideo()) {
            showToast(i18n::tr("Connect a device before running a graph"));
            return;
        }
        graphRunner_->setDebug(debug);
        if (graphRunner_->start(a, g, nowMicros(), entryNodeId)) {
            studioRunningGraphId_ = graphId;
            showToast(i18n::tr("Graph started"));
        } else {
            showToast(graphRunner_->message());
        }
        return;
    }
    showToast(i18n::tr("Graph not found"));
}

void App::studioStopGraph() {
    if (graphRunner_ && graphRunner_->active()) graphRunner_->cancel("stopped by user");
}

// ---- studio graph color probes ----------------------------------------------
// One shared async readback on kGraphProbeSlot serves both Get Pixel Color
// (a tiny 3x3 sample) and Find Color on an arbitrary region (CIELAB scan).

void App::serviceRegionProbe(int64_t now) {
    RegionProbe& p = regionProbe_;
    const uint32_t vw = renderer_.videoWidth(), vh = renderer_.videoHeight();
    if (!vw || !vh) return;
    if (!p.requested) {
        const Rect r = p.wantsScan
                           ? p.region
                           : Rect{p.point.x - 0.004f, p.point.y - 0.004f, 0.008f, 0.008f};
        const uint32_t x0 = std::clamp(uint32_t(std::lround(r.x * vw)), 0u, vw > 2 ? vw - 2 : 0u);
        const uint32_t y0 = std::clamp(uint32_t(std::lround(r.y * vh)), 0u, vh > 2 ? vh - 2 : 0u);
        // Bound the readback so a full-screen Find Color stays a one-frame blit.
        const uint32_t w = std::clamp(uint32_t(std::lround(r.w * vw)) & ~1u, 2u, 640u);
        const uint32_t h = std::clamp(uint32_t(std::lround(r.h * vh)) & ~1u, 2u, 640u);
        if (renderer_.requestRegion(kGraphProbeSlot, x0, y0, w, h)) {
            p.requested = true;
            p.requestedAt = now;
        } else {
            p.ready = true;  // readback rejected — answer with "no data"
            p.readyAt = now;
            p.found = false;
            p.similarity = 0.0f;
        }
        return;
    }
    render::Renderer::RegionPixels px;
    if (!renderer_.pollRegion(kGraphProbeSlot, px)) {
        if (now - p.requestedAt > 250000) p.requested = false;  // stale — re-request
        return;
    }
    p.requested = false;
    p.ready = true;
    p.readyAt = now;
    if (px.rgb.empty() || px.w == 0 || px.h == 0) {
        p.found = false;
        p.similarity = 0.0f;
        return;
    }
    if (!p.wantsScan) {
        uint64_t rs = 0, gs = 0, bs = 0;
        const size_t n = px.rgb.size() / 3;
        for (size_t i = 0; i < n; ++i) { rs += px.rgb[i * 3]; gs += px.rgb[i * 3 + 1]; bs += px.rgb[i * 3 + 2]; }
        p.color = studio::ColorVal{uint8_t(rs / n), uint8_t(gs / n), uint8_t(bs / n), 255, 12.0f};
        p.center = p.point;
        p.found = true;
        p.similarity = 1.0f;
        return;
    }
    // CIELAB scan: centroid + average of pixels within tolerance of the target.
    const Lab tl = rgbToLab(p.target.r, p.target.g, p.target.b);
    uint64_t rs = 0, gs = 0, bs = 0, hits = 0;
    double sx = 0, sy = 0;
    float bestDe = 1e9f;
    for (uint32_t y = 0; y < px.h; ++y) {
        for (uint32_t x = 0; x < px.w; ++x) {
            const size_t o = (size_t(y) * px.w + x) * 3;
            const float de = deltaE(tl, rgbToLab(px.rgb[o], px.rgb[o + 1], px.rgb[o + 2]));
            if (de < bestDe) bestDe = de;
            if (de <= p.target.tolerance) {
                ++hits;
                rs += px.rgb[o]; gs += px.rgb[o + 1]; bs += px.rgb[o + 2];
                sx += x; sy += y;
            }
        }
    }
    p.found = hits >= 4;
    if (p.found) {
        const double cx = sx / hits, cy = sy / hits;
        // centroid back to normalized frame coordinates
        const float nx = (float(px.x) + float(cx)) / float(vw);
        const float ny = (float(px.y) + float(cy)) / float(vh);
        p.center = {std::clamp(nx, 0.0f, 1.0f), std::clamp(ny, 0.0f, 1.0f)};
        p.color = studio::ColorVal{uint8_t(rs / hits), uint8_t(gs / hits), uint8_t(bs / hits), 255,
                                   p.target.tolerance};
    }
    p.similarity = std::clamp(1.0f - bestDe / 50.0f, 0.0f, 1.0f);
}

std::optional<studio::ColorVal> App::graphPixelColor(Vec2 pt) {
    RegionProbe& p = regionProbe_;
    const bool same = p.active && !p.wantsScan && std::abs(p.point.x - pt.x) < 1e-4f &&
                      std::abs(p.point.y - pt.y) < 1e-4f;
    if (same && p.ready && nowMicros() - p.readyAt <= 300000) return p.color;
    if (same && !p.ready) return std::nullopt;  // still in flight
    p.active = true;
    p.wantsScan = false;
    p.point = pt;
    p.requested = false;
    p.ready = false;
    return std::nullopt;
}

std::optional<studio::ColorScanResult> App::graphColorScan(const Rect& region,
                                                         const studio::ColorVal& target) {
    RegionProbe& p = regionProbe_;
    const bool same = p.active && p.wantsScan && p.region.x == region.x && p.region.y == region.y &&
                      p.region.w == region.w && p.region.h == region.h && p.target.r == target.r &&
                      p.target.g == target.g && p.target.b == target.b &&
                      std::abs(p.target.tolerance - target.tolerance) < 0.01f;
    if (same && p.ready && nowMicros() - p.readyAt <= 300000) {
        return studio::ColorScanResult{p.found, p.center, p.color, p.similarity};
    }
    if (same && !p.ready) return std::nullopt;
    p.active = true;
    p.wantsScan = true;
    p.region = region;
    p.target = target;
    p.requested = false;
    p.ready = false;
    return std::nullopt;
}

// ---- studio graph object-detection probes ------------------------------------
// Three stages per probe: GPU readback on its own slot -> inference job on the
// scan worker (ONNX, ~15-40ms) -> mailbox result drained here.

void App::serviceDetectProbe(DetectProbe& p, int slot, int64_t now) {
    const uint32_t vw = renderer_.videoWidth(), vh = renderer_.videoHeight();
    if (!vw || !vh) return;

    DetectWorkerResult& box = slot == kDetectWatchSlot   ? detectWatchResult_
                              : slot == kDetectPreviewSlot ? detectPreviewResult_
                                                           : detectResult_;
    if (p.pendingReqId) {
        // Inference finished? Drain the mailbox matching our request id.
        if (box.ready.load(std::memory_order_acquire) && box.requestId == p.pendingReqId) {
            p.ready = true;
            p.readyAt = now;
            p.pendingReqId = 0;
            p.lastInferUs = box.inferUs;
            p.frameId = box.frameId;
            p.captureUs = box.captureUs;
            if (slot == kDetectWatchSlot) inferSched_.noteInferUs(box.inferUs, now);
            detInferUs_.add(static_cast<double>(box.inferUs));
            if (box.doneAtUs) detMailUs_.add(static_cast<double>(now - box.doneAtUs));
            if (p.readbackAtUs) detEndToEndUs_.add(static_cast<double>(now - p.readbackAtUs));
            detReadbackUs_.add(static_cast<double>(p.readbackAtUs - p.requestedAt));
            p.result = {};
            p.hits.clear();
            // Diagnostics recording (bounded JSONL — opt-in, never screenshots).
            if (slot == kDetectWatchSlot && detRec_.is_open() && detRecLines_ < 20000) {
                detRec_ << "{\"ts\":" << now << ",\"frame\":" << box.frameId
                        << ",\"capUs\":" << box.captureUs << ",\"inferUs\":" << box.inferUs
                        << ",\"hits\":" << box.hits.size() << ",\"lock\":" << (int)targetLock_.state
                        << ",\"tid\":" << targetLock_.trackId << "}\n";
                ++detRecLines_;
            }
            // hits are region-normalized; promote to frame-normalized and pick best.
            float bestConf = -1.0f;
            const auto& names = detectClassNames_;
            for (const Detection& d : box.hits) {
                Detection fd = d;
                fd.box.x = p.region.x + d.box.x * p.region.w;
                fd.box.y = p.region.y + d.box.y * p.region.h;
                fd.box.w = d.box.w * p.region.w;
                fd.box.h = d.box.h * p.region.h;
                p.hits.push_back(fd);
                if (d.confidence > bestConf) {
                    bestConf = d.confidence;
                    p.result.bestBox = fd.box;
                    p.result.center = {fd.box.x + fd.box.w * 0.5f, fd.box.y + fd.box.h * 0.5f};
                    p.result.className = (d.classId >= 0 && d.classId < (int)names.size())
                                             ? names[d.classId]
                                             : "class " + std::to_string(d.classId);
                }
            }
            p.result.found = !p.hits.empty();
            p.result.confidence = std::max(0.0f, bestConf);
            p.result.matchCount = (int)p.hits.size();
            p.rawHits = p.hits;  // pre-fusion snapshot for the debug overlay
            box.ready.store(false, std::memory_order_release);
        }
        return;
    }

    if (!p.requested) {
        const Rect& r = p.region;
        const uint32_t x0 = std::clamp(uint32_t(std::lround(r.x * vw)), 0u, vw > 2 ? vw - 2 : 0u);
        const uint32_t y0 = std::clamp(uint32_t(std::lround(r.y * vh)), 0u, vh > 2 ? vh - 2 : 0u);
        // Cap the readback: the model letterboxes to its input size anyway, so
        // beyond ~1280px we only pay copy cost, not accuracy.
        const uint32_t w = std::clamp(uint32_t(std::lround(r.w * vw)) & ~1u, 2u, 1280u);
        const uint32_t h = std::clamp(uint32_t(std::lround(r.h * vh)) & ~1u, 2u, 1280u);
        if (renderer_.requestRegion(slot, x0, y0, w, h)) {
            p.requested = true;
            p.requestedAt = now;
        } else {
            p.ready = true;
            p.readyAt = now;
            p.result = {};
            p.hits.clear();  // tracker must see this as a miss, not a re-detect
        }
        return;
    }

    render::Renderer::RegionPixels px;
    if (!renderer_.pollRegion(slot, px)) {
        if (now - p.requestedAt > 250000) p.requested = false;  // stale — re-request
        return;
    }
    p.requested = false;
    p.readbackAtUs = now;
    if (px.rgb.empty() || px.w == 0 || px.h == 0) {
        p.ready = true;
        p.readyAt = now;
        p.result = {};
        p.hits.clear();
        return;
    }
    // Hand the frame to the inference worker. Resolve class names -> ids here
    // (names the model doesn't know are dropped = match nothing).
    ScanJob job;
    job.isDetect = true;
    job.px = std::move(px);
    job.vw = vw;
    job.vh = vh;
    job.detectConf = p.conf;
    job.detectRegion = p.region;
    job.detectFloor = p.redOn ? p.conf * 0.5f : p.conf;
    job.detectExclOn = p.exclOn;
    job.detectExcl = p.exclRegion;
    job.detectMateOn = p.mateOn;
    job.mateSat = p.mateSat;
    job.mateMinRatio = p.mateMinRatio;
    job.detectRedOn = p.redOn;
    job.redMinRatio = p.redMinRatio;
    job.redBoost = p.redBoost;
    job.requestId = ++detectReqId_;
    job.detectTarget = slot == kDetectWatchSlot ? 2 : slot == kDetectPreviewSlot ? 1 : 0;
    job.frameId = ++detectFrameSeq_;
    job.captureUs = p.readbackAtUs;
    {
        std::lock_guard lk(detectStatusMu_);
        const auto& names = detectClassNames_;
        for (const std::string& want : p.classFilter)
            for (size_t i = 0; i < names.size(); ++i)
                if (names[i] == want) { job.detectClasses.push_back((int)i); break; }
        // Include the "self" class internally so the worker can suppress enemy
        // boxes sitting on the player's avatar — even when the user filters to
        // enemies only. Auto-added self hits are never emitted.
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] != "self") continue;
            job.selfIdx = (int)i;
            // Only auto-add under an explicit class filter — an empty list
            // already means "all classes" and self flows through naturally.
            if (!job.detectClasses.empty() &&
                std::find(job.detectClasses.begin(), job.detectClasses.end(), (int)i) == job.detectClasses.end()) {
                job.detectClasses.push_back((int)i);
                job.selfAutoAdded = true;
            }
            break;
        }
    }
    p.pendingReqId = job.requestId;
    {
        std::lock_guard lk(scanMu_);
        scanQueue_.push(std::move(job));
    }
    scanCv_.notify_one();
}

std::optional<studio::ObjectScanResult> App::graphObjectScan(
    const Rect& region, const std::vector<std::string>& classFilter, float conf) {
    DetectProbe& p = detectProbe_;
    const bool same = p.active && p.region.x == region.x && p.region.y == region.y &&
                      p.region.w == region.w && p.region.h == region.h &&
                      p.classFilter == classFilter && std::abs(p.conf - conf) < 0.01f;
    if (same && p.ready && nowMicros() - p.readyAt <= 300000) return p.result;
    if (same && (!p.ready || p.pendingReqId)) return std::nullopt;
    p.active = true;
    p.region = region;
    p.classFilter = classFilter;
    p.conf = conf;
    p.requested = false;
    p.pendingReqId = 0;
    p.ready = false;
    p.hits.clear();
    return std::nullopt;
}

// ---- detection tracking -----------------------------------------------------
// IoU-matches each watcher result to tracks, blends positions, estimates
// velocity so boxes keep gliding between scans (camera pans included), and
// keeps tracks alive through short detection dips so they never flicker.
// spawnConf gates NEW tracks; existing tracks continue on weaker hits.

void App::setDetectWatch(bool on) {
    if (detectWatchOn_ == on) return;
    detectWatchOn_ = on;
    if (!on) {
        detectWatch_.active = false;
        detectWatch_.hits.clear();
        detectWatch_.ready = false;
        detectWatch_.requested = false;
        detectWatch_.pendingReqId = 0;
        detectTracker_.clear(nowMicros());
        lastWatchResultUs_ = 0;
        aimHeld_ = false;
        aimTrackId_ = 0;
        aimInside_ = false;
        aimAccX_ = aimAccY_ = 0.0f;
        aimLockAtUs_ = 0;
        aimSwallowMouse_ = false;
        targetLock_.reset(nowMicros());
        aimCtrl_.reset();
        fusedChars_.clear();
        if (detRec_.is_open()) detRec_.close();
    }
    showToast(i18n::trf("Object detection: %s", i18n::tr(on ? "ON" : "OFF")));
}

// ---- aim assist --------------------------------------------------------------
// While the assist key is held, each UI tick steers the camera toward a locked
// enemy track through onMouseDelta — the same sensitivity and smoothing path as
// the user's own hand. Closed-loop proportional control: the measured error
// (predicted aim point minus crosshair) produces a bounded mouse delta, and the
// next scan re-measures, so the pull self-corrects regardless of in-game
// sensitivity. Sticky lock keeps one target until it dies or the key releases.

void App::tickAimAssist(int64_t now, const DetectWatch& cfg) {
    if (!cfg.aimOn || !aimHeld_ || !detectWatchOn_) {
        targetLock_.reset(now); aimTrackId_ = 0; aimSwallowMouse_ = false; return;
    }
    // One lock for the check + emit: compute is pure math on UI-thread tracks.
    std::lock_guard lk(inputMu_);
    if (!input_.locked()) {
        targetLock_.reset(now); aimTrackId_ = 0; aimSwallowMouse_ = false; return;
    }

    const float leadS = std::clamp(cfg.aimLeadMs, 0, 500) / 1000.0f;
    const float range = std::clamp(cfg.aimRangePct, 5, 100) / 100.0f;
    const float predS = std::clamp(cfg.trackPredictMs, 0, 2000) / 1000.0f;

    auto predicted = [&](const DetectTrack& t) { return detectTracker_.predicted(t, now, predS); };
    auto centerOf = [](const Rect& r) { return Vec2{r.x + r.w * 0.5f, r.y + r.h * 0.5f}; };
    auto insideExcl = [&](const Vec2& c) {
        return cfg.exclOn && c.x >= cfg.excl.x && c.x <= cfg.excl.x + cfg.excl.w &&
               c.y >= cfg.excl.y && c.y <= cfg.excl.y + cfg.excl.h;
    };

    // ---- candidates: CONFIRMED enemy tracks only ---------------------------
    // Predicted boxes decide nothing; only real measurements may steer.
    std::vector<TargetCandidate> cands;
    for (const DetectTrack& t : detectTracker_.tracks) {
        if (t.classId != 0 || t.state != TrackState::Confirmed) continue;
        const Vec2 c = centerOf(predicted(t));
        if (insideExcl(c)) continue;
        TargetCandidate tc;
        tc.trackId = t.id;
        tc.box = predicted(t);
        tc.conf = t.conf;
        tc.confirmed = true;
        tc.state = t.state;
        cands.push_back(tc);
    }
    // Engage radius gates NEW acquisition only — never steals an active lock.
    if (!targetLock_.trackId)
        std::erase_if(cands, [&](const TargetCandidate& c) {
            const Vec2 cc = centerOf(c.box);
            return std::hypot(cc.x - 0.5f, cc.y - 0.5f) > range;
        });

    // ---- lock FSM: identity over proximity ---------------------------------
    SelectWeights sw = cfg.selPreset == 4
        ? SelectWeights{cfg.selCenterW / 100.0f, cfg.selSizeW / 100.0f,
                        cfg.selConfW / 100.0f, 0.0f}
        : selectPreset(cfg.selPreset);
    LockCfg lc;
    lc.graceUs = int64_t(std::clamp(cfg.lockGraceMs, 0, 2000)) * 1000;
    lc.switchHysteresis = cfg.switchHystPct / 100.0f;
    const TargetCandidate* steer = targetLock_.feed(cands, {0.5f, 0.5f}, sw, lc, now);
    aimTrackId_ = targetLock_.trackId;   // mirror for overlay + tracker protectId
    // Magnet: a live lock owns the camera — the hand can't fight the pull.
    // Releases automatically when the lock dies (trackId 0) or key lets go.
    aimSwallowMouse_ = (targetLock_.trackId != 0);
    if (!steer) { aimInside_ = aimCtrl_.inside; return; }
    aimInside_ = aimCtrl_.inside;

    const DetectTrack* track = nullptr;
    for (const DetectTrack& t : detectTracker_.tracks)
        if (t.id == steer->trackId) { track = &t; break; }
    if (!track) { targetLock_.cancel(now); aimTrackId_ = 0; return; }

    const Rect& pb = steer->box;
    const Vec2 pc = centerOf(pb);
    aimLastPt_ = pc;
    aimLastPtAtUs_ = now;
    // Out-of-frame guard: stale glide dragging the box off-screen releases.
    if (pc.x < -0.05f || pc.x > 1.05f || pc.y < -0.05f || pc.y > 1.05f) {
        targetLock_.cancel(now); aimTrackId_ = 0; return;
    }
    // Divergence guard unchanged: no convergence after 0.4 s of pulling -> drop.
    if (targetLock_.state == LockState::Locked) {
        if (aimLockAtUs_ == 0) { aimLockErr_ = std::hypot(pc.x - 0.5f, pc.y - 0.5f); aimLockAtUs_ = now; }
        else if (now - aimLockAtUs_ > 400000 &&
                 std::hypot(pc.x - 0.5f, pc.y - 0.5f) > aimLockErr_ * 0.9f) {
            targetLock_.cancel(now); aimTrackId_ = 0; aimLockAtUs_ = 0; return;
        }
    } else aimLockAtUs_ = 0;

    // Reference point inside the box (preset or custom rel). A small bounded
    // lead is allowed for strafing targets but never replaces the measurement.
    Vec2 pt = pointInBox(pb, PointPreset(std::clamp(cfg.pointPreset, 0, 4)),
                         cfg.pointX / 100.0f, cfg.pointY / 100.0f);
    pt.x = std::clamp(pt.x + std::clamp(track->vx * leadS, -pb.w * 0.3f, pb.w * 0.3f), 0.02f, 0.98f);
    pt.y = std::clamp(pt.y + std::clamp(track->vy * leadS, -pb.h * 0.3f, pb.h * 0.3f), 0.02f, 0.98f);
    targetLock_.lastPoint = pt;

    // ---- controller: region deadband + caps + rate/age limits --------------
    ControlCfg cc;
    cc.frameW = float(renderer_.videoWidth());
    cc.frameH = float(renderer_.videoHeight());
    if (cc.frameW <= 0 || cc.frameH <= 0) { cc.frameW = 1920.0f; cc.frameH = 1080.0f; }
    cc.kp = 8.0f;
    cc.speedPct = float(std::clamp(cfg.aimSpeedPct, 10, 100));
    cc.maxRatePx = float(cfg.ctrlMaxOut);   // emitted counts/s (physical speed cap)
    cc.deadzonePx = float(cfg.ctrlDeadzonePx);
    // Stop zone = character box inset by ctrlInnerPct: the pull must land the
    // crosshair inside the REAL hit area, not just brush the outer edge.
    cc.insetX = cc.insetY = std::clamp(cfg.ctrlInnerPct, 0, 45) / 100.0f;
    cc.maxRateHz = float(cfg.ctrlMaxRateHz);
    cc.maxMeasAgeUs = int64_t(cfg.ctrlMaxAgeMs) * 1000;
    cc.minConf = cfg.ctrlMinConfPct / 100.0f;
    ControlOut out = aimCtrl_.update({0.5f, 0.5f}, pb, pt, track->vx, track->vy,
                                     track->conf, track->lastMeasUs, now, cc);
    if (!out.steering) { aimInside_ = aimCtrl_.inside; return; }

    // Saturation diagnostics: how often the speed cap actually binds.
    ++aimSteerTicks_;
    if (out.saturated) ++aimSatTicks_;

    // Fractional accumulation -> integer mouse counts (unchanged emission path).
    aimAccX_ += out.dx;
    aimAccY_ += out.dy;
    const int dx = static_cast<int>(std::trunc(aimAccX_));
    const int dy = static_cast<int>(std::trunc(aimAccY_));
    if (!dx && !dy) return;
    aimAccX_ -= dx;
    aimAccY_ -= dy;
    aimEmitX_ += float(dx);   // plant-gain calibration window (reset each scan)
    aimEmitY_ += float(dy);
    input_.onMouseDelta(dx, dy, now);
}

std::vector<std::string> App::detectorClassNames() {
    {
        std::lock_guard lk(detectStatusMu_);
        if (!detectClassNames_.empty()) return detectClassNames_;
    }
    // Warm up the model on the worker so the inspector's class checkboxes fill
    // in as soon as the load lands (the job carries no pixels — it only loads).
    if (!detectWarmupQueued_) {
        detectWarmupQueued_ = true;
        ScanJob job;
        job.isDetect = true;
        job.detectTarget = 1;
        job.requestId = 0;  // no probe is waiting on this id
        {
            std::lock_guard lk(scanMu_);
            scanQueue_.push(std::move(job));
        }
        scanCv_.notify_one();
    }
    std::lock_guard lk(detectStatusMu_);
    return detectClassNames_;
}

std::string App::detectorStatus() const {
    std::lock_guard lk(detectStatusMu_);
    return detectStatusText_;
}

}  // namespace wasd::app
