// NEXPLAY Automation Studio — docked panels: toolbar, node library, properties
// inspector, execution console, empty workspace and status bar.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "app/i18n.h"
#include "app/studio_ui.h"
#include "app/text_shaping.h"
#include "core/profile.h"
#include "core/studio_converter.h"
#include "imgui_internal.h"

namespace wasd::app {
namespace {

const char* VL(const std::string& logical) { return text::visual(logical).c_str(); }
const char* TL(const char* en) { return text::visual(i18n::tr(en)).c_str(); }
const char* TLs(const std::string& en) { return text::visual(i18n::tr(en.c_str())).c_str(); }

#define ICON_BACK "\uE72B"
#define ICON_UNDO "\uE7A7"
#define ICON_REDO "\uE7A6"
#define ICON_PLAY "\uE768"
#define ICON_STOP "\uE71A"
#define ICON_ADD "\uE710"
#define ICON_DELETE "\uE74D"
#define ICON_SAVE "\uE74E"
#define ICON_FOLDER "\uE8B7"
#define ICON_ZOOMIN "\uE8A3"
#define ICON_ZOOMOUT "\uE71F"
#define ICON_FIT "\uE8A1"
#define ICON_ARRANGE "\uE8A5"
#define ICON_GRID "\uE77E"
#define ICON_MAP "\uE7E6"
#define ICON_DEBUG "\uE8BE"
#define ICON_CLOSE "\uE711"
#define ICON_LIST "\uE8FD"
#define ICON_PROP "\uE8F3"
#define ICON_STAR "\uE734"
#define ICON_NODE "\uE8A9"

bool panelHeader(const char* vis, ImU32 col) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    const bool open = ImGui::TreeNodeEx(vis, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    ImGui::PopStyleColor();
    return open;
}

// Pixel-art thumbnail of a captured ImageRef drawn into a specific rect
// (same 14x14 sampling as the Simple Mode images panel).
void drawImageThumbAt(ImDrawList* dl, const ImageRef& r, ImVec2 p, float size) {
    dl->AddRectFilled(p, {p.x + size, p.y + size}, IM_COL32(30, 30, 34, 255), 4.0f);
    if (!r.rgb.empty() && r.w && r.h) {
        const int side = 14;
        const float cs = size / side;
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x) {
                const int px = std::min<int>(r.w - 1, x * r.w / side);
                const int py = std::min<int>(r.h - 1, y * r.h / side);
                const uint8_t* c = &r.rgb[(size_t(py) * r.w + px) * 3];
                dl->AddRectFilled({p.x + x * cs, p.y + y * cs}, {p.x + (x + 1) * cs, p.y + (y + 1) * cs},
                                  IM_COL32(c[0], c[1], c[2], 255));
            }
        // click-offset marker
        dl->AddCircle({p.x + r.clickOffset.x * size, p.y + r.clickOffset.y * size}, size * 0.06f,
                      IM_COL32(80, 220, 120, 255), 0, 1.5f);
    }
    dl->AddRect(p, {p.x + size, p.y + size}, IM_COL32(90, 90, 100, 255), 4.0f);
}

void drawImageThumb(const ImageRef& r, float size) {
    drawImageThumbAt(ImGui::GetWindowDrawList(), r, ImGui::GetCursorScreenPos(), size);
    ImGui::Dummy({size, size});
}

}  // namespace

void StudioEditor::pushRecent(const studio::NodeTypeSpec& spec) {
    recentNodes_.erase(std::remove(recentNodes_.begin(), recentNodes_.end(), spec.typeKey),
                       recentNodes_.end());
    recentNodes_.insert(recentNodes_.begin(), spec.typeKey);
    if (recentNodes_.size() > 8) recentNodes_.resize(8);
}

// ============================================================================
// Toolbar
// ============================================================================

void StudioEditor::drawToolbar(float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = m_.toolbarH;
    dl->AddRectFilled(p, {p.x + w, p.y + h}, pal_.panelBg);
    dl->AddLine({p.x, p.y + h - 1}, {p.x + w, p.y + h - 1}, pal_.divider);

    ImGui::BeginChild("##studio_toolbar", {w, h}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos({m_.spMD, (h - ImGui::GetFrameHeight()) * 0.5f});

    const auto tbtn = [&](const char* icon, const char* tip, bool enabled = true, bool active = false) {
        // icon is the visible label; the ##suffix keeps the ImGui ID unique even
        // when two buttons share the same glyph.
        char lbl[48];
        std::snprintf(lbl, sizeof lbl, "%s##%s", icon, tip ? tip : "btn");
        ImGui::PushStyleColor(ImGuiCol_Button, active ? ImGui::ColorConvertU32ToFloat4(pal_.accentPrimary)
                                                      : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(enabled ? pal_.textPrimary : pal_.textDim));
        const bool r = enabled && ImGui::SmallButton(lbl);
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && tip) ImGui::SetTooltip("%s", TL(tip));
        ImGui::SameLine(0, m_.spXS);
        return r;
    };
    const auto sep = [&] {
        ImGui::SameLine(0, m_.spSM);
        const ImVec2 q = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine({q.x, q.y}, {q.x, q.y + ImGui::GetFrameHeight()}, pal_.divider);
        ImGui::SameLine(0, m_.spSM + m_.spXS);
    };

    if (tbtn(ICON_BACK, "Back to simple mode") && requestClose) requestClose();
    ImGui::SameLine(0, m_.spSM);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.accentCyan), "NEXPLAY");
    ImGui::SameLine(0, m_.spXS);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textSecondary), "Studio");
    sep();

    // graph selector
    if (automation_ && !automation_->studioGraphs.empty()) {
        const auto* g = graph();
        ImGui::SetNextItemWidth(180.0f * m_.scale);
        const std::string preview = g ? g->name : "";
        if (ImGui::BeginCombo("##gsel", VL(preview))) {
            for (int i = 0; i < static_cast<int>(automation_->studioGraphs.size()); ++i) {
                ImGui::PushID(i);  // graphs may share a name — keep IDs unique
                if (ImGui::Selectable(VL(automation_->studioGraphs[i].name), i == graphIndex_))
                    selectGraph(i);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, m_.spSM);
        if (tbtn(ICON_DELETE, "Delete whole graph", true) && g) deleteGraph(graphIndex_);
    }
    if (tbtn(ICON_ADD, "New graph")) {
        if (automation_) newGraph(studio::GraphTemplateKind::BasicSequence);
    }
    sep();

    if (tbtn(ICON_UNDO, "Undo (Ctrl+Z)", history_.canUndo())) undoHistory();
    if (tbtn(ICON_REDO, "Redo (Ctrl+Y)", history_.canRedo())) redoHistory();
    // Selection-scoped delete sits with the edit tools — the icon next to the
    // graph selector deletes the whole graph, this one deletes picked items.
    const bool hasSel = !sel_.empty() || !selNotes_.empty() || selConn_ != 0 || selGroup_ != 0;
    if (tbtn(ICON_DELETE, "Delete selection (Del)", hasSel)) deleteSelection();
    if (tbtn(ICON_SAVE, "Save profile (Ctrl+S)", save != nullptr) && save) save();
    sep();

    auto* g = graph();
    const std::string gid = g ? g->id : std::string{};
    const bool running = g && runtime.running && runtime.running(gid);
    const bool paused = running && runtime.paused && runtime.paused();
    if (!running) {
        if (tbtn(ICON_PLAY, "Run graph", g != nullptr && runtime.run != nullptr)) {
            if (g && runtime.run) runtime.run(gid, false);
            else if (toast) toast(i18n::tr("Graph engine is not available yet"));
        }
        if (tbtn(ICON_DEBUG, "Run with debugger (breakpoints)", g != nullptr && runtime.run != nullptr)) {
            if (g && runtime.run) runtime.run(gid, true);
        }
    } else {
        if (paused) {
            if (tbtn(ICON_PLAY, "Resume", true) && runtime.resume) runtime.resume();
            if (tbtn("\uE87A", "Step over", true) && runtime.stepOnce) runtime.stepOnce();
        } else {
            if (tbtn("\uE769", "Pause", runtime.pause != nullptr) && runtime.pause) runtime.pause();
        }
        if (tbtn(ICON_STOP, "Stop", true, true) && runtime.stop) runtime.stop();
    }
    // Master monitoring switch — same semantic as the old panel's
    // "Start/Stop monitoring" (arms zones, images and rules at runtime).
    const bool autoOn = automationOn && automationOn();
    if (tbtn("\uE7E8", autoOn ? "Monitoring is running — click to stop" : "Start monitoring",
             setAutomationOn != nullptr, autoOn) &&
        setAutomationOn)
        setAutomationOn(!autoOn);
    sep();

    // zoom controls
    if (g) {
        if (tbtn(ICON_ZOOMOUT, "Zoom out")) {
            auto& v = view();
            const Vec2 center{canvasSize_.x / (2 * v.zoom) - v.pan.x, canvasSize_.y / (2 * v.zoom) - v.pan.y};
            v.zoom = std::clamp(v.zoom / 1.15f, 0.2f, 2.5f);
            v.pan = {canvasSize_.x / (2 * v.zoom) - center.x, canvasSize_.y / (2 * v.zoom) - center.y};
        }
        ImGui::SetNextItemWidth(54.0f * m_.scale);
        char zbuf[16];
        std::snprintf(zbuf, sizeof zbuf, "%d%%", static_cast<int>(view().zoom * 100));
        ImGui::TextUnformatted(zbuf);
        ImGui::SameLine(0, m_.spXS);
        if (tbtn(ICON_ZOOMIN, "Zoom in")) {
            auto& v = view();
            const Vec2 center{canvasSize_.x / (2 * v.zoom) - v.pan.x, canvasSize_.y / (2 * v.zoom) - v.pan.y};
            v.zoom = std::clamp(v.zoom * 1.15f, 0.2f, 2.5f);
            v.pan = {canvasSize_.x / (2 * v.zoom) - center.x, canvasSize_.y / (2 * v.zoom) - center.y};
        }
        if (tbtn(ICON_FIT, "Fit to view (F)")) fitView();
        if (tbtn(ICON_ARRANGE, "Auto arrange")) { g->autoArrange(); commit(); }
        sep();
        if (tbtn(ICON_GRID, "Toggle grid", true, view().showGrid)) view().showGrid = !view().showGrid;
        if (tbtn(ICON_MAP, "Toggle minimap", true, view().showMinimap)) view().showMinimap = !view().showMinimap;
    }

    // right side: panel toggles
    const float rightW = 140.0f * m_.scale;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + m_.spSM, w - rightW));
    if (tbtn(ICON_LIST, "Toggle node library", true, libOpen_)) libOpen_ = !libOpen_;
    if (tbtn(ICON_PROP, "Toggle inspector", true, inspOpen_)) inspOpen_ = !inspOpen_;
    if (tbtn("\uE946", "How to use", true)) ImGui::OpenPopup("##studio_help");
    if (tbtn("\uE91D", "Toggle console", true, consolePinned_)) {
        consolePinned_ = !consolePinned_;
        if (consolePinned_ && consoleH_ < 40.0f) consoleH_ = 150.0f;
        if (!consolePinned_) consoleH_ = 0.0f;
    }

    ImGui::EndChild();
}

