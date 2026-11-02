/* Real GL integration: synthetic resources, no game or benchmark process. */
#include "platform.h"
#include "xgpu.h"
#include <assert.h>
int noble_d3d8_init(void);
void noble_d3d8_set_guest_offset(ptrdiff_t);
void noble_d3d8_set_state_arrays(const DWORD *, const DWORD (*)[D3DTSS_MAX]);
void noble_d3d8_state(unsigned, unsigned);
void noble_d3d8_render_target(unsigned, unsigned);
void noble_d3d8_vertex_shader(unsigned);
unsigned noble_d3d8_create_vertex_shader(unsigned, unsigned, unsigned);
void noble_d3d8_vertex_constants(int, unsigned, unsigned);
int noble_d3d8_fixed_function(unsigned, unsigned, unsigned);
int noble_d3d8_copy_rects(unsigned,unsigned,unsigned,unsigned,unsigned);
void noble_d3d8_texture(unsigned, unsigned);
void noble_d3d8_pixel_shader(unsigned);
HRESULT WINAPI D3DDevice_CreateTexture(UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, D3DTexture **);
void noble_d3d8_draw_up(unsigned, unsigned, unsigned, unsigned);
void noble_d3d8_draw_indexed_up(unsigned, unsigned, unsigned, unsigned, unsigned);
void noble_d3d8_clear(unsigned, unsigned, unsigned, unsigned, float, unsigned);
int noble_d3d8_readback(unsigned);
int noble_d3d8_readback_swap(unsigned);
void d3d8_surface_initialize(D3DSurface *, D3DFORMAT, unsigned long, unsigned long);
typedef struct { float x,y,z,rhw; DWORD color; float u,v; } ScreenVertex;
typedef struct { float x,y,z; DWORD color; float u,v; } WorldVertex;
static unsigned guest(const void *p) { return (unsigned)((uintptr_t)p - noble_guest_memory_offset); }
static void check_pixel(D3DSurface *s, DWORD rgb)
{
    unsigned pitch = (((s->Size >> D3DSIZE_PITCH_SHIFT) & 0xFFu) + 1) * 64;
    const unsigned char *data = PLATFORM_PHYSICAL_TO_VIRTUAL(s->Data);
    DWORD pixel;
    memcpy(&pixel, data + 32 * pitch + 32 * 4, 4);
    printf("center ARGB=%08lx expected RGB=%06lx\n", pixel, rgb);
    if ((pixel & 0x00FFFFFFu) != rgb) exit(1);
}
int main(void)
{
    D3DSurface *surface;
    ScreenVertex *screen;
    WorldVertex *world;
    WORD *indices;
    unsigned i;
    D3DTexture *texture;
    DWORD *texels;
    noble_d3d8_set_guest_offset(0x10000);
    assert(noble_d3d8_init());
    noble_d3d8_set_state_arrays(NULL, NULL);
    surface = calloc(1, sizeof(*surface));
    screen = calloc(4, sizeof(*screen));
    world = calloc(3, sizeof(*world));
    indices = calloc(4, sizeof(*indices));
    assert(surface && screen && world && indices);
    d3d8_surface_initialize(surface, D3DFMT_LIN_A8R8G8B8, 64, 64);
    noble_d3d8_render_target(guest(surface), 0);
    noble_d3d8_state(D3DRS_CULLMODE, D3DCULL_NONE);
    noble_d3d8_state(D3DRS_ZENABLE, 0);
    noble_d3d8_state(D3DRS_ZWRITEENABLE, 0);
    noble_d3d8_state(D3DRS_ALPHABLENDENABLE, 0);
    noble_d3d8_state(D3DRS_PSCOMBINERCOUNT, 0);
    noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSABCD, 4);
    noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSEFG, 0x1400);
    noble_d3d8_state(D3DRS_PSTEXTUREMODES, 0);
    screen[0].x=8; screen[0].y=8;
    screen[1].x=56; screen[1].y=8;
    screen[2].x=32; screen[2].y=56;
    for(i=0;i<4;i++) { screen[i].z=.5f; screen[i].rhw=1; screen[i].color=0xFF00FF00; }
    noble_d3d8_vertex_shader(0x144);
    noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF000000,1,0);
    noble_d3d8_draw_up(D3DPT_TRIANGLELIST,3,guest(screen),sizeof(*screen));
    assert(noble_d3d8_readback(guest(surface)));
    check_pixel(surface,0x00FF00);
    world[0].x=-.75f; world[0].y=.75f;
    world[1].x=.75f; world[1].y=.75f;
    world[2].x=0; world[2].y=-.75f;
    for(i=0;i<3;i++) { world[i].z=.5f; world[i].color=0xFF0000FF; }
    noble_d3d8_vertex_shader(0x142);
    noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF000000,1,0);
    noble_d3d8_draw_up(D3DPT_TRIANGLELIST,3,guest(world),sizeof(*world));
    assert(noble_d3d8_readback(guest(surface)));
    check_pixel(surface,0x0000FF);
    screen[2].x=56; screen[2].y=56; screen[3].x=8; screen[3].y=56;
    for(i=0;i<4;i++) { screen[i].color=0xFFFF0000; indices[i]=(WORD)i; }
    noble_d3d8_vertex_shader(0x144);
    noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF000000,1,0);
    noble_d3d8_draw_indexed_up(D3DPT_QUADLIST,4,guest(indices),guest(screen),sizeof(*screen));
    assert(noble_d3d8_readback(guest(surface)));
    check_pixel(surface,0xFF0000);
    assert(D3DDevice_CreateTexture(2,2,1,0,D3DFMT_LIN_A8R8G8B8,0,&texture) >= 0);
    texels = PLATFORM_PHYSICAL_TO_VIRTUAL(texture->Data);
    texels[0]=texels[1]=texels[16]=texels[17]=0xFF00FFFF;
    noble_d3d8_texture(0,guest(texture));
    noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSABCD,8);
    noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSEFG,0x1800);
    noble_d3d8_state(D3DRS_PSTEXTUREMODES,1);
    noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF000000,1,0);
    noble_d3d8_draw_indexed_up(D3DPT_QUADLIST,4,guest(indices),guest(screen),sizeof(*screen));
    assert(noble_d3d8_readback(guest(surface)));
    check_pixel(surface,0x00FFFF);
    /* Same address, same frame: exact dirty tracking must upload new bytes. */
    texels[0]=texels[1]=texels[16]=texels[17]=0xFFFF00FF;
    noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF000000,1,0);
    {
        long full_before = noble_d3d8_gl_shadow_probe(_noble_gl_shadow_full_invalidations, 0, 0);
        long bindings_before = noble_d3d8_gl_shadow_probe(_noble_gl_shadow_binding_invalidations, 0, 0);

        noble_d3d8_draw_indexed_up(D3DPT_QUADLIST,4,guest(indices),guest(screen),sizeof(*screen));
        /* The upload took the narrow path: it invalidated the 2D bindings, not
         * the whole GL state shadow (a full reset here would re-send all of it
         * at the next draw). */
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_binding_invalidations, 0, 0) > bindings_before);
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_full_invalidations, 0, 0) == full_before);
        /* The draw that uploaded re-established every other shadow itself, so
         * the frame after an upload does not pay for them again. */
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_program, 0, 0));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_framebuffer, 0, 0));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_raster, 0, 0));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_vertex_array, 0, 0));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_array_buffer, 0, 0));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_attribute_value, 1, 0));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_texture_binding, 0, 0));
        /* Direct contract: a texture-target invalidation leaves program,
         * framebuffer, raster, vertex array, buffer and attribute shadows
         * known, and marks every unit's binding of that target stale. */
        xgpu_gl_texture_binding_invalidated(GL_TEXTURE_2D);
        assert(!noble_d3d8_gl_shadow_probe(_noble_gl_shadow_texture_binding, 0, 0));
        assert(!noble_d3d8_gl_shadow_probe(_noble_gl_shadow_texture_binding, 1, 0));
        assert(!noble_d3d8_gl_shadow_probe(_noble_gl_shadow_texture_binding, 2, 0));
        assert(!noble_d3d8_gl_shadow_probe(_noble_gl_shadow_texture_binding, 3, 0));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_program, 0, 0));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_framebuffer, 0, 0));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_raster, 0, 0));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_vertex_array, 0, 0));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_array_buffer, 0, 0));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_attribute_value, 1, 0));
        /* The next draw rebinds the stale stage and samples the new texels. */
        noble_d3d8_draw_indexed_up(D3DPT_QUADLIST,4,guest(indices),guest(screen),sizeof(*screen));
        assert(noble_d3d8_gl_shadow_probe(_noble_gl_shadow_texture_binding, 0, 0));
    }
    assert(noble_d3d8_readback(guest(surface)));
    check_pixel(surface,0xFF00FF);
    /* A programmable shader may use its own screen-space conversion rather
     * than the reserved c[-38]/c[-37] viewport pair. Synthetic NV2A program:
     * mov oPos,v0; mov oD0,v1; mul oPos.xyz,r12,c10 + rcc r1.x,r12.w;
     * mad oPos.xyz,r12,r1.x,c11. Keep pixel coordinates with scale=1,offset=0. */
    {
        DWORD *decl = calloc(14, sizeof(DWORD));
        DWORD *code = calloc(21, sizeof(DWORD));
        float *constants = calloc(8, sizeof(float));
        unsigned shader;
        assert(decl && code && constants);
        decl[0] = D3DVSD_TOKEN_STREAM << D3DVSD_TOKENTYPESHIFT;
        decl[1] = (D3DVSD_TOKEN_STREAMDATA << D3DVSD_TOKENTYPESHIFT) | (D3DVSDT_FLOAT4 << D3DVSD_DATATYPESHIFT);
        decl[2] = (D3DVSD_TOKEN_STREAMDATA << D3DVSD_TOKENTYPESHIFT) | (D3DVSDT_D3DCOLOR << D3DVSD_DATATYPESHIFT) | 1;
        decl[3] = (D3DVSD_TOKEN_STREAMDATA << D3DVSD_TOKENTYPESHIFT) | (D3DVSDT_FLOAT2 << D3DVSD_DATATYPESHIFT) | 2;
        decl[4] = (D3DVSD_TOKEN_CONSTMEM<<D3DVSD_TOKENTYPESHIFT) | (2u<<D3DVSD_CONSTCOUNTSHIFT) | 106;
        for(i=0;i<4;i++) decl[5+i]=0x3F800000;
        decl[13] = D3DVSD_END();
        code[0] = (5u << 16) | 0x2078u;
        code[2] = (1u<<21) | 0x1Bu;
        code[3] = 2u<<26;
        code[4] = (15u<<12) | (1u<<11);
        code[6] = (1u<<21) | (1u<<9) | 0x1Bu;
        code[7] = 2u<<26;
        code[8] = (15u<<12) | (1u<<11) | (3u<<3);
        code[10] = (2u<<21) | (3u<<25) | (106u<<13) | 0x1Bu;
        code[11] = (12u<<28) | (1u<<26) | (0x1Bu<<17) | (3u<<11) | (0xFFu<<2) | 3u;
        code[12] = (1u<<28) | (8u<<16) | (14u<<12) | (1u<<11);
        code[14] = (4u<<21) | (107u<<13) | 0x1Bu;
        code[15] = (12u<<28) | (1u<<26) | (1u<<13) | (1u<<11) | (0x1Bu<<2);
        code[16] = (3u<<28) | (14u<<12) | (1u<<11);
        code[18] = (1u<<21) | (2u<<9) | 0x1Bu;
        code[19] = 2u<<26;
        code[20] = (15u<<12) | (1u<<11) | (9u<<3) | 1u;
        constants[0]=constants[1]=constants[2]=constants[3]=1.0f;
        shader=noble_d3d8_create_vertex_shader(guest(decl),guest(code),0);
        assert(shader);
        noble_d3d8_vertex_shader(shader);
        noble_d3d8_texture(0,0);
        noble_d3d8_state(D3DRS_PSTEXTUREMODES,0);
        noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSABCD,4);
        noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSEFG,0x1400);
        for(i=0;i<4;i++) screen[i].color=0xFF00FF00;
        noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF000000,1,0);
        noble_d3d8_draw_up(D3DPT_QUADLIST,4,guest(screen),sizeof(*screen));
        assert(noble_d3d8_readback(guest(surface)));
        check_pixel(surface,0x00FF00);
        noble_d3d8_texture(0,guest(texture));
        noble_d3d8_state(D3DRS_PSTEXTUREMODES,1);
        noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSABCD,8);
        noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSEFG,0x1800);
        noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF000000,1,0);
        noble_d3d8_draw_up(D3DPT_QUADLIST,4,guest(screen),sizeof(*screen));
        assert(noble_d3d8_readback(guest(surface)));
        check_pixel(surface,0xFF00FF);
        {
            DWORD *packet=calloc(10,sizeof(DWORD));
            assert(packet);
            packet[0]=0x41E60; packet[1]=1;
            packet[2]=0x40AC0; packet[3]=0x04200000;
            packet[4]=0x41E40; packet[5]=0xC00;
            packet[6]=0x40260; packet[7]=0x14200000;
            packet[8]=0x40AA0; packet[9]=0xC00;
            /* Stale programmable state must not replace compiled FF state. */
            noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSABCD,0);
            noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSEFG,0);
            assert(noble_d3d8_fixed_function(guest(packet),10,0));
            noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF000000,1,0);
            noble_d3d8_draw_up(D3DPT_QUADLIST,4,guest(screen),sizeof(*screen));
            assert(noble_d3d8_readback(guest(surface)));
            check_pixel(surface,0x00FF00);
            {
                D3DSurface *next=calloc(1,sizeof(*next));
                assert(next);
                d3d8_surface_initialize(next,D3DFMT_LIN_A8R8G8B8,64,64);
                /* A framebuffer copy must preserve GPU-only pixels, without
                 * consulting the stale guest-memory shadow. */
                assert(noble_d3d8_copy_rects(guest(surface),0,0,guest(next),0));
                noble_d3d8_render_target(guest(next),0);
                assert(noble_d3d8_readback(guest(next)));
                check_pixel(next,0x00FF00);
                /* Sampling the copied texture must use its resident GPU
                 * image even while its CPU shadow is stale. */
                memset(PLATFORM_PHYSICAL_TO_VIRTUAL(next->Data),0,64*64*4);
                noble_d3d8_render_target(guest(surface),0);
                noble_d3d8_pixel_shader(0);
                noble_d3d8_texture(0,guest(next));
                noble_d3d8_state(D3DRS_PSTEXTUREMODES,1);
                noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSABCD,8);
                noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSEFG,0x1800);
                for(i=0;i<4;i++) { screen[i].u=32;screen[i].v=32; }
                noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF000000,1,0);
                noble_d3d8_draw_up(D3DPT_QUADLIST,4,guest(screen),sizeof(*screen));
                assert(noble_d3d8_readback(guest(surface)));
                check_pixel(surface,0x00FF00);
            }
            packet[0]=0x201E60; /* Truncated payload must be rejected. */
            assert(!noble_d3d8_fixed_function(guest(packet),2,0));
        }
    }
    /* Swap readback may lag by two calls once primed, but a successful cold
     * read must contain real pixels, and a size transition must never map an
     * old allocation using the new surface's dimensions or row pitch. */
    noble_d3d8_render_target(guest(surface),0);
    noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFFFF0000,1,0);
    assert(noble_d3d8_readback_swap(guest(surface)));
    check_pixel(surface,0xFF0000);
    noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF00FF00,1,0);
    assert(noble_d3d8_readback_swap(guest(surface)));
    check_pixel(surface,0xFF0000);
    noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF0000FF,1,0);
    assert(noble_d3d8_readback_swap(guest(surface)));
    check_pixel(surface,0x00FF00);
    noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFFFFFFFF,1,0);
    assert(noble_d3d8_readback_swap(guest(surface)));
    check_pixel(surface,0x0000FF);
    {
        D3DSurface *larger=calloc(1,sizeof(*larger));
        assert(larger);
        d3d8_surface_initialize(larger,D3DFMT_LIN_A8R8G8B8,80,80);
        noble_d3d8_render_target(guest(larger),0);
        noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF00FFFF,1,0);
        assert(noble_d3d8_readback_swap(guest(larger)));
        check_pixel(larger,0x00FFFF);
        noble_d3d8_render_target(guest(surface),0);
        noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFFFF00FF,1,0);
        assert(noble_d3d8_readback_swap(guest(surface)));
        check_pixel(surface,0xFF00FF);
        /* Explicit readback always means the latest GPU pixels, even while
         * the swap ring contains earlier frames. */
        noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF0000FF,1,0);
        assert(noble_d3d8_readback(guest(surface)));
        check_pixel(surface,0x0000FF);
    }
    if (getenv("RECOMP_WIDE_EXPERIMENT")) {
        D3DSurface *wide = calloc(1, sizeof(*wide));
        const unsigned char *data;
        const unsigned xs[] = {10, 320, 630};
        assert(wide);
        d3d8_surface_initialize(wide, D3DFMT_LIN_A8R8G8B8, 640, 480);
        noble_d3d8_render_target(guest(wide), 0);
        noble_d3d8_state(D3DRS_ZENABLE, 0);
        noble_d3d8_state(D3DRS_ALPHABLENDENABLE, 0);
        noble_d3d8_state(D3DRS_PSTEXTUREMODES, 0);
        noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSABCD, 4);
        noble_d3d8_state(D3DRS_PSFINALCOMBINERINPUTSEFG, 0x1400);
        screen[0].x=0; screen[0].y=0; screen[1].x=640; screen[1].y=0;
        screen[2].x=640; screen[2].y=480; screen[3].x=0; screen[3].y=480;
        for(i=0;i<4;i++) { screen[i].z=.5f; screen[i].rhw=1; screen[i].color=0xFFFF0000; }
        noble_d3d8_vertex_shader(0x144);
        noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF000000,1,0);
        noble_d3d8_draw_up(D3DPT_QUADLIST,4,guest(screen),sizeof(*screen));
        assert(noble_d3d8_readback(guest(wide)));
        data = PLATFORM_PHYSICAL_TO_VIRTUAL(wide->Data);
        for(i=0;i<3;i++) {
            DWORD pixel;
            memcpy(&pixel, data + 240 * 640 * 4 + xs[i] * 4, 4);
            if ((pixel & 0x00FFFFFFu) != 0xFF0000u) {
                fprintf(stderr,"wide central readback x=%u: %08lx\n",xs[i],pixel);
                return 1;
            }
        }
        {
            D3DSurface *copy = calloc(1, sizeof(*copy));
            assert(copy);
            d3d8_surface_initialize(copy, D3DFMT_LIN_A8R8G8B8, 640, 480);
            assert(noble_d3d8_copy_rects(guest(wide), 0, 0, guest(copy), 0));
            noble_d3d8_render_target(guest(copy), 0);
            assert(noble_d3d8_readback(guest(copy)));
            data = PLATFORM_PHYSICAL_TO_VIRTUAL(copy->Data);
            for(i=0;i<3;i++) {
                DWORD pixel;
                memcpy(&pixel, data + 240 * 640 * 4 + xs[i] * 4, 4);
                if ((pixel & 0x00FFFFFFu) != 0xFF0000u) {
                    fprintf(stderr,"wide copied readback x=%u: %08lx\n",xs[i],pixel);
                    return 1;
                }
            }
            {
                RECT *rect = calloc(1, sizeof(*rect));
                NobleD3DPoint *point = calloc(1, sizeof(*point));
                DWORD left, inside, right;
                assert(rect && point);
                rect->left=100; rect->top=0; rect->right=200; rect->bottom=480;
                point->x=100; point->y=0;
                noble_d3d8_clear(0,0,D3DCLEAR_TARGET,0xFF000000,1,0);
                if (!noble_d3d8_copy_rects(guest(wide),guest(rect),1,guest(copy),guest(point)) ||
                    !noble_d3d8_readback(guest(copy))) { fprintf(stderr,"wide partial copy failed\n"); return 1; }
                memcpy(&left, data + 240 * 640 * 4 + 50 * 4, 4);
                memcpy(&inside, data + 240 * 640 * 4 + 150 * 4, 4);
                memcpy(&right, data + 240 * 640 * 4 + 250 * 4, 4);
                if ((left & 0xFFFFFF) != 0 || (inside & 0xFFFFFF) != 0xFF0000 || (right & 0xFFFFFF) != 0) {
                    fprintf(stderr,"wide partial copy: %08lx %08lx %08lx\n",left,inside,right); return 1;
                }
                {
                    D3DSurface *fresh = calloc(1, sizeof(*fresh));
                    DWORD *pixels;
                    unsigned j;
                    assert(fresh);
                    d3d8_surface_initialize(fresh, D3DFMT_LIN_A8R8G8B8, 640, 480);
                    pixels = PLATFORM_PHYSICAL_TO_VIRTUAL(fresh->Data);
                    for(j=0;j<640u*480u;j++) pixels[j]=0xFF00FF00;
                    if (!noble_d3d8_copy_rects(guest(wide),guest(rect),1,guest(fresh),guest(point))) {
                        fprintf(stderr,"fresh wide partial copy failed\n"); return 1;
                    }
                    noble_d3d8_render_target(guest(fresh), 0);
                    assert(noble_d3d8_readback(guest(fresh)));
                    memcpy(&left, pixels + 240*640 + 50, 4);
                    memcpy(&inside, pixels + 240*640 + 150, 4);
                    memcpy(&right, pixels + 240*640 + 250, 4);
                    if ((left & 0xFFFFFF) != 0x00FF00 || (inside & 0xFFFFFF) != 0xFF0000 || (right & 0xFFFFFF) != 0x00FF00) {
                        fprintf(stderr,"fresh wide partial copy: %08lx %08lx %08lx\n",left,inside,right); return 1;
                    }
                }
            }
        }
    }
    puts("D3D8GL GPU draw, synchronous readback and asynchronous swap readback passed");
    return 0;
}
