/* Real GL integration: synthetic resources, no game or benchmark process. */
#include "platform.h"
#include <assert.h>
int noble_d3d8_init(void);
void noble_d3d8_set_guest_offset(ptrdiff_t);
void noble_d3d8_set_state_arrays(const DWORD *, const DWORD (*)[D3DTSS_MAX]);
void noble_d3d8_state(unsigned, unsigned);
void noble_d3d8_render_target(unsigned, unsigned);
void noble_d3d8_vertex_shader(unsigned);
void noble_d3d8_texture(unsigned, unsigned);
HRESULT WINAPI D3DDevice_CreateTexture(UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, D3DTexture **);
void noble_d3d8_draw_up(unsigned, unsigned, unsigned, unsigned);
void noble_d3d8_draw_indexed_up(unsigned, unsigned, unsigned, unsigned, unsigned);
void noble_d3d8_clear(unsigned, unsigned, unsigned, unsigned, float, unsigned);
int noble_d3d8_readback(unsigned);
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
    assert((pixel & 0x00FFFFFFu) == rgb);
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
    noble_d3d8_draw_indexed_up(D3DPT_QUADLIST,4,guest(indices),guest(screen),sizeof(*screen));
    assert(noble_d3d8_readback(guest(surface)));
    check_pixel(surface,0xFF00FF);
    puts("D3D8GL real GPU FVF triangles, indexed quads and mutable texture readback passed");
    return 0;
}