// ============================================================================
// Node library
// ============================================================================

void StudioEditor::libraryItem(const studio::NodeTypeSpec& spec) {
    const ImU32 accent = studio_ui::categoryAccent(spec.category);
    ImGui::PushID(spec.typeKey.c_str());
    ImGui::PushStyleColor(ImGuiCol_Header, ImGui::ColorConvertU32ToFloat4(studio_ui::withAlpha(accent, 0.25f)));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                          ImGui::ColorConvertU32ToFloat4(studio_ui::withAlpha(accent, 0.35f)));
    // The star gets its own layout slot (never overlapped by the scrollbar and
    // never shares hover with the row) — previously it was drawn inside the
    // row's right edge where the scrollbar could cover it.
    const float starW = 22.0f * m_.scale;
    const float rowH = m_.nodeRowH + m_.spXS;
    const float itemW = ImGui::GetContentRegionAvail().x - starW;
    if (ImGui::Selectable("##item", false, ImGuiSelectableFlags_AllowOverlap, {itemW, rowH})) {
    }
    const bool itemHov = ImGui::IsItemHovered();
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        const char* tic = spec.icon.empty() ? ICON_NODE : spec.icon.c_str();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(accent), "%s  %s", tic, TLs(spec.displayName));
        ImGui::Separator();
        if (!spec.summary.empty()) ImGui::TextWrapped("%s", TLs(spec.summary));
        if (!spec.example.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "%s", TLs(spec.example));
        }
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "%s",
                           TL("Drag onto the canvas or double-click to add"));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    // drag source → drop on canvas creates the node
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        int t = static_cast<int>(spec.type);
        ImGui::SetDragDropPayload("NEXPLAY_STUDIO_NODE", &t, sizeof t);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(accent), "%s", TLs(spec.displayName));
        ImGui::EndDragDropSource();
    }
    // icon chip + name
    const ImVec2 ip = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float chip = m_.nodeRowH - 4.0f;
    dl->AddRectFilled({ip.x + m_.spXS, ip.y + 3.0f}, {ip.x + m_.spXS + chip, ip.y + 3.0f + chip},
                      studio_ui::withAlpha(accent, 0.30f), m_.radiusSM);
    const char* ic = spec.icon.empty() ? ICON_NODE : spec.icon.c_str();
    const ImVec2 is = ImGui::CalcTextSize(ic);
    dl->AddText({ip.x + m_.spXS + (chip - is.x) * 0.5f, ip.y + 3.0f + (chip - is.y) * 0.5f - 1.0f}, accent, ic);
    const ImVec2 ts = ImGui::CalcTextSize(TLs(spec.displayName));
    dl->AddText({ip.x + m_.spXS + chip + m_.spSM, ip.y + (m_.nodeRowH + m_.spXS - ts.y) * 0.5f},
                pal_.textPrimary, TLs(spec.displayName));
    // favorite star — its own column, always visible (dim = not favorite)
    ImGui::SameLine(0, 0);
    const bool fav = favNodes_.count(spec.typeKey) != 0;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(fav ? pal_.warning : pal_.textDim));
    if (ImGui::SmallButton(ICON_STAR)) {
        if (fav) favNodes_.erase(spec.typeKey);
        else favNodes_.insert(spec.typeKey);
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", TL(fav ? "Remove from favorites" : "Add to favorites"));
    // double-click adds at view center
    if (itemHov && ImGui::IsMouseDoubleClicked(0)) {
        const Vec2 center = toCanvas({canvasOrigin_.x + canvasSize_.x * 0.5f, canvasOrigin_.y + canvasSize_.y * 0.5f});
        addNodeAt(spec.type, {center.x - 60, center.y - 15});
    }
    ImGui::PopID();
}

