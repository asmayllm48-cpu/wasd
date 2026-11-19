#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wasd::platform {

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
std::wstring quoteArg(const std::wstring& a);

struct RunResult {
    bool started = false;
    uint32_t exitCode = 0;
    bool timedOut = false;
    std::string output;  // stdout + stderr
};

// Runs a process to completion with hidden window, capturing output.
RunResult runProcess(const std::wstring& exe, const std::vector<std::wstring>& args, uint32_t timeoutMs = 30000);

// Long-running child (e.g. `adb shell app_process ...`). Output lines are delivered
// on a background thread. The child is killed on destruction (job object).
class ChildProcess {
public:
    using LineFn = std::function<void(const std::string&)>;
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const std::wstring& exe, const std::vector<std::wstring>& args, LineFn onLine);
    bool running() const;
    void kill();
    void* handle() const { return process_; }

private:
    void* process_ = nullptr;
    void* job_ = nullptr;
    void* readPipe_ = nullptr;
    void* reader_ = nullptr;  // thread handle
    LineFn onLine_;
    static unsigned long __stdcall readerMain(void* self);
};

}  // namespace wasd::platform
