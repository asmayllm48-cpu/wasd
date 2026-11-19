// NEXPLAY Automation Studio — main shell layout + infinite canvas.
// Canvas is logically LTR-independent: node inputs stay on the left edge and
// outputs on the right edge regardless of UI text direction (spec §Canvas).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "app/i18n.h"
#include "app/studio_ui.h"
#include "app/text_shaping.h"
#include "imgui_internal.h"

namespace wasd::app {
namespace {

const char* VL(const std::string& logical) { return text::visual(logical).c_str(); }
const char* TL(const char* en) { return text::visual(i18n::tr(en)).c_str(); }
const char* TLs(const std::string& en) { return text::visual(i18n::tr(en.c_str())).c_str(); }

ImVec2 imv(Vec2 v) { return {v.x, v.y}; }
Vec2 vec(ImVec2 v) { return {v.x, v.y}; }

ImU32 mix(ImU32 a, ImU32 b, float t) {
    const auto ch = [&](int shift) {
        const int va = (a >> shift) & 0xFF, vb = (b >> shift) & 0xFF;
        return static_cast<ImU32>(va + (vb - va) * t) << shift;
    };
    return ch(0) | ch(8) | ch(16) | ch(24);
}

void arrowHead(ImDrawList* dl, ImVec2 tip, ImVec2 dir, float size, ImU32 col) {
    const float len = std::max(0.001f, std::sqrt(dir.x * dir.x + dir.y * dir.y));
    const ImVec2 u{dir.x / len, dir.y / len}, n{-u.y, u.x};
    dl->AddTriangleFilled(tip, {tip.x - u.x * size + n.x * size * 0.55f, tip.y - u.y * size + n.y * size * 0.55f},
                          {tip.x - u.x * size - n.x * size * 0.55f, tip.y - u.y * size - n.y * size * 0.55f}, col);
}

// Execution ports are drawn as a small chevron/pentagon; data ports as circles.
void execPortGlyph(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool filled) {
    const ImVec2 pts[5] = {{c.x - r * 0.55f, c.y - r}, {c.x + r * 0.45f, c.y - r},
                           {c.x + r, c.y}, {c.x + r * 0.45f, c.y + r}, {c.x - r * 0.55f, c.y + r}};
    if (filled) dl->AddConvexPolyFilled(pts, 5, col);
    dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed, 1.4f);
}

bool rectIntersects(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

}  // namespace

// ============================================================================
// Document access
// ============================================================================

StudioEditor::StudioEditor() = default;

studio::AutomationGraph* StudioEditor::graph() {
    if (!automation_ || graphIndex_ < 0 || graphIndex_ >= static_cast<int>(automation_->studioGraphs.size()))
        return nullptr;
    return &automation_->studioGraphs[graphIndex_];
}

const studio::AutomationGraph* StudioEditor::graph() const {
    return const_cast<StudioEditor*>(this)->graph();
}

studio::AutomationGraph& StudioEditor::ensureGraph() {
    if (!graph()) {
        automation_->studioGraphs.push_back(studio::makeStarterGraph());
        graphIndex_ = static_cast<int>(automation_->studioGraphs.size()) - 1;
        history_.reset(automation_->studioGraphs[graphIndex_]);
    }
    return automation_->studioGraphs[graphIndex_];
}

void StudioEditor::selectGraph(int i) {
    if (!automation_ || i < 0 || i >= static_cast<int>(automation_->studioGraphs.size())) return;
    graphIndex_ = i;
    clearSel();
    drag_ = Drag::None;
    history_.reset(automation_->studioGraphs[i]);
}

void StudioEditor::newGraph(studio::GraphTemplateKind kind) {
    if (!automation_) return;
    automation_->studioGraphs.push_back(studio::makeTemplateGraph(kind));
    selectGraph(static_cast<int>(automation_->studioGraphs.size()) - 1);
}

void StudioEditor::deleteGraph(int i) {
    if (!automation_ || i < 0 || i >= static_cast<int>(automation_->studioGraphs.size())) return;
    automation_->studioGraphs.erase(automation_->studioGraphs.begin() + i);
    if (automation_->studioGraphs.empty()) graphIndex_ = -1;
    else selectGraph(std::min(i, static_cast<int>(automation_->studioGraphs.size()) - 1));
}

studio::CanvasViewState& StudioEditor::view() { return ensureGraph().view; }
const studio::CanvasViewState& StudioEditor::view() const {
    static const studio::CanvasViewState kFallback{};
    return graph() ? graph()->view : kFallback;
}

void StudioEditor::commit() {
    if (auto* g = graph()) history_.commit(*g);
}

void StudioEditor::undoHistory() {
    if (auto* g = graph())
        if (history_.undo(*g)) clearSel();
}

void StudioEditor::redoHistory() {
    if (auto* g = graph())
        if (history_.redo(*g)) clearSel();
}

void StudioEditor::clearSel() {
    sel_.clear();
    selNotes_.clear();
    selConn_ = 0;
    selGroup_ = 0;
}

void StudioEditor::log(const std::string& msg, ImU32 col) {
    console_.push_back({col ? col : pal_.textSecondary, msg});
    if (console_.size() > 400) console_.erase(console_.begin(), console_.begin() + 100);
}

// Merge live GraphRunner events into the studio console (new run resets the vector).
void StudioEditor::drainRunnerEvents() {
    if (!runtime.events) return;
    const auto* ev = runtime.events();
    if (!ev) { seenEvents_ = 0; return; }
    if (seenEvents_ > ev->size()) seenEvents_ = 0;  // a new run restarted the log
    for (; seenEvents_ < ev->size(); ++seenEvents_) {
        const studio::GraphEvent& e = (*ev)[seenEvents_];
        ImU32 col = e.level == 2 ? pal_.danger : e.level == 1 ? pal_.warning : pal_.textSecondary;
        char ts[24];
        std::snprintf(ts, sizeof ts, "[%02d:%06.3f]", static_cast<int>(e.us / 60000000),
                      static_cast<double>(e.us % 60000000) / 1000.0);
        log(std::string(ts) + " " + e.text, col);
    }
}

// ============================================================================
// Transform & layout
// ============================================================================

ImVec2 StudioEditor::toScreen(Vec2 c) const {
    const auto& v = view();
    return {canvasOrigin_.x + (c.x + v.pan.x) * v.zoom, canvasOrigin_.y + (c.y + v.pan.y) * v.zoom};
}

Vec2 StudioEditor::toCanvas(ImVec2 s) const {
    const auto& v = view();
    return {(s.x - canvasOrigin_.x) / v.zoom - v.pan.x, (s.y - canvasOrigin_.y) / v.zoom - v.pan.y};
}

float StudioEditor::gridStep() const { return view().gridSize; }

ImVec2 StudioEditor::nodeSize(const studio::GraphNode& n) const {
    const float s = m_.scale;
    if (n.collapsed) return {m_.nodeMinW, m_.nodeHeaderH};
    float wIn = 0, wOut = 0;
    for (const auto& p : n.inputs)
        wIn = std::max(wIn, ImGui::CalcTextSize(VL(i18n::tr(p.name.c_str()))).x);
    for (const auto& p : n.outputs)
        wOut = std::max(wOut, ImGui::CalcTextSize(VL(i18n::tr(p.name.c_str()))).x);
    float w = std::max(m_.nodeMinW, wIn + wOut + m_.spXL * 2.4f);
    const float titleW = ImGui::CalcTextSize(VL(nodeTitle(n))).x + 64.0f * s;
    w = std::max(w, std::min(titleW, 360.0f * s));
    const size_t rows = std::max(n.inputs.size(), n.outputs.size());
    float h = m_.nodeHeaderH + static_cast<float>(rows) * m_.nodeRowH + m_.spSM;
    return {w, h};
}

Vec2 StudioEditor::portPos(const studio::GraphNode& n, const studio::Port& p) const {
    const auto& list = (p.dir == studio::PortDir::Input) ? n.inputs : n.outputs;
    int idx = 0;
    for (const auto& q : list) {
        if (q.id == p.id) break;
        ++idx;
    }
    const ImVec2 sz = nodeSize(n);
    const float x = (p.dir == studio::PortDir::Input) ? n.pos.x : n.pos.x + sz.x;
    const float y = n.pos.y + m_.nodeHeaderH + idx * m_.nodeRowH + m_.nodeRowH * 0.5f;
    return {x, y};
}

std::string StudioEditor::nodeTitle(const studio::GraphNode& n) const {
    if (!n.title.empty()) return n.title;
    if (const auto* spec = studio::findNodeSpec(n.type, n.customTypeKey)) return spec->displayName;
    return "Node";
}

ImU32 StudioEditor::nodeAccent(const studio::GraphNode& n) const {
    if (const auto* spec = studio::findNodeSpec(n.type, n.customTypeKey))
        return studio_ui::categoryAccent(spec->category);
    return pal_.accentPrimary;
}

// Compact one-line summary of the first non-default property (e.g. "250 ms").
std::string StudioEditor::nodeSummary(const studio::GraphNode& n) const {
    const auto* spec = studio::findNodeSpec(n.type, n.customTypeKey);
    if (!spec) return {};
    char buf[96];
    for (const auto& ps : spec->properties) {
        if (ps.advanced) continue;
        const auto it = n.props.find(ps.key);
        const studio::Value& v = (it != n.props.end()) ? it->second : ps.defaultValue;
        const char* pl = i18n::tr(ps.label.c_str());
        switch (v.type) {
            case studio::ValueType::Integer:
                if (v.i != ps.defaultValue.i || true) { std::snprintf(buf, sizeof buf, "%s: %lld", pl, static_cast<long long>(v.i)); return buf; }
                break;
            case studio::ValueType::Float:
                std::snprintf(buf, sizeof buf, "%s: %.3g", pl, v.f);
                return buf;
            case studio::ValueType::Boolean:
                std::snprintf(buf, sizeof buf, "%s: %s", pl, i18n::tr(v.b ? "on" : "off"));
                return buf;
            case studio::ValueType::String:
                if (!v.s.empty()) { std::snprintf(buf, sizeof buf, "%s: %.18s", pl, v.s.c_str()); return buf; }
                break;
            case studio::ValueType::Coordinate:
            case studio::ValueType::Vector2:
                std::snprintf(buf, sizeof buf, "(%.3g, %.3g)", v.v.x, v.v.y);
                return buf;
            default:
                break;
        }
        break;  // only the first non-advanced property
    }
    return {};
}

// ============================================================================
// Hit testing
// ============================================================================

namespace {
// Nodes hidden because their frame is collapsed.
std::set<uint32_t> hiddenNodes(const studio::AutomationGraph& g) {
    std::set<uint32_t> out;
    for (const auto& gr : g.groups)
        if (gr.collapsed)
            for (uint32_t id : gr.nodeIds) out.insert(id);
    return out;
}
}  // namespace

uint32_t StudioEditor::nodeAt(Vec2 c) const {
    const auto* g = graph();
    if (!g) return 0;
    const auto hidden = hiddenNodes(*g);
    for (auto it = g->nodes.rbegin(); it != g->nodes.rend(); ++it) {
        if (hidden.count(it->id)) continue;
        const ImVec2 sz = nodeSize(*it);
        const Rect r{it->pos.x, it->pos.y, sz.x, sz.y};
        if (r.contains(c)) return it->id;
    }
    return 0;
}

bool StudioEditor::portAt(Vec2 c, uint32_t& nodeId, uint32_t& portId) const {
    const auto* g = graph();
    if (!g) return false;
    const auto hidden = hiddenNodes(*g);
    const float r = (m_.portHitRadius + 2.0f) / std::max(0.2f, view().zoom);
    const float r2 = r * r;
    for (auto it = g->nodes.rbegin(); it != g->nodes.rend(); ++it) {
        if (hidden.count(it->id) || it->collapsed) continue;
        const ImVec2 sz = nodeSize(*it);
        const Rect nr{it->pos.x - r, it->pos.y, sz.x + 2 * r, sz.y};
        if (!nr.contains(c)) continue;
        for (const auto* list : {&it->inputs, &it->outputs})
            for (const auto& p : *list) {
                const Vec2 pp = portPos(*it, p);
                const float dx = c.x - pp.x, dy = c.y - pp.y;
                if (dx * dx + dy * dy <= r2) {
                    nodeId = it->id;
                    portId = p.id;
                    return true;
                }
            }
    }
    return false;
}

// Distance from point to the connection curve (sampled polyline, screen space).
uint32_t StudioEditor::connAt(Vec2 c) const {
    const auto* g = graph();
    if (!g) return 0;
    const auto hidden = hiddenNodes(*g);
    const ImVec2 m = toScreen(c);
    const float tol = 6.0f;
    for (auto it = g->connections.rbegin(); it != g->connections.rend(); ++it) {
        const studio::GraphNode *na, *nb;
        const studio::Port *pa, *pb;
        if (!g->findPortOwner(it->fromPort, na, pa) || !g->findPortOwner(it->toPort, nb, pb)) continue;
        if (hidden.count(na->id) || hidden.count(nb->id)) continue;
        const ImVec2 a = toScreen(portPos(*na, *pa));
        const ImVec2 b = toScreen(portPos(*nb, *pb));
        const float dx = std::max(30.0f * view().zoom, std::fabs(b.x - a.x) * 0.5f);
        ImVec2 prev = a;
        for (int i = 1; i <= 20; ++i) {
            const float t = i / 20.0f, u = 1.0f - t;
            ImVec2 p;
            p.x = u * u * u * a.x + 3 * u * u * t * (a.x + dx) + 3 * u * t * t * (b.x - dx) + t * t * t * b.x;
            p.y = u * u * u * a.y + 3 * u * u * t * a.y + 3 * u * t * t * b.y + t * t * t * b.y;
            // segment distance
            const float ex = p.x - prev.x, ey = p.y - prev.y;
            const float l2 = ex * ex + ey * ey;
            float tt = l2 > 0 ? ((m.x - prev.x) * ex + (m.y - prev.y) * ey) / l2 : 0;
            tt = std::clamp(tt, 0.0f, 1.0f);
            const float px = prev.x + ex * tt - m.x, py = prev.y + ey * tt - m.y;
            if (px * px + py * py <= tol * tol) return it->id;
            prev = p;
        }
    }
    return 0;
}

uint32_t StudioEditor::noteAt(Vec2 c) const {
    const auto* g = graph();
    if (!g) return 0;
    for (auto it = g->notes.rbegin(); it != g->notes.rend(); ++it)
        if (Rect{it->pos.x, it->pos.y, it->size.x, it->size.y}.contains(c)) return it->id;
    return 0;
}

uint32_t StudioEditor::groupAt(Vec2 c, int& corner) const {
    const auto* g = graph();
    corner = -1;
    if (!g) return 0;
    const float grab = 10.0f / std::max(0.2f, view().zoom);
    for (auto it = g->groups.rbegin(); it != g->groups.rend(); ++it) {
        const Rect& r = it->rect;
        if (it->collapsed) {
            if (Rect{r.x, r.y, std::max(r.w, 180.0f), 30.0f}.contains(c)) return it->id;
            continue;
        }
        if (!r.contains(c)) continue;
        const Vec2 corners[4] = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x, r.y + r.h}, {r.x + r.w, r.y + r.h}};
        for (int i = 0; i < 4; ++i) {
            const float dx = c.x - corners[i].x, dy = c.y - corners[i].y;
            if (std::fabs(dx) <= grab && std::fabs(dy) <= grab) {
                corner = i;
                return it->id;
            }
        }
        return it->id;
    }
    return 0;
}

