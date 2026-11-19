// Automation editor: color zones (picker + circular overlay), monitors and the visual macro editor.
#include <algorithm>
#include <cfloat>
#include <cstdio>

#include "app/app.h"
#include "app/i18n.h"
#include "app/studio_ui.h"
#include "app/text_shaping.h"
#include "core/studio_converter.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace wasd::app {
namespace {
const char* T(const char* en) { return text::visual(i18n::tr(en)).c_str(); }
const char* V(const std::string& s) { return text::visual(s).c_str(); }
float S(float v) { return v * std::max(1.0f, ImGui::GetStyle().FontScaleDpi); }
bool rtl() { return i18n::rtl(); }
float availW() { return ImGui::GetContentRegionAvail().x; }

constexpr ImU32 kAccent     = IM_COL32(59, 130, 246, 255);  // #3B82F6
constexpr ImU32 kAccentCyan = IM_COL32(34, 211, 238, 255);  // #22D3EE
constexpr ImU32 kAmber      = IM_COL32(245, 158, 11, 255);
constexpr ImU32 kOk         = IM_COL32(34, 197, 94, 255);
constexpr ImU32 kDanger     = IM_COL32(239, 68, 68, 255);
constexpr ImU32 kMuted      = IM_COL32(148, 163, 184, 255);

ImU32 cardBg() {
    const ImVec4 w = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    return w.x < 0.5f ? IM_COL32(37, 37, 38, 255) : IM_COL32(255, 255, 255, 255);
}
ImU32 elevBg() {
    const ImVec4 w = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    return w.x < 0.5f ? IM_COL32(45, 45, 48, 255) : IM_COL32(226, 232, 240, 255);
}
ImU32 textCol() { return ImGui::GetColorU32(ImGuiCol_Text); }
ImU32 borderCol() { return ImGui::GetColorU32(ImGuiCol_Border); }
ImU32 alpha(ImU32 c, float a) {
    return (c & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24);
}

// Step types offered in the editor (legacy image step types are never listed).
const StepType kUiSteps[] = {StepType::Wait,      StepType::Tap,   StepType::LongPress, StepType::PressDown,
                             StepType::Release,   StepType::Swipe, StepType::Key,       StepType::RunMacro,
                             StepType::If,        StepType::Loop,  StepType::Break,     StepType::Stop,
                             StepType::Comment,   StepType::ImageRecognize};
constexpr int kStepCount = int(sizeof(kUiSteps) / sizeof(kUiSteps[0]));
const char* stepName(StepType t) {
    switch (t) {
        case StepType::Wait: return "Wait";
        case StepType::Tap: return "Tap";
        case StepType::LongPress: return "Long press";
        case StepType::PressDown: return "Press down";
        case StepType::Release: return "Release";
        case StepType::Swipe: return "Swipe";
        case StepType::Key: return "Key";
        case StepType::RunMacro: return "Run macro";
        case StepType::If: return "IF / ELSE";
        case StepType::Loop: return "Loop";
        case StepType::Break: return "Break";
        case StepType::Stop: return "Stop";
        case StepType::Comment: return "Comment";
        case StepType::ImageRecognize: return "Image recognition";
        default: return "?";
    }
}
const char* stepIcon(StepType t) {
    switch (t) {
        case StepType::Wait: return "\uE823";
        case StepType::Tap:
        case StepType::LongPress:
        case StepType::PressDown:
        case StepType::Release: return "\uE7C9";
        case StepType::Swipe: return "\uE72A";
        case StepType::Key: return "\uE765";
        case StepType::RunMacro: return "\uE945";
        case StepType::If: return "\uE8FB";
        case StepType::Loop: return "\uE8EE";
        case StepType::Break:
        case StepType::Stop: return "\uE71A";
        case StepType::Comment: return "\uE90A";
        case StepType::ImageRecognize: return "\uEB9F";
        default: return "\uE945";
    }
}
ImU32 stepColor(StepType t) {
    switch (t) {
        case StepType::Wait: return IM_COL32(148, 163, 184, 255);
        case StepType::If: return IM_COL32(192, 132, 252, 255);
        case StepType::Loop: return IM_COL32(251, 146, 60, 255);
        case StepType::Break:
        case StepType::Stop: return kDanger;
        case StepType::Comment: return IM_COL32(100, 110, 125, 255);
        case StepType::ImageRecognize: return kAccentCyan;
        default: return kAccent;
    }
}
void title(const char* vis) {
    ImGui::Dummy({0, S(4)});
    const float w = ImGui::CalcTextSize(vis).x;
    if (rtl()) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, availW() - w));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", vis);
    ImGui::Separator();
}

// Short human-readable summary of a step's parameters for compact step cards.
std::string stepSummary(const Step& s, const Automation& a) {
    char buf[96];
    switch (s.type) {
        case StepType::Wait:
            std::snprintf(buf, sizeof buf, "%d ms", s.ms);
            return buf;
        case StepType::Tap:
        case StepType::PressDown:
            std::snprintf(buf, sizeof buf, "(%.2f, %.2f)", s.a.x, s.a.y);
            return buf;
        case StepType::LongPress:
            std::snprintf(buf, sizeof buf, "(%.2f, %.2f) %d ms", s.a.x, s.a.y, s.ms);
            return buf;
        case StepType::Release:
            std::snprintf(buf, sizeof buf, "F%d", s.finger + 1);
            return buf;
        case StepType::Swipe:
            std::snprintf(buf, sizeof buf, "(%.2f,%.2f)->(%.2f,%.2f) %dms", s.a.x, s.a.y, s.b.x, s.b.y, s.ms);
            return buf;
        case StepType::Key:
            return s.key ? keyName(s.key) : std::string("?");
        case StepType::RunMacro:
            return (s.macro >= 0 && s.macro < static_cast<int>(a.macros.size())) ? a.macros[s.macro].name : std::string("-");
        case StepType::If: {
            const std::string src = s.image >= 0
                                        ? (s.image < static_cast<int>(a.images.size()) ? a.images[s.image].name : "?")
                                        : (s.zone >= 0 && s.zone < static_cast<int>(a.zones.size()) ? a.zones[s.zone].name : "?");
            return (s.negate ? "! " : "") + src;
        }
        case StepType::Loop:
            std::snprintf(buf, sizeof buf, "x%d", s.count);
            return buf;
        case StepType::Comment:
            return s.text;
        case StepType::ImageRecognize: {
            const char* fa = s.foundAction == ImageFoundAction::ClickMatch    ? "click"
                             : s.foundAction == ImageFoundAction::ClickPoint  ? "click pt"
                             : s.foundAction == ImageFoundAction::RunMacro    ? "macro"
                             : s.foundAction == ImageFoundAction::RunSteps    ? "steps"
                             : s.foundAction == ImageFoundAction::JumpStep    ? "jump"
                             : s.foundAction == ImageFoundAction::Stop        ? "stop"
                                                                            : "next";
            const char* nfa = s.notFoundAction == ImageNotFoundAction::RetrySearch ? "retry"
                              : s.notFoundAction == ImageNotFoundAction::RunSteps  ? "steps"
                              : s.notFoundAction == ImageNotFoundAction::JumpStep  ? "jump"
                              : s.notFoundAction == ImageNotFoundAction::Stop      ? "stop"
                              : s.notFoundAction == ImageNotFoundAction::Fail      ? "fail"
                                                                                   : "next";
            const std::string name =
                (s.image >= 0 && s.image < static_cast<int>(a.images.size())) ? a.images[s.image].name : "-";
            char b[128];
            std::snprintf(b, sizeof b, "%s  found:%s / !found:%s", name.c_str(), fa, nfa);
            return b;
        }
        default:
            return {};
    }
}

// Renumber image references inside macro steps after deleting a reference image.
static void fixImageRefsInSteps(std::vector<Step>& list, int del) {
    for (Step& s : list) {
        if (s.image >= 0) s.image = s.image == del ? -1 : s.image > del ? s.image - 1 : s.image;
        fixImageRefsInSteps(s.then, del);
        fixImageRefsInSteps(s.otherwise, del);
    }
}
bool intField(const char* label, int* v, int lo, int hi) {
    ImGui::SetNextItemWidth(std::min(S(130), availW() * 0.55f));
    const bool ch = ImGui::InputInt(label, v, 10, 100);
    *v = std::clamp(*v, lo, hi);
    return ch;
}
}  // namespace

void App::cancelPick() {
    pickMode_ = 0;
    pickTarget_ = pickTarget2_ = nullptr;
    pickRectTarget_ = nullptr;
    pickColorTarget_ = nullptr;
    pickColorList_ = nullptr;
    pickMulti_ = false;
    pickDragging_ = false;
    pickSwipeHandle_ = 0;
    // pickPending_/pickPendingImage_ stay armed: an in-flight readback still
    // lands harmlessly (its target pointers are already cleared).
}

void App::automationUiTick() {
    // pending reference-image capture (returned region readback -> ImageRef)
    if (pickPendingImage_) {
        render::Renderer::RegionPixels px;
        if (!renderer_.pollRegion(kPickImageSlot, px) || px.rgb.size() < 3) return;
        pickPendingImage_ = false;
        const int idx = pickImage_ >= 0 && pickImage_ < static_cast<int>(profile_.automation.images.size())
                            ? pickImage_
                            : profile_.automation.images.size() < kMaxImages ? -1 : -2;
        if (idx == -2) {
            showToast(i18n::trf("Image list is full (%d)", kMaxImages));
        } else {
            ImageRef r;
            if (idx >= 0) r = profile_.automation.images[idx];
            r.w = px.w;
            r.h = px.h;
            r.rgb = px.rgb;
            if (idx < 0) {
                r.name = i18n::trf("Image %d", static_cast<int>(profile_.automation.images.size()) + 1);
                char stamp[32];
                std::snprintf(stamp, sizeof stamp, "%s-%llu.wasdimg", profile_.name.c_str(),
                              static_cast<unsigned long long>(nowMicros()));
                r.file = stamp;
                profile_.automation.images.push_back(r);
                selectedImage_ = static_cast<int>(profile_.automation.images.size()) - 1;
            } else {
                profile_.automation.images[idx] = r;
                selectedImage_ = idx;
            }
            std::string err;
            if (!saveRefImageFile(profile_.automation.images[selectedImage_], err))
                showToast(i18n::trf("Save failed: %s", err.c_str()));
            showToast(i18n::trf("Image captured: %dx%d, named '%s'", px.w, px.h,
                                profile_.automation.images[selectedImage_].name.c_str()));
        }
        return;
    }
    if (!pickPending_) return;
    render::Renderer::RegionPixels px;
    if (!renderer_.pollRegion(kPickColorSlot, px) || px.rgb.size() < 3) return;
    pickPending_ = false;
    // pickMode 8 — studio node color sampling: overwrite the target color or
    // append "r,g,b" to the extra-colors list, then keep listening until Esc.
    if (pickColorTarget_ || pickColorList_) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "#%02X%02X%02X", px.rgb[0], px.rgb[1], px.rgb[2]);
        if (pickColorTarget_) {
            pickColorTarget_->r = px.rgb[0];
            pickColorTarget_->g = px.rgb[1];
            pickColorTarget_->b = px.rgb[2];
        }
        if (pickColorList_) {
            int count = 1;
            for (char ch : *pickColorList_) count += ch == ';';
            if (count < 12) {
                char triplet[16];
                std::snprintf(triplet, sizeof triplet, "%s%u,%u,%u", pickColorList_->empty() ? "" : ";",
                              unsigned(px.rgb[0]), unsigned(px.rgb[1]), unsigned(px.rgb[2]));
                *pickColorList_ += triplet;
            } else {
                showToast(i18n::tr("Color list is full (12)"));
            }
        }
        showToast(i18n::trf("Color picked: %s - keep clicking (Esc to finish)", hex));
        return;
    }
    auto& zones = profile_.automation.zones;
    if (pickZone_ < 0 || pickZone_ >= static_cast<int>(zones.size())) return;
    ColorTarget c;
    c.r = px.rgb[0], c.g = px.rgb[1], c.b = px.rgb[2];
    auto& colors = zones[pickZone_].colors;
    if (pickColor_ >= 0 && pickColor_ < static_cast<int>(colors.size())) c.name = colors[pickColor_].name, c.tolerance = colors[pickColor_].tolerance, colors[pickColor_] = c;
    else if (colors.size() < 48) colors.push_back(c);
    char hex[16];
    std::snprintf(hex, sizeof hex, "#%02X%02X%02X", c.r, c.g, c.b);
    if (pickMulti_ && pickColor_ < 0) {
        // Continuous picking: keep listening for the next click until the zone
        // is full or the user presses Esc.
        if (colors.size() >= 48) {
            cancelPick();
            showToast(i18n::trf("Color picked: %s - zone is full (48)", hex));
        } else {
            showToast(i18n::trf("Color picked: %s - keep clicking (Esc to finish)", hex));
        }
    } else {
        showToast(i18n::trf("Color picked: %s", hex));
    }
}

