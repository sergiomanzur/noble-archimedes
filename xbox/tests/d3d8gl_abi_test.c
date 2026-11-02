/* Headless ABI regression: no device/window initialization and no game run. */
#include "platform.h"
#include <assert.h>

/* The Windows prefix maps the Xbox VirtualProtect name. These checks use the
 * host guard pages, so call the host import directly. */
#undef VirtualProtect
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, size_t size,
                                                  unsigned long type,
                                                  unsigned long protect);
__declspec(dllimport) int __stdcall VirtualFree(void *address, size_t size,
                                                unsigned long type);
__declspec(dllimport) int __stdcall VirtualProtect(void *address, size_t size,
                                                   unsigned long protect,
                                                   unsigned long *old_protect);
#ifndef MEM_RESERVE
#define MEM_RESERVE 0x2000UL
#endif
#ifndef MEM_COMMIT
#define MEM_COMMIT 0x1000UL
#endif
#ifndef PAGE_READWRITE
#define PAGE_READWRITE 0x04UL
#endif
#ifndef PAGE_NOACCESS
#define PAGE_NOACCESS 0x01UL
#endif

void noble_d3d8_set_guest_offset(ptrdiff_t offset);
void noble_d3d8_set_state_arrays(const DWORD *, const DWORD (*)[D3DTSS_MAX]);
void noble_d3d8_sync_state(void);
int noble_d3d8_decode_p8_selftest(void);
int noble_d3d8_content_equal_test(const void *, const void *, size_t, int);
void WINAPI D3DDevice_SetVertexShader(DWORD handle);
void WINAPI D3DDevice_SetViewport(const D3DVIEWPORT8 *viewport);

#define CONTENT_TEST_PAGE 4096u

static void content_fill(unsigned char *bytes, size_t size, unsigned seed)
{
    size_t i;
    for (i = 0; i < size; i++)
        bytes[i] = (unsigned char)(seed + i * 37u + (i >> 8));
}

static void content_check_pair(const unsigned char *a, const unsigned char *b,
                               size_t size, int expected)
{
    assert(noble_d3d8_content_equal_test(a, b, size, 0) == expected);
    assert(noble_d3d8_content_equal_test(a, b, size, 1) == expected);
}

static unsigned char *content_guarded_buffer(size_t size, void **base_out)
{
    unsigned char *base = (unsigned char *)VirtualAlloc(
        NULL, CONTENT_TEST_PAGE * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    unsigned long old_protect = 0;
    assert(base);
    assert(size <= CONTENT_TEST_PAGE);
    assert(VirtualProtect(base + CONTENT_TEST_PAGE, CONTENT_TEST_PAGE,
                          PAGE_NOACCESS, &old_protect));
    *base_out = base;
    return base + CONTENT_TEST_PAGE - size;
}

static void content_test_guard_pages(void)
{
    static const size_t sizes[] = {
        0, 1, 2, 15, 16, 17, 31, 32, 33, 63, 64, 65,
        127, 128, 129, 255, 256, 257, 4095, 4096
    };
    size_t index;

    assert(noble_d3d8_content_equal_test(NULL, NULL, 0, 0) == 1);
    for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
        size_t size = sizes[index];
        void *a_base, *b_base;
        unsigned char *a = content_guarded_buffer(size, &a_base);
        unsigned char *b = content_guarded_buffer(size, &b_base);
        size_t diffs[3];
        size_t diff_count = size ? 3 : 0;
        size_t diff_index;

        if (size)
            content_fill(a, size, (unsigned)size);
        if (size)
            memcpy(b, a, size);
        if (size == 0) {
            /* Both pointers are at the no-access page: zero must not read. */
            assert(noble_d3d8_content_equal_test(a, b, 0, 0) == 1);
        } else {
            content_check_pair(a, b, size, 1);
            diffs[0] = 0;
            diffs[1] = size / 2;
            diffs[2] = size - 1;
            for (diff_index = 0; diff_index < diff_count; diff_index++) {
                size_t position = diffs[diff_index];
                b[position] ^= 0x80;
                content_check_pair(a, b, size, 0);
                b[position] ^= 0x80;
            }
        }
        VirtualFree(a_base, 0, 0x8000UL);
        VirtualFree(b_base, 0, 0x8000UL);
    }
}

