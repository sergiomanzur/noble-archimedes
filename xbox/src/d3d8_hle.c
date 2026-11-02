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
extern unsigned recomp_timeline_swap_begin(void);
extern void recomp_timeline_swap_end(void);
extern unsigned recomp_timeline_frame(void);
extern void recomp_timeline_event(const char *kind, const char *detail,
                                  double value0, double value1);
extern void recomp_rcap_snapshot(const char *type, uint32_t addr, uint32_t size,
                                 uint32_t a0, uint32_t a1, uint32_t a2);
extern void recomp_rcap_state(const char *type, uint32_t a0, uint32_t a1,
                              uint32_t a2);
extern void recomp_rcap_sync(const char *name);
extern void recomp_rcap_unsupported(const char *name);
extern void recomp_rcap_measure(const char *type, uint32_t addr, uint32_t size);
extern void recomp_rcap_require(const char *type, uint32_t size);
extern void recomp_rcap_texture_bind(uint32_t stage, uint32_t resource);
extern int recomp_rcap_active(void);
extern void recomp_rcap_renderer_bytes(uint64_t content, uint64_t upload);
extern void recomp_rcap_texture_frame(uint32_t frame);
extern uint32_t xbox_FramebufferFlipCount(void);
extern int recomp_guest_profile_stack(uint32_t *out, int capacity);

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
typedef void (*gpu_begin_fn)(void);
typedef double (*gpu_end_fn)(void);
typedef void (*shader_stats_fn)(unsigned long *, unsigned long *, unsigned long *);
typedef int (*gpu_poll_fn)(unsigned long *, double *);
typedef unsigned long (*texture_bytes_fn)(const uint32_t *resource);
typedef int (*texture_manifest_fn)(const uint32_t *resource, uint32_t *out);
typedef void (*texture_stats_fn)(unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *);
typedef unsigned long (*texture_frame_fn)(void);
typedef void (*upload_stats_fn)(unsigned long long *, unsigned long long *, unsigned long long *, double *);
typedef void (*readback_stats_fn)(unsigned long long *, double *, double *, double *);
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
static readback_fn p_readback_swap;
static gpu_begin_fn p_gpu_frame_begin;
static gpu_end_fn p_gpu_frame_end;
static shader_stats_fn p_shader_stats;
static gpu_poll_fn p_gpu_frame_poll;
static texture_stats_fn p_texture_stats;
static texture_bytes_fn p_texture_bytes;
static texture_manifest_fn p_texture_manifest;
static texture_frame_fn p_texture_frame;
static upload_stats_fn p_upload_stats;
static readback_stats_fn p_readback_stats;
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
static void (*g_host_input_pump)(void);
void noble_d3d8_set_host_input_pump(void (*pump)(void))
{
    g_host_input_pump = pump;
}
static __declspec(thread) int g_guest_only;
static uint32_t cap_stream_ptr, cap_stream_stride;
static void capture_indexed_vdata(uint32_t idx_ptr, uint32_t count)
{
    uint32_t i, max_index = 0, bytes;
    if (!cap_stream_ptr || !cap_stream_stride)
        return;
    for (i = 0; i < count; i++) {
        uint16_t v = MEM16(idx_ptr + i * 2u);
        if (v > max_index) max_index = v;
    }
    bytes = (max_index + 1u) * cap_stream_stride;
    recomp_rcap_snapshot("indexed_vdata", cap_stream_ptr, bytes, 0, 0, 0);
}
static int hle_trace_enabled(void)
{
    static int enabled=-1;
    if(enabled<0) enabled=getenv("RECOMP_D3D8GL_TRACE")!=NULL;
    return enabled;
}
static double g_hle_cost[20];
static unsigned g_hle_draw_up_calls, g_hle_draw_indexed_up_calls;
static unsigned g_hle_draw_calls, g_hle_draw_indexed_calls, g_hle_clear_calls;
static unsigned g_hle_create_shader_calls, g_hle_pixel_shader_calls;
static double g_hle_gpu_ms;
static double hle_time(void)
{
    static int enabled=-1;
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    if(enabled<0) {
        enabled=getenv("RECOMP_HLE_STATS")!=NULL;
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

static int gpu_query_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("RECOMP_GPU_QUERY") != NULL;
    return enabled;
}

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
    {
        typedef void (*set_pump_fn)(void (*)(void));
        set_pump_fn set_pump = (set_pump_fn)GetProcAddress(
            g_module, "noble_d3d8_set_input_pump");
        if (set_pump) set_pump(g_host_input_pump);
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
    LOAD(readback_swap, readback_fn);
    LOAD(gpu_frame_begin, gpu_begin_fn);
    LOAD(gpu_frame_end, gpu_end_fn);
    LOAD(shader_stats, shader_stats_fn);
    LOAD(gpu_frame_poll, gpu_poll_fn);
    p_texture_bytes = (texture_bytes_fn)GetProcAddress(g_module,
                                                       "noble_d3d8_texture_bytes");
    p_texture_manifest = (texture_manifest_fn)GetProcAddress(g_module,
                                                              "noble_d3d8_texture_manifest");
    LOAD(texture_stats, texture_stats_fn);
    LOAD(texture_frame, texture_frame_fn);
    LOAD(upload_stats, upload_stats_fn);
    LOAD(readback_stats, readback_stats_fn);
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

static void wide_probe_draw(uint32_t va, uint32_t device)
{
    static int initialized, enabled;
    static unsigned frames[8], count;
    unsigned i, flip;
    if (!initialized) {
        const char *p = getenv("RECOMP_WIDE_GUEST_TRACE");
        initialized = 1;
        while (p && *p && count < 8) {
            char *end;
            unsigned long frame = strtoul(p, &end, 10);
            if (end == p) break;
            frames[count++] = (unsigned)frame;
            if (*end != ',') break;
            p = end + 1;
        }
        enabled = count != 0;
    }
    if (!enabled) return;
    flip = xbox_FramebufferFlipCount();
    for (i = 0; i < count; i++) {
        if (flip + 1 >= frames[i] && flip <= frames[i] + 1) {
            uint32_t depth = MEM32(0x00A4E2D8u);
            uint32_t state = depth < 64 ? MEM32(0x00A4E2DCu + depth * 4) : 0xffffffffu;
            uint32_t fp = g_ebp, parent = 0, grandparent = 0;
            if (fp >= 0x10000u && fp < 0x03fffff8u) {
                uint32_t previous = MEM32(fp);
                parent = MEM32(fp + 4);
                if (previous >= 0x10000u && previous < 0x03fffff8u && previous > fp)
                    grandparent = MEM32(previous + 4);
            }
            fprintf(stderr, "[WIDE-GUEST] flip=%u entry=%08X caller=%08X shader=%08X depth=%u state=%u parent=%08X grandparent=%08X\n",
                    flip, va, MEM32(g_esp), MEM32(device + 0x798), depth, state, parent, grandparent);
            {
                uint32_t stack[32];
                int j, n = recomp_guest_profile_stack(stack, 32);
                if (n) {
                    fprintf(stderr, "[WIDE-STACK] flip=%u caller=%08X", flip, MEM32(g_esp));
                    for (j = 0; j < n; ++j) fprintf(stderr, " %08X", stack[j]);
                    fputc('\n', stderr);
                }
            }
            break;
        }
    }
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
    /* Swap entry is guest/main-thread owned on both renderer paths. The GPU
     * consumer's pacing callback is deliberately not used for SDL events. */
    if (va == 0x003B6520u && g_host_input_pump) g_host_input_pump();
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
        wide_probe_draw(va, device);
        recomp_timeline_event("prepare_begin", "pixel_shader", 0, 0);
        recomp_rcap_state("prepare", 0, 0, 0);
        if(!prepare_pixel_shader(device)) {
            fprintf(stderr,"[D3D8HLE] unsupported fixed-function state; stopping instead of discarding a draw\n");
            ExitProcess(1);
        }
        g_hle_cost[3]+=hle_time()-prepare_start;
        recomp_timeline_event("prepare_end", "pixel_shader", 0, 0);
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
        recomp_timeline_swap_begin();
        uint32_t surface=MEM32(device+0x1A04);
        recomp_timeline_event("gpu_end_begin", "query", 0, 0);
        if (gpu_query_enabled()) {
            p_gpu_frame_end();
            recomp_timeline_event("gpu_poll_begin", "query", 0, 0);
            { unsigned long gf=0; double gms=0.0; if (p_gpu_frame_poll(&gf,&gms)) { static int gpu_log=-1; if(gpu_log<0) gpu_log=getenv("RECOMP_GPU_FRAME_LOG")!=NULL; g_hle_gpu_ms += gms; recomp_timeline_event("gpu_complete", "query", (double)gf, gms); if(gpu_log) fprintf(stderr,"[GPU-FRAME] frame=%lu ms=%.3f region=draws\n",gf,gms); } }
            recomp_timeline_event("gpu_poll_end", "query", 0, 0);
        }
        recomp_timeline_event("readback_begin", "swap", 0, 0);
        recomp_rcap_sync("readback");
        int readback=p_readback_swap(surface);
        recomp_timeline_event("readback_end", "swap", 0, 0);
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
                recomp_timeline_event("framebuffer_present_begin", "swap", 0, 0);
                xbox_FramebufferWindowPresent(data,pitch);
                recomp_timeline_event("framebuffer_present_end", "swap", 0, 0);
            }
        }
        {
            static unsigned measured_frames;
            double present_start=hle_time();
            unsigned i;
            recomp_timeline_event("present_begin", "swap", 0, 0);
            recomp_rcap_sync("present");
            p_present();
            recomp_timeline_event("present_end", "swap", 0, 0);
            if (gpu_query_enabled()) p_gpu_frame_begin();
            recomp_timeline_event("gpu_begin", "query", 0, 0);
            g_hle_cost[1]+=hle_time()-present_start;
            if(++measured_frames==120 && present_start) {
                unsigned long shader_compiles=0, shader_queries=0, shader_misses=0;
                unsigned long long up_uploads=0, up_bytes=0, up_calls=0, rb_frames=0;
                double up_ms=0.0, rb_map=0.0, rb_copy=0.0, rb_enq=0.0;
                p_shader_stats(&shader_compiles,&shader_queries,&shader_misses);
                p_upload_stats(&up_uploads,&up_bytes,&up_calls,&up_ms);
                p_readback_stats(&rb_frames,&rb_map,&rb_copy,&rb_enq);
                {
                    static unsigned long long prev[15];
                    static unsigned long prev_frame;
                    unsigned long long cur[15];
                    unsigned long frame = p_texture_frame();
                    unsigned j;
                    p_texture_stats(&cur[0],&cur[1],&cur[2],&cur[3],&cur[4],&cur[5],&cur[6],
                                    &cur[7],&cur[8],&cur[9],&cur[10],&cur[11],&cur[12],&cur[13],&cur[14]);
                    if (!prev_frame) prev_frame = frame;
                    if (frame - prev_frame >= 120) {
                        fprintf(stderr,"[TEX-PERF] frames=%lu-%lu lookups=%llu hits=%llu misses=%llu content_checks=%llu content_bytes=%llu changes=%llu uploads=%llu pal_lookups=%llu pal_select=%llu pal_select_changes=%llu pal_hashes=%llu pal_bytes=%llu pal_content_changes=%llu variant_mismatch=%llu invalidation=%llu per_frame_lookups=%.1f\n",
                            prev_frame, frame,
                            cur[0]-prev[0],cur[1]-prev[1],cur[2]-prev[2],cur[3]-prev[3],cur[4]-prev[4],cur[5]-prev[5],cur[6]-prev[6],
                            cur[7]-prev[7],cur[8]-prev[8],cur[9]-prev[9],cur[10]-prev[10],cur[11]-prev[11],cur[12]-prev[12],cur[13]-prev[13],cur[14]-prev[14],
                            (double)(cur[0]-prev[0]) / (double)(frame - prev_frame ? frame - prev_frame : 1));
                        for (j=0;j<15;j++) prev[j]=cur[j];
                        prev_frame=frame;
                    }
                }
                fprintf(stderr,"[HLE-PERF] readback=%.2f present=%.2f submit=%.2f prepare=%.2f copy=%.2f set=%.2f stream=%.2f texture=%.2f palette=%.2f end=%.2f begin=%.2f draw_up=%.2f idx_up=%.2f draw=%.2f idx=%.2f clear=%.2f vsh=%.2f psh=%.2f gpu=%.2f ms/frame draws=%u/%u/%u/%u/%u vsh=%u psh=%u shader_compiles=%lu psh_cache=%lu/%lu up_kib=%.2f up_calls=%.1f rb_map=%.3f rb_copy=%.3f rb_enq=%.3f\n",
                    g_hle_cost[0]/120,g_hle_cost[1]/120,g_hle_cost[2]/120,g_hle_cost[3]/120,g_hle_cost[4]/120,
                    g_hle_cost[5]/120,g_hle_cost[9]/120,g_hle_cost[7]/120,g_hle_cost[8]/120,
                    g_hle_cost[10]/120,g_hle_cost[11]/120,g_hle_cost[12]/120,g_hle_cost[13]/120,
                    g_hle_cost[14]/120,g_hle_cost[15]/120,g_hle_cost[16]/120,g_hle_cost[18]/120,g_hle_cost[19]/120,
                    g_hle_gpu_ms/120,
                    g_hle_draw_up_calls,g_hle_draw_indexed_up_calls,g_hle_draw_calls,g_hle_draw_indexed_calls,g_hle_clear_calls,
                    g_hle_create_shader_calls,g_hle_pixel_shader_calls,
                    shader_compiles,shader_queries,shader_misses,
                    (double)up_bytes/(1024.0*120.0),(double)up_calls/120.0,
                    rb_frames ? rb_map/rb_frames : 0.0,
                    rb_frames ? rb_copy/rb_frames : 0.0,
                    rb_frames ? rb_enq/rb_frames : 0.0);
                for(i=0;i<20;i++) g_hle_cost[i]=0;
                g_hle_draw_up_calls=g_hle_draw_indexed_up_calls=0;
                g_hle_draw_calls=g_hle_draw_indexed_calls=g_hle_clear_calls=0;
                g_hle_create_shader_calls=g_hle_pixel_shader_calls=0;
                g_hle_gpu_ms=0;
                measured_frames=0;
            }
        }
        recomp_rcap_state("swap", surface, 0, 0);
        recomp_timeline_swap_end();
        return 0; /* Guest buffer rotation, counter and pacing run once. */
    }
    case 0x003B6FB0u: p_render_target(arg(0),arg(1)); recomp_rcap_state("render_target", arg(0), arg(1), 0); return 0;
    case 0x003B7280u: {
        static unsigned copies;
        recomp_timeline_event("copy_begin", "copy_rects", 0, 0);
        recomp_rcap_state("copy", arg(0), arg(2), arg(3));
        int copied=p_copy_rects(arg(0),arg(1),arg(2),arg(3),arg(4));
        recomp_timeline_event("copy_end", "copy_rects", 0, 0);
        g_hle_cost[4]+=hle_time()-submit_start;
        if(copies++<3) fprintf(stderr,"[D3D8HLE] copy src=%08X dst=%08X count=%u gpu=%d\n",
            MEM32(arg(0)+4),MEM32(arg(3)+4),arg(2),copied);
        if(!copied) return 0;
        break;
    }
    case 0x003B75F0u: { double t=hle_time(); recomp_timeline_event("state_begin", "viewport", arg(0), 0); recomp_rcap_state("viewport", arg(0), 0, 0); p_viewport(arg(0)); recomp_timeline_event("state_end", "viewport", 0, 0); g_hle_cost[5]+=hle_time()-t; return 0; }
    case 0x003B6B80u: { double t=hle_time(); recomp_timeline_event("state_begin", "transform", arg(0), 0); p_transform(arg(0),arg(1)); recomp_timeline_event("state_end", "transform", 0, 0); g_hle_cost[6]+=hle_time()-t; return 0; }
    case 0x003B7820u: { double t=hle_time(); recomp_timeline_event("state_begin", "texture", arg(0), arg(1)); { uint32_t _manifest[19]; int _valid = 0; int _ok = 0; unsigned long long _pre[15] = {0}, _post[15] = {0}; unsigned long long _pu0 = 0, _pu1 = 0, _pu2 = 0; double _pd = 0.0; int _capture = recomp_rcap_active(); recomp_rcap_texture_bind(arg(0), arg(1)); if (_capture) { recomp_rcap_texture_frame(p_texture_frame()); p_texture_stats(&_pre[0],&_pre[1],&_pre[2],&_pre[3],&_pre[4],&_pre[5],&_pre[6],&_pre[7],&_pre[8],&_pre[9],&_pre[10],&_pre[11],&_pre[12],&_pre[13],&_pre[14]); p_upload_stats(&_pu0,&_pu1,&_pu2,&_pd); } if (p_texture_manifest && arg(1)) { memset(_manifest, 0, sizeof(_manifest)); _valid = p_texture_manifest((const uint32_t *)XBOX_PTR(arg(1)), _manifest); _ok = _manifest[16] != 0; } if (_ok) { recomp_rcap_manifest("texture", arg(1), _manifest[16], (uint32_t)_valid, _manifest, 19); if (_valid) recomp_rcap_snapshot("texture_header", arg(1), 64, arg(0), arg(1), 0); } else recomp_rcap_unsupported("texture_manifest"); p_texture(arg(0),arg(1)); if (_capture) { p_texture_stats(&_post[0],&_post[1],&_post[2],&_post[3],&_post[4],&_post[5],&_post[6],&_post[7],&_post[8],&_post[9],&_post[10],&_post[11],&_post[12],&_post[13],&_post[14]); { unsigned long long _pu3 = 0, _pu4 = 0, _pu5 = 0; double _pd2 = 0.0; p_upload_stats(&_pu3,&_pu4,&_pu5,&_pd2); recomp_rcap_renderer_bytes(_post[4] - _pre[4], _pu4 - _pu1); } } recomp_timeline_event("state_end", "texture", 0, 0); g_hle_cost[7]+=hle_time()-t; return 0; } }
    case 0x003B79D0u: { double t=hle_time(); recomp_timeline_event("state_begin", "palette", arg(0), arg(1)); if (arg(1)) { uint32_t _pdata = MEM32(arg(1) + 4); if (_pdata < 0x04000000u) _pdata |= 0x80000000u; recomp_rcap_snapshot("palette_header", arg(1), 64, arg(0), arg(1), 0); recomp_rcap_snapshot("palette_data", _pdata, 1024, arg(0), arg(1), 0); } p_palette(arg(0),arg(1)); recomp_timeline_event("state_end", "palette", 0, 0); g_hle_cost[8]+=hle_time()-t; return 0; }
    case 0x003B8A50u: { double t=hle_time(); recomp_timeline_event("state_begin", "stream", arg(0), arg(1)); recomp_rcap_state("stream", arg(0), arg(1), arg(2)); cap_stream_ptr = arg(1); cap_stream_stride = arg(2); p_stream(arg(0),arg(1),arg(2)); recomp_timeline_event("state_end", "stream", 0, 0); g_hle_cost[9]+=hle_time()-t; return 0; }
    case 0x003B7A60u: { double t=hle_time(); recomp_timeline_event("state_begin", "indices", arg(0), arg(1)); recomp_rcap_state("indices", arg(0), arg(1), 0); p_indices(arg(0),arg(1)); recomp_timeline_event("state_end", "indices", 0, 0); g_hle_cost[5]+=hle_time()-t; return 0; }
    case 0x003B8820u:
        recomp_rcap_snapshot("vconst", g_edx, 16, (uint32_t)g_ecx - 96u, 1, 0);
        p_vertex_constants((int)g_ecx-96,g_edx,1); return 0;
    case 0x003B8880u:
        recomp_rcap_snapshot("vconst", g_edx, 64, (uint32_t)g_ecx - 96u, 4, 0);
        p_vertex_constants((int)g_ecx-96,g_edx,4); return 0;
    case 0x003B8930u:
        if (!(arg(0)&3)) { recomp_rcap_snapshot("vconst", g_edx, (arg(0)/4)*16u, (uint32_t)g_ecx - 96u, arg(0)/4, 0); p_vertex_constants((int)g_ecx-96,g_edx,arg(0)/4); }
        return 0;
    case 0x003B86E0u: {
        uint32_t declaration=arg(0),function=arg(1),out=arg(2),usage=arg(3);
        double t=hle_time();
        uint32_t host=p_create_vertex_shader(declaration,function,usage);
        g_hle_cost[18]+=hle_time()-t; g_hle_create_shader_calls++;
        g_guest_only=1;
        sub_003B86E0();
        g_guest_only=0;
        if ((int32_t)g_eax>=0 && out && host && g_shader_count<SHADER_SLOTS) {
            g_shaders[g_shader_count].guest=MEM32(out);
            g_shaders[g_shader_count++].host=host;
        }
        return 1;
    }
    case 0x003B8E30u: p_vertex_shader(host_shader(arg(0))); recomp_rcap_state("vsh_select", arg(0), 0, 0); return 0;
    case 0x003B8AE0u: p_load_vertex_shader(host_shader(arg(0)),arg(1)); return 0;
    case 0x003B8B40u: p_select_vertex_shader(host_shader(arg(0)),arg(1)); return 0;
    case 0x003BAE20u: { /* Guest shader object owns the definition at +8. */
        double t=hle_time(); recomp_rcap_unsupported("pshader_content"); p_pixel_shader(arg(0)?MEM32(arg(0)+8):0); g_hle_cost[19]+=hle_time()-t; g_hle_pixel_shader_calls++; return 0; }
    case 0x003B66D0u: {
        uint32_t bits=arg(4);
        float z;
        memcpy(&z,&bits,sizeof z);
        { double t=hle_time(); recomp_timeline_event("draw_begin", "clear", arg(1), 0); recomp_rcap_state("clear", arg(0), arg(1), arg(5)); p_clear(arg(0),arg(1),arg(2),arg(3),z,arg(5)); recomp_timeline_event("draw_end", "clear", 0, 0); g_hle_cost[16]+=hle_time()-t; g_hle_clear_calls++; }
        break;
    }
    case 0x003BA740u:
        if (hle_trace_enabled())
            fprintf(stderr,"[D3D8HLE] DRAW_UP prim=%u count=%u data=%08X stride=%u\n",
                    arg(0),arg(1),arg(2),arg(3));
        if (hle_trace_enabled())
            fprintf(stderr,"[D3D8HLE] DRAW_UP host=%p before\n",(void*)p_draw_up);
        { double t=hle_time(); recomp_timeline_event("draw_begin", "draw_up", arg(0), arg(1)); recomp_rcap_snapshot("draw_up_vdata", arg(2), arg(1) * (arg(3) ? arg(3) : 12u), arg(0), arg(1), arg(3)); p_draw_up(arg(0),arg(1),arg(2),arg(3)); recomp_timeline_event("draw_end", "draw_up", 0, 0); g_hle_cost[12]+=hle_time()-t; g_hle_draw_up_calls++; }
        if (hle_trace_enabled())
            fprintf(stderr,"[D3D8HLE] DRAW_UP after\n");
        break;
    case 0x003BA8B0u:
        if (hle_trace_enabled())
            fprintf(stderr,"[D3D8HLE] DRAW_IDX_UP prim=%u count=%u idx=%08X data=%08X stride=%u\n",
                    arg(0),arg(1),arg(2),arg(3),arg(4));
        { double t=hle_time(); recomp_timeline_event("draw_begin", "draw_idx_up", arg(0), arg(1)); recomp_rcap_snapshot("draw_idx_up_idx", arg(2), arg(1) * 2u, arg(0), arg(1), 0); capture_indexed_vdata(arg(2), arg(1)); recomp_rcap_snapshot("draw_idx_up_vdata", arg(3), arg(1) * (arg(4) ? arg(4) : 12u), arg(0), arg(1), arg(4)); p_draw_indexed_up(arg(0),arg(1),arg(2),arg(3),arg(4)); recomp_timeline_event("draw_end", "draw_idx_up", 0, 0); g_hle_cost[13]+=hle_time()-t; g_hle_draw_indexed_up_calls++; }
        break;
    case 0x003BA9F0u:
        if (hle_trace_enabled())
            fprintf(stderr,"[D3D8HLE] DRAW_IDX prim=%u count=%u idx=%08X\n",
                    arg(0),arg(1),arg(2));
        { double t=hle_time(); recomp_timeline_event("draw_begin", "draw_idx", arg(0), arg(1)); recomp_rcap_snapshot("draw_idx_idx", arg(2), arg(1) * 2u, arg(0), arg(1), 0); p_draw_indexed(arg(0),arg(1),arg(2)); recomp_timeline_event("draw_end", "draw_idx", 0, 0); g_hle_cost[15]+=hle_time()-t; g_hle_draw_indexed_calls++; }
        break;
    case 0x003BAD40u: { double t=hle_time(); p_begin(arg(0)); g_hle_cost[11]+=hle_time()-t; return 0; } /* Guest Begin/End maintain device flags. */
    case 0x003BAD80u: { double t=hle_time(); p_end(); g_hle_cost[10]+=hle_time()-t; return 0; }
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
