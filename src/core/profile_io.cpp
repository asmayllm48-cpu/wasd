#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/profile.h"

namespace wasd {
namespace fs = std::filesystem;

bool saveProfileFile(const std::string& path, const Profile& p, std::string& err) {
    auto problems = validateProfile(p);
    if (!problems.empty()) {
        err = problems.front();
        return false;
    }
    std::error_code ec;
    const fs::path target = fs::u8path(path);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot write " + tmp.string();
            return false;
        }
        f << profileToJson(p);
        if (!f) {
            err = "write failed: " + tmp.string();
            return false;
        }
    }
    if (fs::exists(target, ec)) {
        fs::path bak = target;
        bak += ".bak";
        fs::copy_file(target, bak, fs::copy_options::overwrite_existing, ec);
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        err = "rename failed: " + ec.message();
        return false;
    }
    return true;
}

bool loadProfileFile(const std::string& path, Profile& out, std::string& err) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return profileFromJson(ss.str(), out, err);
}

std::string keyName(int vk) {
    switch (vk) {
        case 0x01: return "Mouse L";
        case 0x02: return "Mouse R";
        case 0x04: return "Mouse M";
        case 0x05: return "Mouse X1";
        case 0x06: return "Mouse X2";
        case 0x08: return "Backspace";
        case 0x09: return "Tab";
        case 0x0D: return "Enter";
        case 0x10: return "Shift";
        case 0x11: return "Ctrl";
        case 0x12: return "Alt";
        case 0x14: return "CapsLock";
        case 0x1B: return "Esc";
        case 0x20: return "Space";
        case 0x25: return "Left";
        case 0x26: return "Up";
        case 0x27: return "Right";
        case 0x28: return "Down";
        case 0xA0: return "LShift";
        case 0xA1: return "RShift";
        case 0xA2: return "LCtrl";
        case 0xA3: return "RCtrl";
        case 0xC0: return "`";
        case kVkWheelUp: return "Wheel Up";
        case kVkWheelDown: return "Wheel Down";
        case kVkWheelLeft: return "Wheel Left";
        case kVkWheelRight: return "Wheel Right";
        default: break;
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, static_cast<char>(vk));
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x6F);
    if (vk >= 0x60 && vk <= 0x69) return "Num" + std::to_string(vk - 0x60);
    char buf[16];
    std::snprintf(buf, sizeof buf, "VK 0x%02X", vk);
    return buf;
}

}  // namespace wasd