void App::drawZonesOverlay() {
    auto& a = profile_.automation;
    const bool inAutoSec = section_ == Section::Colors || section_ == Section::Macros || section_ == Section::Images;
    // Studio-initiated picks hide the studio and set studioResumeAfterPick_ —
    // they are edits even though no legacy automation section is open.
    // An armed pick is always interactive, no matter which panel armed it —
    // otherwise the overlay would sit non-interactive over a frozen frame.
    const bool editing = pickMode_ != 0 ||
                         (inAutoSec && showPanel_ && !settings_.panelCollapsed && !editMode_);
    if ((!editing && !automationOn_) || videoRect_.w <= 0 || (a.zones.empty() && pickMode_ == 0)) return;
    if (pickMode_ >= 4 && pickMode_ != 7 && pickMode_ != 8) return;  // image capture / search-region pick handled by drawImagesOverlay
    const ImVec2 o{videoRect_.x, videoRect_.y}, sz{videoRect_.w, videoRect_.h};
    ImGui::SetNextWindowPos(o);
    ImGui::SetNextWindowSize(sz);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (!editing) fl |= ImGuiWindowFlags_NoInputs;
    ImGui::Begin("##zones", nullptr, fl);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto toScreen = [&](Vec2 n) { return ImVec2(o.x + n.x * sz.x, o.y + n.y * sz.y); };
    if (section_ == Section::Colors || automationOn_) {
        for (int i = 0; i < static_cast<int>(a.zones.size()); ++i) {
            Zone& z = a.zones[i];
            const ImVec2 c = toScreen(z.center);
            const float r = std::max(2.0f, z.radius * sz.y);
            const bool sel = editing && section_ == Section::Colors && i == selectedZone_;
            const bool present = i < static_cast<int>(zoneScan_.size()) && zoneScan_[i].present.value_or(false);
            bool monitored = false;
            for (const Monitor& m : a.monitors) monitored |= m.zone == i && m.enabled;
            const ImU32 col = zoneTest_ && sel ? kAmber : !monitored ? IM_COL32(160, 160, 170, 200) : automationOn_ ? kOk : kAccent;
            if (present) dl->AddCircleFilled(c, r, IM_COL32(34, 197, 94, 50));
            dl->AddCircle(c, r, sel ? kAccent : col, 64, sel ? S(2.5f) : S(1.5f));
            dl->AddText({c.x - r, c.y - r - ImGui::GetTextLineHeight() - S(2)}, sel ? kAccent : col, V(z.name));
            if (!editing || section_ != Section::Colors || pickMode_) continue;
            ImGui::PushID(i);
            ImGui::SetCursorScreenPos({c.x - S(10), c.y - S(10)});
            ImGui::InvisibleButton("##c", {S(20), S(20)});
            if (ImGui::IsItemActivated()) selectedZone_ = i, dragStart_ = z.center;
            if (ImGui::IsItemActive()) {
                const ImVec2 d = ImGui::GetMouseDragDelta(0, 0);
                z.center = {std::clamp(dragStart_.x + d.x / sz.x, 0.0f, 1.0f), std::clamp(dragStart_.y + d.y / sz.y, 0.0f, 1.0f)};
            }
            dl->AddCircleFilled(c, S(4), sel ? kAccent : col);
            if (sel) {
                const ImVec2 h{c.x + r, c.y};
                ImGui::SetCursorScreenPos({h.x - S(8), h.y - S(8)});
                ImGui::InvisibleButton("##r", {S(16), S(16)});
                if (ImGui::IsItemActive()) z.radius = std::clamp((ImGui::GetIO().MousePos.x - c.x) / sz.y, 0.005f, 0.5f);
                if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                dl->AddRectFilled({h.x - S(5), h.y - S(5)}, {h.x + S(5), h.y + S(5)}, kAccent, S(2));
            }
            ImGui::PopID();
        }
    }
    // pickMode 7 — swipe path editor: a start circle and an end handle joined
    // by an arrow, both draggable on the video, mirroring the Drag binding's
    // markers in the key editor. Done commits, Esc restores the originals.
    if (editing && pickMode_ == 7 && pickTarget_ && pickTarget2_) {
        const ImVec2 c1 = toScreen(*pickTarget_), c2 = toScreen(*pickTarget2_);
        const ImVec2 d{c2.x - c1.x, c2.y - c1.y};
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        const float r1 = S(20);
        if (len > r1 + S(12)) {
            const ImVec2 u{d.x / len, d.y / len}, n{-u.y, u.x};
            const ImVec2 s0{c1.x + u.x * r1, c1.y + u.y * r1}, tip{c2.x - u.x * S(9), c2.y - u.y * S(9)};
            dl->AddLine(s0, tip, kAmber, S(2));
            dl->AddTriangleFilled({tip.x + u.x * S(1), tip.y + u.y * S(1)},
                                  {tip.x - u.x * S(9) + n.x * S(5), tip.y - u.y * S(9) + n.y * S(5)},
                                  {tip.x - u.x * S(9) - n.x * S(5), tip.y - u.y * S(9) - n.y * S(5)}, kAmber);
        }
        // draggable start marker
        ImGui::SetCursorScreenPos({c1.x - S(22), c1.y - S(22)});
        ImGui::InvisibleButton("##swp1", {S(44), S(44)});
        if (ImGui::IsItemActivated()) pickSwipeHandle_ = 1, dragStart_ = *pickTarget_;
        if (ImGui::IsItemActive() && pickSwipeHandle_ == 1 && ImGui::IsMouseDown(0)) {
            const ImVec2 dd = ImGui::GetMouseDragDelta(0, 0);
            *pickTarget_ = {std::clamp(dragStart_.x + dd.x / sz.x, 0.0f, 1.0f),
                            std::clamp(dragStart_.y + dd.y / sz.y, 0.0f, 1.0f)};
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        // draggable end marker
        ImGui::SetCursorScreenPos({c2.x - S(13), c2.y - S(13)});
        ImGui::InvisibleButton("##swp2", {S(26), S(26)});
        if (ImGui::IsItemActivated()) pickSwipeHandle_ = 2, dragStart_ = *pickTarget2_;
        if (ImGui::IsItemActive() && pickSwipeHandle_ == 2 && ImGui::IsMouseDown(0)) {
            const ImVec2 dd = ImGui::GetMouseDragDelta(0, 0);
            *pickTarget2_ = {std::clamp(dragStart_.x + dd.x / sz.x, 0.0f, 1.0f),
                             std::clamp(dragStart_.y + dd.y / sz.y, 0.0f, 1.0f)};
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseReleased(0)) pickSwipeHandle_ = 0;
        dl->AddCircleFilled(c1, r1, alpha(kAccent, 0.25f));
        dl->AddCircle(c1, r1, kAccent, 0, S(2));
        dl->AddText({c1.x - r1, c1.y - r1 - ImGui::GetTextLineHeight() - S(2)}, kAccent, T("Start"));
        dl->AddCircleFilled(c2, S(9), alpha(kAmber, 0.3f));
        dl->AddCircle(c2, S(9), kAmber, 0, S(1.5f));
        dl->AddText({c2.x + S(12), c2.y - ImGui::GetTextLineHeight() * 0.5f}, kAmber, T("End"));
        // Done / hint strip at the top of the video
        ImGui::SetCursorScreenPos({o.x + S(10), o.y + S(10)});
        if (ImGui::Button(T("Done"))) cancelPick();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 1, 1, 0.75f), "%s", T("Drag the Start and End points (Esc restores)"));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            *pickTarget_ = pickSaved1_, *pickTarget2_ = pickSaved2_;
            cancelPick();
        }
    } else if (editing && pickMode_) {
        ImGui::SetCursorScreenPos(o);
        if (ImGui::InvisibleButton("##pick", sz)) {
            const ImVec2 m = ImGui::GetIO().MouseClickedPos[0];
            const Vec2 n{std::clamp((m.x - o.x) / sz.x, 0.0f, 1.0f), std::clamp((m.y - o.y) / sz.y, 0.0f, 1.0f)};
            if (pickMode_ == 1 || pickMode_ == 8) {
                if (!renderer_.hasVideo()) showToast(i18n::tr("No video: connect the phone to pick colors"));
                else if (renderer_.requestRegion(kPickColorSlot, static_cast<uint32_t>(n.x * renderer_.videoWidth()),
                                                 static_cast<uint32_t>(n.y * renderer_.videoHeight()), 2, 2))
                    pickPending_ = true;
            } else if (pickMode_ == 2) {
                Zone z;
                z.name = i18n::trf("Zone %d", static_cast<int>(a.zones.size()) + 1);
                z.center = n;
                a.zones.push_back(z);
                selectedZone_ = static_cast<int>(a.zones.size()) - 1;
                // Drawing from inside a rule auto-binds the new zone to it.
                if (selectedMonitor_ >= 0 && selectedMonitor_ < static_cast<int>(a.monitors.size()) &&
                    a.monitors[selectedMonitor_].image < 0) {
                    a.monitors[selectedMonitor_].zone = selectedZone_;
                    monitorRt_.clear();
                }
            } else if (pickTarget_) {
                *pickTarget_ = n;
            }
            // Continuous color picking stays armed until Esc or a full zone.
            if (!(pickMode_ == 1 && pickMulti_) && pickMode_ != 8) cancelPick();
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddRect(o, {o.x + sz.x, o.y + sz.y}, kAmber, 0, 0, S(2));
        dl->AddText({o.x + S(10), o.y + S(10)}, kAmber,
                    T(pickMode_ == 8 ? "Click pixels to register their colors (Esc to finish)"
                      : pickMode_ == 1 && pickMulti_ ? "Keep clicking to add colors (Esc to finish)"
                                                  : "Click on the game (Esc cancels)"));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            cancelPick();
        }
    }
    ImGui::End();
}

// ---- reference images: overlay + capture + search region --------------------------------