// ============================================================================
// Editing operations
// ============================================================================

void StudioEditor::addNodeAt(studio::NodeType t, Vec2 pos, const std::string& customKey) {
    auto& g = ensureGraph();
    if (static_cast<int>(g.nodes.size()) >= studio::kMaxGraphNodes) {
        if (toast) toast(i18n::tr("Graph node limit reached"));
        return;
    }
    auto& n = g.addNode(t, pos, customKey);
    if (const auto* spec = studio::findNodeSpec(t, customKey)) pushRecent(*spec);
    clearSel();
    sel_.insert(n.id);
    commit();
}

void StudioEditor::deleteSelection() {
    auto* g = graph();
    if (!g) return;
    bool changed = false;
    if (selConn_) changed |= g->disconnect(selConn_);
    if (selGroup_) changed |= g->removeGroup(selGroup_);
    for (uint32_t id : selNotes_) changed |= g->removeNote(id);
    for (uint32_t id : sel_) changed |= g->removeNode(id);
    if (changed) {
        clearSel();
        commit();
    }
}

void StudioEditor::copySelection(bool cut) {
    auto* g = graph();
    if (!g || (sel_.empty() && selNotes_.empty())) return;
    clipboard_ = g->copySelection({sel_.begin(), sel_.end()}, {selNotes_.begin(), selNotes_.end()});
    if (cut) {
        for (uint32_t id : sel_) g->removeNode(id);
        for (uint32_t id : selNotes_) g->removeNote(id);
        clearSel();
        commit();
    }
}

void StudioEditor::pasteAt(Vec2 canvasPos) {
    auto* g = graph();
    if (!g || clipboard_.empty()) return;
    if (static_cast<int>(g->nodes.size() + clipboard_.nodes.size()) > studio::kMaxGraphNodes) return;
    // Offset so the pasted cluster lands on the cursor.
    Vec2 min{FLT_MAX, FLT_MAX};
    for (const auto& n : clipboard_.nodes) { min.x = std::min(min.x, n.pos.x); min.y = std::min(min.y, n.pos.y); }
    for (const auto& n : clipboard_.notes) { min.x = std::min(min.x, n.pos.x); min.y = std::min(min.y, n.pos.y); }
    const Vec2 off{canvasPos.x - min.x, canvasPos.y - min.y};
    const auto ids = g->pasteClipboard(clipboard_, off);
    clearSel();
    // pasteClipboard returns new node ids followed by new note ids.
    for (size_t i = 0; i < ids.size(); ++i)
        if (i < clipboard_.nodes.size()) sel_.insert(ids[i]);
        else selNotes_.insert(ids[i]);
    commit();
}

void StudioEditor::duplicateSelection() {
    auto* g = graph();
    if (!g || sel_.empty()) return;
    const auto clip = g->copySelection({sel_.begin(), sel_.end()}, {selNotes_.begin(), selNotes_.end()});
    const auto ids = g->pasteClipboard(clip, {40.0f, 40.0f});
    clearSel();
    for (size_t i = 0; i < ids.size(); ++i)
        if (i < clip.nodes.size()) sel_.insert(ids[i]);
        else selNotes_.insert(ids[i]);
    commit();
}

