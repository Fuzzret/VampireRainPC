// Keyboard and mouse as a virtual Xbox 360 controller. Each controller
// input (button, trigger, stick direction) has two bindable keys or mouse
// buttons; mouse movement can drive the right stick for aiming. A real
// controller still works at the same time.
#pragma once
#include <cstdint>

namespace input
{
    enum Action : int
    {
        A, B, X, Y, LB, RB, LT, RT, Back, Start, LS, RS,
        DpadUp, DpadDown, DpadLeft, DpadRight,
        MoveUp, MoveDown, MoveLeft, MoveRight,
        LookUp, LookDown, LookLeft, LookRight,
        ActionCount
    };

    const char* ActionLabel(int action);  // shown in the launcher
    const char* ActionIniKey(int action); // settings.ini name
    // Default bindings (Windows virtual-key codes; 0 = none).
    void DefaultBindings(uint16_t keys[ActionCount][2]);
    // Display name of a virtual-key code (also mouse buttons).
    const char* KeyName(uint16_t vk);

    // Merges keyboard/mouse input into a controller state (in/out).
    void Poll(uint16_t& buttons, uint8_t& lt, uint8_t& rt, int16_t& lx, int16_t& ly, int16_t& rx, int16_t& ry);
    // Whether keyboard/mouse input is enabled (so a controller is reported
    // even when none is plugged in).
    bool Enabled();

    // From the game window: raw mouse movement, focus changes.
    void OnMouseMove(int dx, int dy);
    void SetGameWindow(void* hwnd);
    void SetCapture(bool on); // hide and confine the cursor while playing

    // Direct camera control: the game's camera update takes the mouse
    // movement since its last call (in counts). Returns false when the mouse
    // isn't being used for the camera. While the camera update keeps asking,
    // the mouse no longer drives the right stick.
    bool TakeCameraDelta(float& dx, float& dy);

    // Controllers. Players take the connected XInput controllers in order, so
    // a pad Windows numbers 2 still plays as player 1. Returns the XInput
    // index, or -1 if that player has no controller.
    int PadForPlayer(uint32_t player);
    // Controller vibration as the game requests it (0-65535 per motor),
    // scaled by the vibration setting; silenced while the game window isn't
    // in front.
    void SetVibration(uint32_t player, uint16_t left, uint16_t right);
    // Stops all vibration (window in the background, or the game closing).
    void StopVibration();
}
