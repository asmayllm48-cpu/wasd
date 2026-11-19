// Detection Fusion: merges raw model hits that belong to the SAME physical
// character into one CharacterDet record. The model emits several class boxes
// (enemy/self/…) — part boxes (e.g. a partial/face-sized box) contained inside
// a full character box are attached, not counted twice. Overlap alone never
// merges — a big box next to a small one could be two players.
//
// Every fused record keeps `parts` (the original detections) so raw data is
// never destroyed; `partial` marks boxes clipped by the frame edge.
#pragma once
#include <algorithm>
#include <vector>

#include "core/coords.h"
#include "core/detector.h"

namespace wasd {

struct FusionCfg {
    float containmentRatio = 0.65f; // area of small box inside big box to attach
    float iouMerge = 0.55f;         // near-duplicate same-class boxes merge
    float centerDistFrac = 0.35f;   // center distance < frac*min(diag) = same entity
    float conf = 0.0f;              // extra floor (0 = pass-through)
};

struct CharacterDet {
    int classId = -1;
    Rect box{};                     // fused character box (union of parts)
    float conf = 0;                 // best part confidence
    std::vector<Detection> parts;   // original detections, never modified
    bool partial = false;           // box touches a frame edge
    bool fused = false;             // >1 part merged
    int detId = 0;                  // index of primary part
};

// Greedy fusion: sort by conf desc; each seed box absorbs same-CLASS detections
// that are mostly inside it OR near-duplicates (high IoU / tiny center gap).
// Cross-class merges require both ids listed in `compat`.
inline std::vector<CharacterDet> fuseDetections(std::vector<Detection> hits,
                                                const FusionCfg& cfg,
                                                const std::vector<std::pair<int,int>>& compat = {}) {
    auto ctrDist = [](const Rect& a, const Rect& b) {
        const float dx = (a.x + a.w * .5f) - (b.x + b.w * .5f);
        const float dy = (a.y + a.h * .5f) - (b.y + b.h * .5f);
        return std::sqrt(dx * dx + dy * dy);
    };
    auto iou = [](const Rect& a, const Rect& b) {
        const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
        const float x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
        const float i = std::max(0.f, x1 - x0) * std::max(0.f, y1 - y0);
        const float u = a.w * a.h + b.w * b.h - i;
        return u > 1e-6f ? i / u : 0.f;
    };
    auto inside = [](const Rect& sm, const Rect& big) {
        const float ix = std::max(0.f, std::min(sm.x + sm.w, big.x + big.w) - std::max(sm.x, big.x));
        const float iy = std::max(0.f, std::min(sm.y + sm.h, big.y + big.h) - std::max(sm.y, big.y));
        const float a = sm.w * sm.h;
        return a > 1e-6f ? (ix * iy) / a : 0.f;
    };
    auto okClass = [&](int a, int b) {
        if (a == b) return true;
        for (auto& p : compat) if ((p.first == a && p.second == b) || (p.first == b && p.second == a)) return true;
        return false;
    };

    std::stable_sort(hits.begin(), hits.end(), [](const Detection& a, const Detection& b) {
        return a.confidence > b.confidence;
    });
    std::vector<CharacterDet> out;
    std::vector<char> used(hits.size(), 0);
    for (size_t i = 0; i < hits.size(); ++i) {
        if (used[i] || hits[i].confidence < cfg.conf) continue;
        CharacterDet c;
        c.classId = hits[i].classId;
        c.box = hits[i].box;
        c.conf = hits[i].confidence;
        c.detId = (int)i;
        c.parts.push_back(hits[i]);
        used[i] = 1;
        for (size_t j = i + 1; j < hits.size(); ++j) {
            if (used[j] || hits[j].confidence < cfg.conf) continue;
            if (!okClass(c.classId, hits[j].classId)) continue;
            const float cont = inside(hits[j].box, c.box);
            const float ov = iou(hits[j].box, c.box);
            const float dd = ctrDist(hits[j].box, c.box);
            const float diag = std::min(std::hypot(c.box.w, c.box.h), std::hypot(hits[j].box.w, hits[j].box.h));
            // attach: mostly inside, near-duplicate, or essentially same center
            if (cont >= cfg.containmentRatio || ov >= cfg.iouMerge ||
                dd <= diag * cfg.centerDistFrac) {
                used[j] = 1;
                c.fused = true;
                c.parts.push_back(hits[j]);
                const float x0 = std::min(c.box.x, hits[j].box.x), y0 = std::min(c.box.y, hits[j].box.y);
                const float x1 = std::max(c.box.x + c.box.w, hits[j].box.x + hits[j].box.w);
                const float y1 = std::max(c.box.y + c.box.h, hits[j].box.y + hits[j].box.h);
                c.box = {x0, y0, x1 - x0, y1 - y0};
                c.conf = std::max(c.conf, hits[j].confidence);
            }
        }
        c.partial = c.box.x <= 0.005f || c.box.y <= 0.005f ||
                    c.box.x + c.box.w >= 0.995f || c.box.y + c.box.h >= 0.995f;
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace wasd
