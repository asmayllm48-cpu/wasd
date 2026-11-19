#include <doctest/doctest.h>

#include <string>

#include "app/text_shaping.h"

using namespace wasd::app;

static unsigned firstCodepoint(const std::string& s, size_t at) {
    const unsigned char c = static_cast<unsigned char>(s[at]);
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) return ((c & 0x1Fu) << 6) | (s[at + 1] & 0x3Fu);
    return ((c & 0x0Fu) << 12) | ((s[at + 1] & 0x3Fu) << 6) | (s[at + 2] & 0x3Fu);
}

TEST_CASE("LTR text and shortcuts are returned unchanged") {
    CHECK(text::visual("Ctrl + Alt + Q") == "Ctrl + Alt + Q");
    CHECK(text::visual("Mouse X1") == "Mouse X1");
    CHECK_FALSE(text::hasRtl("Ctrl + Alt + Q"));
    CHECK(text::hasRtl("حساسية"));
}

TEST_CASE("Arabic is shaped into font glyphs, English runs keep their order") {
    const std::string v = text::visual("حساسية الماوس Mouse Sensitivity");
    // RTL paragraph: the English run is visually leftmost, the Arabic glyphs follow.
    REQUIRE(v.rfind("Mouse Sensitivity", 0) == 0);
    const unsigned cp = firstCodepoint(v, std::string("Mouse Sensitivity").size());
    CHECK(cp >= text::kSlotFirst);
    CHECK(cp <= text::kSlotLast);
    CHECK(text::glyphForSlot(cp) > 0);
}

TEST_CASE("shortcuts embedded in Arabic keep left-to-right order") {
    const std::string v = text::visual("اضغط Ctrl + Alt + Q للحفظ");
    CHECK(v.find("Ctrl + Alt + Q") != std::string::npos);
}

TEST_CASE("contextual forms differ: isolated and joined letters map to different glyphs") {
    const std::string a = text::visual("ب");
    const std::string b = text::visual("ببب");
    // A joined run uses initial/medial/final forms, so it does not repeat the isolated glyph three times.
    CHECK(b != a + a + a);
}
