#include "platform/process.h"

#include <windows.h>

namespace wasd::platform {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++bs;
            continue;
        }
        if (c == L'"') r.append(bs * 2 + 1, L'\\');
        else r.append(bs, L'\\');
        bs = 0;
        r.push_back(c);
    }
    r.append(bs * 2, L'\\');
    r.push_back(L'"');
    return r;
}

static std::wstring buildCmd(const std::wstring& exe, const std::vector<std::wstring>& args) {
    std::wstring cmd = quoteArg(exe);
    for (const auto& a : args) cmd += L" " + quoteArg(a);
    return cmd;
}

static bool spawn(const std::wstring& exe, const std::vector<std::wstring>& args, HANDLE& proc, HANDLE& readPipe, HANDLE job = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE wr = nullptr;
    if (!CreatePipe(&readPipe, &wr, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = buildCmd(exe, args);
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(readPipe);
        readPipe = nullptr;
        return false;
    }
    CloseHandle(pi.hThread);
    proc = pi.hProcess;
    if (job) AssignProcessToJobObject(job, proc);  // before resuming, so children are in the job too
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, pi.dwThreadId);
    if (th) {
        ResumeThread(th);
        CloseHandle(th);
    }
    return true;
}

RunResult runProcess(const std::wstring& exe, const std::vector<std::wstring>& args, uint32_t timeoutMs) {
    RunResult r;
    HANDLE proc = nullptr, rd = nullptr;
    if (!spawn(exe, args, proc, rd)) return r;
    r.started = true;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD n = 0;
            if (!ReadFile(rd, buf, sizeof buf, &n, nullptr) || n == 0) break;
            r.output.append(buf, n);
        }
        if (WaitForSingleObject(proc, 10) == WAIT_OBJECT_0) {
            DWORD n = 0;
            while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) r.output.append(buf, n);
            break;
        }
        if (GetTickCount64() > deadline) {
            TerminateProcess(proc, 1);
            r.timedOut = true;
            break;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    r.exitCode = code;
    CloseHandle(proc);
    CloseHandle(rd);
    return r;
}

ChildProcess::~ChildProcess() { kill(); }

bool ChildProcess::start(const std::wstring& exe, const std::vector<std::wstring>& args, LineFn onLine) {
    kill();
    onLine_ = std::move(onLine);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    HANDLE proc = nullptr, rd = nullptr;
    if (!spawn(exe, args, proc, rd, job)) {
        if (job) CloseHandle(job);
        return false;
    }
    process_ = proc;
    job_ = job;
    readPipe_ = rd;
    reader_ = CreateThread(nullptr, 0, &ChildProcess::readerMain, this, 0, nullptr);
    return true;
}

unsigned long __stdcall ChildProcess::readerMain(void* p) {
    auto* self = static_cast<ChildProcess*>(p);
    std::string line;
    char buf[1024];
    DWORD n = 0;
    while (ReadFile(self->readPipe_, buf, sizeof buf, &n, nullptr) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (self->onLine_) self->onLine_(line);
                line.clear();
            } else {
                line.push_back(buf[i]);
            }
        }
    }
    if (!line.empty() && self->onLine_) self->onLine_(line);
    return 0;
}

bool ChildProcess::running() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

void ChildProcess::kill() {
    if (process_) {
        TerminateProcess(process_, 0);
        WaitForSingleObject(process_, 2000);
    }
    if (reader_) {
        WaitForSingleObject(reader_, 2000);
        CloseHandle(reader_);
        reader_ = nullptr;
    }
    if (readPipe_) CloseHandle(readPipe_), readPipe_ = nullptr;
    if (process_) CloseHandle(process_), process_ = nullptr;
    if (job_) CloseHandle(job_), job_ = nullptr;
}

}  // namespace wasd::platform
