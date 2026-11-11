// PC settings, chosen in the launcher and saved to <data>/settings.ini.
#pragma once
#include <cstdint>
#include <string>
#include "input.h"

namespace settings
{
    // Fullscreen = borderless window covering the monitor (desktop resolution);
    // BorderlessWindow = the chosen window size without a frame, centred.
    enum class DisplayMode : int { Windowed = 0, Fullscreen = 1, BorderlessWindow = 2 };
    // Shape of the picture the game renders. Auto = the window's (or, in
    // fullscreen, the monitor's) shape.
    enum class AspectRatio : int { Auto = 0, R4x3, R16x10, R16x9, R21x9, R32x9 };

    struct Settings
    {
        uint32_t windowWidth = 1280, windowHeight = 720;
        DisplayMode displayMode = DisplayMode::Windowed;
        bool vsync = true;
        uint32_t renderScale = 1;   // internal resolution multiplier (1 = 1280x720)
        uint32_t antiAliasing = 0;  // 0 off, 1 FXAA (edge smoothing on the final image)
        AspectRatio aspectRatio = AspectRatio::R16x9;
        // Displayed frame rate: 30, 60, 120, 144, or 0 = unlimited. The game
        // itself always runs at 30 (its simulation advances one step per
        // frame); above 30 the renderer draws in-between frames by blending
        // each draw's vertex shader constants (positions, camera, bones)
        // between the last two game frames. See d3d12_backend.cpp.
        uint32_t frameRate = 30;
        bool showLauncher = true;   // show the launcher at startup

        // Keyboard and mouse (see input.h): two keys per controller input.
        uint16_t keys[input::ActionCount][2] = {};
        bool mouseLook = true;          // mouse moves the right stick
        uint32_t mouseSensitivity = 50; // 1..100, 50 = default speed
        bool invertY = false;
        // Mouse speed -> stick: deflection = speed ^ (curve / 100). Below 100
        // boosts small movements, cancelling the game's own stick curve.
        uint32_t mouseCurve = 60;
        // Mouse turns the game camera directly (see mouselook.cpp) instead of
        // being turned into right stick movement.
        bool directCamera = true;
        // Controller vibration strength: 0 = off, 100 = as the game asks.
        uint32_t vibration = 100;

        // LAN play: 0 = normal (this PC's network address); 1-4 = same-PC
        // test copy number (each uses 127.0.0.N).
        uint32_t lanTestInstance = 0;
        bool liveSignIn = true;       // report the profile as signed in to Xbox Live (online menus over LAN)
        std::string playerName = "Player"; // gamertag shown to other players (up to 15 characters)

        Settings() { input::DefaultBindings(keys); }
    };

    Settings& Get();
    // The aspect ratio to render at (width / height), resolving Auto.
    float RenderAspect();
    void Load(const std::string& dataDir);
    void Save();

    // Shows the launcher window. Returns false if the player closed it
    // instead of pressing Play. (Windows only; elsewhere returns true.)
    bool RunLauncher();

    // The same settings over the running game (Esc). The caller applies what
    // changed; render resolution, aspect ratio and the player name need a
    // restart.
    struct MenuResult
    {
        bool quit = false;             // Quit game was confirmed
        bool displayChanged = false;   // display mode or window size
        bool frameRateChanged = false;
        bool restartNeeded = false;
    };
    MenuResult RunInGameMenu(void* owner);
}
