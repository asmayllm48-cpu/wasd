// ImGui user interface: the key-mapping editor drawn over the game viewport and the
// collapsible side panel (key mapping, device, video, performance, logs, help).
#include <algorithm>
#include <numeric>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <dwmapi.h>

#include "app/app.h"
#include "app/i18n.h"
#include "app/studio_ui.h"
#include "app/text_shaping.h"
#include "core/log.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "imgui_internal.h"

namespace wasd::app {
namespace {

#define ICON_HOME "\uE80F"
#define ICON_ADD "\uE710"
#define ICON_DELETE "\uE74D"
#define ICON_COPY "\uE8C8"
#define ICON_SAVE "\uE74E"
#define ICON_SAVEAS "\uE792"
#define ICON_UNDO "\uE7A7"
#define ICON_REDO "\uE7A6"
#define ICON_RESET "\uE72C"
#define ICON_EDIT "\uE70F"
#define ICON_RENAME "\uE8AC"
#define ICON_KEYBOARD "\uE765"
#define ICON_PHONE "\uE8EA"
#define ICON_VIDEO "\uE714"
#define ICON_PERF "\uE9D9"
#define ICON_LOGS "\uE8FD"
#define ICON_HELP "\uE897"
#define ICON_GLOBE "\uE774"
#define ICON_CHEV_LEFT "\uE76B"
#define ICON_CHEV_RIGHT "\uE76C"
#define ICON_LOCK "\uE72E"
#define ICON_CLOSE "\uE711"
#define ICON_CHECK "\uE73E"
#define ICON_WARNING "E7BA"
#define ICON_TAP "\uE7C9"
#define ICON_GAME "\uE7FC"
#define ICON_EYE "\uE890"
#define ICON_SWIPE "\uE72A"
#define ICON_PAGE "\uE7C3"
#define ICON_CONNECT "\uE703"
#define ICON_MACRO "\uE945"
#define ICON_IMAGE "\uEB9F"
#define ICON_COLOR "\uE790"
#define ICON_SETTINGS "\uE713"
#define ICON_SUN "\uE706"
#define ICON_MOON "\uE708"
#define ICON_PLAY "\uE768"
#define ICON_STOP "\uE71A"
#define ICON_FIRE "\uE8B0"

// NEXPLAY official palette (switched dynamically between Dark and Light themes)
ImU32 kBgPrimary   = IM_COL32(30, 30, 30, 255);    // #0B1220
ImU32 kBgSecondary = IM_COL32(37, 37, 38, 255);    // #131F33
ImU32 kSurfaceElev = IM_COL32(45, 45, 48, 255);    // #1E293B
ImU32 kAccent      = IM_COL32(59, 130, 246, 255);  // #3B82F6 (Primary Accent)
ImU32 kAccentCyan  = IM_COL32(34, 211, 238, 255);  // #22D3EE (Secondary Accent)
ImU32 kAmber       = IM_COL32(245, 158, 11, 255);  // Warning
ImU32 kDanger      = IM_COL32(239, 68, 68, 255);   // Error
ImU32 kOk          = IM_COL32(34, 197, 94, 255);   // Success
ImU32 kMuted       = IM_COL32(148, 163, 184, 255); // #94A3B8 (Secondary Text)
ImU32 kText        = IM_COL32(248, 250, 252, 255); // #F8FAFC (Primary Text)
ImU32 kRailBg      = IM_COL32(27, 27, 27, 255);     // Deep navy sidebar
ImU32 kBorderCol   = IM_COL32(60, 60, 60, 255);    // #2B3B52 (Borders)
ImU32 kLine        = IM_COL32(60, 60, 60, 180);
bool g_darkTheme   = true;

float g_scale = 1.0f;
std::vector<Rect>* g_captureRects = nullptr;
ImFontLoader g_shapedLoader;

float S(float v) { return v * g_scale; }
bool rtl() { return i18n::rtl(); }
ImU32 alpha(ImU32 c, float a) { return (c & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24); }
float pulse() { return 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f); }

// Visual (shaped, bidi-ordered) strings for drawing.
const char* V(const std::string& logical) { return text::visual(logical).c_str(); }
const char* T(const char* en) { return text::visual(i18n::tr(en)).c_str(); }
template <class... A>
const char* TF(const char* en, A... a) {
    return V(i18n::trf(en, a...));
}

int shapedGlyphIndex(ImFontAtlas* atlas, ImFontConfig* src, ImWchar cp) {
    if (cp >= text::kSlotFirst && cp <= text::kSlotLast) return text::glyphForSlot(cp);
    return ImFontAtlasGetFontLoaderForStbTruetype()->FontSrcGetGlyphIndexFromCodepoint(atlas, src, cp);
}

void addCaptureRect(ImVec2 a, ImVec2 b) {
    if (g_captureRects) g_captureRects->push_back({a.x, a.y, b.x - a.x, b.y - a.y});
}

const char* typeIcon(BindingType t) {
    switch (t) {
        case BindingType::Tap: return ICON_TAP;
        case BindingType::Joystick: return ICON_GAME;
        case BindingType::Camera: return ICON_EYE;
        case BindingType::Drag: return ICON_SWIPE;
        case BindingType::Fire: return ICON_FIRE;
    }
    return "";
}
const char* typeName(BindingType t) {
    switch (t) {
        case BindingType::Tap: return "Tap";
        case BindingType::Joystick: return "Joystick";
        case BindingType::Camera: return "Mouse look";
        case BindingType::Drag: return "Swipe";
        case BindingType::Fire: return "Smart Fire";
    }
    return "";
}
std::string keyText(int vk) { return vk ? keyName(vk) : std::string("?"); }
std::string bindingKeyText(const Binding& b) {
    if (b.key2 != 0) return keyText(b.key) + "+" + keyText(b.key2);
    return keyText(b.key);
}
std::string keySummary(const Binding& b) {
    switch (b.type) {
        case BindingType::Joystick:
            return keyText(b.keyUp) + " " + keyText(b.keyLeft) + " " + keyText(b.keyDown) + " " + keyText(b.keyRight);
        case BindingType::Camera: return i18n::tr("Mouse");
        default: return bindingKeyText(b);
    }
}

// Icon + label in reading order: the icon leads on the side the text starts from.
std::string iconLabel(const char* icon, const char* vis) {
    if (!icon) return vis;
    if (!vis || !*vis) return icon;
    return rtl() ? std::string(vis) + "  " + icon : std::string(icon) + "  " + vis;
}

float availW() { return ImGui::GetContentRegionAvail().x; }

void alignedText(const char* vis, ImU32 col = 0) {
    const float w = ImGui::CalcTextSize(vis).x;
    if (rtl()) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, availW() - w));
    if (col) ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(vis);
    if (col) ImGui::PopStyleColor();
}

// Word-wrapped paragraph; each line is reordered/shaped as a whole and aligned to the UI direction.
void paragraph(const std::string& logical, ImU32 col = 0) {
    const float maxW = availW(), spaceW = ImGui::CalcTextSize(" ").x;
    size_t p = 0;
    while (p <= logical.size()) {
        size_t nl = logical.find('\n', p);
        if (nl == std::string::npos) nl = logical.size();
        const std::string_view para(logical.data() + p, nl - p);
        if (para.empty()) ImGui::Dummy({0, ImGui::GetTextLineHeight() * 0.4f});
        std::string line;
        float lw = 0;
        size_t q = 0;
        while (q < para.size()) {
            size_t sp = para.find(' ', q);
            if (sp == std::string_view::npos) sp = para.size();
            const std::string word(para.substr(q, sp - q));
            const float ww = ImGui::CalcTextSize(V(word)).x;
            if (!line.empty() && lw + spaceW + ww > maxW) alignedText(V(line), col), line.clear(), lw = 0;
            if (!line.empty()) line += ' ', lw += spaceW;
            line += word;
            lw += ww;
            q = sp + 1;
        }
        if (!line.empty()) alignedText(V(line), col);
        p = nl + 1;
    }
}

void sectionTitle(const char* vis) {
    ImGui::Dummy({0, S(6)});
    ImGui::PushFont(nullptr, 12.5f);
    alignedText(vis, kMuted);
    ImGui::PopFont();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p, {p.x + availW(), p.y}, kLine);
    ImGui::Dummy({0, S(4)});
}

// Label on the reading side, value on the opposite side.
void labelValue(const char* labelVis, const char* valueVis, ImU32 labelCol = kMuted, ImU32 valueCol = kText) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = availW(), lh = ImGui::GetTextLineHeight();
    const float lw = ImGui::CalcTextSize(labelVis).x, vw = ImGui::CalcTextSize(valueVis).x;
    const float lx = rtl() ? p.x + w - lw : p.x, vx = rtl() ? p.x : p.x + w - vw;
    dl->AddText({lx, p.y}, labelCol, labelVis);
    dl->AddText({vx, p.y}, valueCol, valueVis);
    ImGui::Dummy({w, lh});
}

// WASD+ style: a real checkbox on the reading side followed by its label.
bool checkRow(const char* vis, bool* v) {
    ImGui::PushID(vis);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = availW(), h = ImGui::GetFrameHeight();
    const bool clicked = ImGui::InvisibleButton("##chk", {w, h});
    if (clicked) *v = !*v;
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float box = S(16), bx = rtl() ? p.x + w - box : p.x, by = p.y + (h - box) * 0.5f;
    dl->AddRectFilled({bx, by}, {bx + box, by + box},
                      *v ? kAccent : (g_darkTheme ? kSurfaceElev : IM_COL32(255, 255, 255, 255)), S(3));
    dl->AddRect({bx, by}, {bx + box, by + box}, (*v || hov) ? kAccent : kBorderCol, S(3));
    if (*v) {
        const ImVec2 ts = ImGui::CalcTextSize(ICON_CHECK);
        dl->AddText({bx + (box - ts.x) * 0.5f - S(1), by + (box - ts.y) * 0.5f}, IM_COL32_WHITE, ICON_CHECK);
    }
    const float lh = ImGui::GetTextLineHeight();
    dl->AddText({rtl() ? bx - S(10) - ImGui::CalcTextSize(vis).x : bx + box + S(10), p.y + (h - lh) * 0.5f}, kText, vis);
    ImGui::PopID();
    return clicked;
}

// WASD+ style row: label on the reading side, dropdown pinned to the other side.
bool comboRow(const char* labelVis, const char* previewVis, const char* const* itemsVis, int n, int* sel, float w) {
    ImGui::PushID(labelVis);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float aw = availW(), h = ImGui::GetFrameHeight(), lh = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddText({rtl() ? p.x + aw - ImGui::CalcTextSize(labelVis).x : p.x, p.y + (h - lh) * 0.5f},
                                        kText, labelVis);
    ImGui::SetCursorScreenPos({rtl() ? p.x : p.x + aw - w, p.y});
    ImGui::SetNextItemWidth(w);
    bool ch = false;
    if (ImGui::BeginCombo("##combo", previewVis)) {
        for (int i = 0; i < n; ++i)
            if (ImGui::Selectable(itemsVis[i], *sel == i)) *sel = i, ch = true;
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return ch;
}

bool toggleRow(const char* vis, bool* v) {
    ImGui::PushID(vis);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = availW(), h = ImGui::GetFrameHeight();
    const bool clicked = ImGui::InvisibleButton("##t", {w, h});
    if (clicked) *v = !*v;
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float sw = S(36), sh = S(20), sx = rtl() ? p.x : p.x + w - sw, sy = p.y + (h - sh) * 0.5f;
    const ImU32 offBg = g_darkTheme ? (hov ? IM_COL32(51, 65, 85, 255) : IM_COL32(38, 51, 71, 255))
                                    : (hov ? IM_COL32(180, 192, 210, 255) : IM_COL32(203, 213, 225, 255));
    const ImU32 track = *v ? kAccent : offBg;
    dl->AddRectFilled({sx, sy}, {sx + sw, sy + sh}, track, sh * 0.5f);
    const bool knobFar = *v != rtl();
    dl->AddCircleFilled({knobFar ? sx + sw - sh * 0.5f : sx + sh * 0.5f, sy + sh * 0.5f}, sh * 0.5f - S(3), IM_COL32_WHITE);
    const float tw = ImGui::CalcTextSize(vis).x;
    dl->AddText({rtl() ? p.x + w - tw : p.x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f}, kText, vis);
    ImGui::PopID();
    return clicked;
}

template <class F>
bool sliderRow(const char* vis, const char* valueVis, F&& slider) {
    ImGui::PushID(vis);
    labelValue(vis, valueVis, kText, kAccentCyan);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool ch = slider();
    ImGui::PopID();
    return ch;
}

// Segmented slider: a horizontal track with discrete snap stops and a draggable
// indicator - the WASD+ quality/bitrate control. Click or drag anywhere and the
// handle snaps to the nearest stop. `valueVis` shows the current choice on the
// label row; stop positions mirror in RTL.
bool segSlider(const char* vis, const char* valueVis, int count, int* index) {
    ImGui::PushID(vis);
    labelValue(vis, valueVis, kText, kAccentCyan);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = availW(), h = S(30), cy = p.y + h * 0.5f;
    ImGui::InvisibleButton("##seg", {w, h});
    bool changed = false;
    const auto xOf = [&](int i) {
        float t = count > 1 ? static_cast<float>(i) / (count - 1) : 0.0f;
        if (rtl()) t = 1.0f - t;
        return p.x + t * w;
    };
    if (ImGui::IsItemActivated() || (ImGui::IsItemActive() && ImGui::GetIO().MouseDown[0])) {
        float t = std::clamp((ImGui::GetIO().MousePos.x - p.x) / w, 0.0f, 1.0f);
        if (rtl()) t = 1.0f - t;
        const int ni = std::clamp(static_cast<int>(std::lround(t * (count - 1))), 0, count - 1);
        if (ni != *index) { *index = ni; changed = true; }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine({p.x, cy}, {p.x + w, cy}, kBorderCol, S(1.5f));
    for (int i = 0; i < count; ++i) {
        const float x = xOf(i);
        dl->AddLine({x, cy - S(4)}, {x, cy + S(4)},
                    i == *index ? kAccent : alpha(kBorderCol, 1.0f), S(1.5f));
    }
    const float hx = xOf(*index);
    const ImU32 hov = ImGui::IsItemHovered() || ImGui::IsItemActive() ? kAccentCyan : kAccent;
    dl->AddCircleFilled({hx, cy}, S(7), hov);
    dl->AddCircle({hx, cy}, S(7), kText, 0, S(1));
    ImGui::PopID();
    return changed;
}

// Numeric field: label+value row, then [-] slider [+] [typed input]. The +/- steppers
// repeat while held; Shift shrinks the step 10x for fine tuning; typing an exact
// value always works. A wider input than the old 62px field keeps units visible.
bool sliderNumF(const char* vis, float* v, float vmin, float vmax, const char* fmt, float step,
                ImGuiSliderFlags flags = ImGuiSliderFlags_None) {
    ImGui::PushID(vis);
    char vv[32];
    std::snprintf(vv, sizeof vv, fmt, static_cast<double>(*v));
    labelValue(vis, vv, kText, kAccentCyan);
    const float bw = ImGui::GetFrameHeight(), sp = ImGui::GetStyle().ItemSpacing.x, iw = S(78);
    if (step <= 0.0f) step = (vmax - vmin) * 0.01f;
    const float adj = step * (ImGui::GetIO().KeyShift ? 0.1f : 1.0f);
    bool ch = false;
    ImGui::PushButtonRepeat(true);
    if (ImGui::SmallButton("-")) { *v = std::clamp(*v - adj, vmin, vmax); ch = true; }
    ImGui::SameLine(0, sp);
    ImGui::SetNextItemWidth(std::max(S(30), availW() - bw - iw - sp * 2.0f));
    ch |= ImGui::SliderFloat("##s", v, vmin, vmax, "", flags);
    ImGui::SameLine(0, sp);
    if (ImGui::SmallButton("+")) { *v = std::clamp(*v + adj, vmin, vmax); ch = true; }
    ImGui::PopButtonRepeat();
    ImGui::SameLine(0, sp);
    ImGui::SetNextItemWidth(iw);
    if (ImGui::InputFloat("##n", v, 0.0f, 0.0f, fmt)) { *v = std::clamp(*v, vmin, vmax); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", T("Type a value, or hold Shift while stepping for fine tuning"));
    ImGui::PopID();
    return ch;
}

bool sliderNumI(const char* vis, int* v, int vmin, int vmax, const char* fmt, int step) {
    ImGui::PushID(vis);
    char vv[32];
    std::snprintf(vv, sizeof vv, fmt, *v);
    labelValue(vis, vv, kText, kAccentCyan);
    const float bw = ImGui::GetFrameHeight(), sp = ImGui::GetStyle().ItemSpacing.x, iw = S(78);
    if (step <= 0) step = 1;
    const int adj = ImGui::GetIO().KeyShift ? 1 : step;  // Shift = unit step for fine tuning
    bool ch = false;
    ImGui::PushButtonRepeat(true);
    if (ImGui::SmallButton("-")) { *v = std::clamp(*v - adj, vmin, vmax); ch = true; }
    ImGui::SameLine(0, sp);
    ImGui::SetNextItemWidth(std::max(S(30), availW() - bw - iw - sp * 2.0f));
    ch |= ImGui::SliderInt("##s", v, vmin, vmax, "");
    ImGui::SameLine(0, sp);
    if (ImGui::SmallButton("+")) { *v = std::clamp(*v + adj, vmin, vmax); ch = true; }
    ImGui::PopButtonRepeat();
    ImGui::SameLine(0, sp);
    ImGui::SetNextItemWidth(iw);
    if (ImGui::InputInt("##n", v, 0)) { *v = std::clamp(*v, vmin, vmax); ch = true; }
    ImGui::PopID();
    return ch;
}

// Draws the official NEXPLAY geometric 'N' badge inside a square box at `p` of side `sz`.
void drawNexplayLogo(ImDrawList* dl, ImVec2 p, float sz) {
    const float r = sz * 0.24f;
    dl->AddRectFilled(p, {p.x + sz, p.y + sz}, IM_COL32(30, 30, 30, 255), r);
    dl->AddRect(p, {p.x + sz, p.y + sz}, alpha(kAccent, 0.7f), r, 0, std::max(1.0f, sz * 0.04f));
    const float lx = p.x + sz * 0.23f, rx = p.x + sz * 0.77f;
    const float ty = p.y + sz * 0.21f, by = p.y + sz * 0.79f;
    const float sw = std::max(2.0f, sz * 0.145f);
    dl->AddRectFilled({lx, ty}, {lx + sw, by}, IM_COL32(59, 130, 246, 255), sw * 0.2f);
    dl->AddRectFilled({rx - sw, ty}, {rx, by}, IM_COL32(34, 211, 238, 255), sw * 0.2f);
    const ImVec2 diag[4] = {{lx, ty}, {lx + sw * 1.05f, ty}, {rx, by}, {rx - sw * 1.05f, by}};
    dl->AddConvexPolyFilled(diag, 4, IM_COL32(46, 172, 242, 255));
}

struct Btn {
    const char* icon = nullptr;
    const char* label = nullptr;  // visual text
    bool enabled = true;
    bool primary = false;
    bool danger = false;
};

bool button(const Btn& b, float w, float h = 0) {
    ImGui::BeginDisabled(!b.enabled);
    int pushed = 0;
    if (b.primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(59, 130, 246, 255));        // #3B82F6
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(37, 99, 235, 255));  // #2563EB
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(29, 78, 216, 255));   // #1D4ED8
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
        pushed = 4;
    } else if (b.danger) {
        ImGui::PushStyleColor(ImGuiCol_Button, g_darkTheme ? IM_COL32(127, 29, 29, 220) : IM_COL32(254, 226, 226, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(220, 38, 38, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(185, 28, 28, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, g_darkTheme ? IM_COL32(254, 202, 202, 255) : IM_COL32(185, 28, 28, 255));
        pushed = 4;
    }
    const bool r = ImGui::Button(iconLabel(b.icon, b.label).c_str(), {w, h > 0 ? h : ImGui::GetFrameHeight() + S(2)});
    ImGui::PopStyleColor(pushed);
    ImGui::EndDisabled();
    return r;
}

// Responsive flow button group: automatically wraps onto 2+ rows when the panel is too narrow
// to fit all buttons on a single line without clipping or hiding any button.
int buttonRow(std::initializer_list<Btn> items) {
    const std::vector<Btn> v(items);
    const int n = static_cast<int>(v.size());
    if (n <= 0) return -1;
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float totalAvail = availW();
    const float padX = ImGui::GetStyle().FramePadding.x * 2.0f + S(10);
    float maxNeeded = S(72);
    for (const auto& b : v) {
        const float tw = ImGui::CalcTextSize(iconLabel(b.icon, b.label).c_str()).x + padX;
        maxNeeded = std::max(maxNeeded, tw);
    }
    int cols = n;
    while (cols > 1 && (totalAvail - sp * (cols - 1)) / cols < maxNeeded) --cols;
    int clicked = -1;
    for (int rowStart = 0; rowStart < n; rowStart += cols) {
        const int rowCount = std::min(cols, n - rowStart);
        const float w = (totalAvail - sp * (rowCount - 1)) / rowCount;
        for (int k = 0; k < rowCount; ++k) {
            const int i = rowStart + (rtl() ? rowCount - 1 - k : k);
            if (k) ImGui::SameLine();
            ImGui::PushID(i);
            if (button(v[i], w)) clicked = i;
            ImGui::PopID();
        }
    }
    return clicked;
}

bool iconButton(const char* id, const char* icon, const char* tipVis, bool enabled = true, bool active = false) {
    ImGui::BeginDisabled(!enabled);
    int pushed = 0;
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(59, 130, 246, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
        pushed = 2;
    }
    ImGui::PushID(id);
    const float h = ImGui::GetFrameHeight();
    const bool r = ImGui::Button(icon, {h + S(4), h});
    ImGui::PopID();
    if (pushed) ImGui::PopStyleColor(pushed);
    ImGui::EndDisabled();
    if (tipVis) ImGui::SetItemTooltip("%s", tipVis);
    return r;
}

// Key field: shows the bound key; while waiting it pulses and accepts a click as "Mouse L".
bool keyChip(const char* id, int vk, bool waiting, float w) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight() + S(2);
    const bool clicked = ImGui::InvisibleButton("##chip", {w, h});
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q{p.x + w, p.y + h};
    const ImU32 fillIdle = g_darkTheme ? (hov ? IM_COL32(38, 52, 74, 255) : kSurfaceElev)
                                       : (hov ? IM_COL32(214, 224, 238, 255) : IM_COL32(226, 232, 240, 255));
    dl->AddRectFilled(p, q, waiting ? alpha(kAmber, 0.16f) : fillIdle, S(6));
    dl->AddRect(p, q, waiting ? alpha(kAmber, 0.55f + 0.45f * pulse()) : (hov ? kAccent : kBorderCol), S(6), 0,
                waiting ? S(1.5f) : S(1));
    const std::string label = waiting ? (w < S(110) ? std::string("...") : std::string(T("Press a key..."))) : keyText(vk);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    dl->AddText({p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f}, waiting ? kAmber : vk ? kText : kMuted, label.c_str());
    if (waiting) addCaptureRect(p, q);
    if (!waiting) ImGui::SetItemTooltip("%s", T("Click, then press a key or mouse button"));
    ImGui::PopID();
    return clicked;
}

// Text input that shows Arabic shaped/ordered while not being edited.
bool inputShaped(const char* id, std::string& s, const char* hintVis, size_t cap = 63) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s", s.c_str());
    const bool active = ImGui::GetActiveID() == ImGui::GetID(id);
    const bool overlay = !active && text::hasRtl(s);
    if (overlay) ImGui::PushStyleColor(ImGuiCol_Text, 0);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool ch = ImGui::InputTextWithHint(id, hintVis, buf, std::min(sizeof buf, cap + 1));
    if (overlay) {
        ImGui::PopStyleColor();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const char* vis = V(s);
        const float tw = ImGui::CalcTextSize(vis).x, pad = ImGui::GetStyle().FramePadding.x;
        ImGui::GetWindowDrawList()->AddText({rtl() ? b.x - pad - tw : a.x + pad, a.y + ImGui::GetStyle().FramePadding.y}, kText, vis);
    }
    if (ch) s = buf;
    return ch;
}

struct MarkerState {
    bool selected = false, hovered = false, waiting = false, dragging = false, dim = false;
    int waitSlot = -1;
    int comboFirst = 0;
};

void chipAt(ImDrawList* dl, ImVec2 c, const std::string& label, bool waiting, float a) {
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    const float w = std::max(S(22), ts.x + S(10)), h = S(20);
    const ImVec2 p0{c.x - w * 0.5f, c.y - h * 0.5f}, p1{c.x + w * 0.5f, c.y + h * 0.5f};
    dl->AddRectFilled(p0, p1, waiting ? alpha(kAmber, 0.30f * a + 0.1f) : alpha(IM_COL32(10, 12, 18, 255), 0.75f * a), S(5));
    dl->AddRect(p0, p1, waiting ? alpha(kAmber, (0.6f + 0.4f * pulse()) * a) : alpha(IM_COL32_WHITE, 0.45f * a), S(5));
    dl->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, waiting ? kAmber : alpha(kText, a), label.c_str());
}

