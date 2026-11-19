// NEXPLAY Automation Studio — bidirectional converter between legacy Simple-Mode
// macros (Step trees) and visual AutomationGraphs.
//
// macroToGraph is strictly NON-DESTRUCTIVE: the source Macro is never modified;
// the produced graph records sourceLegacyMacro + conversion notes so the UI can
// show a review report and offer the reverse trip (graphToMacroSteps) with
// explicit data-loss warnings for constructs Simple Mode cannot represent.
#pragma once
#include <string>
#include <vector>

#include "core/automation.h"
#include "core/studio_graph.h"

namespace wasd::studio {

struct ConversionReport {
    AutomationGraph graph;
    std::vector<std::string> notes;   // human-readable review lines (English keys; UI localizes)
    int stepsConverted = 0;
    int nodesCreated = 0;
    int warnings = 0;
};

// Converts a Simple-Mode macro into a visual graph. Handles every StepType:
// Wait/Tap/LongPress/PressDown/Release/Swipe/Key/RunMacro/If/Loop/Break/Stop/
// Comment/ImageRecognize. Legacy JumpStep targets become JumpTarget anchors.
ConversionReport macroToGraph(const Macro& m, const Automation& a);

struct SimplifyReport {
    std::vector<Step> steps;
    std::vector<std::string> warnings;  // data-loss / approximation notes
    int nodesDropped = 0;
    bool lossless() const { return warnings.empty() && nodesDropped == 0; }
};

// Best-effort graph -> Simple-Mode step list. Follows the exec chain from Start;
// pure data nodes, unrepresentable flow constructs and graph metadata that have
// no Simple-Mode equivalent are reported in `warnings` (never silently dropped).
SimplifyReport graphToMacroSteps(const AutomationGraph& g);

}  // namespace wasd::studio
