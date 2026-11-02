/*
 * Bounded guest-call profiler for recompiled Xbox functions.
 *
 * RECOMP_GUEST_PROFILE=1 enables enter/exit timing around every
 * RECOMP_ABI_CALL site.  Data is kept in memory (per-VA calls, inclusive ns,
 * children ns) and written once when the presented-flip window closes.
 * Exclusive time = inclusive - children, so nested calls are not counted
 * twice.  HLE-intercepted and kernel-range VAs stay in the same table and are
 * classified later by address.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int recomp_call_profile_on = 0;

#define GP_TABLE_BITS 14
#define GP_TABLE_SIZE (1u << GP_TABLE_BITS)
#define GP_STACK_MAX 64

struct gp_entry {
    uint32_t va;
    unsigned long long calls;
    unsigned long long inclusive_ns;
    unsigned long long child_ns;
    uint32_t caller;
};

struct gp_frame {
    uint32_t va;
    LARGE_INTEGER start;
    unsigned long long child;
};

static struct gp_entry *gp_table;
static SRWLOCK gp_lock = SRWLOCK_INIT;
static __declspec(thread) struct gp_frame gp_stack[GP_STACK_MAX];
static __declspec(thread) int gp_depth;
static int gp_active;
static unsigned long gp_start_flip;
static uint32_t gp_va_filter;
static unsigned long gp_end_flip;
static char gp_file[1024];
static int gp_dumped;
static LARGE_INTEGER gp_frequency;

int recomp_guest_profile_stack(uint32_t *out, int capacity)
{
    int i, count;
    if (!out || capacity <= 0 || !gp_active) return 0;
    count = gp_depth < capacity ? gp_depth : capacity;
    for (i = 0; i < count; ++i) out[i] = gp_stack[gp_depth - count + i].va;
    return count;
}

static unsigned gp_hash(uint32_t va)
{
    return (unsigned)((va * 2654435761u) >> (32 - GP_TABLE_BITS));
}

void recomp_guest_profile_init(void)
{
    const char *value;
    const char *file;
    if (recomp_call_profile_on)
        return;
    value = getenv("RECOMP_GUEST_PROFILE");
    if (!value || !*value || !strcmp(value, "0"))
        return;
    recomp_call_profile_on = 1;
    QueryPerformanceFrequency(&gp_frequency);
    value = getenv("RECOMP_GUEST_PROFILE_START");
    gp_start_flip = value ? strtoul(value, NULL, 0) : 0;
    value = getenv("RECOMP_GUEST_PROFILE_END");
    gp_end_flip = value ? strtoul(value, NULL, 0) : 0;
    value = getenv("RECOMP_GUEST_PROFILE_VA");
    gp_va_filter = value ? (uint32_t)strtoul(value, NULL, 0) : 0;
    file = getenv("RECOMP_GUEST_PROFILE_FILE");
    snprintf(gp_file, sizeof(gp_file), "%s",
             (file && *file) ? file : "guest_profile.csv");
    gp_table = (struct gp_entry *)calloc(GP_TABLE_SIZE, sizeof(struct gp_entry));
    if (!gp_table) {
        recomp_call_profile_on = 0;
        return;
    }
}

void recomp_guest_profile_update_window(unsigned long flip)
{
    if (!recomp_call_profile_on || !gp_table)
        return;
    if (gp_start_flip && flip < gp_start_flip) {
        gp_active = 0;
        return;
    }
    if (gp_end_flip && flip >= gp_end_flip) {
        if (!gp_dumped) {
            FILE *f = fopen(gp_file, "w");
            unsigned i;
            if (f) {
                fprintf(f, "va,name,calls,inclusive_ms,exclusive_ms,caller\n");
                for (i = 0; i < GP_TABLE_SIZE; i++) {
                    const struct gp_entry *e = &gp_table[i];
                    if (!e->va)
                        continue;
                    fprintf(f, "0x%08X,sub_%08X,%llu,%.6f,%.6f,0x%08X\n",
                            e->va, e->va, e->calls,
                            (double)e->inclusive_ns / 1e6,
                            (double)(e->inclusive_ns - e->child_ns) / 1e6,
                            e->caller);
                }
                fclose(f);
            }
            gp_dumped = 1;
        }
        gp_active = 0;
        recomp_call_profile_on = 0;
        return;
    }
    gp_active = 1;
}

/* --- aggregate region counters/timers for one function (sub_00042E40) --- */
int recomp_count_on = 0;
static unsigned long long rcnt[16];
static unsigned long long rtimer_ns[16];
static __declspec(thread) LARGE_INTEGER rtimer_start[16];
static int rcount_active;
static int count_inited;
static unsigned long rcount_start_flip;
static unsigned long rcount_end_flip;
static char rcount_file[1024];
static int rcount_dumped;

void recomp_region_count(unsigned id)
{
    if (recomp_count_on && rcount_active && id < 16)
        rcnt[id]++;
}

