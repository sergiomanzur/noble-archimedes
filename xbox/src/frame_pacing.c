#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;
extern void nv2a_pb_perf_report(void);
extern uint32_t xbox_GuestTickCount(void);
extern uint32_t xbox_VblankCount(void);
extern uint32_t xbox_FramebufferFlipCount(void);
extern void xbox_StallStatsGet(unsigned long long *, unsigned long long *,
                               unsigned long long *, unsigned long long *);
extern void xbox_StallStatsReset(void);
extern int recomp_call_profile_on;
extern void recomp_guest_profile_update_window(unsigned long flip);
extern int recomp_count_on;
extern void recomp_count_update_window(unsigned long flip);
extern int recomp_pair_time_enabled(void);
extern void recomp_pair_time_update_window(unsigned long flip);
extern int recomp_render_capture_enabled(void);
extern void recomp_render_capture_update_window(unsigned long flip);
extern void xbox_StallStatsReset(void);

/* In-memory frame-duration recorder.  Enabled only with RECOMP_FRAME_STATS.
 * No per-frame formatting, allocation, file I/O, or percentile work: one
 * array store per presented frame, one file write at the end flip. */
#define FRAME_STATS_CAPACITY 65536
static LONGLONG g_fs_ticks[FRAME_STATS_CAPACITY];
static unsigned long g_fs_flips[FRAME_STATS_CAPACITY];
static unsigned char g_fs_capture[FRAME_STATS_CAPACITY];
static unsigned g_fs_count;
static int g_fs_enabled = -1;
static int g_fs_dumped;
static unsigned long g_fs_start;
static unsigned long g_fs_end;
static char g_fs_file[1024];
static unsigned long g_fs_capture_flips[32];
static unsigned g_fs_capture_count;
static LARGE_INTEGER g_fs_frequency;

static void frame_stats_init(void)
{
    const char *value;
    const char *file;
    const char *captures;
    if (g_fs_enabled >= 0)
        return;
    value = getenv("RECOMP_FRAME_STATS");
    g_fs_enabled = value && *value && strcmp(value, "0") != 0;
    if (!g_fs_enabled)
        return;
    QueryPerformanceFrequency(&g_fs_frequency);
    value = getenv("RECOMP_FRAME_STATS_START");
    g_fs_start = value ? strtoul(value, NULL, 0) : 0;
    value = getenv("RECOMP_FRAME_STATS_END");
    g_fs_end = value ? strtoul(value, NULL, 0) : 0;
    file = getenv("RECOMP_FRAME_STATS_FILE");
    snprintf(g_fs_file, sizeof(g_fs_file), "%s",
             (file && *file) ? file : "framestats.csv");
    captures = getenv("RECOMP_CAPTURE_FLIPS");
    if (captures && *captures) {
        const char *p = captures;
        while (*p && g_fs_capture_count < 32) {
            char *end;
            unsigned long flip = strtoul(p, &end, 0);
            if (end == p)
                break;
            g_fs_capture_flips[g_fs_capture_count++] = flip;
            p = end;
            while (*p == ',' || *p == ' ' || *p == '\t')
                p++;
        }
    }
}

static int frame_stats_capture_match(unsigned long flip)
{
    unsigned i;
    for (i = 0; i < g_fs_capture_count; i++)
        if (g_fs_capture_flips[i] == flip)
            return 1;
    return 0;
}

static void frame_stats_dump(void)
{
    FILE *file;
    unsigned i;
    if (g_fs_dumped || !g_fs_enabled)
        return;
    g_fs_dumped = 1;
    file = fopen(g_fs_file, "w");
    if (!file)
        return;
    fprintf(file, "flip,us,capture\n");
    for (i = 0; i < g_fs_count; i++) {
        fprintf(file, "%lu,%llu,%u\n", g_fs_flips[i],
                (unsigned long long)(g_fs_ticks[i] * 1000000LL
                                     / g_fs_frequency.QuadPart),
                (unsigned)g_fs_capture[i]);
    }
    fclose(file);
}

static void frame_stats_store(LONGLONG period_ticks)
{
    unsigned long flip;
    frame_stats_init();
    if (recomp_call_profile_on)
        recomp_guest_profile_update_window(
            (unsigned long)xbox_FramebufferFlipCount());
    if (recomp_count_on)
        recomp_count_update_window(
            (unsigned long)xbox_FramebufferFlipCount());
    if (recomp_pair_time_enabled())
        recomp_pair_time_update_window(
            (unsigned long)xbox_FramebufferFlipCount());
    if (recomp_render_capture_enabled())
        recomp_render_capture_update_window(
            (unsigned long)xbox_FramebufferFlipCount());
    if (g_fs_enabled <= 0)
        return;
    if (!g_fs_frequency.QuadPart)
        return;
    flip = (unsigned long)xbox_FramebufferFlipCount();
    if (g_fs_start && flip < g_fs_start)
        return;
    if (g_fs_count < FRAME_STATS_CAPACITY) {
        g_fs_ticks[g_fs_count] = period_ticks;
        g_fs_flips[g_fs_count] = flip;
        g_fs_capture[g_fs_count] =
            (unsigned char)frame_stats_capture_match(flip);
        g_fs_count++;
    }
    if (g_fs_end && flip >= g_fs_end)
        frame_stats_dump();
}