void StudioEditor::drawLibrary(float w, float h) {
    ImGui::BeginChild("##studio_library", {w, h}, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##libsearch", TL("Search nodes..."), libSearch_, sizeof libSearch_);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "%s",
                       TL("Drag a node onto the canvas — or double-click it."));
    ImGui::Spacing();

    ImGui::BeginChild("##studio_library_list", {0, 0}, ImGuiChildFlags_None);
    std::string query = libSearch_;
    std::transform(query.begin(), query.end(), query.begin(), ::tolower);
    const auto matches = [&](const studio::NodeTypeSpec& spec) {
        if (query.empty()) return true;
        std::string dn = spec.displayName, tk = spec.typeKey;
        std::transform(dn.begin(), dn.end(), dn.begin(), ::tolower);
        std::transform(tk.begin(), tk.end(), tk.begin(), ::tolower);
        return dn.find(query) != std::string::npos || tk.find(query) != std::string::npos;
    };
    // pseudo-categories: Favorites & Recently used
    if (!favNodes_.empty()) {
        ImGui::PushID("fav");
        if (ImGui::TreeNodeEx(TL("Favorites"), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& spec : studio::allNodeSpecs())
                if (favNodes_.count(spec.typeKey) && matches(spec)) libraryItem(spec);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (!recentNodes_.empty()) {
        ImGui::PushID("recent");
        if (ImGui::TreeNodeEx(TL("Recently used"), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& key : recentNodes_)
                for (const auto& spec : studio::allNodeSpecs())
                    if (spec.typeKey == key && matches(spec)) libraryItem(spec);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    for (int ci = 0; ci < 7; ++ci) {
        const auto cat = static_cast<studio::NodeCategory>(ci);
        // pre-filter category content
        bool any = false;
        for (const auto& spec : studio::allNodeSpecs()) {
            if (spec.category != cat) continue;
            if (query.empty()) { any = true; break; }
            std::string dn = spec.displayName, tk = spec.typeKey;
            std::transform(dn.begin(), dn.end(), dn.begin(), ::tolower);
            std::transform(tk.begin(), tk.end(), tk.begin(), ::tolower);
            if (dn.find(query) != std::string::npos || tk.find(query) != std::string::npos) { any = true; break; }
        }
        if (!any) continue;
        const ImU32 accent = studio_ui::categoryAccent(cat);
        ImGui::PushID(ci);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(accent));
        const bool open = ImGui::TreeNodeEx(TLs(studio::categoryName(cat)), ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor();
        if (open) {
            for (const auto& spec : studio::allNodeSpecs()) {
                if (spec.category != cat || !matches(spec)) continue;
                ImGui::Indent(m_.spSM);
                libraryItem(spec);
                ImGui::Unindent(m_.spSM);
            }
            ImGui::TreePop();
            ImGui::Spacing();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
}

// ============================================================================
// Inspector
// ============================================================================

bool StudioEditor::valueEditor(const char* id, const studio::PropertySpec& ps, studio::Value& v) {
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool ch = false;
    switch (v.type) {
        case studio::ValueType::Boolean:
            ch = ImGui::Checkbox(TL(ps.label.c_str()), &v.b);
            break;
        case studio::ValueType::Integer: {
            int iv = static_cast<int>(v.i);
            ImGui::TextUnformatted(TL(ps.label.c_str()));
            ch = ImGui::DragInt("##v", &iv, 1.0f, static_cast<int>(ps.minVal), static_cast<int>(ps.maxVal));
            if (ch) v.i = iv;
            break;
        }
        case studio::ValueType::Float: {
            float fv = static_cast<float>(v.f);
            ImGui::TextUnformatted(TL(ps.label.c_str()));
            ch = ImGui::DragFloat("##v", &fv, 0.5f, static_cast<float>(ps.minVal), static_cast<float>(ps.maxVal), "%.2f");
            if (ch) v.f = fv;
            break;
        }
        case studio::ValueType::String: {
            char buf[256];
            std::snprintf(buf, sizeof buf, "%s", v.s.c_str());
            ImGui::TextUnformatted(TL(ps.label.c_str()));
            if (ImGui::InputText("##v", buf, sizeof buf)) { v.s = buf; ch = true; }
            break;
        }
        case studio::ValueType::Coordinate:
        case studio::ValueType::Vector2: {
            float xy[2] = {v.v.x, v.v.y};
            ImGui::TextUnformatted(TL(ps.label.c_str()));
            if (ImGui::DragFloat2("##v", xy, 0.005f,
                                  v.type == studio::ValueType::Coordinate ? 0.0f : -10000.0f,
                                  v.type == studio::ValueType::Coordinate ? 1.0f : 10000.0f, "%.3f")) {
                v.v = {xy[0], xy[1]};
                ch = true;
            }
            if (v.type == studio::ValueType::Coordinate && pickPoint && sessionActive && sessionActive()) {
                if (ImGui::SmallButton(TL("Pick on screen"))) {
                    pickPoint(&v.v);
                    hideForPick_ = true;
                }
            }
            break;
        }
        case studio::ValueType::Color: {
            float c[4] = {v.color.r / 255.0f, v.color.g / 255.0f, v.color.b / 255.0f, v.color.a / 255.0f};
            ImGui::TextUnformatted(TL(ps.label.c_str()));
            if (ImGui::ColorEdit4("##v", c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
                v.color.r = static_cast<uint8_t>(c[0] * 255);
                v.color.g = static_cast<uint8_t>(c[1] * 255);
                v.color.b = static_cast<uint8_t>(c[2] * 255);
                v.color.a = static_cast<uint8_t>(c[3] * 255);
                ch = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##tol", &v.color.tolerance, 0.5f, 0.0f, 100.0f, "tol %.0f")) ch = true;
            break;
        }
        case studio::ValueType::Image: {
            ImGui::TextUnformatted(TL(ps.label.c_str()));
            std::string preview = v.image.refIndex >= 0 && automation_ &&
                                          v.image.refIndex < static_cast<int>(automation_->images.size())
                                      ? automation_->images[v.image.refIndex].name
                                      : std::string(TL("(no image)"));
            if (ImGui::BeginCombo("##v", VL(preview))) {
                for (int i = 0; automation_ && i < static_cast<int>(automation_->images.size()); ++i)
                    if (ImGui::Selectable(VL(automation_->images[i].name), i == v.image.refIndex)) {
                        v.image.refIndex = i;
                        ch = true;
                    }
                ImGui::EndCombo();
            }
            if (pickImage && sessionActive && sessionActive() &&
                ImGui::SmallButton(TL("Capture from screen"))) {
                pickImage(v.image.refIndex);
                hideForPick_ = true;
            }
            break;
        }
        default:
            ImGui::TextDisabled("%s: %s", TL(ps.label.c_str()), studio::valueTypeName(v.type));
            break;
    }
    ImGui::PopID();
    return ch;
}

// Parses enum hints written in spec tooltips as "0=Saved, 1=FullScreen, 2=Custom"
// (spaces around '=' allowed). Returns the {value,label} list when every token
// matches; empty otherwise — the caller falls back to a plain int drag.
static std::vector<std::pair<int, std::string>> parseEnumTooltip(const std::string& tip) {
    std::vector<std::pair<int, std::string>> out;
    if (tip.find('=') == std::string::npos) return out;
    size_t pos = 0;
    while (pos <= tip.size()) {
        const size_t comma = tip.find(',', pos);
        std::string tok = tip.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) return {};
        const char* p = tok.c_str();
        char* end = nullptr;
        const long k = std::strtol(p, &end, 10);
        if (end != tok.c_str() + eq) return {};  // text before '=' isn't a bare int
        std::string label = tok.substr(eq + 1);
        const auto trim = [](std::string& s) {
            const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
            s = a == std::string::npos ? "" : s.substr(a, b - a + 1);
        };
        trim(label);
        if (label.empty()) return {};
        out.emplace_back(static_cast<int>(k), std::move(label));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out.size() >= 2 ? out : std::vector<std::pair<int, std::string>>{};
}

// Rect-prop groups whose four float members collapse into one editor + pick.
static const char* const kRectGroups[][4] = {
    {"regionX", "regionY", "regionW", "regionH"},
    {"x", "y", "w", "h"},
};

bool StudioEditor::nodePropEditor(const studio::NodeTypeSpec& spec, studio::GraphNode& n,
                                  const studio::PropertySpec& ps, studio::Value& v, bool& changed) {
    const bool session = pickPoint && sessionActive && sessionActive();
    // Pick buttons stay visible but disabled without a session — hiding them
    // made the on-screen picking feature undiscoverable.
    auto pickBtn = [&](const char* labelKey, auto&& fn) {
        ImGui::BeginDisabled(!session);
        if (ImGui::SmallButton(TL(labelKey))) fn();
        ImGui::EndDisabled();
        if (!session) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", TL("(connect a device to pick on screen)"));
        }
    };
    auto armRectPick = [&](const char* const* grp, const float* r) {
        pendingPick_ = {};
        pendingPick_.nodeId = n.id;
        for (int i = 0; i < 4; ++i) pendingPick_.keys[i] = grp[i];
        pendingPick_.rect = {r[0], r[1], r[2], r[3]};
        pendingPick_.requestedAt = ImGui::GetTime();
        pickRect(&pendingPick_.rect);
        hideForPick_ = true;
    };

    // ---- swipe path (Slide): 'from' + 'to' as one editor with draggable
    // on-screen markers — the same interaction the key editor gives a Drag
    // binding, instead of two bare coordinate fields.
    if ((ps.key == "from" || ps.key == "to") && v.type == studio::ValueType::Coordinate) {
        auto other = n.props.find(ps.key == "from" ? "to" : "from");
        const bool pair = other != n.props.end() && other->second.type == studio::ValueType::Coordinate;
        if (!pair) return false;  // lone coordinate -> normal editor
        if (ps.key == "to") return true;  // rendered with 'from'
        ImGui::PushID("swipePath");
        ImGui::TextUnformatted(TL("Swipe Path (Start -> End)"));
        float a[2] = {v.v.x, v.v.y}, b[2] = {other->second.v.x, other->second.v.y};
        ImGui::TextUnformatted(TL("Start"));
        ImGui::SameLine(0, m_.spSM);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragFloat2("##a", a, 0.005f, 0.0f, 1.0f, "%.3f")) {
            v.v = {a[0], a[1]};
            changed = true;
        }
        ImGui::TextUnformatted(TL("End"));
        ImGui::SameLine(0, m_.spSM);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragFloat2("##b", b, 0.005f, 0.0f, 1.0f, "%.3f")) {
            other->second.v = {b[0], b[1]};
            changed = true;
        }
        pickBtn("Pick swipe path on screen", [&] {
            pickSwipe(&v.v, &other->second.v);
            hideForPick_ = true;
        });
        ImGui::PopID();
        return true;
    }

    // ---- reference image picker (FindImage, WaitForImage) ----
    if (ps.key == "imageIndex" && v.type == studio::ValueType::Integer) {
        ImGui::PushID("imageIndex");
        ImGui::TextUnformatted(TL("Reference Image"));
        const int idx = static_cast<int>(v.i);
        const bool valid = automation_ && idx >= 0 && idx < static_cast<int>(automation_->images.size());
        std::string preview = valid ? automation_->images[idx].name : std::string(i18n::tr("(no image)"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##img", VL(preview))) {
            for (int i = 0; automation_ && i < static_cast<int>(automation_->images.size()); ++i)
                if (ImGui::Selectable(VL(automation_->images[i].name), i == idx)) { v.i = i; changed = true; }
            ImGui::EndCombo();
        }
        pickBtn("Capture from screen", [&] {
            pendingPick_ = {};
            pendingPick_.nodeId = n.id;
            pendingPick_.imageKey = "imageIndex";
            pendingPick_.imagesBefore = automation_ ? static_cast<int>(automation_->images.size()) : 0;
            pendingPick_.requestedAt = ImGui::GetTime();
            pickImage(-1);
            hideForPick_ = true;
        });
        if (valid) {
            ImGui::SameLine();
            pickBtn("Replace capture", [&] {
                pickImage(idx);
                hideForPick_ = true;
            });
            // Live thumbnail of what was captured — the user asked to see the
            // image, not just its list index.
            const auto& img = automation_->images[idx];
            drawImageThumb(img, 56.0f * m_.scale);
            ImGui::SameLine(0, m_.spSM);
            ImGui::BeginGroup();
            ImGui::TextUnformatted(VL(img.name));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "%ux%u  ·  %d%%", img.w, img.h,
                               img.thresholdPct);
            ImGui::EndGroup();
        }
        ImGui::PopID();
        return true;
    }

    // ---- saved color zone (FindColor, CheckColorZone, ...) ----
    if (ps.key == "zoneIndex" && v.type == studio::ValueType::Integer) {
        ImGui::PushID("zoneIndex");
        ImGui::TextUnformatted(TL(ps.label.c_str()));
        const int zi = static_cast<int>(v.i);
        const bool valid = automation_ && zi >= 0 && zi < static_cast<int>(automation_->zones.size());
        std::string preview = zi < 0 ? i18n::tr("Custom / none")
                                     : valid ? automation_->zones[zi].name : i18n::tr("(missing zone)");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##zone", VL(preview))) {
            if (ps.minVal <= -1 && ImGui::Selectable(TL("Custom / none"), zi < 0)) {
                v.i = -1;
                changed = true;
            }
            for (int i = 0; automation_ && i < static_cast<int>(automation_->zones.size()); ++i)
                if (ImGui::Selectable(VL(automation_->zones[i].name), i == zi)) { v.i = i; changed = true; }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return true;
    }

    // ---- legacy macro reference (RunMacro) ----
    if (ps.key == "macroIndex" && v.type == studio::ValueType::Integer) {
        ImGui::PushID("macroIndex");
        ImGui::TextUnformatted(TL(ps.label.c_str()));
        const int mi = static_cast<int>(v.i);
        const bool valid = automation_ && mi >= 0 && mi < static_cast<int>(automation_->macros.size());
        std::string preview = mi < 0 ? i18n::tr("(none)")
                                     : valid ? automation_->macros[mi].name : i18n::tr("(missing macro)");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##macro", VL(preview))) {
            if (ps.minVal <= -1 && ImGui::Selectable(TL("(none)"), mi < 0)) {
                v.i = -1;
                changed = true;
            }
            for (int i = 0; automation_ && i < static_cast<int>(automation_->macros.size()); ++i)
                if (ImGui::Selectable(VL(automation_->macros[i].name), i == mi)) { v.i = i; changed = true; }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return true;
    }

    // ---- region mode: combo + direct "pick region" when Custom is chosen ----
    if (ps.key == "regionMode" && v.type == studio::ValueType::Integer) {
        ImGui::PushID("regionMode");
        ImGui::TextUnformatted(TL(ps.label.c_str()));
        const auto opts = parseEnumTooltip(ps.tooltip);
        const int cur = static_cast<int>(v.i);
        std::string preview = i18n::trf("%d", cur);
        for (const auto& o : opts) if (o.first == cur) preview = i18n::tr(o.second.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##rmode", VL(preview))) {
            for (const auto& o : opts)
                if (ImGui::Selectable(TLs(o.second), o.first == cur)) { v.i = o.first; changed = true; }
            ImGui::EndCombo();
        }
        if (cur == 0) {
            ImGui::TextDisabled("%s", TL("Uses the search region saved with the reference image"));
        } else if (cur == 2) {
            float r[4] = {0, 0, 1, 1};
            static const char* const keys[4] = {"regionX", "regionY", "regionW", "regionH"};
            for (int i = 0; i < 4; ++i) {
                const auto it = n.props.find(keys[i]);
                if (it != n.props.end()) r[i] = static_cast<float>(it->second.f);
            }
            pickBtn("Pick region on screen", [&] { armRectPick(keys, r); });
        }
        ImGui::PopID();
        return true;
    }

    // ---- normalized rectangle groups (regionX..H, x/y/w/h) ----
    for (const auto& grp : kRectGroups) {
        bool member = false, leader = ps.key == grp[0];
        for (int i = 1; i < 4 && !member; ++i) member = ps.key == grp[i];
        if (!member) continue;
        // Only group when the spec actually carries all four keys.
        bool all = true;
        for (int i = 0; i < 4 && all; ++i) {
            bool found = false;
            for (const auto& q : spec.properties) found = found || q.key == grp[i];
            all = found;
        }
        if (!all) return false;  // unrelated x/y pair etc -> normal float editor
        if (!leader) return true;  // members 1..3 render inside the leader editor
        ImGui::PushID(grp[0]);
        float r[4] = {0, 0, 1, 1};
        for (int i = 0; i < 4; ++i) {
            const auto it = n.props.find(grp[i]);
            r[i] = it != n.props.end() ? static_cast<float>(it->second.f) : r[i];
        }
        ImGui::TextUnformatted(TL(ps.key == "x" ? "Region (X, Y, W, H)" : "Custom Region (X, Y, W, H)"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragFloat4("##rect", r, 0.005f, 0.0f, 1.0f, "%.3f")) {
            for (int i = 0; i < 4; ++i) {
                auto& pv = n.props[grp[i]];
                if (pv.type != studio::ValueType::Float) pv = studio::Value::makeFloat(r[i]);
                pv.f = r[i];
            }
            changed = true;
        }
        // FindImage honors regionX..H only in Region Mode = Custom; other rect
        // nodes (SelectSearchRegion) have no such gate.
        const auto rm = n.props.find("regionMode");
        const bool showPick = rm == n.props.end() || rm->second.i == 2;
        if (showPick) pickBtn("Pick region on screen", [&] { armRectPick(grp, r); });
        ImGui::PopID();
        return true;
    }

    // ---- color picker target (FindColor 'targetColor') ----
    if (v.type == studio::ValueType::Color && pickColors) {
        ImGui::PushID(ps.key.c_str());
        ImGui::TextUnformatted(TL(ps.label.c_str()));
        float c[4] = {v.color.r / 255.0f, v.color.g / 255.0f, v.color.b / 255.0f, v.color.a / 255.0f};
        if (ImGui::ColorEdit4("##v", c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
            v.color.r = static_cast<uint8_t>(c[0] * 255);
            v.color.g = static_cast<uint8_t>(c[1] * 255);
            v.color.b = static_cast<uint8_t>(c[2] * 255);
            v.color.a = static_cast<uint8_t>(c[3] * 255);
            changed = true;
        }
        ImGui::SameLine();
        pickBtn("Pick color on screen", [&] {
            pickColors(&v.color, nullptr);
            hideForPick_ = true;
        });
        ImGui::PopID();
        return true;
    }

    // ---- recorded color list (FindColor 'colors': "r,g,b;r,g,b") ----
    if (ps.key == "colors" && v.type == studio::ValueType::String && pickColors) {
        ImGui::PushID("colors");
        ImGui::TextUnformatted(TL("Extra Target Colors"));
        // swatches with per-entry delete
        std::vector<std::string> toks;
        for (size_t pos = 0; pos <= v.s.size();) {
            const size_t semi = v.s.find(';', pos);
            toks.push_back(v.s.substr(pos, semi == std::string::npos ? semi : semi - pos));
            if (semi == std::string::npos) break;
            pos = semi + 1;
        }
        int del = -1;
        const float sw = 18.0f * m_.scale;
        const float rowMax = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        bool firstEntry = true;
        for (int i = 0; i < static_cast<int>(toks.size()); ++i) {
            unsigned rr = 0, gg = 0, bb = 0;
            if (std::sscanf(toks[i].c_str(), "%u,%u,%u", &rr, &gg, &bb) != 3) continue;
            const float entryW = sw + m_.spXS + ImGui::CalcTextSize(toks[i].c_str()).x +
                                 m_.spXS + ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 4.0f;
            // Wrap to a new line instead of running off the panel edge.
            if (!firstEntry && ImGui::GetCursorPosX() + entryW < rowMax) ImGui::SameLine(0, m_.spSM);
            firstEntry = false;
            ImGui::PushID(i);
            ImGui::ColorButton("##sw", ImVec4(rr / 255.0f, gg / 255.0f, bb / 255.0f, 1.0f),
                               ImGuiColorEditFlags_NoTooltip, {sw, sw});
            ImGui::SameLine(0, m_.spXS);
            ImGui::TextUnformatted(toks[i].c_str());
            ImGui::SameLine(0, m_.spXS);
            if (ImGui::SmallButton("x")) { del = i; changed = true; }
            ImGui::PopID();
        }
        if (del >= 0) {
            toks.erase(toks.begin() + del);
            v.s.clear();
            for (size_t i = 0; i < toks.size(); ++i) {
                if (i) v.s += ';';
                v.s += toks[i];
            }
        }
        pickBtn("Record colors on screen", [&] {
            pickColors(nullptr, &v.s);
            hideForPick_ = true;
        });
        if (!v.s.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(TL("Clear"))) { v.s.clear(); changed = true; }
        }
        ImGui::TextDisabled("%s", TL("A match on ANY of these colors counts as found"));
        ImGui::PopID();
        return true;
    }

    // ---- Find Object class filter ('classes': "name,name") + live test ----
    if (ps.key == "classes" && n.type == studio::NodeType::FindObject &&
        v.type == studio::ValueType::String) {
        ImGui::PushID("objClasses");
        ImGui::TextUnformatted(TL("Detect Classes"));
        const std::vector<std::string> names = detectClasses ? detectClasses() : std::vector<std::string>{};
        // Parse the current comma-separated filter into a set.
        std::vector<std::string> sel;
        for (size_t pos = 0; pos <= v.s.size();) {
            const size_t c = v.s.find(',', pos);
            std::string t = v.s.substr(pos, c == std::string::npos ? c : c - pos);
            while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
            while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
            if (!t.empty()) sel.push_back(t);
            if (c == std::string::npos) break;
            pos = c + 1;
        }
        auto isSel = [&](const std::string& cls) {
            for (const auto& s : sel) if (s == cls) return true;
            return false;
        };
        auto writeSel = [&] {
            v.s.clear();
            for (size_t i = 0; i < sel.size(); ++i) { if (i) v.s += ','; v.s += sel[i]; }
            changed = true;
        };
        if (names.empty()) {
            // Model not loaded yet (lazy load on first scan) — free text stays usable.
            char buf[512];
            std::snprintf(buf, sizeof buf, "%s", v.s.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint("##cls", TL("soldier,camouflage_soldier"), buf, sizeof buf)) {
                v.s = buf;
                changed = true;
            }
            ImGui::TextDisabled("%s: %s", TL("Detector"), detectStatus ? VL(detectStatus()) : "?");
        } else {
            for (const std::string& cls : names) {
                bool on = isSel(cls);
                if (ImGui::Checkbox(VL(cls), &on)) {
                    if (on) sel.push_back(cls);
                    else sel.erase(std::remove(sel.begin(), sel.end(), cls), sel.end());
                    writeSel();
                }
            }
            ImGui::TextDisabled("%s", TL("None checked = detect every class"));
        }
        // Live test: streams detection boxes over the video using the node's
        // current region + confidence + classes.
        if (setDetectPreview) {
            const bool canTest = session;
            ImGui::BeginDisabled(!canTest);
            if (ImGui::SmallButton(detectPreviewOn_ ? TL("Stop Live Test") : TL("Live Test"))) {
                detectPreviewOn_ = !detectPreviewOn_;
                Rect rg{0, 0, 1, 1};
                auto it = n.props.find("regionMode");
                if (it != n.props.end() && it->second.i == 2) {
                    auto gp = [&](const char* k, float d) {
                        auto p = n.props.find(k);
                        return p != n.props.end() ? float(p->second.f) : d;
                    };
                    rg = {gp("regionX", 0), gp("regionY", 0), gp("regionW", 1), gp("regionH", 1)};
                }
                auto cp = n.props.find("confidencePct");
                const float cf = cp != n.props.end() ? std::clamp(int(cp->second.i), 1, 99) / 100.0f : 0.5f;
                setDetectPreview(detectPreviewOn_, rg, sel, cf);
            }
            ImGui::EndDisabled();
        }
        ImGui::PopID();
        return true;
    }

    // ---- enum-like ints encoded in tooltips ("0=A, 1=B, ...") ----
    if (v.type == studio::ValueType::Integer && !ps.tooltip.empty()) {
        const auto opts = parseEnumTooltip(ps.tooltip);
        if (!opts.empty()) {
            ImGui::PushID(ps.key.c_str());
            ImGui::TextUnformatted(TL(ps.label.c_str()));
            const int cur = static_cast<int>(v.i);
            std::string preview = i18n::trf("%d", cur);
            for (const auto& o : opts) if (o.first == cur) preview = i18n::tr(o.second.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##enum", VL(preview))) {
                for (const auto& o : opts)
                    if (ImGui::Selectable(TLs(o.second), o.first == cur)) { v.i = o.first; changed = true; }
                ImGui::EndCombo();
            }
            ImGui::PopID();
            return true;
        }
    }

    return false;
}

void StudioEditor::syncPendingPick() {
    if (!pendingPick_.nodeId) return;
    PendingPick pp = pendingPick_;
    auto* g = graph();
    auto* n = g ? g->findNode(pp.nodeId) : nullptr;
    if (!n) {
        pendingPick_ = {};
        return;
    }
    bool ch = false, waiting = false;
    if (!pp.keys[0].empty()) {
        const float vals[4] = {pp.rect.x, pp.rect.y, pp.rect.w, pp.rect.h};
        for (int i = 0; i < 4; ++i) {
            auto& pv = n->props[pp.keys[i]];
            if (pv.type != studio::ValueType::Float) pv = studio::Value::makeFloat(vals[i]);
            if (pv.f != vals[i]) { pv.f = vals[i]; ch = true; }
        }
        pendingPick_.keys = {};
    }
    if (pp.imagesBefore >= 0) {
        // The capture lands in automation_->images a few frames after the pick
        // ends (async region readback) — keep waiting briefly instead of
        // resolving too early.
        const int count = automation_ ? static_cast<int>(automation_->images.size()) : 0;
        if (count > pp.imagesBefore) {
            auto& pv = n->props[pp.imageKey];
            if (pv.i != count - 1) { pv.i = count - 1; ch = true; }
            pendingPick_.imagesBefore = -1;
        } else if (ImGui::GetTime() - pp.requestedAt < 3.0) {
            waiting = true;
        } else {
            pendingPick_.imagesBefore = -1;  // timed out / cancelled
        }
    }
    if (ch) commit();
    if (!waiting && pendingPick_.keys[0].empty() && pendingPick_.imagesBefore < 0) pendingPick_ = {};
}

void StudioEditor::triggerKeyRowFor(int* k1, int* k2, int* inh1, int* inh2) {
    ImGui::TextUnformatted(TL("Trigger key"));
    const float half = (ImGui::GetContentRegionAvail().x - m_.spSM) * 0.5f;
    const ImVec4 amber = ImGui::ColorConvertU32ToFloat4(pal_.warning);
    // Effective value = the node's own key, else the inherited graph-level
    // fallback (first Start only — keeps older profiles' metadata keys alive).
    const int e1 = *k1 ? *k1 : (inh1 ? *inh1 : 0);
    const int e2 = *k2 ? *k2 : (inh2 ? *inh2 : 0);
    auto btn = [&](int* p, int eff, int* combo, const char* fallbackKey, float w) {
        const bool waiting = capturingKey && capturingKey(p);
        const std::string lbl =
            waiting ? i18n::tr("Press a key...") : eff > 0 ? keyName(eff) : i18n::tr(fallbackKey);
        ImGui::PushID(p);
        if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, amber);
        // Capturing key1 passes key2 as the combo target: hold key 1, tap key 2
        // for a combo, release key 1 for a single-key trigger.
        if (ImGui::Button(VL(lbl), {w, 0}) && captureKey) captureKey(p, combo, 7);
        if (waiting) ImGui::PopStyleColor();
        ImGui::PopID();
    };
    btn(k1, e1, k2, "Set trigger key", half);
    ImGui::SameLine(0, m_.spSM);
    btn(k2, e2, nullptr, "Second key", -FLT_MIN);
    if (e1 > 0 || e2 > 0) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "%s",
                           TL(e2 > 0 ? "Runs when both keys are held together during play"
                                     : "Runs when this key is pressed during play"));
        ImGui::SameLine();
        if (ImGui::SmallButton(TL("Clear"))) {
            *k1 = *k2 = 0;
            if (inh1) *inh1 = 0;
            if (inh2) *inh2 = 0;
            commit();
        }
    }
}

void StudioEditor::inspectorNode(studio::GraphNode& n) {
    const auto* spec = studio::findNodeSpec(n.type, n.customTypeKey);
    const ImU32 accent = spec ? studio_ui::categoryAccent(spec->category) : pal_.accentPrimary;

    // header card
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(studio_ui::withAlpha(accent, 0.12f)));
    ImGui::BeginChild("##insp_head", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(accent), "%s",
                       spec ? VL(spec->displayName) : "Node");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textSecondary), "id %u  ·  %s", n.id,
                       spec ? TLs(studio::categoryName(spec->category)) : "");
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // general
    if (panelHeader(TL("General"), pal_.textSecondary)) {
        char tbuf[160];
        std::snprintf(tbuf, sizeof tbuf, "%s", n.title.c_str());
        ImGui::TextUnformatted(TL("Title"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##ntitle", tbuf, sizeof tbuf)) { n.title = tbuf; commit(); }
        ImGui::TextUnformatted(TL("Comment"));
        char cbuf[256];
        std::snprintf(cbuf, sizeof cbuf, "%s", n.comment.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##ncomment", cbuf, sizeof cbuf)) { n.comment = cbuf; commit(); }
        commitIf(ImGui::Checkbox(TL("Enabled"), &n.enabled));
        ImGui::SameLine();
        commitIf(ImGui::Checkbox(TL("Breakpoint"), &n.breakpoint));
        ImGui::SameLine();
        commitIf(ImGui::Checkbox(TL("Collapsed"), &n.collapsed));
        // The Start node IS the graph entry — its own trigger key lives here
        // (a graph can host several Starts, each a separate "macro"). The
        // first Start also inherits the legacy graph-level metadata key.
        if (n.type == studio::NodeType::Start) {
            auto* g0 = graph();
            const bool first = g0 && [&] {
                for (auto& m : g0->nodes)
                    if (m.type == studio::NodeType::Start) return m.id == n.id;
                return false;
            }();
            triggerKeyRowFor(&n.triggerKey, &n.triggerKey2,
                             first ? &g0->metadata.triggerKey : nullptr,
                             first ? &g0->metadata.triggerKey2 : nullptr);
        }
        ImGui::TreePop();
    }

    // properties (regular then advanced = progressive disclosure)
    if (spec && !spec->properties.empty()) {
        bool anyBasic = false, anyAdv = false;
        for (const auto& ps : spec->properties) (ps.advanced ? anyAdv : anyBasic) = true;
        if (anyBasic && panelHeader(TL("Parameters"), pal_.textSecondary)) {
            for (const auto& ps : spec->properties) {
                if (ps.advanced) continue;
                studio::Value& v = n.props[ps.key];
                if (v.type != ps.defaultValue.type) v = ps.defaultValue;
                bool ch = false;
                if (nodePropEditor(*spec, n, ps, v, ch) ? ch : valueEditor(ps.key.c_str(), ps, v))
                    commit();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !ps.tooltip.empty())
                    ImGui::SetTooltip("%s", TLs(ps.tooltip));
            }
            ImGui::TreePop();
        }
        if (anyAdv) {
            if (ImGui::TreeNode(TL("Advanced settings"))) {
                for (const auto& ps : spec->properties) {
                    if (!ps.advanced) continue;
                    studio::Value& v = n.props[ps.key];
                    if (v.type != ps.defaultValue.type) v = ps.defaultValue;
                    bool ch = false;
                    if (nodePropEditor(*spec, n, ps, v, ch) ? ch : valueEditor(ps.key.c_str(), ps, v))
                        commit();
                }
                ImGui::TreePop();
            }
        }
    }

    // port reference (read-only)
    if (panelHeader(TL("Ports"), pal_.textSecondary)) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "%s", TL("Inputs"));
        for (const auto& p : n.inputs)
            ImGui::BulletText("%s  (%s)", VL(i18n::tr(p.name.c_str())),
                              p.kind == studio::PortKind::Execution ? "exec" : studio::valueTypeName(p.dataType));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "%s", TL("Outputs"));
        for (const auto& p : n.outputs)
            ImGui::BulletText("%s  (%s)", VL(i18n::tr(p.name.c_str())),
                              p.kind == studio::PortKind::Execution ? "exec" : studio::valueTypeName(p.dataType));
        ImGui::TreePop();
    }
}

