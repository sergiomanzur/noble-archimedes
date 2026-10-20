/*
 * D3D8 high-level emulation over the OpenCE/OpenGL device.
 *
 * The title's statically linked Xbox D3D8 library is translated.  Its
 * low-level D3DDevice_* entry points are redirected here; the OpenCE device
 * (d3d8gl.dll) consumes the guest render state arrays and the guest vertex,
 * texture and shader objects directly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "recomp_types.h"
#include "d3d8_hle.h"

typedef int  (*init_fn)(void);
typedef void (*clear_fn)(unsigned, unsigned, unsigned, unsigned, float, unsigned);
typedef void (*present_fn)(void);
typedef void (*texture_fn)(unsigned, unsigned);
typedef void (*palette_fn)(unsigned, unsigned);
typedef void (*constants_fn)(int, unsigned, unsigned);
typedef void (*stream_fn)(unsigned, unsigned, unsigned);
typedef void (*indices_fn)(unsigned, unsigned);
typedef void (*vertex_shader_fn)(unsigned);
typedef unsigned (*create_vsh_fn)(unsigned, unsigned, unsigned);
typedef void (*pixel_shader_fn)(unsigned);
typedef void (*draw_fn)(unsigned, unsigned, unsigned);
typedef void (*draw_indexed_fn)(unsigned, unsigned, unsigned);
typedef void (*scene_fn)(void);
typedef void (*viewport_fn)(unsigned);
typedef void (*render_target_fn)(unsigned, unsigned);

static HMODULE g_module;
static int g_ready;
static int g_attempted;

static init_fn p_init;
static clear_fn p_clear;
static present_fn p_present;
static texture_fn p_texture;
static palette_fn p_palette;
static constants_fn p_vertex_constants;
static stream_fn p_stream;
static indices_fn p_indices;
static vertex_shader_fn p_vertex_shader;
static create_vsh_fn p_create_vertex_shader;
static pixel_shader_fn p_pixel_shader;
static draw_fn p_draw;
static draw_indexed_fn p_draw_indexed;
static scene_fn p_begin_scene;
static scene_fn p_end_scene;
static viewport_fn p_viewport;
static render_target_fn p_render_target;

static int load_dll(void)
{
    if (g_attempted)
        return g_ready;
    g_attempted = 1;
    g_module = LoadLibraryA("d3d8gl.dll");
    if (!g_module)
        g_module = LoadLibraryA("output\\d3d8gl.dll");
    if (!g_module) {
        fprintf(stderr, "[D3D8HLE] cannot load d3d8gl.dll (error %lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }
#define LOAD(name, type) \
    do { p_##name = (type)GetProcAddress(g_module, "noble_d3d8_" #name); \
         if (!p_##name) { \
             fprintf(stderr, "[D3D8HLE] missing noble_d3d8_%s\n", #name); \
             return 0; \
         } } while (0)
    LOAD(init, init_fn);
    LOAD(clear, clear_fn);
    LOAD(present, present_fn);
    LOAD(texture, texture_fn);
    LOAD(palette, palette_fn);
    LOAD(vertex_constants, constants_fn);
    LOAD(stream, stream_fn);
    LOAD(indices, indices_fn);
    LOAD(vertex_shader, vertex_shader_fn);
    LOAD(create_vertex_shader, create_vsh_fn);
    LOAD(pixel_shader, pixel_shader_fn);
    LOAD(draw, draw_fn);
    LOAD(draw_indexed, draw_indexed_fn);
    LOAD(begin_scene, scene_fn);
    LOAD(end_scene, scene_fn);
    LOAD(viewport, viewport_fn);
    LOAD(render_target, render_target_fn);
#undef LOAD
    g_ready = 1;
    return 1;
}

int noble_d3d8_init(void)
{
    fprintf(stderr, "[D3D8HLE] init: loading dll\n");
    if (!load_dll())
        return 0;
    fprintf(stderr, "[D3D8HLE] init: calling host create\n");
    if (!p_init()) {
        fprintf(stderr, "[D3D8HLE] init: host create failed\n");
        return 0;
    }
    fprintf(stderr, "[D3D8HLE] init: ready\n");
    return 1;
}

static uint32_t arg(unsigned index)
{
    return MEM32(g_esp + 4 + index * 4);
}

static void log_call_once(uint32_t va, int slots)
{
    static uint32_t seen[32];
    static unsigned count;
    unsigned i;
    uint32_t device = MEM32(0x003C6F98u);
    for (i = 0; i < count; i++)
        if (seen[i] == va)
            return;
    if (count >= 32)
        return;
    seen[count++] = va;
    fprintf(stderr, "[D3D8DBG] %08X ret=%08X esp=%08X ecx=%08X edx=%08X",
            va, MEM32(g_esp), g_esp, g_ecx, g_edx);
    for (i = 0; i < (unsigned)slots; i++)
        fprintf(stderr, " a%u=%08X", i, arg(i));
    if (device)
        fprintf(stderr, " dev=%08X bcc=%08X bd0=%08X c54=%08X",
                device, MEM32(device + 0xBCC), MEM32(device + 0xBD0),
                MEM32(device + 0xC54));
    fputc('\n', stderr);
}

static void set_result(uint32_t value)
{
    g_eax = value;
}

static void handled(uint32_t cleanup)
{
    g_esp += cleanup;
}

int noble_d3d8_guest_call(uint32_t va, uint32_t cleanup)
{
    static int enabled = -1;
    static int hle_active;
    static DWORD hle_start;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_D3D8GL");
        enabled = value && *value && strcmp(value, "0") ? 1 : 0;
    }
    if (!enabled)
        return 0;


    static uint32_t seen[64];
    static unsigned seen_count;
    unsigned i;

    if (!load_dll())
        return 0;

    /* Device initialization calls many of these entry points before the first
     * frame.  Leave the guest implementation in charge until a clear marks the
     * frame boundary; from then on the host device consumes state. */
    if (!hle_start)
        hle_start = GetTickCount();
    if (!hle_active && GetTickCount() - hle_start > 10000)
        hle_active = 1;
    if (!hle_active && va != 0x003B66D0u && va != 0x003B6FB0u)
        return 0;

    for (i = 0; i < seen_count; i++)
        if (seen[i] == va)
            break;
    if (i == seen_count && seen_count < 64) {
        seen[seen_count++] = va;
        fprintf(stderr, "[D3D8HLE] first call %08X\n", va);
    }

    switch (va) {
    case 0x003B66D0u: { /* Clear(count, rects, flags, color, z, stencil) */
        uint32_t z_bits = arg(4);
        float z;
        fprintf(stderr, "[D3D8HLE] clear: ensure host\n");
        if (!p_init())
            return 0;
        memcpy(&z, &z_bits, sizeof(z));
        p_clear(arg(0), arg(1), arg(2), arg(3), z, arg(5));
        set_result(0);
        handled(cleanup);
        return 1;
    }

    case 0x003B6FB0u: /* Present */
        p_present();
        set_result(0);
        handled(cleanup);
        return 1;

    case 0x003B6520u: /* BeginScene / frame separator */
        set_result(0);
        handled(cleanup);
        return 1;

    case 0x003B7820u: /* SetTexture(stage, texture) */
        p_texture(arg(0), arg(1));
        set_result(0);
        handled(cleanup);
        return 1;

    case 0x003B79D0u: /* SetPalette(stage, palette) */
        p_palette(arg(0), arg(1));
        set_result(0);
        handled(cleanup);
        return 1;

    case 0x003B8820u: /* SetVertexShaderConstant(reg, data, 1) */
        p_vertex_constants((int)g_ecx, g_edx, 1);
        set_result(0);
        handled(cleanup);
        return 1;

    case 0x003B8880u: /* SetVertexShaderConstant(reg, data, 4) */
        p_vertex_constants((int)g_ecx, g_edx, 4);
        set_result(0);
        handled(cleanup);
        return 1;

    case 0x003B7B60u: { /* SetStreamSource(stream, vertex_buffer) */
        uint32_t vb = arg(1);
        log_call_once(0x003B7B60u, 2);
        return 0;
    }

    case 0x003B86E0u: { /* CreateVertexShader(declaration, function, handle, usage) */
        uint32_t handle = 0;
        log_call_once(0x003B86E0u, 4);
        uint32_t out = arg(2);
        handle = p_create_vertex_shader(arg(0), arg(1), arg(3));
        if (out)
            MEM32(out) = handle;
        set_result(0);
        handled(cleanup);
        return 1;
    }

    case 0x003B8E30u: /* SetVertexShader(handle) */
        p_vertex_shader(arg(0));
        set_result(0);
        handled(cleanup);
        return 1;

    case 0x003BAE20u: /* SetPixelShaderProgram */
        p_pixel_shader(arg(0));
        set_result(0);
        handled(cleanup);
        return 1;

    case 0x003BA740u: { /* log the first draws and leave the guest path */
        static unsigned draw_logs;
        uint32_t device = MEM32(0x003C6F98u);
        uint32_t a0 = arg(0), a1 = arg(1), a2 = arg(2), a3 = arg(3);
        if (draw_logs < 12) {
            unsigned st;
            draw_logs++;
            fprintf(stderr, "[D3D8DRAW] t=%lu esp=%08X a=%08X,%08X,%08X,%08X dev=%08X",
                    (unsigned long)GetTickCount(), g_esp, a0, a1, a2, a3, device);
            for (st = 0; st < 4; st++) {
                uint32_t e = device + 0x1AC0 + st * 24;
                fprintf(stderr, " s%u=%08X/%08X/%08X/%08X/%08X/%08X", st,
                        MEM32(e), MEM32(e + 4), MEM32(e + 8), MEM32(e + 12),
                        MEM32(e + 16), MEM32(e + 20));
            }
            if (a2)
                fprintf(stderr, " vb=%08X/%08X/%08X/%08X/%08X/%08X",
                        MEM32(a2), MEM32(a2 + 4), MEM32(a2 + 8),
                        MEM32(a2 + 12), MEM32(a2 + 16), MEM32(a2 + 20));
            fputc('\n', stderr);
        }
        return 0;
    }

    case 0x003B8FD0u: /* simple render state: the game updated the guest array */
        set_result(0);
        handled(cleanup);
        return 1;

    default:
        return 0;
    }
}





/*
 * The guest D3D8 GetDisplayFieldStatus.  The XMV decoder waits for an odd
 * display field (device + 0x1DF8 semaphore) before starting the movie clock.
 * This port presents progressive, so report Field = 3 and start immediately.
 * Mirrors BLACK-2006-recomp's field override.
 */
int noble_field_status_hook(void)
{
    extern uint32_t xbox_VblankCount(void);
    uint32_t out = MEM32(g_esp + 4);
    if (out) {
        MEM32(out) = 3;
        /* The guest D3D counter is advanced by GPU callbacks which this host
         * does not execute. XMV schedules frames against this 60 Hz clock. */
        MEM32(out + 4) = xbox_VblankCount();
    }
    g_esp += 8; /* ret 4 */
    return 1;
}
