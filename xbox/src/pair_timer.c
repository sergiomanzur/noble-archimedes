/* Balanced, nested pair timer for sub_00042E40 (parent) and sub_00012740
 * (child).  Function-boundary hooks only; results stay in memory and are
 * emitted once when the presented-flip window closes.
 *
 * The core (pt_begin/pt_end) is pure and unit-tested; the wrappers add QPC,
 * the window gate, and the file dump.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

#define PT_IDS 8
#define PT_PARENT 0u
#define PT_CHILD 1u

struct pt_timer {
    LARGE_INTEGER t0;
    int active;
};

struct pt_stats {
    unsigned long long calls;
    unsigned long long inclusive_ns;
    unsigned long long child_in_parent_ns;
    unsigned long long unmatched_enter;
    unsigned long long unmatched_exit;
};

struct pt_ctx {
    struct pt_timer t[PT_IDS];
    struct pt_stats s[PT_IDS];
};

static void pt_begin(struct pt_ctx *c, unsigned id, LONGLONG t)
{
    if (id >= PT_IDS)
        return;
    if (c->t[id].active) {
        c->s[id].unmatched_enter++;
        return;
    }
    c->t[id].active = 1;
    c->t[id].t0.QuadPart = t;
}

static void pt_end(struct pt_ctx *c, unsigned id, unsigned parent_id,
                   LONGLONG t)
{
    if (id >= PT_IDS || !c->t[id].active) {
        if (id < PT_IDS)
            c->s[id].unmatched_exit++;
        return;
    }
    c->t[id].active = 0;
    c->s[id].calls++;
    c->s[id].inclusive_ns +=
        (unsigned long long)(t - c->t[id].t0.QuadPart);
    if (parent_id < PT_IDS && c->t[parent_id].active) {
        unsigned long long span =
            (unsigned long long)(t - c->t[id].t0.QuadPart);
        c->s[parent_id].child_in_parent_ns += span; /* parent's children sum */
        c->s[id].child_in_parent_ns += span;        /* child's inside-parent time */
    }
}

/* ---- live wrappers ---- */

static struct pt_ctx pt;
static int pt_on;
static int pt_inited;
static int pt_window_active;
static unsigned long pt_start_flip;
static unsigned long pt_end_flip;
static char pt_file[1024];
static int pt_dumped;
static LARGE_INTEGER pt_freq;

void recomp_pair_time_init(void)
{
    const char *value;
    const char *file;
    if (pt_inited)
        return;
    pt_inited = 1;
    value = getenv("RECOMP_PAIR_TIME_42E40");
    if (!value || !*value || !strcmp(value, "0"))
        return;
    pt_on = 1;
    QueryPerformanceFrequency(&pt_freq);
    value = getenv("RECOMP_PAIR_TIME_42E40_START");
    pt_start_flip = value ? strtoul(value, NULL, 0) : 0;
    value = getenv("RECOMP_PAIR_TIME_42E40_END");
    pt_end_flip = value ? strtoul(value, NULL, 0) : 0;
    file = getenv("RECOMP_PAIR_TIME_42E40_FILE");
    snprintf(pt_file, sizeof(pt_file), "%s",
             (file && *file) ? file : "42e40_pairtime.csv");
}

void recomp_pair_time_update_window(unsigned long flip)
{
    if (!pt_on)
        return;
    if (pt_start_flip && flip < pt_start_flip) {
        pt_window_active = 0;
        return;
    }
    if (pt_end_flip && flip >= pt_end_flip) {
        if (!pt_dumped) {
            FILE *f = fopen(pt_file, "w");
            if (f) {
                unsigned i;
                const char *names[PT_IDS] = {
                    "parent_42E40", "child_12740", "r2", "r3",
                    "r4", "r5", "r6", "r7" };
                fprintf(f, "id,name,calls,inclusive_ms,child_in_parent_ms,"
                        "exclusive_ms,unmatched_enter,unmatched_exit\n");
                for (i = 0; i < PT_IDS; i++) {
                    unsigned long long child = pt.s[i].child_in_parent_ns;
                    unsigned long long excl = pt.s[i].inclusive_ns - child;
                    double incl_ms = pt.s[i].inclusive_ns * 1000.0
                                     / (double)pt_freq.QuadPart;
                    double child_ms = child * 1000.0
                                     / (double)pt_freq.QuadPart;
                    double excl_ms = excl * 1000.0
                                     / (double)pt_freq.QuadPart;
                    fprintf(f, "%u,%s,%llu,%.6f,%.6f,%.6f,%llu,%llu\n",
                            i, names[i], pt.s[i].calls, incl_ms, child_ms,
                            excl_ms, pt.s[i].unmatched_enter,
                            pt.s[i].unmatched_exit);
                }
                fclose(f);
            }
            pt_dumped = 1;
        }
        pt_on = 0;
        return;
    }
    pt_window_active = 1;
}

void recomp_pt_enter(unsigned id)
{
    LARGE_INTEGER now;
    if (!pt_on || !pt_window_active)
        return;
    QueryPerformanceCounter(&now);
    pt_begin(&pt, id, now.QuadPart);
}

void recomp_pt_exit(unsigned id)
{
    LARGE_INTEGER now;
    if (!pt_on || !pt_window_active)
        return;
    QueryPerformanceCounter(&now);
    pt_end(&pt, id, PT_PARENT, now.QuadPart);
}

int recomp_pair_time_enabled(void)
{
    return pt_on;
}
