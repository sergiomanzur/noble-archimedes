/*
 * Platform shim for the OpenCE D3D8/OpenGL renderer inside the Castlevania
 * recomp. Guest virtual addresses are translated using the runtime mapping
 * offset supplied before device initialization.
 */
#ifndef NOBLE_D3D8GL_PLATFORM_H
#define NOBLE_D3D8GL_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>

/* XDK types (D3DDevice, D3DBaseTexture, DWORD, ...). */
#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- logging ---------- */
void platform_log(const char *format, ...);
void platform_unimplemented(const char *name);

/* ---------- contiguous / physical memory ---------- */
#define PLATFORM_CONTIGUOUS_BASE 0x80000000UL
#define PLATFORM_CONTIGUOUS_SIZE 0x20000000UL
#define PLATFORM_ANY_PHYSICAL_ADDRESS 0xffffffffUL

void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
                                unsigned long physical_address, DWORD protect);
void platform_contiguous_free(void *address);
BOOL platform_is_contiguous(const void *address);

/* Guest pointers and resource Data words have different representations.
 * Export arguments are Xbox VAs; texture/vertex Data words below 64 MiB
 * are physical offsets, and high Data words are Xbox VAs. The host arena
 * lives above the guest physical alias and must never receive the offset. */
extern ptrdiff_t noble_guest_memory_offset;
static inline void *noble_guest_pointer(uint32_t address)
{
    return address ? (void *)((uintptr_t)address + noble_guest_memory_offset) : NULL;
}
static inline void *noble_phys_to_virt(unsigned long physical)
{
    if (physical < 0x04000000UL)
        return (void *)((uintptr_t)(0x80000000UL + physical) + noble_guest_memory_offset);
    if (physical < 0x84000000UL)
        return noble_guest_pointer((uint32_t)physical);
    return (void *)(uintptr_t)physical;
}
static inline unsigned long noble_virt_to_phys(const void *address)
{
    uintptr_t value = (uintptr_t)address;
    uintptr_t base = (uintptr_t)0x80000000UL + noble_guest_memory_offset;
    if (value >= base && value < base + 0x04000000UL)
        return (unsigned long)(value - base);
    return (unsigned long)value;
}
#define PLATFORM_PHYSICAL_TO_VIRTUAL(physical) noble_phys_to_virt((unsigned long)(physical))
#define PLATFORM_VIRTUAL_TO_PHYSICAL(address) noble_virt_to_phys((const void *)(address))

/* ---------- guest-memory write tracking ---------- */
void memory_watch_initialize(void);
void memory_watch_protect(unsigned long address, unsigned long size);
unsigned long memory_watch_generation(unsigned long address, unsigned long size);
unsigned long memory_watch_serial(void);
void memory_watch_prepare_write(void *address, unsigned long size);
void memory_watch_forget(void *address, unsigned long size);
void memory_watch_begin_frame(void);

/* ---------- settings ---------- */
const char *config_string(const char *name);
long config_integer(const char *name);
float config_real(const char *name);
BOOL config_boolean(const char *name);
unsigned long config_changes(void);

/* ---------- Halo screen policy (neutral defaults for this title) ---------- */
long halo_screen_width(void);
long halo_screen_commit(void);
void halo_screen_ui_offset(unsigned char centered);
float halo_screen_scale(void);
float halo_screen_pixel_scale(void);
long halo_shadow_map_scale(void);
int halo_model_detail_stock(void);
float halo_model_detail_scale(int level);
int halo_interpolation_enabled(void);

/* ---------- window, OpenGL and input ---------- */
BOOL platform_video_initialize(unsigned long width, unsigned long height);
void platform_video_swap(void);
BOOL platform_screen_mode(long *width, long *height);
void platform_video_drawable_size(int *width, int *height);
void platform_video_window_size(int *width, int *height);
BOOL platform_video_hdr(float *sdr_white, float *headroom);
void platform_pump_events(void);

struct platform_ui_pointer
{
    float x, y;
    float click_x, click_y;
    BOOL moved;
    int left_clicks, right_clicks;
    int wheel_steps;
    int downs;
    float down_x, down_y;
    BOOL touch;
};
void platform_menus_set_active(BOOL active);
void platform_ui_pointer_set_active(BOOL active);
BOOL platform_ui_pointer_read(struct platform_ui_pointer *pointer);
BOOL platform_scoreboard_pointer(BOOL offered, struct platform_ui_pointer *pointer);
const char *platform_data_root(void);
BOOL platform_screenshot_take_request(void);
void console_printf(BOOL error, const char *format, ...);

/* ---------- low (32-bit) host allocator ---------- *
 * D3D8 handles are DWORDs and OpenCE stores host vertex-shader object pointers
 * in them.  Keeping every host allocation below 4 GiB preserves those casts
 * and also keeps every resource Data field a real 32-bit Xbox-style address. */
void *noble_low_malloc(size_t size);
void *noble_low_calloc(size_t count, size_t size);
void *noble_low_realloc(void *pointer, size_t size);
void noble_low_free(void *pointer);
#define malloc noble_low_malloc
#define calloc noble_low_calloc
#define realloc noble_low_realloc
#define free noble_low_free

#ifdef __cplusplus
}
#endif

#endif