// Minimal translucent chip for play mode: key label only - no rings, arrows or
// frames that obstruct the view. Overlay opacity is applied by the caller.
void drawOverlayChip(ImDrawList* dl, const Binding& b, ImVec2 c) {
    std::string label;
    if (b.type == BindingType::Joystick)
        label = keyText(b.keyUp) + keyText(b.keyLeft) + keyText(b.keyDown) + keyText(b.keyRight);
    else if (b.type == BindingType::Camera)
        label = b.key ? bindingKeyText(b) : i18n::tr("Camera");
    else
        label = bindingKeyText(b);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    const float w = ts.x + S(11), h = S(17);
    const ImVec2 p0{c.x - w * 0.5f, c.y - h * 0.5f}, p1{c.x + w * 0.5f, c.y + h * 0.5f};
    dl->AddRectFilled(p0, p1, alpha(IM_COL32(24, 24, 24, 255), 0.62f), h * 0.5f);
    dl->AddRect(p0, p1, alpha(IM_COL32_WHITE, 0.12f), h * 0.5f, 0, S(1));
    dl->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, alpha(kText, 0.92f), label.c_str());
}

// Returns the marker's main radius (for bubbles/hit areas).
float drawMarker(ImDrawList* dl, const Binding& b, ImVec2 c, ImVec2 end, float videoH, const MarkerState& st) {
    const float a = st.dim ? 0.55f : 1.0f;
    const ImU32 border = st.waiting    ? alpha(kAmber, 0.55f + 0.45f * pulse())
                         : st.selected ? kAccent
                         : alpha(IM_COL32_WHITE, (st.hovered ? 1.0f : 0.75f) * a);
    const ImU32 fill = st.waiting ? alpha(kAmber, 0.18f) : st.selected ? alpha(kAccent, 0.22f) : alpha(IM_COL32(10, 12, 18, 255), 0.45f * a);
    const float thick = (st.selected || st.waiting) ? S(2.5f) : S(1.5f);
    float r = S(20);
    switch (b.type) {
        case BindingType::Tap:
        case BindingType::Drag: {
            const std::string label = (st.waiting && st.waitSlot == 0)
                                          ? (st.comboFirst ? (keyText(st.comboFirst) + "+...") : std::string("..."))
                                          : bindingKeyText(b);
            const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            r = std::max(S(20), ts.x * 0.5f + S(9));
            if (b.type == BindingType::Drag) {
                const ImVec2 d{end.x - c.x, end.y - c.y};
                const float len = std::sqrt(d.x * d.x + d.y * d.y);
                if (len > r + S(10)) {
                    const ImVec2 u{d.x / len, d.y / len};
                    const ImVec2 s0{c.x + u.x * r, c.y + u.y * r}, tip{end.x - u.x * S(8), end.y - u.y * S(8)};
                    dl->AddLine(s0, tip, alpha(border, 0.8f), S(2));
                    const ImVec2 n{-u.y, u.x};
                    dl->AddTriangleFilled({tip.x + u.x * S(1), tip.y + u.y * S(1)},
                                          {tip.x - u.x * S(9) + n.x * S(5), tip.y - u.y * S(9) + n.y * S(5)},
                                          {tip.x - u.x * S(9) - n.x * S(5), tip.y - u.y * S(9) - n.y * S(5)}, alpha(border, 0.9f));
                }
                dl->AddCircleFilled(end, S(7), fill);
                dl->AddCircle(end, S(7), border, 0, S(1.5f));
            }
            if (st.selected || st.dragging) dl->AddCircle(c, r + S(5), alpha(kAccent, 0.35f), 0, S(2));
            dl->AddCircleFilled(c, r, fill);
            dl->AddCircle(c, r, border, 0, thick);
            dl->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, st.waiting ? kAmber : alpha(kText, a), label.c_str());
            break;
        }
        case BindingType::Joystick: {
            r = std::max(S(40), b.radius * videoH);
            if (st.selected || st.dragging) dl->AddCircle(c, r + S(5), alpha(kAccent, 0.35f), 0, S(2));
            dl->AddCircleFilled(c, r, alpha(IM_COL32(10, 12, 18, 255), 0.30f * a));
            dl->AddCircle(c, r, border, 0, thick);
            dl->AddCircleFilled(c, std::max(S(10), r * 0.22f), alpha(IM_COL32_WHITE, 0.18f * a));
            dl->AddCircle(c, std::max(S(10), r * 0.22f), alpha(border, 0.8f), 0, S(1.5f));
            const float k = r * 0.64f;
            const int keys[] = {b.keyUp, b.keyLeft, b.keyDown, b.keyRight};
            const ImVec2 at[] = {{c.x, c.y - k}, {c.x - k, c.y}, {c.x, c.y + k}, {c.x + k, c.y}};
            for (int d = 0; d < 4; ++d) {
                const bool w = st.waiting && st.waitSlot == d + 1;
                chipAt(dl, at[d], w ? std::string("...") : keyText(keys[d]), w, a);
            }
            break;
        }
        case BindingType::Camera: {
            // The aim ring is an ellipse: radiusX widens it horizontally
            // (0 = circle). Drawn as dashed segments.
            const float rx = std::max(S(30), (b.radiusX > 0.0f ? b.radiusX : b.radius) * videoH);
            const float ry = std::max(S(30), b.radius * videoH);
            const int seg = 72;
            for (int s = 0; s < seg; s += 2) {
                const float a0 = s * IM_PI * 2 / seg, a1 = (s + 1) * IM_PI * 2 / seg;
                dl->AddLine({c.x + rx * std::cos(a0), c.y + ry * std::sin(a0)},
                            {c.x + rx * std::cos(a1), c.y + ry * std::sin(a1)}, alpha(border, 0.8f),
                            st.selected ? S(2) : S(1.5f));
            }
            // Selected: 8 free-resize handles on the ring's box - corners move
            // both axes, edge handles move one - square, ellipse, wide or tall.
            if (st.selected) {
                const float hs = S(4.5f);
                const ImVec2 hp[8] = {{c.x - rx, c.y - ry}, {c.x + rx, c.y - ry},
                                      {c.x - rx, c.y + ry}, {c.x + rx, c.y + ry},
                                      {c.x - rx, c.y},        {c.x + rx, c.y},
                                      {c.x, c.y - ry},        {c.x, c.y + ry}};
                for (const ImVec2& q : hp) dl->AddRectFilled({q.x - hs, q.y - hs}, {q.x + hs, q.y + hs}, kAccent);
            }
            // Bounds rectangle drawn live so the configured limits are visible
            // on the preview. X mirrors the height when camBoundsRangeX is 0.
            if (b.camBoundsEnabled) {
                const float brY = std::max(S(30), b.camBoundsRange * videoH);
                const float brX =
                    std::max(S(30), (b.camBoundsRangeX > 0.0f ? b.camBoundsRangeX : b.camBoundsRange) * videoH);
                dl->AddRect({c.x - brX, c.y - brY}, {c.x + brX, c.y + brY}, alpha(kAmber, 0.9f), 0, 0, S(1.5f));
            }
            const float cr = S(20);
            if (st.selected || st.dragging) dl->AddCircle(c, cr + S(5), alpha(kAccent, 0.35f), 0, S(2));
            dl->AddCircleFilled(c, cr, fill);
            dl->AddCircle(c, cr, border, 0, thick);
            const ImVec2 ts = ImGui::CalcTextSize(ICON_EYE);
            dl->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, alpha(kText, a), ICON_EYE);
            r = cr;
            break;
        }
        case BindingType::Fire: {
            const std::string label = (st.waiting && st.waitSlot == 0)
                                          ? (st.comboFirst ? (keyText(st.comboFirst) + "+...") : std::string("..."))
                                          : bindingKeyText(b);
            const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            r = std::max(S(22), ts.x * 0.5f + S(10));
            // Green when Rapid Fire (Pistol/DMR) mode is active; Cyan when Continuous Recoil mode is active
            const ImU32 fireCol = b.rapidFire ? alpha(kOk, a) : b.recoilEnabled ? alpha(kAccentCyan, a) : border;
            const ImU32 fireFill = b.rapidFire ? alpha(kOk, 0.55f * a) : fill;
            if (st.selected || st.dragging || b.rapidFire)
                dl->AddCircle(c, r + S(5), alpha(b.rapidFire ? kOk : kAccentCyan, 0.55f * a), 0, S(2));
            dl->AddCircleFilled(c, r, fireFill);
            dl->AddCircle(c, r, fireCol, 0, b.rapidFire ? S(2.5f) : thick);
            // Crosshair ticks around the circle
            const float t0 = r - S(3), t1 = r + S(4);
            dl->AddLine({c.x - t1, c.y}, {c.x - t0, c.y}, fireCol, S(1.5f));
            dl->AddLine({c.x + t0, c.y}, {c.x + t1, c.y}, fireCol, S(1.5f));
            dl->AddLine({c.x, c.y - t1}, {c.x, c.y - t0}, fireCol, S(1.5f));
            dl->AddLine({c.x, c.y + t0}, {c.x, c.y + t1}, fireCol, S(1.5f));
            // Small recoil direction indicator arrow when recoil is active
            const float vx = static_cast<float>(b.recoilRight - b.recoilLeft);
            const float vy = static_cast<float>(b.recoilDown - b.recoilUp);
            const float vlen = std::hypot(vx, vy);
            if (b.recoilEnabled && vlen > 0.5f) {
                const float ux = vx / vlen, uy = vy / vlen;
                const ImVec2 a0{c.x + ux * (r + S(3)), c.y + uy * (r + S(3))};
                const ImVec2 a1{c.x + ux * (r + S(14)), c.y + uy * (r + S(14))};
                dl->AddLine(a0, a1, alpha(kOk, a), S(2.0f));
                dl->AddCircleFilled(a1, S(2.5f), alpha(kOk, a));
            }
            dl->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, st.waiting ? kAmber : b.rapidFire ? alpha(kOk, a) : alpha(kText, a),
                        label.c_str());
            break;
        }
    }
    return r;
}

// Scales the alpha of everything drawn since `from` (one opacity for the whole marker).
void fadeVertices(ImDrawList* dl, int from, float k) {
    k = std::clamp(k, 0.0f, 1.0f);
    for (int i = from; i < dl->VtxBuffer.Size; ++i) {
        ImU32& c = dl->VtxBuffer[i].col;
        c = (c & 0x00FFFFFFu) | (static_cast<ImU32>(static_cast<float>((c >> 24) & 0xFFu) * k + 0.5f) << 24);
    }
}

}  // namespace

// Floating buttons are a visual layer only; hiding them never changes input handling.
bool App::overlayAllowed() const {
    if (editMode_) return true;
    if (IsIconic(hwnd_) || GetForegroundWindow() != hwnd_) return false;
    return !(settings_.overlayLandscapeOnly && renderer_.hasVideo() && renderer_.videoHeight() > renderer_.videoWidth());
}

float App::panelWidth() const {
    const float winW = static_cast<float>(renderer_.width());
    const float railW = S(34);  // icon-only rail, ~title-bar height
    if (settings_.panelCollapsed) return railW;
    const float maxContent = std::max(S(310), winW * 0.52f - railW);
    const float contentW = std::clamp(S(static_cast<float>(settings_.panelWidthPx)), S(310), std::min(S(640), maxContent));
    return railW + contentW;
}

