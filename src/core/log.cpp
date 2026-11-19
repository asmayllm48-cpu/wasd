#include "core/log.h"

#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>

#include "core/stats.h"

namespace wasd::log {
namespace {
std::mutex g_mu;
std::deque<Entry> g_entries;
std::ofstream g_file;
constexpr std::size_t kMaxEntries = 2000;
}  // namespace

const char* levelName(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
    }
    return "?";
}

void write(Level level, std::string text) {
    const int64_t t = nowMicros();
    std::lock_guard lk(g_mu);
    if (g_file.is_open()) {
        g_file << t << ' ' << levelName(level) << ' ' << text << '\n';
        g_file.flush();
    }
    std::fprintf(stderr, "[%s] %s\n", levelName(level), text.c_str());
    g_entries.push_back({t, level, std::move(text)});
    while (g_entries.size() > kMaxEntries) g_entries.pop_front();
}

std::vector<Entry> recent(std::size_t max) {
    std::lock_guard lk(g_mu);
    const std::size_t n = std::min(max, g_entries.size());
    return {g_entries.end() - static_cast<std::ptrdiff_t>(n), g_entries.end()};
}

void setFile(const std::string& path) {
    std::lock_guard lk(g_mu);
    if (g_file.is_open()) g_file.close();
    if (!path.empty()) g_file.open(path, std::ios::app | std::ios::binary);
}

}  // namespace wasd::log
