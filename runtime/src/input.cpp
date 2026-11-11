// Keyboard and mouse -> virtual controller (see input.h).
#include "input.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <thread>
#endif

namespace input
{
    namespace
    {
        struct ActionInfo { const char* label; const char* ini; };
        const ActionInfo kActions[ActionCount] = {
            { "A button", "a" }, { "B button", "b" }, { "X button", "x" }, { "Y button", "y" },
            { "Left bumper (LB)", "lb" }, { "Right bumper (RB)", "rb" },
            { "Left trigger (LT)", "lt" }, { "Right trigger (RT)", "rt" },
            { "Back", "back" }, { "Start", "start" },
            { "Left stick click", "ls" }, { "Right stick click", "rs" },
            { "D-pad up", "dpad_up" }, { "D-pad down", "dpad_down" }, { "D-pad left", "dpad_left" }, { "D-pad right", "dpad_right" },
            { "Move forward", "move_up" }, { "Move back", "move_down" }, { "Move left", "move_left" }, { "Move right", "move_right" },
            { "Look up", "look_up" }, { "Look down", "look_down" }, { "Look left", "look_left" }, { "Look right", "look_right" },
        };

        std::mutex g_mouseMutex;
        double g_mouseX = 0, g_mouseY = 0;      // accumulated since the last poll
        double g_camX = 0, g_camY = 0;          // accumulated since the last camera update
        std::chrono::steady_clock::time_point g_lastCamera{};
        float g_stickX = 0, g_stickY = 0;       // smoothed mouse stick
        std::chrono::steady_clock::time_point g_lastPoll = std::chrono::steady_clock::now();
        void* volatile g_hwnd = nullptr;
        std::atomic<bool> g_captured{ false };

        bool Down(uint16_t vk)
        {
#ifdef _WIN32
            return vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
            (void)vk;
            return false;
#endif
        }

        bool Focused()
        {
#ifdef _WIN32
            return g_hwnd && GetForegroundWindow() == HWND(g_hwnd);
#else
            return false;
#endif
        }
    }

    const char* ActionLabel(int a) { return a >= 0 && a < ActionCount ? kActions[a].label : "?"; }
    const char* ActionIniKey(int a) { return a >= 0 && a < ActionCount ? kActions[a].ini : "?"; }

    void DefaultBindings(uint16_t k[ActionCount][2])
    {
        for (int i = 0; i < ActionCount; i++)
            k[i][0] = k[i][1] = 0;
#ifdef _WIN32
        auto set = [&](int a, uint16_t k0, uint16_t k1 = 0) { k[a][0] = k0; k[a][1] = k1; };
        set(A, VK_SPACE);
        set(B, 'C', VK_LCONTROL);
        set(X, 'R');
        set(Y, 'E');
        set(LB, 'Q');
        set(RB, 'F');
        set(LT, VK_RBUTTON);
        set(RT, VK_LBUTTON);
        set(Back, VK_TAB);
        set(Start, VK_RETURN);
        set(LS, VK_LSHIFT);
        set(RS, VK_MBUTTON, 'V');
        set(DpadUp, VK_UP, '1');
        set(DpadDown, VK_DOWN, '3');
        set(DpadLeft, VK_LEFT, '4');
        set(DpadRight, VK_RIGHT, '2');
        set(MoveUp, 'W');
        set(MoveDown, 'S');
        set(MoveLeft, 'A');
        set(MoveRight, 'D');
        set(LookUp, 'I');
        set(LookDown, 'K');
        set(LookLeft, 'J');
        set(LookRight, 'L');
#endif
    }

    const char* KeyName(uint16_t vk)
    {
        static thread_local char buf[64];
        if (!vk)
            return "";
#ifdef _WIN32
        switch (vk)
        {
        case VK_LBUTTON: return "Left mouse";
        case VK_RBUTTON: return "Right mouse";
        case VK_MBUTTON: return "Middle mouse";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_LSHIFT: return "Left Shift";
        case VK_RSHIFT: return "Right Shift";
        case VK_LCONTROL: return "Left Ctrl";
        case VK_RCONTROL: return "Right Ctrl";
        case VK_LMENU: return "Left Alt";
        case VK_RMENU: return "Right Alt";
        }
        UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LONG lp = LONG(scan << 16);
        switch (vk)
        {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
        case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            lp |= 1 << 24; // extended key
            break;
        }
        if (GetKeyNameTextA(lp, buf, sizeof(buf)) > 0)
            return buf;
#endif
        snprintf(buf, sizeof(buf), "Key %u", vk);
        return buf;
    }

