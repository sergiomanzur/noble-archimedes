/* Real fragment rasterization with independent color/depth/stencil buffers. */
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

typedef struct {
    XfTri tri;
    Nv2aVshOutput vertices[3];
    float depth;
    uint8_t stencil;
    uint32_t color;
    XfCount count;
} Fragment;

static void init_fragment(Fragment *f)
{
    unsigned i;
    memset(f, 0, sizeof *f);
    f->vertices[1].pos[0] = 2;
    f->vertices[2].pos[1] = 2;
    for (i = 0; i < 3; i++) {
        f->vertices[i].pos[2] = 0.25f;
        f->vertices[i].pos[3] = 1;
        f->vertices[i].d0[0] = f->vertices[i].d0[3] = 1;
        f->tri.iw[i] = 1;
    }
    f->tri.va = &f->vertices[0]; f->tri.a = f->vertices[0].pos;
    f->tri.vb = &f->vertices[1]; f->tri.b = f->vertices[1].pos;
    f->tri.vc = &f->vertices[2]; f->tri.c = f->vertices[2].pos;
    f->tri.inv_area = 0.25f;
    f->tri.rc_plan.rc = &f->tri.rc;
    f->tri.maxx = f->tri.maxy = 1;
    f->tri.bpp = 4;
    f->tri.zb = &f->depth;
    f->tri.sb = &f->stencil;
    f->tri.g.surface = (uint8_t *)&f->color;
    f->tri.g.pitch = 4;
    f->tri.g.clip_w = f->tri.g.clip_h = 1;
    f->tri.g.color_mask = 0x01010101;
    f->tri.g.depth_test = f->tri.g.depth_mask = 1;
    f->tri.g.depth_func = 0x201; /* incoming .25 < stored .5 */
    f->tri.g.stencil_enable = 1;
    f->tri.g.stencil_func = 0x207;
    f->tri.g.stencil_ref = 0xB3;
    f->tri.g.stencil_func_mask = 0x0F;
    f->tri.g.stencil_write_mask = 0x0F;
    f->tri.g.stencil_fail = f->tri.g.stencil_zfail = f->tri.g.stencil_zpass = 0x1E01;
    f->tri.g.zpass_enable = 1;
    f->depth = 0.5f;
    f->stencil = 0xA4;
    f->color = 0xFF123456;
}

static int failures;
static void check(Fragment *f, const char *name, uint8_t expected_stencil,
                  int writes_depth, int writes_color, int depth_failure)
{
    uint32_t color = writes_color ? 0xFFFF0000 : 0xFF123456;
    float depth = writes_depth ? 0.25f : 0.5f;
    xf_rows(&f->tri, 0, 1, &f->count);
    if (f->stencil != expected_stencil || f->depth != depth || f->color != color ||
        f->count.depth_fail != (uint64_t)depth_failure ||
        f->count.zpass != (uint32_t)writes_depth ||
        f->count.pixels != (uint64_t)writes_color) {
        fprintf(stderr, "FAIL %s: stencil %02X expected %02X; depth %.2f expected %.2f; "
                "color %08X expected %08X; depth_fail/zpass/pixels %llu/%u/%llu\n",
                name, f->stencil, expected_stencil, f->depth, depth,
                f->color, color, (unsigned long long)f->count.depth_fail,
                f->count.zpass, (unsigned long long)f->count.pixels);
        failures++;
    }
}

