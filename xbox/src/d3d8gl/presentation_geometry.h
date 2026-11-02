#ifndef NOBLE_PRESENTATION_GEOMETRY_H
#define NOBLE_PRESENTATION_GEOMETRY_H
#include <stdint.h>

struct noble_present_rect { int x, y, width, height; };
struct noble_projection_scale { float x, y; };

/* A guest screen coordinate refers to the centered original-width region
 * of a wider target; vertical scale defines the original pixel size. */
static int noble_guest_target_x(int x, int guest_width, int target_width, float vertical_scale)
{
    int border = (int)((target_width - guest_width * vertical_scale) * 0.5f);
    return border + (int)(x * vertical_scale + 0.5f);
}

static int noble_wide_window_width(int original_width, int height, float aspect)
{
    if (original_width <= 0 || height <= 0 ||
        !(aspect >= 4.0f / 3.0f && aspect <= 4.0f) || height > 100000)
        return original_width;
    return (int)(height * aspect + 0.5f);
}

/* Scale the tangent of the vertical FOV on both axes, preserving geometry. */
static int noble_projection_scaling(float aspect, float fov_multiplier,
                                    struct noble_projection_scale *out)
{
    if (!out || !(aspect >= 4.0f / 3.0f && aspect <= 4.0f) ||
        !(fov_multiplier >= 0.25f && fov_multiplier <= 4.0f)) return 0;
    out->x = (4.0f / 3.0f) / (aspect * fov_multiplier);
    out->y = 1.0f / fov_multiplier;
    return 1;
}

/* Horizontal expansion with the original vertical field of view. A FOV
 * multiplier greater than one increases the tangent of the horizontal FOV. */
static float noble_wide_clip_scale(float aspect, float fov_multiplier)
{
    if (!(aspect >= 4.0f / 3.0f && aspect <= 4.0f) ||
        !(fov_multiplier >= 0.25f && fov_multiplier <= 4.0f)) return 1.0f;
    return (4.0f / 3.0f) / (aspect * fov_multiplier);
}

/* Fit the entire source inside the drawable without distortion. */
static int noble_present_fit(int window_width, int window_height,
                             int source_width, int source_height,
                             struct noble_present_rect *out)
{
    int width, height;
    int64_t scaled_height;
    if (!out || window_width <= 0 || window_height <= 0 ||
        source_width <= 0 || source_height <= 0) return 0;
    width = window_width;
    scaled_height = (int64_t)window_width * source_height / source_width;
    if (scaled_height > window_height) {
        height = window_height;
        width = (int)((int64_t)window_height * source_width / source_height);
    } else height = (int)scaled_height;
    if (width <= 0 || height <= 0) return 0;
    out->x = (window_width - width) / 2;
    out->y = (window_height - height) / 2;
    out->width = width;
    out->height = height;
    return 1;
}
#endif
