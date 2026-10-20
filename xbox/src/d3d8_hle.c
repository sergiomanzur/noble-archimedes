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
typedef void (*offset_fn)(ptrdiff_t);
typedef void (*transform_fn)(unsigned, unsigned);
typedef void (*draw_up_fn)(unsigned, unsigned, unsigned, unsigned);
typedef void (*draw_indexed_up_fn)(unsigned, unsigned, unsigned, unsigned, unsigned);
typedef int (*readback_fn)(unsigned);
typedef void (*pair_fn)(unsigned, unsigned);
typedef void (*vertex2_fn)(unsigned, unsigned, unsigned);
typedef void (*vertex4_fn)(unsigned, unsigned, unsigned, unsigned, unsigned);
typedef int (*fixed_function_fn)(unsigned, unsigned, unsigned);
typedef int (*copy_rects_fn)(unsigned,unsigned,unsigned,unsigned,unsigned);

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
static offset_fn p_set_guest_offset;
static transform_fn p_transform;
static draw_up_fn p_draw_up;
static draw_indexed_up_fn p_draw_indexed_up;
static readback_fn p_readback;
static vertex_shader_fn p_begin;
static scene_fn p_end;
static vertex2_fn p_vertex2f;
static vertex4_fn p_vertex4f;
static pair_fn p_vertex_color;
static pair_fn p_load_vertex_shader;
static pair_fn p_select_vertex_shader;
static fixed_function_fn p_fixed_function;
static copy_rects_fn p_copy_rects;
static int g_host_ready;
static __declspec(thread) int g_guest_only;
static int hle_trace_enabled(void)
{
    static int enabled=-1;
    if(enabled<0) enabled=getenv("RECOMP_D3D8GL_TRACE")!=NULL;
    return enabled;
}
static double g_hle_cost[5];
static double hle_time(void)
{
    static int enabled=-1;
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    if(enabled<0) {
        enabled=getenv("RECOMP_GPU_STATS")!=NULL;
        QueryPerformanceFrequency(&frequency);
    }
    if(!enabled) return 0;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart*1000.0/frequency.QuadPart;
}

extern void sub_003BB4C0(void);
extern void sub_003B86E0(void);
extern void sub_003C3D10(void);
extern void xbox_FramebufferWindowSet(uint32_t, uint32_t);
extern void xbox_FramebufferWindowPresent(uint32_t, uint32_t);
extern void xbox_FramebufferWindowSetAA(uint32_t);
extern void xbox_FramebufferWindowStart(void);

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
    LOAD(set_guest_offset, offset_fn);
    LOAD(transform, transform_fn);
    LOAD(draw_up, draw_up_fn);
    LOAD(draw_indexed_up, draw_indexed_up_fn);
    LOAD(readback, readback_fn);
    LOAD(begin, vertex_shader_fn);
    LOAD(end, scene_fn);
    LOAD(vertex2f, vertex2_fn);
    LOAD(vertex4f, vertex4_fn);
    LOAD(vertex_color, pair_fn);
    LOAD(load_vertex_shader, pair_fn);
    LOAD(select_vertex_shader, pair_fn);
    LOAD(fixed_function, fixed_function_fn);
    LOAD(copy_rects,copy_rects_fn);
#undef LOAD
    g_ready = 1;
    if (hle_trace_enabled()) {
        char path[MAX_PATH];
        HMODULE by_addr = NULL;
        FARPROC exp = GetProcAddress(g_module, "noble_d3d8_draw_up");
        GetModuleFileNameA(g_module, path, sizeof path);
        GetModuleHandleExA(0x00000004 /* FROM_ADDRESS */, (LPCSTR)p_draw_up, &by_addr);
        fprintf(stderr, "[D3D8HLE] module=%s draw_up=%p export=%p addr_base=%p offset=%llX\n",
                path, (void *)p_draw_up, (void *)exp, (void *)by_addr,
                (unsigned long long)((uintptr_t)p_draw_up - (uintptr_t)by_addr));
    }
    return 1;
}

