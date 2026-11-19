// Reference-image matching ("did this saved crop appear on screen?").
// Pure logic: gray zero-mean NCC, coarse-to-fine over the search region,
// multi-scale (0.9 / 1.0 / 1.1) so small DPI changes survive. No dependencies.
//
// The reported score is a normalized cross-correlation in [-1, 1]; the user-facing
// "threshold %" is exactly this value, NOT a statistical probability.
#pragma once
#include <cstdint>
#include <vector>

#include "core/automation.h"

namespace wasd {

// Frame view: RGB24, top-down rows.
struct FrameView {
    const uint8_t* rgb = nullptr;
    int w = 0, h = 0, stride = 0;
};

// Searches ref.rgb inside frame (restricted by ref.region). Keeps scale variation
// within +/-10 %. Deterministic and allocation-light: one gray scratch reused per scale.
ImageMatch matchImage(const FrameView& frame, const ImageRef& ref);

// Multi-match variant: returns up to `maxMatches` non-overlapping detections,
// best score first (only candidates meeting ref.thresholdPct). Powers the
// First / Nearest / All selection policies of the unified image action.
std::vector<ImageMatch> matchImageAll(const FrameView& frame, const ImageRef& ref, int maxMatches = 4);

// Same as matchImage, but the search window is given as view pixels instead of
// ref.region. Used when `frame` is already clipped to the search region
// (e.g. a GPU readback containing exactly that region's pixels).
ImageMatch matchImageInRect(const FrameView& frame, const ImageRef& ref, int sx, int sy, int sw, int sh);
std::vector<ImageMatch> matchImageInRectAll(const FrameView& frame, const ImageRef& ref, int sx, int sy, int sw, int sh,
                                            int maxMatches = 4);

// Translates a match obtained in a clipped sub-view (x,y,w,h in frame pixels)
// back into frame-normalized coordinates.
ImageMatch toFrameCoords(const ImageMatch& m, int x, int y, int w, int h, int fw, int fh);

// NCC in [-1, 1] of two equal-sized gray blocks (row stride-relative addresses).
float grayNcc(const float* t, const float* c, int n);

}  // namespace wasd
