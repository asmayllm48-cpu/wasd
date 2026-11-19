// ONNX object detection for the automation studio ("Find Object" node).
// Wraps onnxruntime so the rest of the app only deals in pixels + rectangles.
// Model: Ultralytics YOLOv8 export — input [1,3,H,W] RGB, output [1,4+nc,anchors]
// (nms=false), e.g. [1,16,2100] for 12 classes at 320px.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/coords.h"

namespace wasd {

struct Detection {
    int classId = -1;
    float confidence = 0.0f;
    Rect box{};          // normalized [0,1] within the scanned region
    bool boosted = false;  // sub-threshold hit rescued by the red-outline check
};

// One inference result: detections meeting the confidence threshold, already
// NMS-filtered and sorted by confidence descending.
struct DetectResult {
    std::vector<Detection> hits;
    int inferUs = 0;  // session run time
};

class Detector {
public:
    Detector();
    ~Detector();  // pimpl keeps onnxruntime out of this header

    Detector(const Detector&) = delete;
    Detector& operator=(const Detector&) = delete;

    // Loads the .onnx model. `err` gets a human-readable message on failure.
    bool load(const std::string& path, std::string& err);
    bool loaded() const;
    void unload();

    const std::vector<std::string>& classNames() const { return classes_; }
    int inputSize() const { return inputW_; }  // square model input
    // True when the session runs on a hardware EP (DirectML) instead of CPU.
    bool usingGpu() const;

    // Runs detection on an RGB24 frame region (top-down rows). `classes` selects
    // which class ids count; empty = all. Coordinates are normalized to the
    // input region (caller maps back to the frame).
    DetectResult detect(const uint8_t* rgb, int w, int h, int stride, float confThresh,
                        const std::vector<int>& classes, float iouThresh = 0.45f);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<std::string> classes_;
    int inputW_ = 0, inputH_ = 0;
};

}  // namespace wasd
