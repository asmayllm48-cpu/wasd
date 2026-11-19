#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>

#include "core/profile.h"

using namespace wasd;

TEST_CASE("default profile is valid and round-trips through JSON") {
    Profile p = makeDefaultProfile();
    CHECK(validateProfile(p).empty());
    Profile q;
    std::string err;
    REQUIRE_MESSAGE(profileFromJson(profileToJson(p), q, err), err);
    REQUIRE(q.bindings.size() == p.bindings.size());
    for (std::size_t i = 0; i < p.bindings.size(); ++i) {
        CHECK(q.bindings[i].type == p.bindings[i].type);
        CHECK(q.bindings[i].key == p.bindings[i].key);
        CHECK(q.bindings[i].pos.x == doctest::Approx(p.bindings[i].pos.x));
    }
}

TEST_CASE("duplicate keys and bad positions are rejected") {
    Profile p = makeDefaultProfile();
    p.bindings[3].key = 'W';  // collides with joystick
    CHECK_FALSE(validateProfile(p).empty());
    p = makeDefaultProfile();
    p.bindings[2].pos = {1.5f, 0.2f};
    CHECK_FALSE(validateProfile(p).empty());
}

TEST_CASE("v0 profile migrates keys -> bindings") {
    const char* v0 = R"({"name":"old","keys":[{"type":"tap","key":32,"pos":[0.5,0.5]}]})";
    Profile p;
    std::string err;
    REQUIRE_MESSAGE(profileFromJson(v0, p, err), err);
    CHECK(p.bindings.size() == 1);
    CHECK(p.schemaVersion == kProfileSchemaVersion);
}

TEST_CASE("newer schema and malformed JSON fail with a message") {
    Profile p;
    std::string err;
    CHECK_FALSE(profileFromJson(R"({"schemaVersion":99,"name":"x"})", p, err));
    CHECK(err.find("newer") != std::string::npos);
    CHECK_FALSE(profileFromJson("{not json", p, err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(profileFromJson(R"({"schemaVersion":1,"name":"x","bindings":[{"type":"laser","pos":[0,0]}]})", p, err));
}

TEST_CASE("save keeps a backup and load restores") {
    auto dir = std::filesystem::temp_directory_path() / "wasd_test_profiles";
    std::filesystem::remove_all(dir);
    const std::string path = (dir / "game.json").string();
    std::string err;
    Profile p = makeDefaultProfile();
    REQUIRE_MESSAGE(saveProfileFile(path, p, err), err);
    p.name = "Second";
    REQUIRE_MESSAGE(saveProfileFile(path, p, err), err);
    CHECK(std::filesystem::exists(path + ".bak"));
    Profile q;
    REQUIRE_MESSAGE(loadProfileFile(path, q, err), err);
    CHECK(q.name == "Second");
    Profile bak;
    REQUIRE(loadProfileFile(path + ".bak", bak, err));
    CHECK(bak.name == "Default");
    std::filesystem::remove_all(dir);
}

TEST_CASE("Smart Fire binding round-trips through JSON and validates recoil bounds") {
    Profile p = makeDefaultProfile();
    Binding sf;
    sf.name = "Rifle Fire";
    sf.type = BindingType::Fire;
    sf.key = 0x05;  // Mouse X1
    sf.pos = {0.82f, 0.68f};
    sf.recoilEnabled = true;
    sf.recoilDown = 35;
    sf.recoilUp = 2;
    sf.recoilLeft = 4;
    sf.recoilRight = 7;
    sf.recoilDelayMs = 45;
    sf.recoilToggleKey = 0x74;  // F5
    sf.rapidFire = true;
    sf.rapidFireDelayMs = 80;
    p.bindings.push_back(sf);

    CHECK(validateProfile(p).empty());
    Profile out;
    std::string err;
    REQUIRE_MESSAGE(profileFromJson(profileToJson(p), out, err), err);
    const Binding& got = out.bindings.back();
    CHECK(got.type == BindingType::Fire);
    CHECK(got.recoilEnabled == true);
    CHECK(got.recoilDown == 35);
    CHECK(got.recoilUp == 2);
    CHECK(got.recoilLeft == 4);
    CHECK(got.recoilRight == 7);
    CHECK(got.recoilDelayMs == 45);
    CHECK(got.recoilToggleKey == 0x74);
    CHECK(got.rapidFire == true);
    CHECK(got.rapidFireDelayMs == 80);
}

TEST_CASE("Camera advanced settings round-trip through JSON") {
    Profile p = makeDefaultProfile();
    Binding* camptr = nullptr;
    for (Binding& x : p.bindings)
        if (x.type == BindingType::Camera) camptr = &x;
    REQUIRE(camptr != nullptr);
    Binding& cam = *camptr;
    cam.pos = {0.709f, 0.240f};
    cam.sensitivity = 0.9f;
    cam.sensitivityY = 0.75f;
    cam.camBoundsEnabled = true;
    cam.camBoundsRange = 0.375f;
    cam.camReturnType = 2;
    cam.camReturnDelayMs = 300;
    cam.camAverageMs = 12;
    cam.camAutoFire = true;
    cam.camAutoFireReleaseMs = 60;
    cam.comment = "my aim";

    CHECK(validateProfile(p).empty());
    Profile out;
    std::string err;
    REQUIRE_MESSAGE(profileFromJson(profileToJson(p), out, err), err);
    const Binding* gp = nullptr;
    for (const Binding& x : out.bindings)
        if (x.type == BindingType::Camera) gp = &x;
    REQUIRE(gp != nullptr);
    const Binding& g = *gp;
    CHECK(g.pos.x == doctest::Approx(0.709f));
    CHECK(g.sensitivity == doctest::Approx(0.9f));
    CHECK(g.sensitivityY == doctest::Approx(0.75f));
    CHECK(g.camBoundsEnabled);
    CHECK(g.camBoundsRange == doctest::Approx(0.375f));
    CHECK(g.camReturnType == 2);
    CHECK(g.camReturnDelayMs == 300);
    CHECK(g.camAverageMs == 12);
    CHECK(g.camAutoFire);
    CHECK(g.camAutoFireReleaseMs == 60);
    CHECK(g.comment == "my aim");
}

TEST_CASE("old camera profile without new keys loads with safe defaults") {
    // Simulates a profile written before the advanced mouse settings existed.
    const std::string oldJson = R"({"schemaVersion":1,"name":"old","landscape":true,
        "bindings":[{"name":"Aim","type":"camera","pos":[0.7,0.45],"radius":0.25,"sensitivity":1.4,"invertY":true}]})";
    Profile out;
    std::string err;
    REQUIRE_MESSAGE(profileFromJson(oldJson, out, err), err);
    const Binding& g = out.bindings.front();
    CHECK(g.sensitivity == doctest::Approx(1.4f));
    CHECK(g.sensitivityY == doctest::Approx(1.4f));  // Y inherits legacy X value
    CHECK_FALSE(g.camBoundsEnabled);
    CHECK(g.camReturnType == 0);
    CHECK(g.camAverageMs == 0);
    CHECK_FALSE(g.camAutoFire);
    CHECK(g.comment.empty());
}