static int frame_ms_compare(const void *a, const void *b)
{
    LONGLONG x = *(const LONGLONG *)a, y = *(const LONGLONG *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}


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
    static LONGLONG frame_durations[2048];
    static unsigned frame_duration_count;
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
    frame_stats_init();
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
        if (frame_duration_count < sizeof(frame_durations) / sizeof(frame_durations[0]))
            frame_durations[frame_duration_count++] = period;
        frame_stats_store(period);
        {
            static int frame_log = -1;
            if (frame_log < 0)
                frame_log = getenv("RECOMP_FRAME_LOG") != NULL;
            if (frame_log)
                fprintf(stderr, "[FRAME] flip=%u us=%.0f\n",
                        xbox_FramebufferFlipCount(),
                        (double)period * 1000000.0 / frequency.QuadPart);
        }
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
                LONGLONG sorted[2048];
                unsigned count = frame_duration_count;
                LONGLONG p50 = 0, p95 = 0, p99 = 0, fmax = 0;
                memcpy(sorted, frame_durations, count * sizeof(sorted[0]));
                qsort(sorted, count, sizeof(sorted[0]), frame_ms_compare);
                if (count) {
                    p50 = sorted[(count - 1) * 50 / 100];
                    p95 = sorted[(count - 1) * 95 / 100];
                    p99 = sorted[(count - 1) * 99 / 100];
                    fmax = sorted[count - 1];
                }
                fprintf(stderr, "[PACE] %u flips/s fps=%u movie=%u resets=%u "
                        "period=%.2fms min=%.2f max=%.2f wait=%.2fms flag=%08X "
                        "frame_p50=%.2f p95=%.2f p99=%.2f max=%.2f frames=%u\n",
                        report_calls, fps, report_movie, report_resets,
                        avg_ms,
                        (double)period_min * 1000.0 / frequency.QuadPart,
                        (double)period_max * 1000.0 / frequency.QuadPart,
                        (double)report_wait_ticks * 1000.0
                          / (frequency.QuadPart * (report_calls ? report_calls : 1)),
                        movie,
                        (double)p50 * 1000.0 / frequency.QuadPart,
                        (double)p95 * 1000.0 / frequency.QuadPart,
                        (double)p99 * 1000.0 / frequency.QuadPart,
                        (double)fmax * 1000.0 / frequency.QuadPart,
                        count);
                {
                    unsigned long long calls, requested, actual, dsp;
                    xbox_StallStatsGet(&calls, &requested, &actual, &dsp);
                    fprintf(stderr, "[STALL] calls=%llu requested=%.2fms actual=%.2fms dsp10=%llu\n",
                            calls, requested / 1000.0, actual / 1000.0, dsp);
                    xbox_StallStatsReset();
                }
                {
                    static int hist_enabled = -1;
                    if (hist_enabled < 0)
                        hist_enabled = getenv("RECOMP_FRAME_HIST") != NULL;
                    if (hist_enabled) {
                        unsigned long long buckets[2001] = {0};
                        unsigned i;
                        for (i = 0; i < frame_duration_count; i++) {
                            double ms = (double)frame_durations[i] * 1000.0 / frequency.QuadPart;
                            unsigned b = ms <= 0.0 ? 0 : (unsigned)(ms / 0.1 + 0.5);
                            if (b > 2000) b = 2000;
                            buckets[b]++;
                        }
                        fprintf(stderr, "[FRAMEHIST] flip=%u", xbox_FramebufferFlipCount());
                        for (i = 0; i <= 2000; i++)
                            if (buckets[i]) fprintf(stderr, " %u:%llu", i, buckets[i]);
                        fputc('\n', stderr);
                    }
                }
            }
            if (getenv("RECOMP_BENCH_STATS")) {
                static DWORD bench_start;
                DWORD bench_now = GetTickCount();
                if (!bench_start)
                    bench_start = bench_now;
                fprintf(stderr, "[BENCH] ms=%lu flips=%u\n",
                        (unsigned long)(bench_now - bench_start),
                        xbox_FramebufferFlipCount());
                fprintf(stderr, "[TICK] guest_ms=%u vblank=%u flips=%u\n",
                        xbox_GuestTickCount(), xbox_VblankCount(),
                        (unsigned)calls);
            }
            report_calls = report_movie = report_resets = 0;
            report_period_ticks = 0; period_min = period_max = 0;
            report_wait_ticks = 0;
            frame_duration_count = 0;
            report_ms = t;
        } else if (!report_ms) {
            report_ms = GetTickCount();
            if (!report_ms) report_ms = 1;
        }
    }
}