static void content_test_small_exhaustive(void)
{
    enum { MAX_SMALL = 4097 };
    static const size_t sizes[] = {1023, 1024, 1025, 4095, 4096, 4097};
    static const size_t offsets[] = {0, 1, 15, 31};
    unsigned char *a = (unsigned char *)malloc(MAX_SMALL + 32);
    unsigned char *b = (unsigned char *)malloc(MAX_SMALL + 32);
    size_t size, offset_index, position;
    unsigned char saved;

    assert(a && b);
    for (size = 0; size <= 512; size++) {
        for (offset_index = 0; offset_index < 4; offset_index++) {
            size_t offset = offsets[offset_index];
            content_fill(a + offset, size, (unsigned)(size + offset));
            memcpy(b + offset, a + offset, size);
            content_check_pair(a + offset, b + offset, size, 1);
            for (position = 0; position < size; position++) {
                saved = b[offset + position];
                b[offset + position] ^= 0x80;
                content_check_pair(a + offset, b + offset, size, 0);
                b[offset + position] = saved;
            }
        }
    }
    /* Every byte at selected larger boundaries, with representative
     * alignments rather than the full offset matrix. */
    for (size = 0; size < sizeof(sizes) / sizeof(sizes[0]); size++) {
        for (offset_index = 0; offset_index < 2; offset_index++) {
            size_t offset = offsets[offset_index * 3]; /* 0, 31 */
            content_fill(a + offset, sizes[size], (unsigned)(sizes[size] + offset));
            memcpy(b + offset, a + offset, sizes[size]);
            content_check_pair(a + offset, b + offset, sizes[size], 1);
            for (position = 0; position < sizes[size]; position++) {
                saved = b[offset + position];
                b[offset + position] ^= 0x80;
                content_check_pair(a + offset, b + offset, sizes[size], 0);
                b[offset + position] = saved;
            }
        }
    }
    free(a);
    free(b);
}

static void content_test_alignment_matrix(void)
{
    static const size_t sizes[] = {
        0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129,
        255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097
    };
    unsigned char *a = (unsigned char *)malloc(4097 + 32);
    unsigned char *b = (unsigned char *)malloc(4097 + 32);
    size_t size_index, a_offset, b_offset;

    assert(a && b);
    for (size_index = 0; size_index < sizeof(sizes) / sizeof(sizes[0]);
         size_index++) {
        size_t size = sizes[size_index];
        for (a_offset = 0; a_offset < 32; a_offset++) {
            for (b_offset = 0; b_offset < 32; b_offset++) {
                content_fill(a + a_offset, size, (unsigned)(size + a_offset));
                memcpy(b + b_offset, a + a_offset, size);
                content_check_pair(a + a_offset, b + b_offset, size, 1);
                if (size) {
                    size_t positions[3];
                    size_t position_index;
                    positions[0] = 0;
                    positions[1] = size / 2;
                    positions[2] = size - 1;
                    for (position_index = 0; position_index < 3; position_index++) {
                        size_t position = positions[position_index];
                        b[b_offset + position] ^= 0x80;
                        content_check_pair(a + a_offset, b + b_offset, size, 0);
                        b[b_offset + position] ^= 0x80;
                    }
                }
            }
        }
    }
    free(a);
    free(b);
}