void StudioEditor::selectAll() {
    auto* g = graph();
    if (!g) return;
    clearSel();
    for (const auto& n : g->nodes) sel_.insert(n.id);
    for (const auto& n : g->notes) selNotes_.insert(n.id);
}

void StudioEditor::groupSelection() {
    auto* g = graph();
    if (!g || sel_.size() < 2) return;
    Vec2 lo{FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX};
    for (uint32_t id : sel_)
        if (const auto* n = g->findNode(id)) {
            const ImVec2 sz = nodeSize(*n);
            lo.x = std::min(lo.x, n->pos.x); lo.y = std::min(lo.y, n->pos.y);
            hi.x = std::max(hi.x, n->pos.x + sz.x); hi.y = std::max(hi.y, n->pos.y + sz.y);
        }
    const float pad = 28.0f;
    g->addGroup("Group", {lo.x - pad, lo.y - pad - 14.0f, hi.x - lo.x + 2 * pad, hi.y - lo.y + 2 * pad + 14.0f},
                {sel_.begin(), sel_.end()});
    commit();
}

void StudioEditor::beginWire(uint32_t nodeId, uint32_t portId) {
    auto* g = graph();
    if (!g) return;
    const studio::GraphNode* n = g->findNode(nodeId);
    if (!n) return;
    const studio::Port* p = n->findPort(portId);
    if (!p) return;
    wireSrcOtherConn_ = 0;
    wireSrcNode_ = nodeId;
    wireSrcPort_ = portId;
    // Pulling a connected Input detaches its wire and drags from the source Output.
    if (p->dir == studio::PortDir::Input) {
        for (const auto& c : g->connections)
            if (c.toPort == portId) {
                wireSrcOtherConn_ = c.id;
                wireSrcNode_ = c.fromNode;
                wireSrcPort_ = c.fromPort;
                break;
            }
    }
    drag_ = Drag::Wire;
}

void StudioEditor::dropWire() {
    auto* g = graph();
    if (!g) { drag_ = Drag::None; return; }
    uint32_t tNode = 0, tPort = 0;
    const Vec2 mc = toCanvas(ImGui::GetIO().MousePos);
    if (portAt(mc, tNode, tPort)) {
        auto chk = g->connect(wireSrcNode_, wireSrcPort_, tNode, tPort, nullptr, true);
        if (chk.ok) {
            if (wireSrcOtherConn_) g->disconnect(wireSrcOtherConn_);
            commit();
        } else if (toast) {
            toast(chk.reason);
            log(chk.reason, pal_.warning);
        }
    } else {
        // Dropped on empty canvas: offer a filtered quick-create menu.
        const float dx = ImGui::GetIO().MousePos.x - dragMouse0_.x;
        const float dy = ImGui::GetIO().MousePos.y - dragMouse0_.y;
        if (dx * dx + dy * dy > 64.0f) {
            quickAt_ = toCanvas(ImGui::GetIO().MousePos);
            quickFilter_[0] = '\0';
            quickMenu_ = true;
            ImGui::OpenPopup("##studio_quick_create");
        }
    }
    if (!quickMenu_) { wireSrcNode_ = wireSrcPort_ = wireSrcOtherConn_ = 0; }
    drag_ = Drag::None;
}

void StudioEditor::fitView() {
    auto* g = graph();
    if (!g || canvasSize_.x < 8) return;
    Vec2 lo{FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX};
    bool any = false;
    for (const auto& n : g->nodes) {
        const ImVec2 sz = nodeSize(n);
        lo.x = std::min(lo.x, n.pos.x); lo.y = std::min(lo.y, n.pos.y);
        hi.x = std::max(hi.x, n.pos.x + sz.x); hi.y = std::max(hi.y, n.pos.y + sz.y);
        any = true;
    }
    for (const auto& n : g->notes) {
        lo.x = std::min(lo.x, n.pos.x); lo.y = std::min(lo.y, n.pos.y);
        hi.x = std::max(hi.x, n.pos.x + n.size.x); hi.y = std::max(hi.y, n.pos.y + n.size.y);
        any = true;
    }
    if (!any) return;
    auto& v = view();
    const float pad = 60.0f;
    const float bw = std::max(1.0f, hi.x - lo.x + 2 * pad), bh = std::max(1.0f, hi.y - lo.y + 2 * pad);
    v.zoom = std::clamp(std::min(canvasSize_.x / bw, canvasSize_.y / bh), 0.2f, 2.0f);
    const Vec2 center{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f};
    v.pan = {canvasSize_.x / (2 * v.zoom) - center.x, canvasSize_.y / (2 * v.zoom) - center.y};
}

// ============================================================================
// Main frame
// ============================================================================

void StudioEditor::draw(Automation& a) {
    automation_ = &a;
    syncPendingPick();
    drainRunnerEvents();
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) &&
        !ImGui::GetIO().WantTextInput && save)
        save();
    dark_ = ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x < 0.4f;
    m_ = studio_ui::Metrics::make(std::max(1.0f, ImGui::GetFontSize() / 15.0f));
    pal_ = studio_ui::Palette::current(dark_);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {m_.spSM, m_.spSM});
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    drawToolbar(avail.x);

    const float statusH = m_.statusBarH;
    const float bodyH = std::max(80.0f, avail.y - m_.toolbarH - statusH);
    const float libW = libOpen_ ? std::min(libW_ * m_.scale, avail.x * 0.30f) : 0.0f;
    const float inspW = inspOpen_ ? std::min(inspW_ * m_.scale, avail.x * 0.36f) : 0.0f;
    const float canvasW = std::max(120.0f, avail.x - libW - inspW);

    if (libOpen_) {
        drawLibrary(libW, bodyH);
        // Thin drag splitter between the library and the canvas.
        ImGui::InvisibleButton("##lib_split", {5.0f * m_.scale, bodyH});
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive())
            libW_ = std::clamp(libW_ + ImGui::GetIO().MouseDelta.x / m_.scale, 150.0f, 480.0f);
        ImGui::SameLine(0, 0);
    }

    // Center column: canvas + optional console below.
    ImGui::BeginChild("##studio_center", {canvasW, bodyH}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float consoleH = (consolePinned_ || consoleH_ > 0.5f) ? consoleH_ : 0.0f;
    const float cvH = std::max(60.0f, ImGui::GetContentRegionAvail().y - consoleH);
    if (graph()) {
        drawCanvasArea();
        ImGui::Dummy({0, 0});
        if (consoleH > 0.5f) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
            drawConsole(canvasW, consoleH);
        }
        (void)cvH;
    } else {
        drawEmptyState(canvasW, bodyH);
    }
    ImGui::EndChild();

    if (inspOpen_) {
        ImGui::SameLine(0, 0);
        // Thin drag splitter between the canvas and the inspector.
        ImGui::InvisibleButton("##insp_split", {5.0f * m_.scale, bodyH});
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive())
            inspW_ = std::clamp(inspW_ - ImGui::GetIO().MouseDelta.x / m_.scale, 220.0f, 560.0f);
        ImGui::SameLine(0, 0);
        drawInspector(inspW, bodyH);
    }

    drawStatusBar(avail.x);
    drawQuickCreate();
    drawContextMenus();
    drawRenameBox();

    // First-run help: explains every canvas interaction in the active language.
    if (ImGui::BeginPopupModal("##studio_help", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal_.accentCyan), "%s", TL("How to use the Studio"));
        ImGui::Separator();
        ImGui::BulletText("%s", TL("Add nodes: drag from the left library, double-click a library item, or right-click the canvas -> Add node."));
        ImGui::BulletText("%s", TL("Connect: drag from a round/arrow port to a compatible port. Drag a wire to empty space to open quick-create."));
        ImGui::BulletText("%s", TL("Pan: middle-mouse drag or Space + left drag. Zoom: mouse wheel (centers on the cursor)."));
        ImGui::BulletText("%s", TL("Select: click, drag a box, or Ctrl+A for all. Move: drag. Copy: Ctrl+C / Ctrl+V, duplicate: Ctrl+D."));
        ImGui::BulletText("%s", TL("Edit properties in the right inspector. Use the pick buttons to capture points, colors and regions straight from the phone."));
        ImGui::BulletText("%s", TL("Run: the play button executes the graph; the bug button runs with breakpoints. Live values appear on wires while debugging."));
        ImGui::BulletText("%s", TL("Delete: select items and press Delete. Undo/Redo: Ctrl+Z / Ctrl+Y."));
        ImGui::Spacing();
        if (ImGui::Button(TL("Got it"), {140 * m_.scale, 0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

// ============================================================================
// Canvas
// ============================================================================

void StudioEditor::drawCanvasArea() {
    auto& g = *graph();
    auto& v = g.view;
    ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
                        {ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x,
                         ImGui::GetCursorScreenPos().y + ImGui::GetContentRegionAvail().y - consoleH_},
                        false);
    const ImVec2 cpos = ImGui::GetCursorScreenPos();
    const ImVec2 csize{ImGui::GetContentRegionAvail().x,
                       std::max(60.0f, ImGui::GetContentRegionAvail().y - ((consolePinned_ || consoleH_ > 0.5f) ? consoleH_ : 0.0f))};
    canvasOrigin_ = cpos;
    canvasSize_ = csize;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cpos, {cpos.x + csize.x, cpos.y + csize.y}, pal_.canvasBg);

    // Invisible input surface owning mouse interaction inside the canvas rect.
    ImGui::SetCursorScreenPos(cpos);
    ImGui::InvisibleButton("##studio_canvas_hit", csize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);

    if (v.showGrid) drawGrid(dl);
    drawGroupsLayer(dl);
    drawWiresLayer(dl);
    drawNodesLayer(dl);
    drawNotesLayer(dl);
    if (drag_ == Drag::Wire) drawWireDrag(dl);
    if (drag_ == Drag::Marquee) drawMarquee(dl);
    if (v.showMinimap) drawMinimap();

    // Node library drag&drop target.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("NEXPLAY_STUDIO_NODE")) {
            int type = *static_cast<const int*>(pl->Data);
            addNodeAt(static_cast<studio::NodeType>(type), toCanvas(ImGui::GetIO().MousePos));
        }
        ImGui::EndDragDropTarget();
    }

    canvasInput();
    shortcuts();
    ImGui::PopClipRect();
}

