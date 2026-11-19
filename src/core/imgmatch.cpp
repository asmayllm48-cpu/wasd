#include "core/imgmatch.h"

#include <algorithm>
#include <cmath>

namespace wasd {
namespace {

inline float grayOf(const uint8_t* p) { return (int(p[0]) * 77 + int(p[1]) * 150 + int(p[2]) * 29) / 256.0f; }

inline float refGray(const ImageRef& ref, float sx, float sy /* source px */) {
    const int x = std::clamp(int(sx), 0, int(ref.w) - 1);
    const int y = std::clamp(int(sy), 0, int(ref.h) - 1);
    return grayOf(ref.rgb.data() + (size_t(y) * ref.w + x) * 3);
}

inline float frameGray(const FrameView& f, int x, int y) {
    x = std::clamp(x, 0, f.w - 1);
    y = std::clamp(y, 0, f.h - 1);
    return grayOf(f.rgb + size_t(y) * f.stride + size_t(x) * 3);
}

// One candidate detection: coarse-pass hit refined by the fine pass.
struct Cand {
    float score = -2.0f;
    float scale = 1.0f;
    int x = 0, y = 0, tw = 0, th = 0;
};

// Coarse pass over all scales; keeps the best `keep` candidates spaced by at
// least half the template size (so the list covers distinct screen objects).
std::vector<Cand> coarseScan(const FrameView& frame, const ImageRef& ref, int x0, int y0, int rw, int rh, int keep) {
    std::vector<Cand> out;
    // Wider range than before: the same item can render ~20% larger/smaller in
    // different UI contexts (quick-bar vs backpack grid) and still be a match.
    static constexpr float kScales[] = {0.8f, 0.9f, 1.0f, 1.1f, 1.25f};
    for (float scale : kScales) {
        // Template grid at this scale: sampled from the reference with nearest sampling.
        const int tW = std::min(int(std::lround(float(ref.w) * scale)), rw);
        const int tH = std::min(int(std::lround(float(ref.h) * scale)), rh);
        if (tW < 4 || tH < 4) continue;

        const int step = std::max(2, std::min(tW, tH) / 10);
        std::vector<float> cT;
        cT.assign(size_t(tW) * tH, 0.0f);  // full gray template at this scale
        for (int y = 0; y < tH; ++y)
            for (int x = 0; x < tW; ++x) cT[size_t(y) * tW + x] = refGray(ref, x / scale, y / scale);

        constexpr int kGrid = 10;
        float tSig[kGrid * kGrid], rSig[kGrid * kGrid];
        for (int gy = 0; gy < kGrid; ++gy)
            for (int gx = 0; gx < kGrid; ++gx) tSig[gy * kGrid + gx] = cT[size_t(gy * tH / kGrid) * tW + gx * tW / kGrid];

        const int sep = std::max(4, std::min(tW, tH) / 2);
        for (int y = 0; y <= rh - tH; y += step) {
            for (int x = 0; x <= rw - tW; x += step) {
                for (int gy = 0; gy < kGrid; ++gy)
                    for (int gx = 0; gx < kGrid; ++gx)
                        rSig[gy * kGrid + gx] = frameGray(frame, x0 + x + gx * tW / kGrid, y0 + y + gy * tH / kGrid);
                const float s = grayNcc(tSig, rSig, kGrid * kGrid);
                if (int(out.size()) >= keep && s <= out.back().score) continue;
                // Skip positions that sit on a better/equal candidate already kept.
                bool dup = false;
                for (const Cand& c : out)
                    if (c.score >= s && std::abs(c.x - (x0 + x)) <= sep && std::abs(c.y - (y0 + y)) <= sep) {
                        dup = true;
                        break;
                    }
                if (dup) continue;
                Cand c;
                c.score = s, c.scale = scale, c.x = x0 + x, c.y = y0 + y, c.tw = tW, c.th = tH;
                auto pos = std::find_if(out.begin(), out.end(), [&](const Cand& o) { return s > o.score; });
                out.insert(pos, c);
                if (int(out.size()) > keep) out.pop_back();
            }
        }
    }
    return out;
}

// Fine pass: strict full-template NCC around the coarse point; updates c in place.
void fineTune(const FrameView& frame, const ImageRef& ref, Cand& c) {
    const int tW = c.tw, tH = c.th;
    std::vector<float> t(size_t(tW) * tH);
    for (int y = 0; y < tH; ++y)
        for (int x = 0; x < tW; ++x) t[size_t(y) * tW + x] = refGray(ref, x / c.scale, y / c.scale);

    // If the template is large, compare on a fixed 48x48 sampling grid instead of every pixel.
    const bool gridMode = size_t(tW) * tH > 48 * 48;
    std::vector<float> tGrid, rGrid;
    int n = tW * tH, gW = tW, gH = tH;
    if (gridMode) {
        gW = 48, gH = 48, n = gW * gH;
        tGrid.assign(size_t(n), 0.0f);
        for (int y = 0; y < gH; ++y)
            for (int x = 0; x < gW; ++x) tGrid[size_t(y) * gW + x] = t[size_t(y * tH / gH) * tW + x * tW / gW];
    }
    rGrid.assign(size_t(n), 0.0f);

    const int d = std::max(2, std::min(tW, tH) / 10) * 2;  // refine radius
    for (int y = std::max(0, c.y - d); y <= std::min(frame.h - tH, c.y + d); ++y) {
        for (int x = std::max(0, c.x - d); x <= std::min(frame.w - tW, c.x + d); ++x) {
            float s;
            if (gridMode) {
                for (int gy = 0; gy < gH; ++gy)
                    for (int gx = 0; gx < gW; ++gx)
                        rGrid[size_t(gy) * gW + gx] = frameGray(frame, x + gx * tW / gW, y + gy * tH / gH);
                s = grayNcc(tGrid.data(), rGrid.data(), n);
            } else {
                for (int yy = 0; yy < tH; ++yy)
                    for (int xx = 0; xx < tW; ++xx)
                        rGrid[size_t(yy) * tW + xx] = frameGray(frame, x + xx, y + yy);
                s = grayNcc(t.data(), rGrid.data(), n);
            }
            if (s > c.score) c.score = s, c.x = x, c.y = y;
        }
    }
}

ImageMatch candToMatch(const FrameView& frame, const Cand& c, const ImageRef& ref) {
    ImageMatch m;
    m.score = std::max(0.0f, c.score);
    if (m.score >= ref.thresholdPct / 100.0f) {
        m.found = true;
        m.rect = Rect{c.x / float(frame.w), c.y / float(frame.h), c.tw / float(frame.w), c.th / float(frame.h)};
        m.center = {m.rect.x + m.rect.w * 0.5f, m.rect.y + m.rect.h * 0.5f};
    }
    return m;
}

// Non-max suppression on refined candidates: two detections whose centers are
// closer than half the smaller template are the same object.
void nms(std::vector<Cand>& cs) {
    std::vector<Cand> out;
    for (const Cand& c : cs) {
        const int sep = std::max(4, std::min(c.tw, c.th) / 2);
        bool dup = false;
        for (const Cand& o : out)
            if (std::abs(o.x - c.x) <= sep && std::abs(o.y - c.y) <= sep) {
                dup = true;
                break;
            }
        if (!dup) out.push_back(c);
    }
    cs = std::move(out);
}

}  // namespace

float grayNcc(const float* t, const float* c, int n) {
    double mt = 0, mc = 0;
    for (int i = 0; i < n; ++i) mt += t[i], mc += c[i];
    mt /= n, mc /= n;
    double cov = 0, vt = 0, vc = 0;
    for (int i = 0; i < n; ++i) {
        const double a = t[i] - mt, b = c[i] - mc;
        cov += a * b, vt += a * a, vc += b * b;
    }
    const double d = std::sqrt(vt) * std::sqrt(vc);
    return d < 1e-6 ? 0.0f : float(cov / d);
}

ImageMatch matchImage(const FrameView& frame, const ImageRef& ref) {
    if (!frame.rgb || frame.w <= 0 || frame.h <= 0) return {};
    const int x0 = std::clamp(int(std::lround(ref.region.x * frame.w)), 0, frame.w - 1);
    const int y0 = std::clamp(int(std::lround(ref.region.y * frame.h)), 0, frame.h - 1);
    const int rw = std::min(frame.w - x0, std::max(1, int(std::lround(ref.region.w * frame.w))));
    const int rh = std::min(frame.h - y0, std::max(1, int(std::lround(ref.region.h * frame.h))));
    return matchImageInRect(frame, ref, x0, y0, rw, rh);
}

std::vector<ImageMatch> matchImageAll(const FrameView& frame, const ImageRef& ref, int maxMatches) {
    if (!frame.rgb || frame.w <= 0 || frame.h <= 0) return {};
    const int x0 = std::clamp(int(std::lround(ref.region.x * frame.w)), 0, frame.w - 1);
    const int y0 = std::clamp(int(std::lround(ref.region.y * frame.h)), 0, frame.h - 1);
    const int rw = std::min(frame.w - x0, std::max(1, int(std::lround(ref.region.w * frame.w))));
    const int rh = std::min(frame.h - y0, std::max(1, int(std::lround(ref.region.h * frame.h))));
    return matchImageInRectAll(frame, ref, x0, y0, rw, rh, maxMatches);
}

ImageMatch toFrameCoords(const ImageMatch& m, int x, int y, int w, int h, int fw, int fh) {
    if (!m.found || w <= 0 || h <= 0 || fw <= 0 || fh <= 0) return m;
    ImageMatch out = m;
    out.rect = Rect{(x + m.rect.x * w) / float(fw), (y + m.rect.y * h) / float(fh), m.rect.w * w / float(fw),
                    m.rect.h * h / float(fh)};
    out.center = {out.rect.x + out.rect.w * 0.5f, out.rect.y + out.rect.h * 0.5f};
    return out;
}

ImageMatch matchImageInRect(const FrameView& frame, const ImageRef& ref, int sx, int sy, int sw, int sh) {
    ImageMatch m;
    if (!frame.rgb || frame.w <= 0 || frame.h <= 0 || ref.w <= 0 || ref.h <= 0 ||
        size_t(ref.w) * ref.h * 3 != ref.rgb.size())
        return m;

    const int x0 = std::clamp(sx, 0, frame.w - 1);
    const int y0 = std::clamp(sy, 0, frame.h - 1);
    const int rw = std::min(frame.w - x0, std::max(1, sw));
    const int rh = std::min(frame.h - y0, std::max(1, sh));

    std::vector<Cand> cs = coarseScan(frame, ref, x0, y0, rw, rh, 1);
    if (cs.empty()) return m;
    Cand& c = cs.front();
    fineTune(frame, ref, c);
    return candToMatch(frame, c, ref);
}

std::vector<ImageMatch> matchImageInRectAll(const FrameView& frame, const ImageRef& ref, int sx, int sy, int sw, int sh,
                                            int maxMatches) {
    std::vector<ImageMatch> out;
    if (!frame.rgb || frame.w <= 0 || frame.h <= 0 || ref.w <= 0 || ref.h <= 0 ||
        size_t(ref.w) * ref.h * 3 != ref.rgb.size() || maxMatches <= 0)
        return out;

    const int x0 = std::clamp(sx, 0, frame.w - 1);
    const int y0 = std::clamp(sy, 0, frame.h - 1);
    const int rw = std::min(frame.w - x0, std::max(1, sw));
    const int rh = std::min(frame.h - y0, std::max(1, sh));

    std::vector<Cand> cs = coarseScan(frame, ref, x0, y0, rw, rh, std::max(4, maxMatches * 3));
    for (Cand& c : cs) fineTune(frame, ref, c);
    std::sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    nms(cs);
    // Candidates are returned best-first regardless of the threshold; each carries
    // its own `found` flag so callers can also read the best sub-threshold score.
    for (const Cand& c : cs) {
        out.push_back(candToMatch(frame, c, ref));
        if (int(out.size()) >= maxMatches) break;
    }
    return out;
}

}  // namespace wasd
