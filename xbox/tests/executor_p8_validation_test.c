/* Real executor preparation: P8 writes at the same address and flip must be visible. */
#include "../../tools/xboxrecomp/src/kernel/nv2a_pb_exec.c"

static uint8_t s_mem[0x10000];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_image_lo, g_xbox_image_hi, g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 0; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static void queue_sample(void)
{
    Nv2aVshOutput v[3] = {0};
    unsigned i;
    v[1].pos[0] = 2; v[2].pos[1] = 2;
    for (i = 0; i < 3; i++) {
        v[i].pos[3] = 1;
        v[i].d0[0] = v[i].d0[1] = v[i].d0[2] = v[i].d0[3] = 1;
    }
    s_raster_active = 1;
    raster_xf_triangle(&v[0], &v[1], &v[2], 0);
    s_raster_active = 0;
}

int main(void)
{
    Texture *t = &s_gpu.texs[0];
    uint32_t *palette = (uint32_t *)(s_mem + 0x3000);
    uint32_t *pixel = (uint32_t *)(s_mem + 0x4000);
    const float *first;
    _putenv_s("RECOMP_RASTER_THREADS", "1");
    _putenv_s("RECOMP_NO_P8_CACHE", "");
    _putenv_s("RECOMP_P8_EXACT", "1");
    g_xbox_mem_offset = (ptrdiff_t)s_mem;
    s_gpu.flips = 16;
    s_gpu.color_offset = 0x4000; s_gpu.pitch = 8;
    s_gpu.clip_w = s_gpu.clip_h = 2; s_gpu.format = 8;
    s_gpu.aa_sx = s_gpu.aa_sy = 1;
    s_gpu.color_mask = 0x01010101;
    s_gpu.rc_seen = 1;
    s_gpu.rc.control = 1; s_gpu.rc.final0 = 0xC; s_gpu.rc.final1 = 0x1C80;
    s_gpu.rc.color_icw[0] = 0x04080000; s_gpu.rc.alpha_icw[0] = 0x14200000;
    s_gpu.rc.color_ocw[0] = s_gpu.rc.alpha_ocw[0] = 0xC00;
    s_gpu.rc.stage_program = 1;
    t->offset = 0x2000; t->palette = 0x3000; t->width = t->height = 2;
    t->color = 0x0B; t->valid = 1; t->addr_u = t->addr_v = 3;
    palette[0] = 0xFFFF0000; palette[1] = 0xFF00FF00;

    prepare_p8_draw_textures();
    first = t->p8_rgba;
    CHECK(first && first[0] == 1 && first[1] == 0 && first[2] == 0);
    queue_sample();
    CHECK(s_raster_count == 1 && *pixel == 0);
    prepare_p8_draw_textures();
    CHECK(t->p8_rgba == first && s_raster_count == 1); /* unchanged data reuses storage */

    palette[0] = 0xFF0000FF;
    prepare_p8_draw_textures(); /* same flip and addresses, palette-only mutation */
    CHECK(t->p8_rgba[0] == 0 && t->p8_rgba[1] == 0 && t->p8_rgba[2] == 1);
    CHECK(s_raster_count == 0 && *pixel == 0xFFFF0000); /* old sample finished before overwrite */

    queue_sample();
    CHECK(s_raster_count == 1);
    s_mem[0x2000] = 1;
    prepare_p8_draw_textures(); /* same flip and addresses, index-only mutation */
    CHECK(t->p8_rgba[0] == 0 && t->p8_rgba[1] == 1 && t->p8_rgba[2] == 0);
    CHECK(s_raster_count == 0 && *pixel == 0xFF0000FF);

    s_gpu.flips = 17; palette[1] = 0xFFFFFFFF;
    prepare_p8_draw_textures();
    CHECK(t->p8_rgba[0] == 1 && t->p8_rgba[1] == 1 && t->p8_rgba[2] == 1);
    free(s_raster_jobs);
    puts("PASS: executor validates P8 palette/indices every draw and preserves queued readers");
    return 0;
}
