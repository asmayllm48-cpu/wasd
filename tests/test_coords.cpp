#include <doctest/doctest.h>

#include <cstdlib>

#include "core/coords.h"

using namespace wasd;

TEST_CASE("landscape frame letterboxed in wide window") {
    CoordinateMapper m;
    m.update(1920, 1080, 2400, 1080);  // 20:9 frame in 16:9 window -> bars top/bottom
    REQUIRE(m.valid());
    CHECK(m.viewport().x == doctest::Approx(0));
    CHECK(m.viewport().w == doctest::Approx(1920));
    CHECK(m.viewport().h == doctest::Approx(864));
    CHECK(m.viewport().y == doctest::Approx(108));
    CHECK_FALSE(m.windowToNormalized({10, 50}).has_value());
    auto n = m.windowToNormalized({960, 540});
    REQUIRE(n);
    CHECK(n->x == doctest::Approx(0.5));
    CHECK(n->y == doctest::Approx(0.5));
}

TEST_CASE("portrait frame pillarboxed") {
    CoordinateMapper m;
    m.update(1280, 720, 1080, 2400);
    CHECK(m.viewport().h == doctest::Approx(720));
    CHECK(m.viewport().w == doctest::Approx(324));
    CHECK(m.viewport().x == doctest::Approx(478));
}

TEST_CASE("round trip window -> normalized -> frame is within one pixel") {
    CoordinateMapper m;
    struct Case {
        float cw, ch;
        uint32_t fw, fh;
    };
    const Case cases[] = {{1280.f, 720.f, 2400u, 1080u}, {800.f, 900.f, 1080u, 2400u}, {1919.f, 1201.f, 1600u, 720u}};
    for (const auto& [cw, ch, fw, fh] : cases) {
        m.update(cw, ch, fw, fh);
        for (float fx : {0.f, 0.25f, 0.5f, 0.999f}) {
            for (float fy : {0.f, 0.33f, 0.75f, 0.999f}) {
                Vec2 w = m.normalizedToWindow({fx, fy});
                auto n = m.windowToNormalized(w);
                REQUIRE(n);
                int32_t x, y;
                m.normalizedToFrame(*n, x, y);
                CHECK(std::abs(x - static_cast<int32_t>(fx * fw)) <= 1);
                CHECK(std::abs(y - static_cast<int32_t>(fy * fh)) <= 1);
            }
        }
    }
}

TEST_CASE("frame coordinates are clamped") {
    CoordinateMapper m;
    m.update(100, 100, 1000, 500);
    int32_t x, y;
    m.normalizedToFrame({1.5f, -1.0f}, x, y);
    CHECK(x == 999);
    CHECK(y == 0);
}

TEST_CASE("invalid sizes produce invalid mapper") {
    CoordinateMapper m;
    m.update(0, 100, 1000, 500);
    CHECK_FALSE(m.valid());
    CHECK_FALSE(m.windowToNormalized({1, 1}).has_value());
}
