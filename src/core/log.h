#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wasd::log {

enum class Level { Debug, Info, Warn, Error };

struct Entry {
    int64_t tMicros;
    Level level;
    std::string text;
};

void write(Level level, std::string text);
std::vector<Entry> recent(std::size_t max = 500);
// Mirrors all entries to a file (UTF-8). Empty path disables.
void setFile(const std::string& path);
const char* levelName(Level l);

inline void debug(std::string t) { write(Level::Debug, std::move(t)); }
inline void info(std::string t) { write(Level::Info, std::move(t)); }
inline void warn(std::string t) { write(Level::Warn, std::move(t)); }
inline void error(std::string t) { write(Level::Error, std::move(t)); }

}  // namespace wasd::log
