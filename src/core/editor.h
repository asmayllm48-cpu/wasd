// Editor-only state helpers for the key-mapping UI: key capture filtering and
// undo/redo snapshots. They never touch the input engine or the profile format.
#pragma once
#include <cstddef>
#include <utility>
#include <vector>

#include "core/profile.h"

namespace wasd {

// Decides which Win32 key/mouse event becomes a binding while the editor waits for input.
// Modifiers alone are accepted on release (so Shift can be bound, but Shift+F binds F).
class KeyCapture {
public:
    enum class Result { None, Assigned, Cancelled };

    void begin() { active_ = true, pendingModifier_ = 0; }
    void end() { active_ = false, pendingModifier_ = 0; }
    bool active() const { return active_; }

    static bool isModifier(int vk) {
        return vk == 0x10 || vk == 0x11 || vk == 0x12 || (vk >= 0xA0 && vk <= 0xA5);
    }
    // Keys the editor keeps for itself or that cannot be bound independently.
    static bool isReserved(int vk) {
        return vk <= 0 || vk > 0xFE || vk == 0x70 /*F1*/ || vk == 0x71 /*F2*/ || vk == 0x5B || vk == 0x5C /*Win*/ ||
               vk == 0x5D /*Apps*/ || vk == 0x90 /*NumLock*/ || vk == 0x91 /*Scroll*/;
    }

    Result onKey(int vk, bool down, bool repeat, int& out) {
        if (!active_) return Result::None;
        if (down) {
            if (repeat) return Result::None;
            if (vk == 0x1B) return end(), Result::Cancelled;  // Esc
            if (isReserved(vk)) return Result::None;
            if (isModifier(vk)) {
                pendingModifier_ = vk;
                return Result::None;
            }
            out = vk;
            end();
            return Result::Assigned;
        }
        if (pendingModifier_ && vk == pendingModifier_) {
            out = vk;
            end();
            return Result::Assigned;
        }
        return Result::None;
    }

    Result onMouseButton(int vk, int& out) {
        if (!active_) return Result::None;
        out = vk;
        end();
        return Result::Assigned;
    }

private:
    bool active_ = false;
    int pendingModifier_ = 0;
};

// Snapshot-based undo/redo. `commit` records a finished edit gesture (drag, key change, ...).
class EditHistory {
public:
    explicit EditHistory(std::size_t limit = 100) : limit_(limit) {}

    void reset(const Profile& p) {
        base_ = p;
        undo_.clear();
        redo_.clear();
    }
    const Profile& base() const { return base_; }
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

    bool commit(const Profile& p) {
        if (p == base_) return false;
        undo_.push_back(std::move(base_));
        if (undo_.size() > limit_) undo_.erase(undo_.begin());
        base_ = p;
        redo_.clear();
        return true;
    }
    bool undo(Profile& out) {
        if (undo_.empty()) return false;
        redo_.push_back(std::move(base_));
        base_ = std::move(undo_.back());
        undo_.pop_back();
        out = base_;
        return true;
    }
    bool redo(Profile& out) {
        if (redo_.empty()) return false;
        undo_.push_back(std::move(base_));
        base_ = std::move(redo_.back());
        redo_.pop_back();
        out = base_;
        return true;
    }

private:
    std::size_t limit_;
    Profile base_;
    std::vector<Profile> undo_, redo_;
};

// Rejects names that would escape the profiles folder or are not valid file names.
inline bool isValidProfileName(const std::string& n) {
    if (n.empty() || n.size() > 60 || n.front() == '.' || n.front() == ' ' || n.back() == ' ' || n.back() == '.') return false;
    for (unsigned char c : n)
        if (c < 32 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    return true;
}

}  // namespace wasd