static void check_shadow_quad(const char *name, int subset, int alpha_reject,
                              int depth_reject)
{
    Nv2aVshOutput v[4] = {0};
    Nv2aRcPlan plan;
    uint32_t *pixels = (uint32_t *)(s_mem + 0x4000);
    unsigned i;
    _putenv_s("RECOMP_RASTER_THREADS", "1");
    nv2a_pb_exec_method(0, NV097_SET_SURFACE_FORMAT, 0x28);
    nv2a_pb_exec_method(0, NV097_SET_SURFACE_CLIP_HORIZONTAL, 32u << 16);
    nv2a_pb_exec_method(0, NV097_SET_SURFACE_CLIP_VERTICAL, 32u << 16);
    nv2a_pb_exec_method(0, NV097_SET_SURFACE_PITCH, 128);
    nv2a_pb_exec_method(0, NV097_SET_SURFACE_COLOR_OFFSET, 0x4000);
    s_gpu.prim = NV_PRIM_QUADS;
    s_gpu.rc_seen = 1;
    memset(&s_gpu.rc, 0, sizeof s_gpu.rc);
    s_gpu.rc.control = 1;
    s_gpu.rc.final0 = 0xC; s_gpu.rc.final1 = 0x1C80;
    s_gpu.rc.color_icw[0] = 0x01200000;
    s_gpu.rc.alpha_icw[0] = 0x11200000;
    s_gpu.rc.color_ocw[0] = s_gpu.rc.alpha_ocw[0] = 0xC00;
    s_gpu.rc.factor0[0] = 0xFF000000; /* exact plan 9: opaque black */
    nv2a_rc_prepare(&s_gpu.rc, &plan);
    if (plan.kind != 9) { fprintf(stderr, "FAIL: quad fixture must use plan 9\n"); failures++; return; }
    s_cpu_stencil = 1; /* explicitly opt this fixture into the CPU stencil buffer */
    s_reg[0x032C / 4] = 1;
    s_reg[0x0364 / 4] = 0x201; /* LESS: ref 0 < positive stencil */
    s_reg[0x0368 / 4] = 0;
    s_reg[0x036C / 4] = 0xFF;
    s_reg[0x0360 / 4] = 0;
    s_gpu.depth_test = depth_reject;
    s_gpu.depth_func = 0x200; /* NEVER */
    s_gpu.depth_mask = 0;
    s_gpu.alpha_test = alpha_reject;
    s_gpu.alpha_func = 0x200; /* NEVER */
    zbuf_current(1);
    memset(s_zbuf_s, 0, (size_t)NV_ZBUF_W * NV_ZBUF_H);
    if (subset) s_zbuf_s[8 * NV_ZBUF_W + 8] = 1;
    for (i = 0; i < 32 * 32; i++) pixels[i] = 0xFFA0B0C0;
    v[1].pos[0] = v[2].pos[0] = 32;
    v[2].pos[1] = v[3].pos[1] = 32;
    for (i = 0; i < 4; i++) v[i].pos[3] = 1;
    raster_xf_triangle(&v[0], &v[1], &v[2], 0);
    raster_xf_triangle(&v[0], &v[2], &v[3], 0);
    for (i = 0; i < 32 * 32; i++) {
        uint32_t expected = subset && !alpha_reject && !depth_reject && i == 8 * 32 + 8
                          ? 0xFF000000 : 0xFFA0B0C0;
        if (pixels[i] != expected) {
            fprintf(stderr, "FAIL %s: pixel %u color %08X expected %08X\n",
                    name, i, pixels[i], expected);
            failures++;
            break;
        }
    }
}

