// Win32 entry point: window, Raw Input registration, ImGui setup and an event-driven
// loop that wakes on window messages or on a newly decoded frame.
#include <windows.h>
#include <mmsystem.h>

#include <string>

#include "app/app.h"
#include "app/resource.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "winmm.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static wasd::app::App* g_app = nullptr;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Gameplay mouse/keyboard input belongs exclusively to App/InputEngine.
    // Feeding it to ImGui as well lets fire clicks activate side-rail buttons
    // whenever the cursor drifts over that part of the window.
    const bool lockedInput = g_app && g_app->inputLocked() &&
                             ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                              (msg >= WM_KEYFIRST && msg <= WM_KEYLAST));
    if (!lockedInput && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    LRESULT r = 0;
    if (g_app && g_app->handleMessage(msg, wp, lp, r)) return r;
    if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_KEYMENU) return 0;  // Alt must not open the menu
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // System tuning for low-latency input: 1 ms timer resolution and high process priority.
    // These make recoil/return/auto-fire/camera-smoothing ticks land on a steady 1 ms grid
    // instead of the default ~15.6 ms Windows timer, and keep the process responsive under
    // sustained load (the exact condition where the user reported lag during firefights).
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetProcessPriorityBoost(GetCurrentProcess(), FALSE);  // keep boost for input bursts
    // Disable Win11 EcoQoS / power throttling for this process so background activity
    // (video decode, automation) does not get silently down-clocked.
    PROCESS_POWER_THROTTLING_STATE pts{};
    pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pts.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pts.StateMask = 0;  // 0 => throttle off
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pts, sizeof(pts));
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_NEXPLAY), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_NEXPLAY), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"WasdMainWindow";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"NEXPLAY", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1600, 900, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    if (wc.hIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    if (wc.hIconSm) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

    // Mouse raw input is NOT registered on the main window: the dedicated input
    // thread owns a message-only RIDEV_INPUTSINK window that receives all raw
    // mouse deltas, so this thread's queue is never flooded by 1000+ Hz input.
    // (Keyboard/mouse-button legacy messages still arrive normally here.)

    wasd::app::App app;
    std::string err;
    if (!app.init(hwnd, err)) {
        MessageBoxA(nullptr, err.c_str(), "NEXPLAY", MB_ICONERROR);
        return 1;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(app.rendererDevice(), app.rendererContext());
    app.initUi();
    g_app = &app;
    ShowWindow(hwnd, show);

    bool running = true;
    while (running) {
        HANDLE ev = app.frameEvent();
        // Wake immediately on a new video frame; otherwise redraw the UI at most ~240 Hz.
        // The 4 ms wake also drives InputEngine::tick() -> recoil/return/auto-fire
        // emit in smaller, steadier quanta instead of one lump step per video frame.
        MsgWaitForMultipleObjectsEx(1, &ev, app.sessionActive() ? 4 : 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        app.frame();
    }
    g_app = nullptr;
    app.shutdown();  // releases all fingers and stops the session before teardown
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    timeEndPeriod(1);
    return 0;
}
