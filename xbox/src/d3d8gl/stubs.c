/* Host-side platform shim for the OpenCE D3D8/OpenGL layer. */
#include "platform.h"
#include "presentation_geometry.h"

#include <SDL3/SDL.h>
#include "gl.h"
#include <stdarg.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif

int gl_functions_load(void);
float noble_wide_window_aspect(void);

/* Windows kernel entry points (avoid including windows.h next to the XDK). */
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, size_t size,
                                                  unsigned long type,
                                                  unsigned long protect);
__declspec(dllimport) int __stdcall VirtualFree(void *address, size_t size,
                                                unsigned long type);
__declspec(dllimport) int __stdcall IsBadReadPtr(const void *address, size_t size);

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
        /* Every resource Data word and shader handle is 32 bit. A fallback
         * outside that range cannot be represented and must fail explicitly. */
        if (base && ((uintptr_t)base > UINT32_MAX - LOW_ARENA_SIZE)) {
            VirtualFree(base, 0, 0x8000UL);
            base = NULL;
        }
        g_low_next = base;
        g_low_end = base ? base + LOW_ARENA_SIZE : NULL;
        if (!base)
            platform_log("D3D8GL: cannot reserve the low host arena");
    }
}

/* Reusable blocks prevent temporary decoded textures and GLSL strings
 * from consuming the finite 32-bit arena over a long gameplay session. */
struct low_block { size_t capacity; struct low_block *next; };
static struct low_block *low_free_blocks;
void *noble_low_malloc(size_t size)
{
    struct low_block **link, *block;
    size_t needed;
    low_init();
    if (!g_low_next || size > SIZE_MAX - 31u) return NULL;
    needed = (size + 15u) & ~(size_t)15u;
    if (!needed) needed = 16;
    for (link = &low_free_blocks; (block = *link) != NULL; link = &block->next) {
        if (block->capacity >= needed) {
            *link = block->next;
            if (block->capacity >= needed + sizeof(*block) + 16) {
                struct low_block *remainder = (struct low_block *)((unsigned char *)(block + 1) + needed);
                remainder->capacity = block->capacity - needed - sizeof(*block);
                remainder->next = *link;
                *link = remainder;
                block->capacity = needed;
            }
            block->next = NULL;
            return block + 1;
        }
    }
    if (needed + sizeof(*block) > (size_t)(g_low_end - g_low_next)) {
        platform_log("D3D8GL: low host arena exhausted (request %zu, bump used %zu)", size,
                     (size_t)(LOW_ARENA_SIZE - (g_low_end - g_low_next)));
        return NULL;
    }
    block = (struct low_block *)(void *)g_low_next;
    block->capacity = needed;
    block->next = NULL;
    g_low_next += needed + sizeof(*block);
    return block + 1;
}

void *noble_low_calloc(size_t count, size_t size)
{
    size_t total;
    void *result;
    if (size && count > SIZE_MAX / size) return NULL;
    total = count * size;
    result = noble_low_malloc(total);
    if (result) memset(result, 0, total);
    return result;
}

void noble_low_free(void *pointer)
{
    struct low_block *block, **link;
    if (!pointer) return;
    block = (struct low_block *)pointer - 1;
    for (link = &low_free_blocks; *link && (uintptr_t)*link < (uintptr_t)block; link = &(*link)->next) {}
    block->next = *link;
    *link = block;
    /* Coalesce in address order, including the predecessor when adjacent. */
    for (block = low_free_blocks; block && block->next;) {
        if ((unsigned char *)(block + 1) + block->capacity == (unsigned char *)block->next) {
            block->capacity += sizeof(*block) + block->next->capacity;
            block->next = block->next->next;
        } else block = block->next;
    }
}

void *noble_low_realloc(void *pointer, size_t size)
{
    void *result;
    size_t old_size = pointer ? ((struct low_block *)pointer - 1)->capacity : 0;
    if (pointer && size <= old_size) return pointer;
    result = noble_low_malloc(size);
    if (result && pointer) {
        memcpy(result, pointer, old_size);
        noble_low_free(pointer);
    }
    return result;
}