int main(void)
{
    static const struct { uint32_t func; int pass; const char *name; } compares[] = {
        {0x200, 0, "NEVER"}, {0x201, 1, "masked ref LESS stored"},
        {0x202, 0, "EQUAL unequal"}, {0x203, 1, "masked ref LEQUAL stored"},
        {0x204, 0, "masked ref GREATER stored"}, {0x205, 1, "NOTEQUAL"},
        {0x206, 0, "masked ref GEQUAL stored"}, {0x207, 1, "ALWAYS"}
    };
    static const struct { uint32_t op; uint8_t expected; const char *name; } ops[] = {
        {0x1E00, 0xA4, "KEEP"}, {0, 0xA0, "ZERO"}, {0x1E01, 0xA3, "REPLACE"},
        {0x1E02, 0xA5, "INCR"}, {0x1E03, 0xA3, "DECR"}, {0x150A, 0xAB, "INVERT"},
        {0x8507, 0xA5, "INCR_WRAP"}, {0x8508, 0xA3, "DECR_WRAP"}
    };
    Fragment f;
    unsigned i, path, plan;
    for (i = 0; i < sizeof compares / sizeof compares[0]; i++) {
        init_fragment(&f);
        f.tri.g.stencil_func = compares[i].func;
        f.tri.g.stencil_fail = 0x1E00; /* a failed compare keeps the byte */
        check(&f, compares[i].name, compares[i].pass ? 0xA3 : 0xA4,
              compares[i].pass, compares[i].pass, 0);
    }
    init_fragment(&f);
    f.tri.g.stencil_func = 0x202;
    f.tri.g.stencil_ref = 0xB4; /* high bits differ, masked values equal */
    check(&f, "masked EQUAL", 0xA4, 1, 1, 0);

    /* Every operation and write mask applies identically on each outcome. */
    for (path = 0; path < 3; path++)
    for (i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        init_fragment(&f);
        if (path == 0) f.tri.g.stencil_func = 0x200;
        if (path == 1) f.tri.g.depth_func = 0x200;
        f.tri.g.stencil_fail = f.tri.g.stencil_zfail = f.tri.g.stencil_zpass = ops[i].op;
        check(&f, ops[i].name, ops[i].expected, path == 2, path == 2, path == 1);
    }
    for (path = 0; path < 3; path++) {
        init_fragment(&f);
        if (path == 0) f.tri.g.stencil_func = 0x200;
        if (path == 1) f.tri.g.depth_func = 0x200;
        f.tri.g.alpha_test = 1;
        f.tri.g.alpha_func = 0x204; /* alpha 0 must exceed 128 */
        f.tri.g.alpha_ref = 128;
        for (i = 0; i < 3; i++) f.vertices[i].d0[3] = 0;
        check(&f, "alpha rejection leaves FAIL/ZFAIL/ZPASS untouched", 0xA4, 0, 0, 0);
    }
    init_fragment(&f);
    f.tri.g.alpha_test = 1;
    f.tri.g.alpha_func = 0x204;
    f.tri.g.alpha_ref = 128;
    check(&f, "accepted alpha permits stencil and depth writes", 0xA3, 1, 1, 0);
    /* CLIPPLANE kills must run even on stencil/depth-only passes. */
    for (path = 0; path < 3; path++)
    for (plan = 0; plan < 2; plan++)
    for (i = 0; i < 2; i++) {
        unsigned vertex;
        init_fragment(&f);
        if (path == 0) f.tri.g.stencil_func = 0x200;
        if (path == 1) f.tri.g.depth_func = 0x200;
        if (!i) f.tri.g.color_mask = 0;
        f.tri.use_rc = 1;
        f.tri.rc_plan.kind = plan ? 3 : 0; /* solid plan doesn't sample textures */
        f.tri.stage_program = 5; /* stage 0 CLIPPLANE rejects negative x */
        for (vertex = 0; vertex < 3; vertex++) f.tri.stc[vertex][0][0] = -1;
        check(&f, "CLIP rejection leaves FAIL/ZFAIL/ZPASS untouched", 0xA4, 0, 0, 0);
    }
    init_fragment(&f);
    f.tri.use_rc = 1;
    f.tri.rc_plan.kind = 3;
    f.tri.g.color_mask = 0;
    f.tri.stage_program = 5;
    check(&f, "accepted CLIP permits stencil and depth writes", 0xA3, 1, 0, 0);
    check_shadow_quad("zero stencil rejects full-screen plan 9 shadow quad", 0, 0, 0);
    check_shadow_quad("shadow quad darkens only positive stencil subset", 1, 0, 0);
    check_shadow_quad("stencil-constrained quad preserves alpha rejection", 1, 1, 0);
    check_shadow_quad("stencil-constrained quad preserves depth rejection", 1, 0, 1);
    if (!failures) puts("PASS: masked stencil comparisons, operations, alpha and CLIP fragment rejection");
    return failures ? 1 : 0;
}
