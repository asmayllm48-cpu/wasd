#include "app/text_shaping.h"

#include <windows.h>
#include <usp10.h>

#include <unordered_map>
#include <vector>

namespace wasd::app::text {
namespace {

struct Shaper {
    HDC dc = nullptr;
    HFONT font = nullptr;
    SCRIPT_CACHE cache = nullptr;
    std::vector<WORD> slotGlyph;                 // slot index -> glyph id
    std::unordered_map<WORD, unsigned> glyphSlot;  // glyph id -> slot codepoint
    std::unordered_map<std::string, std::string> visualCache;

    bool ready() {
        if (dc) return true;
        dc = CreateCompatibleDC(nullptr);
        font = CreateFontW(-32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (dc && font) SelectObject(dc, font);
        return dc && font;
    }
    void release() {
        if (cache) ScriptFreeCache(&cache);
        if (dc) DeleteDC(dc), dc = nullptr;
        if (font) DeleteObject(font), font = nullptr;
        slotGlyph.clear();
        glyphSlot.clear();
        visualCache.clear();
    }
    unsigned slotFor(WORD glyph) {
        if (auto it = glyphSlot.find(glyph); it != glyphSlot.end()) return it->second;
        if (slotGlyph.size() > kSlotLast - kSlotFirst) return '?';
        const unsigned cp = kSlotFirst + static_cast<unsigned>(slotGlyph.size());
        slotGlyph.push_back(glyph);
        glyphSlot.emplace(glyph, cp);
        return cp;
    }
};

Shaper& shaper() {
    static Shaper s;
    return s;
}

void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) out += static_cast<char>(0xC0 | (cp >> 6)), out += static_cast<char>(0x80 | (cp & 0x3F));
    else if (cp < 0x10000)
        out += static_cast<char>(0xE0 | (cp >> 12)), out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
            out += static_cast<char>(0x80 | (cp & 0x3F));
    else
        out += static_cast<char>(0xF0 | (cp >> 18)), out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void appendWide(std::string& out, const wchar_t* w, int n) {
    if (n <= 0) return;
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    const size_t at = out.size();
    out.resize(at + static_cast<size_t>(len));
    WideCharToMultiByte(CP_UTF8, 0, w, n, out.data() + at, len, nullptr, nullptr);
}

bool isRtlChar(wchar_t c) { return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF); }
bool isStrongLtr(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= 0xC0 && c < 0x0590); }

// Shapes one right-to-left run; glyphs come back from ScriptShape in visual order.
bool shapeRun(Shaper& s, const wchar_t* run, int len, SCRIPT_ANALYSIS a, std::string& out) {
    int maxGlyphs = len * 3 / 2 + 16;
    std::vector<WORD> glyphs, clusters(static_cast<size_t>(len));
    std::vector<SCRIPT_VISATTR> attrs;
    int count = 0;
    HRESULT hr;
    for (int attempt = 0; attempt < 3; ++attempt) {
        glyphs.resize(static_cast<size_t>(maxGlyphs));
        attrs.resize(static_cast<size_t>(maxGlyphs));
        hr = ScriptShape(s.dc, &s.cache, run, len, maxGlyphs, &a, glyphs.data(), clusters.data(), attrs.data(), &count);
        if (hr == USP_E_SCRIPT_NOT_IN_FONT) a.eScript = SCRIPT_UNDEFINED;
        else if (hr == E_OUTOFMEMORY) maxGlyphs *= 2;
        else break;
    }
    if (FAILED(hr)) return false;
    for (int g = 0; g < count; ++g)
        if (glyphs[static_cast<size_t>(g)]) appendUtf8(out, s.slotFor(glyphs[static_cast<size_t>(g)]));
    return true;
}

std::string reorder(std::string_view logical) {
    Shaper& s = shaper();
    const std::wstring w = widen(logical);
    if (w.empty() || !s.ready()) return std::string(logical);
    bool rtlBase = false;  // paragraph direction from the first strong character
    for (wchar_t c : w) {
        if (isRtlChar(c)) {
            rtlBase = true;
            break;
        }
        if (isStrongLtr(c)) break;
    }
    SCRIPT_CONTROL control{};
    SCRIPT_STATE state{};
    state.uBidiLevel = rtlBase ? 1 : 0;
    std::vector<SCRIPT_ITEM> items(w.size() + 2);
    int n = 0;
    if (FAILED(ScriptItemize(w.data(), static_cast<int>(w.size()), static_cast<int>(items.size()) - 1, &control, &state, items.data(), &n)))
        return std::string(logical);
    std::vector<BYTE> levels(static_cast<size_t>(n));
    std::vector<int> visualToLogical(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) levels[static_cast<size_t>(i)] = static_cast<BYTE>(items[static_cast<size_t>(i)].a.s.uBidiLevel);
    ScriptLayout(n, levels.data(), visualToLogical.data(), nullptr);
    std::string out;
    out.reserve(logical.size() * 2);
    for (int v = 0; v < n; ++v) {
        const int i = visualToLogical[static_cast<size_t>(v)];
        const SCRIPT_ITEM& it = items[static_cast<size_t>(i)];
        const int start = it.iCharPos, len = items[static_cast<size_t>(i) + 1].iCharPos - start;
        if (levels[static_cast<size_t>(i)] & 1) {
            if (!shapeRun(s, w.data() + start, len, it.a, out)) appendWide(out, w.data() + start, len);
        } else {
            appendWide(out, w.data() + start, len);
        }
    }
    return out;
}

}  // namespace

bool hasRtl(std::string_view u) {
    for (size_t i = 0; i + 1 < u.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(u[i]);
        if (c >= 0xD6 && c <= 0xDF) return true;  // U+0580..U+07FF lead bytes
        if (c == 0xE0 && static_cast<unsigned char>(u[i + 1]) <= 0xA3) return true;  // U+0800..U+08FF
        if (c == 0xEF && i + 1 < u.size()) {
            const unsigned char d = static_cast<unsigned char>(u[i + 1]);
            if (d >= 0xAC && d <= 0xBB) return true;  // presentation forms
        }
    }
    return false;
}

const std::string& visual(std::string_view logical) {
    Shaper& s = shaper();
    auto it = s.visualCache.find(std::string(logical));
    if (it != s.visualCache.end()) return it->second;
    std::string v = hasRtl(logical) ? reorder(logical) : std::string(logical);
    return s.visualCache.emplace(std::string(logical), std::move(v)).first->second;
}

int glyphForSlot(unsigned cp) {
    const Shaper& s = shaper();
    if (cp < kSlotFirst || cp > kSlotLast) return 0;
    const size_t i = cp - kSlotFirst;
    return i < s.slotGlyph.size() ? s.slotGlyph[i] : 0;
}

void frameTick() {
    if (shaper().visualCache.size() > 4096) shaper().visualCache.clear();
}

void shutdown() { shaper().release(); }

}  // namespace wasd::app::text
