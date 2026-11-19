#include "platform/paths.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <cstdio>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace wasd::platform {

std::wstring exeDir() {
    std::wstring p(MAX_PATH, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size()));
    p.resize(n);
    auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::wstring runtimeDir() { return exeDir() + L"\\runtime"; }

static std::wstring ensure(const std::wstring& d) {
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::wstring appDataDir() {
    PWSTR p = nullptr;
    std::wstring base = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) base = p;
    CoTaskMemFree(p);
    return ensure(base + L"\\WASD");
}

std::wstring profilesDir() { return ensure(appDataDir() + L"\\profiles"); }
std::wstring logsDir() { return ensure(appDataDir() + L"\\logs"); }

std::string sha256File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 16);
        DWORD n = 0;
        bool ok = true;
        while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0)
            if (BCryptHashData(h, buf.data(), n, 0) != 0) ok = false;
        unsigned char digest[32];
        if (ok && BCryptFinishHash(h, digest, sizeof digest, 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
            out.assign(hex, 64);
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

}  // namespace wasd::platform