/* ---------- logging ---------- */
void platform_log(const char *format, ...)
{
    static FILE *probe_log;
    static int probe_checked;
    FILE *stream = stderr;
    va_list arguments;
    if (!probe_checked) {
        const char *path = getenv("RECOMP_WIDE_LOG");
        probe_checked = 1;
        if (path && *path) probe_log = fopen(path, "w");
    }
    if (probe_log) stream = probe_log;
    va_start(arguments, format);
    vfprintf(stream, format, arguments);
    fputc('\n', stream);
    if (probe_log) fflush(stream);
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

BOOL platform_range_mapped(const void *address, size_t size)
{
    if (!address || !size)
        return FALSE;
    return IsBadReadPtr(address, size) ? FALSE : TRUE;
}

/* Experimental exact equality retained for the archived SSE2 tests and
 * benchmark. Production memory_watch_generation uses memcmp as before; this
 * helper is not called by production. It only answers "all bytes equal" and
 * does not need the ordering of the first difference. SSE2 is baseline on
 * x86-64. Every byte is still inspected on equality: the vector stages cover
 * only whole blocks, and the scalar tail is exact. */
#if defined(_M_X64) || defined(__x86_64__)
#if defined(__clang__) || defined(__GNUC__)
#define CONTENT_M128_UNALIGNED __m128i_u
#else
#define CONTENT_M128_UNALIGNED __m128i
#endif
static int content_bytes_equal_sse2(const void *left, const void *right,
                                    size_t size)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    const __m128i zero = _mm_setzero_si128();
    size_t i = 0;

#define CONTENT_LOAD_DIFF(offset) \
    _mm_xor_si128( \
        _mm_loadu_si128((const CONTENT_M128_UNALIGNED *)(const void *)(a + i + (offset))), \
        _mm_loadu_si128((const CONTENT_M128_UNALIGNED *)(const void *)(b + i + (offset))))

    while (size - i >= 128) {
        __m128i d0 = CONTENT_LOAD_DIFF(0);
        __m128i d1 = CONTENT_LOAD_DIFF(16);
        __m128i d2 = CONTENT_LOAD_DIFF(32);
        __m128i d3 = CONTENT_LOAD_DIFF(48);
        __m128i d4 = CONTENT_LOAD_DIFF(64);
        __m128i d5 = CONTENT_LOAD_DIFF(80);
        __m128i d6 = CONTENT_LOAD_DIFF(96);
        __m128i d7 = CONTENT_LOAD_DIFF(112);
        __m128i d = _mm_or_si128(
            _mm_or_si128(_mm_or_si128(d0, d1), _mm_or_si128(d2, d3)),
            _mm_or_si128(_mm_or_si128(d4, d5), _mm_or_si128(d6, d7)));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(d, zero)) != 0xFFFF)
            return 0;
        i += 128;
    }
    while (size - i >= 16) {
        __m128i d = CONTENT_LOAD_DIFF(0);
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(d, zero)) != 0xFFFF)
            return 0;
        i += 16;
    }
#undef CONTENT_LOAD_DIFF

    while (i < size) {
        if (a[i] != b[i])
            return 0;
        i++;
    }
    return 1;
}
#undef CONTENT_M128_UNALIGNED
#endif

static int content_bytes_equal(const void *left, const void *right, size_t size)
{
#if defined(_M_X64) || defined(__x86_64__)
    return content_bytes_equal_sse2(left, right, size);
#else
    return memcmp(left, right, size) == 0;
#endif
}

/* Test seam: mode 0 is the experimental SSE2 equality path; mode 1 is the
 * CRT reference. Production memory_watch_generation uses memcmp and does not
 * call this seam. */
int noble_d3d8_content_equal_test(const void *left, const void *right,
                                  size_t size, int mode)
{
    if (mode == 1)
        return memcmp(left, right, size) == 0;
    return content_bytes_equal(left, right, size);
}

/* ---------- exact memory content generations
 * The recomp writes guest memory directly, so OpenCE's page fault watcher
 * cannot see those stores. Compare a snapshot before reusing an upload.
 * Snapshots use native VirtualAlloc, not the process-lifetime low arena.
 * The bounded table/budget may evict entries; that only causes reuploads. */
#define CONTENT_WATCH_SLOTS 2048u
#define CONTENT_WATCH_BUDGET (64u * 1024u * 1024u)
static struct {
    unsigned long address, size, generation;
    void *snapshot;
    size_t allocated;
} content_watch[CONTENT_WATCH_SLOTS];
static size_t content_snapshot_bytes;
static unsigned content_evict;
static unsigned long content_generation = 1;
static unsigned long content_lookup = 1;
static void content_watch_drop(unsigned slot)
{
    if (content_watch[slot].snapshot) {
        VirtualFree(content_watch[slot].snapshot, 0, 0x8000UL);
        content_snapshot_bytes -= content_watch[slot].allocated;
    }
    memset(&content_watch[slot], 0, sizeof(content_watch[slot]));
}
int page_watch_enabled(void);
void page_watch_initialize(void);
void page_watch_protect(unsigned long address, unsigned long size);
unsigned long page_watch_generation(unsigned long address, unsigned long size);
unsigned long page_watch_serial(void);
void page_watch_prepare_write(void *address, unsigned long size);
void page_watch_forget(void *address, unsigned long size);