void App::drawImagesOverlay() {
    auto& a = profile_.automation;
    const bool inImgSec = section_ == Section::Images || section_ == Section::Macros;
    const bool pickActive = pickMode_ == 4 || pickMode_ == 5 || pickMode_ == 6;
    // An armed pick is always interactive (see drawZonesOverlay).
    const bool editing = pickActive ||
                         (inImgSec && showPanel_ && !settings_.panelCollapsed && !editMode_);
    // Detection overlays draw here too: the standalone watcher (detectWatch_),
    // the inspector's live test (detectPreview_) and the last Find Object
    // result (detectProbe_) while a graph runs.
    const bool detectBoxes = detectWatchOn_ || detectPreview_.active ||
                             (detectProbe_.ready && nowMicros() - detectProbe_.readyAt < 3000000);
    if ((!editing && !automationOn_ && !imgTest_ && !detectBoxes) || videoRect_.w <= 0 ||
        (a.images.empty() && !pickActive && !detectBoxes)) return;
    const ImVec2 o{videoRect_.x, videoRect_.y}, sz{videoRect_.w, videoRect_.h};
    ImGui::SetNextWindowPos(o);
    ImGui::SetNextWindowSize(sz);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (!editing) fl |= ImGuiWindowFlags_NoInputs;
    ImGui::Begin("##images", nullptr, fl);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto toScreen = [&](Vec2 n) { return ImVec2(o.x + n.x * sz.x, o.y + n.y * sz.y); };

    for (int i = 0; i < static_cast<int>(a.images.size()) && i < kMaxImages; ++i) {
        ImageRef& r = a.images[i];
        const bool sel = editing && section_ == Section::Images && i == selectedImage_;
        const bool scanning = i < static_cast<int>(imgScan_.size()) && imgScan_[i].tested && nowMicros() - imgScan_[i].lastUs < 500000;
        const ImU32 col = sel ? kAccent : scanning ? IM_COL32(160, 160, 170, 220) : IM_COL32(120, 120, 130, 180);
        const Rect& g = r.region;
        const ImVec2 t1 = toScreen({g.x, g.y}), t2 = toScreen({g.x + g.w, g.y + g.h});
        const bool showFrame = sel || (r.showOverlay && (automationOn_ || imgTest_));
        if (showFrame) {
            dl->AddRect(t1, t2, col, S(3), 0, sel ? S(2) : S(1));
            dl->AddText({t1.x, t1.y - ImGui::GetTextLineHeight() - S(2)}, col, V(r.name));
        }
        // last positive match
        if ((automationOn_ || imgTest_) && i < static_cast<int>(imgScan_.size()) && imgScan_[i].match.found &&
            nowMicros() - imgScan_[i].lastUs < 1000000) {
            const ImageMatch& mm = imgScan_[i].match;
            const ImVec2 m1 = toScreen({mm.rect.x, mm.rect.y}), m2 = toScreen({mm.rect.x + mm.rect.w, mm.rect.y + mm.rect.h});
            dl->AddRect(m1, m2, kOk, 0, 0, S(2));
            char score[24];
            std::snprintf(score, sizeof score, "%.0f%%", imgScan_[i].score * 100);
            dl->AddText({m2.x + S(4), m1.y}, kOk, score);
        }
        if (sel && pickMode_ == 0) {
            ImGui::PushID(100 + i);
            const ImVec2 cen{t1.x + (t2.x - t1.x) * 0.5f, t1.y + (t2.y - t1.y) * 0.5f};
            ImGui::SetCursorScreenPos({cen.x - S(10), cen.y - S(10)});
            ImGui::InvisibleButton("##imc", {S(20), S(20)});
            if (ImGui::IsItemActivated()) imgHandle_ = 1, imgRegionStart_ = r.region;
            if (ImGui::IsItemActive()) {
                const ImVec2 d = ImGui::GetMouseDragDelta(0, 0);
                const Vec2 dx = {d.x / sz.x, d.y / sz.y};
                r.region = {std::clamp(imgRegionStart_.x + dx.x, 0.0f, 1.0f - r.region.w),
                            std::clamp(imgRegionStart_.y + dx.y, 0.0f, 1.0f - r.region.h), r.region.w, r.region.h};
            }
            dl->AddCircleFilled(cen, S(4), sel ? kAccent : col);
            const ImVec2 hnd{t2.x, t2.y};  // bottom-right resize handle
            ImGui::SetCursorScreenPos({hnd.x - S(9), hnd.y - S(9)});
            ImGui::InvisibleButton("##ims", {S(18), S(18)});
            if (ImGui::IsItemActivated()) imgHandle_ = 2, imgRegionStart_ = r.region;
            if (ImGui::IsItemActive()) {
                const ImVec2 m = ImGui::GetIO().MousePos;
                const float nx = std::clamp((m.x - o.x) / sz.x, 0.0f, 1.0f), ny = std::clamp((m.y - o.y) / sz.y, 0.0f, 1.0f);
                r.region.x = imgRegionStart_.x;
                r.region.y = imgRegionStart_.y;
                r.region.w = std::clamp(nx - r.region.x, 0.02f, 1.0f - r.region.x);
                r.region.h = std::clamp(ny - r.region.y, 0.02f, 0.0f + 1.0f - r.region.y);
            }
            dl->AddRectFilled({hnd.x - S(4), hnd.y - S(4)}, {hnd.x + S(4), hnd.y + S(4)}, kAccent, S(2));
            ImGui::PopID();
        }
    }

    // capture / search-region pick: drag a rectangle on the game
    if (editing && pickActive) {
        ImGui::SetCursorScreenPos(o);
        ImGui::InvisibleButton("##impick", sz);
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(0)) {
            pickDragging_ = true;
            const ImVec2 m = ImGui::GetIO().MouseClickedPos[0];
            pickDragOrigin_ = {m.x, m.y, 0, 0};
            pickRect_ = Rect{m.x, m.y, 0, 0};
        }
        if (pickDragging_ && ImGui::IsMouseDown(0)) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            pickRect_ = Rect{pickDragOrigin_.x, pickDragOrigin_.y, m.x - pickDragOrigin_.x, m.y - pickDragOrigin_.y};
            dl->AddRect({pickRect_.x, pickRect_.y}, {pickRect_.x + pickRect_.w, pickRect_.y + pickRect_.h}, kAmber, 0, 0, S(2));
        }
        if (ImGui::IsItemDeactivated() && pickDragging_) {
            pickDragging_ = false;
            const float x0 = std::clamp((std::min(pickRect_.x, pickRect_.x + pickRect_.w) - o.x) / sz.x, 0.0f, 1.0f);
            const float y0 = std::clamp((std::min(pickRect_.y, pickRect_.y + pickRect_.h) - o.y) / sz.y, 0.0f, 1.0f);
            const float x1 = std::clamp((std::max(pickRect_.x, pickRect_.x + pickRect_.w) - o.x) / sz.x, 0.0f, 1.0f);
            const float y1 = std::clamp((std::max(pickRect_.y, pickRect_.y + pickRect_.h) - o.y) / sz.y, 0.0f, 1.0f);
            const Rect n{x0, y0, x1 - x0, y1 - y0};
            if (pickMode_ == 4) {
                // capture the crop from the live frame
                const uint32_t vw = renderer_.videoWidth(), vh = renderer_.videoHeight();
                const bool ok = renderer_.hasVideo() &&
                                renderer_.requestRegion(kPickImageSlot, uint32_t(n.x * vw), uint32_t(n.y * vh), uint32_t(n.w * vw),
                                                        uint32_t(n.h * vh));
                if (!ok) showToast(i18n::tr("No video: connect the phone to capture an image"));
                else pickPendingImage_ = true;
            } else if (pickMode_ == 6 && pickRectTarget_) {
                *pickRectTarget_ = n;
                pickRectTarget_ = nullptr;
                showToast(i18n::tr("Search region updated"));
            } else if (pickImage_ >= 0 && pickImage_ < static_cast<int>(a.images.size())) {
                a.images[pickImage_].region = n;
                showToast(i18n::tr("Search region updated"));
            }
            cancelPick();
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        dl->AddRect(o, {o.x + sz.x, o.y + sz.y}, kAmber, 0, 0, S(2));
        dl->AddText({o.x + S(10), o.y + S(10)}, kAmber,
                    T(pickMode_ == 4 ? "Drag a rectangle to capture (Esc cancels)" : "Drag the search region (Esc cancels)"));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) cancelPick();
    }

    // Detection boxes: the continuous watcher first, then the inspector live
    // test, then the running graph's last Find Object result — green box +
    // "class conf%" label.
    auto drawDetect = [&](const DetectProbe& p, bool labels) {
        const std::vector<std::string> names = detectorClassNames();
        for (const Detection& d : p.hits) {
            const ImU32 bc = d.boosted ? kAmber : kOk;  // amber = rescued by red outline
            const ImVec2 b1 = toScreen({d.box.x, d.box.y}), b2 = toScreen({d.box.x + d.box.w, d.box.y + d.box.h});
            dl->AddRect(b1, b2, bc, 0, 0, S(2));
            if (!labels) continue;
            std::string lbl;
            if (d.classId >= 0 && d.classId < (int)names.size()) lbl = names[d.classId] + " ";
            char conf[16];
            std::snprintf(conf, sizeof conf, "%.0f%%", d.confidence * 100);
            lbl += conf;
            dl->AddText({b1.x, b1.y - ImGui::GetTextLineHeight() - S(1)}, bc, lbl.c_str());
        }
    };
    if (detectWatchOn_) {
        const DetectWatch& dw = a.detect;
        if (dw.showRegion) {
            const ImVec2 r1 = toScreen({dw.region.x, dw.region.y}),
                         r2 = toScreen({dw.region.x + dw.region.w, dw.region.y + dw.region.h});
            dl->AddRect(r1, r2, kAccentCyan, S(3), 0, S(1));
            dl->AddText({r1.x, r1.y + S(2)}, kAccentCyan, T("Detection area"));
        }
        if (dw.exclOn && dw.showExcl) {
            const ImVec2 e1 = toScreen({dw.excl.x, dw.excl.y}),
                         e2 = toScreen({dw.excl.x + dw.excl.w, dw.excl.y + dw.excl.h});
            dl->AddRect(e1, e2, kAmber, S(3), 0, S(1));
            dl->AddText({e1.x, e1.y + S(2)}, kAmber, T("Ignored zone"));
        }
        // Tracked boxes: predicted positions keep each box glued to its target
        // between scans (velocity extrapolation) and through brief detection
        // dips — no per-scan flicker.
        if (dw.drawBoxes) {
            const int64_t nowU = nowMicros();
            const float predS = std::clamp(dw.trackPredictMs, 0, 2000) / 1000.0f;
            const std::vector<std::string> names = detectorClassNames();
            for (const DetectTrack& t : detectTracker_.tracks) {
                const Rect pb = detectTracker_.predicted(t, nowU, predS);
                const bool locked = dw.aimOn && aimHeld_ && t.id == aimTrackId_;
                // State-colored: a lost/candidate track must NOT look like a
                // confirmed detection — muted for lost, amber for unproven.
                ImU32 bc;
                if (locked) bc = kDanger;
                else if (t.state == TrackState::TempLost) bc = IM_COL32(150, 150, 160, 190);
                else if (t.state == TrackState::Reacquiring) bc = kAmber;
                else bc = t.boosted ? kAmber : kOk;
                const ImVec2 b1 = toScreen({pb.x, pb.y}), b2 = toScreen({pb.x + pb.w, pb.y + pb.h});
                dl->AddRect(b1, b2, bc, 0, 0, locked ? S(3) : S(2));
                if (locked) {
                    // Aim point inside the box (preset or custom rel point).
                    const Vec2 tp = pointInBox(pb, PointPreset(std::clamp(dw.pointPreset, 0, 4)),
                                               dw.pointX / 100.0f, dw.pointY / 100.0f);
                    const ImVec2 ap = toScreen(tp);
                    dl->AddCircle(ap, S(4), kDanger, 0, S(1.5f));
                    // Inner stop zone: the pull settles only once the crosshair
                    // is inside this inset box (the real hit area).
                    const float zx = pb.w * (dw.ctrlInnerPct / 100.0f);
                    const float zy = pb.h * (dw.ctrlInnerPct / 100.0f);
                    const ImVec2 z1 = toScreen({pb.x + zx, pb.y + zy});
                    const ImVec2 z2 = toScreen({pb.x + pb.w - zx, pb.y + pb.h - zy});
                    dl->AddRect(z1, z2, IM_COL32(255, 120, 120, 160), 0, 0, S(1));
                }
                if (!dw.drawLabels) continue;
                std::string lbl;
                if (t.classId >= 0 && t.classId < (int)names.size()) lbl = names[t.classId] + " ";
                char conf[16];
                std::snprintf(conf, sizeof conf, "%.0f%%", t.conf * 100);
                lbl += conf;
                if (t.state == TrackState::TempLost) lbl += " (lost)";
                else if (t.state == TrackState::Reacquiring) lbl += " (?)";
                if (dw.dbgIds) {
                    char idb[32];
                    std::snprintf(idb, sizeof idb, " #%u %s", t.id, trackStateName(t.state));
                    lbl += idb;
                }
                dl->AddText({b1.x, b1.y - ImGui::GetTextLineHeight() - S(1)}, bc, lbl.c_str());
                // Debug: raw last-measurement box under the smoothed one.
                if (dw.dbgRaw) {
                    const ImVec2 r1 = toScreen({t.measBox.x, t.measBox.y});
                    const ImVec2 r2 = toScreen({t.measBox.x + t.measBox.w, t.measBox.y + t.measBox.h});
                    dl->AddRect(r1, r2, IM_COL32(255, 255, 255, 140), 0, 0, S(1));
                }
            }
            // Debug: every raw model hit (pre-fusion), thin white outlines.
            if (dw.dbgRaw)
                for (const Detection& d : detectWatch_.rawHits) {
                    const ImVec2 r1 = toScreen({d.box.x, d.box.y});
                    const ImVec2 r2 = toScreen({d.box.x + d.box.w, d.box.y + d.box.h});
                    dl->AddRect(r1, r2, IM_COL32(120, 200, 255, 110), 0, 0, S(1));
                }
            // Debug perf chip: inference rate, frame age, effective Auto fps,
            // plus the pull diagnostics — saturation % (how starved the cap
            // leaves the controller), calibrated plant gain, camera teleports
            // and bounds clamps.
            if (dw.dbgPerf) {
                const int sat = aimSteerTicks_
                    ? int(100.0 * double(aimSatTicks_) / double(aimSteerTicks_)) : 0;
                char perf[256];
                std::snprintf(perf, sizeof perf,
                              "infer %lldus  fps %d%s  frame #%llu  age %lldms  tracks %d  lock %s",
                              (long long)detectWatch_.lastInferUs, inferSched_.effectiveFps(),
                              dw.inferFps ? "" : "A", (unsigned long long)detectWatch_.frameId,
                              (long long)((nowU - detectWatch_.captureUs) / 1000),
                              (int)detectTracker_.tracks.size(), lockStateName(targetLock_.state));
                dl->AddText({o.x + S(6), o.y + S(6)}, IM_COL32(255, 255, 255, 220), perf);
                char perf2[256];
                uint64_t tele = 0, clamps = 0;
                {
                    std::lock_guard lk(inputMu_);
                    tele = input_.camTeleports();
                    clamps = input_.camBoundsClamps();
                }
                std::snprintf(perf2, sizeof perf2,
                              "sat %d%%  k %.2fpx/cnt  tele %llu  clamp %llu",
                              sat, double(aimCtrl_.kPxPerCount),
                              (unsigned long long)tele, (unsigned long long)clamps);
                dl->AddText({o.x + S(6), o.y + S(22)}, IM_COL32(255, 220, 140, 220), perf2);
            }
        }
    } else if (detectPreview_.active) {
        drawDetect(detectPreview_, true);
    } else if (detectProbe_.ready && nowMicros() - detectProbe_.readyAt < 3000000) {
        drawDetect(detectProbe_, true);
    }
    ImGui::End();
}

void App::imagePixelPreview(const ImageRef& r, float box, const Vec2* mark) {
    if (r.rgb.empty() || !r.w || !r.h) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float size = S(box);
    const int side = 14;
    const float cs = size / side;
    dl->AddRectFilled(p, {p.x + size, p.y + size}, IM_COL32(30, 30, 30, 255), S(4));
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            const int px = std::min<int>(r.w - 1, x * r.w / side);
            const int py = std::min<int>(r.h - 1, y * r.h / side);
            const uint8_t* c = &r.rgb[(size_t(py) * r.w + px) * 3];
            dl->AddRectFilled({p.x + x * cs, p.y + y * cs}, {p.x + (x + 1) * cs, p.y + (y + 1) * cs}, IM_COL32(c[0], c[1], c[2], 255));
        }
    dl->AddRect(p, {p.x + size, p.y + size}, borderCol(), S(4));
    const Vec2 markPt = mark ? *mark : r.clickOffset;
    const ImVec2 cp{p.x + markPt.x * size, p.y + markPt.y * size};
    dl->AddCircle(cp, S(5), kOk, 0, S(1.5f));
    ImGui::Dummy({size, size});
}

