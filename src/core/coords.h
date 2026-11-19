// Single source of truth for coordinate conversion between:
//   window client pixels -> letterboxed video viewport -> device frame pixels -> normalized [0,1].
#pragma once
#include <cstdint>
#include <optional>

namespace wasd {

struct Vec2 {
    float x = 0;
    float y = 0;
    bool operator==(const Vec2&) const = default;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    bool operator==(const Rect&) const = default;
};

// Largest rect with the frame's aspect ratio centered inside the client area.
Rect fitViewport(float clientW, float clientH, uint32_t frameW, uint32_t frameH);

class CoordinateMapper {
public:
    void update(float clientW, float clientH, uint32_t frameW, uint32_t frameH);

    bool valid() const { return frameW_ > 0 && frameH_ > 0 && viewport_.w > 0 && viewport_.h > 0; }
    const Rect& viewport() const { return viewport_; }
    uint32_t frameWidth() const { return frameW_; }
    uint32_t frameHeight() const { return frameH_; }

    // Window pixel -> normalized; nullopt when outside the video viewport.
    std::optional<Vec2> windowToNormalized(Vec2 window) const;
    Vec2 normalizedToWindow(Vec2 n) const;
    // Normalized -> device frame pixel (clamped to the frame).
    void normalizedToFrame(Vec2 n, int32_t& outX, int32_t& outY) const;

private:
    Rect viewport_{};
    uint32_t frameW_ = 0;
    uint32_t frameH_ = 0;
};

}  // namespace wasd
