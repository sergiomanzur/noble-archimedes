/*
 * Native SSE2 replacement for the guest XMV YUV 4:2:0 -> ARGB converter
 * (guest sub_004005EC, the LIN_A8R8G8B8 path used by the movie surface).
 *
 * The recompiled guest converter emulates the MMX/SSE inner loop lane by lane
 * and is the dominant cost of movie decode.  This implementation is ported
 * from BLACK-2006-recomp's black_xmv_yuv.inc and preserves the guest's exact
 * arithmetic (float Y' = (Y-16)*38139, chroma scaled by the same constants,
 * cvtps2dq, >> 15, packssdw/packuswb, alpha 0xFF) and its macroblock walk.
 *
 * All guest addresses include the runtime's mapping offset, including ESP
 * and high contiguous-memory movie surfaces.
 */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <emmintrin.h>

extern __declspec(thread) uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;

static __forceinline uint8_t *yuv_guest(uint32_t address)
{
    return (uint8_t *)((uintptr_t)g_xbox_mem_offset + address);
}

static __forceinline __m128 yuv_f32(uint32_t bits) { return _mm_castsi128_ps(_mm_set1_epi32((int)bits)); }

static __forceinline void yuv_pixels4(const uint8_t *y, __m128 uf, __m128 vf, uint8_t *dst)
{
    const __m128 cb = yuv_f32(0x47812700u), cr = yuv_f32(0x474C4A00u);
    const __m128 cgv = yuv_f32(0xC6D02000u), cgu = yuv_f32(0xC6483000u);
    const __m128i zero = _mm_setzero_si128();
    uint32_t y4;
    __m128i yw, yd, bi, gi, ri, b8, g8, r8, bg, ra;
    __m128 yf, bf, gf, rf;
    memcpy(&y4, y, 4);
    yw = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)y4), zero);
    yw = _mm_add_epi16(yw, _mm_set1_epi16(-16));
    yd = _mm_madd_epi16(_mm_unpacklo_epi16(yw, yw), _mm_set1_epi32(0x400054FB));
    yf = _mm_cvtepi32_ps(yd);
    bf = _mm_add_ps(_mm_mul_ps(uf, cb), yf);
    rf = _mm_add_ps(_mm_mul_ps(vf, cr), yf);
    gf = _mm_add_ps(_mm_add_ps(yf, _mm_mul_ps(vf, cgv)), _mm_mul_ps(uf, cgu));
    bi = _mm_srai_epi32(_mm_cvtps_epi32(bf), 15);
    gi = _mm_srai_epi32(_mm_cvtps_epi32(gf), 15);
    ri = _mm_srai_epi32(_mm_cvtps_epi32(rf), 15);
    b8 = _mm_packus_epi16(_mm_packs_epi32(bi, bi), zero);
    g8 = _mm_packus_epi16(_mm_packs_epi32(gi, gi), zero);
    r8 = _mm_packus_epi16(_mm_packs_epi32(ri, ri), zero);
    bg = _mm_unpacklo_epi8(b8, g8);
    ra = _mm_unpacklo_epi8(r8, _mm_set1_epi8((char)0xFF));
    _mm_storeu_si128((__m128i *)dst, _mm_unpacklo_epi16(bg, ra));
}