void App::applyTheme() {
    g_darkTheme = settings_.darkTheme;
    const BOOL darkTitle = g_darkTheme ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkTitle, sizeof darkTitle);
    // Tint the native title bar to match the theme (Windows 11 caption/text color).
    const COLORREF caption = g_darkTheme ? RGB(30, 30, 30) : RGB(245, 245, 245);
    DwmSetWindowAttribute(hwnd_, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof caption);
    const COLORREF capText = g_darkTheme ? RGB(212, 212, 212) : RGB(30, 30, 30);
    DwmSetWindowAttribute(hwnd_, 36 /*DWMWA_TEXT_COLOR*/, &capText, sizeof capText);

    ImGuiStyle& st = ImGui::GetStyle();
    ImVec4* c = st.Colors;
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };

    if (g_darkTheme) {
        // Neutral IDE-style dark palette (VS Code dark) - replaces the navy theme
        kBgPrimary   = IM_COL32(30, 30, 30, 255);    // #1E1E1E
        kBgSecondary = IM_COL32(37, 37, 38, 255);    // #252526
        kSurfaceElev = IM_COL32(45, 45, 48, 255);    // #2D2D30
        kAccent      = IM_COL32(0, 122, 204, 255);   // #007ACC
        kAccentCyan  = IM_COL32(79, 193, 255, 255);  // #4FC1FF
        kText        = IM_COL32(212, 212, 212, 255); // #D4D4D4
        kMuted       = IM_COL32(150, 150, 150, 255); // #969696
        kBorderCol   = IM_COL32(60, 60, 60, 255);    // #3C3C3C
        kRailBg      = IM_COL32(27, 27, 27, 255);    // #1B1B1B
        kLine        = IM_COL32(60, 60, 60, 180);
        kOk          = IM_COL32(78, 201, 176, 255);  // #4EC9B0
        kAmber       = IM_COL32(215, 186, 125, 255); // #D7BA7D
        kDanger      = IM_COL32(241, 76, 76, 255);   // #F14C4C

        c[ImGuiCol_Text]                 = rgb(212, 212, 212);
        c[ImGuiCol_TextDisabled]         = rgb(100, 116, 139);
        c[ImGuiCol_WindowBg]             = rgb(30, 30, 30);
        c[ImGuiCol_ChildBg]              = rgb(0, 0, 0, 0);
        c[ImGuiCol_PopupBg]              = rgb(19, 31, 51, 0.98f);
        c[ImGuiCol_Border]               = rgb(60, 60, 60);
        c[ImGuiCol_FrameBg]              = rgb(45, 45, 48);
        c[ImGuiCol_FrameBgHovered]       = rgb(38, 52, 74);
        c[ImGuiCol_FrameBgActive]        = rgb(45, 62, 88);
        c[ImGuiCol_Button]               = rgb(45, 45, 48);
        c[ImGuiCol_ButtonHovered]        = rgb(60, 60, 60);
        c[ImGuiCol_ButtonActive]         = rgb(0, 122, 204);
        c[ImGuiCol_Header]               = rgb(45, 45, 48);
        c[ImGuiCol_HeaderHovered]        = rgb(60, 60, 60);
        c[ImGuiCol_HeaderActive]         = rgb(0, 122, 204);
        c[ImGuiCol_SliderGrab]           = rgb(0, 122, 204);
        c[ImGuiCol_SliderGrabActive]     = rgb(79, 193, 255);
        c[ImGuiCol_CheckMark]            = rgb(79, 193, 255);
        c[ImGuiCol_Separator]            = rgb(60, 60, 60);
        c[ImGuiCol_ScrollbarBg]          = rgb(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]        = rgb(60, 60, 60);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(71, 85, 105);
        c[ImGuiCol_TableHeaderBg]        = rgb(37, 37, 38);
        c[ImGuiCol_TableRowBgAlt]        = rgb(255, 255, 255, 0.025f);
        c[ImGuiCol_NavCursor]            = rgb(59, 130, 246, 0.85f);
    } else {
        // Integrated NEXPLAY Light Palette
        kBgPrimary   = IM_COL32(241, 245, 249, 255); // #F1F5F9
        kBgSecondary = IM_COL32(255, 255, 255, 255); // #FFFFFF
        kSurfaceElev = IM_COL32(226, 232, 240, 255); // #E2E8F0
        kAccent      = IM_COL32(37, 99, 235, 255);   // #2563EB
        kAccentCyan  = IM_COL32(8, 145, 178, 255);   // #0891B2
        kText        = IM_COL32(15, 23, 42, 255);    // #0F172A
        kMuted       = IM_COL32(71, 85, 105, 255);   // #475569
        kBorderCol   = IM_COL32(203, 213, 225, 255); // #CBD5E1
        kRailBg      = IM_COL32(226, 232, 240, 255);
        kLine        = IM_COL32(203, 213, 225, 220);
        kOk          = IM_COL32(22, 163, 74, 255);
        kAmber       = IM_COL32(217, 119, 6, 255);
        kDanger      = IM_COL32(220, 38, 38, 255);

        c[ImGuiCol_Text]                 = rgb(15, 23, 42);
        c[ImGuiCol_TextDisabled]         = rgb(150, 150, 150);
        c[ImGuiCol_WindowBg]             = rgb(241, 245, 249);
        c[ImGuiCol_ChildBg]              = rgb(0, 0, 0, 0);
        c[ImGuiCol_PopupBg]              = rgb(255, 255, 255, 0.99f);
        c[ImGuiCol_Border]               = rgb(203, 213, 225);
        c[ImGuiCol_FrameBg]              = rgb(226, 232, 240);
        c[ImGuiCol_FrameBgHovered]       = rgb(203, 213, 225);
        c[ImGuiCol_FrameBgActive]        = rgb(186, 200, 218);
        c[ImGuiCol_Button]               = rgb(226, 232, 240);
        c[ImGuiCol_ButtonHovered]        = rgb(203, 213, 225);
        c[ImGuiCol_ButtonActive]         = rgb(37, 99, 235);
        c[ImGuiCol_Header]               = rgb(226, 232, 240);
        c[ImGuiCol_HeaderHovered]        = rgb(203, 213, 225);
        c[ImGuiCol_HeaderActive]         = rgb(37, 99, 235);
        c[ImGuiCol_SliderGrab]           = rgb(37, 99, 235);
        c[ImGuiCol_SliderGrabActive]     = rgb(8, 145, 178);
        c[ImGuiCol_CheckMark]            = rgb(37, 99, 235);
        c[ImGuiCol_Separator]            = rgb(203, 213, 225);
        c[ImGuiCol_ScrollbarBg]          = rgb(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]        = rgb(180, 192, 210);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(150, 150, 150);
        c[ImGuiCol_TableHeaderBg]        = rgb(226, 232, 240);
        c[ImGuiCol_TableRowBgAlt]        = rgb(0, 0, 0, 0.025f);
        c[ImGuiCol_NavCursor]            = rgb(37, 99, 235, 0.85f);
    }
}

void App::initUi() {
    g_scale = std::max(1.0f, GetDpiForWindow(hwnd_) / 96.0f);
    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 0;
    st.ChildRounding = 8;
    st.FrameRounding = 6;
    st.PopupRounding = 8;
    st.GrabRounding = 6;
    st.ScrollbarRounding = 6;
    st.TabRounding = 6;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 1;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 1;
    st.WindowPadding = {14, 14};
    st.FramePadding = {10, 6};
    st.ItemSpacing = {8, 8};
    st.ItemInnerSpacing = {6, 6};
    st.ScrollbarSize = 10;
    st.GrabMinSize = 12;
    st.ButtonTextAlign = {0.5f, 0.5f};
    applyTheme();
    st.ScaleAllSizes(g_scale);
    st.FontScaleDpi = g_scale;
    st.FontSizeBase = 15.0f;

    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring fonts = std::wstring(win) + L"\\Fonts\\";
    auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    auto narrowPath = [](const std::wstring& w) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    };
    std::wstring mainFont = fonts + L"segoeui.ttf";
    if (exists(fonts + L"Cairo-Regular.ttf")) mainFont = fonts + L"Cairo-Regular.ttf";
    else if (exists(fonts + L"NotoSansArabic-Regular.ttf")) mainFont = fonts + L"NotoSansArabic-Regular.ttf";
    if (exists(mainFont)) {
        g_shapedLoader = *ImFontAtlasGetFontLoaderForStbTruetype();
        g_shapedLoader.Name = "stb_truetype+uniscribe";
        g_shapedLoader.FontSrcGetGlyphIndexFromCodepoint = shapedGlyphIndex;
        ImFontConfig cfg;
        cfg.FontLoader = &g_shapedLoader;
        io.Fonts->AddFontFromFileTTF(narrowPath(mainFont).c_str(), 15.0f, &cfg);
        const std::wstring icons = exists(fonts + L"SegoeIcons.ttf") ? fonts + L"SegoeIcons.ttf" : fonts + L"segmdl2.ttf";
        if (exists(icons)) {
            ImFontConfig ic;
            ic.MergeMode = true;
            ic.GlyphOffset = {0, 2};
            io.Fonts->AddFontFromFileTTF(narrowPath(icons).c_str(), 15.0f, &ic);
        }
    } else {
        io.Fonts->AddFontDefault();
    }
    setLanguage(settings_.language);
    // Shrink into the launcher window before the first ShowWindow to avoid a
    // one-frame flash of the full-size layout.
    launcherUi_ = launcherMode();
    applyLauncherWindow(launcherUi_);
}

void App::drawUi() {
    text::frameTick();
    ImGuiIO& io = ImGui::GetIO();
    const bool navKeys = !capture_.active() && (editMode_ || !sessionActive());
    if (navKeys) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // A click anywhere except the waiting marker/key field cancels the capture.
    if (capture_.active() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool inside = false;
        for (const Rect& r : captureRects_) inside |= r.contains({io.MousePos.x, io.MousePos.y});
        if (!inside) cancelCapture(), suppressMouse_ = true;
    }
    captureRectsNext_.clear();
    g_captureRects = &captureRectsNext_;
    if (selectedBinding_ >= static_cast<int>(profile_.bindings.size())) selectedBinding_ = -1;
    hoveredListBinding_ = -1;

    handleShortcuts();
    const bool launcher = launcherMode();
    if (launcher != launcherUi_) {
        launcherUi_ = launcher;
        applyLauncherWindow(launcherUi_);
    }
    // Key-mapping IS edit mode: entering the section starts editing, leaving
    // stops (setEditMode releases input / clears capture either way). The page
    // must actually be open - a collapsed rail or hidden panel counts as left.
    const bool wantEdit = !launcher && !studioOpen_ && section_ == Section::Mapping && sessionActive() &&
                          showPanel_ && !settings_.panelCollapsed;
    if (wantEdit != editMode_) setEditMode(wantEdit);
    // A studio pick (point / region / image capture) hides the studio so the
    // video stays clickable; reopen once the pick finishes.
    if (studioResumeAfterPick_ && pickMode_ == 0) {
        studioResumeAfterPick_ = false;
        studioOpen_ = true;
    }
    // Pending pick readbacks (image capture / color sample) resolve here every
    // frame — also while the studio is open, or results would never land.
    automationUiTick();
    if (launcherUi_) {
        drawLauncher();
    } else if (studioOpen_ && studio_) {
        drawStudio();
    } else {
        drawEmptyState();
        drawOverlay();
        drawZonesOverlay();
        drawImagesOverlay();
        drawFpsOverlay();
        if (showPanel_) drawPanel();
    }
    drawToast();
    drawDiscardPrompt();
    drawReconnectPrompt();
    drawNamePrompt();

    captureRects_.swap(captureRectsNext_);
    g_captureRects = nullptr;
    if (suppressMouse_ && !ImGui::IsAnyMouseDown()) suppressMouse_ = false;

    if (!(profile_ == engineProfile_)) {
        { std::lock_guard lk(inputMu_); input_.setProfile(profile_); }
        engineProfile_ = profile_;
    }
    // One undo step per finished gesture (drag, slider, key capture...).
    if (!capture_.active() && !ImGui::IsAnyItemActive()) history_.commit(profile_);

    ImGui::Render();
    std::lock_guard lk(video::d3dContextLock());
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// NEXPLAY Automation Studio: opens the fullscreen visual-graph workspace. The
// editor edits profile_.automation.studioGraphs (the live draft), so the normal
// Save / Discard guard keeps working unchanged.
void App::openStudio() {
    // A pick left armed while the studio opens would freeze the video forever:
    // pending_.take() is skipped while pickMode_ != 0, and the pick overlay is
    // unreachable under the fullscreen studio. Cancel it.
    if (pickMode_ != 0) cancelPick();
    if (!studio_) {
        studio_ = std::make_unique<StudioEditor>();
        studio_->toast = [this](const std::string& s) { showToast(s); };
        studio_->requestClose = [this] {
            studioOpen_ = false;
            detectPreview_.active = false;  // stop any Find Object live test
        };
        studio_->sessionActive = [this] { return sessionActive(); };
        studio_->pickPoint = [this](Vec2* t) {
            pickTarget_ = t;
            pickMode_ = 3;
            studioOpen_ = false;
            studioResumeAfterPick_ = true;
        };
        studio_->pickSwipe = [this](Vec2* a, Vec2* b) {
            pickTarget_ = a;
            pickTarget2_ = b;
            pickSaved1_ = *a;
            pickSaved2_ = *b;
            pickSwipeHandle_ = 0;
            pickMode_ = 7;
            studioOpen_ = false;
            studioResumeAfterPick_ = true;
        };
        studio_->pickColors = [this](studio::ColorVal* c, std::string* list) {
            pickColorTarget_ = c;
            pickColorList_ = list;
            pickMode_ = 8;
            studioOpen_ = false;
            studioResumeAfterPick_ = true;
        };
        studio_->pickRect = [this](Rect* r) {
            pickRectTarget_ = r;
            pickMode_ = 6;
            studioOpen_ = false;
            studioResumeAfterPick_ = true;
        };
        studio_->pickImage = [this](int idx) {
            pickImage_ = idx;
            pickMode_ = 4;
            studioOpen_ = false;
            studioResumeAfterPick_ = true;
        };
        // Graph execution engine hooks (Phase 5+): the editor stays engine-free
        // and talks to GraphRunner only through these std::function hooks.
        studio_->runtime.run = [this](const std::string& gid, bool debug) { studioRunGraph(gid, debug); };
        studio_->runtime.stop = [this] { studioStopGraph(); };
        studio_->runtime.running = [this](const std::string& gid) {
            return graphRunner_ && graphRunner_->active() && studioRunningGraphId_ == gid;
        };
        studio_->runtime.paused = [this] { return graphRunner_ && graphRunner_->paused(); };
        studio_->runtime.pause = [this] { if (graphRunner_) graphRunner_->pause(); };
        studio_->runtime.resume = [this] { if (graphRunner_) graphRunner_->resume(); };
        studio_->runtime.stepOnce = [this] { if (graphRunner_) graphRunner_->stepOnce(); };
        studio_->runtime.nodeBadge = [this](const std::string&, uint32_t nodeId) {
            using B = studio_ui::NodeRunBadge;
            if (!graphRunner_) return B::None;
            switch (graphRunner_->nodeState(nodeId)) {
                case studio::NodeRunState::Active:
                case studio::NodeRunState::Waiting: return B::Executing;
                case studio::NodeRunState::Done: return B::Completed;
                case studio::NodeRunState::Error: return B::Error;
                case studio::NodeRunState::Skipped: return B::Warning;
                default: return B::None;
            }
        };
        studio_->runtime.wireValue = [this](uint32_t connId) {
            return graphRunner_ ? graphRunner_->wireValue(connId) : std::string{};
        };
        studio_->runtime.events = [this]() -> const std::vector<studio::GraphEvent>* {
            return graphRunner_ ? &graphRunner_->events() : nullptr;
        };
        studio_->save = [this] { saveProfile(); };
        // External key capture shared with the automation UI: slot 7 = trigger /
        // toggle keys (exempt from the single-key owner check, key2 = combo),
        // slot 6 = action keys (owner-checked against bindings).
        studio_->captureKey = [this](int* p, int* p2, int slot) { beginExternalCapture(p, slot, p2); };
        studio_->capturingKey = [this](const int* p) {
            return capture_.active() && (capExternal_ == p || capExternal2_ == p);
        };
        studio_->deleteImage = [this](int i) { deleteRefImage(i); };

        // ---- merged legacy automation surface ---------------------------------
        studio_->automationOn = [this] { return automationOn_; };
        studio_->setAutomationOn = [this](bool on) { setAutomation(on, i18n::tr("Stopped")); };
        studio_->newZone = [this] {
            pickMode_ = 2;
            studioOpen_ = false;
            studioResumeAfterPick_ = true;
        };
        studio_->pickZoneColor = [this](int z, int c, bool multi) {
            pickMode_ = 1;
            pickZone_ = z;
            pickColor_ = c;
            pickMulti_ = multi;
            studioOpen_ = false;
            studioResumeAfterPick_ = true;
        };
        studio_->testImage = [this](int i) {
            if (imgTest_ && imgTestImage_ == i) { imgTest_ = false; imgTestImage_ = -1; }
            else { imgTest_ = true; imgTestImage_ = i; }
        };
        studio_->imageTesting = [this](int i) { return imgTest_ && imgTestImage_ == i; };
        studio_->zoneTestOn = [this] { return zoneTest_; };
        studio_->setZoneTest = [this](bool on) { zoneTest_ = on; };
        studio_->reloadImages = [this] {
            loadRefImageFiles();
            showToast(i18n::tr("Reference images reloaded"));
        };
        studio_->resetMonitorRuntime = [this] { monitorRt_.clear(); };
        studio_->armMonitor = [this](int i) {
            if (i >= 0 && i < static_cast<int>(monitorRt_.size())) {
                MonitorRuntime& rt = monitorRt_[i];
                rt.switchedOn = !rt.switchedOn;
                if (rt.switchedOn) { rt.reset(); rt.switchedOn = true; }
            }
        };
        studio_->zoneStatus = [this](int i) {
            StudioEditor::ZoneStatus s;
            if (i >= 0 && i < static_cast<int>(zoneScan_.size())) {
                const auto& zs = zoneScan_[i];
                s.tested = zs.present.has_value();
                s.present = zs.present.value_or(false);
                s.total = zs.match.total;
                s.matched = std::accumulate(zs.match.perColor.begin(), zs.match.perColor.end(), 0);
                s.perColor = zs.match.perColor;
            }
            return s;
        };
        studio_->imageStatus = [this](int i) {
            StudioEditor::ImageStatus s;
            if (i >= 0 && i < static_cast<int>(imgScan_.size())) {
                const auto& is = imgScan_[i];
                s.tested = is.tested;
                s.found = is.found;
                s.score = is.score;
                s.costUs = is.scanCostUs;
            }
            return s;
        };
        studio_->monitorStatus = [this](int i) {
            StudioEditor::MonitorStatus s;
            if (i >= 0 && i < static_cast<int>(monitorRt_.size())) {
                const auto& rt = monitorRt_[i];
                s.armed = rt.switchedOn;
                s.present = rt.present();
                s.runs = rt.runs();
            }
            return s;
        };
        studio_->detectClasses = [this] { return detectorClassNames(); };
        studio_->detectStatus = [this] { return detectorStatus(); };
        studio_->setDetectPreview = [this](bool on, const Rect& r,
                                           const std::vector<std::string>& classes, float conf) {
            detectPreview_.active = on;
            if (on) {
                detectPreview_.region = r;
                detectPreview_.classFilter = classes;
                detectPreview_.conf = conf;
                detectPreview_.requested = false;
                detectPreview_.pendingReqId = 0;
                detectPreview_.ready = false;
                detectPreview_.hits.clear();
            } else {
                detectPreview_.hits.clear();
                detectPreview_.requested = false;
                detectPreview_.pendingReqId = 0;
            }
        };
    }
    setEditMode(false);
    studioOpen_ = true;
}

// Fullscreen studio workspace: a borderless ImGui window over the whole client
// area; StudioEditor::draw fills it (toolbar / library / canvas / inspector /
// console / status bar).
void App::drawStudio() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##nexplay_studio", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    if (studio_->wantsHideForPick()) {
        studio_->clearHideForPick();
        // The pick callback already hid the studio when it armed pickMode_, so
        // this flag is only meaningful while a pick is still in flight — after
        // the pick finished, acting on it would close the freshly reopened
        // studio for no reason.
        if (pickMode_ != 0) studioOpen_ = false;
    } else {
        studio_->draw(profile_.automation);
    }
    ImGui::End();
}

