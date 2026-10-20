/* Real executor + software pixels: an inline HUD must follow queued FFP draws. */
#include "../../tools/xboxrecomp/src/kernel/nv2a_pb_exec.c"

static uint8_t s_mem[0x10000];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
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

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static void mf(uint32_t method, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    m(method, bits);
}
static uint32_t pixel(void)
{
    uint32_t value;
    memcpy(&value, s_mem + 0x4000 + 8 * 128 + 8 * 4, sizeof value);
    return value;
}
static void scene(void)
{
    m(NV097_SET_BEGIN_END, NV_PRIM_TRIANGLES);
    m(NV097_DRAW_ARRAYS, 2u << 24);  /* three array vertices */
    m(NV097_SET_BEGIN_END, 0);
}
static void hud(void)
{
    static const float positions[3][3] = {{4,4,0}, {20,4,0}, {4,20,0}};
    unsigned i, j;
    m(NV097_SET_BEGIN_END, NV_PRIM_TRIANGLES);
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) mf(NV097_INLINE_ARRAY, positions[i][j]);
        m(NV097_INLINE_ARRAY, 0xFF00FF00); /* opaque green HUD */
    }
    m(NV097_SET_BEGIN_END, 0);
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s (pixel 0x%08X)\n", \
                            __LINE__, #c, pixel()); return 1; } } while (0)

int main(void)
{
    static const float positions[3][3] = {{2,2,0}, {30,2,0}, {2,30,0}};
    static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    unsigned i;
    _putenv_s("RECOMP_RASTER_THREADS", "1");
    s_drawn_dumps = FB_DUMP_AFTER_DRAW; /* this test inspects guest pixels directly */
    memcpy(s_mem + 0x2000, positions, sizeof positions);
    for (i = 0; i < 3; i++) {
        uint32_t red = 0xFFFF0000;
        memcpy(s_mem + 0x3000 + i * 4, &red, 4);
    }
    m(NV097_SET_SURFACE_FORMAT, 8); /* A8R8G8B8 */
    m(NV097_SET_SURFACE_CLIP_HORIZONTAL, 32u << 16);
    m(NV097_SET_SURFACE_CLIP_VERTICAL, 32u << 16);
    m(NV097_SET_SURFACE_PITCH, 128);
    m(NV097_SET_SURFACE_COLOR_OFFSET, 0x4000);
    m(0x1720, 0x2000); m(0x1760, 2 | (3 << 4) | (12 << 8));
    m(0x172C, 0x3000); m(0x176C, (4 << 4) | (4 << 8));
    for (i = 0; i < 16; i++) mf(0x0680 + i * 4, identity[i]);

    /* Establish that the queued scene really covers the probe pixel. */
    scene();
    CHECK(s_raster_count > 0);
    CHECK(pixel() == 0);
    m(NV097_FLIP_STALL, 0);
    CHECK(pixel() == 0xFFFF0000);
    memset(s_mem + 0x4000, 0, 32 * 128);

    scene();
    CHECK(s_raster_count > 0);
    CHECK(pixel() == 0);
    hud(); /* inline data bypasses the FFP transform and rasterizes immediately */
    CHECK(pixel() == 0xFF00FF00);
    m(NV097_FLIP_STALL, 0);
    CHECK(pixel() == 0xFF00FF00);
    puts("PASS: queued scene completes before the immediate screen-space HUD");
    free(s_xf);
    free(s_raster_jobs);
    return 0;
}
