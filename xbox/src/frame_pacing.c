#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

extern ptrdiff_t g_xbox_mem_offset;
extern void nv2a_pb_perf_report(void);
extern uint32_t xbox_GuestTickCount(void);
extern uint32_t xbox_VblankCount(void);

/* Runs only on the GPU consumer thread, once per completed flip. The game's
 * movie-player object at 0x012B57E0 keeps its active flag at +0x48. The state
 * stack is a set of nested coroutines; their small state numbers do not
 * identify movies (menus also use states 5 and 7). */
void recomp_present_frame(void)
{
    static LARGE_INTEGER frequency;
    static LONGLONG deadline;
    static LONGLONG fraction;
    static unsigned previous_fps;
    static unsigned calls;
    static unsigned report_calls, report_movie, report_resets;
    static LONGLONG report_wait_ticks, report_period_ticks, report_last_call;
    static LONGLONG period_min, period_max;
    static DWORD report_ms;
    static int config;
    LARGE_INTEGER now;
    unsigned fps = 60;
    uint32_t movie = g_xbox_mem_offset ? *(volatile uint32_t *)
            ((uintptr_t)g_xbox_mem_offset + 0x012B5828u) : 0;

    if (g_xbox_mem_offset && getenv("RECOMP_SKIP_MOVIE")) {
        static DWORD skip_start;
        DWORD now_ms = GetTickCount();
        if (!skip_start)
            skip_start = now_ms;
        if (now_ms - skip_start > 35000) {
            *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x012B5828u) = 0;
            movie = 0;
        }
    }

    if (!config) {
        const char *force = getenv("RECOMP_PACE_FPS");
        config = force && *force ? atoi(force) : -1;
        if (config <= 0) config = -1;
        if (getenv("RECOMP_PACE_OFF")) config = 0;
    }
    if (config == 0)
        return;

    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);

    if (config > 0)
        fps = (unsigned)config;
    else if (movie && getenv("RECOMP_MOVIE_30"))
        fps = 30;
    else
        fps = 60;

    if (!previous_fps) {
        deadline = now.QuadPart;
    } else {
        /* Use the new period immediately on a transition. Retain sub-tick
         * fractions so 60 Hz does not slowly drift on arbitrary QPC clocks. */
        if (fps != previous_fps) fraction = 0;
        LONGLONG ticks = frequency.QuadPart + fraction;
        deadline += ticks / fps;
        fraction = ticks % fps;
        /* Do not emit bursts of catch-up frames after loading or a slow draw. */
        if (now.QuadPart > deadline) {
            deadline = now.QuadPart;
            fraction = 0;
            report_resets++;
        }
    }
    previous_fps = fps;
    while (now.QuadPart < deadline) {
        LONGLONG remaining = deadline - now.QuadPart;
        DWORD ms = (DWORD)(remaining * 1000 / frequency.QuadPart);
        if (ms > 1) Sleep(ms - 1);
        else YieldProcessor();
        QueryPerformanceCounter(&now);
    }

    calls++;
    report_calls++;
    report_movie += movie != 0;
    report_wait_ticks += deadline - now.QuadPart;
    if (report_last_call) {
        LONGLONG period = now.QuadPart - report_last_call;
        report_period_ticks += period;
        if (!period_min || period < period_min) period_min = period;
        if (period > period_max) period_max = period;
    }
    report_last_call = now.QuadPart;
    {
        DWORD t = GetTickCount();
        if (report_ms && t - report_ms >= 1000) {
            static int enabled = -1, gpu_stats = -1;
            if (enabled < 0)
                enabled = getenv("RECOMP_PACE_STATS") != NULL;
            if (gpu_stats < 0)
                gpu_stats = getenv("RECOMP_GPU_STATS") != NULL;
            if (gpu_stats)
                nv2a_pb_perf_report();
            if (enabled) {
                double avg_ms = report_calls
                    ? (double)report_period_ticks * 1000.0
                      / (frequency.QuadPart * report_calls) : 0.0;
                fprintf(stderr, "[PACE] %u flips/s fps=%u movie=%u resets=%u "
                        "period=%.2fms min=%.2f max=%.2f wait=%.2fms flag=%08X\n",
                        report_calls, fps, report_movie, report_resets,
                        avg_ms,
                        (double)period_min * 1000.0 / frequency.QuadPart,
                        (double)period_max * 1000.0 / frequency.QuadPart,
                        (double)report_wait_ticks * 1000.0
                          / (frequency.QuadPart * (report_calls ? report_calls : 1)),
                        movie);
            }
            if (getenv("RECOMP_BENCH_STATS")) {
                static DWORD bench_start;
                DWORD bench_now = GetTickCount();
                if (!bench_start)
                    bench_start = bench_now;
                fprintf(stderr, "[BENCH] ms=%lu\n", (unsigned long)(bench_now - bench_start));
                fprintf(stderr, "[TICK] guest_ms=%u vblank=%u flips=%u\n",
                        xbox_GuestTickCount(), xbox_VblankCount(),
                        (unsigned)calls);
            }
            report_calls = report_movie = report_resets = 0;
            report_period_ticks = 0; period_min = period_max = 0;
            report_wait_ticks = 0;
            report_ms = t;
        } else if (!report_ms) {
            report_ms = GetTickCount();
            if (!report_ms) report_ms = 1;
        }
    }
}
