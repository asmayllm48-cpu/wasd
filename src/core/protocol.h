// Wire format of the pinned scrcpy-server (v5.0.1). The protocol is internal to
// scrcpy and changes between versions, so the server version is pinned and
// verified by SHA-256 before use (see docs/architecture-decisions.md).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace wasd::proto {

inline constexpr const char* kServerVersion = "5.0.1";
inline constexpr std::size_t kDeviceNameLength = 64;
inline constexpr std::size_t kHeaderSize = 12;

inline constexpr uint32_t kCodecH264 = 0x68323634;
inline constexpr uint32_t kCodecH265 = 0x68323635;
inline constexpr uint32_t kCodecAv1 = 0x00617631;
inline constexpr uint32_t kStreamDisabled = 0;
inline constexpr uint32_t kStreamError = 1;

inline constexpr uint64_t kPointerIdMouse = ~uint64_t{0};
inline constexpr uint64_t kPointerIdGenericFinger = ~uint64_t{0} - 1;

enum class MotionAction : uint8_t { Down = 0, Up = 1, Move = 2, Cancel = 3 };
enum class KeyAction : uint8_t { Down = 0, Up = 1 };

enum class MsgType : uint8_t {
    InjectKeycode = 0,
    InjectText = 1,
    InjectTouch = 2,
    InjectScroll = 3,
    BackOrScreenOn = 4,
    SetDisplayPower = 10,
    ResetVideo = 17,
};

namespace keycode {
inline constexpr uint32_t Home = 3;
inline constexpr uint32_t Back = 4;
inline constexpr uint32_t AppSwitch = 187;
inline constexpr uint32_t VolumeUp = 24;
inline constexpr uint32_t VolumeDown = 25;
}  // namespace keycode

struct SessionHeader {
    uint32_t width = 0;
    uint32_t height = 0;
    bool clientResized = false;
};

struct MediaHeader {
    uint64_t pts = 0;
    uint32_t size = 0;
    bool config = false;
    bool keyFrame = false;
};

struct StreamHeader {
    bool isSession = false;
    SessionHeader session;
    MediaHeader media;
};

uint16_t readU16(const uint8_t* p);
uint32_t readU32(const uint8_t* p);
uint64_t readU64(const uint8_t* p);
void writeU16(uint8_t* p, uint16_t v);
void writeU32(uint8_t* p, uint32_t v);
void writeU64(uint8_t* p, uint64_t v);

// Parses one 12-byte header from the video socket.
StreamHeader parseHeader(std::span<const uint8_t, kHeaderSize> bytes);

// Encodes a value in [0, 1] as the u16 fixed point used for pressure.
uint16_t floatToU16Fixed(float v);

struct TouchEvent {
    MotionAction action = MotionAction::Move;
    uint64_t pointerId = 0;
    int32_t x = 0;
    int32_t y = 0;
    uint16_t frameWidth = 0;   // must equal the current video size, else the server ignores the event
    uint16_t frameHeight = 0;
    float pressure = 0.0f;  // WASD+ parity: synthetic touches report Prs 0.0
};

inline constexpr std::size_t kTouchMsgSize = 32;
inline constexpr std::size_t kKeycodeMsgSize = 14;
inline constexpr std::size_t kMaxMsgSize = 32;

using MsgBuffer = std::array<uint8_t, kMaxMsgSize>;

std::size_t serializeTouch(const TouchEvent& ev, uint8_t* out);
std::size_t serializeKeycode(KeyAction action, uint32_t keycode, uint32_t repeat, uint32_t metaState, uint8_t* out);
std::size_t serializeDisplayPower(bool on, uint8_t* out);
std::size_t serializeResetVideo(uint8_t* out);

}  // namespace wasd::proto