// Renders only the properties relevant to the single selected step `s`.
void App::drawStepInspector(Step& s, int /*depth*/, const std::vector<Step>& siblings) {
    auto& a = profile_.automation;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, elevBg());
    ImGui::BeginChild("##step_insp", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);

    // Command type selector
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("Command type"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##stype", T(stepName(s.type)))) {
        for (int t = 0; t < kStepCount; ++t)
            if (ImGui::Selectable(T(stepName(kUiSteps[t])), kUiSteps[t] == s.type)) s.type = kUiSteps[t];
        ImGui::EndCombo();
    }
    ImGui::Checkbox(T("Step enabled"), &s.enabled);

    auto pointRow = [&](const char* labelVis, Vec2& v) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", labelVis);
        const float pickW = S(86);
        ImGui::SetNextItemWidth(std::max(S(90), availW() - pickW - ImGui::GetStyle().ItemSpacing.x));
        ImGui::SliderFloat2((std::string("##pt_") + labelVis).c_str(), &v.x, 0.0f, 1.0f, "%.3f");
        ImGui::SameLine();
        if (ImGui::Button((std::string(T("Pick")) + "##" + labelVis).c_str(), {pickW, 0})) pickMode_ = 3, pickTarget_ = &v;
    };

    switch (s.type) {
        case StepType::Wait:
            intField(T("Wait duration (ms)"), &s.ms, 0, 600000);
            break;
        case StepType::Tap:
            pointRow(T("Position (X, Y)"), s.a);
            ImGui::Checkbox(T("Advanced settings"), &stepAdvOpen_);
            if (stepAdvOpen_) intField(T("Finger"), &s.finger, 0, kMaxFingers - 1);
            break;
        case StepType::PressDown:
            pointRow(T("Position (X, Y)"), s.a);
            intField(T("Finger (0-7, keep different fingers for simultaneous holds)"), &s.finger, 0, kMaxFingers - 1);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                               T("Finger stays down until a Release step or the macro ends"));
            break;
        case StepType::LongPress:
            pointRow(T("Position (X, Y)"), s.a);
            intField(T("Hold duration (ms)"), &s.ms, 0, 600000);
            ImGui::Checkbox(T("Advanced settings"), &stepAdvOpen_);
            if (stepAdvOpen_) intField(T("Finger"), &s.finger, 0, kMaxFingers - 1);
            break;
        case StepType::Release:
            intField(T("Finger (which finger to lift, 0-7)"), &s.finger, 0, kMaxFingers - 1);
            break;
        case StepType::Swipe:
            pointRow(T("From"), s.a);
            pointRow(T("To"), s.b);
            intField(T("Swipe duration (ms)"), &s.ms, 0, 600000);
            ImGui::Checkbox(T("Advanced settings"), &stepAdvOpen_);
            if (stepAdvOpen_) intField(T("Finger"), &s.finger, 0, kMaxFingers - 1);
            break;
        case StepType::Key: {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("Key"));
            const bool waiting = capture_.active() && capTarget_.slot == 6 && capExternal_ == &s.key;
            if (ImGui::Button(waiting ? T("Press a key...") : s.key ? keyName(s.key).c_str() : T("Set key"), {-FLT_MIN, 0})) {
                if (waiting) finishCapture(VK_LBUTTON);
                else beginExternalCapture(&s.key, 6);
            }
            int km = static_cast<int>(s.keyMode);
            const char* modes[] = {T("Press"), T("Hold"), T("Release")};
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo(T("Action"), &km, modes, 3)) s.keyMode = static_cast<KeyMode>(km);
            break;
        }
        case StepType::RunMacro: {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("Target macro"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##mac", s.macro >= 0 && s.macro < static_cast<int>(a.macros.size()) ? V(a.macros[s.macro].name) : "-")) {
                for (int m = 0; m < static_cast<int>(a.macros.size()); ++m) {
                    ImGui::PushID(m);
                    if (ImGui::Selectable(V(a.macros[m].name), m == s.macro)) s.macro = m;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            break;
        }
        case StepType::If:
        case StepType::Loop: {
            if (s.type == StepType::Loop) {
                int lm = static_cast<int>(s.loopMode);
                const char* modes[] = {T("Repeat N times"), T("While condition holds"), T("Until condition holds")};
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo(T("Loop mode"), &lm, modes, 3)) s.loopMode = static_cast<LoopMode>(lm);
                intField(s.loopMode == LoopMode::Count ? T("Times") : T("Max times"), &s.count, 1, kMaxLoopCount);
                intField(T("Pause ms"), &s.ms, 0, 600000);
            }
            if (s.type == StepType::If || s.loopMode != LoopMode::Count) {
                int src = s.image >= 0 ? 1 : 0;
                const char* srcs[] = {T("Color zone"), T("Image")};
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo(T("Source"), &src, srcs, 2)) {
                    if (src == 1) s.zone = -1, s.image = a.images.size() > 0 ? 0 : -1;
                    else s.image = -1, s.zone = a.zones.size() > 0 ? 0 : -1;
                }
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (src == 1) {
                    if (ImGui::BeginCombo(T("Image"), s.image >= 0 && s.image < static_cast<int>(a.images.size()) ? V(a.images[s.image].name)
                                                                                                                  : T("Image..."))) {
                        for (int im = 0; im < static_cast<int>(a.images.size()) && im < kMaxImages; ++im) {
                            ImGui::PushID(im);
                            if (ImGui::Selectable(V(a.images[im].name), im == s.image)) s.image = im;
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                } else if (ImGui::BeginCombo(T("Zone"), s.zone >= 0 && s.zone < static_cast<int>(a.zones.size()) ? V(a.zones[s.zone].name)
                                                                                                                 : T("Zone..."))) {
                    for (int z = 0; z < static_cast<int>(a.zones.size()); ++z) {
                        ImGui::PushID(z);
                        if (ImGui::Selectable(V(a.zones[z].name), z == s.zone)) s.zone = z;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::Checkbox(T("Invert condition (NOT)"), &s.negate);
            }
            break;
        }
        case StepType::Comment: {
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s", s.text.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText(T("Comment"), buf, sizeof buf)) s.text = buf;
            break;
        }
        case StepType::ImageRecognize:
            drawImageStepInspector(s, siblings);
            break;
        default: break;
    }

    if (ImGui::Button(T("Close step settings"), {-FLT_MIN, 0})) selectedStepPath_.clear();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::deleteRefImage(int idx) {
    auto& a = profile_.automation;
    if (idx < 0 || idx >= static_cast<int>(a.images.size())) return;
    deleteRefImageFile(a.images[idx]);
    a.images.erase(a.images.begin() + idx);
    for (Monitor& m : a.monitors) m.image = m.image == idx ? -1 : m.image > idx ? m.image - 1 : m.image;
    for (Macro& mac : a.macros) fixImageRefsInSteps(mac.steps, idx);
    // Studio graphs hold image refs as integer props — renumber them too.
    for (auto& gg : a.studioGraphs)
        for (auto& n : gg.nodes)
            if (auto it = n.props.find("imageIndex"); it != n.props.end() && it->second.i >= 0)
                it->second.i = it->second.i == idx ? -1 : it->second.i > idx ? it->second.i - 1 : it->second.i;
    if (selectedImage_ == idx) selectedImage_ = -1;
    else if (selectedImage_ > idx) --selectedImage_;
    if (imgTestImage_ == idx) imgTestImage_ = -1;
    else if (imgTestImage_ > idx) --imgTestImage_;
    imgScan_.assign(a.images.size(), ImageScan{});
}

// Unified "Image recognition" step editor: which image, how to search, and what to
// do on the found / not-found branches — with a live single-shot test.
void App::drawImageStepInspector(Step& s, const std::vector<Step>& siblings) {
    auto& a = profile_.automation;
    const bool haveImg = s.image >= 0 && s.image < static_cast<int>(a.images.size());

    auto pointRow = [&](const char* id, const char* labelVis, Vec2& v) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", labelVis);
        const float pickW = S(86);
        ImGui::SetNextItemWidth(std::max(S(90), availW() - pickW - ImGui::GetStyle().ItemSpacing.x));
        ImGui::SliderFloat2((std::string("##pt_") + id).c_str(), &v.x, 0.0f, 1.0f, "%.3f");
        ImGui::SameLine();
        if (ImGui::Button((std::string(T("Pick")) + "##" + id).c_str(), {pickW, 0})) pickMode_ = 3, pickTarget_ = &v;
    };

    // ---- A. Reference image -------------------------------------------------
    if (ImGui::CollapsingHeader(T("Reference image"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##img_sel", haveImg ? V(a.images[s.image].name) : T("Image..."))) {
            for (int im = 0; im < static_cast<int>(a.images.size()) && im < kMaxImages; ++im) {
                ImGui::PushID(im);
                if (ImGui::Selectable(V(a.images[im].name), im == s.image)) s.image = im;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button(T("+ Capture new image from game"), {-FLT_MIN, 0})) {
            pickMode_ = 4;
            pickImage_ = -1;
        }
        if (haveImg) {
            ImageRef& ref = a.images[s.image];
            imagePixelPreview(ref, 64.0f, &s.clickOfs);
            ImGui::SameLine();
            ImGui::BeginGroup();
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s", ref.name.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText(T("Name"), buf, sizeof buf)) ref.name = buf;
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                               V(i18n::trf("Crop %dx%d", ref.w, ref.h)));
            if (ImGui::SmallButton(T("Replace crop (drag a rectangle)"))) pickMode_ = 4, pickImage_ = s.image;
            if (ImGui::SmallButton(T("Delete image"))) deleteRefImage(s.image);
            ImGui::EndGroup();
        } else {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), "%s",
                               T("Select a saved image or capture a new one."));
        }
    }

    // ---- B. Recognition settings --------------------------------------------
    if (ImGui::CollapsingHeader(T("Search settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
        // Similarity: per-step override, 0 = inherit the image's own threshold.
        bool thrOv = s.thresholdPct > 0;
        if (ImGui::Checkbox(T("Override similarity threshold"), &thrOv))
            s.thresholdPct = thrOv ? (haveImg ? a.images[s.image].thresholdPct : 85) : 0;
        if (thrOv) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderInt(T("Matching threshold %d%%"), &s.thresholdPct, 1, 100);
        } else if (haveImg) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                               V(i18n::trf("Threshold: image default %d%%", a.images[s.image].thresholdPct)));
        }
        // Search region: saved on the image / whole screen / a custom step rect.
        int rm = static_cast<int>(s.regionMode);
        const char* rmNames[] = {T("Saved region"), T("Entire screen"), T("Custom region")};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo(T("Search region"), &rm, rmNames, 3)) s.regionMode = static_cast<ImageRegionMode>(rm);
        if (s.regionMode == ImageRegionMode::Saved && haveImg) {
            ImageRef& ref = a.images[s.image];
            if (ImGui::SmallButton(T("Search region: drag"))) pickMode_ = 5, pickImage_ = s.image;
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Whole screen"))) ref.region = Rect{0.0f, 0.0f, 1.0f, 1.0f};
        } else if (s.regionMode == ImageRegionMode::Custom) {
            if (ImGui::SmallButton(T("Drag the region on the game"))) pickMode_ = 6, pickRectTarget_ = &s.region;
            ImGui::SameLine();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                               V(i18n::trf("(%.2f,%.2f) %0.2fx%0.2f", s.region.x, s.region.y, s.region.w, s.region.h)));
        }
        intField(T("Search timeout ms (0 = check once)"), &s.ms, 0, 600000);
        intField(T("Retry count (0 = until timeout)"), &s.retryCount, 0, 1000);
        if (s.retryCount > 0) intField(T("Retry interval ms"), &s.retryIntervalMs, 0, 600000);
        ImGui::Checkbox(T("Continuous search (re-fire on reappearance)"), &s.continuous);
        if (s.continuous && s.ms <= 0)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), "%s",
                               T("Continuous search needs a timeout as exit condition."));
        intField(T("Scan interval ms (0 = image default)"), &s.intervalMs, 0, 60000);
        // Which candidate to act on when several copies match.
        int mp = static_cast<int>(s.matchPolicy);
        const char* mpNames[] = {T("Highest score"), T("First (top-left)"), T("Nearest to a point"), T("All matches")};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo(T("If several match"), &mp, mpNames, 4)) s.matchPolicy = static_cast<MatchPolicy>(mp);
        if (s.matchPolicy == MatchPolicy::Nearest) pointRow("near", T("Near point (X, Y)"), s.nearTo);
        ImGui::Checkbox(T("Invert: found = image absent"), &s.negate);
    }

    // ---- C. ON FOUND ----------------------------------------------------------
    if (ImGui::CollapsingHeader(T("When image is found"), ImGuiTreeNodeFlags_DefaultOpen)) {
        int fa = static_cast<int>(s.foundAction);
        const char* faNames[] = {T("Continue to next step"), T("Click the detected image"), T("Click a fixed point"),
                                 T("Run macro"), T("Run a step sequence"), T("Jump to a step"), T("Stop the macro")};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##found_act", &fa, faNames, 7)) s.foundAction = static_cast<ImageFoundAction>(fa);
        const bool clicking = s.foundAction == ImageFoundAction::ClickMatch || s.foundAction == ImageFoundAction::ClickPoint;
        if (s.foundAction == ImageFoundAction::ClickMatch) {
            pointRow("cofs", T("Click point inside the image"), s.clickOfs);
            if (ImGui::SmallButton(T("Center"))) s.clickOfs = {0.5f, 0.5f};
        } else if (s.foundAction == ImageFoundAction::ClickPoint) {
            pointRow("fc", T("Click point (X, Y)"), s.foundClick);
        }
        if (clicking) {
            int ck = static_cast<int>(s.clickKind);
            const char* ckNames[] = {T("Single tap"), T("Long press"), T("Double tap")};
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo(T("Gesture"), &ck, ckNames, 3)) s.clickKind = static_cast<ClickKind>(ck);
            if (s.clickKind != ClickKind::Tap) intField(T("Press duration ms"), &s.pressMs, 1, 600000);
            intField(T("Delay before click ms"), &s.delayMs, 0, 600000);
            intField(T("Finger"), &s.finger, 0, kMaxFingers - 1);
        }
        if (s.foundAction == ImageFoundAction::RunMacro) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo(T("Macro to run"), s.foundMacro >= 0 && s.foundMacro < static_cast<int>(a.macros.size())
                                                          ? V(a.macros[s.foundMacro].name)
                                                          : "-")) {
                for (int m = 0; m < static_cast<int>(a.macros.size()); ++m) {
                    ImGui::PushID(m);
                    if (ImGui::Selectable(V(a.macros[m].name), m == s.foundMacro)) s.foundMacro = m;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }
        if (s.foundAction == ImageFoundAction::RunSteps)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("Edit the steps in the ON FOUND list below."));
        else if (s.foundAction == ImageFoundAction::Continue || s.foundAction == ImageFoundAction::ClickMatch ||
                 s.foundAction == ImageFoundAction::ClickPoint || s.foundAction == ImageFoundAction::RunMacro)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                               T("Optional: add steps in the ON FOUND list below to run after this action."));
        if (s.foundAction == ImageFoundAction::JumpStep || s.notFoundAction == ImageNotFoundAction::JumpStep) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            char cur[32];
            std::snprintf(cur, sizeof cur, "#%d", s.jumpTo + 1);
            if (ImGui::BeginCombo(T("Jump to step"), s.jumpTo >= 0 && s.jumpTo < static_cast<int>(siblings.size())
                                                          ? (std::string(cur) + " " + i18n::tr(stepName(siblings[s.jumpTo].type))).c_str()
                                                          : T("Step..."))) {
                for (int j = 0; j < static_cast<int>(siblings.size()); ++j) {
                    char lbl[64];
                    std::snprintf(lbl, sizeof lbl, "#%d %s", j + 1, i18n::tr(stepName(siblings[j].type)));
                    if (ImGui::Selectable(lbl, j == s.jumpTo)) s.jumpTo = j;
                }
                ImGui::EndCombo();
            }
        }
    }

    // ---- D. ON NOT FOUND ------------------------------------------------------
    if (ImGui::CollapsingHeader(T("When image is not found"), ImGuiTreeNodeFlags_DefaultOpen)) {
        int nfa = static_cast<int>(s.notFoundAction);
        const char* nfaNames[] = {T("Continue to next step"), T("Keep retrying"), T("Run a step sequence"),
                                  T("Jump to a step"), T("Stop the macro"), T("Fail the macro")};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##nf_act", &nfa, nfaNames, 6)) s.notFoundAction = static_cast<ImageNotFoundAction>(nfa);
        if (s.notFoundAction == ImageNotFoundAction::RetrySearch)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                               T("Retries reset the attempt count; the timeout still bounds the search."));
        if (s.notFoundAction == ImageNotFoundAction::RunSteps)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                               T("Edit the steps in the ON NOT FOUND list below."));
    }

    // ---- E. Live test -----------------------------------------------------------
    ImGui::Separator();
    if (!haveImg) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("Select an image to test the search."));
        return;
    }
    const bool testing = imgTest_ && imgTestImage_ == s.image;
    if (ImGui::Button(testing ? T("Stop test") : T("Test search (no clicks)"), {-FLT_MIN, 0})) {
        if (testing) imgTest_ = false, imgTestImage_ = -1;
        else imgTest_ = true, imgTestImage_ = s.image;
    }
    if (testing) {
        // Drive the shared override with this step's effective parameters so the
        // test exercises exactly what the step would search for.
        const ImageQuery q = imageQueryFor(s, a);
        imgOverride_.image = s.image;
        imgOverride_.region = q.region;
        imgOverride_.thresholdPct = q.thresholdPct;
        imgOverride_.intervalMs = q.intervalMs;
        imgOverride_.lastQueryUs = nowMicros();
        if (s.image < static_cast<int>(imgScan_.size())) {
            const ImageScan& sc = imgScan_[s.image];
            const bool ok = sc.tested && sc.doneRegion == q.region && sc.doneThreshold == q.thresholdPct &&
                            nowMicros() - sc.lastUs <= 500000;
            if (!ok) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("scanning..."));
            } else if (sc.found) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kOk), "%s",
                                   V(i18n::trf("Found: %d%% at (%.3f, %.3f)  box (%.2f,%.2f %.2fx%.2f)  %.1f ms  x%d",
                                               int(sc.score * 100), sc.match.center.x, sc.match.center.y, sc.match.rect.x,
                                               sc.match.rect.y, sc.match.rect.w, sc.match.rect.h,
                                               sc.scanCostUs / 1000.0, int(sc.matches.size()))));
            } else {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDanger), "%s",
                                   V(i18n::trf("Not found: best %d%%  %.1f ms", int(sc.score * 100), sc.scanCostUs / 1000.0)));
            }
        }
    }
}

