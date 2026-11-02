/*
 * Regression for the fb_present frame-source snapshot and extent:
 *
 * - an unsupported pitch/bpp touches no source pointer and shows black;
 * - the checked extent is the last-row active bytes (padding is not read);
 * - bpp 2/4/8 and AA 1/2 use exactly the snapshot's pitch and factor;
 * - the conversion is called with the same immutable (pitch, aa) that the
 *   range check used (this test passes explicit values; fb_present's window
 *   thread takes one snapshot and passes those locals through).
 *
 * The source buffer is placed so its checked extent ends exactly at a
 * PAGE_NOACCESS guard page: if the conversion reads a padding byte or uses a
 * different pitch/stride, the test faults rather than silently passing.
 */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* fb_present.c's test seams and the two symbols it normally gets from the
 * kernel layout. This test does not start a window. */
uint64_t xbox_FramebufferSourceBytesTest(unsigned width, unsigned height,
                                         uint32_t pitch, uint32_t aa_sy,
                                         unsigned bpp);
int xbox_FramebufferFrameSourceTest(uint32_t va, uint32_t pitch, uint32_t aa_sy,
                                    unsigned width, unsigned height,
                                    uint32_t *bpp_out, uint64_t *bytes_out);
int xbox_FramebufferConvertTest(const uint8_t *src, unsigned width,
                                unsigned height, uint32_t pitch, uint32_t aa_sy,
                                unsigned bpp, uint32_t *dst);
int xbox_FramebufferCopyFrameTest(uint32_t va, uint32_t pitch, uint32_t aa_sy,
                                  uint32_t *dst, unsigned width, unsigned height,
                                  uint64_t *bytes_out);

static ptrdiff_t stub_offset;
ptrdiff_t xbox_GetMemoryOffset(void) { return stub_offset; }

static uint32_t stub_lo, stub_hi;
static unsigned stub_calls;
int xbox_GuestMemoryContains(uint32_t va, uint64_t bytes)
{
    stub_calls++;
    return bytes && va >= stub_lo && (uint64_t)va + bytes <= stub_hi;
}

