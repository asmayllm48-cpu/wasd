// NEXPLAY Automation Studio — Unified Design System ("Modern Dark Creative Studio").
// Centralizes Color, Typography, Spacing, Radius, Shadow, Category & Port tokens.
#pragma once
#include <algorithm>
#include <cstdint>

#include "core/studio_graph.h"
#include "imgui.h"

namespace wasd::app::studio_ui {

// ---------------------------------------------------------------- Spacing & Radius Tokens

struct Metrics {
    float scale = 1.0f;
    float spXS = 4.0f;
    float spSM = 8.0f;
    float spMD = 12.0f;
    float spLG = 16.0f;
    float spXL = 24.0f;

    float radiusSM = 5.0f;
    float radiusMD = 8.0f;
    float radiusLG = 11.0f;

    float toolbarH = 44.0f;
    float statusBarH = 26.0f;
    float nodeHeaderH = 30.0f;
    float nodeRowH = 24.0f;
    float nodeMinW = 210.0f;
    float portHitRadius = 9.0f;

    static Metrics make(float dpiScale) {
        const float s = std::max(1.0f, dpiScale);
        Metrics m;
        m.scale = s;
        m.spXS = 4.0f * s;
        m.spSM = 8.0f * s;
        m.spMD = 12.0f * s;
        m.spLG = 16.0f * s;
        m.spXL = 24.0f * s;
        m.radiusSM = 5.0f * s;
        m.radiusMD = 8.0f * s;
        m.radiusLG = 11.0f * s;
        m.toolbarH = 44.0f * s;
        m.statusBarH = 26.0f * s;
        m.nodeHeaderH = 30.0f * s;
        m.nodeRowH = 24.0f * s;
        m.nodeMinW = 210.0f * s;
        m.portHitRadius = 9.0f * s;
        return m;
    }
};

// ---------------------------------------------------------------- Color Palette Tokens

struct Palette {
    // Surfaces
    ImU32 appBg;          // Dark Charcoal shell
    ImU32 canvasBg;       // Deep Graphite infinite workspace
    ImU32 gridMinor;      // Subtle sub-grid
    ImU32 gridMajor;      // Subtle major grid
    ImU32 panelBg;        // Docked sidebars / console
    ImU32 surfaceCard;    // Elevated surface
    ImU32 surfaceHover;   // Interactive hover fill
    ImU32 nodeBody;       // Node card fill
    ImU32 nodeBodyMuted;  // Disabled node fill
    ImU32 nodeBorder;     // Default hairline border
    ImU32 shadowSoft;     // Subtle elevation shadow

    // Brand & Semantic Accents
    ImU32 accentPrimary;  // Electric Blue #3B82F6
    ImU32 accentCyan;     // Electric Cyan #22D3EE
    ImU32 success;        // Green #22C55E
    ImU32 warning;        // Amber #F59E0B
    ImU32 danger;         // Red #EF4444

    // Typography
    ImU32 textPrimary;    // Soft White
    ImU32 textSecondary;  // Muted Slate Gray
    ImU32 textDim;        // Subtle hint text
    ImU32 divider;        // Structural separator