int noble_d3d8_init(void)
{
    fprintf(stderr, "[D3D8HLE] init: loading dll\n");
    if (!load_dll())
        return 0;
    p_set_guest_offset(g_xbox_mem_offset);
    fprintf(stderr, "[D3D8HLE] init: calling host create\n");
    if (!p_init()) {
        fprintf(stderr, "[D3D8HLE] init: host create failed\n");
        return 0;
    }
    fprintf(stderr, "[D3D8HLE] init: ready\n");
    g_host_ready = 1;
    xbox_FramebufferWindowStart();
    return 1;
}

static uint32_t arg(unsigned index)
{
    return MEM32(g_esp + 4 + index * 4);
}

/* Keep guest shader handles separate from the host's private objects. */
#define SHADER_SLOTS 512
static struct { uint32_t guest, host; } g_shaders[SHADER_SLOTS];
static unsigned g_shader_count;

static unsigned host_shader(unsigned guest)
{
    unsigned i;
    if (!(guest & 1)) return guest;
    for (i=0;i<g_shader_count;i++)
        if (g_shaders[i].guest==guest) return g_shaders[i].host;
    fprintf(stderr,"[D3D8HLE] unmapped guest vertex shader %08X\n",guest);
    return 0;
}

/* The guest stores programmable PS state in a shadow array, but compiles
 * fixed-function TSS directly into pushbuffer methods only at draw time.
 * Run that state compiler in isolated guest-stack scratch; do not submit a
 * guest draw or change the real pushbuffer cursor. */
static int prepare_pixel_shader(uint32_t device)
{
    uint32_t ps=MEM32(device+0x784);
    if(ps) {
        p_pixel_shader(MEM32(ps+8));
        return 1;
    }
    {
        uint32_t regs[]={g_eax,g_ecx,g_edx,g_ebx,g_esi,g_edi,g_esp,g_ebp,g_seh_ebp};
        uint32_t cursor=MEM32(device),limit=MEM32(device+4),flags=MEM32(device+8);
        uint32_t scratch=g_esp-0x400, end, modes=0;
        unsigned stage;
        int ok;
        MEM32(device)=scratch; MEM32(device+4)=scratch+0x100;
        g_esp-=0x100;
        PUSH32(g_esp,MEM32(0x003C6AF8u));
        PUSH32(g_esp,device);
        PUSH32(g_esp,0);
        g_guest_only=1;
        sub_003C3D10();
        g_guest_only=0;
        end=MEM32(device);
        MEM32(device)=cursor; MEM32(device+4)=limit; MEM32(device+8)=flags;
        g_eax=regs[0];g_ecx=regs[1];g_edx=regs[2];g_ebx=regs[3];g_esi=regs[4];
        g_edi=regs[5];g_esp=regs[6];g_ebp=regs[7];g_seh_ebp=regs[8];
        for(stage=0;stage<4;stage++) {
            uint32_t texture=MEM32(device+0xF88+4*stage);
            if(texture) modes|=((MEM32(texture+12)&4)?3u:1u)<<(5*stage);
        }
        ok=end>=scratch && end<=scratch+0x100 && !((end-scratch)&3) &&
            p_fixed_function(scratch,(end-scratch)/4,modes);
        return ok;
    }
}