// Reference-image manager inside the studio: list, preview, capture, search
// region, replace and delete — the features of the standalone Images panel so
// the workflow stays in one place.
void StudioEditor::inspectorImages() {
    if (!automation_) return;
    auto& imgs = automation_->images;
    const bool session = pickImage && sessionActive && sessionActive();

    ImGui::BeginDisabled(!session || static_cast<int>(imgs.size()) >= kMaxImages);
    if (ImGui::Button(TL("New image (drag a rectangle on the game)"), {-FLT_MIN, 0})) {
        pickImage(-1);
        hideForPick_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, m_.spSM);
    if (ImGui::SmallButton(TL("Refresh from disk")) && reloadImages) reloadImages();
    if (!session) ImGui::TextDisabled("%s", TL("(connect a device to pick on screen)"));

    if (imgs.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", TL("No images yet — capture one from the screen, then pick it in a Find Image node."));
        return;
    }

    // ---- image list --------------------------------------------------------
    ImGui::BeginChild("##img_list", {0, std::min(ImGui::GetContentRegionAvail().y * 0.45f, 180.0f * m_.scale)},
                      ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(imgs.size()); ++i) {
        ImGui::PushID(i);
        const auto& r = imgs[i];
        const float th = 24.0f * m_.scale;
        if (ImGui::Selectable("##row", i == imagesSel_, ImGuiSelectableFlags_None, {0, th + m_.spXS}))
            imagesSel_ = i;
        const ImVec2 ip = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        drawImageThumbAt(dl, r, {ip.x + m_.spXS, ip.y + m_.spXS * 0.5f}, th);
        dl->AddText({ip.x + m_.spXS + th + m_.spSM, ip.y + m_.spXS * 0.5f}, pal_.textPrimary,
                    VL(r.name));
        char meta[64];
        std::snprintf(meta, sizeof meta, "%ux%u  ·  %d%%", r.w, r.h, r.thresholdPct);
        dl->AddText({ip.x + m_.spXS + th + m_.spSM, ip.y + m_.spXS * 0.5f + ImGui::GetTextLineHeight()},
                    pal_.textDim, meta);
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ---- selected image details --------------------------------------------
    if (imagesSel_ < 0 || imagesSel_ >= static_cast<int>(imgs.size())) imagesSel_ = -1;
    if (imagesSel_ < 0) {
        ImGui::TextDisabled("%s", TL("Select an image to edit it"));
        return;
    }
    auto& r = imgs[imagesSel_];
    ImGui::Spacing();
    drawImageThumb(r, 84.0f * m_.scale);
    ImGui::SameLine(0, m_.spSM);
    ImGui::BeginGroup();
    {
        char nbuf[96];
        std::snprintf(nbuf, sizeof nbuf, "%s", r.name.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##iname", nbuf, sizeof nbuf)) r.name = nbuf;
    }
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "%ux%u", r.w, r.h);
    ImGui::EndGroup();

    ImGui::TextUnformatted(TL("Accuracy (%)"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##acc", &r.thresholdPct, 1, 100);

    ImGui::BeginDisabled(!session);
    if (ImGui::Button(TL("Search region: drag"), {-FLT_MIN, 0})) {
        pickRect(&r.region);
        hideForPick_ = true;
    }
    if (ImGui::Button(TL("Replace capture"), {-FLT_MIN, 0})) {
        pickImage(imagesSel_);
        hideForPick_ = true;
    }
    ImGui::EndDisabled();

    // Live scan test: toggles a continuous background scan of this image and
    // shows the last match score/cost inline (same as the old panel's test).
    const bool testing = imageTesting && imageTesting(imagesSel_);
    ImGui::BeginDisabled(!session);
    if (ImGui::Button(testing ? TL("Stop test") : TL("Live test"), {-FLT_MIN, 0}) && testImage)
        testImage(imagesSel_);
    ImGui::EndDisabled();
    if (imageStatus) {
        const auto st = imageStatus(imagesSel_);
        // The worker reports the best score even on a miss — showing it makes
        // "searched but below threshold" distinguishable from "not searched".
        if (st.tested)
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(st.found ? pal_.success : pal_.textDim), "%s",
                VL(i18n::trf(st.found ? "Detected — score %.0f%% (%.1f ms)"
                                      : "Not found — best match %.0f%% (%.1f ms)",
                             st.score * 100.0f, st.costUs / 1000.0)));
    }

    if (ImGui::TreeNode(TL("Advanced settings"))) {
        ImGui::TextUnformatted(TL("Click offset (X, Y)"));
        float ofs[2] = {r.clickOffset.x, r.clickOffset.y};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat2("##clkofs", ofs, 0.0f, 1.0f, "%.3f")) {
            r.clickOffset.x = ofs[0];
            r.clickOffset.y = ofs[1];
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt(TL("Scan every ms"), &r.intervalMs, 5.0f, 30, 5000);
        ImGui::TreePop();
    }

    ImGui::Checkbox(TL("Show overlay on screen"), &r.showOverlay);

    ImGui::Spacing();
    if (ImGui::Button(TL("Delete image"), {-FLT_MIN, 0})) {
        if (deleteImage) deleteImage(imagesSel_);
        imagesSel_ = -1;
    }
}

// Color-zone manager merged from the old Colors panel: draw zones on the game,
// pick colors point-by-point, tune tolerance and match mode, watch live
// per-color hit counts.
void StudioEditor::inspectorZones() {
    if (!automation_) return;
    auto& zones = automation_->zones;
    const bool session = sessionActive && sessionActive();

    // ---- live-test + add row -------------------------------------------------
    if (setZoneTest) {
        bool zt = zoneTestOn && zoneTestOn();
        if (ImGui::Checkbox(TL("Live test"), &zt)) setZoneTest(zt);
        ImGui::SameLine(0, m_.spSM);
    }
    ImGui::BeginDisabled(!session);
    if (ImGui::Button(TL("+ Add zone (click on the game)"), {-FLT_MIN, 0}) && newZone) {
        newZone();
        hideForPick_ = true;
    }
    ImGui::EndDisabled();
    if (!session) ImGui::TextDisabled("%s", TL("(connect a device to pick on screen)"));

    if (zones.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s",
                           TL("No color zones yet — draw one on the game, then pick the colors to detect inside it."));
        return;
    }

    // ---- zone list -----------------------------------------------------------
    ImGui::BeginChild("##zone_list", {0, std::min(ImGui::GetContentRegionAvail().y * 0.35f, 130.0f * m_.scale)},
                      ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(zones.size()); ++i) {
        ImGui::PushID(i);
        std::string label = zones[i].name;
        if (zoneStatus) {
            const auto st = zoneStatus(i);
            if (st.present) label += "  [*]";
        }
        if (ImGui::Selectable(VL(label), i == zonesSel_)) zonesSel_ = (i == zonesSel_ ? -1 : i);
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (zonesSel_ < 0 || zonesSel_ >= static_cast<int>(zones.size())) zonesSel_ = -1;
    if (zonesSel_ < 0) {
        ImGui::TextDisabled("%s", TL("Select an item to view its settings"));
        return;
    }
    Zone& z = zones[zonesSel_];
    StudioEditor::ZoneStatus st = zoneStatus ? zoneStatus(zonesSel_) : StudioEditor::ZoneStatus{};

    char nbuf[96];
    std::snprintf(nbuf, sizeof nbuf, "%s", z.name.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##zname", nbuf, sizeof nbuf)) z.name = nbuf;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##zradius", &z.radius, 0.005f, 0.3f, TL("Radius %.4f"));
    int mode = z.mode == ColorMatchMode::All ? 1 : 0;
    const char* modes[] = {TL("Any color"), TL("All colors together")};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##zmode", &mode, modes, 2)) z.mode = mode ? ColorMatchMode::All : ColorMatchMode::Any;

    // ---- colors --------------------------------------------------------------
    ImGui::SeparatorText(TL("Colors"));
    int delColor = -1;
    for (int c = 0; c < static_cast<int>(z.colors.size()); ++c) {
        ColorTarget& t = z.colors[c];
        ImGui::PushID(100 + c);
        ImGui::ColorButton("##sw", ImVec4(t.r / 255.0f, t.g / 255.0f, t.b / 255.0f, 1), 0, {22.0f * m_.scale, 22.0f * m_.scale});
        ImGui::SameLine();
        char hex[16];
        std::snprintf(hex, sizeof hex, "#%02X%02X%02X", t.r, t.g, t.b);
        ImGui::TextUnformatted(hex);
        if (st.tested && c < static_cast<int>(st.perColor.size())) {
            ImGui::SameLine();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.success), "%d/%d", st.perColor[c], st.total);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!session);
        if (ImGui::SmallButton(TL("Re-pick")) && pickZoneColor) {
            pickZoneColor(zonesSel_, c, false);
            hideForPick_ = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_DELETE)) delColor = c;
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##tol", &t.tolerance, 0.0f, 60.0f, TL("Tolerance %.1f"));
        ImGui::PopID();
    }
    if (delColor >= 0) z.colors.erase(z.colors.begin() + delColor);
    ImGui::BeginDisabled(!session || z.colors.size() >= 48);
    if (ImGui::Button(TL("+ Add color (pick from the game)"), {-FLT_MIN, 0}) && pickZoneColor) {
        pickZoneColor(zonesSel_, -1, true);
        hideForPick_ = true;
    }
    ImGui::EndDisabled();

    if (ImGui::TreeNode(TL("Advanced settings"))) {
        float pos[2] = {z.center.x, z.center.y};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat2("##zpos", pos, 0.0f, 1.0f, "%.4f")) { z.center.x = pos[0]; z.center.y = pos[1]; }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt(TL("Min pixels"), &z.minPixels, 1.0f, 1, 100000);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat(TL("Min ratio"), &z.minRatio, 0.0f, 1.0f, "%.2f");
        ImGui::TreePop();
    }

    ImGui::Spacing();
    const float half = (ImGui::GetContentRegionAvail().x - m_.spSM) * 0.5f;
    if (ImGui::Button(TL("Delete zone"), {half, 0})) {
        const int zi = zonesSel_;
        zones.erase(zones.begin() + zi);
        // Renumber references — mirrors the old panel plus graph props.
        for (Monitor& m : automation_->monitors) m.zone = m.zone == zi ? -1 : m.zone > zi ? m.zone - 1 : m.zone;
        for (auto& gg : automation_->studioGraphs)
            for (auto& n : gg.nodes) {
                auto it = n.props.find("zoneIndex");
                if (it != n.props.end() && it->second.type == studio::ValueType::Integer)
                    it->second.i = it->second.i == zi ? -1 : it->second.i > zi ? it->second.i - 1 : it->second.i;
            }
        if (resetMonitorRuntime) resetMonitorRuntime();
        zonesSel_ = -1;
    }
    ImGui::SameLine(0, m_.spSM);
    if (ImGui::Button(TL("Close"), {half, 0})) zonesSel_ = -1;
}

