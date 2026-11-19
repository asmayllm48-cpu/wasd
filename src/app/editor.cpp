// Key-mapping editor actions: direct placement, key capture, undo/redo and profile
// management. Only the in-memory Profile is edited; InputEngine receives it unchanged.
#include <windowsx.h>

#include <algorithm>
#include <filesystem>

#include "app/app.h"
#include "app/i18n.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "platform/paths.h"

namespace wasd::app {
using platform::narrow;
using platform::widen;
using i18n::tr;
using i18n::trf;
namespace fs = std::filesystem;

static std::string profilePathFor(const std::string& name) { return narrow(platform::profilesDir()) + "/" + name + ".json"; }

static const char* typeLabel(BindingType t) {
    switch (t) {
        case BindingType::Tap: return tr("Tap");
        case BindingType::Joystick: return tr("Joystick");
        case BindingType::Camera: return tr("Mouse look");
        case BindingType::Drag: return tr("Swipe");
        case BindingType::Fire: return tr("Smart Fire");
    }
    return "";
}

void App::showToast(const std::string& text) {
    toast_ = text;
    toastUntil_ = nowMicros() + 2500000;
}

void App::setEditMode(bool on) {
    if (on == editMode_) return;
    if (on && !sessionActive()) {
        showToast(tr("Connect a device to edit keys"));
        return;
    }
    if (!on) cancelCapture(), dragBinding_ = -1, dragging_ = false;
    if (!on) selectedBinding_ = -1;
    // Release every finger BEFORE the flag goes up: sendTouch() drops events in
    // edit mode, so releasing after would leave ghosts stuck down on the phone.
    if (on) releaseInput(), setLocked(false);
    editMode_ = on;
}

void App::setLanguage(const std::string& code) {
    i18n::setLanguage(code);
    settings_.language = i18n::language();
    ImGui::GetStyle().SelectableTextAlign = {i18n::rtl() ? 1.0f : 0.0f, 0.5f};
    SetWindowTextW(hwnd_, L"NEXPLAY");
    saveSettings();
}

int* App::captureSlot(const CaptureTarget& t) {
    if (t.slot == 5) return &profile_.lockToggleKey;
    if (t.slot == 6 || t.slot == 7) return capExternal_;
    if (t.binding < 0 || t.binding >= static_cast<int>(profile_.bindings.size())) return nullptr;
    Binding& b = profile_.bindings[t.binding];
    switch (t.slot) {
        case 0: return &b.key;
        case 1: return &b.keyUp;
        case 2: return &b.keyLeft;
        case 3: return &b.keyDown;
        case 4: return &b.keyRight;
        case 8: return &b.recoilToggleKey;
        case 9: return &b.rapidFireToggleKey;
        case 10: return &b.key2;
        case 11: return &b.joyShiftKey;
        default: return nullptr;
    }
}

// Name of the control that already uses vk (ignoring the slot being edited), empty if free.
std::string App::keyOwner(int vk, int binding, int slot) const {
    if (slot == 10 || slot == 11) return {};  // combo second key and Shift modifier can share keys freely
    if (slot != 5 && profile_.lockToggleKey == vk) return tr("Mouse lock key");
    const Automation& au = profile_.automation;
    if (!(au == Automation{})) {
        if (au.toggleKey == vk && !(slot == 7 && capExternal_ == &au.toggleKey)) return tr("Automation toggle key");
        if (au.detect.startKey == vk && !(slot == 7 && capExternal_ == &au.detect.startKey))
            return tr("Detection start key");
        if (au.detect.stopKey == vk && !(slot == 7 && capExternal_ == &au.detect.stopKey))
            return tr("Detection stop key");
        for (const Macro& m : au.macros)
            if (m.key == vk && !(slot == 7 && capExternal_ == &m.key)) return m.name;
        for (const Monitor& m : au.monitors)
            if (m.toggleKey == vk && !(slot == 7 && capExternal_ == &m.toggleKey)) return m.name;
    }
    for (int j = 0; j < static_cast<int>(profile_.bindings.size()); ++j) {
        const Binding& b = profile_.bindings[j];
        if ((b.type == BindingType::Tap || b.type == BindingType::Drag || b.type == BindingType::Fire) && b.key2 != 0)
            continue;  // dual-key combo bindings do not block single keys
        const std::pair<int, int> keys[] = {{0, b.key},     {1, b.keyUp},           {2, b.keyLeft},         {3, b.keyDown},
                                            {4, b.keyRight}, {8, b.recoilToggleKey}, {9, b.rapidFireToggleKey}};
        for (auto [s, k] : keys) {
            const bool relevant = (b.type == BindingType::Tap || b.type == BindingType::Drag) ? s == 0
                                  : b.type == BindingType::Fire                               ? (s == 0 || s == 8 || s == 9)
                                  : b.type == BindingType::Joystick                           ? (s >= 1 && s <= 4)
                                                                                              : false;
            if (!relevant || (j == binding && s == slot) || k <= 0 || k != vk) continue;
            return b.name.empty() ? typeLabel(b.type) : b.name;
        }
    }
    return {};
}

void App::beginCapture(int binding, int slot, bool isNew, bool sequence) {
    if (capTarget_.slot >= 0 && !(capTarget_.binding == binding && capTarget_.isNew)) cancelCapture();
    capTarget_ = {binding, slot, isNew, sequence, 0};
    comboFirstVk_ = 0;
    if (int* p = captureSlot(capTarget_)) capTarget_.previous = *p;
    capture_.begin();
    if (ImGui::GetCurrentContext()) {
        if (ImGui::GetIO().WantTextInput) ImGui::ClearActiveID();
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    }
}

void App::endCapture() {
    capture_.end();
    capTarget_ = {};
    capExternal_ = nullptr;
    capExternal2_ = nullptr;
    comboFirstVk_ = 0;
}

void App::beginExternalCapture(int* p, int slot, int* p2) {
    if (capTarget_.slot >= 0) cancelCapture();
    capExternal_ = p;
    capExternal2_ = p2;
    beginCapture(-1, slot, false);
}

void App::finishCapture(int vk) {
    const CaptureTarget t = capTarget_;
    int* p = captureSlot(t);
    if (!p) return endCapture();
    // External captures (slot 6 step/monitor key, slot 7 macro/toggle trigger) are
    // exempt from the single-key owner check: triggers may share keys with bindings.
    if (const std::string owner = (t.slot == 6 || t.slot == 7) ? std::string() : keyOwner(vk, t.binding, t.slot);
        !owner.empty()) {
        showToast(trf("%s is already used by %s", keyName(vk).c_str(), owner.c_str()));
        capture_.begin();  // keep waiting for another key
        return;
    }
    *p = vk;
    if (t.sequence && t.slot >= 1 && t.slot < 4) {
        capTarget_.slot = t.slot + 1;
        capTarget_.previous = *captureSlot(capTarget_);
        capture_.begin();
        return;
    }
    endCapture();
    showToast(trf("Assigned %s", keyName(vk).c_str()));
}

void App::cancelCapture() {
    const CaptureTarget t = capTarget_;
    endCapture();
    if (t.slot < 0) return;
    if (t.isNew) {
        if (t.binding >= 0 && t.binding < static_cast<int>(profile_.bindings.size()))
            profile_.bindings.erase(profile_.bindings.begin() + t.binding);
        selectedBinding_ = -1;
    } else if (int* p = captureSlot(t)) {
        *p = t.previous;
    }
}

// Feeds Win32 input to an active key capture. Returns true when the message was consumed.
bool App::captureMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (!capture_.active() || (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput)) return false;