void App::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (!editMode_ || io.WantTextInput || capture_.active()) return;
    const bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? redo() : undo();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) saveProfile();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateBinding(selectedBinding_);
    else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteBinding(selectedBinding_);
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive()) selectedBinding_ = -1;
}

// WASD+-style flow: the app opens as a compact device-picker window; on Connect
// the window fits itself to the video aspect instead of restoring an old size.
void App::applyLauncherWindow(bool on) {
    if (IsIconic(hwnd_)) return;
    if (IsZoomed(hwnd_)) return;  // never kick the user out of a maximized window
    if (!on) {
        fitWindowToVideo();
        return;
    }
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const int w = static_cast<int>(S(430)), h = static_cast<int>(S(500));
    const int x = work.left + (work.right - work.left - w) / 2;
    const int y = work.top + (work.bottom - work.top - h) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Sizes the window so the client area fits the video aspect (plus the rail/panel):
// the WASD+ mirror look - no dead margins around a portrait phone. Called when the
// stream size arrives or changes (rotation), on panel show/hide, and when leaving
// the launcher - before the first frame dims are known a portrait phone is assumed.
void App::fitWindowToVideo() {
    if (IsIconic(hwnd_)) return;
    if (IsZoomed(hwnd_)) return;  // maximized stays maximized - the layout refits itself
    const uint32_t fw = appliedW_ ? appliedW_ : 1080, fh = appliedH_ ? appliedH_ : 2400;
    RECT wr, cr;
    GetWindowRect(hwnd_, &wr);
    GetClientRect(hwnd_, &cr);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const int frameW = (wr.right - wr.left) - cr.right;
    const int frameH = (wr.bottom - wr.top) - cr.bottom;
    const int rail = showPanel_ ? static_cast<int>(panelWidth()) : 0;
    // WASD+ mirror look: a medium centered window. The video area fills ~55%
    // of the work area while preserving the stream aspect; portrait streams get
    // a ~30% larger window since a tall phone leaves so much width unused.
    const float frac = fh > fw ? 0.72f : 0.55f;
    const int maxClientW = static_cast<int>((work.right - work.left - frameW - rail) * frac);
    const int maxClientH = static_cast<int>((work.bottom - work.top - frameH) * frac);
    const float scale = std::min(
        maxClientW / static_cast<float>(fw),
        maxClientH / static_cast<float>(fh));
    const int clientW = std::max(320, static_cast<int>(fw * scale));
    const int clientH = std::max(200, static_cast<int>(fh * scale));
    int winW = clientW + rail + frameW;
    int winH = clientH + frameH;
    winW = std::min<int>(winW, work.right - work.left);
    // Center on the work area, not on the previous window rect.
    const int x = work.left + (work.right - work.left - winW) / 2;
    const int y = work.top + (work.bottom - work.top - winH) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Startup screen: the device list - pick a phone, Connect or Refresh. USB only;
// the side panel stays hidden until a session exists.
// WASD+-style launcher: plain dark card, utility icons on the top-right edge,
// centered title, inset device list, flat full-width buttons, guide link.
void App::drawLauncher() {
    const float W = static_cast<float>(renderer_.width()), H = static_cast<float>(renderer_.height());
    ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, {W, H}, kBgPrimary);
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({W, H});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(18), S(12)});
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = S(18), top = S(12);

    // Top strip: utility icons on the right edge (help, settings, theme, lang).
    const float ibh = ImGui::GetFrameHeight(), ibw = ibh + S(4), isp = ImGui::GetStyle().ItemSpacing.x;
    const float iconsW = ibw * 4 + isp * 3;
    ImGui::SetCursorScreenPos({W - pad - iconsW, top});
    if (iconButton("lhelp", ICON_HELP, T("Help"))) {
        launcherSkip_ = true;
        section_ = Section::Settings;
        settingsTab_ = 3;
        settings_.panelCollapsed = false;
    }
    ImGui::SameLine();
    if (iconButton("lset", ICON_SETTINGS, T("Settings"))) {
        launcherSkip_ = true;
        section_ = Section::Settings;
        settings_.panelCollapsed = false;
    }
    ImGui::SameLine();
    if (iconButton("ltheme", settings_.darkTheme ? ICON_SUN : ICON_MOON,
                   settings_.darkTheme ? T("Light theme") : T("Dark theme"))) {
        settings_.darkTheme = !settings_.darkTheme;
        applyTheme();
        saveSettings();
    }
    ImGui::SameLine();
    if (iconButton("llang", ICON_GLOBE, i18n::rtl() ? "English" : T("Arabic"))) setLanguage(i18n::rtl() ? "en" : "ar");

    // Centered title.
    const char* title = T("Device list");
    ImGui::PushFont(nullptr, 17.0f);
    const float th = ImGui::GetTextLineHeight();
    dl->AddText({(W - ImGui::CalcTextSize(title).x) * 0.5f, top + ibh + S(14)}, kText, title);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos({pad, top + ibh + S(14) + th + S(12)});

    std::vector<platform::AdbDevice> devs;
    {
        std::lock_guard lk(devMu_);
        devs = devices_;
    }
    // Keep the selection on a listed device, preferring a usable one.
    int sel = -1;
    for (int i = 0; i < static_cast<int>(devs.size()); ++i)
        if (devs[i].serial == launcherSerial_) sel = i;
    if (sel < 0) {
        for (int i = 0; i < static_cast<int>(devs.size()); ++i)
            if (devs[i].usable()) {
                sel = i;
                break;
            }
        if (sel < 0 && !devs.empty()) sel = 0;
        launcherSerial_ = sel >= 0 ? devs[sel].serial : std::string();
    }

    // Inset list box - remaining height minus the buttons, guide link, and status.
    const float btnH = ImGui::GetFrameHeight() + S(10);
    const float listH = std::max(S(80), ImGui::GetContentRegionAvail().y - btnH * 2.0f - ImGui::GetTextLineHeight() * 2.0f - S(36));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, g_darkTheme ? IM_COL32(22, 23, 27, 255) : IM_COL32(250, 250, 251, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, g_darkTheme ? IM_COL32(58, 60, 66, 255) : IM_COL32(210, 212, 216, 255));
    ImGui::BeginChild("##devlist", {0, listH}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    if (devs.empty()) {
        const char* hint = T("No device. Connect the phone by USB and enable USB debugging (Developer options).");
        const ImVec2 ts = ImGui::CalcTextSize(hint, nullptr, false, availW());
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetCursorPosY() + (listH - S(16) - ts.y) * 0.5f));
        paragraph(i18n::tr("No device. Connect the phone by USB and enable USB debugging (Developer options)."), kMuted);
    }
    ImGui::PushStyleColor(ImGuiCol_Header, alpha(kAccent, 0.38f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, alpha(kAccent, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, alpha(kAccent, 0.45f));
    for (int i = 0; i < static_cast<int>(devs.size()); ++i) {
        const platform::AdbDevice& d = devs[i];
        ImGui::PushID(i);
        std::string label = (d.model.empty() ? d.serial : d.model) + " : " + d.serial;
        label += " --- [" + std::string(d.usable() ? T("Connected") : i18n::tr(d.state.c_str())) + "]";
        if (ImGui::Selectable(V(label), i == sel, 0, {0, ImGui::GetTextLineHeight() + S(10)})) {
            sel = i;
            launcherSerial_ = d.serial;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && d.usable()) connect(d.serial);
        ImGui::PopID();
    }
    ImGui::PopStyleColor(3);
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    if (!trackerError_.empty()) paragraph(trackerError_, kDanger);
    if (sel >= 0 && devs[sel].state == "unauthorized")
        paragraph(i18n::tr("Accept the 'Allow USB debugging' prompt on the phone."), kAmber);

    // Flat neutral buttons like the WASD+ launcher - not the accent style.
    ImGui::PushStyleColor(ImGuiCol_Button, g_darkTheme ? IM_COL32(45, 47, 52, 255) : IM_COL32(229, 231, 235, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_darkTheme ? IM_COL32(56, 59, 65, 255) : IM_COL32(214, 217, 222, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_darkTheme ? IM_COL32(38, 40, 45, 255) : IM_COL32(200, 203, 209, 255));
    const bool canConnect = sel >= 0 && devs[sel].usable();
    if (button({ICON_CONNECT, T("Connect"), canConnect, canConnect}, availW(), btnH) && canConnect) connect(devs[sel].serial);
    if (button({ICON_RESET, T("Refresh"), true}, availW(), btnH)) refreshDevices();
    ImGui::PopStyleColor(3);

    // Centered guide link -> Help tab.
    const char* guide = T("Guide");
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(guide).x) * 0.5f);
    if (ImGui::Button(guide)) {
        launcherSkip_ = true;
        section_ = Section::Settings;
        settingsTab_ = 3;
        settings_.panelCollapsed = false;
    }
    ImGui::PopStyleColor(4);

    // Bottom line: status on the left, brand on the right (like "wasd.plus").
    const char* st;
    if (!trackerError_.empty()) st = "adb: offline";
    else if (sel >= 0) st = V(i18n::tr(devs[sel].state.c_str()));
    else st = T("No device");
    const float sy = H - pad - ImGui::GetTextLineHeight();
    dl->AddText({pad, sy}, kMuted, st);
    const char* brand = "NEXPLAY";
    dl->AddText({W - pad - ImGui::CalcTextSize(brand).x, sy}, kMuted, brand);
    ImGui::End();
}

void App::drawEmptyState() {
    if (renderer_.hasVideo() || gameArea_.w <= 0) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 a{videoRect_.x, videoRect_.y}, b{videoRect_.x + videoRect_.w, videoRect_.y + videoRect_.h};
    dl->AddRectFilled(a, b, kBgSecondary, S(12));
    dl->AddRect(a, b, kBorderCol, S(12), 0, S(1.5f));
    if (editMode_) return;
    const float cx = gameArea_.x + gameArea_.w * 0.5f, cy = gameArea_.y + gameArea_.h * 0.5f;
    // Device status card instead of the app logo: shows the detected phone and a
    // Connect button so the user can start the session from the empty state itself.
    std::vector<platform::AdbDevice> devs;
    {
        std::lock_guard lk(devMu_);
        devs = devices_;
    }
    const platform::AdbDevice* dev = nullptr;
    for (const auto& d : devs)
        if (d.usable()) {
            dev = &d;
            break;
        }
    ImGui::SetNextWindowPos({cx, cy}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({S(310), 0});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(kBgPrimary));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(20), S(16)});
    const SessionState ss = session_ ? session_->state() : SessionState::Idle;
    const bool busy = ss == SessionState::Starting || ss == SessionState::Running || ss == SessionState::Stopping;
    if (ImGui::Begin("##devstate", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
        if (busy) {
            // A session exists - frames just have not arrived yet. Never offer a
            // stale "No device / Connect" card during the handshake.
            alignedText(T(ss == SessionState::Starting ? "Connecting…" : "Starting video…"), kMuted);
        } else if (ss == SessionState::Failed) {
            alignedText(T("Connection failed"), kDanger);
            if (dev && button({ICON_RESET, T("Retry"), true, true}, availW())) connect(dev->serial);
        } else {
            alignedText(T("No device connected"), kMuted);
            if (dev) {
                alignedText(V(dev->model.empty() ? dev->serial : dev->model), kText);
                paragraph(i18n::tr("Device detected over USB."), kMuted);
                if (button({ICON_PLAY, T("Connect"), true, true}, availW())) connect(dev->serial);
            } else {
                paragraph(i18n::tr("Connect your phone by USB with USB debugging enabled."), kMuted);
            }
            if (launcherSkip_ && !session_ && button({ICON_PHONE, T("Device list")}, availW())) launcherSkip_ = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::drawOverlay() {
    const bool show = editMode_ || (sessionActive() && overlayAllowed() && settings_.overlayOpacity > 0);
    if (!show || videoRect_.w <= 0 || videoRect_.h <= 0) return;
    const ImVec2 o{videoRect_.x, videoRect_.y}, sz{videoRect_.w, videoRect_.h};
    ImGui::SetNextWindowPos(o);
    ImGui::SetNextWindowSize(sz);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                          ImGuiWindowFlags_NoScrollWithMouse;
    if (!editMode_) fl |= ImGuiWindowFlags_NoInputs;
    ImGui::Begin("##overlay", nullptr, fl);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto toScreen = [&](Vec2 n) { return ImVec2(o.x + n.x * sz.x, o.y + n.y * sz.y); };
    auto toNorm = [&](ImVec2 p) { return Vec2{std::clamp((p.x - o.x) / sz.x, 0.0f, 1.0f), std::clamp((p.y - o.y) / sz.y, 0.0f, 1.0f)}; };
    const int n = static_cast<int>(profile_.bindings.size());
    const bool waiting = capture_.active() && capTarget_.slot >= 0 && capTarget_.slot != 5;
    int hovered = -1;
    bool openMarkerMenu = false;

    if (editMode_) {
        if (waiting) dl->AddRect(o, {o.x + sz.x, o.y + sz.y}, alpha(kAmber, 0.5f), 0, 0, S(2));
        else dl->AddRect(o, {o.x + sz.x, o.y + sz.y}, alpha(kAccent, 0.45f), 0, 0, S(1.5f));
        // Interaction pass, topmost (last drawn) first so it wins the hover.
        for (int i = n - 1; i >= 0; --i) {
            Binding& b = profile_.bindings[i];
            const int parts = b.type == BindingType::Drag ? 2 : 1;
            for (int part = parts - 1; part >= 0; --part) {
                const ImVec2 c = toScreen(part ? b.pos2 : b.pos);
                float hr = part ? S(11) : S(22);
                if (b.type == BindingType::Joystick) hr = std::max(S(26), b.radius * sz.y * 0.35f);
                ImGui::SetCursorScreenPos({c.x - hr, c.y - hr});
                ImGui::PushID(i * 4 + part);
                ImGui::InvisibleButton("##m", {hr * 2, hr * 2}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                ImGui::PopID();
                if (ImGui::IsItemHovered() && hovered < 0) hovered = i;
                if (waiting && capTarget_.binding == i && part == 0) {
                    const float rr = b.type == BindingType::Joystick ? std::max(S(40), b.radius * sz.y) : hr;
                    addCaptureRect({c.x - rr, c.y - rr}, {c.x + rr, c.y + rr});
                }
                if (suppressMouse_) continue;
                if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    if (waiting && capTarget_.binding == i && part == 0) {
                        finishCapture(VK_LBUTTON);
                        suppressMouse_ = true;
                        continue;
                    }
                    selectedBinding_ = i;
                    dragBinding_ = i;
                    dragPart_ = part;
                    dragStart_ = part ? b.pos2 : b.pos;
                    dragging_ = false;
                }
                if (ImGui::IsItemActive() && dragBinding_ == i && dragPart_ == part && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
                    if (!dragging_ && d.x * d.x + d.y * d.y > S(4) * S(4)) dragging_ = true;
                    if (dragging_) {
                        Vec2& p = part ? b.pos2 : b.pos;
                        p = {std::clamp(dragStart_.x + d.x / sz.x, 0.0f, 1.0f), std::clamp(dragStart_.y + d.y / sz.y, 0.0f, 1.0f)};
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                    }
                }
                if (ImGui::IsItemDeactivated() && dragBinding_ == i && dragPart_ == part) dragBinding_ = -1, dragging_ = false;
                if (ImGui::IsItemHovered() && !dragging_) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !capture_.active()) selectedBinding_ = i, openMarkerMenu = true;
            }
        }
        // Free-resize handles on the selected camera's aim ring: corners adjust
        // both axes, edges adjust one - circle, ellipse, wide or tall.
        if (!waiting && selectedBinding_ >= 0 && selectedBinding_ < n) {
            Binding& b = profile_.bindings[selectedBinding_];
            if (b.type == BindingType::Camera) {
                const ImVec2 c = toScreen(b.pos);
                const float ry = std::max(S(30), b.radius * sz.y);
                const float rx = std::max(S(30), (b.radiusX > 0.0f ? b.radiusX : b.radius) * sz.y);
                const ImVec2 hp[8] = {{c.x - rx, c.y - ry}, {c.x + rx, c.y - ry},
                                      {c.x - rx, c.y + ry}, {c.x + rx, c.y + ry},
                                      {c.x - rx, c.y},        {c.x + rx, c.y},
                                      {c.x, c.y - ry},        {c.x, c.y + ry}};
                for (int h = 0; h < 8; ++h) {
                    ImGui::SetCursorScreenPos({hp[h].x - S(7), hp[h].y - S(7)});
                    ImGui::PushID(64 + h);
                    ImGui::InvisibleButton("##camh", {S(14), S(14)});
                    ImGui::PopID();
                    const ImGuiMouseCursor cur =
                        h <= 3 ? ImGuiMouseCursor_ResizeAll : h <= 5 ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS;
                    if (ImGui::IsItemHovered() || (camDragHandle_ == h && ImGui::IsMouseDown(ImGuiMouseButton_Left)))
                        ImGui::SetMouseCursor(cur);
                    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) camDragHandle_ = h;
                    if (camDragHandle_ == h && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        const ImVec2 mp = ImGui::GetIO().MousePos;
                        const float dx = std::abs(mp.x - c.x) / sz.y, dy = std::abs(mp.y - c.y) / sz.y;
                        if (h <= 5) b.radiusX = std::clamp(dx, 0.0f, 0.50f);
                        if (h <= 3 || h >= 6) b.radius = std::clamp(dy, 0.05f, 0.45f);
                    }
                    if (camDragHandle_ == h && ImGui::IsItemDeactivated()) camDragHandle_ = -1;
                }
            }
        }
        // Empty game area: click places the current tool, right-click offers the type menu.
        ImGui::SetCursorScreenPos(o);
        const bool pressed = ImGui::InvisibleButton("##canvas", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        if (ImGui::IsItemHovered() && !capture_.active()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (pressed && !suppressMouse_) {
            const bool right = ImGui::IsMouseReleased(ImGuiMouseButton_Right);
            const ImGuiMouseButton mb = right ? ImGuiMouseButton_Right : ImGuiMouseButton_Left;
            const ImVec2 at = ImGui::GetIO().MouseClickedPos[mb];
            const ImVec2 d = ImGui::GetMouseDragDelta(mb, 0.0f);
            const bool inside = at.x >= o.x && at.y >= o.y && at.x < o.x + sz.x && at.y < o.y + sz.y;
            if (inside && d.x * d.x + d.y * d.y < S(6) * S(6)) {
                if (right) menuPos_ = toNorm(at), ImGui::OpenPopup("##addmenu");
                else if (selectedBinding_ >= 0 && !capture_.active() && ImGui::IsKeyDown(ImGuiMod_Alt)) selectedBinding_ = -1;
                else addBindingAt(tool_, toNorm(at));
            }
        }
        // Palette drag & drop lands a new control at the drop point.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("WASD_TOOL")) {
                const BindingType t = *static_cast<const BindingType*>(pl->Data);
                addBindingAt(t, toNorm(ImGui::GetMousePos()));
            }
            ImGui::EndDragDropTarget();
        }
        if (openMarkerMenu) ImGui::OpenPopup("##markermenu");

        if (ImGui::BeginPopup("##addmenu")) {
            ImGui::PushFont(nullptr, 12.5f);
            alignedText(T("Add here"), kMuted);
            ImGui::PopFont();
            const BindingType types[] = {BindingType::Tap, BindingType::Fire, BindingType::Joystick, BindingType::Camera, BindingType::Drag};
            for (BindingType t : types) {
                bool hasCamera = false;
                for (const Binding& e : profile_.bindings) hasCamera |= e.type == BindingType::Camera;
                const bool disabled = t == BindingType::Camera && hasCamera;
                if (ImGui::Selectable(iconLabel(typeIcon(t), T(typeName(t))).c_str(), false,
                                      disabled ? ImGuiSelectableFlags_Disabled : 0, {S(180), 0})) {
                    tool_ = t;
                    addBindingAt(t, menuPos_);
                }
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##markermenu")) {
            const int i = selectedBinding_;
            if (i >= 0 && i < static_cast<int>(profile_.bindings.size())) {
                const Binding& b = profile_.bindings[i];
                ImGui::PushFont(nullptr, 12.5f);
                alignedText(V(b.name), kMuted);
                ImGui::PopFont();
                const float w = S(170);
                if (b.type != BindingType::Camera &&
                    ImGui::Selectable(iconLabel(ICON_KEYBOARD, T(b.type == BindingType::Joystick ? "Change keys" : "Change key")).c_str(),
                                      false, 0, {w, 0}))
                    beginCapture(i, b.type == BindingType::Joystick ? 1 : 0, false, b.type == BindingType::Joystick);
                if (b.type != BindingType::Camera && ImGui::Selectable(iconLabel(ICON_COPY, T("Duplicate")).c_str(), false, 0, {w, 0}))
                    duplicateBinding(i);
                if (ImGui::Selectable(iconLabel(ICON_DELETE, T("Delete")).c_str(), false, 0, {w, 0})) deleteBinding(i);
            }
            ImGui::EndPopup();
        }
    }

    // Draw pass.
    for (int i = 0; i < static_cast<int>(profile_.bindings.size()); ++i) {
        const Binding& b = profile_.bindings[i];
        MarkerState st;
        st.selected = editMode_ && i == selectedBinding_;
        st.hovered = editMode_ && (i == hovered || i == hoveredListBinding_);
        st.waiting = waiting && capTarget_.binding == i;
        st.waitSlot = st.waiting ? capTarget_.slot : -1;
        st.comboFirst = st.waiting ? comboFirstVk_ : 0;
        st.dragging = dragging_ && dragBinding_ == i;
        const ImVec2 c = toScreen(b.pos);
        const int v0 = dl->VtxBuffer.Size;
        if (!editMode_) {
            drawOverlayChip(dl, b, c);
            fadeVertices(dl, v0, settings_.overlayOpacity / 100.0f);
            continue;
        }
        const float r = drawMarker(dl, b, c, toScreen(b.pos2), sz.y, st);
        const float jr = b.type == BindingType::Joystick ? std::max(S(40), b.radius * sz.y) : r;
        if (st.waiting) {
            std::string hint = capTarget_.sequence
                                   ? i18n::trf("%s: press a key", i18n::tr(capTarget_.slot == 1   ? "Up"
                                                                           : capTarget_.slot == 2 ? "Left"
                                                                           : capTarget_.slot == 3 ? "Down"
                                                                                                  : "Right"))
                                   : std::string(i18n::tr("Press a key or mouse button"));
            const char* h1 = V(hint);
            const char* h2 = T("Esc: cancel");
            const float fs = ImGui::GetFontSize() * 0.85f;
            ImFont* f = ImGui::GetFont();
            const ImVec2 t1 = f->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, h1), t2 = f->CalcTextSizeA(fs, FLT_MAX, 0, h2);
            const float bw = std::max(t1.x, t2.x) + S(20), bh = t1.y + t2.y + S(14);
            float bx = std::clamp(c.x - bw * 0.5f, o.x + S(4), std::max(o.x + S(4), o.x + sz.x - bw - S(4)));
            float by = c.y + jr + S(10);
            if (by + bh > o.y + sz.y) by = c.y - jr - S(10) - bh;
            dl->AddRectFilled({bx, by}, {bx + bw, by + bh}, IM_COL32(20, 22, 28, 240), S(8));
            dl->AddRect({bx, by}, {bx + bw, by + bh}, alpha(kAmber, 0.7f), S(8));
            dl->AddText({bx + (bw - t1.x) * 0.5f, by + S(6)}, kAmber, h1);
            dl->AddText(f, fs, {bx + (bw - t2.x) * 0.5f, by + S(8) + t1.y}, kMuted, h2);
        } else if (st.hovered || st.selected) {
            const char* name = V(b.name);
            const float fs = ImGui::GetFontSize() * 0.85f;
            const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, name);
            const ImVec2 p{c.x - ts.x * 0.5f, c.y + jr + S(6)};
            dl->AddRectFilled({p.x - S(6), p.y - S(2)}, {p.x + ts.x + S(6), p.y + ts.y + S(2)}, IM_COL32(10, 12, 18, 200), S(4));
            dl->AddText(ImGui::GetFont(), fs, p, st.selected ? kAccent : kText, name);
        }
    }
    if (probe_.active || editMode_) {
        const ImVec2 p = toScreen(probe_.point);
        dl->AddRect({p.x - 8, p.y - 8}, {p.x + 8, p.y + 8}, IM_COL32(255, 60, 60, 220), 0, 0, 2.0f);
    }
    ImGui::End();
}

