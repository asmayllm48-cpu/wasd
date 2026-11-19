// NEXPLAY Automation Studio — editor shell state shared by the canvas
// (studio_canvas.cpp) and the docked panels (studio_panels.cpp).
// The editor mutates Automation::studioGraphs inside the live profile draft, so
// dirty tracking / Save / Discard integrate through the existing profile guard.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "app/studio_design.h"
#include "core/automation.h"
#include "core/studio_engine.h"
#include "core/studio_graph.h"
#include "imgui.h"

namespace wasd::app {

class StudioEditor {
public:
    StudioEditor();

    // ---- Host callbacks wired by App --------------------------------------
    std::function<void(const std::string&)> toast;
    std::function<void()> requestClose;
    std::function<bool()> sessionActive;
    // Picking on the live video: App hides the studio, runs pickMode_, then
    // reopens. The editor sets wantsPick_ and App performs the switch.
    std::function<void(Vec2*)> pickPoint;
    // pickMode 7: two draggable on-screen markers for a swipe's start and end
    // points — the same interaction the key editor gives a Drag binding.
    std::function<void(Vec2* from, Vec2* to)> pickSwipe;
    // pickMode 8: click pixels on the live video — `color` (if set) is
    // overwritten with the latest sample and each click appends "r,g,b" to
    // `list` (if set). Stays armed until Esc.
    std::function<void(studio::ColorVal* color, std::string* list)> pickColors;
    std::function<void(Rect*)> pickRect;
    std::function<void(int imageIndex)> pickImage;
    std::function<void()> save;                              // persist the profile draft (Ctrl+S)
    // Key capture into *target (slot: 7 = trigger/toggle keys, 6 = action keys).
    std::function<void(int* target, int* comboTarget, int slot)> captureKey;
    std::function<bool(const int* target)> capturingKey;     // is *target the live capture slot
    std::function<void(int imageIndex)> deleteImage;         // remove a reference image (renumbers refs)

    // ---- Legacy automation surface merged into the studio ----------------------
    std::function<bool()> automationOn;                      // master monitoring switch state
    std::function<void(bool)> setAutomationOn;               // flip the master switch
    std::function<void()> newZone;                           // arm "draw a zone on the game" (pickMode 2)
    std::function<void(int zone, int color, bool multi)> pickZoneColor;  // pickMode 1
    std::function<void(int image)> testImage;                // toggle live scan test on an image
    std::function<bool(int)> imageTesting;
    std::function<bool()> zoneTestOn;                        // live-test all color zones
    std::function<void(bool)> setZoneTest;
    std::function<void()> reloadImages;                      // re-read ImageRef.rgb from disk
    std::function<void()> resetMonitorRuntime;               // resync runtime state after structural edits
    std::function<void(int monitor)> armMonitor;             // toggle MonitorRuntime::switchedOn

    // Live statuses reported to the studio (read-only snapshots).
    struct ZoneStatus { bool tested = false, present = false; int matched = 0, total = 0; std::vector<int> perColor; };
    struct ImageStatus { bool tested = false, found = false; float score = 0; int64_t costUs = 0; };
    struct MonitorStatus { bool armed = false, present = false; int runs = 0; };
    std::function<ZoneStatus(int zone)> zoneStatus;
    std::function<ImageStatus(int image)> imageStatus;
    std::function<MonitorStatus(int monitor)> monitorStatus;

    // Find Object (ONNX): model classes for the inspector checkboxes, worker
    // status text, and the live-preview toggle (draws boxes over the video).
    std::function<std::vector<std::string>()> detectClasses;
    std::function<std::string()> detectStatus;
    std::function<void(bool on, const Rect& region, const std::vector<std::string>& classes,
                       float conf)> setDetectPreview;

    // Optional runtime hooks supplied by the execution engine (Phase 5+).
    struct RuntimeHooks {
        std::function<void(const std::string& graphId, bool debug)> run;
        std::function<void()> stop;
        std::function<bool(const std::string& graphId)> running;
        std::function<bool()> paused;
        std::function<void()> pause;
        std::function<void()> resume;
        std::function<void()> stepOnce;
        std::function<studio_ui::NodeRunBadge(const std::string& graphId, uint32_t nodeId)> nodeBadge;
        std::function<std::string(uint32_t connId)> wireValue;  // live value preview on wires (debug)
        // Live console/timeline feed (pointer valid until the run ends).
        std::function<const std::vector<studio::GraphEvent>*()> events;
    } runtime;

    // ---- Frame entry -------------------------------------------------------
    // Draws the whole studio workspace filling the current ImGui window.
    // `a` is the live draft automation (profile_.automation).
    void draw(Automation& a);
    // True while a pending pick was requested (App should hide the studio so the
    // video is clickable, then reopen once the pick resolves).
    bool wantsHideForPick() const { return hideForPick_; }
    void clearHideForPick() { hideForPick_ = false; }

    // Selects a graph by index (used after converting a legacy macro so the new
    // graph is the one on screen when the studio opens).
    void focusGraph(int idx) { selectGraph(idx); }

