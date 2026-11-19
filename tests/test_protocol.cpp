#include <doctest/doctest.h>

#include "core/protocol.h"

using namespace wasd::proto;

TEST_CASE("session header parsing") {
    std::array<uint8_t, 12> b{0x80, 0, 0, 1, 0, 0, 0x04, 0x38, 0, 0, 0x07, 0x80};
    auto h = parseHeader(b);
    CHECK(h.isSession);
    CHECK(h.session.clientResized);
    CHECK(h.session.width == 1080);
    CHECK(h.session.height == 1920);
}

TEST_CASE("media header flags and pts") {
    std::array<uint8_t, 12> b{};
    writeU64(b.data(), (uint64_t{1} << 61) | 123456);
    writeU32(b.data() + 8, 4096);
    auto h = parseHeader(b);
    CHECK_FALSE(h.isSession);
    CHECK(h.media.keyFrame);
    CHECK_FALSE(h.media.config);
    CHECK(h.media.pts == 123456);
    CHECK(h.media.size == 4096);

    writeU64(b.data(), uint64_t{1} << 62);
    h = parseHeader(b);
    CHECK(h.media.config);
    CHECK(h.media.pts == 0);
}

TEST_CASE("touch serialization matches scrcpy 5.0.1 layout") {
    TouchEvent ev;
    ev.action = MotionAction::Down;
    ev.pointerId = 0x1234567887654321ULL;
    ev.x = 100;
    ev.y = 200;
    ev.frameWidth = 1080;
    ev.frameHeight = 1920;
    ev.pressure = 1.0f;
    uint8_t buf[32];
    REQUIRE(serializeTouch(ev, buf) == 32);
    CHECK(buf[0] == 2);
    CHECK(buf[1] == 0);
    CHECK(readU64(buf + 2) == 0x1234567887654321ULL);
    CHECK(readU32(buf + 10) == 100);
    CHECK(readU32(buf + 14) == 200);
    CHECK(readU16(buf + 18) == 1080);
    CHECK(readU16(buf + 20) == 1920);
    CHECK(readU16(buf + 22) == 0xFFFF);
    CHECK(readU32(buf + 24) == 0);
    CHECK(readU32(buf + 28) == 0);

    ev.action = MotionAction::Up;
    serializeTouch(ev, buf);
    CHECK(readU16(buf + 22) == 0);

    // A held touch may legitimately report pressure 0 (WASD+ style): the event
    // is still Down/Move — only the pressure field reads zero on the device.
    ev.action = MotionAction::Move;
    ev.pressure = 0.0f;
    serializeTouch(ev, buf);
    CHECK(buf[1] == 2);
    CHECK(readU16(buf + 22) == 0);
}

TEST_CASE("keycode, display power and reset video messages") {
    uint8_t buf[32];
    REQUIRE(serializeKeycode(KeyAction::Up, keycode::Back, 0, 0, buf) == 14);
    CHECK(buf[0] == 0);
    CHECK(buf[1] == 1);
    CHECK(readU32(buf + 2) == 4);
    REQUIRE(serializeDisplayPower(false, buf) == 2);
    CHECK(buf[0] == 10);
    CHECK(buf[1] == 0);
    REQUIRE(serializeResetVideo(buf) == 1);
    CHECK(buf[0] == 17);
}

TEST_CASE("pressure fixed point") {
    CHECK(floatToU16Fixed(0.0f) == 0);
    CHECK(floatToU16Fixed(0.5f) == 0x8000);
    CHECK(floatToU16Fixed(1.0f) == 0xFFFF);
    CHECK(floatToU16Fixed(2.0f) == 0xFFFF);
}