void StudioEditor::drawGrid(ImDrawList* dl) {
    const auto& v = view();
    const float stepPx = v.gridSize * v.zoom;
    if (stepPx < 5.0f) {
        // Zoomed far out: draw only major lines.
    }
    const Vec2 lo = toCanvas(canvasOrigin_);
    const Vec2 hi = toCanvas({canvasOrigin_.x + canvasSize_.x, canvasOrigin_.y + canvasSize_.y});
    const float minor = v.gridSize;
    const auto drawLines = [&](float step, ImU32 col) {
        if (step * v.zoom < 4.0f) return;
        for (float x = std::floor(lo.x / step) * step; x <= hi.x; x += step) {
            const float sx = toScreen({x, 0}).x;
            dl->AddLine({sx, canvasOrigin_.y}, {sx, canvasOrigin_.y + canvasSize_.y}, col, 1.0f);
        }
        for (float y = std::floor(lo.y / step) * step; y <= hi.y; y += step) {
            const float sy = toScreen({0, y}).y;
            dl->AddLine({canvasOrigin_.x, sy}, {canvasOrigin_.x + canvasSize_.x, sy}, col, 1.0f);
        }
    };
    if (stepPx >= 5.0f) drawLines(minor, pal_.gridMinor);
    drawLines(minor * 4.0f, pal_.gridMajor);
}

void StudioEditor::drawGroupsLayer(ImDrawList* dl) {
    const auto& g = *graph();
    for (const auto& gr : g.groups) {
        const Rect& r = gr.rect;
        if (gr.collapsed) {
            const ImVec2 a = toScreen({r.x, r.y});
            const float w = std::max(r.w, 190.0f) * view().zoom;
            const float h = 30.0f * view().zoom;
            dl->AddRectFilled(a, {a.x + w, a.y + h}, studio_ui::withAlpha(gr.colorRgba, 0.22f), m_.radiusMD * view().zoom);
            dl->AddRect(a, {a.x + w, a.y + h}, gr.colorRgba, m_.radiusMD * view().zoom, 0, 1.2f);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * view().zoom * 0.9f,
                        {a.x + 10 * view().zoom, a.y + (h - ImGui::GetFontSize() * view().zoom) * 0.5f},
                        pal_.textPrimary, VL(i18n::trf("%s  ▸ %d", gr.title.c_str(), static_cast<int>(gr.nodeIds.size()))));
            continue;
        }
        const ImVec2 a = toScreen({r.x, r.y}), b = toScreen({r.x + r.w, r.y + r.h});
        const Vec2 lo = toCanvas(canvasOrigin_), hi = toCanvas({canvasOrigin_.x + canvasSize_.x, canvasOrigin_.y + canvasSize_.y});
        if (!rectIntersects(r, {lo.x, lo.y, hi.x - lo.x, hi.y - lo.y})) continue;
        const ImU32 col = gr.colorRgba;
        dl->AddRectFilled(a, b, studio_ui::withAlpha(col, 0.08f), m_.radiusLG * view().zoom);
        dl->AddRect(a, b, selGroup_ == gr.id ? pal_.accentPrimary : studio_ui::withAlpha(col, 0.85f),
                    m_.radiusLG * view().zoom, 0, selGroup_ == gr.id ? 2.0f : 1.2f);
        const float th = 26.0f * view().zoom;
        dl->AddRectFilled(a, {b.x, a.y + th}, studio_ui::withAlpha(col, 0.30f), m_.radiusLG * view().zoom,
                          ImDrawFlags_RoundCornersTop);
        const float fs = ImGui::GetFontSize() * view().zoom * 0.92f;
        dl->AddText(ImGui::GetFont(), fs, {a.x + 10 * view().zoom, a.y + (th - fs) * 0.5f},
                    pal_.textPrimary, VL(gr.title));
        // resize corner marks
        const float grab = 8.0f * view().zoom;
        dl->AddRectFilled({b.x - grab, b.y - grab}, b, studio_ui::withAlpha(col, 0.9f), 3.0f);
    }
}

void StudioEditor::wireBezier(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thick, bool arrow) {
    const auto& v = view();
    switch (v.wireStyle) {
        case studio::WireStyle::Straight:
            dl->AddLine(a, b, col, thick);
            if (arrow) arrowHead(dl, {(a.x + b.x) * 0.5f + 4, (a.y + b.y) * 0.5f},
                                 {b.x - a.x, b.y - a.y}, 7.0f * v.zoom, col);
            break;
        case studio::WireStyle::Rounded: {
            const float mx = (a.x + b.x) * 0.5f;
            ImVec2 pts[5] = {a, {mx, a.y}, {mx, b.y}, b, b};
            int n = (std::fabs(a.y - b.y) < 2.0f) ? 2 : 4;
            if (n == 2) { pts[1] = b; }
            dl->AddPolyline(pts, n, col, ImDrawFlags_None, thick);
            if (arrow) arrowHead(dl, {mx + 2, (a.y + b.y) * 0.5f}, {0, b.y > a.y ? 1.0f : -1.0f}, 7.0f * v.zoom, col);
            break;
        }
        case studio::WireStyle::Curved:
        default: {
            const float dx = std::max(30.0f * v.zoom, std::fabs(b.x - a.x) * 0.5f);
            dl->AddBezierCubic(a, {a.x + dx, a.y}, {b.x - dx, b.y}, b, col, thick);
            if (arrow) {
                // midpoint of the curve, tangent approx along +x
                ImVec2 mid{(a.x + b.x) * 0.5f + 4, 0};
                const float t = 0.5f, u = 0.5f;
                mid.y = u * u * u * a.y + 3 * u * u * t * a.y + 3 * u * t * t * b.y + t * t * t * b.y;
                arrowHead(dl, mid, {b.x - a.x, b.y - a.y}, 7.0f * v.zoom, col);
            }
            break;
        }
    }
}

void StudioEditor::drawWiresLayer(ImDrawList* dl) {
    const auto& g = *graph();
    const auto hidden = hiddenNodes(g);
    const Vec2 lo = toCanvas(canvasOrigin_);
    const Vec2 hi = toCanvas({canvasOrigin_.x + canvasSize_.x, canvasOrigin_.y + canvasSize_.y});
    const Rect vis{lo.x - 40, lo.y - 40, hi.x - lo.x + 80, hi.y - lo.y + 80};

    for (const auto& c : g.connections) {
        const studio::GraphNode *na, *nb;
        const studio::Port *pa, *pb;
        if (!g.findPortOwner(c.fromPort, na, pa) || !g.findPortOwner(c.toPort, nb, pb)) continue;
        if (hidden.count(na->id) || hidden.count(nb->id)) continue;
        const Vec2 ap = portPos(*na, *pa), bp = portPos(*nb, *pb);
        const Rect bb{std::min(ap.x, bp.x), std::min(ap.y, bp.y), std::fabs(ap.x - bp.x), std::fabs(ap.y - bp.y)};
        if (!rectIntersects(bb, vis) && !vis.contains(ap) && !vis.contains(bp)) continue;
        const ImVec2 a = toScreen(ap), b = toScreen(bp);
        const bool exec = c.kind == studio::PortKind::Execution;
        ImU32 col = exec ? studio_ui::portColor(*pa, dark_) : studio_ui::portColor(*pa, dark_);
        const bool hov = hovConn_ == c.id, sel = selConn_ == c.id;
        if (sel) col = pal_.accentPrimary;
        else if (hov) col = mix(col, IM_COL32_WHITE, 0.35f);
        const float thick = (exec ? 2.6f : 2.0f) * view().zoom * (sel ? 1.25f : 1.0f);
        if (sel) wireBezier(dl, a, b, studio_ui::withAlpha(pal_.accentPrimary, 0.25f), thick + 5.0f, false);
        wireBezier(dl, a, b, col, thick, exec);
        if (runtime.wireValue) {
            if (std::string txt = runtime.wireValue(c.id); !txt.empty()) {
                const ImVec2 mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f - 10 * view().zoom};
                const float fs = ImGui::GetFontSize() * view().zoom * 0.75f;
                dl->AddText(ImGui::GetFont(), fs, mid, pal_.textSecondary, VL(txt));
            }
        }
    }
}

