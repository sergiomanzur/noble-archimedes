/* Host-side platform shim for the OpenCE D3D8/OpenGL layer. */
#include "platform.h"

#include <SDL3/SDL.h>
#include "gl.h"
#include <stdarg.h>

int gl_functions_load(void);

/* Windows kernel entry points (avoid including windows.h next to the XDK). */
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, size_t size,
                                                  unsigned long type,
                                                  unsigned long protect);
__declspec(dllimport) int __stdcall VirtualFree(void *address, size_t size,
                                                unsigned long type);

#define MEM_RESERVE 0x2000UL
#define MEM_COMMIT 0x1000UL
#define PAGE_READWRITE 0x04UL

#define LOW_ARENA_BASE ((void *)(uintptr_t)0x90000000u)
#define LOW_ARENA_SIZE (512u * 1024u * 1024u)

static unsigned char *g_low_next;
static unsigned char *g_low_end;

static void low_init(void)
{
    if (!g_low_next) {
        unsigned char *base = (unsigned char *)VirtualAlloc(
            LOW_ARENA_BASE, LOW_ARENA_SIZE, MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE);
        if (!base)
            base = (unsigned char *)VirtualAlloc(
                NULL, LOW_ARENA_SIZE, MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE);
        g_low_next = base;
        g_low_end = base ? base + LOW_ARENA_SIZE : NULL;
        if (!base)
            platform_log("D3D8GL: cannot reserve the low host arena");
    }
}

void *noble_low_malloc(size_t size)
{
    void *result;
    low_init();
    if (!g_low_next)
        return NULL;
    size = (size + 15u) & ~(size_t)15u;
    if (size == 0)
        size = 16;
    if (g_low_next + size > g_low_end) {
        platform_log("D3D8GL: low host arena exhausted (%zu bytes)", size);
        return NULL;
    }
    result = g_low_next;
    g_low_next += size;
    return result;
}

void *noble_low_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *result = noble_low_malloc(total);
    if (result)
        memset(result, 0, total);
    return result;
}

void *noble_low_realloc(void *pointer, size_t size)
{
    void *result = noble_low_malloc(size);
    if (result && pointer) {
        /* The old allocation's size is not tracked; TLS shader/string blocks
         * are small and callers pass the old contents' length implicitly only
         * through the new size.  The OpenCE layer only reallocs growable text
         * buffers, so copying the new size is safe within the arena. */
        memcpy(result, pointer, size);
    }
    return result;
}

void noble_low_free(void *pointer)
{
    (void)pointer; /* arena allocations live for the process */
}

/* ---------- logging ---------- */
void platform_log(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

void platform_unimplemented(const char *name)
{
    static const char *last;
    if (last != name) {
        last = name;
        platform_log("D3D8GL: unimplemented %s", name);
    }
}

void console_printf(BOOL error, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(error ? stderr : stdout, format, arguments);
    fputc('\n', error ? stderr : stdout);
    va_end(arguments);
}

/* ---------- contiguous memory ---------- */
void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
                                unsigned long physical_address, DWORD protect)
{
    unsigned char *result;
    (void)physical_address;
    (void)protect;
    if (alignment < 16)
        alignment = 16;
    low_init();
    if (!g_low_next)
        return NULL;
    result = (unsigned char *)(((uintptr_t)g_low_next + alignment - 1u) &
                               ~((uintptr_t)alignment - 1u));
    if (result + size > g_low_end)
        return NULL;
    g_low_next = result + size;
    return result;
}

void platform_contiguous_free(void *address)
{
    (void)address;
}

BOOL platform_is_contiguous(const void *address)
{
    uintptr_t value = (uintptr_t)address;
    return (value >= 0x80000000u && value < 0xA0000000u) ? TRUE : FALSE;
}

/* ---------- memory watch (the guest pages are not write-protected here) */
void memory_watch_initialize(void) {}
void memory_watch_protect(unsigned long address, unsigned long size) { (void)address; (void)size; }
unsigned long memory_watch_generation(unsigned long address, unsigned long size) { (void)address; (void)size; return 0; }
unsigned long memory_watch_serial(void) { return 0; }
void memory_watch_prepare_write(void *address, unsigned long size) { (void)address; (void)size; }
void memory_watch_forget(void *address, unsigned long size) { (void)address; (void)size; }
void memory_watch_begin_frame(void) {}

/* ---------- settings ---------- */
const char *config_string(const char *name)
{
    if (!strcmp(name, "display.anti_aliasing")) return "off";
    if (!strcmp(name, "display.model_detail")) return "stock";
    if (!strcmp(name, "display.hdr_tone_mapping")) return "linear";
    if (!strcmp(name, "debug.gpu_skip_vertex_shaders")) return "";
    if (!strcmp(name, "debug.gpu_dump_shaders")) return "";
    return "";
}
long config_integer(const char *name)
{
    if (!strcmp(name, "display.shadow_resolution")) return 0;
    if (!strcmp(name, "display.screen_width")) return 640;
    if (!strcmp(name, "debug.gpu_flush_draws")) return 0;
    return 0;
}
float config_real(const char *name)
{
    (void)name;
    return 1.0f;
}
BOOL config_boolean(const char *name)
{
    (void)name;
    return FALSE;
}
unsigned long config_changes(void) { return 0; }