    bool Enabled() { return true; }

    void OnMouseMove(int dx, int dy)
    {
        if (!g_captured)
            return;
        std::lock_guard lock(g_mouseMutex);
        g_mouseX += dx;
        g_mouseY += dy;
        g_camX += dx;
        g_camY += dy;
    }

    void SetGameWindow(void* hwnd) { g_hwnd = hwnd; }

    // ---- Controllers ----
    namespace
    {
        std::atomic<int> g_padMap[4] = { -1, -1, -1, -1 };
        std::mutex g_vibMutex;
        uint16_t g_vibWanted[4][2] = {};  // per player, as the game asked
        bool g_vibActive[4] = {};         // a non-zero vibration was sent to that pad

        // Asking XInput about an empty slot can take milliseconds, so which
        // controllers are connected is checked here, once a second, instead
        // of on the game's threads.
        void StartPadWatcher()
        {
#ifdef _WIN32
            static std::once_flag once;
            std::call_once(once, [] {
                auto scan = [] {
                    int n = 0, map[4] = { -1, -1, -1, -1 };
                    for (int i = 0; i < 4; i++)
                    {
                        XINPUT_CAPABILITIES c;
                        if (XInputGetCapabilities(DWORD(i), 0, &c) == ERROR_SUCCESS)
                            map[n++] = i;
                    }
                    for (int i = 0; i < 4; i++)
                        g_padMap[i] = map[i];
                };
                scan();
                std::thread([scan] {
                    for (;;)
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        scan();
                    }
                }).detach();
            });
#endif
        }

