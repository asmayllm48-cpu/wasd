#include "core/detector.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>

#ifdef WASD_WITH_ORT
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#endif

namespace wasd {

#ifndef WASD_WITH_ORT
// Stub build (onnxruntime not vendored): Detector reports "not available" so
// the app can still compile and tests still run.
struct Detector::Impl {};
Detector::Detector() : impl_(std::make_unique<Impl>()) {}
Detector::~Detector() = default;
bool Detector::loaded() const { return false; }
void Detector::unload() {}
bool Detector::load(const std::string&, std::string& err) {
    err = "onnxruntime is not bundled in this build";
    return false;
}
DetectResult Detector::detect(const uint8_t*, int, int, int, float, const std::vector<int>&, float) {
    return {};
}
bool Detector::usingGpu() const { return false; }
#else

struct Detector::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "wasd-detect"};
    Ort::SessionOptions opts;
    std::unique_ptr<Ort::Session> session;
    bool gpu = false;
};

Detector::Detector() : impl_(std::make_unique<Impl>()) {}
Detector::~Detector() = default;
bool Detector::usingGpu() const { return impl_->gpu; }

bool Detector::loaded() const { return impl_->session != nullptr; }
void Detector::unload() { impl_->session.reset(); classes_.clear(); inputW_ = inputH_ = 0; }