    // Opens the inspector on a given tab (0 Node, 1 Graph, 2 Variables,
    // 3 Images, 4 Zones, 5 Rules) — used by the home-screen shortcut cards.
    void focusInspectorTab(int t) { inspOpen_ = true; inspTab_ = t; }

private:
    // ---- Document ---------------------------------------------------------
    Automation* automation_ = nullptr;
    int graphIndex_ = -1;                    // index into automation_->studioGraphs
    studio::AutomationGraph* graph();
    const studio::AutomationGraph* graph() const;
    studio::AutomationGraph& ensureGraph();  // creates a starter graph when empty
    void selectGraph(int i);
    void newGraph(studio::GraphTemplateKind kind);
    void deleteGraph(int i);

    // ---- Canvas transform --------------------------------------------------
    ImVec2 canvasOrigin_{}, canvasSize_{};
    ImVec2 toScreen(Vec2 c) const;
    Vec2 toCanvas(ImVec2 s) const;
    studio::CanvasViewState& view();
    const studio::CanvasViewState& view() const;
    void fitView();
    float gridStep() const;

    // ---- Node layout --------------------------------------------------------
    ImVec2 nodeSize(const studio::GraphNode& n) const;                 // canvas units
    Vec2 portPos(const studio::GraphNode& n, const studio::Port& p) const;  // canvas units
    std::string nodeTitle(const studio::GraphNode& n) const;           // display title
    std::string nodeSummary(const studio::GraphNode& n) const;         // compact prop summary line
    ImU32 nodeAccent(const studio::GraphNode& n) const;

    // ---- Selection ----------------------------------------------------------
    std::set<uint32_t> sel_;        // node ids
    std::set<uint32_t> selNotes_;   // note ids
    uint32_t selConn_ = 0;          // connection id (0 = none)
    uint32_t selGroup_ = 0;         // frame id (0 = none)
    bool isSel(uint32_t nodeId) const { return sel_.count(nodeId) != 0; }
    void clearSel();

    // ---- Interaction state ---------------------------------------------------
    enum class Drag : uint8_t {
        None,
        Pan,          // MMB / Space+LMB panning
        Move,         // dragging selected nodes/notes
        Marquee,      // rubber-band selection
        Wire,         // pulling a wire off a port
        GroupMove,
        GroupResize,
        NoteResize
    } drag_ = Drag::None;

    ImVec2 dragMouse0_{};           // screen pos where the drag began
    Vec2 panAtDrag_{};              // view.pan snapshot while panning
    std::vector<std::pair<uint32_t, Vec2>> moveOffsets_;  // item id -> pos delta vs grab
    std::vector<std::pair<uint32_t, Vec2>> moveNoteOffsets_;
    bool moved_ = false;            // a Move drag actually displaced items
    // wire drag
    uint32_t wireSrcNode_ = 0, wireSrcPort_ = 0;
    uint32_t wireSrcOtherConn_ = 0; // existing conn being re-routed (0 = fresh wire)
    ImVec2 wireMouse_{};            // current free end (screen)
    studio::ConnectionCheck wireCheck_{};
    bool wireCheckValid_ = false;
    // marquee
    ImVec2 marqueeA_{}, marqueeB_{};
    bool marqueeAdd_ = false;
    // groups / notes
    uint32_t dragGroup_ = 0;
    int groupCorner_ = -1;          // 0..3 corners, -1 body
    Rect groupRect0_{};
    uint32_t dragNote_ = 0;
    Vec2 noteSize0_{};

    // hover (canvas units resolved per frame)
    uint32_t hovNode_ = 0, hovPort_ = 0, hovConn_ = 0, hovNote_ = 0, hovGroup_ = 0;
    bool hovPortIsOut_ = false;

    // quick-create popup after a wire is dropped on empty canvas
    bool quickMenu_ = false;
    Vec2 quickAt_{};
    char quickFilter_[96] = {};

    // inline rename
    uint32_t renameNode_ = 0, renameNote_ = 0, renameGroup_ = 0;
    char renameBuf_[160] = {};

    // context menus
    enum class Ctx : uint8_t { None, Canvas, Node, Wire, Group, Note } ctx_ = Ctx::None;
    uint32_t ctxId_ = 0;
    Vec2 ctxCanvas_{};

    // ---- History / clipboard -------------------------------------------------
    studio::GraphHistory history_;
    studio::GraphClipboard clipboard_;
    void commit();                  // push current graph onto history
    void undoHistory();
    void redoHistory();

    // ---- Editing ops ----------------------------------------------------------
    void addNodeAt(studio::NodeType t, Vec2 pos, const std::string& customKey = {});
    void deleteSelection();
    void copySelection(bool cut);
    void pasteAt(Vec2 canvasPos);
    void duplicateSelection();
    void selectAll();
    void groupSelection();
    void beginWire(uint32_t nodeId, uint32_t portId);
    void dropWire();

