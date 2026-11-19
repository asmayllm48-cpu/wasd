#include "core/coords.h"

#include <algorithm>
#include <cmath>

namespace wasd {

Rect fitViewport(float clientW, float clientH, uint32_t frameW, uint32_t frameH) {
    if (clientW <= 0 || clientH <= 0 || frameW == 0 || frameH == 0) return {};
    const float frameAspect = static_cast<float>(frameW) / static_cast<float>(frameH);
    float w = clientW;
    float h = w / frameAspect;
    if (h > clientH) {
        h = clientH;
        w = h * frameAspect;
    }
    return Rect{(clientW - w) * 0.5f, (clientH - h) * 0.5f, w, h};
}

void CoordinateMapper::update(float clientW, float clientH, uint32_t frameW, uint32_t frameH) {
    frameW_ = frameW;
    frameH_ = frameH;
    viewport_ = fitViewport(clientW, clientH, frameW, frameH);
}

std::optional<Vec2> CoordinateMapper::windowToNormalized(Vec2 p) const {
    if (!valid() || !viewport_.contains(p)) return std::nullopt;
    return Vec2{(p.x - viewport_.x) / viewport_.w, (p.y - viewport_.y) / viewport_.h};
}

Vec2 CoordinateMapper::normalizedToWindow(Vec2 n) const {
    return Vec2{viewport_.x + n.x * viewport_.w, viewport_.y + n.y * viewport_.h};
}

void CoordinateMapper::normalizedToFrame(Vec2 n, int32_t& outX, int32_t& outY) const {
    const float fx = std::clamp(n.x, 0.0f, 1.0f) * static_cast<float>(frameW_);
    const float fy = std::clamp(n.y, 0.0f, 1.0f) * static_cast<float>(frameH_);
    outX = std::clamp(static_cast<int32_t>(std::floor(fx)), 0, static_cast<int32_t>(frameW_) - 1);
    outY = std::clamp(static_cast<int32_t>(std::floor(fy)), 0, static_cast<int32_t>(frameH_) - 1);
}

}  // namespace wasd