bool Detector::load(const std::string& path, std::string& err) {
    unload();
    try {
        impl_->opts.SetIntraOpNumThreads(2);  // keep the UI thread's cores free
        impl_->opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // Prefer DirectML on the default GPU; the session silently falls back
        // to CPU if the EP cannot be created (no D3D12 device, old driver, ...).
        impl_->gpu = false;
        if (const OrtApi* api = OrtGetApiBase()->GetApi(ORT_API_VERSION)) {
            const OrtDmlApi* dml = nullptr;
            if (OrtStatus* s = api->GetExecutionProviderApi(
                    "DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dml)))
                api->ReleaseStatus(s);
            if (dml) {
                if (OrtStatus* st = dml->SessionOptionsAppendExecutionProvider_DML(
                        impl_->opts, /*device_id=*/0))
                    api->ReleaseStatus(st);
                else
                    impl_->gpu = true;
            }
        }
        std::wstring wpath(path.begin(), path.end());
        impl_->session = std::make_unique<Ort::Session>(impl_->env, wpath.c_str(), impl_->opts);

        // Input geometry: [1,3,H,W] (Ultralytics exports are square, e.g. 320).
        Ort::TypeInfo ti = impl_->session->GetInputTypeInfo(0);
        auto shape = ti.GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 4) { err = "unexpected model input rank"; unload(); return false; }
        inputH_ = static_cast<int>(shape[2]);
        inputW_ = static_cast<int>(shape[3]);
        if (inputW_ <= 0 || inputH_ <= 0) { err = "dynamic model input not supported"; unload(); return false; }

        // Class names come from the 'names' metadata written by Ultralytics:
        // "{0: 'soldier', 1: 'weapon', ...}".
        Ort::ModelMetadata md = impl_->session->GetModelMetadata();
        Ort::AllocatorWithDefaultOptions alloc;
        std::string s;
        if (auto namesVal = md.LookupCustomMetadataMapAllocated("names", alloc))
            s = namesVal.get();
        if (s.empty()) {
            // Some exports strip the map key lookup — enumerate keys instead.
            for (auto& k : md.GetCustomMetadataMapKeysAllocated(alloc)) {
                if (k && std::strcmp(k.get(), "names") == 0) {
                    if (auto v = md.LookupCustomMetadataMapAllocated(k.get(), alloc)) s = v.get();
                    break;
                }
            }
        }
        if (!s.empty()) {
            // "{0: 'a', 1: 'b'}" — entry separators carry ", " so the id must be
            // read from the first digit, not raw pos (atoi on ", 1:" returns 0).
            size_t pos = 0;
            std::vector<std::pair<int, std::string>> tmp;
            while (pos < s.size()) {
                while (pos < s.size() && !std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
                if (pos >= s.size()) break;
                const int id = std::atoi(s.c_str() + pos);
                while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
                size_t q1 = s.find('\'', pos);
                size_t q2 = s.find('"', pos);
                char qc = '\'';
                if (q1 == std::string::npos || (q2 != std::string::npos && q2 < q1)) { q1 = q2; qc = '"'; }
                if (q1 == std::string::npos) break;
                const size_t qe = s.find(qc, q1 + 1);
                if (qe == std::string::npos) break;
                tmp.push_back({id, s.substr(q1 + 1, qe - q1 - 1)});
                pos = qe + 1;
            }
            int maxId = -1;
            for (const auto& p : tmp) maxId = std::max(maxId, p.first);
            classes_.assign(maxId + 1, "");
            for (const auto& p : tmp)
                if (p.first >= 0) classes_[p.first] = p.second;
        }
        // Fallback: derive the class count from the output [1, 4+nc, anchors]
        // so unknown exports still get a selectable list.
        if (classes_.empty()) {
            Ort::TypeInfo oti = impl_->session->GetOutputTypeInfo(0);
            auto os = oti.GetTensorTypeAndShapeInfo().GetShape();
            const int nc = os.size() == 3 ? int(os[1]) - 4 : 1;
            classes_.assign(std::max(1, nc), "");
        }
        for (size_t i = 0; i < classes_.size(); ++i)
            if (classes_[i].empty()) classes_[i] = "class " + std::to_string(i);
        return true;
    } catch (const Ort::Exception& e) {
        err = e.what();
        unload();
        return false;
    }
}

static inline float iou(const float* a, const float* b) {
    const float ix0 = std::max(a[0], b[0]), iy0 = std::max(a[1], b[1]);
    const float ix1 = std::min(a[2], b[2]), iy1 = std::min(a[3], b[3]);
    const float iw = std::max(0.0f, ix1 - ix0), ih = std::max(0.0f, iy1 - iy0);
    const float inter = iw * ih;
    const float areaA = (a[2] - a[0]) * (a[3] - a[1]), areaB = (b[2] - b[0]) * (b[3] - b[1]);
    const float uni = areaA + areaB - inter;
    return uni > 0 ? inter / uni : 0.0f;
}

DetectResult Detector::detect(const uint8_t* rgb, int w, int h, int stride, float confThresh,
                              const std::vector<int>& classes, float iouThresh) {
    DetectResult out;
    if (!impl_->session || !rgb || w <= 0 || h <= 0) return out;
    if (stride <= 0) stride = w * 3;

    // Letterbox into the square model input (Ultralytics pads with 114).
    const int iw = inputW_, ih = inputH_;
    const float scale = std::min(static_cast<float>(iw) / w, static_cast<float>(ih) / h);
    const int rw = std::max(1, static_cast<int>(std::lround(w * scale)));
    const int rh = std::max(1, static_cast<int>(std::lround(h * scale)));
    const int padX = (iw - rw) / 2, padY = (ih - rh) / 2;
    std::vector<float> input(static_cast<size_t>(3) * ih * iw, 114.0f / 255.0f);
    for (int y = 0; y < rh; ++y) {
        const int sy = std::min(h - 1, static_cast<int>(y / scale));
        const uint8_t* srow = rgb + static_cast<size_t>(sy) * stride;
        for (int x = 0; x < rw; ++x) {
            const int sx = std::min(w - 1, static_cast<int>(x / scale));
            const uint8_t* px = srow + static_cast<size_t>(sx) * 3;
            const size_t di = static_cast<size_t>(y + padY) * iw + (x + padX);
            input[di] = px[0] / 255.0f;
            input[static_cast<size_t>(ih) * iw + di] = px[1] / 255.0f;
            input[2ull * ih * iw + di] = px[2] / 255.0f;
        }
    }

    int64_t dims[4] = {1, 3, ih, iw};
    Ort::MemoryInfo mi = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value in = Ort::Value::CreateTensor<float>(mi, input.data(), input.size(), dims, 4);

    const auto t0 = std::chrono::steady_clock::now();
    Ort::AllocatorWithDefaultOptions alloc;
    char* inName = impl_->session->GetInputNameAllocated(0, alloc).release();
    char* outName = impl_->session->GetOutputNameAllocated(0, alloc).release();
    std::vector<Ort::Value> outs;
    try {
        outs = impl_->session->Run(Ort::RunOptions{nullptr}, &inName, &in, 1, &outName, 1);
    } catch (const Ort::Exception&) {
        alloc.Free(inName);
        alloc.Free(outName);
        return out;
    }
    out.inferUs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    alloc.Free(inName);
    alloc.Free(outName);

    const float* pred = outs[0].GetTensorData<float>();
    auto oshape = outs[0].GetTensorTypeAndShapeInfo().GetShape();  // [1, ch, anchors]
    if (oshape.size() != 3) return out;
    const int ch = static_cast<int>(oshape[1]), anchors = static_cast<int>(oshape[2]);
    const int nclasses = ch - 4;
    if (nclasses <= 0) return out;

    const auto accepted = [&](int c) {
        return classes.empty() ||
               std::find(classes.begin(), classes.end(), c) != classes.end();
    };

    // Ultralytics layout: [1, 4+nc, N] — row-major, channel first.
    std::vector<std::array<float, 5>> cands;  // x0,y0,x1,y1,score(classId in idx)
    std::vector<int> cids;
    for (int a = 0; a < anchors; ++a) {
        int bestC = -1;
        float bestS = 0;
        for (int c = 0; c < nclasses; ++c) {
            const float s = pred[static_cast<size_t>(4 + c) * anchors + a];
            if (s > bestS) { bestS = s; bestC = c; }
        }
        if (bestS < confThresh || !accepted(bestC)) continue;
        const float cx = pred[static_cast<size_t>(0) * anchors + a];
        const float cy = pred[static_cast<size_t>(1) * anchors + a];
        const float bw = pred[static_cast<size_t>(2) * anchors + a];
        const float bh = pred[static_cast<size_t>(3) * anchors + a];
        cands.push_back({cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2, bestS});
        cids.push_back(bestC);
    }

    // Greedy NMS across classes.
    std::vector<int> order(cands.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int A, int B) { return cands[A][4] > cands[B][4]; });
    std::vector<bool> dead(cands.size(), false);
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const int i = order[oi];
        if (dead[i]) continue;
        const auto& b = cands[i];
        // Map from letterbox space back to the source region, clamped.
        Detection d;
        d.classId = cids[i];
        d.confidence = b[4];
        const float x0 = (b[0] - padX) / scale, y0 = (b[1] - padY) / scale;
        const float x1 = (b[2] - padX) / scale, y1 = (b[3] - padY) / scale;
        d.box = {std::clamp(x0 / w, 0.0f, 1.0f), std::clamp(y0 / h, 0.0f, 1.0f),
                 std::clamp((x1 - x0) / w, 0.0f, 1.0f), std::clamp((y1 - y0) / h, 0.0f, 1.0f)};
        if (d.box.w > 0.001f && d.box.h > 0.001f) out.hits.push_back(d);
        for (size_t oj = oi + 1; oj < order.size(); ++oj) {
            const int j = order[oj];
            if (!dead[j] && iou(b.data(), cands[j].data()) > iouThresh) dead[j] = true;
        }
    }
    return out;
}
#endif  // WASD_WITH_ORT

}  // namespace wasd