// Draggable palette of control types, shown in the side panel during edit
// mode. Click selects the placement tool; dragging a card onto the game drops
// the control where it lands (WASD+-style).
void App::drawToolPalette() {
    const BindingType tools[] = {BindingType::Tap, BindingType::Fire, BindingType::Joystick, BindingType::Camera, BindingType::Drag};
    const int n = static_cast<int>(sizeof(tools) / sizeof(tools[0]));
    bool hasCamera = false;
    for (const Binding& e : profile_.bindings) hasCamera |= e.type == BindingType::Camera;
    // Circular icon buttons like the reference: circle glyph + label below.
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const int cols = std::clamp(static_cast<int>((availW() + gap) / S(62)), 2, n);
    const float cw = (availW() - gap * (cols - 1)) / cols;
    const float r = S(20);
    const float ch = r * 2 + S(22);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < n; ++i) {
        const BindingType t = tools[i];
        if (i % cols) ImGui::SameLine();
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool active = tool_ == t;
        const bool dim = t == BindingType::Camera && hasCamera;
        ImGui::InvisibleButton("##pal", {cw, ch});
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemActivated()) tool_ = t;
        const ImVec2 c{p.x + cw * 0.5f, p.y + r + S(1)};
        const ImU32 fg = dim ? kMuted : active ? kAccent : hov ? kAccentCyan : kText;
        dl->AddCircleFilled(c, r, active ? alpha(kAccent, 0.28f) : hov ? alpha(kAccent, 0.12f) : kSurfaceElev);
        dl->AddCircle(c, r, active ? kAccent : hov ? alpha(kAccent, 0.55f) : kBorderCol, 0, active ? S(1.8f) : S(1));
        if (t == BindingType::Fire) {
            // Custom crosshair: ring + four ticks (no suitable MDL2 glyph exists).
            const float cr = r * 0.42f, arm = r * 0.72f, tick = S(1.4f);
            dl->AddCircle(c, cr, fg, 0, tick);
            dl->AddLine({c.x - arm, c.y}, {c.x - cr - S(1.5f), c.y}, fg, tick);
            dl->AddLine({c.x + cr + S(1.5f), c.y}, {c.x + arm, c.y}, fg, tick);
            dl->AddLine({c.x, c.y - arm}, {c.x, c.y - cr - S(1.5f)}, fg, tick);
            dl->AddLine({c.x, c.y + cr + S(1.5f)}, {c.x, c.y + arm}, fg, tick);
        } else {
            const char* ic = typeIcon(t);
            const ImVec2 is = ImGui::CalcTextSize(ic);
            dl->AddText({c.x - is.x * 0.5f, c.y - is.y * 0.5f}, fg, ic);
        }
        const char* nm = T(typeName(t));
        ImGui::PushFont(nullptr, 11.5f);
        dl->AddText({c.x - ImGui::CalcTextSize(nm).x * 0.5f, p.y + r * 2 + S(6)}, dim ? alpha(kMuted, 0.55f) : kMuted, nm);
        ImGui::PopFont();
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("WASD_TOOL", &t, sizeof(t));
            const std::string lbl = iconLabel(typeIcon(t), nm);
            alignedText(lbl.c_str(), kText);
            ImGui::EndDragDropSource();
        }
        ImGui::SetItemTooltip("%s", T(dim ? "Only one mouse-look allowed"
                                          : "Drag onto the game, or click then click on the game"));
        ImGui::PopID();
    }
    ImGui::Dummy({0, S(4)});
}


void App::drawToast() {
    if (toast_.empty() || nowMicros() > toastUntil_) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const char* s = V(toast_);
    const ImVec2 ts = ImGui::CalcTextSize(s);
    const float cx = gameArea_.x + gameArea_.w * 0.5f, y = gameArea_.y + gameArea_.h - S(64);
    const ImVec2 a{cx - ts.x * 0.5f - S(16), y}, b{cx + ts.x * 0.5f + S(16), y + ts.y + S(16)};
    dl->AddRectFilled(a, b, kBgSecondary, S(8));
    dl->AddRect(a, b, kAccent, S(8), 0, S(1.5f));
    dl->AddText({a.x + S(16), a.y + S(8)}, kText, s);
}

void App::drawDiscardPrompt() {
    if (!discardPrompt_) return;
    ImGui::OpenPopup(T("Unsaved changes##discard_modal"));
    const ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({S(340), 0});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(kBgSecondary));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(16), S(14)});
    if (ImGui::BeginPopupModal(T("Unsaved changes##discard_modal"), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(nullptr, 20.0f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), ICON_WARNING);
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kText), "%s", T("Unsaved changes"));
        ImGui::PopFont();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                           V(i18n::trf("Save changes to layout \"%s\"?", profile_.name.c_str())));
        if (!status_.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDanger), "%s", V(status_));
        ImGui::Spacing();
        int action = -1;
        switch (buttonRow({{ICON_SAVE, T("Save"), true, true}, {ICON_STOP, T("Don't save")}, {ICON_CLOSE, T("Cancel")}})) {
            case 0:
                if (saveProfile()) action = 0;
                break;  // save failed -> status_ shows the reason, modal stays open
            case 1:
                discardChanges();  // actually revert, not just mark clean
                action = 1;
                break;
            case 2: discardPrompt_ = false; discardAction_ = {}; ImGui::CloseCurrentPopup(); break;
            default: break;
        }
        if (action >= 0) {
            discardPrompt_ = false;
            const auto fn = std::move(discardAction_);
            discardAction_ = {};
            ImGui::CloseCurrentPopup();
            fn();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// Settings that apply on the next connection were changed - the only way out
// of the page is a reconnect. Pressing it reconnects and continues the
// navigation the user asked for.
void App::drawReconnectPrompt() {
    if (!reconnectPrompt_) return;
    ImGui::OpenPopup(T("Reconnect required##reconnect_modal"));
    const ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({S(340), 0});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(kBgSecondary));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(16), S(14)});
    if (ImGui::BeginPopupModal(T("Reconnect required##reconnect_modal"), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(nullptr, 20.0f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), ICON_WARNING);
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kText), "%s", T("Reconnect required"));
        ImGui::PopFont();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "%s",
                           T("To apply the new settings, reconnect to the device."));
        ImGui::Spacing();
        int action = -1;
        switch (buttonRow({{ICON_RESET, T("Reconnect"), true, true}, {ICON_CLOSE, T("Cancel")}})) {
            case 0:
                reconnectTo(serial_);
                action = 0;
                break;
            case 1:
                reconnectPrompt_ = false;
                reconnectAction_ = {};
                ImGui::CloseCurrentPopup();
                break;
            default: break;
        }
        if (action >= 0) {
            reconnectPrompt_ = false;
            const auto fn = std::move(reconnectAction_);
            reconnectAction_ = {};
            ImGui::CloseCurrentPopup();
            fn();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// Shared name-input modal: Save as / Rename / New layout all flow through the
// same dialog (beginNamePrompt sets the title + action callback).
void App::drawNamePrompt() {
    if (!namePrompt_.open) return;
    ImGui::OpenPopup("##name_modal");
    const ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({S(320), 0});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(kBgSecondary));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(16), S(14)});
    if (ImGui::BeginPopupModal("##name_modal", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kText), "%s", T(namePrompt_.title));
        if (namePrompt_.buf[0] == 0) ImGui::SetKeyboardFocusHere();
        std::string name = namePrompt_.buf;
        const bool entered = inputShaped("##nname", name, T("Layout name..."));
        std::snprintf(namePrompt_.buf, sizeof namePrompt_.buf, "%s", name.c_str());
        const bool valid = isValidProfileName(namePrompt_.buf);
        if (namePrompt_.buf[0] && !valid)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDanger), "%s", T("Invalid layout name"));
        int action = -1;
        switch (buttonRow({{ICON_SAVE, T("OK"), valid}, {ICON_CLOSE, T("Cancel")}})) {
            case 0: action = 0; break;
            case 1: action = 1; break;
            default: break;
        }
        if (entered && valid) action = 0;
        if (action == 0) {
            const std::string chosen = namePrompt_.buf;
            const auto fn = std::move(namePrompt_.action);
            namePrompt_ = {};
            ImGui::CloseCurrentPopup();
            fn(chosen);
        } else if (action == 1) {
            namePrompt_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::drawPanel() {
    const float W = static_cast<float>(renderer_.width()), H = static_cast<float>(renderer_.height());
    const float railW = S(34);  // icon-only rail, ~title-bar height
    const float pw = panelWidth(), x0 = rtl() ? W - pw : 0.0f;
    ImGui::SetNextWindowPos({x0, 0});
    ImGui::SetNextWindowSize({pw, H});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::Begin("##panel", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float railX = rtl() ? x0 + pw - railW : x0;
    dl->AddRectFilled({railX, 0}, {railX + railW, H}, kRailBg);
    const float edge = rtl() ? railX : railX + railW;
    dl->AddLine({edge, 0}, {edge, H}, kLine);
    if (!settings_.panelCollapsed) {
        const float outer = rtl() ? x0 : x0 + pw;
        dl->AddLine({outer - (rtl() ? 0 : 1), 0}, {outer - (rtl() ? 0 : 1), H}, kLine);
    }

    // Brand mark: geometric N logo, centered in the icon-only rail.
    {
        const float logoSz = S(22);
        const float lx = railX + (railW - logoSz) * 0.5f;
        drawNexplayLogo(dl, {lx, S(8)}, logoSz);
    }

    auto railItem = [&](const char* id, const char* icon, const char* labelVis, bool selected, float y) {
        const float padX = S(4), itemW = railW - padX * 2.0f, itemH = S(30);
        const ImVec2 p{railX + padX, y};
        ImGui::SetCursorScreenPos(p);
        const bool clicked = ImGui::InvisibleButton(id, {itemW, itemH});
        const bool hov = ImGui::IsItemHovered();
        if (selected || hov)
            dl->AddRectFilled(p, {p.x + itemW, p.y + itemH},
                              selected ? alpha(kAccent, g_darkTheme ? 0.20f : 0.14f) : alpha(kText, 0.06f), S(6));
        if (selected) {
            const float bx = rtl() ? railX + railW - S(2) : railX;
            dl->AddRectFilled({bx, p.y + S(6)}, {bx + S(2), p.y + itemH - S(6)}, kAccent, S(1));
        }
        ImGui::PushFont(nullptr, 15.5f);
        const ImVec2 is = ImGui::CalcTextSize(icon);
        const float iy = p.y + (itemH - is.y) * 0.5f;
        const float ix = p.x + (itemW - is.x) * 0.5f;
        dl->AddText({ix, iy}, selected ? kAccent : hov ? kText : kMuted, icon);
        ImGui::PopFont();
        if (labelVis && *labelVis) ImGui::SetItemTooltip("%s", labelVis);
        return clicked;
    };

    struct Item {
        Section s;
        const char* icon;
        const char* name;
    };
    const Item items[] = {
        {Section::Home, ICON_HOME, "Home"},
        {Section::Device, ICON_PHONE, "Devices"},
        {Section::Mapping, ICON_KEYBOARD, "Key mapping"},
        {Section::Macros, ICON_MACRO, "Automation"},  // one unified entry: macros + images + colors
        {Section::Detect, ICON_EYE, "Object detection"},
        {Section::Settings, ICON_SETTINGS, "Settings"},
    };
    const bool autoSel =
        section_ == Section::Macros || section_ == Section::Images || section_ == Section::Colors;
    for (int k = 0; k < 6; ++k) {
        const Item& it = items[k];
        const bool sel = (it.s == Section::Macros ? autoSel : section_ == it.s) && !settings_.panelCollapsed;
        if (railItem(it.name, it.icon, T(it.name), sel, S(42) + k * S(34))) {
            if (it.s == Section::Macros) {
                // Automation is the Studio now: the rail icon opens the
                // fullscreen workspace directly (no side panel).
                openStudio();
            } else if (sel) {
                settings_.panelCollapsed = true;
                fitWindowToVideo();
            } else {
                requestNav([this, s = it.s] {
                    section_ = s;
                    settings_.panelCollapsed = false;
                    fitWindowToVideo();
                });
            }
        }
    }

    // Android navigation buttons at the rail bottom: Back / Home / Recents.
    auto navRailItem = [&](const char* id, int kind, bool enabled, float y) {
        const float padX = S(4), itemW = railW - padX * 2.0f, itemH = S(28);
        const ImVec2 p{railX + padX, y};
        ImGui::SetCursorScreenPos(p);
        ImGui::BeginDisabled(!enabled);
        const bool clicked = ImGui::InvisibleButton(id, {itemW, itemH});
        const bool hov = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        if (hov && enabled) dl->AddRectFilled(p, {p.x + itemW, p.y + itemH}, alpha(kText, 0.06f), S(6));
        const ImU32 col = !enabled ? alpha(kMuted, 0.4f) : hov ? kText : kMuted;
        const ImVec2 c{p.x + itemW * 0.5f, p.y + itemH * 0.5f};
        const float s = S(7);
        if (kind == 0) {  // Back: left-pointing triangle outline
            dl->AddTriangle({c.x - s * 0.7f, c.y}, {c.x + s * 0.7f, c.y - s}, {c.x + s * 0.7f, c.y + s}, col, S(1.5f));
        } else if (kind == 1) {  // Home: circle outline
            dl->AddCircle(c, s * 0.95f, col, 0, S(1.5f));
        } else {  // Recents: square outline
            dl->AddRect({c.x - s * 0.75f, c.y - s * 0.75f}, {c.x + s * 0.75f, c.y + s * 0.75f}, col, S(1.3f), 0, S(1.5f));
        }
        if (hov && enabled)
            ImGui::SetItemTooltip("%s", T(kind == 0 ? "Back" : kind == 1 ? "Home" : "Recents"));
        return clicked;
    };
    const bool nav = sessionActive();
    const char* navIds[] = {"##nav_back", "##nav_home", "##nav_recents"};
    const uint32_t navKeys[] = {proto::keycode::Back, proto::keycode::Home, proto::keycode::AppSwitch};
    for (int k = 0; k < 3; ++k)
        if (navRailItem(navIds[k], k, nav, H - S(104) + k * S(32)) && nav) session_->sendKeycode(navKeys[k]);

    if (!settings_.panelCollapsed) {
        const float cx = rtl() ? x0 : x0 + railW;
        const float contentW = pw - railW;
        ImGui::SetCursorScreenPos({cx, 0});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPrimary);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(16), S(14)});
        ImGui::BeginChild("##content", {contentW, H}, ImGuiChildFlags_AlwaysUseWindowPadding);
        panelHeader();
        // Unified automation home: macros, reference images and color zones all
        // live behind one rail icon, switched with in-panel tabs. Re-evaluate the
        // section here (not the stale pre-click autoSel): a rail click that left
        // an auto section must not record a non-auto section into autoSec_.
        if (section_ == Section::Macros || section_ == Section::Images || section_ == Section::Colors) {
            const int t = buttonRow({{ICON_MACRO, T("Macros"), true, section_ == Section::Macros},
                                     {ICON_IMAGE, T("Images"), true, section_ == Section::Images},
                                     {ICON_COLOR, T("Colors"), true, section_ == Section::Colors}});
            if (t >= 0) {
                const Section target = t == 0 ? Section::Macros : t == 1 ? Section::Images : Section::Colors;
                requestDiscard([this, target] { section_ = target; });
            }
            autoSec_ = section_;
        }
        switch (section_) {
            case Section::Home: panelHome(); break;
            case Section::Device: panelDevice(); break;
            case Section::Mapping: panelMapping(); break;
            case Section::Macros: panelMacros(); break;
            case Section::Images: panelImages(); break;
            case Section::Colors: panelColors(); break;
            case Section::Detect: panelDetect(); break;
            case Section::Settings: panelSettings(); break;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // Resizable border drag handle on the outer edge of the panel
        const float handleX = rtl() ? x0 : x0 + pw - S(5);
        ImGui::SetCursorScreenPos({handleX, 0});
        ImGui::InvisibleButton("##panel_resize", {S(6), H});
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const float mouseX = ImGui::GetIO().MousePos.x;
            const float newTotal = rtl() ? (W - mouseX) : mouseX;
            const int targetPx = static_cast<int>((newTotal - railW) / g_scale);
            settings_.panelWidthPx = std::clamp(targetPx, 320, 640);
        }
    }
    ImGui::End();
}