static void content_test_large_boundaries(void)
{
    static const size_t sizes[] = {
        65535, 65536, 65537, 131071, 131072, 131073
    };
    static const size_t offsets[] = {0, 1, 15, 16, 31};
    unsigned char *a = (unsigned char *)malloc(131073 + 32);
    unsigned char *b = (unsigned char *)malloc(131073 + 32);
    size_t size_index, a_offset, b_offset, position;

    assert(a && b);
    for (size_index = 0; size_index < sizeof(sizes) / sizeof(sizes[0]);
         size_index++) {
        size_t size = sizes[size_index];
        for (a_offset = 0; a_offset < 5; a_offset++) {
            for (b_offset = 0; b_offset < 5; b_offset++) {
                content_fill(a + offsets[a_offset], size,
                             (unsigned)(size + offsets[a_offset]));
                memcpy(b + offsets[b_offset], a + offsets[a_offset], size);
                content_check_pair(a + offsets[a_offset], b + offsets[b_offset],
                                   size, 1);
                {
                    size_t positions[3];
                    size_t position_index;
                    positions[0] = 0;
                    positions[1] = size / 2;
                    positions[2] = size - 1;
                    for (position_index = 0; position_index < 3; position_index++) {
                        size_t at = positions[position_index];
                        b[offsets[b_offset] + at] ^= 0x80;
                        content_check_pair(a + offsets[a_offset],
                                           b + offsets[b_offset], size, 0);
                        b[offsets[b_offset] + at] ^= 0x80;
                    }
                }
            }
        }
    }

    /* Full 0..31 alignment matrix at the two principal large boundaries. */
    {
        static const size_t align_sizes[] = {65536, 131072};
        size_t align_index, a_align, b_align;
        for (align_index = 0; align_index < 2; align_index++) {
            size_t size = align_sizes[align_index];
            for (a_align = 0; a_align < 32; a_align++) {
                for (b_align = 0; b_align < 32; b_align++) {
                    content_fill(a + a_align, size, (unsigned)(size + a_align));
                    memcpy(b + b_align, a + a_align, size);
                    content_check_pair(a + a_align, b + b_align, size, 1);
                    {
                        size_t position_index;
                        for (position_index = 0; position_index < 3; position_index++) {
                            size_t at = position_index == 0 ? 0 :
                                        position_index == 1 ? size / 2 : size - 1;
                            b[b_align + at] ^= 0x80;
                            content_check_pair(a + a_align, b + b_align, size, 0);
                            b[b_align + at] ^= 0x80;
                        }
                    }
                }
            }
        }
    }

    /* 64 KiB: every 16-byte block and every byte in three 128-byte windows. */
    content_fill(a, 65536, 0x1234);
    memcpy(b, a, 65536);
    for (position = 0; position < 65536; position += 16) {
        b[position] ^= 0x80;
        content_check_pair(a, b, 65536, 0);
        b[position] ^= 0x80;
    }
    {
        static const size_t windows[] = {0, 32768 - 64, 65536 - 128};
        size_t window_index;
        for (window_index = 0; window_index < 3; window_index++) {
            for (position = 0; position < 128; position++) {
                size_t at = windows[window_index] + position;
                b[at] ^= 0x80;
                content_check_pair(a, b, 65536, 0);
                b[at] ^= 0x80;
            }
        }
    }

    /* 128 KiB: every 128-byte block and every byte in three windows. */
    content_fill(a, 131072, 0x5678);
    memcpy(b, a, 131072);
    for (position = 0; position < 131072; position += 128) {
        b[position] ^= 0x80;
        content_check_pair(a, b, 131072, 0);
        b[position] ^= 0x80;
    }
    {
        static const size_t windows[] = {0, 65536 - 64, 131072 - 128};
        size_t window_index;
        for (window_index = 0; window_index < 3; window_index++) {
            for (position = 0; position < 128; position++) {
                size_t at = windows[window_index] + position;
                b[at] ^= 0x80;
                content_check_pair(a, b, 131072, 0);
                b[at] ^= 0x80;
            }
        }
    }

    /* Deterministic multiple-byte differences at both large sizes. */
    {
        unsigned state = 0x9E3779B9u;
        size_t round;
        for (round = 0; round < 24; round++) {
            size_t size = (round & 1) ? 131072 : 65536;
            content_fill(a, size, (unsigned)round);
            memcpy(b, a, size);
            for (position = 0; position < 8; position++) {
                size_t at;
                state = state * 1664525u + 1013904223u;
                at = state % size;
                b[at] ^= (unsigned char)(0x40u >> (position & 3));
            }
            if (!memcmp(a, b, size))
                b[0] ^= 0x80;
            content_check_pair(a, b, size, 0);
        }
    }
    free(a);
    free(b);
}