    // Dual-key combo support when capturing a binding's primary trigger (slot == 0):
    // Holding key 1 (e.g. H) and pressing key 2 (e.g. T) binds both as one combined key "H + T".
    // Releasing key 1 without pressing a second key binds key 1 alone.
    if (capTarget_.slot == 0 && capTarget_.binding >= 0 && capTarget_.binding < static_cast<int>(profile_.bindings.size())) {
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            const int k = static_cast<int>(wp);
            if (k == VK_F1 || k == VK_F2) return false;
            if (k == 0x1B) {
                cancelCapture();
                return true;
            }
            if ((lp & 0x40000000) != 0) return true;  // ignore auto-repeat while waiting for second key
            if (!KeyCapture::isReserved(k)) {
                if (comboFirstVk_ == 0) {
                    comboFirstVk_ = k;
                    return true;
                } else if (k != comboFirstVk_) {
                    Binding& b = profile_.bindings[capTarget_.binding];
                    b.key = comboFirstVk_;
                    b.key2 = k;
                    const std::string label = keyName(b.key) + " + " + keyName(b.key2);
                    endCapture();
                    showToast(trf("Assigned %s", label.c_str()));
                    return true;
                }
            }
            return true;
        }
        if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
            const int k = static_cast<int>(wp);
            if (comboFirstVk_ != 0 && k == comboFirstVk_) {
                profile_.bindings[capTarget_.binding].key2 = 0;
                finishCapture(comboFirstVk_);
                return true;
            }
        }
    }

    // Combo capture for external two-target slots (e.g. macro trigger key + key2):
    // hold key 1, press key 2 -> both assigned; release key 1 alone -> single key.
    if (capTarget_.slot == 7 && capExternal_ && capExternal2_) {
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            const int k = static_cast<int>(wp);
            if (k == VK_F1 || k == VK_F2) return false;
            if (k == 0x1B) return cancelCapture(), true;
            if ((lp & 0x40000000) != 0) return true;
            if (!KeyCapture::isReserved(k)) {
                if (comboFirstVk_ == 0) {
                    comboFirstVk_ = k;
                    return true;
                }
                if (k != comboFirstVk_) {
                    // A combo PAIR is its own unique binding: bypass the single-key
                    // owner check so "G + R" can be used even if G is already bound.
                    *capExternal_ = comboFirstVk_;
                    *capExternal2_ = k;
                    const std::string label = keyName(*capExternal_) + " + " + keyName(*capExternal2_);
                    endCapture();
                    showToast(trf("Assigned %s", label.c_str()));
                }
            }
            return true;
        }
        if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && comboFirstVk_ != 0 && static_cast<int>(wp) == comboFirstVk_) {
            *capExternal2_ = 0;
            finishCapture(comboFirstVk_);
            return true;
        }
    }

    int vk = 0;
    KeyCapture::Result r = KeyCapture::Result::None;
    bool mouse = false;
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_F1 || wp == VK_F2) return false;
            r = capture_.onKey(static_cast<int>(wp), true, (lp & 0x40000000) != 0, vk);
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            r = capture_.onKey(static_cast<int>(wp), false, false, vk);
            if (r == KeyCapture::Result::None) return false;
            break;
        case WM_RBUTTONDOWN: r = capture_.onMouseButton(VK_RBUTTON, vk), mouse = true; break;
        case WM_MBUTTONDOWN: r = capture_.onMouseButton(VK_MBUTTON, vk), mouse = true; break;
        case WM_MOUSEWHEEL:
            r = capture_.onMouseButton(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? kVkWheelUp : kVkWheelDown, vk), mouse = true;
            break;
        case WM_MOUSEHWHEEL:
            r = capture_.onMouseButton(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? kVkWheelRight : kVkWheelLeft, vk), mouse = true;
            break;
        case WM_XBUTTONDOWN:
            r = capture_.onMouseButton(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2, vk), mouse = true;
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return true;  // handled by the UI (click on the waiting marker = left mouse button)
        default: return false;
    }
    if (mouse) suppressMouse_ = true;
    if (r == KeyCapture::Result::Cancelled) cancelCapture();
    else if (r == KeyCapture::Result::Assigned) {
        if (capTarget_.slot == 0 && capTarget_.binding >= 0 && capTarget_.binding < static_cast<int>(profile_.bindings.size()))
            profile_.bindings[capTarget_.binding].key2 = 0;
        finishCapture(vk);
    }
    return true;
}

