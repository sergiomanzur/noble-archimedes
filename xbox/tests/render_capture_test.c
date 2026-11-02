#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
ptrdiff_t g_xbox_mem_offset;
#include "../src/render_capture.c"

static uint8_t backing[1 << 20];
static struct rcap test_ctx;

int main(void)
{
    uint8_t src[64];
    unsigned i;
    for (i = 0; i < sizeof(src); i++)
        src[i] = (uint8_t)(i * 7 + 1);
    memset(&test_ctx, 0, sizeof(test_ctx));
    memset(backing, 0, sizeof(backing));
    g_xbox_mem_offset = (ptrdiff_t)((uintptr_t)backing - 0x80000000u);
    memcpy(rcap_ptr(0x80000100u), src, sizeof(src));

    assert(rcap_snapshot_core(&test_ctx, "vdata", rcap_ptr(0x80000100u),
                              32, 1, 2, 3) == 0);
    assert(test_ctx.used == 32 && test_ctx.peak == 32);
    assert(test_ctx.records == 1);
    assert(test_ctx.overflow == 0);
    assert(memcmp(test_ctx.arena, src, 32) == 0);
    /* same-address mutation after capture must not change the snapshot */
    {
        uint8_t *live = (uint8_t *)rcap_ptr(0x80000100u);
        memset(live, 0xAA, 64);
        assert(memcmp(test_ctx.arena, src, 32) == 0);
        assert(memcmp(live + 32, src + 32, 32) == 0); /* only 32 were captured */
    }

    /* explicit overflow: cap and arena limits are reported, not silent */
    assert(rcap_snapshot_core(&test_ctx, "big", rcap_ptr(0x80000100u),
                              RCP_SNAPSHOT_CAP + 1, 0, 0, 0) == 1);
    assert(test_ctx.overflow == 1);
    assert(test_ctx.used == 32);

    for (i = 0; i < RCP_MAX_RECORDS; i++)
        rcap_snapshot_core(&test_ctx, "vdata", rcap_ptr(0x80000100u),
                           4, 0, 0, 0);
    assert(test_ctx.overflow > 1); /* record cap overflow is explicit */

    /* Manifest accounting is by normalized contents, not by bind count. */
    memset(&test_ctx, 0, sizeof(test_ctx));
    rcap_on = 1;
    rcap_active = 1;
    {
        uint32_t fields[19] = {0};
        fields[1] = 0x11110000u;
        fields[3] = 0x081u;
        fields[4] = 0;
        fields[17] = 0;
        recomp_rcap_manifest("texture", 0x80020000u, 4096, 1, fields, 19);
        recomp_rcap_manifest("texture", 0x80020000u, 4096, 1, fields, 19);
        assert(test_ctx.manifest_count == 1);
        assert(test_ctx.manifest[0].binds == 2);
        assert(test_ctx.manifest_unique_bytes == 4096);
        assert(test_ctx.manifest_bind_bytes == 8192);
        /* Same address/format but a different header generation is a distinct
         * resource; same-address mutation is not merged silently. */
        fields[4] = 64;
        fields[17] = 1;
        recomp_rcap_manifest("texture", 0x80020000u, 8192, 1, fields, 19);
        assert(test_ctx.manifest_count == 2);
        assert(test_ctx.manifest[1].fields[17] == 1);
        recomp_rcap_texture_bind(0, 0);
        recomp_rcap_texture_bind(0, 0x80020000u);
        assert(test_ctx.texture_binds == 2);
        assert(test_ctx.texture_null_binds == 1);
        recomp_rcap_texture_frame(100);
        recomp_rcap_texture_frame(102);
        assert(test_ctx.texture_frame_first == 100);
        assert(test_ctx.texture_frame_last == 102);
    }
    memset(&test_ctx, 0, sizeof(test_ctx));
    rcap_on = 0;
    rcap_active = 0;

    printf("render-capture core checks passed\n");
    return 0;
}