static void yuv_block16(const uint8_t *Y, size_t ys, const uint8_t *U, const uint8_t *V, size_t cs,
                        uint8_t *dst, size_t ds)
{
    const __m128i zero = _mm_setzero_si128();
    int pair, half, q;
    for (pair = 0; pair < 8; pair++) {
        const uint8_t *y0 = Y + (size_t)(2 * pair) * ys, *y1 = y0 + ys;
        const uint8_t *u = U + (size_t)pair * cs, *v = V + (size_t)pair * cs;
        uint8_t *d0 = dst + (size_t)(2 * pair) * ds, *d1 = d0 + ds;
        for (half = 0; half < 2; half++) {
            uint32_t u4, v4;
            __m128 uf, vf;
            memcpy(&u4, u + 4 * half, 4);
            memcpy(&v4, v + 4 * half, 4);
            uf = _mm_cvtepi32_ps(_mm_sub_epi32(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)u4), zero), zero),
                                               _mm_set1_epi32(128)));
            vf = _mm_cvtepi32_ps(_mm_sub_epi32(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)v4), zero), zero),
                                               _mm_set1_epi32(128)));
            for (q = 0; q < 2; q++) {
                __m128 uq = q ? _mm_shuffle_ps(uf, uf, _MM_SHUFFLE(3, 3, 2, 2)) : _mm_shuffle_ps(uf, uf, _MM_SHUFFLE(1, 1, 0, 0));
                __m128 vq = q ? _mm_shuffle_ps(vf, vf, _MM_SHUFFLE(3, 3, 2, 2)) : _mm_shuffle_ps(vf, vf, _MM_SHUFFLE(1, 1, 0, 0));
                size_t x = (size_t)(8 * half + 4 * q);
                yuv_pixels4(y0 + x, uq, vq, d0 + x * 4);
                yuv_pixels4(y1 + x, uq, vq, d1 + x * 4);
            }
        }
    }
}

static void yuv_convert(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                        uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8)
{
    size_t ys = (size_t)a0 * 16, cs = (size_t)a0 * 8;
    uint32_t right_pad = a0 * 16 - a5;
    uint32_t bottom_pad = a1 * 16 - a6;
    uint32_t bx, by;
    uint8_t scratch[16 * 64];
    for (by = 0; by < a1; by++) {
        const uint8_t *Y = yuv_guest(a2) + (size_t)by * ys * 16;
        const uint8_t *U = yuv_guest(a3) + (size_t)by * cs * 8;
        const uint8_t *V = yuv_guest(a4) + (size_t)by * cs * 8;
        uint8_t *dst = yuv_guest(a7) + (size_t)by * a8 * 16;
        for (bx = 0; bx < a0; bx++, Y += 16, U += 8, V += 8, dst += 64) {
            int last_col = bx == a0 - 1, last_row = by == a1 - 1;
            int partial = (last_col && right_pad != 0) || (last_row && bottom_pad != 0);
            if (!partial) {
                yuv_block16(Y, ys, U, V, cs, dst, a8);
            } else {
                uint32_t cols = last_col ? 16 - right_pad : 16, rows = last_row ? 16 - bottom_pad : 16, r;
                yuv_block16(Y, ys, U, V, cs, scratch, 64);
                for (r = 0; r < rows; r++)
                    memcpy(dst + (size_t)r * a8, scratch + (size_t)r * 64, (size_t)cols * 4);
            }
        }
    }
}

/* Nine guest arguments, callee cleanup: ret 36 (including the return address,
 * advance ESP by 40). The generated CC annotation incorrectly says cdecl. */
int noble_yuv_hook(void)
{
    static int logged;
    uint32_t sp = g_esp;
    uint32_t a0 = *(volatile uint32_t *)yuv_guest(sp + 4);
    uint32_t a1 = *(volatile uint32_t *)yuv_guest(sp + 8);
    uint32_t a2 = *(volatile uint32_t *)yuv_guest(sp + 12);
    uint32_t a3 = *(volatile uint32_t *)yuv_guest(sp + 16);
    uint32_t a4 = *(volatile uint32_t *)yuv_guest(sp + 20);
    uint32_t a5 = *(volatile uint32_t *)yuv_guest(sp + 24);
    uint32_t a6 = *(volatile uint32_t *)yuv_guest(sp + 28);
    uint32_t a7 = *(volatile uint32_t *)yuv_guest(sp + 32);
    uint32_t a8 = *(volatile uint32_t *)yuv_guest(sp + 36);
    if (!logged) {
        logged = 1;
        fprintf(stderr, "[XMVYUV] a0=%u a1=%u Y=%08X U=%08X V=%08X w=%u h=%u dst=%08X pitch=%u\n",
                a0, a1, a2, a3, a4, a5, a6, a7, a8);
    }
    yuv_convert(a0, a1, a2, a3, a4, a5, a6, a7, a8);
    g_esp = sp + 40;
    return 1;
}