/* source[0 .. bytes) ends exactly at the protected page */
static uint8_t *guarded_source(uint64_t bytes, uint8_t **base_out, size_t *page_out)
{
    SYSTEM_INFO info;
    uint8_t *base;
    DWORD old;

    GetSystemInfo(&info);
    *page_out = info.dwPageSize;
    assert(bytes <= *page_out);
    base = (uint8_t *)VirtualAlloc(NULL, *page_out * 2,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(base);
    assert(VirtualProtect(base + *page_out, *page_out, PAGE_NOACCESS, &old));
    *base_out = base;
    return base + *page_out - bytes;
}

static void test_extent_and_conversion(void)
{
    uint32_t dst[16];
    uint32_t i, x;

    /* bpp 4 with three bytes of row padding: exact extent = 19 + 16 = 35. */
    {
        uint8_t *base, *src;
        size_t page;
        const uint64_t bytes = 35;
        uint32_t row0[4], row1[4];

        assert(xbox_FramebufferSourceBytesTest(4, 2, 19, 1, 4) == bytes);
        src = guarded_source(bytes, &base, &page);
        for (i = 0; i < bytes; i++)
            src[i] = (uint8_t)(0x40 + i);
        assert(xbox_FramebufferConvertTest(src, 4, 2, 19, 1, 4, dst));
        memcpy(row0, src, sizeof(row0));
        memcpy(row1, src + 19, sizeof(row1));
        assert(!memcmp(dst, row0, sizeof(row0)));
        assert(!memcmp(dst + 4, row1, sizeof(row1)));
        VirtualFree(base, 0, MEM_RELEASE);
    }

    /* bpp 2, pitch 9: exact extent = 9 + 8 = 17. */
    {
        uint8_t *base, *src;
        size_t page;
        const uint64_t bytes = 17;

        assert(xbox_FramebufferSourceBytesTest(4, 2, 9, 1, 2) == bytes);
        src = guarded_source(bytes, &base, &page);
        for (i = 0; i < bytes; i++)
            src[i] = (uint8_t)(i * 3);
        assert(xbox_FramebufferConvertTest(src, 4, 2, 9, 1, 2, dst));
        {
            uint16_t v;
            memcpy(&v, src, sizeof(v));
            assert(dst[0] == (((uint32_t)((v >> 11) & 0x1F) * 255u / 31u) << 16 |
                              ((uint32_t)((v >> 5) & 0x3F) * 255u / 63u) << 8 |
                              ((uint32_t)(v & 0x1F) * 255u / 31u)));
            memcpy(&v, src + 9, sizeof(v));
            assert(dst[4] == (((uint32_t)((v >> 11) & 0x1F) * 255u / 31u) << 16 |
                              ((uint32_t)((v >> 5) & 0x3F) * 255u / 63u) << 8 |
                              ((uint32_t)(v & 0x1F) * 255u / 31u)));
        }
        VirtualFree(base, 0, MEM_RELEASE);
    }

    /* bpp 8, pitch 32, AA 1: exact extent = 32 + 32 = 64; every other pixel. */
    {
        uint8_t *base, *src;
        size_t page;
        const uint64_t bytes = 64;

        assert(xbox_FramebufferSourceBytesTest(4, 2, 32, 1, 8) == bytes);
        src = guarded_source(bytes, &base, &page);
        for (i = 0; i < bytes; i++)
            src[i] = (uint8_t)(0x80 + i);
        assert(xbox_FramebufferConvertTest(src, 4, 2, 32, 1, 8, dst));
        for (x = 0; x < 4; x++)
        {
            uint32_t v;
            memcpy(&v, src + x * 8, sizeof(v));
            assert(dst[x] == v);
            memcpy(&v, src + 32 + x * 8, sizeof(v));
            assert(dst[4 + x] == v);
        }
        VirtualFree(base, 0, MEM_RELEASE);
    }

    /* bpp 8, AA 2: the second row is at 2 * pitch; extent = 66 + 32 = 98. */
    {
        uint8_t *base, *src;
        size_t page;
        const uint64_t bytes = 98;

        assert(xbox_FramebufferSourceBytesTest(4, 2, 33, 2, 8) == bytes);
        src = guarded_source(bytes, &base, &page);
        for (i = 0; i < bytes; i++)
            src[i] = (uint8_t)(0x20 + i);
        assert(xbox_FramebufferConvertTest(src, 4, 2, 33, 2, 8, dst));
        {
            uint32_t v;
            memcpy(&v, src + 0, sizeof(v));
            assert(dst[0] == v);
            memcpy(&v, src + 66, sizeof(v));
            assert(dst[4] == v);
        }
        VirtualFree(base, 0, MEM_RELEASE);
    }

    /* Unsupported bpp is black and never touches a source pointer, even one
     * that would fault if read. */
    memset(dst, 0xAA, sizeof(dst));
    assert(!xbox_FramebufferConvertTest((const uint8_t *)(uintptr_t)0x10,
                                        4, 2, 15, 1, 3, dst));
    for (x = 0; x < 8; x++)
        assert(dst[x] == 0);

    /* The extent arithmetic is 64-bit: a huge pitch must not wrap. */
    assert(xbox_FramebufferSourceBytesTest(640, 480, 0xFFFFFFFFu, 1, 4) ==
           (uint64_t)479 * 0xFFFFFFFFu + 2560u);
}

static void test_frame_source(void)
{
    uint32_t bpp;
    uint64_t bytes;

    /* width 4 / pitch 19 => bpp 4; exact bytes 35. */
    stub_calls = 0;
    stub_lo = 0x100000;
    stub_hi = 0x100000 + 35;
    assert(xbox_FramebufferFrameSourceTest(0x100000, 19, 1, 4, 2, &bpp, &bytes) == 1);
    assert(bpp == 4 && bytes == 35 && stub_calls == 1);
    /* one byte past the window is refused, not clamped */
    assert(xbox_FramebufferFrameSourceTest(0x100001, 19, 1, 4, 2, &bpp, &bytes) == -1);
    /* padding after the last active row does not count: pitch * height = 38,
     * but the window holds the 35 bytes actually read */
    stub_lo = 0x200000;
    stub_hi = 0x200000 + 35;
    assert(xbox_FramebufferFrameSourceTest(0x200000, 19, 1, 4, 2, &bpp, &bytes) == 1);
    assert(bytes == 35);
    /* AA 2 bpp 8: exact 98, one byte short refused */
    stub_lo = 0x300000;
    stub_hi = 0x300000 + 98;
    assert(xbox_FramebufferFrameSourceTest(0x300000, 33, 2, 4, 2, &bpp, &bytes) == 1);
    assert(bpp == 8 && bytes == 98);
    stub_hi = 0x300000 + 97;
    assert(xbox_FramebufferFrameSourceTest(0x300000, 33, 2, 4, 2, &bpp, &bytes) == -1);

    /* invalid bpp: black, no lookup, no pointer formed */
    stub_calls = 0;
    stub_lo = 0;
    stub_hi = 0xFFFFFFFFu;
    assert(xbox_FramebufferFrameSourceTest(0xFF1103E0u, 15, 1, 4, 2, &bpp, &bytes) == 0);
    assert(bpp == 3 && bytes == 0 && stub_calls == 0);
    /* huge pitch derives an invalid bpp before any extent math */
    assert(xbox_FramebufferFrameSourceTest(0x100000, 0xFFFFFFFFu, 1, 640, 480,
                                           &bpp, &bytes) == 0);
    assert(bytes == 0 && stub_calls == 0);
}

static void test_copy_snapshot(void)
{
    uint8_t *base, *src;
    size_t page;
    uint32_t dst[16];
    uint64_t bytes;
    uint32_t i;
    const uint32_t va = 0x100000;
    const uint32_t pitch = 19;

    src = guarded_source(35, &base, &page);
    for (i = 0; i < 35; i++)
        src[i] = (uint8_t)(0x60 + i);
    stub_lo = va;
    stub_hi = va + 35;
    stub_offset = (ptrdiff_t)((uintptr_t)src - (uintptr_t)va);
    stub_calls = 0;
    assert(xbox_FramebufferCopyFrameTest(va, pitch, 1, dst, 4, 2, &bytes) == 1);
    assert(bytes == 35 && stub_calls == 1);
    assert(!memcmp(dst, src, 16));
    assert(!memcmp(dst + 4, src + 19, 16));

    /* The same snapshot is checked and read: one byte less in the window
     * refuses before touching dst. */
    memset(dst, 0xAA, sizeof(dst));
    stub_hi = va + 34;
    assert(xbox_FramebufferCopyFrameTest(va, pitch, 1, dst, 4, 2, &bytes) == -1);
    for (i = 0; i < 16; i++)
        assert(dst[i] == 0xAAAAAAAAu);

    /* Unsupported bpp: black, no lookup and no source access. */
    stub_calls = 0;
    stub_hi = 0xFFFFFFFFu;
    stub_offset = 0;
    assert(xbox_FramebufferCopyFrameTest(va, 15, 1, dst, 4, 2, &bytes) == 0);
    assert(bytes == 0 && stub_calls == 0);
    for (i = 0; i < 8; i++)
        assert(dst[i] == 0);

    VirtualFree(base, 0, MEM_RELEASE);
}

int main(void)
{
    test_extent_and_conversion();
    test_frame_source();
    test_copy_snapshot();
    puts("framebuffer source snapshot/extent checks passed");
    return 0;
}