        void SendVibration(int pad, uint16_t l, uint16_t r)
        {
#ifdef _WIN32
            XINPUT_VIBRATION v = { l, r };
            XInputSetState(DWORD(pad), &v);
#else
            (void)pad; (void)l; (void)r;
#endif
        }
    }

    int PadForPlayer(uint32_t player)
    {
        StartPadWatcher();
        return player < 4 ? g_padMap[player].load() : -1;
    }

    void SetVibration(uint32_t player, uint16_t left, uint16_t right)
    {
        int pad = PadForPlayer(player);
        if (pad < 0)
            return;
        uint32_t strength = std::min<uint32_t>(settings::Get().vibration, 100);
        std::lock_guard lock(g_vibMutex);
        g_vibWanted[player][0] = left;
        g_vibWanted[player][1] = right;
        if (!Focused())
            left = right = 0;
        uint16_t l = uint16_t(left * strength / 100), r = uint16_t(right * strength / 100);
        SendVibration(pad, l, r);
        g_vibActive[pad] = l || r;
    }

    void StopVibration()
    {
        std::lock_guard lock(g_vibMutex);
        for (int pad = 0; pad < 4; pad++)
            if (g_vibActive[pad])
            {
                SendVibration(pad, 0, 0);
                g_vibActive[pad] = false;
            }
    }

    bool TakeCameraDelta(float& dx, float& dy)
    {
        const auto& st = settings::Get();
        std::lock_guard lock(g_mouseMutex);
        auto now = std::chrono::steady_clock::now();
        bool fresh = now - g_lastCamera < std::chrono::milliseconds(250);
        g_lastCamera = now;
        dx = float(g_camX);
        dy = float(g_camY);
        g_camX = g_camY = 0;
        if (!st.directCamera || !g_captured || !Focused())
            return false;
        if (!fresh)
            dx = dy = 0; // first update after a pause: don't jump
        return true;
    }

    void SetCapture(bool on)
    {
#ifdef _WIN32
        const auto& st = settings::Get();
        on = on && st.mouseLook && g_hwnd;
        if (on == g_captured)
            return;
        g_captured = on;
        if (on)
        {
            RECT rc;
            GetClientRect(HWND(g_hwnd), &rc);
            POINT tl = { rc.left, rc.top }, br = { rc.right, rc.bottom };
            ClientToScreen(HWND(g_hwnd), &tl);
            ClientToScreen(HWND(g_hwnd), &br);
            RECT clip = { tl.x, tl.y, br.x, br.y };
            ClipCursor(&clip);
            while (ShowCursor(FALSE) >= 0) {}
        }
        else
        {
            ClipCursor(nullptr);
            while (ShowCursor(TRUE) < 0) {}
        }
        std::lock_guard lock(g_mouseMutex);
        g_mouseX = g_mouseY = 0;
        g_camX = g_camY = 0;
#else
        (void)on;
#endif
    }

    void Poll(uint16_t& buttons, uint8_t& lt, uint8_t& rt, int16_t& lx, int16_t& ly, int16_t& rx, int16_t& ry)
    {
        const auto& st = settings::Get();
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - g_lastPoll).count();
        g_lastPoll = now;
        double mx, my;
        bool cameraDirect;
        {
            std::lock_guard lock(g_mouseMutex);
            mx = g_mouseX;
            my = g_mouseY;
            g_mouseX = g_mouseY = 0;
            // The game's camera has taken the mouse directly in the last
            // quarter second: don't also turn it into stick movement.
            cameraDirect = st.directCamera && now - g_lastCamera < std::chrono::milliseconds(250);
            if (!cameraDirect)
                g_camX = g_camY = 0; // nothing to hand over later
        }
        if (cameraDirect)
            mx = my = 0;
        if (!Focused())
        {
            g_stickX = g_stickY = 0;
            return;
        }

        auto any = [&](int a) { return Down(st.keys[a][0]) || Down(st.keys[a][1]); };
        static const uint16_t kButtonBits[] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0, 0, 0x0020, 0x0010, 0x0040, 0x0080,
                                                0x0001, 0x0002, 0x0004, 0x0008 };
        for (int a = A; a <= DpadRight; a++)
            if (kButtonBits[a] && any(a))
                buttons |= kButtonBits[a];
        if (any(LT)) lt = 255;
        if (any(RT)) rt = 255;

        // Keys push a stick all the way; added to the controller's value.
        auto addAxis = [](int16_t& v, float f) { v = int16_t(std::clamp(int(v) + int(f * 32767.0f), -32768, 32767)); };
        float kx = float(any(MoveRight)) - float(any(MoveLeft)), ky = float(any(MoveUp)) - float(any(MoveDown));
        if (kx && ky) { kx *= 0.7071f; ky *= 0.7071f; }
        addAxis(lx, kx);
        addAxis(ly, ky);
        float lkx = float(any(LookRight)) - float(any(LookLeft)), lky = float(any(LookUp)) - float(any(LookDown));
        addAxis(rx, lkx);
        addAxis(ry, lky);

        // Mouse: speed -> right stick deflection. Normalized to the game's 30
        // polls a second, smoothed a little, and lifted past the stick's dead
        // zone so slow movements still register.
        if (g_captured && dt > 0.0)
        {
            double norm = (1.0 / 30.0) / std::clamp(dt, 0.004, 0.1);
            double sens = std::max(1u, st.mouseSensitivity) / 50.0; // 50 = 1x
            double vx = mx * norm * sens / 25.0, vy = -my * norm * sens / 25.0;
            if (st.invertY)
                vy = -vy;
            // No smoothing: the stick follows the mouse frame by frame and
            // returns to centre as soon as it stops (smoothing felt floaty).
            // The curve is applied to the 2D speed so diagonals stay true; the
            // result is lifted past the game's dead zone.
            double mag = std::sqrt(vx * vx + vy * vy);
            g_stickX = g_stickY = 0;
            if (mag > 0.002)
            {
                const double dz = 0.24;
                double curved = std::pow(std::min(mag, 1.0), std::clamp(st.mouseCurve, 20u, 100u) / 100.0);
                double out = dz + (1.0 - dz) * curved;
                g_stickX = float(vx / mag * out);
                g_stickY = float(vy / mag * out);
            }
            addAxis(rx, g_stickX);
            addAxis(ry, g_stickY);
        }
    }
}
