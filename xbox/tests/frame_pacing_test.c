#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static LONGLONG clock_ticks;
static BOOL test_frequency(LARGE_INTEGER *value)
{ value->QuadPart = 6000000; return TRUE; }
static BOOL test_counter(LARGE_INTEGER *value)
{ value->QuadPart = clock_ticks++; return TRUE; }
static void test_sleep(DWORD ms) { clock_ticks += (LONGLONG)ms * 6000; }

/* The host build reports GPU timings through this hook; the pacing fixture
 * compiles only frame_pacing.c, so satisfy the symbol here. */
void nv2a_pb_perf_report(void) {}
uint32_t xbox_GuestTickCount(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint32_t xbox_FramebufferFlipCount(void) { return 0; }
void xbox_StallStatsGet(unsigned long long *calls, unsigned long long *requested,
                        unsigned long long *actual, unsigned long long *dsp)
{
    if (calls) *calls = 0;
    if (requested) *requested = 0;
    if (actual) *actual = 0;
    if (dsp) *dsp = 0;
}
void xbox_StallStatsReset(void) {}

#define QueryPerformanceFrequency test_frequency
#define QueryPerformanceCounter test_counter
#define Sleep test_sleep
#include "../src/frame_pacing.c"

ptrdiff_t g_xbox_mem_offset;

int main(void)
{
    _putenv("RECOMP_PACE_FPS=");
    _putenv("RECOMP_PACE_OFF=");
    _putenv("RECOMP_MOVIE_30=");
    _putenv("RECOMP_SKIP_MOVIE=");
    unsigned char *memory = calloc(1, 0x1300000);
    g_xbox_mem_offset = (ptrdiff_t)memory;
    /* State 7 in a nested state machine is not evidence of movie playback. */
    *(uint32_t *)(memory + 0xA4E2DC) = 7;
    recomp_present_frame();
    LONGLONG before = clock_ticks;
    recomp_present_frame();
    if (clock_ticks - before < 99000 || clock_ticks - before > 101000) {
        fprintf(stderr, "Menu frame must take 1/60s; got %lld ticks\n", clock_ticks - before);
        return 1;
    }
    /* Movies retain the default 60 Hz presentation cadence. */
    *(uint32_t *)(memory + 0x12B5828) = 1;
    before = clock_ticks;
    recomp_present_frame();
    if (clock_ticks - before < 99000 || clock_ticks - before > 101000) {
        fprintf(stderr, "Default movie frame must take 1/60s; got %lld ticks\n", clock_ticks - before);
        return 1;
    }
    /* The optional 30 Hz mode takes effect immediately. */
    _putenv("RECOMP_MOVIE_30=1");
    before = clock_ticks;
    recomp_present_frame();
    if (clock_ticks - before < 199000 || clock_ticks - before > 201000) {
        fprintf(stderr, "Movie frame must immediately take 1/30s; got %lld ticks\n", clock_ticks - before);
        return 1;
    }
    *(uint32_t *)(memory + 0x12B5828) = 0;
    before = clock_ticks;
    recomp_present_frame();
    if (clock_ticks - before < 99000 || clock_ticks - before > 101000) return 1;
    /* A long render/load must not result in several uncapped catch-up flips. */
    clock_ticks += 6000000;
    recomp_present_frame();
    before = clock_ticks;
    recomp_present_frame();
    if (clock_ticks - before < 99000 || clock_ticks - before > 101000) return 1;
    free(memory);
    puts("Default 60 Hz, optional 30 Hz movies, transitions and recovery passed.");
    return 0;
}