void StudioEditor::drawNodesLayer(ImDrawList* dl) {
    auto& g = *graph();
    const auto hidden = hiddenNodes(g);
    const Vec2 lo = toCanvas(canvasOrigin_);
    const Vec2 hi = toCanvas({canvasOrigin_.x + canvasSize_.x, canvasOrigin_.y + canvasSize_.y});
    const Rect vis{lo.x - 30, lo.y - 30, hi.x - lo.x + 60, hi.y - lo.y + 60};
    const float z = view().zoom;
    const float fs = ImGui::GetFontSize() * z;
    const float fsSmall = fs * 0.82f;
    ImFont* font = ImGui::GetFont();

    for (auto& n : g.nodes) {
        if (hidden.count(n.id)) continue;
        const ImVec2 sz = nodeSize(n);
        const Rect nr{n.pos.x, n.pos.y, sz.x, sz.y};
        if (!rectIntersects(nr, vis)) continue;

        const ImVec2 a = toScreen({nr.x, nr.y});
        const ImVec2 b = toScreen({nr.x + nr.w, nr.y + nr.h});
        const ImU32 accent = nodeAccent(n);
        const bool sel = isSel(n.id), hov = hovNode_ == n.id;

        studio_ui::NodeRunBadge badge = studio_ui::NodeRunBadge::None;
        if (runtime.nodeBadge) badge = runtime.nodeBadge(g.id, n.id);

        // shadow + body
        dl->AddRectFilled({a.x + 3 * z, a.y + 4 * z}, {b.x + 3 * z, b.y + 4 * z}, pal_.shadowSoft,
                          m_.radiusMD * z);
        ImU32 bodyCol = n.enabled ? pal_.nodeBody : pal_.nodeBodyMuted;
        dl->AddRectFilled(a, b, bodyCol, m_.radiusMD * z);
        ImU32 border = sel ? pal_.accentPrimary : hov ? mix(pal_.nodeBorder, IM_COL32_WHITE, 0.35f) : pal_.nodeBorder;
        float borderT = sel ? 2.0f : 1.0f;
        if (badge == studio_ui::NodeRunBadge::Executing) {
            const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
            dl->AddRect(a, b, studio_ui::withAlpha(pal_.accentCyan, 0.45f + 0.5f * pulse), m_.radiusMD * z, 0,
                        2.4f);
        } else if (badge == studio_ui::NodeRunBadge::Error) {
            dl->AddRect(a, b, pal_.danger, m_.radiusMD * z, 0, 2.4f);
        }
        dl->AddRect(a, b, border, m_.radiusMD * z, 0, borderT);

        // header band tinted by category accent
        const float hh = m_.nodeHeaderH * z;
        dl->AddRectFilled(a, {b.x, a.y + hh}, studio_ui::withAlpha(accent, n.enabled ? 0.20f : 0.10f),
                          m_.radiusMD * z, ImDrawFlags_RoundCornersTop);
        dl->AddRectFilled({a.x, a.y}, {a.x + 4 * z, a.y + hh}, accent, m_.radiusMD * z,
                          ImDrawFlags_RoundCornersTopLeft);

        // breakpoint dot
        float tx = a.x + 10 * z;
        if (n.breakpoint) {
            dl->AddCircleFilled({tx + 5 * z, a.y + hh * 0.5f}, 5 * z, pal_.danger);
            tx += 14 * z;
        }
        // status chips
        if (badge == studio_ui::NodeRunBadge::Completed) {
            dl->AddCircleFilled({b.x - 12 * z, a.y + hh * 0.5f}, 5 * z, pal_.success);
        } else if (badge == studio_ui::NodeRunBadge::Warning) {
            dl->AddCircleFilled({b.x - 12 * z, a.y + hh * 0.5f}, 5 * z, pal_.warning);
        }
        // title
        ImU32 titleCol = n.enabled ? pal_.textPrimary : pal_.textDim;
        const std::string title = nodeTitle(n);
        dl->AddText(font, fs, {tx, a.y + (hh - fs) * 0.5f}, titleCol, VL(i18n::tr(title.c_str())));
        if (n.collapsed) continue;
        if (!n.enabled)
            dl->AddLine({a.x + 6 * z, a.y + hh * 0.5f}, {b.x - 6 * z, a.y + hh * 0.5f},
                        studio_ui::withAlpha(pal_.danger, 0.7f), 1.2f);

        // port rows
        const size_t rows = std::max(n.inputs.size(), n.outputs.size());
        for (size_t i = 0; i < rows; ++i) {
            const float ry = n.pos.y + m_.nodeHeaderH + i * m_.nodeRowH + m_.nodeRowH * 0.5f;
            const float sy = toScreen({0, ry}).y;
            if (i < n.inputs.size()) {
                const auto& p = n.inputs[i];
                const bool exec = p.kind == studio::PortKind::Execution;
                const ImVec2 pc = {a.x, sy};
                const ImU32 pc8 = studio_ui::portColor(p, dark_);
                const bool hot = (hovPort_ == p.id) || (drag_ == Drag::Wire && wireCheckValid_ && hovPort_ == p.id);
                const float pr = (exec ? 5.5f : 4.6f) * z * (hot ? 1.35f : 1.0f);
                if (exec) execPortGlyph(dl, pc, pr, pc8, true);
                else {
                    dl->AddCircleFilled(pc, pr, studio_ui::withAlpha(pc8, 0.25f));
                    dl->AddCircle(pc, pr, pc8, 0, 1.4f);
                    bool wired = false;
                    for (const auto& c : g.connections) if (c.toPort == p.id) { wired = true; break; }
                    if (wired) dl->AddCircleFilled(pc, pr * 0.45f, pc8);
                }
                const char* lbl = VL(i18n::tr(p.name.c_str()));
                dl->AddText(font, fsSmall, {pc.x + 12 * z, sy - fsSmall * 0.55f},
                            exec ? studio_ui::withAlpha(pal_.textPrimary, 0.85f) : pal_.textSecondary, lbl);
            }
            if (i < n.outputs.size()) {
                const auto& p = n.outputs[i];
                const bool exec = p.kind == studio::PortKind::Execution;
                const ImVec2 pc = {b.x, sy};
                const ImU32 pc8 = studio_ui::portColor(p, dark_);
                const bool hot = hovPort_ == p.id;
                const float pr = (exec ? 5.5f : 4.6f) * z * (hot ? 1.35f : 1.0f);
                if (exec) execPortGlyph(dl, pc, pr, pc8, true);
                else {
                    dl->AddCircleFilled(pc, pr, studio_ui::withAlpha(pc8, 0.25f));
                    dl->AddCircle(pc, pr, pc8, 0, 1.4f);
                    bool wired = false;
                    for (const auto& c : g.connections) if (c.fromPort == p.id) { wired = true; break; }
                    if (wired) dl->AddCircleFilled(pc, pr * 0.45f, pc8);
                }
                const char* lbl = VL(i18n::tr(p.name.c_str()));
                const float tw = ImGui::CalcTextSize(lbl).x * (fsSmall / ImGui::GetFontSize());
                dl->AddText(font, fsSmall, {pc.x - 12 * z - tw, sy - fsSmall * 0.55f},
                            exec ? studio_ui::withAlpha(pal_.textPrimary, 0.85f) : pal_.textSecondary, lbl);
            }
        }

        // summary line
        if (!n.collapsed) {
            const std::string sum = nodeSummary(n);
            if (!sum.empty()) {
                const float sy = b.y - (m_.spSM * z) - fsSmall * 0.9f;
                dl->AddText(font, fsSmall, {a.x + 12 * z, sy - fsSmall * 0.4f}, pal_.textDim,
                            VL(sum));
            }
        }
        // comment badge
        if (!n.comment.empty()) {
            dl->AddCircleFilled({b.x - 8 * z, a.y - 2 * z}, 4 * z, pal_.warning);
        }
    }
}

void StudioEditor::drawNotesLayer(ImDrawList* dl) {
    const auto& g = *graph();
    const Vec2 lo = toCanvas(canvasOrigin_);
    const Vec2 hi = toCanvas({canvasOrigin_.x + canvasSize_.x, canvasOrigin_.y + canvasSize_.y});
    const Rect vis{lo.x - 30, lo.y - 30, hi.x - lo.x + 60, hi.y - lo.y + 60};
    const float z = view().zoom;
    ImFont* font = ImGui::GetFont();
    for (const auto& nt : g.notes) {
        const Rect nr{nt.pos.x, nt.pos.y, nt.size.x, nt.size.y};
        if (!rectIntersects(nr, vis)) continue;
        const ImVec2 a = toScreen({nr.x, nr.y}), b = toScreen({nr.x + nr.w, nr.y + nr.h});
        const ImU32 col = nt.colorRgba;
        dl->AddRectFilled({a.x + 2 * z, a.y + 3 * z}, {b.x + 2 * z, b.y + 3 * z}, pal_.shadowSoft,
                          m_.radiusSM * z);
        dl->AddRectFilled(a, b, studio_ui::withAlpha(col, dark_ ? 0.30f : 0.45f), m_.radiusSM * z);
        dl->AddRect(a, b, selNotes_.count(nt.id) ? pal_.accentPrimary : studio_ui::withAlpha(col, 0.9f),
                    m_.radiusSM * z, 0, selNotes_.count(nt.id) ? 2.0f : 1.0f);
        const float fs = ImGui::GetFontSize() * z * 0.9f;
        const float fsT = ImGui::GetFontSize() * z;
        if (!nt.title.empty())
            dl->AddText(font, fsT, {a.x + 10 * z, a.y + 8 * z}, pal_.textPrimary, VL(nt.title));
        if (!nt.text.empty()) {
            // crude wrap: draw up to 4 lines clipped by the drawlist clip rect
            std::string wrapped = nt.text;
            const ImVec4 clip{a.x, a.y, b.x - 6 * z, b.y - 6 * z};
            dl->AddText(font, fs, {a.x + 10 * z, a.y + 8 * z + fsT * 1.3f},
                        studio_ui::withAlpha(pal_.textPrimary, 0.85f), VL(wrapped), nullptr, 0.0f, &clip);
        }
        // resize grip
        dl->AddTriangleFilled({b.x - 12 * z, b.y}, {b.x, b.y - 12 * z}, b,
                              studio_ui::withAlpha(col, 0.9f));
    }
}

