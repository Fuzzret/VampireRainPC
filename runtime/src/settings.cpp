// Settings file and the launcher window (plain Win32 controls).
#include "settings.h"
#include "kernel.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace settings
{
    namespace
    {
        Settings g_settings;
        std::string g_path;
    }

    Settings& Get() { return g_settings; }

    namespace
    {
        constexpr int kAspectCount = 6;
        const char* const kAspectNames[kAspectCount] = { "auto", "4:3", "16:10", "16:9", "21:9", "32:9" };
    }

    float RenderAspect()
    {
        switch (g_settings.aspectRatio)
        {
        case AspectRatio::R4x3: return 4.0f / 3.0f;
        case AspectRatio::R16x10: return 16.0f / 10.0f;
        case AspectRatio::R21x9: return 21.0f / 9.0f;
        case AspectRatio::R32x9: return 32.0f / 9.0f;
        case AspectRatio::Auto:
        {
            float w = float(g_settings.windowWidth), h = float(g_settings.windowHeight);
#ifdef _WIN32
            if (g_settings.displayMode == DisplayMode::Fullscreen)
            {
                w = float(GetSystemMetrics(SM_CXSCREEN));
                h = float(GetSystemMetrics(SM_CYSCREEN));
            }
#endif
            return h > 0 ? std::clamp(w / h, 1.2f, 3.6f) : 16.0f / 9.0f;
        }
        default: return 16.0f / 9.0f;
        }
    }

    void Load(const std::string& dataDir)
    {
        std::error_code ec;
        std::filesystem::create_directories(dataDir, ec);
        g_path = (std::filesystem::path(dataDir) / "settings.ini").string();
        std::ifstream f(g_path);
        std::string line;
        while (std::getline(f, line))
        {
            auto eq = line.find('=');
            if (eq == std::string::npos || line[0] == ';' || line[0] == '[')
                continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            while (!k.empty() && k.back() == ' ') k.pop_back();
            while (!v.empty() && (v.front() == ' ')) v.erase(v.begin());
            uint32_t n = uint32_t(strtoul(v.c_str(), nullptr, 10));
            if (k == "window_width") g_settings.windowWidth = std::clamp<uint32_t>(n, 640, 16384);
            else if (k == "window_height") g_settings.windowHeight = std::clamp<uint32_t>(n, 360, 16384);
            else if (k == "fullscreen") g_settings.displayMode = n ? DisplayMode::Fullscreen : DisplayMode::Windowed; // older files
            else if (k == "display_mode")
                g_settings.displayMode = v == "fullscreen" ? DisplayMode::Fullscreen : v == "borderless" ? DisplayMode::BorderlessWindow : DisplayMode::Windowed;
            else if (k == "vibration") g_settings.vibration = std::min<uint32_t>(n, 100);
            else if (k == "aspect_ratio")
            {
                for (int i = 0; i < kAspectCount; i++)
                    if (v == kAspectNames[i])
                        g_settings.aspectRatio = AspectRatio(i);
            }
            else if (k == "vsync") g_settings.vsync = n != 0;
            else if (k == "render_scale") g_settings.renderScale = std::clamp<uint32_t>(n, 1, 4);
            else if (k == "show_launcher") g_settings.showLauncher = n != 0;
            else if (k == "anti_aliasing") g_settings.antiAliasing = std::min<uint32_t>(n, 1);
            else if (k == "mouse_look") g_settings.mouseLook = n != 0;
            else if (k == "mouse_sensitivity") g_settings.mouseSensitivity = std::clamp<uint32_t>(n, 1, 100);
            else if (k == "invert_y") g_settings.invertY = n != 0;
            else if (k == "mouse_curve") g_settings.mouseCurve = std::clamp<uint32_t>(n, 20, 100);
            else if (k == "direct_camera") g_settings.directCamera = n != 0;
            else if (k == "lan_test_instance") g_settings.lanTestInstance = std::min<uint32_t>(n, 4);
            else if (k == "live_sign_in") g_settings.liveSignIn = n != 0;
            else if (k == "player_name" && !v.empty()) g_settings.playerName = v.substr(0, 15);
            else if (k.rfind("bind_", 0) == 0)
            {
                for (int a = 0; a < input::ActionCount; a++)
                    if (k.substr(5) == input::ActionIniKey(a))
                    {
                        unsigned k0 = 0, k1 = 0;
                        if (sscanf(v.c_str(), "%u , %u", &k0, &k1) >= 1)
                        {
                            g_settings.keys[a][0] = uint16_t(k0 & 0xFF);
                            g_settings.keys[a][1] = uint16_t(k1 & 0xFF);
                        }
                    }
            }
            else if (k == "frame_rate")
            {
                // 30, 60, 120, 144 or "unlimited" (0)
                unsigned f = (v == "unlimited") ? 0u : unsigned(atoi(v.c_str()));
                g_settings.frameRate = (f == 0 || f == 60 || f == 120 || f == 144) ? f : 30u;
            }
        }
        char fpsText[16];
        if (g_settings.frameRate) snprintf(fpsText, sizeof(fpsText), "%u", g_settings.frameRate);
        else snprintf(fpsText, sizeof(fpsText), "unlimited");
        LOG("[settings] %ux%u %s, vsync %s, render scale %ux, anti-aliasing %s, %s fps", g_settings.windowWidth, g_settings.windowHeight,
            g_settings.displayMode == DisplayMode::Fullscreen ? "fullscreen" : g_settings.displayMode == DisplayMode::BorderlessWindow ? "borderless window" : "windowed",
            g_settings.vsync ? "on" : "off",
            g_settings.renderScale, g_settings.antiAliasing ? "FXAA" : "off", fpsText);
    }

    void Save()
    {
        if (g_path.empty())
            return;
        std::ofstream f(g_path);
        f << "; Vampire Rain PC settings (also editable in the launcher)\n"
          << "[display]\n"
          << "window_width = " << g_settings.windowWidth << "\n"
          << "window_height = " << g_settings.windowHeight << "\n"
          << "; windowed, borderless (window without a frame) or fullscreen (borderless, desktop resolution)\n"
          << "display_mode = " << (g_settings.displayMode == DisplayMode::Fullscreen ? "fullscreen" : g_settings.displayMode == DisplayMode::BorderlessWindow ? "borderless" : "windowed") << "\n"
          << "vsync = " << (g_settings.vsync ? 1 : 0) << "\n"
          << "render_scale = " << g_settings.renderScale << "\n"
          << "anti_aliasing = " << g_settings.antiAliasing << "\n"
          << "; auto (the window's or monitor's shape), 4:3, 16:10, 16:9, 21:9 or 32:9\n"
          << "aspect_ratio = " << kAspectNames[int(g_settings.aspectRatio)] << "\n"
          << "frame_rate = " << (g_settings.frameRate ? std::to_string(g_settings.frameRate) : std::string("unlimited")) << "\n"
          << "[controls]\n"
          << "; Keys are Windows virtual-key codes (two per input, 0 = none). Easiest to change in the launcher.\n"
          << "mouse_look = " << (g_settings.mouseLook ? 1 : 0) << "\n"
          << "mouse_sensitivity = " << g_settings.mouseSensitivity << "\n"
          << "invert_y = " << (g_settings.invertY ? 1 : 0) << "\n"
          << "mouse_curve = " << g_settings.mouseCurve << "\n"
          << "direct_camera = " << (g_settings.directCamera ? 1 : 0) << "\n"
          << "; controller vibration strength, 0 (off) to 100\n"
          << "vibration = " << g_settings.vibration << "\n";
        for (int a = 0; a < input::ActionCount; a++)
            f << "bind_" << input::ActionIniKey(a) << " = " << g_settings.keys[a][0] << ", " << g_settings.keys[a][1] << "\n";
        f << "[network]\n"
          << "; 0 = normal LAN play. 1-4 = testing several copies on one PC (give each copy its own --data folder).\n"
          << "lan_test_instance = " << g_settings.lanTestInstance << "\n"
          << "live_sign_in = " << (g_settings.liveSignIn ? 1 : 0) << "\n"
          << "player_name = " << g_settings.playerName << "\n";
        f << "[launcher]\n"
          << "show_launcher = " << (g_settings.showLauncher ? 1 : 0) << "\n";
    }

#ifdef _WIN32
    namespace
    {
        enum : int { IdResolution = 100, IdWindowed, IdFullscreen, IdVsync, IdScale, IdShowLauncher, IdPlay, IdQuit, IdAa, IdFps, IdControls, IdName, IdAspect };

        struct LauncherState
        {
            std::vector<std::pair<uint32_t, uint32_t>> resolutions;
            HWND resolution = nullptr, windowed = nullptr, fullscreen = nullptr, vsync = nullptr, scale = nullptr, show = nullptr;
            HWND aa = nullptr, fps = nullptr, name = nullptr, aspect = nullptr;
            HFONT font = nullptr;
            bool play = false, done = false;
            bool inGame = false, quit = false; // opened from the game (Esc); Quit pressed there
        };
        LauncherState* g_launcher = nullptr;

        std::vector<std::pair<uint32_t, uint32_t>> ListResolutions()
        {
            // Common sizes of each shape (4:3, 16:10, 16:9, 21:9), plus the monitor's own modes.
            std::set<std::pair<uint32_t, uint32_t>> modes = { { 1024, 768 }, { 1440, 1080 }, { 1680, 1050 }, { 1920, 1200 },
                                                              { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
                                                              { 2560, 1080 }, { 3440, 1440 } };
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++)
                if (dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480)
                    modes.insert({ dm.dmPelsWidth, dm.dmPelsHeight });
            // Only sizes that fit on the desktop work as windows.
            RECT work;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            std::vector<std::pair<uint32_t, uint32_t>> out;
            for (auto& m : modes)
                if (m.first <= uint32_t(work.right - work.left) && m.second <= uint32_t(work.bottom - work.top))
                    out.push_back(m);
            if (out.empty())
                out.push_back({ 1280, 720 });
            return out;
        }

        // ---- Controls window (key bindings, mouse) ----
        enum : int { IdList = 300, IdSet1, IdSet2, IdClear, IdReset, IdMouseLook, IdSens, IdSensLabel, IdInvert, IdOk, IdCancel, IdCurve, IdCurveLabel, IdDirect, IdVibration, IdVibrationLabel };
        struct ControlsState
        {
            uint16_t keys[input::ActionCount][2];
            HWND list = nullptr, mouseLook = nullptr, sens = nullptr, sensLabel = nullptr, invert = nullptr;
            HWND curve = nullptr, curveLabel = nullptr, direct = nullptr, vibration = nullptr, vibrationLabel = nullptr;
            int captureRow = -1, captureSlot = 0;
            bool ok = false, done = false;
        };
        ControlsState* g_controls = nullptr;

        void RefreshRow(ControlsState& c, int row)
        {
            for (int slot = 0; slot < 2; slot++)
            {
                std::string name = (c.captureRow == row && c.captureSlot == slot) ? "Press a key or mouse button..."
                                                                                   : input::KeyName(c.keys[row][slot]);
                std::wstring w(name.begin(), name.end());
                LVITEMW it = {};
                it.iSubItem = slot + 1;
                it.pszText = w.data();
                SendMessageW(c.list, LVM_SETITEMTEXTW, WPARAM(row), LPARAM(&it));
            }
        }

        void UpdateSensLabel(ControlsState& c)
        {
            wchar_t b[32];
            swprintf(b, 32, L"%d", int(SendMessageW(c.sens, TBM_GETPOS, 0, 0)));
            SetWindowTextW(c.sensLabel, b);
            if (c.curve)
            {
                swprintf(b, 32, L"%d", int(SendMessageW(c.curve, TBM_GETPOS, 0, 0)));
                SetWindowTextW(c.curveLabel, b);
            }
            if (c.vibration)
            {
                int v = int(SendMessageW(c.vibration, TBM_GETPOS, 0, 0));
                if (v) swprintf(b, 32, L"%d%%", v); else swprintf(b, 32, L"Off");
                SetWindowTextW(c.vibrationLabel, b);
            }
        }

        void StartCapture(ControlsState& c, int slot)
        {
            int row = int(SendMessageW(c.list, LVM_GETNEXTITEM, WPARAM(-1), LVNI_SELECTED));
            if (row < 0)
                return;
            int old = c.captureRow;
            c.captureRow = row;
            c.captureSlot = slot;
            if (old >= 0 && old != row)
                RefreshRow(c, old);
            RefreshRow(c, row);
            SetFocus(c.list);
        }

        // Handles a key/mouse press while waiting for a binding. Returns true if used.
        bool CaptureInput(ControlsState& c, const MSG& m)
        {
            if (c.captureRow < 0)
                return false;
            uint16_t vk = 0;
            switch (m.message)
            {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            {
                vk = uint16_t(m.wParam);
                UINT scan = (UINT(m.lParam) >> 16) & 0xFF;
                bool ext = (m.lParam >> 24) & 1;
                if (vk == VK_SHIFT) vk = uint16_t(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX));
                else if (vk == VK_CONTROL) vk = ext ? VK_RCONTROL : VK_LCONTROL;
                else if (vk == VK_MENU) vk = ext ? VK_RMENU : VK_LMENU;
                if (vk == VK_ESCAPE) vk = 0xFFFF;          // cancel (Esc is the quit key in game)
                else if (vk == VK_BACK || vk == VK_DELETE) vk = 0xFFFE; // clear
                break;
            }
            case WM_LBUTTONDOWN: vk = VK_LBUTTON; break;
            case WM_RBUTTONDOWN: vk = VK_RBUTTON; break;
            case WM_MBUTTONDOWN: vk = VK_MBUTTON; break;
            case WM_XBUTTONDOWN: vk = GET_XBUTTON_WPARAM(m.wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2; break;
            default: return false;
            }
            int row = c.captureRow;
            if (vk == 0xFFFE)
                c.keys[row][c.captureSlot] = 0;
            else if (vk != 0xFFFF && vk)
            {
                // A key does one thing: take it off whatever else had it.
                for (int a = 0; a < input::ActionCount; a++)
                    for (int sl = 0; sl < 2; sl++)
                        if (c.keys[a][sl] == vk && (a != row || sl != c.captureSlot))
                        {
                            c.keys[a][sl] = 0;
                            RefreshRow(c, a);
                        }
                c.keys[row][c.captureSlot] = vk;
            }
            c.captureRow = -1;
            RefreshRow(c, row);
            return true;
        }

        LRESULT CALLBACK ControlsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            ControlsState& c = *g_controls;
            switch (msg)
            {
            case WM_COMMAND:
                switch (LOWORD(wp))
                {
                case IdSet1: StartCapture(c, 0); break;
                case IdSet2: StartCapture(c, 1); break;
                case IdClear:
                {
                    int row = int(SendMessageW(c.list, LVM_GETNEXTITEM, WPARAM(-1), LVNI_SELECTED));
                    if (row >= 0) { c.keys[row][0] = c.keys[row][1] = 0; RefreshRow(c, row); }
                    break;
                }
                case IdReset:
                    input::DefaultBindings(c.keys);
                    c.captureRow = -1;
                    for (int a = 0; a < input::ActionCount; a++) RefreshRow(c, a);
                    break;
                case IdOk:
                {
                    Settings& st = g_settings;
                    memcpy(st.keys, c.keys, sizeof(c.keys));
                    st.mouseLook = SendMessageW(c.mouseLook, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    st.mouseSensitivity = uint32_t(SendMessageW(c.sens, TBM_GETPOS, 0, 0));
                    st.invertY = SendMessageW(c.invert, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    st.mouseCurve = uint32_t(SendMessageW(c.curve, TBM_GETPOS, 0, 0));
                    st.directCamera = SendMessageW(c.direct, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    st.vibration = uint32_t(SendMessageW(c.vibration, TBM_GETPOS, 0, 0));
                    Save();
                    c.ok = true;
                    DestroyWindow(hwnd);
                    break;
                }
                case IdCancel:
                    DestroyWindow(hwnd);
                    break;
                }
                return 0;
            case WM_NOTIFY:
            {
                auto* nm = reinterpret_cast<NMHDR*>(lp);
                if (nm->idFrom == IdList && nm->code == NM_DBLCLK)
                    StartCapture(c, reinterpret_cast<NMITEMACTIVATE*>(lp)->iSubItem == 2 ? 1 : 0);
                return 0;
            }
            case WM_HSCROLL:
                if (HWND(lp) == c.sens || HWND(lp) == c.curve || HWND(lp) == c.vibration)
                    UpdateSensLabel(c);
                return 0;
            case WM_CTLCOLORSTATIC:
                SetBkMode(HDC(wp), TRANSPARENT);
                return LRESULT(GetSysColorBrush(COLOR_WINDOW));
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                c.done = true;
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        void RunControls(HWND parent, HFONT font, int dpi)
        {
            INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
            InitCommonControlsEx(&icc);
            ControlsState c;
            memcpy(c.keys, g_settings.keys, sizeof(c.keys));
            g_controls = &c;
            auto px = [&](int v) { return MulDiv(v, dpi, 96); };
            HINSTANCE inst = GetModuleHandleW(nullptr);
            WNDCLASSW wc = {};
            wc.lpfnWndProc = ControlsProc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
            wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
            wc.lpszClassName = L"VampireRainControls";
            RegisterClassW(&wc);
            DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
            RECT rc = { 0, 0, px(470), px(620) };
            AdjustWindowRect(&rc, style, FALSE);
            RECT pr;
            GetWindowRect(parent, &pr);
            int w = rc.right - rc.left, h = rc.bottom - rc.top;
            HWND hwnd = CreateWindowW(wc.lpszClassName, L"Vampire Rain - Controls", style,
                                      (pr.left + pr.right - w) / 2, std::max(0L, (pr.top + pr.bottom - h) / 2), w, h,
                                      parent, nullptr, inst, nullptr);
            if (!hwnd)
                return;
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int cw, int ch, int id) {
                HWND ctl = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | st, px(x), px(y), px(cw), px(ch), hwnd,
                                         HMENU(INT_PTR(id)), inst, nullptr);
                SendMessageW(ctl, WM_SETFONT, WPARAM(font), TRUE);
                return ctl;
            };
            make(L"STATIC", L"Double-click an input (or select it and press Set) and then press a key or mouse button. "
                            L"Backspace clears, Esc cancels. A controller keeps working too.", 0, 16, 10, 440, 34, 0);
            c.list = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 16, 50, 438, 290, IdList);
            SendMessageW(c.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            const wchar_t* cols[] = { L"Controller input", L"Key 1", L"Key 2" };
            const int widths[] = { 170, 125, 125 };
            for (int i = 0; i < 3; i++)
            {
                LVCOLUMNW col = {};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.pszText = const_cast<wchar_t*>(cols[i]);
                col.cx = px(widths[i]);
                SendMessageW(c.list, LVM_INSERTCOLUMNW, i, LPARAM(&col));
            }
            for (int a = 0; a < input::ActionCount; a++)
            {
                std::string label = input::ActionLabel(a);
                std::wstring wl(label.begin(), label.end());
                LVITEMW it = {};
                it.mask = LVIF_TEXT;
                it.iItem = a;
                it.pszText = wl.data();
                SendMessageW(c.list, LVM_INSERTITEMW, 0, LPARAM(&it));
                RefreshRow(c, a);
            }
            make(L"BUTTON", L"Set key 1", BS_PUSHBUTTON | WS_TABSTOP, 16, 348, 100, 26, IdSet1);
            make(L"BUTTON", L"Set key 2", BS_PUSHBUTTON | WS_TABSTOP, 122, 348, 100, 26, IdSet2);
            make(L"BUTTON", L"Clear", BS_PUSHBUTTON | WS_TABSTOP, 228, 348, 100, 26, IdClear);
            make(L"BUTTON", L"Reset all", BS_PUSHBUTTON | WS_TABSTOP, 354, 348, 100, 26, IdReset);

            c.mouseLook = make(L"BUTTON", L"Aim and look with the mouse", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 382, 200, 22, IdMouseLook);
            SendMessageW(c.mouseLook, BM_SETCHECK, g_settings.mouseLook ? BST_CHECKED : BST_UNCHECKED, 0);
            c.direct = make(L"BUTTON", L"Direct camera control (1:1, recommended)", BS_AUTOCHECKBOX | WS_TABSTOP, 220, 382, 240, 22, IdDirect);
            SendMessageW(c.direct, BM_SETCHECK, g_settings.directCamera ? BST_CHECKED : BST_UNCHECKED, 0);
            make(L"STATIC", L"Off: the mouse moves the right stick (the fine-aim boost below applies only then).", 0, 16, 402, 440, 16, 0);
            make(L"STATIC", L"Mouse sensitivity", 0, 16, 430, 120, 20, 0);
            c.sens = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 140, 426, 250, 28, IdSens);
            SendMessageW(c.sens, TBM_SETRANGE, TRUE, MAKELPARAM(1, 100));
            SendMessageW(c.sens, TBM_SETPOS, TRUE, g_settings.mouseSensitivity);
            c.sensLabel = make(L"STATIC", L"", 0, 398, 430, 50, 20, IdSensLabel);
            make(L"STATIC", L"Fine-aim boost", 0, 16, 464, 120, 20, 0);
            c.curve = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 140, 460, 250, 28, IdCurve);
            SendMessageW(c.curve, TBM_SETRANGE, TRUE, MAKELPARAM(20, 100));
            SendMessageW(c.curve, TBM_SETPOS, TRUE, g_settings.mouseCurve);
            c.curveLabel = make(L"STATIC", L"", 0, 398, 464, 50, 20, IdCurveLabel);
            make(L"STATIC", L"(lower = small movements move the view more; 100 = linear)", 0, 140, 488, 310, 16, 0);
            UpdateSensLabel(c);
            c.invert = make(L"BUTTON", L"Invert vertical mouse look", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 512, 300, 22, IdInvert);
            SendMessageW(c.invert, BM_SETCHECK, g_settings.invertY ? BST_CHECKED : BST_UNCHECKED, 0);
            make(L"STATIC", L"Controller vibration", 0, 16, 548, 120, 20, 0);
            c.vibration = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 140, 544, 250, 28, IdVibration);
            SendMessageW(c.vibration, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessageW(c.vibration, TBM_SETPAGESIZE, 0, 25);
            SendMessageW(c.vibration, TBM_SETPOS, TRUE, g_settings.vibration);
            c.vibrationLabel = make(L"STATIC", L"", 0, 398, 548, 50, 20, IdVibrationLabel);
            UpdateSensLabel(c);

            make(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 258, 584, 95, 28, IdOk);
            make(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 359, 584, 95, 28, IdCancel);

            EnableWindow(parent, FALSE);
            ShowWindow(hwnd, SW_SHOW);
            MSG m;
            while (!c.done && GetMessageW(&m, nullptr, 0, 0) > 0)
            {
                if (CaptureInput(c, m))
                    continue;
                if (IsDialogMessageW(hwnd, &m))
                    continue;
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            EnableWindow(parent, TRUE);
            SetForegroundWindow(parent);
            g_controls = nullptr;
        }

        void UpdateEnabled(LauncherState& s)
        {
            // s.windowed is the display mode list: 0 windowed, 1 borderless window, 2 fullscreen.
            bool sized = SendMessageW(s.windowed, CB_GETCURSEL, 0, 0) != 2;
            EnableWindow(s.resolution, sized);
        }

        LRESULT CALLBACK LauncherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            LauncherState& s = *g_launcher;
            switch (msg)
            {
            case WM_COMMAND:
                switch (LOWORD(wp))
                {
                case IdWindowed:
                case IdFullscreen:
                    UpdateEnabled(s);
                    break;
                case IdPlay:
                {
                    Settings& st = g_settings;
                    int r = int(SendMessageW(s.resolution, CB_GETCURSEL, 0, 0));
                    if (r >= 0 && r < int(s.resolutions.size()))
                    {
                        st.windowWidth = s.resolutions[r].first;
                        st.windowHeight = s.resolutions[r].second;
                    }
                    {
                        int m = int(SendMessageW(s.windowed, CB_GETCURSEL, 0, 0));
                        st.displayMode = m == 2 ? DisplayMode::Fullscreen : m == 1 ? DisplayMode::BorderlessWindow : DisplayMode::Windowed;
                    }
                    st.vsync = SendMessageW(s.vsync, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    int sc = int(SendMessageW(s.scale, CB_GETCURSEL, 0, 0));
                    st.renderScale = uint32_t(std::clamp(sc, 0, 3) + 1);
                    st.showLauncher = SendMessageW(s.show, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    st.antiAliasing = SendMessageW(s.aa, CB_GETCURSEL, 0, 0) == 1 ? 1u : 0u;
                    st.aspectRatio = AspectRatio(std::clamp(int(SendMessageW(s.aspect, CB_GETCURSEL, 0, 0)), 0, kAspectCount - 1));
                    {
                        static const uint32_t rates[] = { 30, 60, 120, 144, 0 };
                        st.frameRate = rates[std::clamp(int(SendMessageW(s.fps, CB_GETCURSEL, 0, 0)), 0, 4)];
                    }
                    {
                        // Gamertag: up to 15 plain characters, no leading/trailing spaces.
                        wchar_t nb[32] = {};
                        GetWindowTextW(s.name, nb, 32);
                        std::string n;
                        for (wchar_t* c = nb; *c && n.size() < 15; c++)
                            if (*c >= 32 && *c < 127)
                                n += char(*c);
                        while (!n.empty() && n.back() == ' ') n.pop_back();
                        while (!n.empty() && n.front() == ' ') n.erase(n.begin());
                        if (!n.empty())
                            st.playerName = n;
                    }
                    Save();
                    s.play = true;
                    DestroyWindow(hwnd);
                    break;
                }
                case IdQuit:
                    if (s.inGame && MessageBoxW(hwnd, L"Quit Vampire Rain?\n\nProgress since your last save will be lost.", L"Vampire Rain",
                                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                        break;
                    s.quit = true;
                    DestroyWindow(hwnd);
                    break;
                case IDCANCEL: // Esc in the in-game menu: back to the game, nothing changed
                    if (s.inGame)
                        DestroyWindow(hwnd);
                    break;
                case IdControls:
                {
                    UINT dpi = 96;
                    if (HDC dc = GetDC(nullptr)) { dpi = UINT(GetDeviceCaps(dc, LOGPIXELSX)); ReleaseDC(nullptr, dc); }
                    RunControls(hwnd, s.font, int(dpi));
                    break;
                }
                }
                return 0;
            case WM_CTLCOLORSTATIC:
            {
                HDC dc = HDC(wp);
                SetBkMode(dc, TRANSPARENT);
                return LRESULT(GetSysColorBrush(COLOR_WINDOW));
            }
            case WM_DESTROY:
                s.done = true;
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    // The launcher, or (inGame) the same settings over the running game with
    // Resume / Quit game instead of Play / Quit. Returns the state when closed.
    LauncherState RunSettingsWindow(bool inGame, HWND owner)
    {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);
        LauncherState s;
        s.inGame = inGame;
        g_launcher = &s;
        s.resolutions = ListResolutions();

        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = LauncherProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"VampireRainLauncher";
        RegisterClassW(&wc);

        // Scale the layout with the display's DPI.
        UINT dpi = 96;
        if (HDC dc = GetDC(nullptr)) { dpi = UINT(GetDeviceCaps(dc, LOGPIXELSX)); ReleaseDC(nullptr, dc); }
        auto px = [&](int v) { return MulDiv(v, int(dpi), 96); };

        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT rc = { 0, 0, px(380), px(520) };
        AdjustWindowRect(&rc, style, FALSE);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        // Centred on the game window's monitor (or the primary one).
        RECT area = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        if (owner)
        {
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi))
                area = mi.rcMonitor;
        }
        HWND hwnd = CreateWindowW(wc.lpszClassName, inGame ? L"Vampire Rain - Menu" : L"Vampire Rain - Settings", style,
                                  area.left + (area.right - area.left - w) / 2, area.top + (area.bottom - area.top - h) / 2, w, h,
                                  owner, nullptr, inst, nullptr);
        if (!hwnd)
        {
            g_launcher = nullptr;
            s.play = true;
            return s;
        }
        if (owner)
            EnableWindow(owner, FALSE); // modal over the game

        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfHeight = -px(12);
        s.font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW titleFont = ncm.lfMessageFont;
        titleFont.lfHeight = -px(17);
        titleFont.lfWeight = FW_SEMIBOLD;
        HFONT bigFont = CreateFontIndirectW(&titleFont);

        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int cw, int ch, int id, HFONT f = nullptr) {
            HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | st, px(x), px(y), px(cw), px(ch), hwnd,
                                   HMENU(INT_PTR(id)), inst, nullptr);
            SendMessageW(c, WM_SETFONT, WPARAM(f ? f : s.font), TRUE);
            return c;
        };
        const Settings& st = g_settings;

        make(L"STATIC", inGame ? L"Settings" : L"Display settings", 0, 20, 14, 340, 26, 0, bigFont);

        make(L"STATIC", L"Display mode", 0, 20, 52, 120, 20, 0);
        s.windowed = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_GROUP, 150, 48, 210, 200, IdWindowed);
        SendMessageW(s.windowed, CB_ADDSTRING, 0, LPARAM(L"Windowed"));
        SendMessageW(s.windowed, CB_ADDSTRING, 0, LPARAM(L"Borderless window"));
        SendMessageW(s.windowed, CB_ADDSTRING, 0, LPARAM(L"Fullscreen (borderless)"));
        SendMessageW(s.windowed, CB_SETCURSEL, st.displayMode == DisplayMode::Fullscreen ? 2 : st.displayMode == DisplayMode::BorderlessWindow ? 1 : 0, 0);

        make(L"STATIC", L"Window size", 0, 20, 88, 120, 20, 0);
        s.resolution = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, 150, 84, 210, 300, IdResolution);
        int sel = -1;
        for (size_t i = 0; i < s.resolutions.size(); i++)
        {
            wchar_t b[32];
            swprintf(b, 32, L"%u x %u", s.resolutions[i].first, s.resolutions[i].second);
            SendMessageW(s.resolution, CB_ADDSTRING, 0, LPARAM(b));
            if (s.resolutions[i].first == st.windowWidth && s.resolutions[i].second == st.windowHeight)
                sel = int(i);
        }
        if (sel < 0)
            for (size_t i = 0; i < s.resolutions.size(); i++)
                if (s.resolutions[i] == std::make_pair(1280u, 720u))
                    sel = int(i);
        SendMessageW(s.resolution, CB_SETCURSEL, std::max(sel, 0), 0);
        make(L"STATIC", L"Fullscreen uses your desktop resolution.", 0, 150, 112, 215, 18, 0);

        make(L"STATIC", L"Render resolution", 0, 20, 144, 125, 20, 0);
        s.scale = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 150, 140, 210, 200, IdScale);
        const wchar_t* scales[] = { L"1280 x 720 (original)", L"2560 x 1440 (2x)", L"3840 x 2160 (3x)", L"5120 x 2880 (4x)" };
        for (auto* t : scales)
            SendMessageW(s.scale, CB_ADDSTRING, 0, LPARAM(t));
        SendMessageW(s.scale, CB_SETCURSEL, st.renderScale - 1, 0);
        make(L"STATIC", L"Sharper, but much harder on the GPU.", 0, 150, 162, 215, 16, 0);

        make(L"STATIC", L"Anti-aliasing", 0, 20, 190, 125, 20, 0);
        s.aa = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 150, 186, 210, 200, IdAa);
        SendMessageW(s.aa, CB_ADDSTRING, 0, LPARAM(L"Off"));
        SendMessageW(s.aa, CB_ADDSTRING, 0, LPARAM(L"FXAA (smooths edges)"));
        SendMessageW(s.aa, CB_SETCURSEL, st.antiAliasing ? 1 : 0, 0);

        make(L"STATIC", L"Aspect ratio", 0, 20, 226, 125, 20, 0);
        s.aspect = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 150, 222, 210, 200, IdAspect);
        const wchar_t* aspects[] = { L"Auto (match the screen)", L"4:3", L"16:10", L"16:9 (original)", L"21:9 (ultrawide)", L"32:9 (super ultrawide)" };
        for (auto* t : aspects)
            SendMessageW(s.aspect, CB_ADDSTRING, 0, LPARAM(t));
        SendMessageW(s.aspect, CB_SETCURSEL, int(st.aspectRatio), 0);

        make(L"STATIC", L"Frame rate", 0, 20, 260, 125, 20, 0);
        s.fps = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 150, 256, 210, 200, IdFps);
        {
            const wchar_t* rates[] = { L"30 (original)", L"60", L"120", L"144", L"Unlimited" };
            for (auto* t : rates)
                SendMessageW(s.fps, CB_ADDSTRING, 0, LPARAM(t));
            uint32_t fr = st.frameRate;
            SendMessageW(s.fps, CB_SETCURSEL, fr == 60 ? 1 : fr == 120 ? 2 : fr == 144 ? 3 : fr == 0 ? 4 : 0, 0);
        }
        make(L"STATIC", L"Above 30, in-between frames are blended (the game still runs at 30).", 0, 150, 280, 215, 30, 0);

        s.vsync = make(L"BUTTON", L"Vertical sync (prevents tearing)", BS_AUTOCHECKBOX | WS_TABSTOP, 150, 316, 215, 22, IdVsync);
        SendMessageW(s.vsync, BM_SETCHECK, st.vsync ? BST_CHECKED : BST_UNCHECKED, 0);

        make(L"STATIC", L"Online", 0, 20, 350, 340, 26, 0, bigFont);
        make(L"STATIC", L"Player name", 0, 20, 386, 120, 20, 0);
        {
            std::wstring wn(st.playerName.begin(), st.playerName.end());
            s.name = make(L"EDIT", wn.c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 150, 382, 210, 24, IdName);
            SendMessageW(s.name, EM_LIMITTEXT, 15, 0);
        }

        s.show = make(L"BUTTON", L"Show this window when the game starts", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 418, 340, 22, IdShowLauncher);
        SendMessageW(s.show, BM_SETCHECK, st.showLauncher ? BST_CHECKED : BST_UNCHECKED, 0);
        make(L"STATIC", inGame ? L"Render resolution, aspect ratio and player name change the next time the game starts."
                               : L"(Start with --launcher to get it back. Alt+Enter switches to and from fullscreen in game.)",
             0, 20, 440, 345, 32, 0);

        make(L"BUTTON", L"Controls...", BS_PUSHBUTTON | WS_TABSTOP, 20, 478, 110, 28, IdControls);
        HWND play = make(L"BUTTON", inGame ? L"Resume" : L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, 170, 478, 90, 28, IdPlay);
        make(L"BUTTON", inGame ? L"Quit game" : L"Quit", BS_PUSHBUTTON | WS_TABSTOP, 270, 478, 90, 28, IdQuit);
        UpdateEnabled(s);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        SetFocus(play);

        MSG m;
        while (!s.done && GetMessageW(&m, nullptr, 0, 0) > 0)
        {
            if (IsDialogMessageW(hwnd, &m))
                continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (owner)
        {
            EnableWindow(owner, TRUE);
            SetForegroundWindow(owner);
        }
        DeleteObject(s.font);
        DeleteObject(bigFont);
        g_launcher = nullptr;
        return s;
    }

    bool RunLauncher()
    {
        return RunSettingsWindow(false, nullptr).play;
    }

    MenuResult RunInGameMenu(void* owner)
    {
        Settings before = g_settings;
        LauncherState s = RunSettingsWindow(true, HWND(owner));
        MenuResult r;
        r.quit = s.quit;
        if (!s.play)
            return r;
        const Settings& now = g_settings;
        r.displayChanged = now.displayMode != before.displayMode || now.windowWidth != before.windowWidth || now.windowHeight != before.windowHeight;
        r.frameRateChanged = now.frameRate != before.frameRate;
        r.restartNeeded = now.renderScale != before.renderScale || now.aspectRatio != before.aspectRatio || now.playerName != before.playerName;
        return r;
    }
#else
    bool RunLauncher() { return true; }
    MenuResult RunInGameMenu(void*) { return {}; }
#endif
}