int noble_d3d8_guest_call(uint32_t va, uint32_t cleanup)
{
    static int enabled = -1;
    uint32_t device;
    double submit_start;
    if (enabled < 0) {
        const char *value=getenv("RECOMP_D3D8GL");
        enabled=value && *value && strcmp(value,"0");
    }
    if (!enabled || g_guest_only) return 0;

    if (va==0x003BB4C0u) {
        /* Initialize fully before claiming any rendering work. Reentry runs
         * the original guest implementation with interception disabled. */
        g_guest_only=1;
        sub_003BB4C0();
        g_guest_only=0;
        if ((int32_t)g_eax>=0 && MEM32(0x003C6F98u) && noble_d3d8_init()) {
            device=MEM32(0x003C6F98u);
            p_render_target(MEM32(device+0x1A04),MEM32(device+0x1A08));
            p_viewport(device+0xEE0);
            p_vertex_shader(host_shader(MEM32(device+0x798)));
            fprintf(stderr,"[D3D8HLE] guest device ready; deterministic handoff\n");
        }
        return 1; /* Guest already cleaned its own stack. */
    }
    if (!g_host_ready) return 0;
    device=MEM32(0x003C6F98u);
    if (!device) return 0;
    /* Draws must retain raw constant uploads, which need not update the
     * guest shadow. Embedded constants are synchronized at shader loading. */
    if (va==0x003BA740u || va==0x003BA8B0u || va==0x003BA9F0u || va==0x003BAD40u) {
        double prepare_start=hle_time();
        if(!prepare_pixel_shader(device)) {
            fprintf(stderr,"[D3D8HLE] unsupported fixed-function state; stopping instead of discarding a draw\n");
            ExitProcess(1);
        }
        g_hle_cost[3]+=hle_time()-prepare_start;
        p_viewport(device+0xEE0); /* Recompute the reserved viewport constants. */
        if (hle_trace_enabled()) {
            static unsigned draws;
            if (draws++<5) {
                uint32_t ps=MEM32(device+0x784);
                fprintf(stderr,"[D3D8HLE] draw state flags=%08X ps=%08X def=%08X tex=%08X,%08X,%08X,%08X\n",
                    MEM32(device+8),ps,ps?MEM32(ps+8):0,
                    MEM32(device+0xF88),MEM32(device+0xF8C),MEM32(device+0xF90),MEM32(device+0xF94));
            }
        }
    }
    {
        static uint32_t seen[32];
        static unsigned count;
        unsigned i;
        for (i=0;i<count && seen[i]!=va;i++) {}
        if (i==count && count<32) {
            seen[count++]=va;
            fprintf(stderr,"[D3D8HLE] entry=%08X args=%08X,%08X,%08X,%08X ecx=%08X edx=%08X shader=%08X maps=%u\n",
                    va,arg(0),arg(1),arg(2),arg(3),g_ecx,g_edx,
                    MEM32(device+0x798),g_shader_count);
        }
    }

    /* Mirror setters, then let the guest maintain references and shadow data.
     * Only complete draw/clear operations are suppressed. */
    submit_start=hle_time();
    switch (va) {
    case 0x003B6520u: { /* Swap(flags), NOT SetRenderTarget. */
        static unsigned trace_swaps;
        uint32_t surface=MEM32(device+0x1A04);
        int readback=p_readback(surface);
        g_hle_cost[0]+=hle_time()-submit_start;
        if (trace_swaps++<3)
            fprintf(stderr,"[D3D8HLE] swap surface=%08X data=%08X format=%08X size=%08X readback=%d\n",
                    surface,MEM32(surface+4),MEM32(surface+12),MEM32(surface+16),readback);
        if (readback) {
            uint32_t data=MEM32(surface+4), size=MEM32(surface+16);
            uint32_t pitch=((size>>24)+1)*64;
            uint32_t width=(size&0xFFF)+1, height=((size>>12)&0xFFF)+1;
            /* Without guest draws the executor has no drawn surface to
             * publish. The guest Swap still supplies its usual flip stats. */
            if (width==640 && height==480 && pitch==640*4) {
                if (data<0x04000000) data|=0x80000000;
                xbox_FramebufferWindowSet(data,pitch);
                xbox_FramebufferWindowSetAA(1);
                xbox_FramebufferWindowPresent(data,pitch);
            }
        }
        {
            static unsigned measured_frames;
            double present_start=hle_time();
            unsigned i;
            p_present();
            g_hle_cost[1]+=hle_time()-present_start;
            if(++measured_frames==120 && present_start) {
                fprintf(stderr,"[HLE-PERF] readback=%.2f present=%.2f submit=%.2f prepare=%.2f copy=%.2f ms/frame\n",
                    g_hle_cost[0]/120,g_hle_cost[1]/120,g_hle_cost[2]/120,g_hle_cost[3]/120,g_hle_cost[4]/120);
                for(i=0;i<5;i++) g_hle_cost[i]=0;
                measured_frames=0;
            }
        }
        return 0; /* Guest buffer rotation, counter and pacing run once. */
    }
    case 0x003B6FB0u: p_render_target(arg(0),arg(1)); return 0;
    case 0x003B7280u: {
        static unsigned copies;
        int copied=p_copy_rects(arg(0),arg(1),arg(2),arg(3),arg(4));
        g_hle_cost[4]+=hle_time()-submit_start;
        if(copies++<3) fprintf(stderr,"[D3D8HLE] copy src=%08X dst=%08X count=%u gpu=%d\n",
            MEM32(arg(0)+4),MEM32(arg(3)+4),arg(2),copied);
        if(!copied) return 0;
        break;
    }
    case 0x003B75F0u: p_viewport(arg(0)); return 0;
    case 0x003B6B80u: p_transform(arg(0),arg(1)); return 0;
    case 0x003B7820u: p_texture(arg(0),arg(1)); return 0;
    case 0x003B79D0u: p_palette(arg(0),arg(1)); return 0;
    case 0x003B8A50u: p_stream(arg(0),arg(1),arg(2)); return 0;
    case 0x003B7A60u: p_indices(arg(0),arg(1)); return 0;
    case 0x003B8820u:
        p_vertex_constants((int)g_ecx-96,g_edx,1); return 0;
    case 0x003B8880u:
        p_vertex_constants((int)g_ecx-96,g_edx,4); return 0;
    case 0x003B8930u:
        if (!(arg(0)&3)) p_vertex_constants((int)g_ecx-96,g_edx,arg(0)/4);
        return 0;
    case 0x003B86E0u: {
        uint32_t declaration=arg(0),function=arg(1),out=arg(2),usage=arg(3);
        uint32_t host=p_create_vertex_shader(declaration,function,usage);
        g_guest_only=1;
        sub_003B86E0();
        g_guest_only=0;
        if ((int32_t)g_eax>=0 && out && host && g_shader_count<SHADER_SLOTS) {
            g_shaders[g_shader_count].guest=MEM32(out);
            g_shaders[g_shader_count++].host=host;
        }
        return 1;
    }
    case 0x003B8E30u: p_vertex_shader(host_shader(arg(0))); return 0;
    case 0x003B8AE0u: p_load_vertex_shader(host_shader(arg(0)),arg(1)); return 0;
    case 0x003B8B40u: p_select_vertex_shader(host_shader(arg(0)),arg(1)); return 0;
    case 0x003BAE20u: /* Guest shader object owns the definition at +8. */
        p_pixel_shader(arg(0)?MEM32(arg(0)+8):0); return 0;
    case 0x003B66D0u: {
        uint32_t bits=arg(4);
        float z;
        memcpy(&z,&bits,sizeof z);
        p_clear(arg(0),arg(1),arg(2),arg(3),z,arg(5));
        break;
    }
    case 0x003BA740u:
        if (hle_trace_enabled())
            fprintf(stderr,"[D3D8HLE] DRAW_UP prim=%u count=%u data=%08X stride=%u\n",
                    arg(0),arg(1),arg(2),arg(3));
        if (hle_trace_enabled())
            fprintf(stderr,"[D3D8HLE] DRAW_UP host=%p before\n",(void*)p_draw_up);
        p_draw_up(arg(0),arg(1),arg(2),arg(3));
        if (hle_trace_enabled())
            fprintf(stderr,"[D3D8HLE] DRAW_UP after\n");
        break;
    case 0x003BA8B0u:
        if (hle_trace_enabled())
            fprintf(stderr,"[D3D8HLE] DRAW_IDX_UP prim=%u count=%u idx=%08X data=%08X stride=%u\n",
                    arg(0),arg(1),arg(2),arg(3),arg(4));
        p_draw_indexed_up(arg(0),arg(1),arg(2),arg(3),arg(4));
        break;
    case 0x003BA9F0u:
        if (hle_trace_enabled())
            fprintf(stderr,"[D3D8HLE] DRAW_IDX prim=%u count=%u idx=%08X\n",
                    arg(0),arg(1),arg(2));
        p_draw_indexed(arg(0),arg(1),arg(2));
        break;
    case 0x003BAD40u: p_begin(arg(0)); return 0; /* Guest Begin/End maintain device flags. */
    case 0x003BAD80u: p_end(); return 0;
    case 0x003BAC50u: p_vertex2f(arg(0),arg(1),arg(2)); break;
    case 0x003BAC90u: p_vertex4f(arg(0),arg(1),arg(2),arg(3),arg(4)); break;
    case 0x003BACF0u: p_vertex_color(arg(0),arg(1)); break;
    default: return 0; /* Includes SetTile and raw method emission. */
    }
    g_eax=0;
    if(va!=0x003B7280u) g_hle_cost[2]+=hle_time()-submit_start;
    g_esp+=cleanup;
    return 1;
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
