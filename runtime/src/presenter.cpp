// Shows finished frames. Windows: the game window (windowed, frameless or
// fullscreen; Direct3D 12 draws into it).
// Elsewhere: writes every 30th frame to <data>/frames/frame_NNNNN.ppm so the
// output can be checked without a display.
#include "gpu.h"
#include "kernel.h"
#include "d3d12/d3d12_backend.h"
#include "settings.h"
#include "input.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace gpu
{
    namespace
    {
        std::mutex g_frameMutex;
        std::vector<uint32_t> g_frame;
        uint32_t g_frameW = 0, g_frameH = 0;
        std::string g_dumpDir;
        uint64_t g_presented = 0;
        std::atomic<bool> g_external{ false };
        // Shown in the window before the first frame (e.g. "Preparing shaders").
        std::mutex g_statusMutex;
        std::wstring g_status;

#ifdef _WIN32
        HWND volatile g_hwnd = nullptr;
        bool g_fullscreen = false;
        RECT g_windowedRect = {};
        // Style when not fullscreen: a normal window, or a frameless one
        // (display mode "borderless window").
        DWORD g_windowStyle = WS_OVERLAPPEDWINDOW;

        // Borderless fullscreen on the window's monitor, or back to a window.
        void SetFullscreen(HWND hwnd, bool on)
        {
            if (on == g_fullscreen)
                return;
            g_fullscreen = on;
            if (on)
            {
                GetWindowRect(hwnd, &g_windowedRect);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
                SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            }
            else
            {
                SetWindowLongPtrW(hwnd, GWL_STYLE, g_windowStyle | WS_VISIBLE);
                SetWindowPos(hwnd, nullptr, g_windowedRect.left, g_windowedRect.top, g_windowedRect.right - g_windowedRect.left,
                             g_windowedRect.bottom - g_windowedRect.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
            }
        }

        // Applies the display mode and window size from the settings (in-game
        // menu): fullscreen, or a window of that client size centred on the
        // window's monitor.
        void ApplyDisplay(HWND hwnd)
        {
            const auto& st = settings::Get();
            bool borderless = st.displayMode == settings::DisplayMode::BorderlessWindow;
            g_windowStyle = borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
            if (st.displayMode == settings::DisplayMode::Fullscreen)
            {
                SetFullscreen(hwnd, true);
                return;
            }
            g_fullscreen = false;
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            RECT area = borderless ? mi.rcMonitor : mi.rcWork;
            RECT r = { 0, 0, LONG(st.windowWidth), LONG(st.windowHeight) };
            AdjustWindowRect(&r, g_windowStyle, FALSE);
            int w = r.right - r.left, h = r.bottom - r.top;
            int x = area.left + std::max(0, int(area.right - area.left - w) / 2);
            int y = area.top + std::max(0, int(area.bottom - area.top - h) / 2);
            SetWindowLongPtrW(hwnd, GWL_STYLE, g_windowStyle | WS_VISIBLE);
            SetWindowPos(hwnd, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            g_windowedRect = { x, y, x + w, y + h };
        }

        LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            switch (msg)
            {
            case WM_PAINT:
            {
                std::wstring status;
                {
                    std::lock_guard lock(g_statusMutex);
                    status = g_status;
                }
                if (!status.empty())
                {
                    PAINTSTRUCT ps;
                    HDC dc = BeginPaint(hwnd, &ps);
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    FillRect(dc, &rc, HBRUSH(GetStockObject(BLACK_BRUSH)));
                    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
                    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
                    ncm.lfMessageFont.lfHeight = -std::max<LONG>(16, rc.bottom / 30);
                    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
                    HGDIOBJ old = SelectObject(dc, font);
                    SetBkMode(dc, TRANSPARENT);
                    SetTextColor(dc, RGB(200, 200, 200));
                    DrawTextW(dc, status.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(dc, old);
                    DeleteObject(font);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                if (g_external)
                {
                    // The Direct3D 12 swap chain draws the window.
                    ValidateRect(hwnd, nullptr);
                    return 0;
                }
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                RECT rc;
                GetClientRect(hwnd, &rc);
                std::lock_guard lock(g_frameMutex);
                if (g_frameW && g_frameH)
                {
                    BITMAPINFO bi = {};
                    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                    bi.bmiHeader.biWidth = LONG(g_frameW);
                    bi.bmiHeader.biHeight = -LONG(g_frameH); // top-down
                    bi.bmiHeader.biPlanes = 1;
                    bi.bmiHeader.biBitCount = 32;
                    bi.bmiHeader.biCompression = BI_RGB;
                    // Keep the aspect ratio; letterbox the rest.
                    int cw = rc.right, ch = rc.bottom;
                    int w = cw, h = int(int64_t(cw) * g_frameH / g_frameW);
                    if (h > ch) { h = ch; w = int(int64_t(ch) * g_frameW / g_frameH); }
                    int x = (cw - w) / 2, y = (ch - h) / 2;
                    PatBlt(dc, 0, 0, cw, y, BLACKNESS);
                    PatBlt(dc, 0, y + h, cw, ch - y - h, BLACKNESS);
                    PatBlt(dc, 0, y, x, h, BLACKNESS);
                    PatBlt(dc, x + w, y, cw - x - w, h, BLACKNESS);
                    SetStretchBltMode(dc, HALFTONE);
                    StretchDIBits(dc, x, y, w, h, 0, 0, int(g_frameW), int(g_frameH), g_frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
                }
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_INPUT:
            {
                // Raw mouse movement for aiming (not limited by the screen edge).
                RAWINPUT ri;
                UINT size = sizeof(ri);
                if (GetRawInputData(HRAWINPUT(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                    ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                    input::OnMouseMove(ri.data.mouse.lLastX, ri.data.mouse.lLastY);
                break;
            }
            case WM_ACTIVATE:
                // Hide and confine the cursor while the game has focus.
                input::SetCapture(LOWORD(wp) != WA_INACTIVE && !HIWORD(wp));
                if (LOWORD(wp) == WA_INACTIVE)
                    input::StopVibration(); // don't keep shaking in the background
                break;
            case WM_SIZE:
            case WM_MOVE:
                if (GetForegroundWindow() == hwnd)
                {
                    input::SetCapture(false);
                    input::SetCapture(true);
                }
                break;
            case WM_SYSKEYDOWN:
                if (wp == VK_RETURN && (lp & (1 << 29)))
                {
                    SetFullscreen(hwnd, !g_fullscreen); // Alt+Enter
                    return 0;
                }
                break;
            case WM_SYSCHAR:
                if (wp == VK_RETURN)
                    return 0; // no beep for Alt+Enter
                break;
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE && !(lp & (1 << 30))) // not auto-repeat
                {
                    // The game itself only reads the controller, so Esc is ours:
                    // the menu (settings, controls, quit).
                    static bool open = false;
                    if (!open)
                    {
                        open = true;
                        input::SetCapture(false);
                        input::StopVibration();
                        settings::MenuResult m = settings::RunInGameMenu(hwnd);
                        open = false;
                        if (m.quit)
                        {
                            LOG("quit from the Esc menu - exiting");
                            input::StopVibration();
                            std::_Exit(0);
                        }
                        if (m.displayChanged)
                            ApplyDisplay(hwnd);
                        if (m.frameRateChanged)
                            d3d12::SetFrameRate(settings::Get().frameRate);
                        if (m.restartNeeded)
                            MessageBoxW(hwnd, L"The new render resolution, aspect ratio or player name will be used the next time you start the game.",
                                        L"Vampire Rain", MB_OK | MB_ICONINFORMATION);
                        input::SetCapture(GetForegroundWindow() == hwnd);
                    }
                    return 0;
                }
                if (wp == VK_F12)
                {
                    LOG("[capture] F12 pressed - logging the next frame");
                    d3d12::RequestCapture();
                }
                if (wp == VK_F11 && !(lp & (1 << 30)))
                {
                    LOG("[mods] F11 pressed - saving the next frame's textures");
                    d3d12::RequestTextureDump();
                }
                if (wp == VK_F10 && !(lp & (1 << 30)))
                {
                    LOG("[mods] F10 pressed - reloading replacement textures");
                    d3d12::ReloadModTextures();
                }
                break;
            case WM_CLOSE:
                LOG("window closed - exiting");
                input::StopVibration();
                std::_Exit(0);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        void WindowThread()
        {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1)); // the game's icon, if the build has one
            wc.lpszClassName = L"VampireRain";
            RegisterClassW(&wc);
            const auto& st = settings::Get();
            bool borderless = st.displayMode == settings::DisplayMode::BorderlessWindow;
            g_windowStyle = borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
            RECT r = { 0, 0, LONG(st.windowWidth), LONG(st.windowHeight) };
            AdjustWindowRect(&r, g_windowStyle, FALSE);
            int w = r.right - r.left, h = r.bottom - r.top;
            // Centred on the primary monitor (its work area for a framed
            // window; the whole screen for a frameless one, like a game).
            RECT area;
            if (borderless)
                area = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
            else
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
            int x = area.left + std::max(0, int(area.right - area.left - w) / 2);
            int y = area.top + std::max(0, int(area.bottom - area.top - h) / 2);
            HWND hwnd = CreateWindowW(L"VampireRain", L"Vampire Rain", g_windowStyle, x, y, w, h,
                                      nullptr, nullptr, wc.hInstance, nullptr);
            if (st.displayMode == settings::DisplayMode::Fullscreen)
                SetFullscreen(hwnd, true);
            else
                ShowWindow(hwnd, SW_SHOW);
            g_hwnd = hwnd;
            input::SetGameWindow(hwnd);
            RAWINPUTDEVICE rid = { 0x01, 0x02, 0, hwnd }; // generic desktop: mouse
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
            input::SetCapture(GetForegroundWindow() == hwnd);
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
#endif

        void WritePpm(const std::string& path, const uint32_t* bgra, uint32_t w, uint32_t h)
        {
            FILE* f = fopen(path.c_str(), "wb");
            if (!f)
                return;
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            std::vector<uint8_t> row(size_t(w) * 3);
            for (uint32_t y = 0; y < h; y++)
            {
                for (uint32_t x = 0; x < w; x++)
                {
                    uint32_t p = bgra[size_t(y) * w + x];
                    row[x * 3 + 0] = uint8_t(p >> 16);
                    row[x * 3 + 1] = uint8_t(p >> 8);
                    row[x * 3 + 2] = uint8_t(p);
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
        }
    }

    void PresenterInit(const char* dataDir)
    {
#ifdef _WIN32
        (void)dataDir;
        std::thread(WindowThread).detach();
#else
        g_dumpDir = std::string(dataDir) + "/frames";
        std::error_code ec;
        std::filesystem::create_directories(g_dumpDir, ec);
        LOG("[gpu] no window on this platform; frames are saved to %s", g_dumpDir.c_str());
#endif
    }

    void* PresenterWindow()
    {
#ifdef _WIN32
        // The window is created on its own thread; wait for it briefly.
        for (int i = 0; i < 500 && !g_hwnd; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return g_hwnd;
#else
        return nullptr;
#endif
    }

    void PresenterSetExternal(bool on) { g_external = on; }

    void PresenterSetStatus(const wchar_t* text)
    {
#ifdef _WIN32
        {
            std::lock_guard lock(g_statusMutex);
            g_status = text ? text : L"";
        }
        if (g_hwnd)
            InvalidateRect(g_hwnd, nullptr, FALSE);
#else
        (void)text;
#endif
    }

    void Present(const uint32_t* bgra, uint32_t width, uint32_t height)
    {
        g_presented++;
        {
            std::lock_guard lock(g_frameMutex);
            g_frame.assign(bgra, bgra + size_t(width) * height);
            g_frameW = width;
            g_frameH = height;
        }
#ifdef _WIN32
        if (g_hwnd)
            InvalidateRect(g_hwnd, nullptr, FALSE);
#else
        if (g_presented <= 12 || g_presented % 30 == 1)
        {
            char name[64];
            snprintf(name, sizeof(name), "/frame_%05llu.ppm", (unsigned long long)g_presented);
            WritePpm(g_dumpDir + name, bgra, width, height);
        }
#endif
    }
}