void StudioEditor::drawWireDrag(ImDrawList* dl) {
    const auto* g = graph();
    if (!g) return;
    const studio::GraphNode* n = g->findNode(wireSrcNode_);
    if (!n) return;
    const studio::Port* p = n->findPort(wireSrcPort_);
    if (!p) return;
    const ImVec2 a = toScreen(portPos(*n, *p));
    const ImVec2 b = ImGui::GetIO().MousePos;
    const bool out = p->dir == studio::PortDir::Output;
    ImU32 col = studio_ui::portColor(*p, dark_);
    bool valid = true;
    if (hovPort_ && hovPort_ != wireSrcPort_) {
        const auto chk = g->canConnect(wireSrcNode_, wireSrcPort_, hovNode_, hovPort_);
        valid = chk.ok;
        col = chk.ok ? pal_.success : pal_.danger;
    }
    wireBezier(dl, out ? a : b, out ? b : a, col, 2.2f * view().zoom, p->kind == studio::PortKind::Execution);
    // rejection reason badge
    if (hovPort_ && hovPort_ != wireSrcPort_ && !valid) {
        const auto chk = g->canConnect(wireSrcNode_, wireSrcPort_, hovNode_, hovPort_);
        const ImVec2 tp{b.x + 14, b.y - 22};
        // Reasons with a dynamic suffix can't hit the translation map verbatim —
        // translate the stable prefix instead.
        std::string reason;
        if (chk.reason.rfind("Incompatible data types", 0) == 0)
            reason = std::string(i18n::tr("Incompatible data types")) + chk.reason.substr(22);
        else
            reason = i18n::tr(chk.reason.c_str());
        const ImVec2 ts = ImGui::CalcTextSize(VL(reason));
        dl->AddRectFilled({tp.x - 6, tp.y - 4}, {tp.x + ts.x + 6, tp.y + ts.y + 4}, pal_.danger, 4.0f);
        dl->AddText(tp, IM_COL32_WHITE, VL(reason));
    }
}

void StudioEditor::drawMarquee(ImDrawList* dl) {
    const ImVec2 a{std::min(marqueeA_.x, marqueeB_.x), std::min(marqueeA_.y, marqueeB_.y)};
    const ImVec2 b{std::max(marqueeA_.x, marqueeB_.x), std::max(marqueeA_.y, marqueeB_.y)};
    dl->AddRectFilled(a, b, studio_ui::withAlpha(pal_.accentPrimary, 0.10f));
    dl->AddRect(a, b, pal_.accentPrimary, 0, 0, 1.0f);
}

void StudioEditor::drawMinimap() {
    const auto* g = graph();
    if (!g) return;
    const float mw = 170.0f * m_.scale, mh = 110.0f * m_.scale;
    const ImVec2 mp{canvasOrigin_.x + canvasSize_.x - mw - 12.0f,
                    canvasOrigin_.y + canvasSize_.y - mh - 12.0f};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Vec2 lo{FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX};
    for (const auto& n : g->nodes) {
        const ImVec2 sz = nodeSize(n);
        lo.x = std::min(lo.x, n.pos.x); lo.y = std::min(lo.y, n.pos.y);
        hi.x = std::max(hi.x, n.pos.x + sz.x); hi.y = std::max(hi.y, n.pos.y + sz.y);
    }
    if (hi.x <= lo.x) { lo = {-200, -150}; hi = {600, 400}; }
    const float pad = 80.0f;
    lo.x -= pad; lo.y -= pad; hi.x += pad; hi.y += pad;
    const float sc = std::min(mw / (hi.x - lo.x), mh / (hi.y - lo.y));
    const auto mm = [&](Vec2 c) {
        return ImVec2{mp.x + (c.x - lo.x) * sc + (mw - (hi.x - lo.x) * sc) * 0.5f,
                      mp.y + (c.y - lo.y) * sc + (mh - (hi.y - lo.y) * sc) * 0.5f};
    };
    dl->AddRectFilled(mp, {mp.x + mw, mp.y + mh}, studio_ui::withAlpha(pal_.panelBg, 0.85f), 6.0f);
    dl->AddRect(mp, {mp.x + mw, mp.y + mh}, pal_.divider, 6.0f);
    for (const auto& n : g->nodes) {
        const ImVec2 sz = nodeSize(n);
        const ImVec2 a = mm(n.pos), b = mm({n.pos.x + sz.x, n.pos.y + sz.y});
        dl->AddRectFilled(a, {std::max(b.x, a.x + 2), std::max(b.y, a.y + 2)},
                          isSel(n.id) ? pal_.accentPrimary : studio_ui::withAlpha(nodeAccent(n), 0.8f), 1.0f);
    }
    const Vec2 vlo = toCanvas(canvasOrigin_), vhi = toCanvas({canvasOrigin_.x + canvasSize_.x, canvasOrigin_.y + canvasSize_.y});
    const ImVec2 va = mm(vlo), vb = mm(vhi);
    dl->AddRect(va, vb, studio_ui::withAlpha(IM_COL32_WHITE, 0.75f), 2.0f, 0, 1.2f);

    ImGui::SetCursorScreenPos(mp);
    ImGui::InvisibleButton("##studio_minimap", {mw, mh});
    if (ImGui::IsItemActivated()) minimapDrag_ = true;
    if (minimapDrag_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 mpos = ImGui::GetIO().MousePos;
        const Vec2 cc{lo.x + (mpos.x - mp.x - (mw - (hi.x - lo.x) * sc) * 0.5f) / sc,
                      lo.y + (mpos.y - mp.y - (mh - (hi.y - lo.y) * sc) * 0.5f) / sc};
        auto& v = view();
        v.pan = {canvasSize_.x / (2 * v.zoom) - cc.x, canvasSize_.y / (2 * v.zoom) - cc.y};
    }
    if (minimapDrag_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) minimapDrag_ = false;
}

// ============================================================================
// Input
// ============================================================================

