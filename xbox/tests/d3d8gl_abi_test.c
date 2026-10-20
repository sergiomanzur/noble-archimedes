/* Headless ABI regression: no device/window initialization and no game run. */
#include "platform.h"
#include <assert.h>
void noble_d3d8_set_guest_offset(ptrdiff_t offset);
void noble_d3d8_set_state_arrays(const DWORD *, const DWORD (*)[D3DTSS_MAX]);
void noble_d3d8_sync_state(void);
void WINAPI D3DDevice_SetVertexShader(DWORD handle);
void WINAPI D3DDevice_SetViewport(const D3DVIEWPORT8 *viewport);
int main(void)
{
    DWORD render[166], texture[4][D3DTSS_MAX];
    unsigned char *watched;
    unsigned long generation;
    unsigned stage, state;
    noble_d3d8_set_guest_offset(0x10000);
    assert(noble_guest_pointer(0) == NULL);
    assert((uintptr_t)noble_guest_pointer(0x003C6D00) == 0x003D6D00);
    assert((uintptr_t)PLATFORM_PHYSICAL_TO_VIRTUAL(0x1234) == 0x80011234);
    assert((uintptr_t)PLATFORM_PHYSICAL_TO_VIRTUAL(0x81230000) == 0x81240000);
    assert((uintptr_t)PLATFORM_PHYSICAL_TO_VIRTUAL(0x90001234) == 0x90001234);
    assert(PLATFORM_VIRTUAL_TO_PHYSICAL((void *)(uintptr_t)0x80011234) == 0x1234);
    assert(PLATFORM_VIRTUAL_TO_PHYSICAL((void *)(uintptr_t)0x90001234) == 0x90001234);
    for (state = 0; state < 166; state++) render[state] = 0x1000 + state;
    for (stage = 0; stage < 4; stage++)
        for (state = 0; state < D3DTSS_MAX; state++) texture[stage][state] = stage * 100 + state;
    noble_d3d8_set_state_arrays(render, texture);
    noble_d3d8_sync_state();
    assert(D3D__RenderState[D3DRS_SRCBLEND] == render[62]);
    assert(D3D__RenderState[D3DRS_PSTEXTUREMODES] == render[136]);
    assert(D3D__RenderState[D3DRS_ZENABLE] == render[143]);
    assert(D3D__RenderState[D3DRS_CULLMODE] == render[147]);
    assert(D3D__RenderState[D3DRS_MULTISAMPLETYPE] == render[154]);
    assert(D3D__RenderState[D3DRS_SHADOWFUNC] == render[156]);
    for (stage = 0; stage < 4; stage++) {
        assert(D3D__TextureState[stage][D3DTSS_ADDRESSU] == stage * 100);
        assert(D3D__TextureState[stage][D3DTSS_MAGFILTER] == stage * 100 + 3);
        assert(D3D__TextureState[stage][D3DTSS_ALPHAKILL] == stage * 100 + 11);
        assert(D3D__TextureState[stage][D3DTSS_COLOROP] == stage * 100 + 12);
        assert(D3D__TextureState[stage][D3DTSS_BUMPENVMAT00] == stage * 100 + 22);
        assert(D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] == stage * 100 + 28);
    }
    /* Small FVF values must never be treated as host object pointers. */
    D3DDevice_SetVertexShader(0x142);
    D3DDevice_SetVertexShader(0x144);
    D3DDevice_SetVertexShader(0);
    D3DDevice_SetViewport(NULL);
    watched = noble_low_malloc(32);
    assert(watched);
    memset(watched, 0x55, 32);
    generation = memory_watch_generation((unsigned long)(uintptr_t)watched, 32);
    assert(generation == memory_watch_generation((unsigned long)(uintptr_t)watched, 32));
    watched[17] ^= 1;
    assert(memory_watch_generation((unsigned long)(uintptr_t)watched, 32) > generation);
    {
        void *first = noble_low_malloc(4096);
        void *second = noble_low_malloc(4096);
        void *third;
        assert(first && second);
        noble_low_free(first);
        noble_low_free(second);
        third = noble_low_malloc(8192);
        assert(third == first); /* adjacent free blocks coalesce */
        noble_low_free(third);
        for (state = 0; state < 2000; state++) {
            void *temporary = noble_low_malloc(1024 * 1024);
            assert(temporary);
            noble_low_free(temporary); /* >512 MiB cumulative reuse */
        }
    }
    puts("D3D8GL offset, state layout, FVF, mutable upload and allocator checks passed");
    return 0;
}