void App::stepList(std::vector<Step>& list, int depth, std::vector<int>& pathPrefix) {
    auto& a = profile_.automation;
    int move = -1, dir = 0, dup = -1, del = -1;
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        Step& s = list[i];
        std::vector<int> curPath = pathPrefix;
        curPath.push_back(i);
        const bool isSelected = (selectedStepPath_ == curPath);
        const bool running = macros_.active() && macros_.currentStep() == &s;

        ImGui::PushID(i);
        // Compact Step Card
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = availW(), cardH = S(36);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bg = isSelected ? elevBg() : cardBg();
        dl->AddRectFilled(p0, {p0.x + w, p0.y + cardH}, bg, S(6));
        dl->AddRect(p0, {p0.x + w, p0.y + cardH}, isSelected ? kAccent : borderCol(), S(6), 0, isSelected ? S(1.5f) : S(1.0f));
        // Colored accent bar on the leading edge
        const float barX = rtl() ? p0.x + w - S(4) : p0.x;
        dl->AddRectFilled({barX, p0.y + S(4)}, {barX + S(4), p0.y + cardH - S(4)},
                          running ? kOk : s.enabled ? stepColor(s.type) : IM_COL32(100, 110, 125, 180), S(2));

        // Clickable main area of the compact step card (toggles step inspector)
        const float actW = S(116);
        const float clickW = std::max(S(60), w - actW - S(8));
        ImGui::SetCursorScreenPos({rtl() ? p0.x + actW : p0.x, p0.y});
        if (ImGui::InvisibleButton("##step_card", {clickW, cardH})) {
            if (isSelected) selectedStepPath_.clear();
            else selectedStepPath_ = curPath;
        }

        // Step number + Icon + Type + Summary
        char num[16];
        std::snprintf(num, sizeof num, "#%d", i + 1);
        const std::string sum = stepSummary(s, a);
        const std::string titleStr = std::string(num) + "  " + stepIcon(s.type) + " " + i18n::tr(stepName(s.type));
        const char* tv = V(titleStr);
        const char* sv = V(sum);
        const float ty = p0.y + (cardH - ImGui::GetTextLineHeight()) * 0.5f;
        if (rtl()) {
            const float tw = ImGui::CalcTextSize(tv).x;
            const float tx = p0.x + w - S(12) - tw;
            dl->AddText({tx, ty}, s.enabled ? textCol() : kMuted, tv);
            if (!sum.empty()) {
                const float sw = ImGui::CalcTextSize(sv).x;
                dl->PushClipRect({p0.x + actW + S(4), p0.y}, {tx - S(8), p0.y + cardH}, true);
                dl->AddText({tx - S(10) - sw, ty}, kMuted, sv);
                dl->PopClipRect();
            }
        } else {
            const float tx = p0.x + S(12);
            const float tw = ImGui::CalcTextSize(tv).x;
            dl->AddText({tx, ty}, s.enabled ? textCol() : kMuted, tv);
            if (!sum.empty()) {
                dl->PushClipRect({tx + tw + S(8), p0.y}, {p0.x + w - actW - S(4), p0.y + cardH}, true);
                dl->AddText({tx + tw + S(10), ty}, kMuted, sv);
                dl->PopClipRect();
            }
        }

        // Compact action buttons on the trailing edge (^, v, copy, delete)
        const float btnY = p0.y + S(6);
        ImGui::SetCursorScreenPos({rtl() ? p0.x + S(6) : p0.x + w - actW, btnY});
        if (ImGui::SmallButton("^")) move = i, dir = -1;
        ImGui::SameLine(0, S(2));
        if (ImGui::SmallButton("v")) move = i, dir = 1;
        ImGui::SameLine(0, S(2));
        if (ImGui::SmallButton("\uE8C8")) dup = i;
        ImGui::SameLine(0, S(2));
        if (ImGui::SmallButton("\uE74D")) del = i;

        ImGui::SetCursorScreenPos({p0.x, p0.y + cardH});
        ImGui::Dummy({w, S(4)});

        // Only open the property inspector for the single selected step
        if (isSelected) {
            drawStepInspector(s, depth, list);
            ImGui::Dummy({0, S(4)});
        }

        // Nested branches stay accessible: IF/ELSE paths, the LOOP body and the
        // ON FOUND / ON NOT FOUND sequences of an image step.
        const bool imgStep = s.type == StepType::ImageRecognize;
        // Image steps show the ON FOUND list for every found action that can be
        // followed by a sequence (RunSteps itself, and click/continue/run-macro
        // where `then` runs after the action finishes).
        const bool imgThen = imgStep && s.foundAction != ImageFoundAction::JumpStep &&
                             s.foundAction != ImageFoundAction::Stop;
        const bool showThen = s.type == StepType::If || s.type == StepType::Loop || imgThen;
        const bool showElse = s.type == StepType::If || (imgStep && s.notFoundAction == ImageNotFoundAction::RunSteps);
        if ((showThen || showElse) && depth < kMaxDepth - 1) {
            ImGui::Indent(S(14));
            if (showThen) {
                const char* lbl = s.type == StepType::If ? "THEN" : s.type == StepType::Loop ? "DO" : "ON FOUND";
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(stepColor(s.type)), "%s", T(lbl));
                ImGui::PushID("then");
                std::vector<int> thenPrefix = curPath;
                thenPrefix.push_back(0);
                stepList(s.then, depth + 1, thenPrefix);
                ImGui::PopID();
            }
            if (showElse) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(stepColor(s.type)), "%s",
                                   s.type == StepType::If ? T("ELSE") : T("ON NOT FOUND"));
                ImGui::PushID("else");
                std::vector<int> elsePrefix = curPath;
                elsePrefix.push_back(1);
                stepList(s.otherwise, depth + 1, elsePrefix);
                ImGui::PopID();
            }
            ImGui::Unindent(S(14));
        }
        ImGui::PopID();
    }

    char addStepId[32], addPopupId[32];
    std::snprintf(addStepId, sizeof addStepId, "##addstep_%d_%d", depth, static_cast<int>(pathPrefix.size()));
    std::snprintf(addPopupId, sizeof addPopupId, "##addstep_pop_%d_%d", depth, static_cast<int>(pathPrefix.size()));
    if (static_cast<int>(list.size()) < kMaxSteps && ImGui::Button(T("+ Add step"), {-FLT_MIN, 0})) ImGui::OpenPopup(addPopupId);
    if (ImGui::BeginPopup(addPopupId)) {
        for (int t = 0; t < kStepCount; ++t)
            if (ImGui::Selectable(T(stepName(kUiSteps[t])))) {
                Step s;
                s.type = kUiSteps[t];
                list.push_back(s);
                selectedStepPath_ = pathPrefix;
                selectedStepPath_.push_back(static_cast<int>(list.size()) - 1);
                cancelPick();
            }
        ImGui::EndPopup();
    }
    if (move >= 0 || dup >= 0 || del >= 0) {
        if (capture_.active()) cancelCapture();
        cancelPick();
    }
    if (move >= 0 && move + dir >= 0 && move + dir < static_cast<int>(list.size())) {
        std::swap(list[move], list[move + dir]);
        std::vector<int> movedFrom = pathPrefix;
        movedFrom.push_back(move);
        if (selectedStepPath_ == movedFrom) selectedStepPath_.back() = move + dir;
    }
    if (dup >= 0 && dup < static_cast<int>(list.size())) {
        list.insert(list.begin() + dup + 1, list[dup]);
        selectedStepPath_ = pathPrefix;
        selectedStepPath_.push_back(dup + 1);
    }
    if (del >= 0 && del < static_cast<int>(list.size())) {
        list.erase(list.begin() + del);
        selectedStepPath_.clear();
    }
}

