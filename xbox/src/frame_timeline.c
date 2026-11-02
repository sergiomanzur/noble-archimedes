/*
 * Bounded, opt-in per-frame timeline for the HLE rendering path.
 *
 * Recording is in memory; the CSV is written once when the requested flip
 * window closes, so per-event file I/O does not distort the timeline.  It is
 * disabled unless RECOMP_FRAME_TIMELINE=1.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMELINE_CAPACITY 131072

struct timeline_event {
    unsigned long long time_ns;
    unsigned frame;
    unsigned long flip;
    unsigned long tid;
    const char *kind;
    const char *detail;
    double value0;
    double value1;
};

static int enabled = -1;
static int started;
static int flushed;
static unsigned long start_flip;
static unsigned long end_flip;
static LARGE_INTEGER frequency;
static LARGE_INTEGER origin;
static CRITICAL_SECTION lock;
static int lock_ready;
static unsigned current_frame;
static unsigned long long event_count;
static struct timeline_event events[TIMELINE_CAPACITY];
static unsigned event_count_in_memory;
static char timeline_path[1024];

extern unsigned long xbox_FramebufferFlipCount(void);

static unsigned long long now_ns(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (unsigned long long)((now.QuadPart - origin.QuadPart)
                                * 1000000000LL / frequency.QuadPart);
}

static void flush_locked(void)
{
    FILE *file;
    unsigned i;
    if (flushed)
        return;
    flushed = 1;
    file = fopen(timeline_path, "w");
    if (!file) {
        enabled = 0;
        return;
    }
    fprintf(file, "time_ns,frame,flip,tid,kind,detail,value0,value1\n");
    for (i = 0; i < event_count_in_memory; i++) {
        const struct timeline_event *e = &events[i];
        fprintf(file, "%llu,%u,%lu,%lu,%s,%s,%.3f,%.3f\n",
                e->time_ns, e->frame, e->flip, e->tid, e->kind,
                e->detail ? e->detail : "", e->value0, e->value1);
    }
    fclose(file);
    enabled = 0;
}

static int in_window(void)
{
    unsigned long flip = (unsigned long)xbox_FramebufferFlipCount();
    if (start_flip && flip < start_flip)
        return 0;
    if (end_flip && flip > end_flip)
        return 0;
    return 1;
}

static void append_event(const char *kind, const char *detail,
                         double value0, double value1)
{
    struct timeline_event *e;
    unsigned long flip;
    if (!lock_ready)
        return;
    EnterCriticalSection(&lock);
    if (enabled <= 0) {
        LeaveCriticalSection(&lock);
        return;
    }
    flip = (unsigned long)xbox_FramebufferFlipCount();
    if (!in_window()) {
        if (started && !flushed)
            flush_locked();
        LeaveCriticalSection(&lock);
        return;
    }
    started = 1;
    if (event_count_in_memory >= TIMELINE_CAPACITY) {
        flush_locked();
        LeaveCriticalSection(&lock);
        return;
    }
    e = &events[event_count_in_memory++];
    e->time_ns = now_ns();
    e->frame = current_frame;
    e->flip = flip;
    e->tid = (unsigned long)GetCurrentThreadId();
    e->kind = kind;
    e->detail = detail;
    e->value0 = value0;
    e->value1 = value1;
    event_count++;
    LeaveCriticalSection(&lock);
}

void recomp_timeline_init(void)
{
    const char *value;
    const char *path;
    const char *root;
    if (enabled >= 0)
        return;
    value = getenv("RECOMP_FRAME_TIMELINE");
    enabled = value && *value && strcmp(value, "0") != 0;
    if (!enabled)
        return;
    {
        const char *s = getenv("RECOMP_FRAME_TIMELINE_START");
        const char *e = getenv("RECOMP_FRAME_TIMELINE_END");
        start_flip = s ? strtoul(s, NULL, 0) : 0;
        end_flip = e ? strtoul(e, NULL, 0) : 0;
    }
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&origin);
    InitializeCriticalSection(&lock);
    lock_ready = 1;
    path = getenv("RECOMP_FRAME_TIMELINE_FILE");
    if (!path || !*path) {
        root = getenv("RECOMP_FRAME_TIMELINE_DIR");
        if (!root || !*root)
            root = "output";
        snprintf(timeline_path, sizeof(timeline_path),
                 "%s\\frame_timeline.csv", root);
    } else {
        snprintf(timeline_path, sizeof(timeline_path), "%s", path);
    }
}

int recomp_timeline_enabled(void)
{
    return enabled > 0;
}

void recomp_timeline_event(const char *kind, const char *detail,
                           double value0, double value1)
{
    if (enabled > 0)
        append_event(kind, detail, value0, value1);
}

unsigned recomp_timeline_swap_begin(void)
{
    if (enabled <= 0 || !in_window())
        return current_frame;
    if (!current_frame) {
        EnterCriticalSection(&lock);
        current_frame = 1;
        LeaveCriticalSection(&lock);
    }
    append_event("logic_end", "pre_swap", 0, 0);
    append_event("swap_begin", "swap", 0, 0);
    return current_frame;
}

void recomp_timeline_swap_end(void)
{
    unsigned long flip;
    if (enabled <= 0 || !in_window())
        return;
    append_event("swap_end", "swap", 0, 0);
    EnterCriticalSection(&lock);
    current_frame++;
    LeaveCriticalSection(&lock);
    append_event("logic_begin", "post_swap", 0, 0);
    flip = (unsigned long)xbox_FramebufferFlipCount();
    if (end_flip && flip >= end_flip) {
        EnterCriticalSection(&lock);
        flush_locked();
        LeaveCriticalSection(&lock);
    }
}

unsigned recomp_timeline_frame(void)
{
    return current_frame;
}

void recomp_timeline_dsp_enter(unsigned long microseconds)
{
    recomp_timeline_event("dsp_enter", "KeStall", (double)microseconds, 0);
}

void recomp_timeline_dsp_exit(unsigned long microseconds,
                              unsigned long long actual_us)
{
    recomp_timeline_event("dsp_exit", "KeStall", (double)microseconds,
                          (double)actual_us);
}

void recomp_timeline_summary(void)
{
    if (enabled > 0 && lock_ready) {
        EnterCriticalSection(&lock);
        flush_locked();
        LeaveCriticalSection(&lock);
    }
    if (event_count)
        fprintf(stderr, "[TIMELINE] events=%llu frames=%u\n",
                event_count, current_frame);
}