void memory_watch_initialize(void)
{
    if (page_watch_enabled())
        page_watch_initialize();
}
void memory_watch_protect(unsigned long address, unsigned long size)
{
    if (page_watch_enabled())
        page_watch_protect(address, size);
}
unsigned long memory_watch_generation(unsigned long address, unsigned long size)
{
    if (page_watch_enabled())
        return page_watch_generation(address, size);
    const void *data = (const void *)(uintptr_t)address;
    unsigned slot = ((address >> 4) ^ size) % CONTENT_WATCH_SLOTS;
    size_t allocated;
    if (!address || !size) return 0;
    if (content_watch[slot].address != address || content_watch[slot].size != size)
        content_watch_drop(slot);
    if (!content_watch[slot].snapshot) {
        /* Larger resources are conservatively uploaded every use. */
        if (size > CONTENT_WATCH_BUDGET) return ++content_generation;
        allocated = ((size_t)size + 4095u) & ~(size_t)4095u;
        while (content_snapshot_bytes + allocated > CONTENT_WATCH_BUDGET) {
            content_watch_drop(content_evict++ % CONTENT_WATCH_SLOTS);
        }
        content_watch[slot].snapshot = VirtualAlloc(NULL, allocated, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!content_watch[slot].snapshot) return ++content_generation;
        content_watch[slot].address = address;
        content_watch[slot].size = size;
        content_watch[slot].allocated = allocated;
        content_snapshot_bytes += allocated;
        memcpy(content_watch[slot].snapshot, data, size);
        content_watch[slot].generation = ++content_generation;
    } else if (memcmp(content_watch[slot].snapshot, data, size)) {
        memcpy(content_watch[slot].snapshot, data, size);
        content_watch[slot].generation = ++content_generation;
    }
    return content_watch[slot].generation;
}
/* Every lookup must inspect the bytes; writes can occur between draws. */
unsigned long memory_watch_serial(void)
{
    if (page_watch_enabled())
        return page_watch_serial();
    return ++content_lookup;
}
void memory_watch_prepare_write(void *address, unsigned long size)
{
    if (page_watch_enabled())
        page_watch_prepare_write(address, size);
}
void memory_watch_forget(void *address, unsigned long size)
{
    unsigned slot;
    if (page_watch_enabled()) {
        page_watch_forget(address, size);
        return;
    }
    uintptr_t first = (uintptr_t)address, last = first + size;
    for (slot = 0; slot < CONTENT_WATCH_SLOTS; slot++) {
        uintptr_t begin = content_watch[slot].address;
        if (content_watch[slot].snapshot && begin < last && begin + content_watch[slot].size > first)
            content_watch_drop(slot);
    }
}
void memory_watch_begin_frame(void) {}

/* ---------- settings ---------- */
const char *config_string(const char *name)
{
    if (!strcmp(name, "debug.screenshot_directory")) {
        const char *value = getenv("RECOMP_WIDE_CAPTURE_DIR");
        return value ? value : "";
    }
    if (!strcmp(name, "debug.texture_dump_directory")) {
        const char *value=getenv("RECOMP_D3D8GL_DUMP_TEXTURES");
        return value?value:"";
    }
    if (!strcmp(name, "display.anti_aliasing")) return "off";
    if (!strcmp(name, "display.model_detail")) return "stock";
    if (!strcmp(name, "display.hdr_tone_mapping")) return "linear";
    if (!strcmp(name, "debug.gpu_skip_vertex_shaders")) return "";
    if (!strcmp(name, "debug.gpu_dump_shaders")) {
        const char *value = getenv("RECOMP_D3D8GL_DUMP_SHADERS");
        return value ? value : "";
    }
    return "";
}
long config_integer(const char *name)
{
    if (!strcmp(name, "debug.gpu_trace_frame")) {
        const char *value = getenv("RECOMP_D3D8GL_TRACE_FRAME");
        return value ? strtol(value, NULL, 10) : -1;
    }
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
    if (!strcmp(name, "debug.gl_debug")) return getenv("RECOMP_D3D8GL_GL_DEBUG") != NULL;
    if (!strcmp(name, "debug.gpu_trace_constants")) return getenv("RECOMP_D3D8GL_TRACE_FRAME") != NULL;
    (void)name;
    return FALSE;
}
unsigned long config_changes(void) { return 0; }
int noble_d3d8_trace_enabled(void)
{
    static int enabled=-1;
    if(enabled<0) enabled=getenv("RECOMP_D3D8GL_TRACE")!=NULL;
    return enabled;
}

/* ---------- Halo screen policy: the title's native 640x480 ---------- */

/* ---------- window / OpenGL ---------- */
static SDL_Window *g_window;
static SDL_GLContext g_context;
static void (*g_host_input_pump)(void);

__declspec(dllexport) void noble_d3d8_set_input_pump(void (*pump)(void))
{
    g_host_input_pump = pump;
}

BOOL platform_video_initialize(unsigned long width, unsigned long height)
{
    int window_width = (int)width;
    int window_height = (int)height;
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
    if (width == 640 && height == 480 && noble_wide_window_aspect() > 4.0f / 3.0f) {
        const char *setting = getenv("RECOMP_WIDE_WINDOW_HEIGHT");
        if (setting && *setting) {
            char *end;
            long parsed = strtol(setting, &end, 10);
            if (end != setting && !*end && parsed >= 360 && parsed <= 2160)
                window_height = (int)parsed;
        }
        window_width = noble_wide_window_width((int)width, window_height, noble_wide_window_aspect());
    }
    g_window = SDL_CreateWindow("Castlevania CoD | D3D8GL", window_width, window_height,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                (getenv("RECOMP_WIDE_PROBE") ? SDL_WINDOW_HIDDEN : 0));
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
    if (g_host_input_pump) {
        g_host_input_pump();
        return;
    }
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

