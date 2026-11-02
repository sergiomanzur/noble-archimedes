/*
 * Pushbuffer DMA-walk contract regression (production nv2a_pb_scan.c).
 *
 * This compiles the real scanner with a synthetic channel, a synthetic
 * contiguous-memory view and a recording nv2a_pb_exec_method, then drives
 * nv2a_pb_scan() through the cases the historical `[PB] bad target` blocks
 * distinguish. It is a parser-semantics test, not a surface test: it asserts
 * the exact ordered (subchannel, method, parameter) stream the walk executes.
 *
 * Domain model
 * ------------
 * A pushbuffer address is legal only if it lies in a buffer the channel
 * configured as a pushbuffer: the ring object plus the prebuilt pushbuffer
 * objects the title calls into. That domain is configuration (object base and
 * limit), not the minimum/maximum of observed PUT values and not a guess from
 * where the walk happens to be. The production scanner currently takes only
 * PUT; the constants below are the channel configuration this contract test
 * requires a fix to consume. They are deliberately not derived from the stream.
 *
 * Expected results on the current production scanner
 * ---------------------------------------------------
 *   PASS  legal ring wrap: GET (walk position) > PUT, wrap JUMP, ordered tail
 *   PASS  valid CALL into a configured prebuilt pushbuffer + RETURN
 *   PASS  method split across two PUTs (PUT arrives mid-parameters)
 *   PASS  restart: the title moves external DMA_GET; the walk resyncs there
 *   FAIL  JUMP into memory that is not a configured pushbuffer: the walk
 *         follows it and executes that memory as commands (the historical
 *         desync class). This is the failing case that motivates the bounded
 *         domain check; it must not be "fixed" by treating GET > PUT as a
 *         restart (the legal-wrap case would then lose the tail commands).
 *
 * No surface validation, no clamping, no production edit, no logging added to
 * the runtime; the scanner is unmodified.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void nv2a_pb_scan(uint32_t put_phys);
extern void nv2a_pb_resync(uint32_t get_phys);

/* ------------------------------------------------------------------ memory */

#define MEM_SIZE 0x40000u

static uint8_t *g_mem;                 /* host backing for the synthetic framebuffer */
static volatile uint32_t g_dma_get;    /* NV2A USER DMA_GET as the title sees it */

/* The scanner reads guest_va 0x80000000|phys as host g_mem + phys. */
ptrdiff_t xbox_GetMemoryOffset(void)
{
    return (ptrdiff_t)((uintptr_t)g_mem - (uintptr_t)0x80000000u);
}

volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    (void)offset;                      /* the walk only asks for NV2A_USER_DMA_GET */
    return &g_dma_get;
}

void xbox_Nv2aAckBusyBits(void) { }

/* -------------------------------------------------------------- recording */

struct event { uint32_t subch, method, param; };
static struct event g_events[128];
static unsigned g_events_count;

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    if (g_events_count < sizeof(g_events) / sizeof(g_events[0])) {
        g_events[g_events_count].subch = subch;
        g_events[g_events_count].method = method;
        g_events[g_events_count].param = param;
        g_events_count++;
    }
}

void nv2a_pb_exec_report(void) { }

/* --------------------------------------------------------- channel domain */

struct dma_object { uint32_t base, size; const char *name; };
struct dma_context {
    struct dma_object ring;
    struct dma_object prebuilt[2];
    unsigned prebuilt_count;
};

/* The channel's configured pushbuffer domain for every case below. */
#define RING_BASE 0x10000u
#define RING_SIZE 0x4000u
#define PREBUILT_BASE 0x20000u
#define PREBUILT_SIZE 0x1000u
#define OUTSIDE_BASE 0x30000u

static const struct dma_context g_domain = {
    { RING_BASE, RING_SIZE, "ring" },
    { { PREBUILT_BASE, PREBUILT_SIZE, "prebuilt" }, { 0, 0, NULL } },
    1
};

static int in_domain(uint32_t address)
{
    unsigned i;

    if (address >= g_domain.ring.base &&
        address < g_domain.ring.base + g_domain.ring.size)
        return 1;
    for (i = 0; i < g_domain.prebuilt_count; i++)
        if (address >= g_domain.prebuilt[i].base &&
            address < g_domain.prebuilt[i].base + g_domain.prebuilt[i].size)
            return 1;
    return 0;
}

/* ------------------------------------------------------------ stream words */

static void put32(uint32_t phys, uint32_t value)
{
    memcpy(g_mem + phys, &value, sizeof(value));
}

/* count parameters, subchannel, first method: the scanner's header decode. */
static uint32_t method_header(uint32_t count, uint32_t subch, uint32_t method)
{
    return (count << 18) | ((subch & 7u) << 13) | (method & 0x1FFCu);
}

