// Detector: model load + inference sanity on the bundled custom enemy YOLOv8n.
#include <doctest/doctest.h>

#include <filesystem>
#include <vector>
#include <windows.h>

#include "core/detector.h"

using namespace wasd;
namespace fs = std::filesystem;

TEST_CASE("detector loads the bundled onnx model") {
#ifdef WASD_WITH_ORT
    // Resolve <exe>\runtime\models\model.onnx (tests run from build/).
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const fs::path mp = fs::path(buf).parent_path() / "runtime" / "models" / "model.onnx";
    if (!fs::exists(mp)) return;  // model not shipped in this layout
    Detector det;
    std::string err;
    REQUIRE(det.load(mp.string(), err));
    CHECK(det.loaded());
    CHECK(det.inputSize() > 0);
    const auto names = det.classNames();
    REQUIRE(!names.empty());
    CHECK(names.front() == "enemy");

    // A solid gray frame must produce no detections at high confidence.
    const int w = 64, h = 64;
    std::vector<uint8_t> gray(size_t(w) * h * 3, 114);
    auto r = det.detect(gray.data(), w, h, w * 3, 0.9f, {});
    CHECK(r.hits.empty());
#else
    Detector det;
    std::string err;
    CHECK(!det.load("anything.onnx", err));
    CHECK(!err.empty());
#endif
}
