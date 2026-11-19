#include <doctest/doctest.h>

#include "core/editor.h"

using namespace wasd;

TEST_CASE("key capture assigns a plain key once and ignores auto-repeat") {
    KeyCapture c;
    int vk = 0;
    CHECK(c.onKey('F', true, false, vk) == KeyCapture::Result::None);  // inactive
    c.begin();
    CHECK(c.onKey('F', true, true, vk) == KeyCapture::Result::None);  // repeat
    CHECK(c.onKey('F', true, false, vk) == KeyCapture::Result::Assigned);
    CHECK(vk == 'F');
    CHECK_FALSE(c.active());
    CHECK(c.onKey('F', true, false, vk) == KeyCapture::Result::None);
}

TEST_CASE("key capture cancels on Esc and skips reserved keys") {
    KeyCapture c;
    int vk = 0;
    c.begin();
    CHECK(c.onKey(0x70, true, false, vk) == KeyCapture::Result::None);  // F1
    CHECK(c.onKey(0x5B, true, false, vk) == KeyCapture::Result::None);  // Win
    CHECK(c.active());
    CHECK(c.onKey(0x1B, true, false, vk) == KeyCapture::Result::Cancelled);
    CHECK_FALSE(c.active());
    c.begin();
    CHECK(c.onKey(0x14, true, false, vk) == KeyCapture::Result::Assigned);  // CapsLock is allowed
    CHECK(vk == 0x14);
}

TEST_CASE("modifier alone binds on release, modifier + key binds the key") {
    KeyCapture c;
    int vk = 0;
    c.begin();
    CHECK(c.onKey(0x10, true, false, vk) == KeyCapture::Result::None);
    CHECK(c.onKey(0x10, false, false, vk) == KeyCapture::Result::Assigned);
    CHECK(vk == 0x10);
    c.begin();
    CHECK(c.onKey(0x11, true, false, vk) == KeyCapture::Result::None);
    CHECK(c.onKey('Q', true, false, vk) == KeyCapture::Result::Assigned);
    CHECK(vk == 'Q');
    CHECK(c.onKey(0x11, false, false, vk) == KeyCapture::Result::None);  // release after capture ended
}

TEST_CASE("key capture accepts mouse buttons") {
    KeyCapture c;
    int vk = 0;
    c.begin();
    CHECK(c.onMouseButton(0x05, vk) == KeyCapture::Result::Assigned);
    CHECK(vk == 0x05);
}

TEST_CASE("edit history undo/redo restores snapshots") {
    Profile a = makeDefaultProfile();
    EditHistory h;
    h.reset(a);
    CHECK_FALSE(h.commit(a));
    Profile b = a;
    b.bindings[0].pos.x = 0.3f;
    CHECK(h.commit(b));
    Profile c = b;
    c.bindings.pop_back();
    CHECK(h.commit(c));
    Profile out;
    REQUIRE(h.undo(out));
    CHECK(out == b);
    REQUIRE(h.undo(out));
    CHECK(out == a);
    CHECK_FALSE(h.canUndo());
    REQUIRE(h.redo(out));
    CHECK(out == b);
    Profile d = b;
    d.name = "x";
    CHECK(h.commit(d));
    CHECK_FALSE(h.canRedo());
}

TEST_CASE("edit history is bounded") {
    Profile p = makeDefaultProfile();
    EditHistory h(3);
    h.reset(p);
    for (int i = 0; i < 10; ++i) {
        p.bindings[0].pos.x = 0.05f * i;
        h.commit(p);
    }
    Profile out;
    int n = 0;
    while (h.undo(out)) ++n;
    CHECK(n == 3);
}

TEST_CASE("profile names cannot escape the profiles folder") {
    CHECK(isValidProfileName("PUBG Mobile"));
    CHECK(isValidProfileName("ببجي"));
    CHECK_FALSE(isValidProfileName(""));
    CHECK_FALSE(isValidProfileName("..\\evil"));
    CHECK_FALSE(isValidProfileName("a/b"));
    CHECK_FALSE(isValidProfileName("con?"));
    CHECK_FALSE(isValidProfileName(" lead"));
}
