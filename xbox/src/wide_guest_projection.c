#include <stdlib.h>
#include <string.h>
#include "d3d8gl/wide_setting_value.h"

/* Experimental and separately gated: this changes game-owned visibility
 * calculations, so it must not affect the default renderer or gameplay. */
static float x_factor = 1.0f, y_factor = 1.0f;
static void wide_guest_init(void)
{
    static int initialized;
    if (!initialized) {
        const char *guest = getenv("RECOMP_WIDE_GUEST_PROJECTION");
        const char *wide = getenv("RECOMP_WIDE_EXPERIMENT");
        const char *renderer = getenv("RECOMP_D3D8GL");
        const char *setting = getenv("RECOMP_WIDE_ASPECT");
        const char *fov_setting = getenv("RECOMP_WIDE_FOV_SCALE");
        initialized = 1;
        if (guest && *guest && strcmp(guest,"0") && wide && *wide && strcmp(wide,"0") &&
            renderer && *renderer && strcmp(renderer,"0")) {
            float aspect = noble_wide_parse_float(setting, 4.0f / 3.0f, 4.0f,
                                                   setting ? 4.0f / 3.0f : 16.0f / 9.0f);
            float fov = noble_wide_parse_float(fov_setting, 0.5f, 2.0f, 1.0f);
            x_factor = (4.0f / 3.0f) / (aspect * fov);
            y_factor = 1.0f / fov;
        }
    }
}

float noble_wide_guest_adjust(float original)
{
    wide_guest_init();
    return x_factor == 1.0f ? original : original * x_factor;
}

int noble_wide_guest_active(void)
{
    wide_guest_init();
    return x_factor != 1.0f || y_factor != 1.0f;
}

float noble_wide_guest_adjust_y(float original)
{
    wide_guest_init();
    return y_factor == 1.0f ? original : original * y_factor;
}