void App::addBindingAt(BindingType type, Vec2 n) {
    if (!sessionActive()) {
        showToast(tr("Connect a device to edit keys"));
        return;
    }
    if (capTarget_.slot >= 0) cancelCapture();
    n = {std::clamp(n.x, 0.0f, 1.0f), std::clamp(n.y, 0.0f, 1.0f)};
    int count = 1;
    for (const Binding& e : profile_.bindings) {
        if (e.type == type) ++count;
        if (type == BindingType::Camera && e.type == BindingType::Camera) {
            showToast(tr("Only one mouse-look control is allowed"));
            selectedBinding_ = static_cast<int>(&e - profile_.bindings.data());
            return;
        }
    }
    Binding b;
    b.type = type;
    b.pos = n;
    b.name = count > 1 ? std::string(typeLabel(type)) + " " + std::to_string(count) : typeLabel(type);
    b.key = 0;
    if (type == BindingType::Drag) b.pos2 = {std::clamp(n.x + (n.x > 0.8f ? -0.12f : 0.12f), 0.0f, 1.0f), n.y};
    const int idx = static_cast<int>(profile_.bindings.size());
    if (type == BindingType::Joystick) {
        const bool free = keyOwner(b.keyUp, -1, -1).empty() && keyOwner(b.keyLeft, -1, -1).empty() &&
                          keyOwner(b.keyDown, -1, -1).empty() && keyOwner(b.keyRight, -1, -1).empty();
        if (!free) b.keyUp = b.keyLeft = b.keyDown = b.keyRight = 0;
        profile_.bindings.push_back(b);
        selectedBinding_ = idx;
        if (free) showToast(tr("Joystick added with W A S D"));
        else beginCapture(idx, 1, true, true);
        return;
    }
    profile_.bindings.push_back(b);
    selectedBinding_ = idx;
    if (type == BindingType::Tap || type == BindingType::Drag || type == BindingType::Fire) beginCapture(idx, 0, true);
}

