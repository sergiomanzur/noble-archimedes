/* Halo's optional high-resolution HUD/text atlases are not part of this title. */
#include "xgpu.h"
#include "hud_hires.h"
#include "text_hires.h"

unsigned long hud_hires_settings_changed(void) { return 0; }
int hud_hires_bitmap_affected(unsigned long address, long width, long height,
                              unsigned long changed)
{
    (void)address; (void)width; (void)height; (void)changed;
    return 0;
}
long hud_hires_override_find(unsigned long address, unsigned long width,
                             unsigned long height, unsigned long level0_size)
{
    (void)address; (void)width; (void)height; (void)level0_size;
    return -1;
}
unsigned int hud_hires_override_texture(long asset, unsigned long *levels)
{
    (void)asset;
    if (levels) *levels = 1;
    return 0;
}
int hud_hires_override_coverage(long asset) { (void)asset; return 0; }
unsigned int hud_hires_placeholder_texture(unsigned long data, unsigned long *levels)
{
    (void)data;
    if (levels) *levels = 1;
    return 0;
}
int hud_hires_override_point_threshold(long asset) { (void)asset; return 0; }
long hud_hires_upscale_at(unsigned long address, long width, long height)
{
    (void)address; (void)width; (void)height;
    return -1;
}
unsigned int hud_hires_upscale_texture(const unsigned char *bgra, unsigned long width,
                                       unsigned long height, long kind,
                                       unsigned long *levels)
{
    (void)bgra; (void)width; (void)height; (void)kind;
    if (levels) *levels = 1;
    return 0;
}
unsigned int hud_hires_png_texture(const void *png, unsigned long size,
                                   unsigned long *levels)
{
    (void)png; (void)size;
    if (levels) *levels = 1;
    return 0;
}
void hud_hires_map_loaded(const long *assets, long count) { (void)assets; (void)count; }
void hud_hires_map_unloaded(void) {}
void text_hires_register_atlas(const unsigned long *texture, unsigned long width,
                               unsigned long height)
{
    (void)texture; (void)width; (void)height;
}
unsigned int text_hires_atlas_texture(unsigned long data) { (void)data; return 0; }
void text_hires_set_reset_hook(void (*hook)(void)) { (void)hook; }
