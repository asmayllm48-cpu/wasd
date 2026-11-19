#include "core/protocol.h"

#include <algorithm>
#include <cmath>

namespace wasd::proto {

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint32_t readU32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

uint64_t readU64(const uint8_t* p) { return (uint64_t{readU32(p)} << 32) | readU32(p + 4); }

void writeU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void writeU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void writeU64(uint8_t* p, uint64_t v) {
    writeU32(p, static_cast<uint32_t>(v >> 32));
    writeU32(p + 4, static_cast<uint32_t>(v));
}

StreamHeader parseHeader(std::span<const uint8_t, kHeaderSize> b) {
    StreamHeader h;
    if (b[0] & 0x80) {
        h.isSession = true;
        h.session.clientResized = (b[3] & 1) != 0;
        h.session.width = readU32(&b[4]);
        h.session.height = readU32(&b[8]);
        return h;
    }
    const uint64_t ptsAndFlags = readU64(&b[0]);
    constexpr uint64_t kConfig = uint64_t{1} << 62;
    constexpr uint64_t kKey = uint64_t{1} << 61;
    h.media.config = (ptsAndFlags & kConfig) != 0;
    h.media.keyFrame = (ptsAndFlags & kKey) != 0;
    h.media.pts = ptsAndFlags & (kKey - 1);
    h.media.size = readU32(&b[8]);
    return h;
}

uint16_t floatToU16Fixed(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    const uint32_t u = static_cast<uint32_t>(std::lround(v * 65536.0f));
    return static_cast<uint16_t>(std::min<uint32_t>(u, 0xFFFF));
}

std::size_t serializeTouch(const TouchEvent& ev, uint8_t* o) {
    o[0] = static_cast<uint8_t>(MsgType::InjectTouch);
    o[1] = static_cast<uint8_t>(ev.action);
    writeU64(&o[2], ev.pointerId);
    writeU32(&o[10], static_cast<uint32_t>(ev.x));
    writeU32(&o[14], static_cast<uint32_t>(ev.y));
    writeU16(&o[18], ev.frameWidth);
    writeU16(&o[20], ev.frameHeight);
    writeU16(&o[22], ev.action == MotionAction::Up ? 0 : floatToU16Fixed(ev.pressure));
    writeU32(&o[24], 0);  // action button
    writeU32(&o[28], 0);  // buttons (must be 0 for finger events)
    return kTouchMsgSize;
}

std::size_t serializeKeycode(KeyAction action, uint32_t keycode, uint32_t repeat, uint32_t metaState, uint8_t* o) {
    o[0] = static_cast<uint8_t>(MsgType::InjectKeycode);
    o[1] = static_cast<uint8_t>(action);
    writeU32(&o[2], keycode);
    writeU32(&o[6], repeat);
    writeU32(&o[10], metaState);
    return kKeycodeMsgSize;
}

std::size_t serializeDisplayPower(bool on, uint8_t* o) {
    o[0] = static_cast<uint8_t>(MsgType::SetDisplayPower);
    o[1] = on ? 1 : 0;
    return 2;
}

std::size_t serializeResetVideo(uint8_t* o) {
    o[0] = static_cast<uint8_t>(MsgType::ResetVideo);
    return 1;
}

}  // namespace wasd::proto