void App::panelMacros() {
    auto& a = profile_.automation;

    // NEXPLAY Automation Studio entry: prominent card switching to the visual
    // node-graph workspace. Simple mode stays fully intact underneath.
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
        ImGui::BeginChild("##studio_open_card", {0, 0},
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
                              ImGuiChildFlags_Borders);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAccentCyan), "%s", T("Automation Studio"));
        ImGui::TextWrapped("%s", T("Design automations as a visual node graph with wires, variables, "
                                   "image and color recognition."));
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(kAccentCyan));
        if (ImGui::Button(T("Open Studio"), {-FLT_MIN, S(34)})) openStudio();
        ImGui::PopStyleColor(2);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // Live macro execution status bar & emergency stop
    if (macros_.state() != RunState::Idle) {
        const RunState rs = macros_.state();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
        ImGui::BeginChild("##mac_run_bar", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rs == RunState::Failed ? kDanger : macros_.active() ? kOk : kMuted), "%s",
                           V(i18n::trf("Macro: %s  %s  (%d steps)", i18n::tr(runStateName(rs)), macros_.message().c_str(), macros_.executedSteps())));
        if (macros_.active() && ImGui::Button(T("STOP (emergency)"), {-FLT_MIN, S(30)})) macros_.cancel("stopped by user");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // ---- Region 1: Compact Macros List --------------------------------------------
    title(T("Macros"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##mac_search", T("Search macros..."), macroSearch_, sizeof macroSearch_);

    if (ImGui::Button(T("+ Add macro"), {-FLT_MIN, S(32)})) {
        Macro m;
        m.name = i18n::trf("Macro %d", static_cast<int>(a.macros.size()) + 1);
        a.macros.push_back(m);
        selectedMacro_ = static_cast<int>(a.macros.size()) - 1;
        selectedStepPath_.clear();
        macroEditorTab_ = 0;
    }

    int dupMacro = -1, delMacro = -1;
    if (a.macros.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
        ImGui::BeginChild("##no_macs", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("No macros yet. Click '+ Add macro' to create one."));
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        const bool scrollList = a.macros.size() > 5;
        const float listMaxH = scrollList ? S(260) : 0.0f;
        const ImGuiChildFlags listFlags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding |
                                          (scrollList ? ImGuiChildFlags_None : ImGuiChildFlags_AutoResizeY);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
        ImGui::BeginChild("##mac_list", {0, listMaxH}, listFlags);
        for (int i = 0; i < static_cast<int>(a.macros.size()); ++i) {
            Macro& m = a.macros[i];
            if (macroSearch_[0] != '\0' && m.name.find(macroSearch_) == std::string::npos) continue;
            ImGui::PushID(2000 + i);
            const bool sel = (i == selectedMacro_);
            const bool runningThis = macros_.active() && macros_.macro() == i;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float w = availW(), cardH = S(40);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, {p0.x + w, p0.y + cardH}, sel ? elevBg() : cardBg(), S(6));
            dl->AddRect(p0, {p0.x + w, p0.y + cardH}, sel ? kAccent : borderCol(), S(6), 0, sel ? S(1.5f) : S(1.0f));
            if (sel) {
                const float bx = rtl() ? p0.x + w - S(4) : p0.x;
                dl->AddRectFilled({bx, p0.y + S(5)}, {bx + S(4), p0.y + cardH - S(5)}, kAccent, S(2));
            }

            const float actW = S(68);
            ImGui::SetCursorScreenPos({rtl() ? p0.x + actW : p0.x, p0.y});
            if (ImGui::InvisibleButton("##mcard", { std::max(S(60), w - actW), cardH })) {
                if (selectedMacro_ != i) {
                    selectedMacro_ = i;
                    selectedStepPath_.clear();
                } else {
                    selectedMacro_ = -1;
                    selectedStepPath_.clear();
                }
            }

            // Status dot + Name + Step count + Key badge
            const std::string keyBadge = m.key ? (m.key2 ? keyName(m.key) + "+" + keyName(m.key2) : keyName(m.key))
                                              : std::string(i18n::tr("No key"));
            const std::string sub = i18n::trf("%d steps", static_cast<int>(m.steps.size())) + "  |  " + keyBadge;
            const char* nv = V(m.name);
            const char* sv = V(sub);
            const float dotR = S(4);
            const ImU32 dotCol = runningThis ? kOk : (m.key ? kAccentCyan : kMuted);
            if (rtl()) {
                const float nw = ImGui::CalcTextSize(nv).x;
                const float sw = ImGui::CalcTextSize(sv).x;
                dl->AddCircleFilled({p0.x + w - S(12), p0.y + cardH * 0.5f}, dotR, dotCol);
                dl->AddText({p0.x + w - S(22) - nw, p0.y + S(4)}, textCol(), nv);
                dl->AddText({p0.x + w - S(22) - sw, p0.y + S(21)}, kMuted, sv);
            } else {
                dl->AddCircleFilled({p0.x + S(12), p0.y + cardH * 0.5f}, dotR, dotCol);
                dl->AddText({p0.x + S(22), p0.y + S(4)}, textCol(), nv);
                dl->AddText({p0.x + S(22), p0.y + S(21)}, kMuted, sv);
            }

            // Compact Copy & Delete buttons on the card
            ImGui::SetCursorScreenPos({rtl() ? p0.x + S(6) : p0.x + w - actW, p0.y + S(8)});
            if (ImGui::SmallButton("\uE8C8")) dupMacro = i;
            ImGui::SameLine(0, S(3));
            if (ImGui::SmallButton("\uE74D")) delMacro = i;

            ImGui::SetCursorScreenPos({p0.x, p0.y + cardH});
            ImGui::Dummy({w, S(4)});
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    if (dupMacro >= 0 && dupMacro < static_cast<int>(a.macros.size())) {
        Macro copy = a.macros[dupMacro];
        copy.name += " (2)";
        copy.key = 0;
        a.macros.insert(a.macros.begin() + dupMacro + 1, copy);
        // Indices after the insert point shift by one — keep monitor references aligned.
        for (Monitor& mo : a.monitors) {
            if (mo.macro > dupMacro) ++mo.macro;
            if (mo.elseMacro > dupMacro) ++mo.elseMacro;
        }
        selectedMacro_ = dupMacro + 1;
        selectedStepPath_.clear();
    }
    if (delMacro >= 0 && delMacro < static_cast<int>(a.macros.size())) {
        if (macros_.active()) macros_.cancel("macro deleted");
        a.macros.erase(a.macros.begin() + delMacro);
        for (Monitor& mo : a.monitors) {
            mo.macro = mo.macro == delMacro ? -1 : mo.macro > delMacro ? mo.macro - 1 : mo.macro;
            mo.elseMacro = mo.elseMacro == delMacro ? -1 : mo.elseMacro > delMacro ? mo.elseMacro - 1 : mo.elseMacro;
        }
        selectedMacro_ = -1;
        selectedStepPath_.clear();
        cancelPick();
    }

    // ---- Region 2: Selected Macro Editor (Master-Detail) -------------------------
    title(T("Selected macro editor"));
    if (selectedMacro_ < 0 || selectedMacro_ >= static_cast<int>(a.macros.size())) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
        ImGui::BeginChild("##no_sel_mac", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
        const char* m1 = T("Select an item to view its settings");
        const char* m2 = T("Click a macro card above to edit its steps, trigger key and settings.");
        if (rtl()) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, availW() - ImGui::CalcTextSize(m1).x));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textCol()), "%s", m1);
        if (rtl()) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, availW() - ImGui::CalcTextSize(m2).x));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", m2);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    Macro& m = a.macros[selectedMacro_];
    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
    ImGui::BeginChild("##mac_editor", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);

    // Sub-tabs inside the selected macro editor: Steps vs Macro Settings
    {
        const float halfW = (availW() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button(T("Steps"), {halfW, 0})) macroEditorTab_ = 0;
        ImGui::SameLine();
        if (ImGui::Button(T("Macro settings"), {halfW, 0})) macroEditorTab_ = 1;
    }
    ImGui::Separator();

    if (macroEditorTab_ == 1) {
        // Basic info & trigger key
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("Name"));
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s", m.name.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##macname", buf, sizeof buf)) m.name = buf;

        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("Key (or hold 2 keys together, e.g. Ctrl + F1)"));
        const bool waiting = capture_.active() && capTarget_.slot == 7 && capExternal_ == &m.key;
        const std::string keyLabel = m.key2 ? keyName(m.key) + "+" + keyName(m.key2) : m.key ? keyName(m.key) : T("Set trigger key");
        if (ImGui::Button(waiting ? T("Press a key...") : keyLabel.c_str(), {-FLT_MIN, 0})) {
            if (waiting) finishCapture(VK_LBUTTON);
            else beginExternalCapture(&m.key, 7, &m.key2);
        }
        if (m.key && ImGui::SmallButton(T("No key"))) m.key = m.key2 = 0;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("Second key (dual-key combo, optional)"));
        const bool waitK2 = capture_.active() && capTarget_.slot == 7 && capExternal_ == &m.key2;
        if (ImGui::Button(waitK2 ? T("Press a key...") : m.key2 ? keyName(m.key2).c_str() : T("Set second key"), {-FLT_MIN, 0})) {
            if (waitK2) finishCapture(VK_LBUTTON);
            else beginExternalCapture(&m.key2, 7);
        }
        if (m.key2 && ImGui::SmallButton(T("No second key"))) m.key2 = 0;
        if (m.key && m.key2)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                               V(i18n::trf("Macro starts when %s and %s are held together", keyName(m.key).c_str(),
                                           keyName(m.key2).c_str())));
        intField(T("Max duration ms"), &m.maxDurationMs, 100, 600000);

        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("Run mode"));
        {
            int rm = static_cast<int>(m.runMode);
            const char* rmNames[] = {T("Once"), T("Continuous (loop)"), T("Until image found"), T("Until image not found")};
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##runmode", &rm, rmNames, 4)) m.runMode = static_cast<MacroRunMode>(rm);
        }
        if (m.runMode != MacroRunMode::Once)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                              T("Continuous modes restart the macro automatically when it completes or fails. Use STOP to end it."));
    } else {
        // Steps sequence (compact cards + single selected step inspector)
        std::vector<int> rootPrefix;
        stepList(m.steps, 1, rootPrefix);
    }

    ImGui::Separator();
    // Bottom action bar for the selected macro (wraps automatically on narrow width)
    const float btnW = (availW() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button(macros_.active() ? T("Stop") : T("Test run"), {btnW, 0})) {
        if (macros_.active()) macros_.cancel("stopped by user");
        else if (!sessionActive()) showToast(i18n::tr("Connect a device first"));
        else if (auto errs = validateAutomation(a); !errs.empty()) showToast(errs.front());
        else macros_.start(a, selectedMacro_, nowMicros());
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Save"), {btnW, 0})) saveProfile();

    // Non-destructive conversion to a visual graph — the macro itself is never
    // touched; the graph records sourceLegacyMacro + a reviewable report.
    if (ImGui::Button(T("Convert to Studio graph"), {-FLT_MIN, 0})) {
        studio::ConversionReport rep = studio::macroToGraph(m, a);
        a.studioGraphs.push_back(std::move(rep.graph));
        const int gi = static_cast<int>(a.studioGraphs.size()) - 1;
        openStudio();
        if (studio_) studio_->focusGraph(gi);
        if (rep.warnings > 0)
            showToast(i18n::trf("Converted with %d note(s) — see Graph tab report", rep.warnings));
        else
            showToast(i18n::trf("Macro converted to graph (%d steps)", rep.stepsConverted));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Creates an editable node graph — this macro stays untouched."));

    if (ImGui::Button(T("Close"), {-FLT_MIN, 0})) {
        selectedMacro_ = -1;
        selectedStepPath_.clear();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::panelImages() {
    auto& a = profile_.automation;
    title(T("Reference images"));

    ImGui::Checkbox(T("Live test"), &imgTest_);
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Refresh from disk"))) loadRefImageFiles(), showToast(i18n::tr("Reference images reloaded"));

    if (ImGui::Button(T("+ New image (drag a rectangle on the game)"), {-FLT_MIN, S(32)})) {
        pickMode_ = 4;
        pickImage_ = -1;
    }

    // Compact cards for saved reference images
    if (a.images.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
        ImGui::BeginChild("##no_imgs", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("No reference images saved yet."));
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        for (int i = 0; i < static_cast<int>(a.images.size()) && i < kMaxImages; ++i) {
            ImageRef& r = a.images[i];
            ImGui::PushID(3000 + i);
            const ImageScan* sc = i < static_cast<int>(imgScan_.size()) ? &imgScan_[i] : nullptr;
            const std::string status = !sc || !sc->tested ? "..." : i18n::trf("last: %d%%", int(sc->score * 100));
            const bool sel = (i == selectedImage_);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, sel ? elevBg() : cardBg());
            ImGui::BeginChild("##img_card", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
            if (ImGui::Selectable(V(r.name + "   [" + status + "]"), sel)) selectedImage_ = (sel ? -1 : i);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }

    // Property Inspector for the single selected Reference Image
    title(T("Properties"));
    if (selectedImage_ < 0 || selectedImage_ >= static_cast<int>(a.images.size()) || selectedImage_ >= kMaxImages) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
        ImGui::BeginChild("##no_sel_img", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textCol()), "%s", T("Select an item to view its settings"));
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    ImageRef& r = a.images[selectedImage_];
    ImageScan* sc = selectedImage_ < static_cast<int>(imgScan_.size()) ? &imgScan_[selectedImage_] : nullptr;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
    ImGui::BeginChild("##img_insp", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);

    char buf[64];
    std::snprintf(buf, sizeof buf, "%s", r.name.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText(T("Name"), buf, sizeof buf)) r.name = buf;

    imagePixelPreview(r, 84.0f);
    ImGui::SameLine();
    ImGui::BeginGroup();
    int pct = r.thresholdPct;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt(T("Matching threshold %d%%"), &pct, 1, 100)) r.thresholdPct = std::clamp(pct, 1, 100);
    if (sc && sc->tested)
        ImGui::TextColored(sc->found ? ImGui::ColorConvertU32ToFloat4(kOk) : ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                           V(sc->found ? i18n::trf("Found: %d%% at (%.3f, %.3f)", int(sc->score * 100), sc->match.center.x, sc->match.center.y)
                                       : i18n::trf("Not found: best score %d%%", int(sc->score * 100))));
    ImGui::EndGroup();

    const float halfW = (availW() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button(T("Search region: drag"), {halfW, 0})) pickMode_ = 5, pickImage_ = selectedImage_;
    ImGui::SameLine();
    if (ImGui::Button(T("Whole screen"), {halfW, 0})) r.region = Rect{0.0f, 0.0f, 1.0f, 1.0f};
    if (ImGui::Button(T("Replace crop (drag a rectangle)"), {-FLT_MIN, 0})) pickMode_ = 4, pickImage_ = selectedImage_;

    ImGui::Checkbox(T("Advanced settings"), &imageAdvOpen_);
    if (imageAdvOpen_) {
        intField(T("Scan every ms"), &r.intervalMs, 30, 5000);
        ImGui::SetNextItemWidth(-FLT_MIN);
        float off[2] = {r.clickOffset.x, r.clickOffset.y};
        if (ImGui::SliderFloat2(T("Click point in image"), off, 0.0f, 1.0f, "%.2f"))
            r.clickOffset = {std::clamp(off[0], 0.0f, 1.0f), std::clamp(off[1], 0.0f, 1.0f)};
        ImGui::Checkbox(T("Show region frame"), &r.showOverlay);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                           V(i18n::trf("Crop %dx%d  |  region (%.2f,%.2f)-(%.2f,%.2f)", r.w, r.h, r.region.x, r.region.y,
                                       r.region.x + r.region.w, r.region.y + r.region.h)));
    }

    if (ImGui::Button(T("Delete image"), {halfW, 0})) deleteRefImage(selectedImage_);
    ImGui::SameLine();
    if (ImGui::Button(T("Close"), {halfW, 0})) selectedImage_ = -1;

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::panelColors() {
    auto& a = profile_.automation;
    // Monitoring status card
    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
    ImGui::BeginChild("##mon_status", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    const bool video = sessionActive() && renderer_.hasVideo();
    const ImU32 dot = !video ? IM_COL32(120, 120, 130, 255) : zoneTest_ ? kAmber : automationOn_ ? kOk : kDanger;
    const char* st = !video ? T("Not ready: no video") : automationOn_ ? T("Monitoring is running") : zoneTest_ ? T("Test mode") : T("Monitoring is stopped");
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled({p.x + S(6), p.y + ImGui::GetTextLineHeight() * 0.5f}, S(5), dot);
    ImGui::SetCursorScreenPos({p.x + S(18), p.y});
    ImGui::TextUnformatted(st);
    if (!automationStatus_.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", V(automationStatus_));
    if (ImGui::Button(automationOn_ ? T("Stop monitoring") : T("Start monitoring"), {-FLT_MIN, S(30)})) setAutomation(!automationOn_, i18n::tr("Stopped"));
    const bool waiting = capture_.active() && capTarget_.slot == 7 && capExternal_ == &a.toggleKey;
    if (ImGui::SmallButton(waiting ? "..." : V(i18n::trf("Toggle / emergency stop key: %s", keyName(a.toggleKey).c_str())))) {
        if (waiting) finishCapture(VK_LBUTTON);
        else beginExternalCapture(&a.toggleKey, 7);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Sub-tabs: Color Zones vs Detection Monitors
    const float halfW = (availW() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button(T("Color zones"), {halfW, 0})) colorsTab_ = 0;
    ImGui::SameLine();
    if (ImGui::Button(T("Rules (what happens on detection)"), {halfW, 0})) colorsTab_ = 1;

    if (colorsTab_ == 0) {
        title(T("Color zones"));
        ImGui::Checkbox(T("Live test"), &zoneTest_);
        if (ImGui::Button(T("+ Add zone (click on the game)"), {-FLT_MIN, S(30)})) pickMode_ = 2;

        for (int i = 0; i < static_cast<int>(a.zones.size()); ++i) {
            ImGui::PushID(i);
            const bool present = i < static_cast<int>(zoneScan_.size()) && zoneScan_[i].present.value_or(false);
            const bool sel = (i == selectedZone_);
            if (ImGui::Selectable(V(a.zones[i].name + (present ? "  [*]" : "")), sel)) selectedZone_ = (sel ? -1 : i);
            ImGui::PopID();
        }

        title(T("Properties"));
        if (selectedZone_ >= 0 && selectedZone_ < static_cast<int>(a.zones.size())) {
            Zone& z = a.zones[selectedZone_];
            ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
            ImGui::BeginChild("##zone_insp", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s", z.name.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText(T("Name"), buf, sizeof buf)) z.name = buf;
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##zr", &z.radius, 0.005f, 0.3f, T("Radius %.4f"));
            int mode = z.mode == ColorMatchMode::All ? 1 : 0;
            const char* modes[] = {T("Any color"), T("All colors together")};
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo(T("Mode"), &mode, modes, 2)) z.mode = mode ? ColorMatchMode::All : ColorMatchMode::Any;

            const ZoneScan* sc = selectedZone_ < static_cast<int>(zoneScan_.size()) ? &zoneScan_[selectedZone_] : nullptr;
            int delColor = -1;
            for (int c = 0; c < static_cast<int>(z.colors.size()); ++c) {
                ColorTarget& t = z.colors[c];
                ImGui::PushID(100 + c);
                ImGui::ColorButton("##sw", ImVec4(t.r / 255.0f, t.g / 255.0f, t.b / 255.0f, 1), 0, {S(22), S(22)});
                ImGui::SameLine();
                ImGui::Text("#%02X%02X%02X", t.r, t.g, t.b);
                if (sc && sc->match.perColor.size() == z.colors.size())
                    ImGui::SameLine(), ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kOk), "%d/%d", sc->match.perColor[c], sc->match.total);
                ImGui::SameLine();
                if (ImGui::SmallButton(T("Re-pick"))) pickMode_ = 1, pickZone_ = selectedZone_, pickColor_ = c, pickMulti_ = false;
                ImGui::SameLine();
                if (ImGui::SmallButton("\uE74D")) delColor = c;
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderFloat(T("Tolerance"), &t.tolerance, 0.0f, 60.0f, "%.1f");
                ImGui::PopID();
            }
            if (delColor >= 0) z.colors.erase(z.colors.begin() + delColor);
            if (z.colors.size() < 48 && ImGui::Button(T("+ Add color (pick from the game)"), {-FLT_MIN, 0}))
                pickMode_ = 1, pickZone_ = selectedZone_, pickColor_ = -1, pickMulti_ = true;

            ImGui::Checkbox(T("Advanced settings"), &zoneAdvOpen_);
            if (zoneAdvOpen_) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderFloat2(T("Position (X, Y)"), &z.center.x, 0.0f, 1.0f, "%.4f");
                intField(T("Min pixels"), &z.minPixels, 1, 100000);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderFloat(T("Min ratio"), &z.minRatio, 0.0f, 1.0f, "%.2f");
            }

            if (ImGui::Button(T("Delete zone"), {halfW, 0})) {
                const int zi = selectedZone_;
                a.zones.erase(a.zones.begin() + zi);
                for (Monitor& m : a.monitors) m.zone = m.zone == zi ? -1 : m.zone > zi ? m.zone - 1 : m.zone;
                selectedZone_ = -1;
                cancelPick();
            }
            ImGui::SameLine();
            if (ImGui::Button(T("Close"), {halfW, 0})) selectedZone_ = -1;
            ImGui::EndChild();
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
            ImGui::BeginChild("##no_sel_z", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textCol()), "%s", T("Select an item to view its settings"));
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
    } else {
        title(T("Rules"));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                           T("A rule watches a zone or an image, then runs an action. Arm it with its key or the switch."));
        if (ImGui::Button(T("+ New rule"), {-FLT_MIN, S(30)})) {
            Monitor m;
            m.name = i18n::trf("Rule %d", static_cast<int>(a.monitors.size()) + 1);
            m.zone = selectedZone_ >= 0 ? selectedZone_ : 0;
            m.enabled = false;
            a.monitors.push_back(m);
            selectedMonitor_ = static_cast<int>(a.monitors.size()) - 1;
        }
        for (int i = 0; i < static_cast<int>(a.monitors.size()); ++i) {
            Monitor& m = a.monitors[i];
            ImGui::PushID(1000 + i);
            ImGui::Checkbox("##on", &m.enabled);
            ImGui::SameLine();
            const MonitorRuntime* rt = i < static_cast<int>(monitorRt_.size()) ? &monitorRt_[i] : nullptr;
            const bool sel = (i == selectedMonitor_);
            // Live status chips: armed by its key, currently detecting, run count.
            std::string label = m.name;
            if (rt && rt->switchedOn) label += "  \xE2\x97\x89";  // ◉ armed
            if (rt && rt->present()) label += "  [*]";
            if (rt && rt->runs() > 0) label += i18n::trf("  x%d", rt->runs());
            if (ImGui::Selectable(V(label), sel))
                selectedMonitor_ = (sel ? -1 : i);
            ImGui::PopID();
        }

        title(T("Rule"));
        if (selectedMonitor_ >= 0 && selectedMonitor_ < static_cast<int>(a.monitors.size())) {
            Monitor& m = a.monitors[selectedMonitor_];
            MonitorRuntime* rt = selectedMonitor_ < static_cast<int>(monitorRt_.size()) ? &monitorRt_[selectedMonitor_] : nullptr;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
            ImGui::BeginChild("##mon_insp", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);

            // ---- armed status + run switch ----
            if (rt) {
                const bool on = rt->switchedOn;
                const ImVec4 ruleSt = ImGui::ColorConvertU32ToFloat4(on ? kOk : textCol());
                ImGui::TextColored(ruleSt, "%s", on ? (rt->present() ? T("Armed — target detected")
                                                               : T("Armed — watching"))
                                                : T("Off — press its key or Start to arm"));
                ImGui::SameLine();
                if (ImGui::SmallButton(on ? T("Stop") : T("Start"))) {
                    rt->switchedOn = !rt->switchedOn;
                    if (rt->switchedOn) { rt->reset(); rt->switchedOn = true; }
                }
            }

            char buf[64];
            std::snprintf(buf, sizeof buf, "%s", m.name.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText(T("Name"), buf, sizeof buf)) m.name = buf;

            // ---- WHEN: what to watch ----
            ImGui::SeparatorText(T("When"));
            int src = m.image >= 0 ? 1 : 0;
            const char* srcs[] = {T("a color zone"), T("an image")};
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo(T("Watch for"), src == 1 ? T("an image") : T("a color zone"))) {
                for (int s = 0; s < 2; ++s)
                    if (ImGui::Selectable(srcs[s], s == src)) {
                        if (s == 1) m.zone = -1, m.image = a.images.size() > 0 ? 0 : -1;
                        else m.image = -1, m.zone = a.zones.size() > 0 ? 0 : -1;
                        monitorRt_.clear();
                    }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (m.image >= 0) {
                if (ImGui::BeginCombo(T("Image"), m.image >= 0 && m.image < static_cast<int>(a.images.size()) ? V(a.images[m.image].name)
                                                                                                              : T("None"))) {
                    for (int im = 0; im < static_cast<int>(a.images.size()) && im < kMaxImages; ++im) {
                        ImGui::PushID(im);
                        if (ImGui::Selectable(V(a.images[im].name), im == m.image)) m.image = im, monitorRt_.clear();
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                if (a.images.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), "%s", T("Capture an image in the Images tab first."));
            } else if (ImGui::BeginCombo(T("Zone"), m.zone >= 0 && m.zone < static_cast<int>(a.zones.size()) ? V(a.zones[m.zone].name) : T("None"))) {
                for (int z = 0; z < static_cast<int>(a.zones.size()); ++z) {
                    ImGui::PushID(z);
                    if (ImGui::Selectable(V(a.zones[z].name), z == m.zone)) m.zone = z, monitorRt_.clear();
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (m.image < 0 && a.zones.empty()) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), "%s", T("No zones yet."));
                if (ImGui::SmallButton(T("+ Draw a zone on the game"))) pickMode_ = 2;
            }

            // ---- DO: the action ----
            ImGui::SeparatorText(T("Do"));
            const bool isTap = m.macro >= 0 && m.macro < static_cast<int>(a.macros.size()) &&
                               a.macros[m.macro].steps.size() == 1 && a.macros[m.macro].steps[0].type == StepType::Tap;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo(T("Action"), isTap ? T("Tap a point on screen")
                                                  : m.macro >= 0 && m.macro < static_cast<int>(a.macros.size())
                                                        ? V(i18n::trf("Run %s", a.macros[m.macro].name.c_str()))
                                                        : m.key > 0 ? V(i18n::trf("Press %s", keyName(m.key).c_str()))
                                                                    : T("Choose…"))) {
                if (ImGui::Selectable(T("Press a key"), m.macro < 0 && m.key > 0)) m.macro = -1;
                if (ImGui::Selectable(T("Tap a point on screen"), isTap)) {
                    // One-step tap macro generated under the hood; the user only
                    // sees the point picker below.
                    Macro tap;
                    tap.name = m.name + i18n::tr(" tap");
                    Step s;
                    s.type = StepType::Tap;
                    s.a = {0.5f, 0.5f};
                    tap.steps = {s};
                    a.macros.push_back(std::move(tap));
                    m.macro = static_cast<int>(a.macros.size()) - 1;
                    m.key = 0;
                    monitorRt_.clear();
                }
                for (int k = 0; k < static_cast<int>(a.macros.size()); ++k) {
                    ImGui::PushID(k);
                    if (ImGui::Selectable(V(i18n::trf("Run %s", a.macros[k].name.c_str())), k == m.macro && !isTap)) m.macro = k;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (isTap) {
                Step& tap = a.macros[m.macro].steps[0];
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderFloat("##tapx", &tap.a.x, 0.0f, 1.0f, T("Tap X %.3f"));
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderFloat("##tapy", &tap.a.y, 0.0f, 1.0f, T("Tap Y %.3f"));
                if (ImGui::SmallButton(T("Pick the point on the game"))) pickMode_ = 3, pickTarget_ = &tap.a;
            }
            if (m.macro < 0) {
                const bool waitKey = capture_.active() && capTarget_.slot == 6 && capExternal_ == &m.key;
                if (ImGui::Button(waitKey ? "..." : m.key ? keyName(m.key).c_str() : T("Pick the key to press"), {-FLT_MIN, 0})) {
                    if (waitKey) finishCapture(VK_LBUTTON);
                    else beginExternalCapture(&m.key, 6);
                }
            }
            if (m.macro < 0 && m.key <= 0) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), "%s", T("Choose an action before enabling this rule."));

            // ---- REPEAT ----
            ImGui::SeparatorText(T("Repeat"));
            int mode = static_cast<int>(m.mode);
            const char* modes[] = {T("Once — first time it appears"), T("Keep going while it stays"), T("Each time it comes back"), T("A set number of times")};
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo(T("##mode"), &mode, modes, 4)) m.mode = static_cast<TriggerMode>(mode), monitorRt_.clear();
            if (m.mode == TriggerMode::Limited) intField(T("How many times"), &m.maxRuns, 1, 100000);

            // ---- IF NOT FOUND: absence branch ----
            ImGui::SeparatorText(T("If not found"));
            int elseMode = m.elseStop ? 2 : (m.elseMacro >= 0 || m.elseKey > 0) ? 1 : 0;
            const char* elseModes[] = {T("Keep watching"), T("Run an action"), T("Stop the rule")};
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo(T("##elsemode"), &elseMode, elseModes, 3)) {
                if (elseMode == 0) m.elseStop = false, m.elseMacro = -1, m.elseKey = 0;
                else if (elseMode == 1) m.elseStop = false;
                else m.elseStop = true, m.elseMacro = -1, m.elseKey = 0;
                monitorRt_.clear();
            }
            if (elseMode == 1) {
                intField(T("After missing for ms"), &m.absentMs, 0, 600000);
                intField(T("Repeat every ms"), &m.elseCooldownMs, 50, 600000);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(T("Else action"), m.elseMacro >= 0 && m.elseMacro < static_cast<int>(a.macros.size())
                                                            ? V(i18n::trf("Run %s", a.macros[m.elseMacro].name.c_str()))
                                                            : m.elseKey > 0 ? V(i18n::trf("Press %s", keyName(m.elseKey).c_str()))
                                                                            : T("Choose…"))) {
                    if (ImGui::Selectable(T("Press a key"), m.elseMacro < 0 && m.elseKey > 0)) m.elseMacro = -1;
                    for (int k = 0; k < static_cast<int>(a.macros.size()); ++k) {
                        if (k == m.macro) continue;  // else branch must differ from the main action
                        ImGui::PushID(k);
                        if (ImGui::Selectable(V(i18n::trf("Run %s", a.macros[k].name.c_str())), k == m.elseMacro)) m.elseMacro = k, m.elseKey = 0;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                if (m.elseMacro < 0) {
                    const bool waitEk = capture_.active() && capTarget_.slot == 6 && capExternal_ == &m.elseKey;
                    if (ImGui::Button(waitEk ? "..." : m.elseKey ? keyName(m.elseKey).c_str() : T("Pick the key to press"), {-FLT_MIN, 0})) {
                        if (waitEk) finishCapture(VK_LBUTTON);
                        else beginExternalCapture(&m.elseKey, 6);
                    }
                }
                if (m.elseMacro < 0 && m.elseKey <= 0)
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), "%s", T("Pick what runs while the target stays missing."));
            }
            if (elseMode == 2) intField(T("After missing for ms"), &m.absentMs, 0, 600000);

            // ---- KEY: per-rule hotkey ----
            ImGui::SeparatorText(T("Activation key"));
            const bool waitTg = capture_.active() && capTarget_.slot == 7 && capExternal_ == &m.toggleKey;
            const std::string tgLabel = m.toggleKey > 0 ? keyName(m.toggleKey) : std::string(T("None — runs with the global switch"));
            if (ImGui::Button(V(tgLabel + "##tg"), {-S(70), 0})) {
                if (waitTg) finishCapture(VK_LBUTTON);
                else beginExternalCapture(&m.toggleKey, 7);
            }
            ImGui::SameLine();
            if (m.toggleKey > 0 && ImGui::SmallButton(T("Clear##tg"))) m.toggleKey = 0;
            if (waitTg) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), "%s", T("Press a key…"));

            ImGui::Checkbox(T("Advanced settings"), &monitorAdvOpen_);
            if (monitorAdvOpen_) {
                intField(T("Scan every ms"), &m.intervalMs, 10, 5000);
                intField(T("Confirm samples"), &m.confirm, 1, 20);
                intField(T("Cooldown ms"), &m.cooldownMs, 0, 600000);
                intField(T("Delay ms"), &m.delayMs, 0, 600000);
            }

            if (ImGui::Button(T("Delete rule"), {halfW, 0})) a.monitors.erase(a.monitors.begin() + selectedMonitor_), selectedMonitor_ = -1, monitorRt_.clear();
            ImGui::SameLine();
            if (ImGui::Button(T("Close"), {halfW, 0})) selectedMonitor_ = -1;
            ImGui::EndChild();
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
            ImGui::BeginChild("##no_sel_m", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textCol()), "%s", T("Select an item to view its settings"));
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
    }
}