static uint32_t jump_word(uint32_t target)
{
    return (target & 0xFFFFFFFCu) | 1u;
}

static uint32_t call_word(uint32_t target)
{
    return (target & 0xFFFFFFFCu) | 2u;
}

#define RETURN_WORD 0x00020000u

/* --------------------------------------------------------------- assertions */

struct expected { uint32_t subch, method, param; };

static int check_stream(const char *name, const struct expected *want,
                        unsigned want_count, const char *why)
{
    unsigned i;
    int ok = g_events_count == want_count;

    for (i = 0; ok && i < want_count; i++)
        ok = g_events[i].subch == want[i].subch &&
             g_events[i].method == want[i].method &&
             g_events[i].param == want[i].param;

    printf("[%s] %s (%u method%s)%s%s\n", ok ? "PASS" : "FAIL", name,
           g_events_count, g_events_count == 1 ? "" : "s",
           why && *why ? ": " : "", why ? why : "");
    if (!ok) {
        printf("       expected %u:", want_count);
        for (i = 0; i < want_count; i++)
            printf(" %u:%03X=%08X", want[i].subch, want[i].method, want[i].param);
        printf("\n       actual   %u:", g_events_count);
        for (i = 0; i < g_events_count; i++)
            printf(" %u:%03X=%08X", g_events[i].subch, g_events[i].method,
                   g_events[i].param);
        printf("\n");
    }
    return ok ? 0 : 1;
}

/* Each case starts from a channel the title has configured: walk position =
 * the configured GET, the scanner's published GET, no pending method. */
static void reset_case(uint32_t walk_position)
{
    memset(g_mem, 0, MEM_SIZE);
    g_events_count = 0;
    g_dma_get = walk_position;
    nv2a_pb_resync(walk_position);
}

/* ------------------------------------------------------------------- cases */

/* A full ring: the walk starts near the end of the previous lap, the title
 * wrote the ring's wrap JUMP and the next lap's commands, and PUT is in the
 * new lap. GET (0x13F00) > PUT (0x10014) is LEGAL here: external GET did not
 * move; the JUMP carries the walk across the wrap. A fix that resyncs on
 * GET > PUT would drop H1 and the JUMP and fail this case. */
static int case_legal_wrap(void)
{
    static const struct expected want[] = {
        { 0, 0x100, 0x1111 }, { 0, 0x104, 0x2222 },
        { 0, 0x108, 0x3333 }, { 0, 0x10C, 0x4444 }
    };
    const uint32_t walk = 0x13F00u, put = 0x10014u;

    reset_case(walk);
    put32(0x13F00u, method_header(1, 0, 0x100)); put32(0x13F04u, 0x1111u);
    put32(0x13F08u, jump_word(RING_BASE));
    put32(0x10000u, method_header(1, 0, 0x104)); put32(0x10004u, 0x2222u);
    put32(0x10008u, method_header(2, 0, 0x108));
    put32(0x1000Cu, 0x3333u); put32(0x10010u, 0x4444u);

    printf("  ring %08X..%08X, walk %08X > PUT %08X, JUMP at %08X\n",
           RING_BASE, RING_BASE + RING_SIZE, walk, put, 0x13F08u);
    nv2a_pb_scan(put);
    return check_stream("legal ring wrap (GET > PUT, wrap JUMP)",
                        want, sizeof(want) / sizeof(want[0]),
                        "ordered tail through the wrap");
}

/* A CALL into a separate configured pushbuffer object, then RETURN to the
 * instruction after the CALL. The target is in the domain because the channel
 * configured it, not because it is near the ring or below GET/PUT. */
static int case_call_return(void)
{
    static const struct expected want[] = {
        { 0, 0x104, 0xAAAA }, { 0, 0x104, 0xBBBB }, { 0, 0x10C, 0xCCCC }
    };
    const uint32_t walk = 0x10000u, put = 0x10014u;

    reset_case(walk);
    put32(0x10000u, method_header(1, 0, 0x104)); put32(0x10004u, 0xAAAAu);
    put32(0x10008u, call_word(PREBUILT_BASE));
    put32(0x1000Cu, method_header(1, 0, 0x10C)); put32(0x10010u, 0xCCCCu);
    put32(PREBUILT_BASE, method_header(1, 0, 0x104));
    put32(PREBUILT_BASE + 4u, 0xBBBBu);
    put32(PREBUILT_BASE + 8u, RETURN_WORD);

    printf("  call target %08X in configured prebuilt object [%08X,%08X)\n",
           PREBUILT_BASE, PREBUILT_BASE, PREBUILT_BASE + PREBUILT_SIZE);
    nv2a_pb_scan(put);
    return check_stream("valid CALL/RETURN into prebuilt pushbuffer",
                        want, sizeof(want) / sizeof(want[0]),
                        "call target is configured, not inferred");
}