void StudioEditor::canvasInput() {
    auto* g = graph();
    if (!g) return;
    auto& v = g->view;
    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mpos = io.MousePos;
    const Vec2 mc = toCanvas(mpos);

    // refresh hover targets
    hovNode_ = hovPort_ = hovConn_ = hovNote_ = hovGroup_ = 0;
    if (hovered) {
        uint32_t pn = 0, pp = 0;
        if (portAt(mc, pn, pp)) { hovNode_ = pn; hovPort_ = pp; }
        else if ((hovNode_ = nodeAt(mc)) != 0) {}
        else if ((hovConn_ = connAt(mc)) != 0) {}
        else if ((hovNote_ = noteAt(mc)) != 0) {}
        else {
            int corner = -1;
            hovGroup_ = groupAt(mc, corner);
        }
    }
    wireCheckValid_ = hovPort_ != 0;

    // zoom on wheel, centered on the cursor
    if (hovered && io.MouseWheel != 0.0f) {
        const Vec2 before = toCanvas(mpos);
        const float nz = std::clamp(v.zoom * std::pow(1.12f, io.MouseWheel), 0.2f, 2.5f);
        v.pan = {(mpos.x - canvasOrigin_.x) / nz - before.x, (mpos.y - canvasOrigin_.y) / nz - before.y};
        v.zoom = nz;
    }

    // ---- begin drags ----
    if (drag_ == Drag::None) {
        const bool space = ImGui::IsKeyDown(ImGuiKey_Space);
        if (ImGui::IsItemActivated()) {
            dragMouse0_ = mpos;
            if (io.MouseClicked[ImGuiMouseButton_Middle] || (io.MouseClicked[0] && space)) {
                drag_ = Drag::Pan;
                panAtDrag_ = v.pan;
            } else if (io.MouseClicked[0]) {
                if (hovPort_) {
                    beginWire(hovNode_, hovPort_);
                } else if (hovNode_) {
                    if (!isSel(hovNode_)) {
                        if (!io.KeyShift) clearSel();
                        sel_.insert(hovNode_);
                        selConn_ = 0;
                    }
                    // gather move offsets for the whole selection
                    moveOffsets_.clear();
                    moveNoteOffsets_.clear();
                    for (uint32_t id : sel_)
                        if (const auto* n = g->findNode(id)) moveOffsets_.emplace_back(id, Vec2{n->pos.x - mc.x, n->pos.y - mc.y});
                    for (uint32_t id : selNotes_)
                        for (const auto& nt : g->notes)
                            if (nt.id == id) moveNoteOffsets_.emplace_back(id, Vec2{nt.pos.x - mc.x, nt.pos.y - mc.y});
                    drag_ = Drag::Move;
                    moved_ = false;
                } else if (hovConn_) {
                    selConn_ = hovConn_;
                    if (!io.KeyShift) sel_.clear(), selNotes_.clear();
                } else if (hovNote_) {
                    if (!io.KeyShift) clearSel();
                    selNotes_.insert(hovNote_);
                    const auto ntIt = std::find_if(g->notes.begin(), g->notes.end(),
                                                   [&](const studio::GraphNote& x) { return x.id == hovNote_; });
                    if (ntIt != g->notes.end()) {
                        const float gx = ntIt->pos.x + ntIt->size.x - 14.0f / v.zoom;
                        const float gy = ntIt->pos.y + ntIt->size.y - 14.0f / v.zoom;
                        if (mc.x > gx && mc.y > gy) {
                            drag_ = Drag::NoteResize;
                            dragNote_ = hovNote_;
                            noteSize0_ = ntIt->size;
                        } else {
                            moveNoteOffsets_.clear();
                            moveNoteOffsets_.emplace_back(ntIt->id, Vec2{ntIt->pos.x - mc.x, ntIt->pos.y - mc.y});
                            moveOffsets_.clear();
                            drag_ = Drag::Move;
                            moved_ = false;
                        }
                    }
                } else if (hovGroup_) {
                    int corner = -1;
                    groupAt(mc, corner);
                    selGroup_ = hovGroup_;
                    if (!io.KeyShift) sel_.clear(), selNotes_.clear(), selConn_ = 0;
                    auto grIt = std::find_if(g->groups.begin(), g->groups.end(),
                                             [&](studio::GraphGroup& x) { return x.id == hovGroup_; });
                    if (grIt != g->groups.end()) {
                        auto& gr = *grIt;
                        if (corner >= 0) {
                            drag_ = Drag::GroupResize;
                            dragGroup_ = hovGroup_;
                            groupCorner_ = corner;
                            groupRect0_ = gr.rect;
                        } else {
                            drag_ = Drag::GroupMove;
                            dragGroup_ = hovGroup_;
                            groupRect0_ = gr.rect;
                            moveOffsets_.clear();
                            for (uint32_t id : gr.nodeIds)
                                if (const auto* n = g->findNode(id))
                                    moveOffsets_.emplace_back(id, Vec2{n->pos.x - mc.x, n->pos.y - mc.y});
                        }
                    }
                } else {
                    drag_ = Drag::Marquee;
                    marqueeA_ = marqueeB_ = mpos;
                    marqueeAdd_ = io.KeyShift;
                    if (!marqueeAdd_) clearSel();
                }
            }
        }
        // right click → context menu
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            ctxId_ = 0;
            if (hovNode_) { ctx_ = Ctx::Node; ctxId_ = hovNode_; if (!isSel(hovNode_)) { clearSel(); sel_.insert(hovNode_); } }
            else if (hovConn_) { ctx_ = Ctx::Wire; ctxId_ = hovConn_; selConn_ = hovConn_; }
            else if (hovNote_) { ctx_ = Ctx::Note; ctxId_ = hovNote_; }
            else if (hovGroup_) { ctx_ = Ctx::Group; ctxId_ = hovGroup_; }
            else ctx_ = Ctx::Canvas;
            ctxCanvas_ = mc;
            ImGui::OpenPopup("##studio_ctx");
        }
        // double click → rename
        if (hovered && io.MouseDoubleClicked[0]) {
            if (hovNode_) {
                if (const auto* n = g->findNode(hovNode_)) {
                    renameNode_ = hovNode_;
                    std::snprintf(renameBuf_, sizeof renameBuf_, "%s", nodeTitle(*n).c_str());
                    ImGui::OpenPopup("##studio_rename");
                }
            } else if (hovNote_) {
                renameNote_ = hovNote_;
                renameBuf_[0] = '\0';
                ImGui::OpenPopup("##studio_rename");
            } else if (hovGroup_) {
                renameGroup_ = hovGroup_;
                renameBuf_[0] = '\0';
                ImGui::OpenPopup("##studio_rename");
            }
        }
        return;
    }

    // ---- continue drags ----
    switch (drag_) {
        case Drag::Pan:
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                (ImGui::IsMouseDown(0) && ImGui::IsKeyDown(ImGuiKey_Space))) {
                v.pan = {panAtDrag_.x + (mpos.x - dragMouse0_.x) / v.zoom,
                         panAtDrag_.y + (mpos.y - dragMouse0_.y) / v.zoom};
            } else drag_ = Drag::None;
            break;
        case Drag::Move:
            if (ImGui::IsMouseDown(0)) {
                const float dx = mpos.x - dragMouse0_.x, dy = mpos.y - dragMouse0_.y;
                if (!moved_ && dx * dx + dy * dy > 9.0f) moved_ = true;
                if (moved_) {
                    const bool snap = v.snapToGrid && !io.KeyAlt;
                    const float gs = v.gridSize;
                    for (auto& [id, off] : moveOffsets_)
                        if (auto* n = g->findNode(id)) {
                            Vec2 np{mc.x + off.x, mc.y + off.y};
                            if (snap) { np.x = std::round(np.x / gs) * gs; np.y = std::round(np.y / gs) * gs; }
                            n->pos = np;
                        }
                    for (auto& [id, off] : moveNoteOffsets_)
                        for (auto& nt : g->notes)
                            if (nt.id == id) {
                                Vec2 np{mc.x + off.x, mc.y + off.y};
                                if (snap) { np.x = std::round(np.x / gs) * gs; np.y = std::round(np.y / gs) * gs; }
                                nt.pos = np;
                            }
                }
            } else {
                if (moved_) commit();
                drag_ = Drag::None;
            }
            break;
        case Drag::Marquee:
            if (ImGui::IsMouseDown(0)) {
                marqueeB_ = mpos;
            } else {
                const Vec2 a = toCanvas({std::min(marqueeA_.x, marqueeB_.x), std::min(marqueeA_.y, marqueeB_.y)});
                const Vec2 b2 = toCanvas({std::max(marqueeA_.x, marqueeB_.x), std::max(marqueeA_.y, marqueeB_.y)});
                const Rect mr{a.x, a.y, b2.x - a.x, b2.y - a.y};
                for (const auto& n : g->nodes) {
                    const ImVec2 sz = nodeSize(n);
                    if (rectIntersects(mr, {n.pos.x, n.pos.y, sz.x, sz.y})) sel_.insert(n.id);
                }
                for (const auto& nt : g->notes)
                    if (rectIntersects(mr, {nt.pos.x, nt.pos.y, nt.size.x, nt.size.y})) selNotes_.insert(nt.id);
                drag_ = Drag::None;
            }
            break;
        case Drag::Wire:
            if (!ImGui::IsMouseDown(0)) dropWire();
            break;
        case Drag::GroupMove:
            if (ImGui::IsMouseDown(0)) {
                auto grIt = std::find_if(g->groups.begin(), g->groups.end(),
                                         [&](studio::GraphGroup& x) { return x.id == dragGroup_; });
                if (grIt != g->groups.end()) {
                    const Vec2 grabC = toCanvas(dragMouse0_);
                    auto& gr = *grIt;
                    gr.rect.x = groupRect0_.x + (mc.x - grabC.x);
                    gr.rect.y = groupRect0_.y + (mc.y - grabC.y);
                    const float ddx = gr.rect.x - groupRect0_.x, ddy = gr.rect.y - groupRect0_.y;
                    for (auto& [id, off] : moveOffsets_)
                        if (auto* n = g->findNode(id)) {
                            n->pos.x = grabC.x + off.x + ddx;
                            n->pos.y = grabC.y + off.y + ddy;
                        }
                    moved_ = true;
                }
            } else {
                if (moved_) { moved_ = false; commit(); }
                drag_ = Drag::None;
            }
            break;
        case Drag::GroupResize:
            if (ImGui::IsMouseDown(0)) {
                auto grIt = std::find_if(g->groups.begin(), g->groups.end(),
                                         [&](studio::GraphGroup& x) { return x.id == dragGroup_; });
                if (grIt != g->groups.end()) {
                    auto& gr = *grIt;
                    Rect r = groupRect0_;
                    const Vec2 d{mc.x - toCanvas(dragMouse0_).x, mc.y - toCanvas(dragMouse0_).y};
                    if (groupCorner_ == 0) { r.x += d.x; r.y += d.y; r.w -= d.x; r.h -= d.y; }
                    if (groupCorner_ == 1) { r.y += d.y; r.w += d.x; r.h -= d.y; }
                    if (groupCorner_ == 2) { r.x += d.x; r.w -= d.x; r.h += d.y; }
                    if (groupCorner_ == 3) { r.w += d.x; r.h += d.y; }
                    if (r.w > 60 && r.h > 50) gr.rect = r;
                    moved_ = true;
                }
            } else {
                if (moved_) { moved_ = false; commit(); }
                drag_ = Drag::None;
            }
            break;
        case Drag::NoteResize:
            if (ImGui::IsMouseDown(0)) {
                for (auto& nt : g->notes)
                    if (nt.id == dragNote_) {
                        const Vec2 d{mc.x - toCanvas(dragMouse0_).x, mc.y - toCanvas(dragMouse0_).y};
                        nt.size.x = std::max(120.0f, noteSize0_.x + d.x);
                        nt.size.y = std::max(60.0f, noteSize0_.y + d.y);
                        moved_ = true;
                    }
            } else {
                if (moved_) { moved_ = false; commit(); }
                drag_ = Drag::None;
            }
            break;
        default:
            drag_ = Drag::None;
            break;
    }
}

void StudioEditor::shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || io.WantTextInput) return;
    const bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? redoHistory() : undoHistory();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redoHistory();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) copySelection(false);
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) copySelection(true);
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) pasteAt(toCanvas(io.MousePos));
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateSelection();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) selectAll();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_G, false)) groupSelection();
    else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
        deleteSelection();
    else if (ImGui::IsKeyPressed(ImGuiKey_F, false)) fitView();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_0, false)) view().zoom = 1.0f;
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup))
        clearSel();
}