void App::duplicateBinding(int i) {
    if (i < 0 || i >= static_cast<int>(profile_.bindings.size())) return;
    if (capTarget_.slot >= 0) cancelCapture();
    if (i >= static_cast<int>(profile_.bindings.size())) return;
    Binding b = profile_.bindings[i];
    if (b.type == BindingType::Camera) return showToast(tr("Only one mouse-look control is allowed"));
    auto shift = [](Vec2 p) { return Vec2{std::clamp(p.x + 0.04f, 0.0f, 1.0f), std::clamp(p.y + 0.04f, 0.0f, 1.0f)}; };
    b.pos = shift(b.pos);
    b.pos2 = shift(b.pos2);
    b.name += " (2)";
    b.key = 0;
    b.recoilToggleKey = 0;
    b.rapidFireToggleKey = 0;
    if (b.type == BindingType::Joystick) b.keyUp = b.keyLeft = b.keyDown = b.keyRight = 0;
    profile_.bindings.push_back(b);
    selectedBinding_ = static_cast<int>(profile_.bindings.size()) - 1;
    if (b.type == BindingType::Joystick) beginCapture(selectedBinding_, 1, true, true);
    else beginCapture(selectedBinding_, 0, true);
}

void App::deleteBinding(int i) {
    if (i < 0 || i >= static_cast<int>(profile_.bindings.size())) return;
    if (capTarget_.slot >= 0) cancelCapture();
    if (i >= static_cast<int>(profile_.bindings.size())) return;
    releaseInput();
    profile_.bindings.erase(profile_.bindings.begin() + i);
    selectedBinding_ = -1;
    showToast(tr("Control deleted (Ctrl+Z to undo)"));
}

void App::undo() {
    if (capTarget_.slot >= 0) cancelCapture();
    cancelPick();
    history_.commit(profile_);
    if (!history_.undo(profile_)) return;
    releaseInput();
    if (selectedBinding_ >= static_cast<int>(profile_.bindings.size())) selectedBinding_ = -1;
    showToast(tr("Undone"));
}

void App::redo() {
    if (capTarget_.slot >= 0) cancelCapture();
    cancelPick();
    history_.commit(profile_);
    if (!history_.redo(profile_)) return;
    releaseInput();
    if (selectedBinding_ >= static_cast<int>(profile_.bindings.size())) selectedBinding_ = -1;
    showToast(tr("Redone"));
}