// Rule (Monitor) editor merged from the old Colors panel: watch a zone or an
// image, run a macro or press a key when found, optional not-found branch,
// per-rule arm key and full timing controls.
void StudioEditor::inspectorRules() {
    if (!automation_) return;
    auto& a = *automation_;
    const bool session = sessionActive && sessionActive();

    ImGui::TextWrapped("%s",
                       TL("A rule watches a zone or an image, then runs an action. Arm it with its key or the switch."));
    if (ImGui::Button(TL("+ New rule"), {-FLT_MIN, 0})) {
        Monitor m;
        m.name = i18n::trf("Rule %d", static_cast<int>(a.monitors.size()) + 1);
        m.zone = zonesSel_ >= 0 ? zonesSel_ : 0;
        m.enabled = false;
        a.monitors.push_back(m);
        monitorSel_ = static_cast<int>(a.monitors.size()) - 1;
        if (resetMonitorRuntime) resetMonitorRuntime();
    }

    // ---- rule list -----------------------------------------------------------
    if (!a.monitors.empty()) {
        ImGui::BeginChild("##rule_list", {0, std::min(ImGui::GetContentRegionAvail().y * 0.30f, 110.0f * m_.scale)},
                          ImGuiChildFlags_Borders);
        for (int i = 0; i < static_cast<int>(a.monitors.size()); ++i) {
            Monitor& m = a.monitors[i];
            ImGui::PushID(i);
            if (ImGui::Checkbox("##on", &m.enabled) && resetMonitorRuntime) resetMonitorRuntime();
            ImGui::SameLine();
            std::string label = m.name;
            if (monitorStatus) {
                const auto st = monitorStatus(i);
                if (st.armed) label += "  \xE2\x97\x89";  // ◉
                if (st.present) label += "  [*]";
                if (st.runs > 0) label += i18n::trf("  x%d", st.runs);
            }
            if (ImGui::Selectable(VL(label), i == monitorSel_)) monitorSel_ = (i == monitorSel_ ? -1 : i);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    if (monitorSel_ < 0 || monitorSel_ >= static_cast<int>(a.monitors.size())) monitorSel_ = -1;
    if (monitorSel_ < 0) {
        ImGui::TextDisabled("%s", TL("Select an item to view its settings"));
        return;
    }
    Monitor& m = a.monitors[monitorSel_];
    const MonitorStatus rst = monitorStatus ? monitorStatus(monitorSel_) : MonitorStatus{};

    // ---- armed switch ---------------------------------------------------------
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rst.armed ? pal_.success : pal_.textPrimary), "%s",
                       rst.armed ? (rst.present ? TL("Armed — target detected") : TL("Armed — watching"))
                                 : TL("Off — press its key or Start to arm"));
    ImGui::SameLine();
    if (ImGui::SmallButton(rst.armed ? TL("Stop") : TL("Start")) && armMonitor) armMonitor(monitorSel_);

    char nbuf[96];
    std::snprintf(nbuf, sizeof nbuf, "%s", m.name.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##mname", nbuf, sizeof nbuf)) m.name = nbuf;

    // ---- WHEN -----------------------------------------------------------------
    ImGui::SeparatorText(TL("When"));
    int src = m.image >= 0 ? 1 : 0;
    const char* srcs[] = {TL("a color zone"), TL("an image")};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(TL("Watch for"), src == 1 ? TL("an image") : TL("a color zone"))) {
        for (int s = 0; s < 2; ++s)
            if (ImGui::Selectable(srcs[s], s == src)) {
                if (s == 1) { m.zone = -1; m.image = a.images.empty() ? -1 : 0; }
                else { m.image = -1; m.zone = a.zones.empty() ? -1 : 0; }
                if (resetMonitorRuntime) resetMonitorRuntime();
            }
        ImGui::EndCombo();
    }
    if (m.image >= 0) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(TL("Image"),
                              m.image < static_cast<int>(a.images.size()) ? VL(a.images[m.image].name) : TL("None"))) {
            for (int im = 0; im < static_cast<int>(a.images.size()) && im < kMaxImages; ++im) {
                ImGui::PushID(im);
                if (ImGui::Selectable(VL(a.images[im].name), im == m.image)) {
                    m.image = im;
                    if (resetMonitorRuntime) resetMonitorRuntime();
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (a.images.empty())
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.warning), "%s",
                               TL("Capture an image in the Images tab first."));
    } else {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(TL("Zone"),
                              m.zone >= 0 && m.zone < static_cast<int>(a.zones.size()) ? VL(a.zones[m.zone].name)
                                                                                     : TL("None"))) {
            for (int zi = 0; zi < static_cast<int>(a.zones.size()); ++zi) {
                ImGui::PushID(zi);
                if (ImGui::Selectable(VL(a.zones[zi].name), zi == m.zone)) {
                    m.zone = zi;
                    if (resetMonitorRuntime) resetMonitorRuntime();
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (a.zones.empty()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.warning), "%s", TL("No zones yet."));
            ImGui::BeginDisabled(!session);
            if (ImGui::SmallButton(TL("+ Draw a zone on the game")) && newZone) {
                newZone();
                hideForPick_ = true;
            }
            ImGui::EndDisabled();
        }
    }

    // ---- DO --------------------------------------------------------------------
    ImGui::SeparatorText(TL("Do"));
    const bool isTap = m.macro >= 0 && m.macro < static_cast<int>(a.macros.size()) &&
                       a.macros[m.macro].steps.size() == 1 && a.macros[m.macro].steps[0].type == StepType::Tap;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(TL("Action"),
                          isTap ? TL("Tap a point on screen")
                                : m.macro >= 0 && m.macro < static_cast<int>(a.macros.size())
                                      ? VL(i18n::trf("Run %s", a.macros[m.macro].name.c_str()))
                                      : m.key > 0 ? VL(i18n::trf("Press %s", keyName(m.key).c_str()))
                                                  : TL("Choose…"))) {
        if (ImGui::Selectable(TL("Press a key"), m.macro < 0 && m.key > 0)) m.macro = -1;
        if (ImGui::Selectable(TL("Tap a point on screen"), isTap)) {
            Macro tap;
            tap.name = m.name + i18n::tr(" tap");
            Step s;
            s.type = StepType::Tap;
            s.a = {0.5f, 0.5f};
            tap.steps = {s};
            a.macros.push_back(std::move(tap));
            m.macro = static_cast<int>(a.macros.size()) - 1;
            m.key = 0;
            if (resetMonitorRuntime) resetMonitorRuntime();
        }
        for (int k = 0; k < static_cast<int>(a.macros.size()); ++k) {
            ImGui::PushID(k);
            if (ImGui::Selectable(VL(i18n::trf("Run %s", a.macros[k].name.c_str())), k == m.macro && !isTap))
                m.macro = k;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (isTap) {
        Step& tap = a.macros[m.macro].steps[0];
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##tapx", &tap.a.x, 0.0f, 1.0f, TL("Tap X %.3f"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##tapy", &tap.a.y, 0.0f, 1.0f, TL("Tap Y %.3f"));
        ImGui::BeginDisabled(!session);
        if (ImGui::SmallButton(TL("Pick the point on the game")) && pickPoint) {
            pickPoint(&tap.a);
            hideForPick_ = true;
        }
        ImGui::EndDisabled();
    }
    if (m.macro < 0) {
        const bool waitKey = capturingKey && capturingKey(&m.key);
        const std::string lbl = waitKey ? i18n::tr("Press a key...")
                                        : m.key > 0 ? keyName(m.key) : i18n::tr("Pick the key to press");
        if (waitKey) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(pal_.warning));
        if (ImGui::Button(VL(lbl + "##actkey"), {-FLT_MIN, 0}) && captureKey) captureKey(&m.key, nullptr, 6);
        if (waitKey) ImGui::PopStyleColor();
        if (m.key <= 0)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.warning), "%s",
                               TL("Choose an action before enabling this rule."));
    }

    // ---- REPEAT -----------------------------------------------------------------
    ImGui::SeparatorText(TL("Repeat"));
    int rmode = static_cast<int>(m.mode);
    const char* rmodes[] = {TL("Once — first time it appears"), TL("Keep going while it stays"),
                            TL("Each time it comes back"), TL("A set number of times")};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##rmode", &rmode, rmodes, 4)) {
        m.mode = static_cast<TriggerMode>(rmode);
        if (resetMonitorRuntime) resetMonitorRuntime();
    }
    if (m.mode == TriggerMode::Limited) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt(TL("How many times"), &m.maxRuns, 1.0f, 1, 100000);
    }

    // ---- IF NOT FOUND -------------------------------------------------------------
    ImGui::SeparatorText(TL("If not found"));
    int elseMode = m.elseStop ? 2 : (m.elseMacro >= 0 || m.elseKey > 0) ? 1 : 0;
    const char* elseModes[] = {TL("Keep watching"), TL("Run an action"), TL("Stop the rule")};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##elsemode", &elseMode, elseModes, 3)) {
        if (elseMode == 0) { m.elseStop = false; m.elseMacro = -1; m.elseKey = 0; }
        else if (elseMode == 1) m.elseStop = false;
        else { m.elseStop = true; m.elseMacro = -1; m.elseKey = 0; }
        if (resetMonitorRuntime) resetMonitorRuntime();
    }
    if (elseMode == 1) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt(TL("After missing for ms"), &m.absentMs, 10.0f, 0, 600000);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt(TL("Repeat every ms"), &m.elseCooldownMs, 10.0f, 50, 600000);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(TL("Else action"),
                              m.elseMacro >= 0 && m.elseMacro < static_cast<int>(a.macros.size())
                                  ? VL(i18n::trf("Run %s", a.macros[m.elseMacro].name.c_str()))
                                  : m.elseKey > 0 ? VL(i18n::trf("Press %s", keyName(m.elseKey).c_str()))
                                                  : TL("Choose…"))) {
            if (ImGui::Selectable(TL("Press a key"), m.elseMacro < 0 && m.elseKey > 0)) m.elseMacro = -1;
            for (int k = 0; k < static_cast<int>(a.macros.size()); ++k) {
                if (k == m.macro) continue;
                ImGui::PushID(k);
                if (ImGui::Selectable(VL(i18n::trf("Run %s", a.macros[k].name.c_str())), k == m.elseMacro)) {
                    m.elseMacro = k;
                    m.elseKey = 0;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (m.elseMacro < 0) {
            const bool waitEk = capturingKey && capturingKey(&m.elseKey);
            const std::string lbl = waitEk ? i18n::tr("Press a key...")
                                           : m.elseKey > 0 ? keyName(m.elseKey) : i18n::tr("Pick the key to press");
            if (waitEk) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(pal_.warning));
            if (ImGui::Button(VL(lbl + "##elsekey"), {-FLT_MIN, 0}) && captureKey) captureKey(&m.elseKey, nullptr, 6);
            if (waitEk) ImGui::PopStyleColor();
            if (m.elseKey <= 0)
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.warning), "%s",
                                   TL("Pick what runs while the target stays missing."));
        }
    }
    if (elseMode == 2) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt(TL("After missing for ms"), &m.absentMs, 10.0f, 0, 600000);
    }

    // ---- ACTIVATION KEY -------------------------------------------------------------
    ImGui::SeparatorText(TL("Activation key"));
    {
        const bool waitTg = capturingKey && capturingKey(&m.toggleKey);
        const std::string lbl = m.toggleKey > 0 ? keyName(m.toggleKey) : i18n::tr("None — runs with the global switch");
        if (waitTg) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(pal_.warning));
        if (ImGui::Button(VL(lbl + "##tg"), {-FLT_MIN, 0}) && captureKey) captureKey(&m.toggleKey, nullptr, 7);
        if (waitTg) ImGui::PopStyleColor();
        if (m.toggleKey > 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton(TL("Clear##tg"))) m.toggleKey = 0;
        }
    }

    if (ImGui::TreeNode(TL("Advanced settings"))) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt(TL("Scan every ms"), &m.intervalMs, 5.0f, 10, 5000);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt(TL("Confirm samples"), &m.confirm, 1.0f, 1, 20);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt(TL("Cooldown ms"), &m.cooldownMs, 10.0f, 0, 600000);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt(TL("Delay ms"), &m.delayMs, 10.0f, 0, 600000);
        ImGui::TreePop();
    }

    ImGui::Spacing();
    const float half = (ImGui::GetContentRegionAvail().x - m_.spSM) * 0.5f;
    if (ImGui::Button(TL("Delete rule"), {half, 0})) {
        a.monitors.erase(a.monitors.begin() + monitorSel_);
        monitorSel_ = -1;
        if (resetMonitorRuntime) resetMonitorRuntime();
    }
    ImGui::SameLine(0, m_.spSM);
    if (ImGui::Button(TL("Close"), {half, 0})) monitorSel_ = -1;
}