void App::panelHeader() {
    const char* titles[] = {"Home",         "Devices",          "Key mapping", "Macros",
                            "Image recognition", "Color monitoring", "Object detection", "Settings"};
    ImGui::PushFont(nullptr, 20.0f);
    const char* title = T(titles[static_cast<int>(section_)]);
    const float th = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = availW();
    const float tw = ImGui::CalcTextSize(title).x;
    ImGui::GetWindowDrawList()->AddText({rtl() ? p.x + w - tw : p.x, p.y}, kText, title);
    ImGui::PopFont();
    // Session status pill on the opposite side.
    const SessionState s = session_ ? session_->state() : SessionState::Idle;
    const ImU32 dot = !session_ ? kMuted : s == SessionState::Running ? kOk : s == SessionState::Failed ? kDanger : kAmber;
    const std::string st = session_ ? i18n::tr(sessionStateName(s)) : i18n::tr("No device");
    const char* sv = V(locked_ ? st + " | " + i18n::tr("Mouse locked") : st);
    ImGui::PushFont(nullptr, 12.5f);
    const ImVec2 ts = ImGui::CalcTextSize(sv);
    const float pwid = ts.x + S(26), ph = ts.y + S(8);
    const float px = rtl() ? p.x : p.x + w - pwid, py = p.y + (th - ph) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({px, py}, {px + pwid, py + ph}, kBgSecondary, ph * 0.5f);
    dl->AddRect({px, py}, {px + pwid, py + ph}, kBorderCol, ph * 0.5f);
    const float dx = rtl() ? px + pwid - S(11) : px + S(11);
    dl->AddCircleFilled({dx, py + ph * 0.5f}, S(3.5f), dot);
    dl->AddText({rtl() ? px + S(8) : px + S(19), py + S(4)}, kText, sv);
    ImGui::PopFont();
    ImGui::Dummy({w, th + S(6)});
    if (!status_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgSecondary);
        ImGui::BeginChild("##status", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
        paragraph(status_, kText);
        if (button({ICON_CLOSE, T("Dismiss")}, availW())) status_.clear();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

void App::panelHome() {
    // Connection summary card
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgSecondary);
    ImGui::BeginChild("##home_conn", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    const SessionState s = session_ ? session_->state() : SessionState::Idle;
    labelValue(T("Status"), V(session_ ? i18n::tr(sessionStateName(s)) : i18n::tr("No device")), kMuted,
               s == SessionState::Running ? kOk : kAmber);
    if (session_ && s == SessionState::Running) {
        labelValue(T("Device"), V(session_->deviceName()), kMuted, kText);
        char res[48];
        std::snprintf(res, sizeof res, "%ux%u  |  %.0f FPS", session_->frameWidth(), session_->frameHeight(),
                      presentFps_.rate(nowMicros()));
        labelValue(T("Video"), res, kMuted, kAccentCyan);
        if (button({ICON_CLOSE, T("Disconnect")}, availW())) disconnect();
    } else {
        std::vector<platform::AdbDevice> devs;
        {
            std::lock_guard lk(devMu_);
            devs = devices_;
        }
        bool connectedAny = false;
        for (const auto& d : devs) {
            if (d.usable()) {
                labelValue(T("Device"), V(d.model.empty() ? d.serial : d.model), kMuted, kOk);
                if (button({ICON_CONNECT, T("Connect"), true, true}, availW())) connect(d.serial);
                connectedAny = true;
                break;
            }
        }
        if (!connectedAny) {
            paragraph(i18n::tr("Connect your phone by USB with USB debugging enabled."), kMuted);
            if (button({ICON_PHONE, T("Open Devices")}, availW())) section_ = Section::Device;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Active Layout & Automation summary card
    sectionTitle(T("Active profile"));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgSecondary);
    ImGui::BeginChild("##home_prof", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    labelValue(T("Layout"), V(hasUnsavedChanges() ? profile_.name + " *" : profile_.name), kMuted, kAccent);
    char cnt[32];
    std::snprintf(cnt, sizeof cnt, "%d", static_cast<int>(profile_.bindings.size()));
    labelValue(T("Key controls"), cnt, kMuted, kText);
    std::snprintf(cnt, sizeof cnt, "%d", static_cast<int>(profile_.automation.macros.size()));
    labelValue(T("Macros"), cnt, kMuted, kText);
    std::snprintf(cnt, sizeof cnt, "%d / %d", static_cast<int>(profile_.automation.images.size()), kMaxImages);
    labelValue(T("Reference images"), cnt, kMuted, kText);
    std::snprintf(cnt, sizeof cnt, "%d", static_cast<int>(profile_.automation.zones.size()));
    labelValue(T("Color zones"), cnt, kMuted, kText);
    switch (buttonRow({{ICON_EDIT, editMode_ ? T("Finish editing (F2)") : T("Edit keys (F2)"), true, true},
                       {ICON_SAVE, T("Save"), hasUnsavedChanges()}})) {
        case 0: setEditMode(!editMode_); break;
        case 1: saveProfile(); break;
        default: break;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Quick navigation cards
    sectionTitle(T("Quick access"));
    switch (buttonRow({{ICON_KEYBOARD, T("Key mapping")}, {ICON_MACRO, T("Automation Studio")}})) {
        case 0: requestDiscard([this] { section_ = Section::Mapping; }); break;
        case 1: openStudio(); break;
        default: break;
    }
    switch (buttonRow({{ICON_IMAGE, T("Image recognition")}, {ICON_COLOR, T("Color monitoring")}})) {
        case 0: openStudio(); if (studio_) studio_->focusInspectorTab(3); break;
        case 1: openStudio(); if (studio_) studio_->focusInspectorTab(4); break;
        default: break;
    }
    switch (buttonRow({{ICON_VIDEO, T("Video & Display")}, {ICON_PERF, T("Performance")}})) {
        case 0: requestDiscard([this] { section_ = Section::Settings, settingsTab_ = 0; }); break;
        case 1: requestDiscard([this] { section_ = Section::Settings, settingsTab_ = 1; }); break;
        default: break;
    }
}

void App::panelSettings() {
    // Sub-navigation tabs inside Settings (General, Metrics, Logs, Help)
    const int tabClick = buttonRow({
        {ICON_SETTINGS, T("General"), true, settingsTab_ == 0},
        {ICON_PERF, T("Metrics"), true, settingsTab_ == 1},
        {ICON_LOGS, T("Logs"), true, settingsTab_ == 2},
        {ICON_HELP, T("Help"), true, settingsTab_ == 3},
    });
    if (tabClick >= 0) settingsTab_ = tabClick;
    ImGui::Dummy({0, S(4)});
    switch (settingsTab_) {
        case 0: panelGeneral(); break;
        case 1: panelPerformance(); break;
        case 2: panelLogs(); break;
        case 3: panelHelp(); break;
    }
}

// WASD+-style settings: one scrolling page with two groups - general and
// performance - using labeled rows, checkboxes, dropdowns and sliders.
void App::panelGeneral() {
    char v[64];
    sectionTitle(T("General"));
    labelValue(T("Version"), "v1.0  |  NEXPLAY", kMuted, kAccentCyan);
    {
        int lang = i18n::rtl() ? 0 : 1;
        const char* items[] = {V("العربية"), "English"};
        if (comboRow(T("Language"), items[lang], items, 2, &lang, S(150))) setLanguage(lang == 0 ? "ar" : "en");
    }
    sectionTitle(T("Performance"));
    paragraph(i18n::tr("Changes apply on the next connection."), kMuted);
    // Dropdowns first, then sliders, then checkboxes - one visual rhythm.
    {
        int pi = -1;
        for (int i = 0; i < static_cast<int>(std::size(kPresets)); ++i)
            if (settings_.maxSize == kPresets[i].maxSize && settings_.bitRate == kPresets[i].bitRate &&
                settings_.maxFps == kPresets[i].maxFps)
                pi = i;
        const char* items[] = {T(kPresets[0].name), T(kPresets[1].name), T(kPresets[2].name), T("Custom")};
        int tmp = pi < 0 ? 3 : pi;
        if (comboRow(T("Quality preset"), items[tmp], items, 4, &tmp, S(170)) && tmp < 3) {
            settings_.preset = tmp;
            settings_.maxSize = kPresets[tmp].maxSize;
            settings_.bitRate = kPresets[tmp].bitRate;
            settings_.maxFps = kPresets[tmp].maxFps;
            pendingSettings_ = true;
            saveSettings();
        }
    }
    {
        const char* items[] = {T("Competitive (lowest latency)"), T("Smooth (jitter buffer)")};
        int lm = settings_.displayMode;
        if (comboRow(T("Latency mode"), items[lm], items, 2, &lm, S(200))) {
            settings_.displayMode = lm;
            pending_.setMaxDepth(lm == 1 ? 2 : 1);
            pendingSettings_ = true;
            saveSettings();
        }
    }
    {
        static const uint32_t resSizes[] = {0, 800, 960, 1024, 1280, 1600, 1920};
        static const char* resNames[] = {"Native", "480P", "540P", "640P", "720P", "900P", "1080P"};
        constexpr int n = static_cast<int>(std::size(resSizes));
        int ri = 0;
        for (int i = 0; i < n; ++i)
            if (resSizes[i] == settings_.maxSize) ri = i;
        if (segSlider(T("Video resolution"), T(resNames[ri]), n, &ri)) {
            settings_.maxSize = resSizes[ri];
            pendingSettings_ = true;
            saveSettings();
        }
    }
    {
        static const uint32_t fpsVals[] = {0, 30, 45, 60, 90, 120, 144};
        constexpr int n = static_cast<int>(std::size(fpsVals));
        int fi = 0;
        for (int i = 0; i < n; ++i)
            if (fpsVals[i] == settings_.maxFps) fi = i;
        char fv[24];
        if (fpsVals[fi]) std::snprintf(fv, sizeof fv, "%u FPS", fpsVals[fi]);
        else std::snprintf(fv, sizeof fv, "%s", T("Unlimited"));
        if (segSlider(T("Max FPS"), fv, n, &fi)) {
            settings_.maxFps = fpsVals[fi];
            pendingSettings_ = true;
            saveSettings();
        }
    }
    {
        static const int brVals[] = {1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 60};
        constexpr int n = static_cast<int>(std::size(brVals));
        const int mbps = static_cast<int>((settings_.bitRate + 500000) / 1000000);
        int bi = 0;
        for (int i = 0; i < n; ++i)
            if (std::abs(brVals[i] - mbps) < std::abs(brVals[bi] - mbps)) bi = i;
        std::snprintf(v, sizeof v, "%d Mbps", brVals[bi]);
        if (segSlider(T("Bitrate"), v, n, &bi)) {
            settings_.bitRate = static_cast<uint32_t>(brVals[bi]) * 1000000u;
            pendingSettings_ = true;
            saveSettings();
        }
    }
    std::snprintf(v, sizeof v, "%d%%", settings_.overlayOpacity);
    if (sliderRow(T("Opacity"), v, [&] { return ImGui::SliderInt("##ovop", &settings_.overlayOpacity, 0, 100, ""); }))
        saveSettings();
    if (checkRow(T("Dark theme"), &settings_.darkTheme)) {
        applyTheme();
        saveSettings();
    }
    if (checkRow(T("FPS"), &settings_.showFps)) saveSettings();
    {
        bool fb = settings_.overlayOpacity > 0;
        if (checkRow(T("Show key positions"), &fb)) {
            settings_.overlayOpacity = fb ? 65 : 0;
            saveSettings();
        }
    }
    if (checkRow(T("Show floating buttons only in landscape"), &settings_.overlayLandscapeOnly)) saveSettings();
    if (checkRow(T("Show touches on phone"), &settings_.showTouches)) { pendingSettings_ = true; saveSettings(); }
    if (checkRow(T("Hardware decoding (D3D11VA)"), &settings_.hardwareDecode)) { pendingSettings_ = true; saveSettings(); }
    if (checkRow(T("H.265 (if supported by the phone)"), &settings_.h265)) { pendingSettings_ = true; saveSettings(); }
    if (checkRow(T("VSync (off = lower latency)"), &settings_.vsync)) { pendingSettings_ = true; saveSettings(); }
    if (checkRow(T("Auto reconnect"), &settings_.autoReconnect)) saveSettings();
    if (!renderer_.tearingSupported()) paragraph(i18n::tr("Tearing not supported: present waits for vblank."), kMuted);
    if (session_) {
        ImGui::Dummy({0, S(4)});
        if (button({ICON_RESET, T("Reconnect with new settings"), true, pendingSettings_}, availW()))
            reconnectTo(serial_);
    }
}

void App::panelMapping() {
    // Everything fits the panel without scrolling; the action bar is pinned at
    // the bottom. Entering this section IS edit mode (auto-enabled in frame()).
    const float barH = ImGui::GetFrameHeight() + S(36);
    ImGui::BeginChild("##map_body", {0, -barH}, 0, ImGuiWindowFlags_NoScrollbar);

    // Layout picker + overflow menu on one compact row.
    const bool unsaved = hasUnsavedChanges();
    const float moreW = ImGui::CalcTextSize(T("More")).x + S(30);
    ImGui::SetNextItemWidth(std::max(S(60), availW() - moreW - ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::BeginCombo("##profiles", V(unsaved ? profile_.name + " *" : profile_.name))) {
        for (auto& n : profileNames_)
            if (ImGui::Selectable(V(n), n == profile_.name) && n != profile_.name)
                requestDiscard([this, n] { loadProfile(n); });
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (button({nullptr, T("More")}, moreW)) ImGui::OpenPopup("##layout_more");
    if (ImGui::BeginPopup("##layout_more")) {
        const float w = S(170);
        if (ImGui::Selectable(iconLabel(ICON_SAVEAS, T("Save as")).c_str(), false, 0, {w, 0}))
            beginNamePrompt(T("Save layout as"), [this](const std::string& n) { saveProfileAs(n); });
        if (ImGui::Selectable(iconLabel(ICON_PAGE, T("New layout")).c_str(), false, 0, {w, 0}))
            beginNamePrompt(T("New layout"), [this](const std::string& n) { newProfile(n); });
        if (ImGui::Selectable(iconLabel(ICON_RENAME, T("Rename")).c_str(), false, 0, {w, 0}))
            beginNamePrompt(T("Rename layout"), [this](const std::string& n) { renameProfile(n); });
        if (ImGui::Selectable(iconLabel(ICON_RESET, T("Restore default")).c_str(), false, 0, {w, 0})) resetProfileToDefault();
        ImGui::EndPopup();
    }

    // Drag-&-drop tool palette (circles) - this page is edit mode itself.
    if (editMode_) drawToolPalette();

    // ---- Detail view: a selected control owns the rest of the page ----------
    if (selectedBinding_ >= 0 && selectedBinding_ < static_cast<int>(profile_.bindings.size())) {
        const Binding& sel = profile_.bindings[selectedBinding_];
        const char* backIcon = rtl() ? ICON_CHEV_RIGHT : ICON_CHEV_LEFT;
        if (button({backIcon, T("All controls")}, S(120))) selectedBinding_ = -1;
        else {
            ImGui::SameLine(0, S(8));
            const std::string who =
                iconLabel(typeIcon(sel.type), sel.name.empty() ? T(typeName(sel.type)) : sel.name.c_str());
            alignedText(who.c_str(), kText);
            // The inspector scrolls inside its own region - the page never does.
            ImGui::BeginChild("##detail_body", {0, 0});
            panelSelected();
            ImGui::EndChild();
        }
    } else {
        // ---- List view ------------------------------------------------------
        if (profile_.bindings.empty()) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgSecondary);
            ImGui::BeginChild("##no_ctrls", {0, 0},
                              ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
            paragraph(i18n::tr("No controls yet. Drag a type onto the game, or click a circle then click the spot."), kMuted);
            ImGui::EndChild();
            ImGui::PopStyleColor();
        } else {
            const float limitsH = ImGui::GetFrameHeight() + S(16);
            const float listH = std::max(S(80), ImGui::GetContentRegionAvail().y - limitsH - ImGui::GetStyle().ItemSpacing.y);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgSecondary);
            ImGui::BeginChild("##ctrl_list", {0, listH}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        for (int i = 0; i < static_cast<int>(profile_.bindings.size()); ++i) {
            const Binding& b = profile_.bindings[i];
            ImGui::PushID(i);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = availW(), h = S(32);
            // Selecting a row always opens its inspector - edit mode is only
            // needed to move markers on the screen, not to review properties.
            if (ImGui::InvisibleButton("##row", {w, h})) selectedBinding_ = (selectedBinding_ == i ? -1 : i);
            const bool hov = ImGui::IsItemHovered(), sel = i == selectedBinding_;
            if (hov) hoveredListBinding_ = i;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (sel || hov) dl->AddRectFilled(p, {p.x + w, p.y + h}, sel ? alpha(kAccent, 0.18f) : alpha(kText, 0.06f), S(6));
            if (sel) {
                const float bx = rtl() ? p.x + w - S(3) : p.x;
                dl->AddRectFilled({bx, p.y + S(5)}, {bx + S(3), p.y + h - S(5)}, kAccent, S(2));
            }
            const char* icon = typeIcon(b.type);
            const ImVec2 is = ImGui::CalcTextSize(icon);
            const float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
            const char* name = V(b.name.empty() ? std::string(i18n::tr(typeName(b.type))) : b.name);
            const std::string keys = keySummary(b);
            const bool waitingRow = capture_.active() && capTarget_.binding == i;
            const char* kv = waitingRow ? "..." : V(keys);
            const float kw = ImGui::CalcTextSize(kv).x + S(14);
            const float nw = ImGui::CalcTextSize(name).x;
            float ix, nx, kx;
            if (rtl()) ix = p.x + w - S(10) - is.x, nx = ix - S(10) - nw, kx = p.x + S(6);
            else ix = p.x + S(10), nx = ix + is.x + S(10), kx = p.x + w - S(6) - kw;
            const bool rapidGreen = b.type == BindingType::Fire && b.rapidFire;
            dl->AddText({ix, ty}, rapidGreen ? kOk : sel ? kAccent : kMuted, icon);
            dl->PushClipRect({rtl() ? kx + kw : p.x, p.y}, {rtl() ? p.x + w : kx, p.y + h}, true);
            dl->AddText({nx, ty}, rapidGreen ? kOk : kText, name);
            dl->PopClipRect();
            dl->AddRectFilled({kx, p.y + S(5)}, {kx + kw, p.y + h - S(5)},
                              waitingRow ? alpha(kAmber, 0.2f) : rapidGreen ? alpha(kOk, 0.45f) : kSurfaceElev, S(5));
            dl->AddText({kx + S(7), ty}, waitingRow ? kAmber : rapidGreen ? kOk : kAccentCyan, kv);
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

        // Compact safety limit (the mouse-lock key lives in the Mouse look inspector).
        sliderNumI(T("Max virtual fingers"), &profile_.maxVirtualFingers, 1, 16, "%d", 1);
        ImGui::SetItemTooltip("%s", T("Simultaneous virtual touches limit; extra presses are ignored instead of sticking."));
    }
    ImGui::EndChild();

    // Pinned action bar: undo / redo / explicit Save / Cancel.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgSecondary);
    ImGui::BeginChild("##map_bar", {0, 0},
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    const bool dirtyBar = hasUnsavedChanges();
    const bool canUndoBar = history_.canUndo() || !(profile_ == history_.base());
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float ib = ImGui::GetFrameHeight() + S(2);
    const float bw = (availW() - ib * 2 - sp * 3) * 0.5f;
    if (iconButton("undo", ICON_UNDO, T("Undo (Ctrl+Z)"), canUndoBar)) undo();
    ImGui::SameLine(0, sp);
    if (iconButton("redo", ICON_REDO, T("Redo (Ctrl+Y)"), history_.canRedo())) redo();
    ImGui::SameLine(0, sp);
    if (button({ICON_SAVE, T("Save"), dirtyBar}, bw)) saveProfile();
    ImGui::SameLine(0, sp);
    if (button({ICON_CLOSE, T("Cancel"), dirtyBar}, bw)) discardChanges();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::panelSelected() {
    const int i = selectedBinding_;
    Binding& b = profile_.bindings[i];
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgSecondary);
    ImGui::BeginChild("##sel_card", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);

    // Header row with type badge and Close button
    labelValue(T("Type"), iconLabel(typeIcon(b.type), T(typeName(b.type))).c_str(), kMuted, kAccent);
    alignedText(T("Name"), kText);
    inputShaped("##bname", b.name, T("Name"));
    {
        bool enabled = !b.disabled;
        if (toggleRow(T("Enabled"), &enabled)) b.disabled = !enabled;
        if (b.disabled) alignedText(T("This control is disabled"), kAmber);
    }

    // Group: Trigger Key
    auto waitingSlot = [&](int slot) { return capture_.active() && capTarget_.binding == i && capTarget_.slot == slot; };
    auto chip = [&](const char* id, int slot, int vk, float w) {
        const bool wtg = waitingSlot(slot);
        if (keyChip(id, vk, wtg, w)) wtg ? finishCapture(VK_LBUTTON) : beginCapture(i, slot, false);
    };
    if (b.type == BindingType::Tap || b.type == BindingType::Drag || b.type == BindingType::Fire) {
        alignedText(T(b.type == BindingType::Fire ? "AIM / Fire key" : "Key"), kText);
        // Primary key (or combined A + B display when key2 is set)
        {
            const bool wtg0 = waitingSlot(0);
            const std::string mainLabel = wtg0 ? (comboFirstVk_ ? (keyText(comboFirstVk_) + " + ...") : std::string(T("Press 1 or 2 keys together...")))
                                               : bindingKeyText(b);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = availW(), h = ImGui::GetFrameHeight() + S(2);
            if (ImGui::InvisibleButton("##mainkey", {w, h})) wtg0 ? finishCapture(VK_LBUTTON) : beginCapture(i, 0, false);
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 q{p.x + w, p.y + h};
            dl->AddRectFilled(p, q, wtg0 ? alpha(kAmber, 0.16f) : kSurfaceElev, S(6));
            dl->AddRect(p, q, wtg0 ? alpha(kAmber, 0.55f + 0.45f * pulse()) : (hov ? kAccent : kBorderCol), S(6), 0, wtg0 ? S(1.5f) : S(1));
            const ImVec2 ts = ImGui::CalcTextSize(mainLabel.c_str());
            dl->AddText({p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f}, wtg0 ? kAmber : b.key ? kAccentCyan : kMuted, mainLabel.c_str());
            if (wtg0) addCaptureRect(p, q);
            if (!wtg0) ImGui::SetItemTooltip("%s", T("Click, then press 1 key or hold 2 keys together (e.g. H + T)"));
        }
    } else if (b.type == BindingType::Joystick) {
        paragraph(i18n::tr("Drag the stick marker on the screen to place its center."), kMuted);
        alignedText(T("Direction keys"), kText);
        const float cw = S(70), sp = ImGui::GetStyle().ItemSpacing.x;
        const float x0 = ImGui::GetCursorPosX() + std::max(0.0f, (availW() - (cw * 3 + sp * 2)) * 0.5f);
        ImGui::SetCursorPosX(x0 + cw + sp);
        chip("up", 1, b.keyUp, cw);
        ImGui::SetCursorPosX(x0);
        chip("left", 2, b.keyLeft, cw);
        ImGui::SameLine();
        chip("down", 3, b.keyDown, cw);
        ImGui::SameLine();
        chip("right", 4, b.keyRight, cw);
    } else {
        paragraph(i18n::trf("Moves the camera with the mouse while the mouse is locked (press %s).", keyName(profile_.lockToggleKey).c_str()),
                  kMuted);
    }

    // Group: Behavior
    if (b.type == BindingType::Tap) {
        alignedText(T("Key press mode"), kText);
        const int modeClick = buttonRow({
            {ICON_TAP, T("Hold"), true, !b.quickTap && !b.tapToggle},
            {ICON_TAP, T("Quick tap"), true, b.quickTap},
            {ICON_TAP, T("Toggle finger"), true, b.tapToggle},
        });
        if (modeClick == 0) b.quickTap = false, b.tapToggle = false;
        else if (modeClick == 1) b.quickTap = true, b.tapToggle = false;
        else if (modeClick == 2) b.tapToggle = true, b.quickTap = false;
        if (b.tapToggle)
            paragraph(i18n::tr("One press lands the finger on the screen; the next press lifts it."), kMuted);
        if (b.quickTap || b.tapRepeat > 1) sliderNumI(T("Tap hold duration (ms)"), &b.tapHoldMs, 15, 300, "%d ms", 5);
        if (!b.tapToggle) {
            sliderNumI(T("Tap repeat count"), &b.tapRepeat, 1, 10, "x%d", 1);
            if (b.tapRepeat > 1) paragraph(i18n::tr("Each press emits this many quick taps in a row."), kMuted);
        }
    }
    if (b.type == BindingType::Joystick) {
        float rp = b.radius * 100.0f;
        if (sliderNumF(T("Range %"), &rp, 2.0f, 45.0f, "%.1f%%", 1.0f))
            b.radius = std::clamp(rp / 100.0f, 0.02f, 0.45f);
        // Movement feel as a visible 3-way choice instead of a buried combo.
        alignedText(T("Movement style"), kText);
        const int jp = buttonRow({{ICON_TAP, T("Instant"), true, b.joyProfile == 0},
                                  {ICON_TAP, T("Smooth"), true, b.joyProfile == 1},
                                  {ICON_TAP, T("Natural"), true, b.joyProfile == 2}});
        if (jp >= 0) {
            b.joyProfile = jp;
            b.joyHumanize = jp == 1;
        }
        paragraph(i18n::tr(b.joyProfile == 0 ? "The stick jumps straight to the target - fastest response."
                             : b.joyProfile == 1 ? "The stick glides with a soft, gamepad-like curve."
                                                 : "The stick walks a stepped path like a real thumb."),
                  kMuted);
    }
    // Extended parameters hide behind one toggle to keep the page light.
    if (b.type == BindingType::Joystick) toggleRow(T("Advanced settings"), &bindingAdvOpen_);
    if (b.type == BindingType::Joystick && bindingAdvOpen_) {
        float upPct = b.joyUpOffset * 100.0f;
        if (sliderNumF(T("Up offset (0 ~ 50%)"), &upPct, 0.0f, 50.0f, "%.1f%%", 1.0f))
            b.joyUpOffset = std::clamp(upPct / 100.0f, 0.0f, 0.50f);

        sliderNumF(T("Deflection angle (15° ~ 75°)"), &b.joyAngleDeg, 15.0f, 75.0f, "%.0f°", 1.0f);
        sliderNumI(T("Release delay (0 ~ 200ms)"), &b.joyReleaseDelayMs, 0, 200, "%d ms", 5);
        sliderNumI(T("Step interval (16 ~ 100ms)"), &b.joyIntervalMs, 16, 100, "%d ms", 1);
        toggleRow(T("Enable Shift modifier (Walk / Sprint)"), &b.joyShiftEnabled);
        if (b.joyShiftEnabled) {
            alignedText(T("Modifier key"), kMuted);
            chip("jshift", 11, b.joyShiftKey, availW());
            float shiftPct = b.joyShiftOffset * 100.0f;
            if (sliderNumF(T("Shift offset (-45% ~ 45%)"), &shiftPct, -45.0f, 45.0f, "%+.1f%%", 1.0f))
                b.joyShiftOffset = std::clamp(shiftPct / 100.0f, -0.45f, 0.45f);
        }

        sliderNumF(T("Step length (0.01 ~ 0.20)"), &b.joyStepLength, 0.01f, 0.20f, "%.2f", 0.01f);
        {
            float px = b.pos.x * 100.0f, py = b.pos.y * 100.0f;
            if (sliderNumF(T("Center X %"), &px, 0.0f, 100.0f, "%.1f%%", 0.5f)) b.pos.x = std::clamp(px / 100.0f, 0.0f, 1.0f);
            if (sliderNumF(T("Center Y %"), &py, 0.0f, 100.0f, "%.1f%%", 0.5f)) b.pos.y = std::clamp(py / 100.0f, 0.0f, 1.0f);
        }
        // The movement style itself is chosen in the basic section above; here
        // only the per-style tunables remain.
        if (b.joyProfile == 1) {
            sliderNumF(T("Move speed (0.25x ~ 4.0x)"), &b.joySpeed, 0.25f, 4.0f, "%.2fx", 0.05f);
            toggleRow(T("Enable micro-shake"), &b.joyShake);
            if (b.joyShake) {
                sliderNumI(T("Shake period (100 ~ 500ms)"), &b.joyShakePeriodMs, 100, 500, "%d ms", 10);
                sliderNumF(T("Shake amplitude (0.1 ~ 10)"), &b.joyShakeAmp, 0.10f, 10.0f, "%.2f", 0.1f);
            }
            sliderNumF(T("Random offset (0.1 ~ 1.0)"), &b.joyRandomOffset, 0.10f, 1.0f, "%.2f", 0.05f);
        } else if (b.joyProfile == 2) {
            sliderNumI(T("Initial transition (0 ~ 60ms)"), &b.joyNatInitialMs, 0, 60, "%d ms", 1);
            sliderNumI(T("Update cadence (8 ~ 60ms)"), &b.joyNatSegMs, 8, 60, "%d ms", 1);
            sliderNumI(T("Move duration (20 ~ 400ms)"), &b.joyNatDurMs, 20, 400, "%d ms", 5);
            sliderNumF(T("Segment spacing px (0 = auto)"), &b.joyNatSpacingPx, 0.0f, 40.0f, "%.1f px", 0.5f);
            sliderNumI(T("Timing variation ±%"), &b.joyNatTimeVarPct, 0, 50, "%d %%", 1);
            sliderNumF(T("Path variation px"), &b.joyNatPathVarPx, 0.0f, 10.0f, "%.1f px", 0.1f);
            ImGui::SetNextItemWidth(-FLT_MIN);
            const char* eases[] = {T("Linear"), T("Ease-out"), T("Ease-in-out")};
            ImGui::Combo(T("Interpolation"), &b.joyNatEase, eases, 3);
        }
    }
    if (b.type == BindingType::Camera) {
        // ---- Aim: the three things a normal user actually touches ------------
        sectionTitle(T("Aim"));
        paragraph(i18n::trf("Mouse-look while locked (%s). Drag the ring on the screen to place the aim anchor.",
                            keyName(profile_.lockToggleKey).c_str()),
                  kMuted);
        // Single sensitivity slider by default; "Separate Y" reveals the second
        // axis, and a loaded profile whose X/Y differ opens in separate mode.
        const bool sepY = camSepSensY_ || (b.sensitivity != b.sensitivityY);
        if (!sepY) {
            float s = b.sensitivity;
            if (sliderNumF(T("Sensitivity"), &s, 0.1f, 2.0f, "%.2f", 0.01f)) b.sensitivity = b.sensitivityY = s;
            if (ImGui::SmallButton(T("Separate X / Y"))) camSepSensY_ = true;
        } else {
            sliderNumF(T("Sensitivity X"), &b.sensitivity, 0.1f, 2.0f, "%.2f", 0.01f);
            sliderNumF(T("Sensitivity Y"), &b.sensitivityY, 0.1f, 2.0f, "%.2f", 0.01f);
            if (ImGui::SmallButton(T("Link X = Y"))) {
                b.sensitivityY = b.sensitivity;
                camSepSensY_ = false;
            }
        }
        toggleRow(T("Invert Y axis"), &b.invertY);
        alignedText(T("Lock / unlock key"), kText);
        const bool waitLock = capture_.active() && capTarget_.slot == 5;
        if (keyChip("camlock", profile_.lockToggleKey, waitLock, availW()))
            waitLock ? finishCapture(VK_LBUTTON) : beginCapture(-1, 5, false);
        if (button({ICON_CLOSE, T("Reset aim")}, availW())) {
            b.pos = {0.5f, 0.5f};
            b.sensitivity = b.sensitivityY = 1.0f;
            b.invertY = false;
        }

        // ---- Everything else: one collapsed advanced group ------------------
        toggleRow(T("Advanced aim settings"), &bindingAdvOpen_);
        if (bindingAdvOpen_) {
            sectionTitle(T("Aim area"));
            const int area = buttonRow({{ICON_TAP, T("Roam ring"), true, !b.camBoundsEnabled},
                                        {ICON_TAP, T("Fixed box"), true, b.camBoundsEnabled}});
            if (area == 0) b.camBoundsEnabled = false;
            else if (area == 1) b.camBoundsEnabled = true;
            // One size pair drives whichever shape is active: ring radius or box
            // half-extent. Width 0 = same as height (circle / square).
            float* hField = b.camBoundsEnabled ? &b.camBoundsRange : &b.radius;
            float* wField = b.camBoundsEnabled ? &b.camBoundsRangeX : &b.radiusX;
            float hPct = *hField * 100.0f, wPct = *wField * 100.0f;
            if (sliderNumF(T("Area height %"), &hPct, 2.0f, 50.0f, "%.1f%%", 1.0f))
                *hField = std::clamp(hPct / 100.0f, 0.02f, 0.50f);
            if (sliderNumF(T("Area width % (0 = same)"), &wPct, 0.0f, 50.0f, "%.1f%%", 1.0f))
                *wField = std::clamp(wPct / 100.0f, 0.0f, 0.50f);
            paragraph(i18n::tr(b.camBoundsEnabled ? "The aim point is blocked from leaving this box around the anchor."
                                                  : "Past the ring edge the finger lifts and lands back on the anchor."),
                      kMuted);

            sectionTitle(T("Anchor position"));
            {
                float cx = b.pos.x * 100.0f, cy = b.pos.y * 100.0f;
                if (sliderNumF(T("Center X %"), &cx, 0.0f, 100.0f, "%.1f%%", 0.5f)) b.pos.x = std::clamp(cx / 100.0f, 0.0f, 1.0f);
                if (sliderNumF(T("Center Y %"), &cy, 0.0f, 100.0f, "%.1f%%", 0.5f)) b.pos.y = std::clamp(cy / 100.0f, 0.0f, 1.0f);
            }

            sectionTitle(T("Auto release"));
            sliderNumI(T("Release after idle (ms)"), &b.camAutoReleaseMs, 0, 500, "%d ms", 10);
            paragraph(i18n::tr("Lifts the aim finger after this much idle time; the next mouse motion lands it "
                               "back on the anchor. 0 = keep the finger held."),
                      kMuted);

            sectionTitle(T("Return to center"));
            const int rt = buttonRow({{ICON_TAP, T("Off"), true, b.camReturnType == 0},
                                      {ICON_TAP, T("Smooth"), true, b.camReturnType == 2},
                                      {ICON_TAP, T("Instant"), true, b.camReturnType == 1},
                                      {ICON_TAP, T("Type P"), true, b.camReturnType == 3}});
            if (rt == 0) b.camReturnType = 0;
            else if (rt == 1) b.camReturnType = 2;
            else if (rt == 2) b.camReturnType = 1;
            else if (rt == 3) b.camReturnType = 3;
            if (b.camReturnType != 0) {
                sliderNumI(T("Return delay (ms)"), &b.camReturnDelayMs, 0, 5000, "%d ms", 50);
                paragraph(i18n::tr("How long the aim must sit still before gliding back to the anchor."), kMuted);
            }

            sectionTitle(T("Input smoothing"));
            sliderNumI(T("Averaging (ms)"), &b.camAverageMs, 0, 200, "%d ms", 5);
            paragraph(i18n::tr("Averages mouse motion over this window to reduce jitter. 0 = no added delay."), kMuted);

            sectionTitle(T("Auto fire"));
            bool hasFire = false;
            for (const Binding& e : profile_.bindings) hasFire |= e.type == BindingType::Fire;
            if (!hasFire) paragraph(i18n::tr("Add a Smart Fire control to use Auto Fire."), kAmber);
            toggleRow(T("Enable auto fire"), &b.camAutoFire);
            if (b.camAutoFire && !hasFire) b.camAutoFire = false;
            if (b.camAutoFire) {
                sliderNumI(T("Pulse duration (ms)"), &b.camAutoFireReleaseMs, 10, 2000, "%d ms", 10);
                paragraph(i18n::tr("Repeats press/release on the Smart Fire point while aiming. Always released on "
                                   "unlock, profile switch or disconnect."),
                          kMuted);
            }

            sectionTitle(T("Notes"));
            inputShaped("##camcomment", b.comment, V("Optional notes about this mouse-look setup..."));
        }
    }
    if (b.type == BindingType::Drag) sliderNumI(T("Swipe duration"), &b.durationMs, 20, 2000, "%d ms", 10);
    if (b.type == BindingType::Fire) {
        sectionTitle(T("Recoil settings"));
        toggleRow(T("Enable recoil compensation"), &b.recoilEnabled);

        alignedText(T("Toggle ON/OFF hotkey (optional)"), kMuted);
        chip("rectog", 8, b.recoilToggleKey, availW());
        if (b.recoilToggleKey && ImGui::SmallButton(T("No key"))) b.recoilToggleKey = 0;

        // 4-direction speed sliders (0 .. 100) with direct numeric entry
        sliderNumI(T("Down (pull down)"), &b.recoilDown, 0, 100, "%d / 100", 1);
        sliderNumI(T("Up (pull up)"), &b.recoilUp, 0, 100, "%d / 100", 1);
        sliderNumI(T("Left"), &b.recoilLeft, 0, 100, "%d / 100", 1);
        sliderNumI(T("Right"), &b.recoilRight, 0, 100, "%d / 100", 1);
        sliderNumI(T("Recoil delay (ms)"), &b.recoilDelayMs, 0, 1000, "%d ms", 10);

        // Bullseye Target Preview showing live recoil pull direction & magnitude
        {
            const float boxH = S(96), boxW = availW();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, {p0.x + boxW, p0.y + boxH}, kSurfaceElev, S(8));
            dl->AddRect(p0, {p0.x + boxW, p0.y + boxH}, kBorderCol, S(8));
            const ImVec2 tc{p0.x + boxW * 0.5f, p0.y + boxH * 0.5f};
            const float maxR = S(38);
            // Concentric red/white target rings
            for (int ring = 4; ring >= 1; --ring) {
                const float rr = maxR * (ring / 4.0f);
                const ImU32 rcol = (ring % 2 == 0) ? IM_COL32(220, 38, 38, 210) : IM_COL32(241, 245, 249, 210);
                dl->AddCircleFilled(tc, rr, rcol, 36);
            }
            dl->AddLine({tc.x - maxR - S(6), tc.y}, {tc.x + maxR + S(6), tc.y}, IM_COL32(15, 23, 42, 160), 1.0f);
            dl->AddLine({tc.x, tc.y - maxR - S(6)}, {tc.x, tc.y + maxR + S(6)}, IM_COL32(15, 23, 42, 160), 1.0f);
            // Green vector dot showing net pull (Right-Left, Down-Up)
            const float nx = std::clamp((b.recoilRight - b.recoilLeft) / 100.0f, -1.0f, 1.0f);
            const float ny = std::clamp((b.recoilDown - b.recoilUp) / 100.0f, -1.0f, 1.0f);
            const ImVec2 dotPos{tc.x + nx * (maxR - S(5)), tc.y + ny * (maxR - S(5))};
            if (b.recoilEnabled && (nx != 0.0f || ny != 0.0f))
                dl->AddLine(tc, dotPos, kAccentCyan, S(2.0f));
            dl->AddCircleFilled(dotPos, S(5.5f), b.recoilEnabled ? kOk : kMuted);
            dl->AddCircle(dotPos, S(5.5f), IM_COL32_WHITE, 0, S(1.5f));
            ImGui::Dummy({boxW, boxH});
        }

        sectionTitle(T("Rapid fire settings"));
        toggleRow(T("Pistol and DMR Rapid Fire"), &b.rapidFire);
        alignedText(T("Switch mode key (Continuous <-> Rapid Fire)"), kMuted);
        chip("rftog", 9, b.rapidFireToggleKey, availW());
        if (b.rapidFireToggleKey && ImGui::SmallButton((std::string(T("No key")) + "##rf").c_str())) b.rapidFireToggleKey = 0;
        sliderNumI(T("Rapid fire delay (ms)"), &b.rapidFireDelayMs, 10, 500, "%d ms", 5);
    }

    ImGui::Dummy({0, S(2)});
    switch (buttonRow({{ICON_COPY, T("Duplicate"), b.type != BindingType::Camera},
                       {ICON_DELETE, T("Delete"), true, false, true},
                       {ICON_CLOSE, T("Close")}})) {
        case 0: duplicateBinding(i); break;
        case 1: deleteBinding(i); break;
        case 2: selectedBinding_ = -1; break;
        default: break;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::panelDevice() {
    if (!trackerError_.empty()) paragraph(trackerError_, kDanger);
    std::vector<platform::AdbDevice> devs;
    {
        std::lock_guard lk(devMu_);
        devs = devices_;
    }
    sectionTitle(T("Devices"));
    if (devs.empty()) paragraph(i18n::tr("No device. Connect the phone by USB and enable USB debugging (Developer options)."), kMuted);
    for (auto& d : devs) {
        ImGui::PushID(d.serial.c_str());
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(255, 255, 255, 8));
        ImGui::BeginChild("##dev", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
        labelValue(V(d.model.empty() ? d.serial : d.model), V(i18n::tr(d.state.c_str())), kText,
                   d.state == "device" ? kOk : kAmber);
        alignedText(d.serial.c_str(), kMuted);
        if (d.state == "unauthorized") paragraph(i18n::tr("Accept the 'Allow USB debugging' prompt on the phone."), kAmber);
        const bool current = session_ && serial_ == d.serial;
        if (d.usable() && !current && button({ICON_CONNECT, T("Connect"), true, true}, availW())) connect(d.serial);
        if (current && button({ICON_CLOSE, T("Disconnect")}, availW())) disconnect();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (session_) {
        sectionTitle(T("Session"));
        labelValue(T("Device"), V(session_->deviceName()));
        char res[32];
        std::snprintf(res, sizeof res, "%ux%u", session_->frameWidth(), session_->frameHeight());
        labelValue(T("Resolution"), res);
        labelValue(T("Decoder"), session_->decoderDescription().c_str());
        const std::string e = session_->lastError();
        if (!e.empty()) paragraph(i18n::trf("Last error: %s", e.c_str()), kDanger);
        if (session_->state() == SessionState::Failed && wantSession_)
            paragraph(i18n::tr(settings_.autoReconnect && reconnectAttempts_ < 5 ? "Reconnecting..." : "Stopped. Press Connect to retry."), kAmber);
    }
    sectionTitle(T("Connection"));
    toggleRow(T("Auto reconnect"), &settings_.autoReconnect);
}

// Small FPS / latency readout over the game. Values refresh twice per second.
void App::drawFpsOverlay() {
    if (!settings_.showFps || !sessionActive() || videoRect_.w <= 0) return;
    static int64_t nextAt = 0;
    static double decFps = 0, presFps = 0, latMs = 0;
    const int64_t now = nowMicros();
    if (now >= nextAt) {
        nextAt = now + 500000;
        decFps = session_->fps.rate(now);
        presFps = presentFps_.rate(now);
        const Percentiles p = presentLatencyUs_.summary();
        latMs = p.count ? p.p50 / 1000.0 : 0;
    }
    char s[96];
    std::snprintf(s, sizeof s, "FPS %.0f / %.0f   %.1f ms", decFps, presFps, latMs);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImGui::PushFont(nullptr, 26.0f);  // doubled font size for readability
    const ImVec2 ts = ImGui::CalcTextSize(s);
    // Top corner of the video: leading edge in RTL, trailing edge in LTR.
    const float margin = S(12);
    const float x = rtl() ? videoRect_.x + margin : videoRect_.x + videoRect_.w - ts.x - margin - S(12);
    const float y = videoRect_.y + margin;
    const ImVec2 a{x, y}, b{x + ts.x + S(16), y + ts.y + S(10)};
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 170), S(8));
    dl->AddText({a.x + S(8), a.y + S(5)}, decFps >= 55 ? kOk : decFps >= 28 ? kAmber : kDanger, s);
    ImGui::PopFont();
}

void App::panelPerformance() {
    paragraph(i18n::tr("All values are measured live in this process. End-to-end (glass-to-glass) latency needs an external camera."), kMuted);
    sectionTitle(T("Process"));
    char v[96];
    std::snprintf(v, sizeof v, "%.1f %%", cpuPercent_);
    labelValue(T("CPU"), v);
    std::snprintf(v, sizeof v, "%.0f MB", workingSetMb_);
    labelValue(T("Memory"), v);
    if (!session_ || session_->state() != SessionState::Running) {
        paragraph(i18n::tr("Connect a device to see video and control metrics."), kMuted);
        return;
    }
    const int64_t now = nowMicros();
    const auto& c = session_->counters;
    sectionTitle(T("Video"));
    std::snprintf(v, sizeof v, "%.1f / %.1f", session_->fps.rate(now), presentFps_.rate(now));
    labelValue(T("FPS (decoded / presented)"), v);
    std::snprintf(v, sizeof v, "%llu / %llu / %llu", (unsigned long long)c.packets.load(), (unsigned long long)c.decodedFrames.load(),
                  (unsigned long long)presented_);
    labelValue(T("Packets / frames / presented"), v);
    std::snprintf(v, sizeof v, "%llu / %llu", (unsigned long long)pending_.dropped(), (unsigned long long)c.decodeErrors.load());
    labelValue(T("Replaced / decode errors"), v);
    std::snprintf(v, sizeof v, "%.1f MB / %llu", c.videoBytes.load() / 1048576.0, (unsigned long long)c.controlSent.load());
    labelValue(T("Video data / control messages"), v);

    sectionTitle(T("Latency (ms)"));
    auto table = [&](std::initializer_list<std::pair<const char*, Percentiles>> rows) {
        if (!ImGui::BeginTable("##lat", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) return;
        ImGui::TableSetupColumn(T("Metric"), ImGuiTableColumnFlags_WidthStretch, 2.2f);
        for (const char* h : {"p50", "p95", "p99", "max"}) ImGui::TableSetupColumn(h, ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (auto& [name, p] : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(T(name));
            const double vals[] = {p.p50, p.p95, p.p99, p.max};
            for (double x : vals) {
                ImGui::TableNextColumn();
                if (p.count) ImGui::Text("%.1f", x / 1000);
                else ImGui::TextDisabled("-");
            }
        }
        ImGui::EndTable();
    };
    table({{"decode", session_->decodeLatencyUs.summary()},
           {"rx -> present", presentLatencyUs_.summary()},
           {"packet interval", session_->frameIntervalUs.summary()},
           {"input -> socket", session_->sendLatencyUs.summary()},
           {"input proc (T0->T2)", inputProcUs_.summary()},
           {"queue wait (T2->T3)", session_->queueWaitUs.summary()},
           {"socket write (T3->T4)", session_->sendWriteUs.summary()},
           {"present (T7->T8)", renderUs_.summary()},
           {"detect readback", detReadbackUs_.summary()},
           {"detect infer", detInferUs_.summary()},
           {"detect mailbox", detMailUs_.summary()},
           {"detect end-to-end", detEndToEndUs_.summary()},
           {"detect track", detTrackUs_.summary()},
           {"detect aim math", detAimUs_.summary()}});

    sectionTitle(T("Control queue"));
    std::snprintf(v, sizeof v, "%zu  |  oldest %.1f ms", session_->controlQueueSize(), session_->controlOldestAgeUs() / 1000.0);
    labelValue(T("Depth / oldest age"), v);
    std::snprintf(v, sizeof v, "%zu / %zu / %zu", session_->controlCoalesced(), session_->controlDropped(),
                  session_->controlStaleDropped());
    labelValue(T("Coalesced / dropped / stale"), v);
    std::snprintf(v, sizeof v, "%zu  |  spikes %zu", flight_.totalRecorded(), flight_.spikeCount());
    labelValue(T("Flight recorder (events / spikes)"), v);
    if (button({ICON_LOGS, T("Dump last events to log")}, availW())) {
        const auto evs = flight_.snapshot(64);
        log::info("--- flight recorder dump ---");
        for (const auto& e : evs)
            log::info(i18n::trf("%s id=0x%llx pos=(%d,%d) src=0x%x lat=%.1fms", flightEventTypeName(e.type),
                                static_cast<unsigned long long>(e.pointerOrKey), e.x, e.y, e.sourceMask, e.latencyUs / 1000.0));
        showToast(i18n::trf("%d events written to the log", static_cast<int>(evs.size())));
    }
    std::snprintf(v, sizeof v, "%zu", input_.sourceConflictsResolved());
    labelValue(T("Source conflicts resolved"), v);

    sectionTitle(T("Touch probe"));
    paragraph(i18n::tr("Sends taps at the red square and times until the image there changes. Enable 'Show touches' (Video) and pick an "
                       "empty, uniform area."),
              kMuted);
    labelValue(T("Probe point"), "", kText);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat2("##probe", &probe_.point.x, 0.02f, 0.98f);
    std::snprintf(v, sizeof v, "%d", probe_.target);
    sliderRow(T("Samples"), v, [&] { return ImGui::SliderInt("##samples", &probe_.target, 5, 200, ""); });
    std::snprintf(v, sizeof v, "%zu / %d", probe_.resultsUs.size(), probe_.misses);
    labelValue(T("Results (ok / timeouts)"), v);
    if (!probe_.resultsUs.empty()) table({{"touch -> frame", computePercentiles(probe_.resultsUs)}});
    switch (buttonRow({{nullptr, probe_.active ? T("Stop probe") : T("Start probe"), true, !probe_.active}, {nullptr, T("Export CSV")}})) {
        case 0:
            if (probe_.active) probe_.active = false;
            else {
                probe_.active = true;
                probe_.phase = 0;
                probe_.misses = 0;
                probe_.resultsUs.clear();
                probe_.nextAt = nowMicros() + 300000;
                releaseInput();
            }
            break;
        case 1: exportCsv(); break;
        default: break;
    }
}

void App::panelLogs() {
    toggleRow(T("Touch trace (log every touch sent to the phone)"), &settings_.touchTrace);
    ImGui::BeginChild("##log", {0, 0}, ImGuiChildFlags_Borders);
    for (const auto& e : log::recent(400)) {
        const ImVec4 col = e.level == log::Level::Error  ? ImVec4(1, 0.45f, 0.45f, 1)
                           : e.level == log::Level::Warn ? ImVec4(1, 0.8f, 0.3f, 1)
                                                         : ImVec4(0.8f, 0.82f, 0.88f, 1);
        ImGui::TextColored(col, "%8.2f  %s", e.tMicros / 1e6, e.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void App::panelHelp() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgSecondary);
    ImGui::BeginChild("##about_card", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    labelValue("NEXPLAY", "v1.0  |  PLAY SMARTER", kText, kAccentCyan);
    paragraph(i18n::tr("Professional low-latency Android game control and visual automation for Windows."), kMuted);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    sectionTitle(T("Getting started"));
    paragraph(i18n::tr("1. Enable Developer options > USB debugging on the phone and accept the prompt.\n"
                       "2. Device > Connect.\n"
                       "3. Click inside the phone image to tap directly. Press the lock key to capture the mouse for camera control; "
                       "press it again, or Alt+Tab, to release.\n"
                       "4. Open Key mapping, drag a control circle onto the game, then press its key. Click a control to edit it; "
                       "Save or Cancel at the bottom."));
    sectionTitle(T("Shortcuts"));
    const std::pair<const char*, std::string> keys[] = {
        {"Show / hide panel", "F1"},
        {"Mouse lock", keyName(profile_.lockToggleKey)},
        {"Cancel key capture", "Esc"},
        {"Delete selected", "Del"},
        {"Undo", "Ctrl + Z"},
        {"Redo", "Ctrl + Y"},
        {"Duplicate", "Ctrl + D"},
        {"Save layout", "Ctrl + S"},
    };
    for (auto& [what, key] : keys) labelValue(T(what), key.c_str(), kText, kAccent);
    sectionTitle(T("Privacy and safety"));
    paragraph(i18n::tr("USB debugging lets this PC run commands on the phone. Disable it when not in use. NEXPLAY does not use root and "
                       "does not modify or hide itself from games; follow each game's rules about input tools."),
              kMuted);
    paragraph(i18n::tr("Profiles and logs are stored locally in %APPDATA%\\WASD. Nothing is uploaded."), kMuted);
}

}  // namespace wasd::app