/* The title's method header and one parameter are written, PUT advances to the
 * middle of the method, then the remaining parameters are written and PUT
 * advances again. The second scan must continue the same method, not decode
 * the parameters as headers. */
static int case_split_method(void)
{
    static const struct expected want[] = {
        { 0, 0x108, 0x1000 }, { 0, 0x10C, 0x1004 }, { 0, 0x110, 0x1008 }
    };
    const uint32_t walk = 0x10000u;

    reset_case(walk);
    put32(0x10000u, method_header(3, 0, 0x108));
    put32(0x10004u, 0x1000u);
    nv2a_pb_scan(0x10008u);            /* PUT inside the method */
    printf("  scan 1: PUT %08X, %u method(s)\n", 0x10008u, g_events_count);

    put32(0x10008u, 0x1004u);
    put32(0x1000Cu, 0x1008u);
    nv2a_pb_scan(0x10010u);            /* PUT after the last parameter */

    return check_stream("PUT split method", want,
                        sizeof(want) / sizeof(want[0]),
                        "parameters resume the pending method");
}

/* The title restarts the ring: it writes a new command stream elsewhere in the
 * ring and moves DMA_GET itself. The walk must resync to that external GET and
 * execute the new stream, not continue from the old position. */
static int case_restart(void)
{
    static const struct expected want[] = {
        { 0, 0x104, 0xAAAA }, { 0, 0x108, 0xBBBB }
    };
    const uint32_t walk = 0x10000u, restart_get = 0x12000u;

    reset_case(walk);
    put32(0x10000u, method_header(1, 0, 0x104)); put32(0x10004u, 0xAAAAu);
    nv2a_pb_scan(0x10008u);
    printf("  scan 1: %u method(s), published GET %08X\n",
           g_events_count, (unsigned)g_dma_get);

    /* The title moves GET; this is the external observation, not a PUT shape. */
    put32(restart_get, method_header(1, 0, 0x108));
    put32(restart_get + 4u, 0xBBBBu);
    g_dma_get = restart_get;
    nv2a_pb_scan(restart_get + 8u);

    return check_stream("restart observed through external GET", want,
                        sizeof(want) / sizeof(want[0]),
                        "resynced to title's GET");
}

/* A JUMP into memory that is not a configured pushbuffer. The contract is that
 * the walk must not execute it; the current scanner only rejects targets at or
 * above 64 MB, so it follows this one. This is the failing case. */
static int case_out_of_domain(void)
{
    static const struct expected want[] = {
        { 0, 0x104, 0x1111 }
    };
    const uint32_t walk = 0x10000u, put = 0x10014u;

    reset_case(walk);
    put32(0x10000u, method_header(1, 0, 0x104)); put32(0x10004u, 0x1111u);
    put32(0x10008u, jump_word(OUTSIDE_BASE));
    put32(0x1000Cu, method_header(1, 0, 0x10Cu)); put32(0x10010u, 0x2222u);
    put32(OUTSIDE_BASE, method_header(1, 0, 0x110u));
    put32(OUTSIDE_BASE + 4u, 0xDEADu);
    put32(OUTSIDE_BASE + 8u, jump_word(0x1000Cu));

    printf("  jump target %08X in_domain=%d (ring + prebuilt only)\n",
           OUTSIDE_BASE, in_domain(OUTSIDE_BASE));
    nv2a_pb_scan(put);
    return check_stream("out-of-domain JUMP target", want,
                        sizeof(want) / sizeof(want[0]),
                        "must resync, not execute the target");
}

int main(void)
{
    int failures = 0;

    /* The scanner's executor (and thus its GET observation) is opt-in. */
    _putenv("RECOMP_PB_EXEC=1");

    g_mem = (uint8_t *)calloc(1, MEM_SIZE);
    if (!g_mem) {
        fprintf(stderr, "no memory for the synthetic channel\n");
        return 2;
    }

    printf("pb scan contract: domain ring [%08X,%08X) + prebuilt [%08X,%08X)\n",
           RING_BASE, RING_BASE + RING_SIZE, PREBUILT_BASE,
           PREBUILT_BASE + PREBUILT_SIZE);
    printf("expected on current production: 4 PASS, 1 FAIL (out-of-domain)\n");

    failures += case_legal_wrap();
    failures += case_call_return();
    failures += case_split_method();
    failures += case_restart();
    failures += case_out_of_domain();

    free(g_mem);
    printf("%d contract case(s) failed\n", failures);
    return failures ? 1 : 0;
}