void StudioEditor::inspectorGraph() {
    auto* g = graph();
    if (!g) return;
    char nbuf[160];
    std::snprintf(nbuf, sizeof nbuf, "%s", g->name.c_str());
    ImGui::TextUnformatted(TL("Graph name"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##gname", nbuf, sizeof nbuf)) g->name = nbuf;

    ImGui::TextUnformatted(TL("Description"));
    char dbuf[512];
    std::snprintf(dbuf, sizeof dbuf, "%s", g->metadata.description.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextMultiline("##gdesc", dbuf, sizeof dbuf, {-FLT_MIN, 60.0f * m_.scale}))
        g->metadata.description = dbuf;

    ImGui::Spacing();
    // Entry triggers: every Start node can carry its own hotkey — one graph
    // can host several independent flows ("macros"). The first Start also
    // inherits the graph-level metadata key from older profiles.
    if (panelHeader(TL("Entry triggers"), pal_.textSecondary)) {
        int startCount = 0;
        for (auto& n : g->nodes) {
            if (n.type != studio::NodeType::Start) continue;
            ++startCount;
            ImGui::PushID(n.id);
            const std::string nm = n.title.empty() ? i18n::trf("Start %d", startCount) : n.title;
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.accentPrimary), "%s", VL(nm));
            triggerKeyRowFor(&n.triggerKey, &n.triggerKey2,
                             startCount == 1 ? &g->metadata.triggerKey : nullptr,
                             startCount == 1 ? &g->metadata.triggerKey2 : nullptr);
            ImGui::PopID();
        }
        if (startCount == 0)
            ImGui::TextDisabled("%s", TL("No Start node — add one to give this graph an entry point."));
        ImGui::TreePop();
    }

    // ---- Global automation (merged from the old Colors panel status card) ------
    if (automation_ && panelHeader(TL("Global automation"), pal_.textSecondary)) {
        const bool on = automationOn && automationOn();
        ImU32 dot = on ? pal_.success : pal_.textDim;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(dot), "%s",
                           on ? TL("Monitoring is running") : TL("Monitoring is stopped"));
        ImGui::SameLine();
        if (ImGui::SmallButton(on ? TL("Stop") : TL("Start")) && setAutomationOn) setAutomationOn(!on);

        // Toggle / emergency-stop key (automation.toggleKey, capture slot 7).
        int& tk = automation_->toggleKey;
        const bool waitTg = capturingKey && capturingKey(&tk);
        const std::string lbl = waitTg ? i18n::tr("Press a key...")
                                       : tk > 0 ? i18n::trf("Toggle / emergency stop key: %s", keyName(tk).c_str())
                                                : i18n::tr("Toggle / emergency stop key: none");
        if (waitTg) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(pal_.warning));
        if (ImGui::Button(VL(lbl), {-FLT_MIN, 0}) && captureKey) captureKey(&tk, nullptr, 7);
        if (waitTg) ImGui::PopStyleColor();
        ImGui::TreePop();
    }

    ImGui::TextUnformatted(TL("Max duration (ms)"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragInt("##gtd", &g->metadata.maxDurationMs, 100.0f, 0, 600000);
    ImGui::TextUnformatted(TL("Max steps budget"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragInt("##gts", &g->metadata.maxStepsBudget, 100.0f, 10, 1000000);

    ImGui::Spacing();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "%s",
                       VL(i18n::trf("%d nodes · %d wires · %d variables",
                                    static_cast<int>(g->nodes.size()), static_cast<int>(g->connections.size()),
                                    static_cast<int>(g->variables.size()))));
    const auto issues = studio::validateGraph(*g);
    if (!issues.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.warning), "%s",
                           VL(i18n::trf("%d validation issue(s)", static_cast<int>(issues.size()))));
        for (const auto& s : issues)
            ImGui::BulletText("%s", VL(s));
    }

    // ---- Legacy Simple-Mode interop (Phase 9) -------------------------------
    if (!g->metadata.sourceLegacyMacro.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "%s",
                           VL(i18n::trf("Converted from macro: %s",
                                        g->metadata.sourceLegacyMacro.c_str())));
    }
    if (!g->metadata.conversionNotes.empty() &&
        ImGui::TreeNode(VL(i18n::trf("Conversion report (%d)",
                                    static_cast<int>(g->metadata.conversionNotes.size()))))) {
        for (const auto& note : g->metadata.conversionNotes) ImGui::BulletText("%s", note.c_str());
        ImGui::TreePop();
    }
    ImGui::Spacing();
    if (ImGui::Button(TL("Export to Simple Mode"), {-FLT_MIN, 0})) {
        studio::SimplifyReport rep = studio::graphToMacroSteps(*g);
        Macro nm;
        nm.name = g->name + " (steps)";
        nm.key = g->metadata.triggerKey;
        nm.key2 = g->metadata.triggerKey2;
        nm.maxDurationMs = g->metadata.maxDurationMs;
        nm.steps = std::move(rep.steps);
        const int created = static_cast<int>(nm.steps.size());
        automation_->macros.push_back(std::move(nm));
        if (rep.lossless())
            log(i18n::trf("Exported %d steps to Simple Mode (lossless)", created),
                ImGui::ColorConvertFloat4ToU32({0.35f, 0.9f, 0.5f, 1.0f}));
        else {
            log(i18n::trf("Exported %d steps to Simple Mode with %d warning(s)", created,
                          static_cast<int>(rep.warnings.size())),
                ImGui::ColorConvertFloat4ToU32({0.95f, 0.75f, 0.25f, 1.0f}));
            for (const auto& wmsg : rep.warnings) log(wmsg, pal_.warning);
        }
        if (toast) toast(i18n::trf("Macro exported to Simple Mode (%d steps)", created));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TL("Creates a new Simple-Mode macro — this graph stays untouched."));
}