// ---- standalone object detection (YOLO/ONNX watch) --------------------------

void App::panelDetect() {
    auto& a = profile_.automation;
    DetectWatch& dw = a.detect;
    title(T("Object detection"));

    // Status card: video state, watcher state, model status, live counters.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
    ImGui::BeginChild("##det_status", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
                                                ImGuiChildFlags_Borders);
    const bool video = sessionActive() && renderer_.hasVideo();
    const ImU32 dot = !video ? IM_COL32(120, 120, 130, 255) : detectWatchOn_ ? kOk : kDanger;
    const char* st = !video ? T("Not ready: no video")
                     : detectWatchOn_ ? T("Detection is running")
                                      : T("Detection is stopped");
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled({p.x + S(6), p.y + ImGui::GetTextLineHeight() * 0.5f}, S(5), dot);
    ImGui::SetCursorScreenPos({p.x + S(18), p.y});
    ImGui::TextUnformatted(st);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s: %s", T("Model"), V(detectorStatus()));
    if (detectWatchOn_) {
        // Always rendered while the watcher runs: `ready` flickers between
        // scans and gating on it made the card height oscillate (jitter).
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textCol()), "%s",
                           V(i18n::trf("%d object(s)", (int)detectTracker_.tracks.size())));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textCol()), "%s",
                           V(i18n::trf("Inference %.1f ms", detectWatch_.lastInferUs / 1000.0)));
        // Track lifecycle transitions: Confirmed/lost/reacquire events with
        // their cause — the ghost-cycle signature is visible here live.
        const size_t ne = detectTracker_.events.size();
        for (size_t i = ne > 3 ? ne - 3 : 0; i < ne; ++i) {
            const TrackEvent& e = detectTracker_.events[i];
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "#%u %s→%s (%s)",
                               e.id, trackStateName(e.from), trackStateName(e.to), trackReasonName(e.why));
        }
    }
    if (ImGui::Button(detectWatchOn_ ? T("Stop detection") : T("Start detection"), {-FLT_MIN, S(30)}))
        setDetectWatch(!detectWatchOn_);
    ImGui::SetItemTooltip("%s", T("Runs continuous scans and draws tracked boxes on detected objects."));
    ImGui::Checkbox(T("Start automatically when connected"), &dw.autostart);
    ImGui::SetItemTooltip("%s", T("Starts detection automatically when the device video appears."));
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Hotkeys: independent start/stop so the watcher never needs the panel open.
    title(T("Hotkeys"));
    auto keyBtn = [&](const char* label, int* target, const char* tip) {
        const bool waiting = capture_.active() && capTarget_.slot == 7 && capExternal_ == target;
        const std::string lbl = *target > 0 ? std::string(label) + ": " + keyName(*target)
                                            : std::string(label) + ": " + T("None");
        if (ImGui::SmallButton(waiting ? "..." : V(lbl))) {
            if (waiting) finishCapture(VK_LBUTTON);
            else beginExternalCapture(target, 7);
        }
        ImGui::SetItemTooltip("%s", tip);
        if (*target > 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton((std::string(T("Clear")) + "##" + label).c_str())) *target = 0;
        }
    };
    keyBtn(T("Start key"), &dw.startKey, T("Press from inside the game to start detection — no need to open this panel."));
    keyBtn(T("Stop key"), &dw.stopKey, T("Stops detection. Without it, the start key toggles on/off."));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                       T("Without a stop key, the start key toggles detection on/off."));

    // Slider with its label on the line above so narrow panels never clip it.
    auto labeledSlider = [&](const char* label, const char* id, int* v, int lo, int hi,
                             const char* fmt, const char* tip) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textCol()), "%s", label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool ch = ImGui::SliderInt(id, v, lo, hi, fmt);
        ImGui::SetItemTooltip("%s", tip);
        return ch;
    };

    // Detection parameters.
    title(T("Detection"));
    int conf = dw.confPct;
    if (labeledSlider(T("Min confidence"), "##dconf", &conf, 1, 99, "%d%%",
                      T("Minimum confidence to open a NEW box. Existing boxes keep tracking at about half this value.")))
        dw.confPct = std::clamp(conf, 1, 99);
    // One-click tuning modes: writes the whole rate/tracking/aim/control
    // knob set at once. Detection semantics (confidence, classes, filters,
    // region) are the user's choice and are never touched.
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textCol()), "%s", T("Tuning mode"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* modes[] = {T("Max performance"), T("Balanced"), T("Light")};
        static int modeSel = 1;
        if (ImGui::BeginCombo("##dmode", modes[modeSel])) {
            for (int i = 0; i < 3; ++i)
                if (ImGui::Selectable(modes[i], modeSel == i)) {
                    modeSel = i;
                    switch (i) {
                        case 0:  // Max performance — fastest loop, snappiest pull
                            dw.inferFps = 60;  dw.trackPosBlendPct = 70; dw.trackVelPct = 60;
                            dw.trackPredictMs = 300; dw.trackMaxMissed = 1; dw.trackMaxAgeMs = 400;
                            dw.trackLowPct = 45; dw.lockGraceMs = 120;
                            dw.aimSpeedPct = 70; dw.ctrlMaxRateHz = 300; dw.ctrlMaxOut = 4000;
                            dw.ctrlMaxAgeMs = 300; dw.ctrlDeadzonePx = 0; dw.ctrlInnerPct = 22;
                            break;
                        case 2:  // Light — weak hardware: calm, cheap, smooth
                            dw.inferFps = 15;  dw.trackPosBlendPct = 45; dw.trackVelPct = 35;
                            dw.trackPredictMs = 250; dw.trackMaxMissed = 3; dw.trackMaxAgeMs = 800;
                            dw.trackLowPct = 60; dw.lockGraceMs = 250;
                            dw.aimSpeedPct = 35; dw.ctrlMaxRateHz = 120; dw.ctrlMaxOut = 2000;
                            dw.ctrlMaxAgeMs = 600; dw.ctrlDeadzonePx = 2; dw.ctrlInnerPct = 22;
                            break;
                        default: // Balanced — Auto rate + shipped defaults
                            dw.inferFps = 0;   dw.trackPosBlendPct = 55; dw.trackVelPct = 45;
                            dw.trackPredictMs = 350; dw.trackMaxMissed = 2; dw.trackMaxAgeMs = 600;
                            dw.trackLowPct = 55; dw.lockGraceMs = 150;
                            dw.aimSpeedPct = 50; dw.ctrlMaxRateHz = 200; dw.ctrlMaxOut = 3000;
                            dw.ctrlMaxAgeMs = 400; dw.ctrlDeadzonePx = 1; dw.ctrlInnerPct = 22;
                            break;
                    }
                }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%s", T("Applies a full tuning preset: analysis rate + tracking response + pull strength + control limits. Max performance ignores CPU load; Light is for weak hardware. Your confidence/classes/filters stay untouched."));
    }

    // Analysis rate: fixed presets or Auto (adapts to measured inference p95).
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textCol()), "%s", T("Analysis rate"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* rateNames[] = {"Auto", "15 fps", "20 fps", "30 fps", "45 fps", "60 fps"};
        const int rateVals[] = {0, 15, 20, 30, 45, 60};
        int cur = 0;
        for (int i = 0; i < 6; ++i) if (rateVals[i] == dw.inferFps) cur = i;
        if (ImGui::BeginCombo("##dfps", rateNames[cur])) {
            for (int i = 0; i < 6; ++i)
                if (ImGui::Selectable(rateNames[i], cur == i)) dw.inferFps = rateVals[i];
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%s", T("Detection analysis rate. Auto adapts to measured inference time — a fast model analyzes more frames, a slow one fewer, without queue build-up."));
        ImGui::Checkbox(V(std::string(T("Merge character parts")) + "##fuse"), &dw.fusionOn);
        ImGui::SetItemTooltip("%s", T("Part boxes contained inside a character box merge into one record — a face inside a body never counts as a second target."));
    }

    // Class filter: empty list = all classes. Names come from the model itself.
    const std::vector<std::string> names = detectorClassNames();
    if (!names.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s", T("Classes (none checked = all):"));
        auto has = [&](const std::string& n) {
            return dw.classes.empty() ||
                   std::find(dw.classes.begin(), dw.classes.end(), n) != dw.classes.end();
        };
        for (const std::string& n : names) {
            bool on = has(n);
            if (ImGui::Checkbox((n + "##dc").c_str(), &on)) {
                if (dw.classes.empty()) dw.classes = names;  // "all" -> materialize
                if (on) {
                    if (std::find(dw.classes.begin(), dw.classes.end(), n) == dw.classes.end())
                        dw.classes.push_back(n);
                    if (dw.classes.size() == names.size()) dw.classes.clear();  // all = empty
                } else {
                    dw.classes.erase(std::remove(dw.classes.begin(), dw.classes.end(), n), dw.classes.end());
                }
            }
            ImGui::SetItemTooltip("%s", T("Check the classes that get boxes. Unchecking everything accepts all classes."));
        }
    }

    const float halfW = (availW() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    // Tracking: how boxes behave between scans — all live-tunable.
    title(T("Tracking"));
    {
        int low = dw.trackLowPct;
        if (labeledSlider(T("Weak-hit floor"), "##dlow", &low, 10, 95, "%d%%",
                          T("Weak detections at this % of min confidence keep an existing box alive (motion blur dips). Lower = box survives more misses.")))
            dw.trackLowPct = std::clamp(low, 10, 95);
        int pos = dw.trackPosBlendPct;
        if (labeledSlider(T("Box responsiveness"), "##dpos", &pos, 10, 100, "%d%%",
                          T("How much the box snaps to each new detection. Higher = faster response; lower = smoother, steadier boxes.")))
            dw.trackPosBlendPct = std::clamp(pos, 10, 100);
        int vel = dw.trackVelPct;
        if (labeledSlider(T("Motion response"), "##dvel", &vel, 0, 100, "%d%%",
                          T("How fast the box's glide reacts to camera/target movement. Higher = follows pans sooner.")))
            dw.trackVelPct = std::clamp(vel, 0, 100);
        int pred = dw.trackPredictMs;
        if (labeledSlider(T("Glide limit"), "##dpre", &pred, 0, 2000, "%d ms",
                          T("Max time the box glides on its last known motion between scans. Higher = keeps up with fast pans; too high may overshoot.")))
            dw.trackPredictMs = std::clamp(pred, 0, 2000);
        int miss = dw.trackMaxMissed;
        if (labeledSlider(T("Grace scans"), "##dmis", &miss, 0, 10, "%d",
                          T("How many consecutive scans a box may miss before it disappears. Higher = survives more blur, slower to vanish.")))
            dw.trackMaxMissed = std::clamp(miss, 0, 10);
        int age = dw.trackMaxAgeMs;
        if (labeledSlider(T("Max box age"), "##dage", &age, 50, 5000, "%d ms",
                          T("Absolute lifetime of a box with no detection. Caps how long a lost target keeps its box.")))
            dw.trackMaxAgeMs = std::clamp(age, 50, 5000);
        int gr = dw.lockGraceMs;
        if (labeledSlider(T("Lock grace"), "##dgr", &gr, 0, 2000, "%d ms",
                          T("How long an aim lock survives a full detection gap (motion blur, occlusion) before the identity is released and a new target may be picked.")))
            dw.lockGraceMs = std::clamp(gr, 0, 2000);
    }

    // Aim assist: hold a key to steer the camera toward the locked enemy.
    title(T("Aim assist"));
    ImGui::Checkbox(V(std::string(T("Aim assist")) + "##on"), &dw.aimOn);
    ImGui::SetItemTooltip("%s", T("While its key is held, the camera glides toward the enemy nearest the crosshair. Works while mouse look is locked."));
    if (dw.aimOn) {
        keyBtn(T("Aim key"), &dw.aimKey,
               T("Hold to pull the aim toward the locked target; releasing reacquires the best enemy next hold."));
        int spd = dw.aimSpeedPct;
        if (labeledSlider(T("Aim speed"), "##daspd", &spd, 10, 100, "%d%%",
                          T("How strongly each correction pulls. Higher = snaps faster but may wobble past the target; lower = smoother and calmer.")))
            dw.aimSpeedPct = std::clamp(spd, 10, 100);
        int rng = dw.aimRangePct;
        if (labeledSlider(T("Engage range"), "##darng", &rng, 5, 100, "%d%%",
                          T("Only enemies within this distance from the crosshair get locked — prevents wild snaps across the screen.")))
            dw.aimRangePct = std::clamp(rng, 5, 100);
        int lead = dw.aimLeadMs;
        if (labeledSlider(T("Target lead"), "##dald", &lead, 0, 500, "%d ms",
                          T("Aims where a moving enemy WILL BE after this time, not where it is — useful for strafing targets.")))
            dw.aimLeadMs = std::clamp(lead, 0, 500);

        // Target selection (used for NEW lock acquisition only — a locked
        // target is never replaced by a slightly-better challenger).
        title(T("Target selection"));
        {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textCol()), "%s", T("Selection preset"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            const char* pres[] = {T("Center priority"), T("Size priority"),
                                  T("Confidence priority"), T("Balanced"), T("Custom")};
            int sp = std::clamp(dw.selPreset, 0, 4);
            if (ImGui::BeginCombo("##dsel", pres[sp])) {
                for (int i = 0; i < 5; ++i)
                    if (ImGui::Selectable(pres[i], sp == i)) dw.selPreset = i;
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("%s", T("How a NEW target is chosen when nothing is locked. A locked target is never replaced by a slightly-better one."));
            if (dw.selPreset == 4) {
                intField(T("Center weight"), &dw.selCenterW, 0, 200);
                intField(T("Size weight"), &dw.selSizeW, 0, 200);
                intField(T("Confidence weight"), &dw.selConfW, 0, 200);
            }
            int hy = dw.switchHystPct;
            if (labeledSlider(T("Switch hysteresis"), "##dhys", &hy, 0, 100, "%d%%",
                              T("A challenger must beat the current pick by this margin before any switch is even considered.")))
                dw.switchHystPct = std::clamp(hy, 0, 100);
        }
        // Reference point inside the character box.
        title(T("Target point"));
        {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textCol()), "%s", T("Point"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            const char* pts[] = {T("Center"), T("Head"), T("Chest"), T("Upper body"), T("Custom")};
            int pp = std::clamp(dw.pointPreset, 0, 4);
            if (ImGui::BeginCombo("##dpt", pts[pp])) {
                for (int i = 0; i < 5; ++i)
                    if (ImGui::Selectable(pts[i], pp == i)) dw.pointPreset = i;
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("%s", T("Where inside the character box the crosshair is pulled: a relative point that scales with the box size."));
            if (dw.pointPreset == 4) {
                intField(T("Point X %"), &dw.pointX, 0, 100);
                intField(T("Point Y %"), &dw.pointY, 0, 100);
            }
        }
        // Controller limits.
        title(T("Control"));
        {
            int dz = dw.ctrlDeadzonePx;
            if (labeledSlider(T("Dead zone"), "##ddz", &dz, 0, 50, "%d px",
                              T("Extra stop margin around the reference point — tiny corrections are suppressed.")))
                dw.ctrlDeadzonePx = std::clamp(dz, 0, 50);
            int iz = dw.ctrlInnerPct;
            if (labeledSlider(T("Inner zone"), "##diz", &iz, 0, 45, "%d%%",
                              T("Stop zone inside the character box: the pull settles only when the crosshair reaches this inset area (the real hit zone) — not just the box edge. 0 = whole box.")))
                dw.ctrlInnerPct = std::clamp(iz, 0, 45);
            int mo = dw.ctrlMaxOut;
            if (labeledSlider(T("Max pull speed"), "##dmo", &mo, 100, 20000, "%d /s",
                              T("Physical speed cap on the pull — emitted counts per second. The controller corrects by frame-pixels and converts through the calibrated plant gain, so this bound is a real rate, not a per-tick step.")))
                dw.ctrlMaxOut = std::clamp(mo, 100, 20000);
            int ma = dw.ctrlMaxAgeMs;
            if (labeledSlider(T("Max data age"), "##dma", &ma, 0, 2000, "%d ms",
                              T("Measurements older than this are refused — the pull never acts on stale data.")))
                dw.ctrlMaxAgeMs = std::clamp(ma, 0, 2000);
            int mc = dw.ctrlMinConfPct;
            if (labeledSlider(T("Min hit confidence"), "##dmc", &mc, 0, 100, "%d%%",
                              T("Measurements weaker than this never move the camera.")))
                dw.ctrlMinConfPct = std::clamp(mc, 0, 100);
        }
    }

    // Filters: drop own character and teammates before drawing.
    title(T("Filtering"));
    ImGui::Checkbox(T("Ignore my character"), &dw.exclOn);
    ImGui::SetItemTooltip("%s", T("Ignores detections whose center falls inside the exclusion zone (your own character)."));
    if (dw.exclOn) {
        if (ImGui::Button(T("Drag exclusion zone"), {halfW, 0})) {
            pickMode_ = 6;
            pickRectTarget_ = &dw.excl;
        }
        ImGui::SetItemTooltip("%s", T("Draw a rectangle around your character — detections inside it are ignored."));
        ImGui::SameLine();
        ImGui::Checkbox(V(std::string(T("Show zone")) + "##ex"), &dw.showExcl);
        ImGui::SetItemTooltip("%s", T("Draw the exclusion zone outline on the video."));
    }
    ImGui::Checkbox(T("Hide teammates (colored marker)"), &dw.mateOn);
    ImGui::SetItemTooltip("%s", T("Removes detections with a colored squad marker (1-4) above the head."));
    if (dw.mateOn) {
        int sat = dw.mateSat;
        if (labeledSlider(T("Marker saturation"), "##dsat", &sat, 20, 160, "%d",
                          T("How vivid the marker color must be. Higher = only strong marker colors count.")))
            dw.mateSat = std::clamp(sat, 20, 160);
        int mp = dw.mateMinPct;
        if (labeledSlider(T("Marker coverage"), "##dcov", &mp, 1, 50, "%d%%",
                          T("Percent of colored pixels needed above the box to drop it as a teammate.")))
            dw.mateMinPct = std::clamp(mp, 1, 50);
    }
    ImGui::Checkbox(T("Red outline boost"), &dw.redOn);
    ImGui::SetItemTooltip("%s", T("Weak detections with a red enemy outline get a confidence boost — drawn as amber boxes."));
    if (dw.redOn) {
        int rm = dw.redMinPct;
        if (labeledSlider(T("Red edge"), "##dred", &rm, 1, 30, "%d%%",
                          T("Percent of red pixels required on the box edge for the rescue.")))
            dw.redMinPct = std::clamp(rm, 1, 30);
        int rb = dw.redBoostPct;
        if (labeledSlider(T("Boost"), "##dbst", &rb, 5, 50, "+%d%%",
                          T("Confidence bonus added when the box edge is red.")))
            dw.redBoostPct = std::clamp(rb, 5, 50);
    }

    // Search region.
    title(T("Search region"));
    if (ImGui::Button(T("Drag on the game"), {halfW, 0})) {
        pickMode_ = 6;
        pickRectTarget_ = &dw.region;
    }
    ImGui::SetItemTooltip("%s", T("Restrict scanning to a drawn region — faster and fewer false positives."));
    ImGui::SameLine();
    if (ImGui::Button(T("Whole screen"), {halfW, 0})) dw.region = Rect{0.0f, 0.0f, 1.0f, 1.0f};
    ImGui::SetItemTooltip("%s", T("Scan the entire video frame."));
    ImGui::Checkbox(T("Show region frame"), &dw.showRegion);
    ImGui::SetItemTooltip("%s", T("Draw the search region outline."));

    // Overlay options.
    title(T("Overlay"));
    ImGui::Checkbox(T("Draw boxes"), &dw.drawBoxes);
    ImGui::SetItemTooltip("%s", T("Draw rectangles on detected objects."));
    ImGui::Checkbox(T("Draw labels"), &dw.drawLabels);
    ImGui::SetItemTooltip("%s", T("Show class name and confidence above each box."));

    // Debug overlay (testing only — hidden in normal play).
    title(T("Debug"));
    ImGui::Checkbox(T("Raw detections"), &dw.dbgRaw);
    ImGui::SetItemTooltip("%s", T("Draw the un-fused, un-smoothed model boxes for comparison."));
    ImGui::Checkbox(T("Track IDs"), &dw.dbgIds);
    ImGui::SetItemTooltip("%s", T("Draw each box's track identity and state."));
    ImGui::Checkbox(T("Perf stats"), &dw.dbgPerf);
    ImGui::SetItemTooltip("%s", T("Overlay inference fps, frame age and pipeline latency."));
    ImGui::Checkbox(T("Record session"), &dw.dbgRecord);
    ImGui::SetItemTooltip("%s", T("Write a bounded JSONL diagnostics log (timestamps, detections, lock events) to vision_log.jsonl — no screenshots."));

    // Pinned action bar: explicit Save applies changes and closes the panel.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg());
    ImGui::BeginChild("##det_bar", {0, 0},
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    const bool dirty = hasUnsavedChanges();
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float bw = (availW() - sp) * 0.5f;
    if (dirty) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
    if (ImGui::Button(T("Save"), {bw, S(28)}) && saveProfile()) settings_.panelCollapsed = true;
    if (dirty) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("%s", T("Save all changes and close this panel."));
    ImGui::SameLine(0, sp);
    if (ImGui::Button(T("Cancel"), {bw, S(28)})) discardChanges();
    ImGui::SetItemTooltip("%s", T("Discard changes made since the last save."));
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::panelAutomation() { panelMacros(); }

}  // namespace wasd::app
