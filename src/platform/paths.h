#pragma once
#include <string>

namespace wasd::platform {
std::wstring exeDir();                 // directory of the running executable
std::wstring runtimeDir();             // <exeDir>\runtime (adb, scrcpy-server)
std::wstring appDataDir();             // %APPDATA%\WASD (created)
std::wstring profilesDir();            // %APPDATA%\WASD\profiles (created)
std::wstring logsDir();                // %APPDATA%\WASD\logs (created)
std::string sha256File(const std::wstring& path);  // lowercase hex, empty on error
}  // namespace wasd::platform