void StudioEditor::inspectorVariables() {
    auto* g = graph();
    if (!g) return;
    if (ImGui::SmallButton(TL("+ Variable"))) {
        studio::GraphVariable nv;
        nv.name = i18n::trf("var%d", static_cast<int>(g->variables.size()) + 1);
        nv.type = studio::ValueType::Integer;
        nv.initialValue = studio::Value::makeInt(0);
        g->upsertVariable(nv);
        commit();
    }
    int del = -1;
    for (int i = 0; i < static_cast<int>(g->variables.size()); ++i) {
        auto& var = g->variables[i];
        ImGui::PushID(i);
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextUnformatted(var.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "(%s)",
                           studio::valueTypeName(var.type));
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 24.0f * m_.scale);
        if (ImGui::SmallButton("x")) del = i;
        ImGui::PopID();
    }
    if (del >= 0) {
        g->removeVariable(g->variables[del].name);
        commit();
    }
}

void StudioEditor::drawInspector(float w, float h) {
    ImGui::BeginChild("##studio_insp", {w, h}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    auto* g = graph();

    // tabs: selection / graph / variables / images / zones / rules — the last
    // three merge the old standalone automation panels into the studio.
    const int selCount = static_cast<int>(sel_.size());
    const uint32_t curSel = selCount == 1 ? *sel_.begin() : 0;
    // ImGuiTabItemFlags_SetSelected must fire only on the frame the selection
    // *changes* — applying it every frame locks the bar on "Node" and makes the
    // other tabs unclickable while a node stays selected.
    const bool selChanged = curSel != lastInspNode_;
    lastInspNode_ = curSel;
    const int reqTab = inspTab_;  // external focus request (-1 = none)
    inspTab_ = -1;
    if (ImGui::BeginTabBar("##insp_tabs", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton)) {
        const std::string selTab = selCount == 1 ? i18n::tr("Node")
                                   : selCount > 1 ? i18n::trf("Selection (%d)", selCount)
                                                  : i18n::tr("Node");
        if (ImGui::BeginTabItem(VL(selTab), nullptr,
                                (selChanged && selCount == 1) || reqTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (!g) {
                ImGui::TextDisabled("%s", TL("Create a graph first."));
            } else if (selCount == 1) {
                if (auto* n = g->findNode(*sel_.begin())) inspectorNode(*n);
            } else if (selCount > 1) {
                ImGui::TextWrapped("%s", VL(i18n::trf("%d nodes selected", selCount)));
                if (ImGui::Button(TL("Group selection"), {-FLT_MIN, 0})) groupSelection();
                if (ImGui::Button(TL("Duplicate"), {-FLT_MIN, 0})) duplicateSelection();
                if (ImGui::Button(TL("Delete"), {-FLT_MIN, 0})) deleteSelection();
            } else if (selConn_) {
                ImGui::TextUnformatted(TL("Connection"));
                if (ImGui::Button(TL("Delete wire"), {-FLT_MIN, 0})) {
                    if (g->disconnect(selConn_)) { selConn_ = 0; commit(); }
                }
            } else {
                ImGui::TextDisabled("%s", TL("Select a node to edit its properties."));
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(TL("Graph"), nullptr, reqTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (g) inspectorGraph();
            else ImGui::TextDisabled("%s", TL("Create a graph first."));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(TL("Variables"), nullptr, reqTab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (g) inspectorVariables();
            else ImGui::TextDisabled("%s", TL("Create a graph first."));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(TL("Images"), nullptr, reqTab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
            inspectorImages();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(TL("Zones"), nullptr, reqTab == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
            inspectorZones();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(TL("Rules"), nullptr, reqTab == 5 ? ImGuiTabItemFlags_SetSelected : 0)) {
            inspectorRules();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

// ============================================================================
// Console + status bar + empty state
// ============================================================================

void StudioEditor::drawConsole(float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // Top-edge drag handle resizing the console height.
    ImGui::InvisibleButton("##console_grip", {w, 5.0f * m_.scale});
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive())
        consoleH_ = std::clamp(consoleH_ - ImGui::GetIO().MouseDelta.y / m_.scale, 70.0f, 420.0f);
    dl->AddLine(p, {p.x + w, p.y}, pal_.divider);
    ImGui::BeginChild("##studio_console", {w, h}, ImGuiChildFlags_AlwaysUseWindowPadding);
    if (ImGui::SmallButton(TL("Clear"))) console_.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton(consolePinned_ ? TL("Hide") : TL("Pin"))) {
        consolePinned_ = !consolePinned_;
        if (!consolePinned_) consoleH_ = 0.0f;
    }
    ImGui::Separator();
    ImGui::BeginChild("##studio_console_lines", {0, 0});
    for (const auto& l : console_)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(l.col), "%s", VL(l.text));
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::EndChild();
}

void StudioEditor::drawStatusBar(float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = m_.statusBarH;
    dl->AddRectFilled(p, {p.x + w, p.y + h}, pal_.panelBg);
    dl->AddLine(p, {p.x + w, p.y}, pal_.divider);
    ImGui::BeginChild("##studio_status", {w, h}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPosY((h - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "  ");
    const auto* g = graph();
    if (g) {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textSecondary), "%s", VL(g->name));
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "·  %s",
                           VL(i18n::trf("%d nodes", static_cast<int>(g->nodes.size()))));
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "·  %d%%",
                           static_cast<int>(view().zoom * 100));
        if (sessionActive && sessionActive()) {
            ImGui::SameLine();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.success), "·  %s", TL("device connected"));
        }
    } else {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textDim), "%s", TL("No graph open"));
    }
    ImGui::EndChild();
}

void StudioEditor::drawEmptyState(float w, float h) {
    ImGui::BeginChild("##studio_empty", {w, h}, ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, {p.x + w, p.y + h}, pal_.canvasBg);

    const float cardW = std::min(560.0f * m_.scale, w - 40.0f);
    const float cx = p.x + (w - cardW) * 0.5f;
    float cy = p.y + h * 0.18f;
    dl->AddRectFilled({cx, cy}, {cx + cardW, cy + 150.0f * m_.scale}, pal_.surfaceCard, m_.radiusLG);
    dl->AddRect({cx, cy}, {cx + cardW, cy + 150.0f * m_.scale}, pal_.divider, m_.radiusLG);
    ImGui::SetCursorScreenPos({cx + m_.spXL, cy + m_.spXL});
    ImGui::BeginChild("##empty_inner", {cardW - 2 * m_.spXL, 150.0f * m_.scale - 2 * m_.spXL});
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.25f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.textPrimary), "%s",
                       TL("NEXPLAY Automation Studio"));
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", TL("Build automation as a visual graph: nodes, wires, variables, "
                                "image and color recognition. Pick a template to begin."));
    ImGui::Spacing();
    if (ImGui::Button(TL("Blank graph"), {140 * m_.scale, 30 * m_.scale}))
        newGraph(studio::GraphTemplateKind::BasicSequence);
    ImGui::EndChild();

    // template cards row
    cy += 150.0f * m_.scale + m_.spLG;
    static const char* names[] = {"Basic sequence", "Conditional flow", "Image recognition",
                                  "Color detection", "Loop", "Variables"};
    const int n = 6;
    const float gap = m_.spMD;
    const float tw = (cardW - gap * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
        const float tx = cx + i * (tw + gap);
        const float th = 84.0f * m_.scale;
        ImGui::SetCursorScreenPos({tx, cy});
        ImGui::PushID(i);
        const bool click = ImGui::InvisibleButton("##tpl", {tw, th});
        const bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled({tx, cy}, {tx + tw, cy + th}, hov ? pal_.surfaceHover : pal_.surfaceCard,
                          m_.radiusMD);
        dl->AddRect({tx, cy}, {tx + tw, cy + th}, hov ? pal_.accentPrimary : pal_.divider, m_.radiusMD);
        const ImVec2 ts = ImGui::CalcTextSize(TL(names[i]));
        dl->AddText({tx + (tw - ts.x) * 0.5f, cy + th * 0.5f - ts.y * 0.5f}, pal_.textSecondary,
                    TL(names[i]));
        if (click) newGraph(static_cast<studio::GraphTemplateKind>(i));
        ImGui::PopID();
    }
    ImGui::EndChild();
}

}  // namespace wasd::app