// ============================================================================
// Quick-create popup (drop a wire on empty canvas)
// ============================================================================

void StudioEditor::drawQuickCreate() {
    if (!quickMenu_) return;
    auto* g = graph();
    ImGui::SetNextWindowPos({ImGui::GetIO().MousePos.x - 12, ImGui::GetIO().MousePos.y - 8},
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize({300 * m_.scale, 340 * m_.scale}, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##studio_quick_create", ImGuiWindowFlags_NoMove)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##qf", TL("Search nodes..."), quickFilter_, sizeof quickFilter_);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        ImGui::Separator();

        // Candidate filter: when dragging a wire, require a compatible opposite port.
        const studio::Port* srcPort = nullptr;
        if (g && wireSrcNode_) {
            if (const auto* n = g->findNode(wireSrcNode_)) srcPort = n->findPort(wireSrcPort_);
        }
        auto compatible = [&](const studio::NodeTypeSpec& spec) {
            if (!srcPort) return true;
            const studio::PortDir want = srcPort->dir == studio::PortDir::Output ? studio::PortDir::Input
                                                                               : studio::PortDir::Output;
            for (const auto& ps : spec.ports) {
                if (ps.dir != want || ps.kind != srcPort->kind) continue;
                if (ps.kind == studio::PortKind::Execution) return true;
                if (srcPort->dir == studio::PortDir::Output
                        ? studio::areDataTypesCompatible(srcPort->dataType, ps.dataType)
                        : studio::areDataTypesCompatible(ps.dataType, srcPort->dataType))
                    return true;
            }
            return false;
        };

        for (const auto& spec : studio::allNodeSpecs()) {
            if (quickFilter_[0] && spec.displayName.find(quickFilter_) == std::string::npos &&
                spec.typeKey.find(quickFilter_) == std::string::npos)
                continue;
            if (!compatible(spec)) continue;
            if (ImGui::Selectable(TLs(spec.displayName))) {
                auto& gg = ensureGraph();
                auto& n = gg.addNode(spec.type, {quickAt_.x - 40, quickAt_.y - 20}, spec.category == studio::NodeCategory::NexplayCommands ? std::string() : std::string());
                // auto-connect the first compatible port pair
                if (srcPort) {
                    const auto& cand = srcPort->dir == studio::PortDir::Output ? n.inputs : n.outputs;
                    for (const auto& cp : cand) {
                        auto chk = gg.connect(wireSrcNode_, wireSrcPort_, n.id, cp.id, nullptr, true);
                        if (chk.ok) break;
                    }
                    if (wireSrcOtherConn_) gg.disconnect(wireSrcOtherConn_);
                }
                clearSel();
                sel_.insert(n.id);
                commit();
                wireSrcNode_ = wireSrcPort_ = wireSrcOtherConn_ = 0;
                quickMenu_ = false;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !spec.summary.empty())
                ImGui::SetTooltip("%s", TLs(spec.summary));
        }
        ImGui::EndPopup();
    } else {
        quickMenu_ = false;
        wireSrcNode_ = wireSrcPort_ = wireSrcOtherConn_ = 0;
    }
}

// ============================================================================
// Context menus
// ============================================================================

void StudioEditor::drawContextMenus() {
    auto* g = graph();
    if (ImGui::BeginPopup("##studio_ctx")) {
        if (!g) { ImGui::EndPopup(); return; }
        switch (ctx_) {
            case Ctx::Canvas:
                if (ImGui::BeginMenu(TL("Add node"))) {
                    for (int ci = 0; ci < 7; ++ci) {
                        const auto cat = static_cast<studio::NodeCategory>(ci);
                        if (ImGui::BeginMenu(TLs(studio::categoryName(cat)))) {
                            for (const auto& spec : studio::allNodeSpecs())
                                if (spec.category == cat && ImGui::MenuItem(TLs(spec.displayName)))
                                    addNodeAt(spec.type, {ctxCanvas_.x - 60, ctxCanvas_.y - 15});
                            ImGui::EndMenu();
                        }
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem(TL("Add note"))) {
                    g->addNote(i18n::tr("Note"), "", {ctxCanvas_.x, ctxCanvas_.y});
                    commit();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(TL("Paste"), "Ctrl+V", false, !clipboard_.empty()))
                    pasteAt(ctxCanvas_);
                if (ImGui::MenuItem(TL("Select all"), "Ctrl+A")) selectAll();
                ImGui::Separator();
                if (ImGui::MenuItem(TL("Auto arrange"))) { g->autoArrange(); commit(); }
                if (ImGui::MenuItem(TL("Fit to view"), "F")) fitView();
                ImGui::Separator();
                if (ImGui::BeginMenu(TL("Wire style"))) {
                    auto& ws = view().wireStyle;
                    if (ImGui::MenuItem(TL("Curved"), nullptr, ws == studio::WireStyle::Curved)) ws = studio::WireStyle::Curved;
                    if (ImGui::MenuItem(TL("Rounded"), nullptr, ws == studio::WireStyle::Rounded)) ws = studio::WireStyle::Rounded;
                    if (ImGui::MenuItem(TL("Straight"), nullptr, ws == studio::WireStyle::Straight)) ws = studio::WireStyle::Straight;
                    ImGui::EndMenu();
                }
                ImGui::MenuItem(TL("Show grid"), nullptr, &view().showGrid);
                ImGui::MenuItem(TL("Snap to grid"), nullptr, &view().snapToGrid);
                ImGui::MenuItem(TL("Show minimap"), nullptr, &view().showMinimap);
                break;
            case Ctx::Node:
                if (auto* n = g->findNode(ctxId_)) {
                    if (ImGui::MenuItem(TL("Rename"))) {
                        renameNode_ = ctxId_;
                        std::snprintf(renameBuf_, sizeof renameBuf_, "%s", nodeTitle(*n).c_str());
                        ImGui::OpenPopup("##studio_rename");
                    }
                    if (ImGui::MenuItem(TL("Duplicate"), "Ctrl+D")) duplicateSelection();
                    ImGui::Separator();
                    if (ImGui::MenuItem(n->breakpoint ? TL("Remove breakpoint") : TL("Add breakpoint"))) {
                        n->breakpoint = !n->breakpoint;
                        commit();
                    }
                    if (ImGui::MenuItem(n->enabled ? TL("Disable node") : TL("Enable node"))) {
                        n->enabled = !n->enabled;
                        commit();
                    }
                    if (ImGui::MenuItem(n->collapsed ? TL("Expand node") : TL("Collapse node"))) {
                        n->collapsed = !n->collapsed;
                        commit();
                    }
                    if (ImGui::MenuItem(TL("Group selection"), "Ctrl+G", false, sel_.size() >= 2))
                        groupSelection();
                    ImGui::Separator();
                    if (ImGui::MenuItem(TL("Delete"), "Del")) deleteSelection();
                }
                break;
            case Ctx::Wire:
                if (ImGui::MenuItem(TL("Delete wire"), "Del")) {
                    if (g->disconnect(selConn_)) { selConn_ = 0; commit(); }
                }
                break;
            case Ctx::Group: {
                auto grIt = std::find_if(g->groups.begin(), g->groups.end(),
                                         [&](studio::GraphGroup& x) { return x.id == ctxId_; });
                if (grIt != g->groups.end()) {
                    auto& gr = *grIt;
                    if (ImGui::MenuItem(TL("Rename frame"))) { renameGroup_ = ctxId_; ImGui::OpenPopup("##studio_rename"); }
                    if (ImGui::MenuItem(gr.collapsed ? TL("Expand frame") : TL("Collapse frame"))) {
                        gr.collapsed = !gr.collapsed;
                        commit();
                    }
                    if (ImGui::MenuItem(TL("Ungroup (keep nodes)"))) {
                        g->removeGroup(ctxId_);
                        selGroup_ = 0;
                        commit();
                    }
                }
                break;
            }
            case Ctx::Note:
                if (ImGui::MenuItem(TL("Edit note"))) { renameNote_ = ctxId_; ImGui::OpenPopup("##studio_rename"); }
                if (ImGui::MenuItem(TL("Delete note"))) {
                    g->removeNote(ctxId_);
                    selNotes_.erase(ctxId_);
                    commit();
                }
                break;
            default: break;
        }
        ImGui::EndPopup();
    } else {
        ctx_ = Ctx::None;
    }
}

// ============================================================================
// Inline rename popup
// ============================================================================

void StudioEditor::drawRenameBox() {
    auto* g = graph();
    ImGui::SetNextWindowSize({260 * m_.scale, 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##studio_rename", ImGuiWindowFlags_NoCollapse)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool done = ImGui::InputTextWithHint("##rn", TL("Name..."), renameBuf_, sizeof renameBuf_,
                                                   ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        if (done || (!ImGui::IsItemActive() && !ImGui::IsWindowAppearing() && ImGui::IsMouseClicked(0))) {
            if (g) {
                if (renameNode_) {
                    if (auto* n = g->findNode(renameNode_)) { n->title = renameBuf_; commit(); }
                    renameNode_ = 0;
                } else if (renameNote_) {
                    for (auto& nt : g->notes) if (nt.id == renameNote_) { nt.title = renameBuf_; commit(); }
                    renameNote_ = 0;
                } else if (renameGroup_) {
                    for (auto& gr : g->groups) if (gr.id == renameGroup_) { gr.title = renameBuf_; commit(); }
                    renameGroup_ = 0;
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace wasd::app
