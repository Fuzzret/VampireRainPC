// Direct mouse camera control.
//
// Found with the camera trace (camtrace.cpp): once per frame the game copies
// the controller input into the player object, and sub_82197C08(obj, params)
// turns that object's right stick (+516 X, +520 Y) into camera movement:
//   - stick -> response curve -> smoothed turn speed (+724 yaw, +728 pitch),
//   - yaw (+704, radians, wrapped to +-pi) += turn speed x params+16,
//     pitch (+700) += turn speed x params+24, clamped to [params+36, params+32],
//   - both speeds divided by a zoom factor (state+100) while aiming, where
//     state = *(obj - 68).
// The curve and smoothing are what made stick-emulated mouse aiming feel
// floaty. Here the mouse movement is added to yaw/pitch directly, before the
// game's update runs, so the game still clamps, wraps and uses the new angles
// in the same frame. A controller's right stick keeps working as before.
#include "kernel.h"
#include "input.h"
#include "settings.h"

#include <algorithm>
#include <cmath>

namespace
{
    float LoadF(uint32_t a)
    {
        uint32_t v = Load32(a);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
}

PPC_FUNC_IMPL(__imp__sub_82197C08);
PPC_FUNC(sub_82197C08)
{
    uint32_t obj = ctx.r3.u32, params = ctx.r4.u32;
    float dx, dy;
    if (obj && params && input::TakeCameraDelta(dx, dy) && (dx != 0.0f || dy != 0.0f))
    {
        const auto& st = settings::Get();
        // Radians per mouse count: 0.0011 at the default sensitivity (50),
        // about the feel of a typical PC shooter at 800 dpi.
        float k = 0.0011f * float(std::max(1u, st.mouseSensitivity)) / 50.0f;

        // Slower while aiming/zoomed, by the same factor the game uses.
        uint32_t state = Load32(obj - 68);
        if (state)
        {
            bool zoomed = (Load8(state + 1) && Load8(state + 2) && Load8(state + 4) == 6) || Load32(state + 96) != 0;
            float zoom = LoadF(state + 100);
            if (zoomed && zoom > 0.01f && std::isfinite(zoom))
                k /= zoom;
        }

        // Stick right turns the yaw down, stick up raises the pitch.
        float yaw = LoadF(obj + 704) - dx * k;
        float pitch = LoadF(obj + 700) + (st.invertY ? dy : -dy) * k;
        const float pi = 3.14159265f;
        while (yaw > pi) yaw -= 2.0f * pi;
        while (yaw < -pi) yaw += 2.0f * pi;
        float pmin = LoadF(params + 36), pmax = LoadF(params + 32);
        if (std::isfinite(pmin) && std::isfinite(pmax) && pmin <= pmax)
            pitch = std::clamp(pitch, pmin, pmax);
        if (std::isfinite(yaw) && std::isfinite(pitch))
        {
            StoreF32(obj + 704, yaw);
            StoreF32(obj + 700, pitch);
        }
    }
    __imp__sub_82197C08(ctx, base);
}