    static Palette current(bool dark) {
        Palette p{};
        if (dark) {
            p.appBg         = IM_COL32(22, 24, 30, 255);    // #16181E Dark Charcoal
            p.canvasBg      = IM_COL32(16, 18, 23, 255);    // #101217 Deep Graphite
            p.gridMinor     = IM_COL32(255, 255, 255, 9);   // ultra-quiet sub-grid
            p.gridMajor     = IM_COL32(255, 255, 255, 20);  // clean major grid
            p.panelBg       = IM_COL32(24, 27, 34, 255);    // #181B22
            p.surfaceCard   = IM_COL32(31, 35, 44, 255);    // #1F232C
            p.surfaceHover  = IM_COL32(41, 47, 59, 255);    // #292F3B
            p.nodeBody      = IM_COL32(27, 31, 39, 246);    // #1B1F27
            p.nodeBodyMuted = IM_COL32(23, 25, 31, 210);
            p.nodeBorder    = IM_COL32(54, 62, 78, 255);    // #363E4E
            p.shadowSoft    = IM_COL32(0, 0, 0, 110);

            p.accentPrimary = IM_COL32(59, 130, 246, 255);  // #3B82F6
            p.accentCyan    = IM_COL32(34, 211, 238, 255);  // #22D3EE
            p.success       = IM_COL32(34, 197, 94, 255);   // #22C55E
            p.warning       = IM_COL32(245, 158, 11, 255);  // #F59E0B
            p.danger        = IM_COL32(239, 68, 68, 255);   // #EF4444

            p.textPrimary   = IM_COL32(241, 245, 249, 255); // #F1F5F9
            p.textSecondary = IM_COL32(148, 163, 184, 255); // #94A3B8
            p.textDim       = IM_COL32(100, 116, 139, 255); // #64748B
            p.divider       = IM_COL32(43, 50, 64, 255);    // #2B3240
        } else {
            p.appBg         = IM_COL32(241, 245, 249, 255);
            p.canvasBg      = IM_COL32(229, 234, 242, 255);
            p.gridMinor     = IM_COL32(15, 23, 42, 11);
            p.gridMajor     = IM_COL32(15, 23, 42, 24);
            p.panelBg       = IM_COL32(248, 250, 252, 255);
            p.surfaceCard   = IM_COL32(255, 255, 255, 255);
            p.surfaceHover  = IM_COL32(226, 232, 240, 255);
            p.nodeBody      = IM_COL32(255, 255, 255, 250);
            p.nodeBodyMuted = IM_COL32(241, 245, 249, 225);
            p.nodeBorder    = IM_COL32(203, 213, 225, 255);
            p.shadowSoft    = IM_COL32(15, 23, 42, 34);

            p.accentPrimary = IM_COL32(37, 99, 235, 255);
            p.accentCyan    = IM_COL32(8, 145, 178, 255);
            p.success       = IM_COL32(22, 163, 74, 255);
            p.warning       = IM_COL32(217, 119, 6, 255);
            p.danger        = IM_COL32(220, 38, 38, 255);

            p.textPrimary   = IM_COL32(15, 23, 42, 255);
            p.textSecondary = IM_COL32(71, 85, 105, 255);
            p.textDim       = IM_COL32(148, 163, 184, 255);
            p.divider       = IM_COL32(203, 213, 225, 255);
        }
        return p;
    }
};

inline ImU32 withAlpha(ImU32 c, float a) {
    const int alphaByte = std::clamp(static_cast<int>(((c >> IM_COL32_A_SHIFT) & 0xFF) * a), 0, 255);
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alphaByte) << IM_COL32_A_SHIFT);
}

// Calm, distinct category accents (used as subtle header tint + crisp icon badge, never garish).
inline ImU32 categoryAccent(studio::NodeCategory cat) {
    switch (cat) {
        case studio::NodeCategory::FlowControl:      return IM_COL32(148, 163, 184, 255); // Cool Slate
        case studio::NodeCategory::InputActions:     return IM_COL32(59, 130, 246, 255);  // Electric Blue
        case studio::NodeCategory::ImageRecognition: return IM_COL32(34, 211, 238, 255);  // Electric Cyan
        case studio::NodeCategory::ColorRecognition: return IM_COL32(244, 114, 182, 255); // Coral Rose
        case studio::NodeCategory::Logic:            return IM_COL32(168, 85, 247, 255);  // Soft Violet
        case studio::NodeCategory::VariablesMath:    return IM_COL32(52, 211, 153, 255);  // Emerald Mint
        case studio::NodeCategory::NexplayCommands:  return IM_COL32(251, 146, 60, 255);  // Warm Amber
    }
    return IM_COL32(59, 130, 246, 255);
}

// Distinct, readable port colors per data type; Execution ports are crisp white/ice.
inline ImU32 portColor(const studio::Port& p, bool dark = true) {
    if (p.kind == studio::PortKind::Execution)
        return dark ? IM_COL32(236, 242, 250, 255) : IM_COL32(30, 41, 59, 255);
    switch (p.dataType) {
        case studio::ValueType::Boolean:     return IM_COL32(239, 68, 68, 255);   // Red-coral boolean
        case studio::ValueType::Integer:     return IM_COL32(52, 211, 153, 255);  // Mint integer
        case studio::ValueType::Float:       return IM_COL32(56, 189, 248, 255);  // Sky float
        case studio::ValueType::String:      return IM_COL32(232, 121, 249, 255); // Magenta string
        case studio::ValueType::Coordinate:  return IM_COL32(250, 204, 21, 255);  // Gold coordinate
        case studio::ValueType::Vector2:     return IM_COL32(251, 146, 60, 255);  // Orange vec2
        case studio::ValueType::Color:       return IM_COL32(244, 114, 182, 255); // Pink color
        case studio::ValueType::Image:       return IM_COL32(34, 211, 238, 255);  // Cyan image
        case studio::ValueType::ImageResult: return IM_COL32(96, 165, 250, 255);  // Royal result
        case studio::ValueType::Array:       return IM_COL32(167, 139, 250, 255); // Indigo array
        case studio::ValueType::Object:      return IM_COL32(148, 163, 184, 255); // Neutral object
        case studio::ValueType::Any:         return IM_COL32(203, 213, 225, 255); // Silver wildcard
    }
    return IM_COL32(148, 163, 184, 255);
}

// Visual runtime state decoupled from the execution engine (Requirement 13).
enum class NodeRunBadge : uint8_t {
    None = 0,
    Executing,
    Completed,
    Warning,
    Error
};

}  // namespace wasd::app::studio_ui
