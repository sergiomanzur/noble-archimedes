/* Bounded texture-header audit: real P8 and linear headers plus invalid range. */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef int (*manifest_fn)(const uint32_t *resource, uint32_t *out);
typedef void (*offset_fn)(ptrdiff_t offset);

#define TEST_BACKING (8u << 20)
static unsigned char *backing;

int main(void)
{
    HMODULE module = LoadLibraryA("d3d8gl.dll");
    manifest_fn manifest;
    offset_fn set_offset;
    uint32_t *resource;
    uint32_t out[19];
    int valid;

    if (!module) {
        fprintf(stderr, "FAIL: d3d8gl.dll not loadable\n");
        return 1;
    }
    manifest = (manifest_fn)(void *)GetProcAddress(module, "noble_d3d8_texture_manifest");
    set_offset = (offset_fn)(void *)GetProcAddress(module, "noble_d3d8_set_guest_offset");
    if (!manifest || !set_offset) {
        fprintf(stderr, "FAIL: manifest/set-offset export missing\n");
        return 1;
    }
    /* The production validator recognizes the guest arena, so map the real
     * guest base rather than a malloc address. */
    backing = (unsigned char *)VirtualAlloc((void *)0x80000000u, TEST_BACKING,
                                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!backing) {
        fprintf(stderr, "FAIL: guest test arena allocation\n");
        return 1;
    }
    set_offset(0);
    resource = (uint32_t *)(backing + 0x1000);

    /* Real P8 header: 256x256, embedded dimensions, 8bpp, no mips. */
    memset(resource, 0, 24);
    resource[0] = 0x00040001u;
    resource[1] = 0x00002000u;
    resource[3] = 0x08810B29u;
    resource[4] = 0;
    valid = manifest(resource, out);
    if (!valid || out[6] != 11 || out[7] != 256 || out[8] != 256 ||
        out[10] != 1 || out[11] != 256 || out[15] != 65536 ||
        out[16] != 65536 || out[17] != 0 || out[18] != 0) {
        fprintf(stderr, "FAIL: P8 manifest mismatch valid=%d fmt=%u %ux%u levels=%u pitch=%u face=%u total=%u gpu=%u ov=%u\n", valid, out[6], out[7], out[8], out[10], out[11], out[15], out[16], out[17], out[18]);
        return 1;
    }

    /* Real linear header: 640x480 A8R8G8B8, pitch 2560. */
    resource[3] = 0x00011229u;
    resource[4] = 0x271DF27Fu;
    valid = manifest(resource, out);
    if (!valid || out[6] != 0x12 || out[7] != 640 || out[8] != 480 ||
        out[11] != 2560 || out[14] != 1 || out[16] != 1228800 ||
        out[17] != 0 || out[18] != 0) {
        fprintf(stderr, "FAIL: linear manifest mismatch\n");
        return 1;
    }

    /* A 4096 cube with 15 levels claims ~0.8 GiB and crosses the guest
     * arena end; it must be rejected rather than clamped. */
    resource[3] = (0x06u << 8) | (1u << 16) | (12u << 20) | (12u << 24) |
                  (15u << 16) | 0x04u;
    resource[4] = 0;
    valid = manifest(resource, out);
    if (valid || out[18] != 0) {
        fprintf(stderr, "FAIL: invalid range accepted\n");
        return 1;
    }

    /* Volume header whose 64-bit total exceeds the 32-bit accounting unit. */
    resource[3] = (0x06u << 8) | (12u << 20) | (12u << 24) |
                  (9u << 28) | (0x3u << 4) | (1u << 16);
    resource[4] = 0;
    valid = manifest(resource, out);
    if (valid || out[18] != 1) {
        fprintf(stderr, "FAIL: overflow not reported\n");
        return 1;
    }

    puts("texture manifest: P8, linear pitch, levels and invalid range pass");
    return 0;
}