void recomp_region_enter(unsigned id)
{
    if (recomp_count_on && rcount_active && id < 16)
        QueryPerformanceCounter(&rtimer_start[id]);
}

void recomp_region_exit(unsigned id)
{
    LARGE_INTEGER now;
    if (recomp_count_on && rcount_active && id < 16) {
        QueryPerformanceCounter(&now);
        rtimer_ns[id] += (unsigned long long)
            ((now.QuadPart - rtimer_start[id].QuadPart) * 1000000000LL
             / gp_frequency.QuadPart);
    }
}

void recomp_count_init(void)
{
    const char *value;
    const char *file;
    if (count_inited)
        return;
    count_inited = 1;
    value = getenv("RECOMP_COUNT_42E40");
    if (!value || !*value || !strcmp(value, "0"))
        return;
    recomp_count_on = 1;
    QueryPerformanceFrequency(&gp_frequency);
    value = getenv("RECOMP_COUNT_42E40_START");
    rcount_start_flip = value ? strtoul(value, NULL, 0) : 0;
    value = getenv("RECOMP_COUNT_42E40_END");
    rcount_end_flip = value ? strtoul(value, NULL, 0) : 0;
    file = getenv("RECOMP_COUNT_42E40_FILE");
    snprintf(rcount_file, sizeof(rcount_file), "%s",
             (file && *file) ? file : "42e40_counts.csv");
}

void recomp_count_update_window(unsigned long flip)
{
    if (!recomp_count_on)
        return;
    if (rcount_start_flip && flip < rcount_start_flip) {
        rcount_active = 0;
        return;
    }
    if (flip >= rcount_end_flip && rcount_end_flip) {
        if (!rcount_dumped) {
            FILE *f = fopen(rcount_file, "w");
            if (f) {
                fprintf(f, "id,name,count,timer_ms\n");
                fprintf(f, "0,function_calls,%llu,%.6f\n",
                        rcnt[0], rtimer_ns[0] / 1e6);
                fprintf(f, "1,loop_iterations,%llu,%.6f\n",
                        rcnt[1], rtimer_ns[1] / 1e6);
                fprintf(f, "2,kernel_executions,%llu,%.6f\n",
                        rcnt[2], rtimer_ns[2] / 1e6);
                fprintf(f, "3,skip_to_43346,%llu,%.6f\n",
                        rcnt[3], rtimer_ns[3] / 1e6);
                fprintf(f, "4,call_12740_site1,%llu,%.6f\n",
                        rcnt[4], rtimer_ns[4] / 1e6);
                fprintf(f, "5,call_12740_site2,%llu,%.6f\n",
                        rcnt[5], rtimer_ns[5] / 1e6);
                fprintf(f, "6,call_122A0,%llu,%.6f\n",
                        rcnt[6], rtimer_ns[6] / 1e6);
                fprintf(f, "7,x87_quaternion_block,%llu,%.6f\n",
                        rcnt[7], rtimer_ns[7] / 1e6);
                fclose(f);
            }
            rcount_dumped = 1;
        }
        recomp_count_on = 0;
        return;
    }
    rcount_active = 1;
}

void recomp_call_profile_enter(uint32_t va)
{
    if (!recomp_call_profile_on || !gp_active || !gp_table)
        return;
    if (gp_va_filter && va != gp_va_filter)
        return;
    if (gp_depth >= GP_STACK_MAX)
        return;
    QueryPerformanceCounter(&gp_stack[gp_depth].start);
    gp_stack[gp_depth].va = va;
    gp_stack[gp_depth].child = 0;
    gp_depth++;
}

void recomp_call_profile_exit(uint32_t va)
{
    LARGE_INTEGER now;
    LONGLONG elapsed;
    unsigned idx, i;
    struct gp_frame *top;
    struct gp_entry *e;
    if (!recomp_call_profile_on || !gp_active || !gp_table || gp_depth == 0)
        return;
    if (gp_va_filter && va != gp_va_filter)
        return;
    QueryPerformanceCounter(&now);
    gp_depth--;
    top = &gp_stack[gp_depth];
    elapsed = now.QuadPart - top->start.QuadPart;
    idx = gp_hash(top->va);
    AcquireSRWLockExclusive(&gp_lock);
    for (i = 0; i < GP_TABLE_SIZE; i++) {
        e = &gp_table[(idx + i) & (GP_TABLE_SIZE - 1)];
        if (!e->va || e->va == top->va) {
            if (!e->va)
                e->va = top->va;
            e->calls++;
            e->inclusive_ns += (unsigned long long)elapsed;
            e->child_ns += top->child;
            if (gp_depth > 0)
                e->caller = gp_stack[gp_depth - 1].va;
            break;
        }
    }
    ReleaseSRWLockExclusive(&gp_lock);
    if (gp_depth > 0)
        gp_stack[gp_depth - 1].child += (unsigned long long)elapsed;
}