    // ---- Hit testing ------------------------------------------------------------
    uint32_t nodeAt(Vec2 c) const;                       // topmost node id or 0
    bool portAt(Vec2 c, uint32_t& nodeId, uint32_t& portId) const;
    uint32_t connAt(Vec2 c) const;
    uint32_t noteAt(Vec2 c) const;
    // group hit: returns frame id; corner -1 = title bar / body, 0..3 = resize corners
    uint32_t groupAt(Vec2 c, int& corner) const;

    // ---- Panels state (Phase 4 fleshes out visuals) ------------------------------
    float libW_ = 236.0f;      // dp, resizable
    float inspW_ = 300.0f;     // dp, resizable
    float consoleH_ = 0.0f;    // dp, 0 = collapsed
    bool libOpen_ = true;
    bool inspOpen_ = true;
    bool consolePinned_ = false;
    char libSearch_[96] = {};
    std::set<int> libClosed_;              // collapsed category indices
    std::set<std::string> favNodes_;       // favorite node typeKeys (session-scoped)
    std::vector<std::string> recentNodes_; // recently created node typeKeys (most recent first)
    void pushRecent(const studio::NodeTypeSpec& spec);
    bool inspAdvanced_ = false;
    int inspTab_ = -1;         // one-shot tab focus request (-1 = none); see focusInspectorTab

    // ---- Execution console ------------------------------------------------------
    struct LogLine {
        ImU32 col = 0;
        std::string text;
    };
    std::vector<LogLine> console_;
    size_t seenEvents_ = 0;  // drained prefix of GraphRunner::events()
    void log(const std::string& msg, ImU32 col = 0);
    void drainRunnerEvents();  // merge live execution events into the console

    // ---- Misc -------------------------------------------------------------------
    studio_ui::Metrics m_{};
    studio_ui::Palette pal_{};
    bool dark_ = true;
    bool hideForPick_ = false;
    bool detectPreviewOn_ = false;  // Find Object live-test overlay armed
    uint32_t minViewNode_ = 0;  // minimap click-drag state
    bool minimapDrag_ = false;

    // ---- Draw passes (studio_canvas.cpp) ------------------------------------------
    void drawCanvasArea();
    void drawGrid(ImDrawList* dl);
    void drawGroupsLayer(ImDrawList* dl);
    void drawWiresLayer(ImDrawList* dl);
    void drawNodesLayer(ImDrawList* dl);
    void drawNotesLayer(ImDrawList* dl);
    void drawWireDrag(ImDrawList* dl);
    void drawMarquee(ImDrawList* dl);
    void drawMinimap();
    void canvasInput();
    void shortcuts();
    void drawQuickCreate();
    void drawContextMenus();
    void drawRenameBox();
    void wireBezier(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thick, bool arrow);

    // ---- Panels (studio_panels.cpp) ----------------------------------------------
    void drawToolbar(float w);
    void drawLibrary(float w, float h);
    void drawInspector(float w, float h);
    void drawConsole(float w, float h);
    void drawEmptyState(float w, float h);
    void drawStatusBar(float w);
    void libraryItem(const studio::NodeTypeSpec& spec);
    void inspectorNode(studio::GraphNode& n);
    void inspectorGraph();
    void inspectorVariables();
    void inspectorImages();
    void inspectorZones();
    void inspectorRules();
    int imagesSel_ = -1;  // selected reference image in the Images tab
    int zonesSel_ = -1;   // selected color zone in the Zones tab
    int monitorSel_ = -1; // selected rule in the Rules tab
    bool zoneAdv_ = false, monitorAdv_ = false;
    bool valueEditor(const char* id, const studio::PropertySpec& ps, studio::Value& v);
    // Dedicated editors for props that are meaningless as raw numbers (image /
    // zone / macro references, enum ints, on-screen rectangles). Returns true
    // when the property was rendered here; `changed` reports an edit.
    bool nodePropEditor(const studio::NodeTypeSpec& spec, studio::GraphNode& n,
                        const studio::PropertySpec& ps, studio::Value& v, bool& changed);
    // Capture-button row for a Start entry's trigger keys. `inh1`/`inh2` are
    // the graph-metadata fallbacks shown (and cleared) when the node's own
    // fields are empty — used only for the FIRST Start so old profiles keep
    // working after per-Start triggers were introduced.
    void triggerKeyRowFor(int* k1, int* k2, int* inh1 = nullptr, int* inh2 = nullptr);

    // Pending screen pick: the studio hides while the user drags a region or
    // captures an image on the live video, then applies the result on resume.
    struct PendingPick {
        uint32_t nodeId = 0;
        std::array<std::string, 4> keys{};   // rect prop keys (keys[0] empty = not a rect pick)
        Rect rect{0, 0, 1, 1};               // staging written by App's pick overlay
        int imagesBefore = -1;               // image capture: images.size() before pick
        std::string imageKey;                // prop receiving the new image index
        double requestedAt = 0.0;            // ImGui::GetTime() for the wait timeout
    } pendingPick_;
    void syncPendingPick();
    uint32_t lastInspNode_ = 0;  // inspector tab: force-select only when selection changes
    void commitIf(bool changed) { if (changed) commit(); }
};

}  // namespace wasd::app