void App::requestDiscard(std::function<void()> action) {
    if (capTarget_.slot >= 0) cancelCapture();
    if (!hasUnsavedChanges()) {
        action();
        return;
    }
    discardAction_ = std::move(action);
    discardPrompt_ = true;
}

// Navigation away from Settings while connection-scoped changes are pending
// requires a reconnect first - the pending action runs after it succeeds.
void App::requestNav(std::function<void()> action) {
    if (pendingSettings_ && section_ == Section::Settings && session_) {
        reconnectAction_ = std::move(action);
        reconnectPrompt_ = true;
        return;
    }
    requestDiscard(std::move(action));
}

void App::discardChanges() {
    releaseInput();
    profile_ = saved_;
    loadRefImageFiles();  // restored Automation may reference images the draft dropped
    { std::lock_guard lk(inputMu_); input_.setProfile(profile_); }
    engineProfile_ = profile_;
    history_.reset(profile_);
    selectedBinding_ = -1;
    dragBinding_ = -1;
    dragging_ = false;
    selectedStepPath_.clear();
}

bool App::confirmDiscard() {
    if (capTarget_.slot >= 0) cancelCapture();
    if (!hasUnsavedChanges()) return true;
    requestDiscard([] {});
    return false;
}

void App::beginNamePrompt(const char* title, std::function<void(const std::string&)> action) {
    namePrompt_.open = true;
    namePrompt_.title = title;
    namePrompt_.action = std::move(action);
    namePrompt_.buf[0] = 0;
}

void App::saveProfileAs(const std::string& name) {
    if (!isValidProfileName(name)) return showToast(tr("Invalid layout name"));
    if (name != profile_.name && fs::exists(fs::u8path(profilePathFor(name)))) {
        const UINT dir = i18n::rtl() ? MB_RTLREADING | MB_RIGHT : 0;
        if (MessageBoxW(hwnd_, widen(trf("Layout \"%s\" exists. Replace it?", name.c_str())).c_str(), L"NEXPLAY",
                        MB_YESNO | MB_ICONWARNING | dir) != IDYES)
            return;
    }
    const std::string old = profile_.name;
    profile_.name = name;
    if (!saveProfile()) {
        profile_.name = old;
        return;
    }
    // Copy reference images from the old profile's directory so the new
    // layout is self-contained and images survive a reload.
    if (old != name) copyImagesDir(old, name);
}

void App::renameProfile(const std::string& name) {
    if (!isValidProfileName(name)) return showToast(tr("Invalid layout name"));
    if (name == profile_.name) return;
    if (fs::exists(fs::u8path(profilePathFor(name)))) return showToast(tr("A layout with this name already exists"));
    const std::string old = profile_.name;
    const bool existed = fs::exists(fs::u8path(profilePathFor(old)));
    profile_.name = name;
    if (!saveProfile()) {
        profile_.name = old;
        return;
    }
    // Move reference images to the new profile's directory so they survive.
    moveImagesDir(old, name);
    if (existed) {
        std::error_code ec;
        fs::remove(fs::u8path(profilePathFor(old)), ec);
        fs::remove(fs::u8path(profilePathFor(old) + ".bak"), ec);
    }
    refreshProfiles();
}

void App::newProfile(const std::string& name) {
    if (!isValidProfileName(name)) return showToast(tr("Invalid layout name"));
    if (fs::exists(fs::u8path(profilePathFor(name)))) return showToast(tr("A layout with this name already exists"));
    requestDiscard([this, name] {
        releaseInput();
        Profile p;
        p.name = name;
        p.lockToggleKey = profile_.lockToggleKey;
        profile_ = p;
        selectedBinding_ = -1;
        if (saveProfile()) history_.reset(profile_);
    });
}

void App::resetProfileToDefault() {
    if (capTarget_.slot >= 0) cancelCapture();
    const std::string n = profile_.name;
    releaseInput();
    profile_ = makeDefaultProfile();
    profile_.name = n;
    selectedBinding_ = -1;
    showToast(tr("Default layout restored (Ctrl+Z to undo)"));
}

}  // namespace wasd::app