static void content_test_production_mutations(void)
{
    static const size_t positions[] = {0, 15, 16, 127, 128, 4095};
    unsigned char *watched = (unsigned char *)noble_low_malloc(4096);
    unsigned long generation;
    size_t index;

    assert(watched);
    content_fill(watched, 4096, 0x2468);
    generation = memory_watch_generation((unsigned long)(uintptr_t)watched, 4096);
    assert(generation != 0);
    assert(generation == memory_watch_generation(
                           (unsigned long)(uintptr_t)watched, 4096));
    for (index = 0; index < sizeof(positions) / sizeof(positions[0]); index++) {
        size_t position = positions[index];
        unsigned char saved = watched[position];
        unsigned long next;
        watched[position] ^= 0x80;
        next = memory_watch_generation((unsigned long)(uintptr_t)watched, 4096);
        assert(next > generation);
        generation = next;
        watched[position] = saved;
        next = memory_watch_generation((unsigned long)(uintptr_t)watched, 4096);
        assert(next > generation);
        generation = next;
    }
    assert(generation == memory_watch_generation(
                           (unsigned long)(uintptr_t)watched, 4096));
    noble_low_free(watched);
}

static volatile unsigned long content_benchmark_sink;

static void content_benchmark_case(size_t size, const char *name, long diff,
                                   int repetitions)
{
    unsigned char *a = (unsigned char *)malloc(size);
    unsigned char *b = (unsigned char *)malloc(size);
    int mode, iteration;

    if (!a || !b) {
        free(a);
        free(b);
        printf("content compare bench: size=%zu case=%s skipped (allocation)\n",
               size, name);
        return;
    }
    content_fill(a, size, (unsigned)size);
    memcpy(b, a, size);
    if (diff >= 0)
        b[(size_t)diff] ^= 0x80;

    /* Warm both paths, alternating, before either measurement. */
    for (iteration = 0; iteration < 256; iteration++) {
        content_benchmark_sink += (unsigned long)noble_d3d8_content_equal_test(
            a, b, size, iteration & 1);
    }
    for (mode = 0; mode < 2; mode++) {
        clock_t start = clock(), end;
        double seconds;
        if (start == (clock_t)-1)
            continue;
        for (iteration = 0; iteration < repetitions; iteration++) {
            content_benchmark_sink += (unsigned long)noble_d3d8_content_equal_test(
                a, b, size, mode);
        }
        end = clock();
        if (end == (clock_t)-1 || end <= start)
            continue;
        seconds = (double)(end - start) / (double)CLOCKS_PER_SEC;
        printf("content compare bench: size=%zu case=%s mode=%s reps=%d ns/call=%.1f MB/s=%.1f\n",
               size, name, mode ? "memcmp" : "sse2", repetitions,
               seconds * 1e9 / (double)repetitions,
               (double)size * (double)repetitions / (seconds * 1e6));
    }
    free(a);
    free(b);
}

static void content_compare_benchmark_if_requested(void)
{
    const char *enabled = getenv("RECOMP_CONTENT_COMPARE_BENCH");
    if (!enabled || !*enabled || !strcmp(enabled, "0"))
        return;
    content_benchmark_case(65536, "equal", -1, 10000);
    content_benchmark_case(65536, "first", 0, 10000);
    content_benchmark_case(65536, "middle", 65536 / 2, 10000);
    content_benchmark_case(65536, "last", 65535, 10000);
    content_benchmark_case(131072, "equal", -1, 5000);
    content_benchmark_case(131072, "first", 0, 5000);
    content_benchmark_case(131072, "middle", 131072 / 2, 5000);
    content_benchmark_case(131072, "last", 131071, 5000);
}

static void content_compare_selftests(void)
{
    content_test_guard_pages();
    content_test_small_exhaustive();
    content_test_alignment_matrix();
    content_test_large_boundaries();
    content_test_production_mutations();
}

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
    /* The P8 upload decode's fast path must convert exactly what the general
     * texel conversion does (linear and swizzled, with and without palette). */
    assert(noble_d3d8_decode_p8_selftest());
    content_compare_selftests();
    content_compare_benchmark_if_requested();
    puts("D3D8GL offset, state layout, FVF, mutable upload, allocator, P8 decode and content compare checks passed");
    return 0;
}
