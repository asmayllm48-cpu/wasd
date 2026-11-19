// Arabic/RTL text for the ImGui UI. Uses Windows Uniscribe for bidi reordering and
// contextual shaping; shaped glyph ids are exposed to the font loader through
// private-use "slot" codepoints so ImGui can draw them without its own shaping.
#pragma once
#include <string>
#include <string_view>

namespace wasd::app::text {

inline constexpr unsigned kSlotFirst = 0xE000, kSlotLast = 0xE6FF;

// True when the UTF-8 text contains Hebrew/Arabic-range characters.
bool hasRtl(std::string_view utf8);
// Visual-order UTF-8 string ready for ImGui drawing. Cached; plain LTR text is returned unchanged.
const std::string& visual(std::string_view logical);
// Glyph id (in Segoe UI) for a slot codepoint produced by visual(); 0 when unknown.
int glyphForSlot(unsigned codepoint);
// Drops the cache when it grew too large. Call once per frame before drawing.
void frameTick();
void shutdown();

}  // namespace wasd::app::text
