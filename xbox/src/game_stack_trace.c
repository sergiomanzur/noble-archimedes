/*
 * Optional [GAME_STACK] tracing for generated guest code.
 *
 * The recompiler emits per-push/pop diagnostic printf calls in several
 * generated functions.  They are useful when debugging the title's coroutine
 * stack, but they are pure host diagnostics and must not run in the measured
 * gameplay configuration.  The generated call sites test the exported flag;
 * this file formats and writes only when RECOMP_GAME_STACK_TRACE is set.
 *
 * RECOMP_IO_TRACE additionally records bounded per-emitter statistics and
 * flushes stdout so a trace run reports actual write completion.  The trace
 * itself is diagnostic and is never enabled by the benchmark.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>

int recomp_game_stack_trace_on = 0;

static int io_trace = 0;
static LARGE_INTEGER io_frequency;
static CRITICAL_SECTION io_lock;
static int io_lock_ready = 0;

#define IO_EMITTERS 8
struct io_emitter_stats {
    const char *emitter;
    DWORD tid;
    unsigned long long calls;
    unsigned long long bytes;
    unsigned long long elapsed_ns;
    void *caller;
};
static struct io_emitter_stats io_stats[IO_EMITTERS];
static unsigned io_emitter_count;
static unsigned long long io_total_calls;

void recomp_game_stack_trace_init(void)
{
    const char *value = getenv("RECOMP_GAME_STACK_TRACE");
    recomp_game_stack_trace_on = value && *value && strcmp(value, "0") != 0;
    io_trace = getenv("RECOMP_IO_TRACE") != NULL;
    if (io_trace) {
        QueryPerformanceFrequency(&io_frequency);
        InitializeCriticalSection(&io_lock);
        io_lock_ready = 1;
    }
}

static struct io_emitter_stats *find_stats(const char *emitter, DWORD tid)
{
    unsigned i;
    for (i = 0; i < io_emitter_count; i++) {
        if (io_stats[i].tid == tid && strcmp(io_stats[i].emitter, emitter) == 0)
            return &io_stats[i];
    }
    if (io_emitter_count >= IO_EMITTERS)
        return NULL;
    io_stats[io_emitter_count].emitter = emitter;
    io_stats[io_emitter_count].tid = tid;
    return &io_stats[io_emitter_count++];
}

static void dump_stats_locked(void)
{
    unsigned i;
    for (i = 0; i < io_emitter_count; i++) {
        struct io_emitter_stats *s = &io_stats[i];
        fprintf(stderr,
                "[IO-TRACE] source=%s tid=%lu dest=stdout caller=%p "
                "calls=%llu bytes=%llu elapsed_us=%.3f\n",
                s->emitter, (unsigned long)s->tid, s->caller,
                s->calls, s->bytes, (double)s->elapsed_ns / 1000.0);
    }
}

void recomp_game_stack_log(const char *emitter, const char *format, ...)
{
    char buffer[512];
    va_list args;
    int length;
    LARGE_INTEGER start, end;
    size_t written;
    DWORD tid;
    struct io_emitter_stats *stats = NULL;

    if (!recomp_game_stack_trace_on)
        return;

    va_start(args, format);
    length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length < 0)
        return;
    if (length >= (int)sizeof(buffer))
        length = (int)sizeof(buffer) - 1;
    written = (size_t)length;

    QueryPerformanceCounter(&start);
    fwrite(buffer, 1, written, stdout);
    if (io_trace)
        fflush(stdout);
    QueryPerformanceCounter(&end);

    if (!io_trace)
        return;

    tid = GetCurrentThreadId();
    if (io_lock_ready)
        EnterCriticalSection(&io_lock);
    stats = find_stats(emitter, tid);
    if (stats) {
        stats->calls++;
        stats->bytes += written;
        stats->elapsed_ns += (unsigned long long)
            ((end.QuadPart - start.QuadPart) * 1000000000LL / io_frequency.QuadPart);
        if (!stats->caller)
            stats->caller = _ReturnAddress();
    }
    io_total_calls++;
    if ((io_total_calls & 0x7ffu) == 0)
        dump_stats_locked();
    if (io_lock_ready)
        LeaveCriticalSection(&io_lock);
}