TEST_CASE("Natural movement joystick profile round-trips through JSON") {
    Profile p = makeDefaultProfile();
    Binding* joy = nullptr;
    for (Binding& x : p.bindings)
        if (x.type == BindingType::Joystick) joy = &x;
    REQUIRE(joy != nullptr);
    joy->joyProfile = 2;
    joy->joyHumanize = false;
    joy->joyNatInitialMs = 20;
    joy->joyNatSegMs = 9;
    joy->joyNatDurMs = 140;
    joy->joyNatSpacingPx = 8.5f;
    joy->joyNatTimeVarPct = 30;
    joy->joyNatPathVarPx = 2.5f;
    joy->joyNatEase = 2;

    CHECK(validateProfile(p).empty());
    Profile out;
    std::string err;
    REQUIRE_MESSAGE(profileFromJson(profileToJson(p), out, err), err);
    const Binding* g = nullptr;
    for (const Binding& x : out.bindings)
        if (x.type == BindingType::Joystick) g = &x;
    REQUIRE(g != nullptr);
    CHECK(g->joyProfile == 2);
    CHECK_FALSE(g->joyHumanize);
    CHECK(g->joyNatInitialMs == 20);
    CHECK(g->joyNatSegMs == 9);
    CHECK(g->joyNatDurMs == 140);
    CHECK(g->joyNatSpacingPx == doctest::Approx(8.5f));
    CHECK(g->joyNatTimeVarPct == 30);
    CHECK(g->joyNatPathVarPx == doctest::Approx(2.5f));
    CHECK(g->joyNatEase == 2);
}

TEST_CASE("Legacy joyHumanize flag maps to smooth-spring profile on load") {
    Profile p = makeDefaultProfile();
    for (Binding& x : p.bindings)
        if (x.type == BindingType::Joystick) x.joyHumanize = true;
    std::string err;
    Profile out;
    REQUIRE_MESSAGE(profileFromJson(profileToJson(p), out, err), err);
    const Binding* g = nullptr;
    for (const Binding& x : out.bindings)
        if (x.type == BindingType::Joystick) g = &x;
    REQUIRE(g != nullptr);
    CHECK(g->joyProfile == 1);   // old humanize flag upgrades to the spring profile
    CHECK(g->joyHumanize);
}