/* ---------- Halo screen policy: the title's native 640x480 ---------- */

/* ---------- window / OpenGL ---------- */
static SDL_Window *g_window;
static SDL_GLContext g_context;

BOOL platform_video_initialize(unsigned long width, unsigned long height)
{
    if (g_window)
        return TRUE;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        platform_log("D3D8GL: SDL_Init failed: %s", SDL_GetError());
        return FALSE;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("Castlevania CoD | D3D8GL", (int)width, (int)height,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        platform_log("D3D8GL: SDL_CreateWindow failed: %s", SDL_GetError());
        return FALSE;
    }
    g_context = SDL_GL_CreateContext(g_window);
    if (!g_context) {
        platform_log("D3D8GL: SDL_GL_CreateContext failed: %s", SDL_GetError());
        return FALSE;
    }
    SDL_GL_MakeCurrent(g_window, g_context);
    if (!gl_functions_load()) {
        platform_log("D3D8GL: cannot load the OpenGL entry points");
        return FALSE;
    }
    SDL_GL_SetSwapInterval(0);
    {
        const char *(*get_string)(unsigned int) =
            (const char *(*)(unsigned int))SDL_GL_GetProcAddress("glGetString");
        platform_log("D3D8GL: OpenGL %s (%s)",
                     get_string ? get_string(0x1F02) : "?",
                     get_string ? get_string(0x1F01) : "?");
    }
    return TRUE;
}

void platform_video_swap(void)
{
    if (g_window)
        SDL_GL_SwapWindow(g_window);
}

BOOL platform_screen_mode(long *width, long *height)
{
    *width = 640;
    *height = 480;
    return TRUE;
}

void platform_video_drawable_size(int *width, int *height)
{
    if (g_window)
        SDL_GetWindowSizeInPixels(g_window, width, height);
    else {
        *width = 640;
        *height = 480;
    }
}

void platform_video_window_size(int *width, int *height)
{
    if (g_window)
        SDL_GetWindowSize(g_window, width, height);
    else {
        *width = 640;
        *height = 480;
    }
}

BOOL platform_video_hdr(float *sdr_white, float *headroom)
{
    if (sdr_white) *sdr_white = 1.0f;
    if (headroom) *headroom = 1.0f;
    return FALSE;
}

void platform_pump_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT)
            exit(0);
    }
}

/* ---------- input ---------- */
void platform_menus_set_active(BOOL active) { (void)active; }
void platform_ui_pointer_set_active(BOOL active) { (void)active; }
BOOL platform_ui_pointer_read(struct platform_ui_pointer *pointer)
{
    if (pointer)
        memset(pointer, 0, sizeof(*pointer));
    return FALSE;
}
BOOL platform_scoreboard_pointer(BOOL offered, struct platform_ui_pointer *pointer)
{
    (void)offered;
    (void)pointer;
    return FALSE;
}

const char *platform_data_root(void) { return "."; }
BOOL platform_screenshot_take_request(void) { return FALSE; }

/* ---------- optional OpenCE post-processing (disabled) ---------- */
BOOL xgpu_post_prepare(BOOL smaa) { (void)smaa; return FALSE; }
BOOL xgpu_post_prepare_hdr(void) { return FALSE; }
BOOL xgpu_post_anti_alias(BOOL smaa, GLuint framebuffer, GLenum format,
                          unsigned long width, unsigned long height)
{
    (void)smaa; (void)framebuffer; (void)format; (void)width; (void)height;
    return FALSE;
}
BOOL xgpu_post_tone_map(GLuint source, GLuint source_texture, GLuint destination,
                        unsigned long width, unsigned long height, float exposure,
                        float peak)
{
    (void)source; (void)source_texture; (void)destination;
    (void)width; (void)height; (void)exposure; (void)peak;
    return FALSE;
}
BOOL xgpu_post_present(GLuint texture, int x, int y, int width, int height, float scale)
{
    (void)texture; (void)x; (void)y; (void)width; (void)height; (void)scale;
    return FALSE;
}


/* Windows file wrappers (OpenCE prefix maps fopen/freopen here). */
#ifdef fopen
#undef fopen
#endif
#ifdef freopen
#undef freopen
#endif
FILE *halo_windows_fopen(const char *path, const char *mode) { return fopen(path, mode); }
FILE *halo_windows_freopen(const char *path, const char *mode, FILE *stream) { return freopen(path, mode, stream); }
int crash_reporter_process(void) { return 0; }
unsigned int menu_art_texture(unsigned long data, unsigned long *levels)
{
    (void)data;
    if (levels) *levels = 1;
    return 0;
}
int halo_interpolation_enabled(void) { return 1; }



int gl_functions_load(void);

