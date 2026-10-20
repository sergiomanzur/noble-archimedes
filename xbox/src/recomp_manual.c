/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <windows.h>

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern RECOMP_TLS uint32_t g_eax, g_esp, g_ecx, g_edx, g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */

static void func_003E46E2(void) {
    *(uint32_t*)(0x003FF088 + g_xbox_mem_offset) = 0x003FE9AC;
    g_esp += 4;
}

static void func_003E46ED(void) {
    *(uint32_t*)(0x003FF084 + g_xbox_mem_offset) = 0x003FE9B0;
    g_esp += 4;
}

static void func_003E76AD(void) {
    *(uint32_t*)(0x003FF130 + g_xbox_mem_offset) = 0x003FE9B4;
    g_esp += 4;
}

static void func_003E76B8(void) {
    *(uint32_t*)(0x003FF12C + g_xbox_mem_offset) = 0x003FE9B8;
    g_esp += 4;
}

recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
void recomp_icall_fail_log(uint32_t va);

#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
#define MEM8(addr)     (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)    (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)    (*(volatile uint32_t *)XBOX_PTR(addr))

#define PUSH32(sp, val) do {           \
    uint32_t _pv = (uint32_t)(val);    \
    (sp) -= 4;                         \
    MEM32(sp) = _pv;                   \
} while(0)

static void recomp_call(uint32_t target_va, uint32_t ret_va)
{
    PUSH32(g_esp, ret_va);
    recomp_func_t fn = recomp_lookup_manual(target_va);
    if (!fn) fn = recomp_lookup(target_va);
    if (!fn) fn = recomp_lookup_kernel(target_va);
    if (fn) {
        fn();
    } else {
        recomp_icall_fail_log(target_va);
        g_esp += 4;
        g_eax = 0;
    }
}

extern void sub_00429352(void);
extern void sub_0042A4BB(void);
extern void sub_00429100(void);
extern void sub_00428E16(void);
extern void sub_00428B94(void);
extern void sub_00429A37(void);
extern void sub_00428BD3(void);
extern void sub_00428BEA(void);
extern void sub_004298AB(void);
extern void sub_0042BBA0(void);

static void func_004293F0(void)
{
    uint32_t saved_ebx = g_ebx;
    uint32_t saved_esi = g_esi;
    uint32_t edx = 1;
    uint8_t bl = 0;
    uint8_t al = 1;
    while (al < 0x80) {
        uint32_t mask_word = MEM32(g_ecx + (uint32_t)bl * 4 + 8);
        if ((mask_word & edx) == 0) {
            MEM32(g_ecx + (uint32_t)bl * 4 + 8) |= edx;
            break;
        }
        edx <<= 1;
        if (edx == 0) {
            bl++;
            edx = 1;
        }
        al++;
    }
    g_eax = (g_eax & 0xFFFFFF00u) | (al & 0x7Fu);
    g_ebx = saved_ebx;
    g_esi = saved_esi;
    g_esp += 4; /* ret */
}

static void func_004293C5(void)
{
    PUSH32(g_esp, 0x12b598c);
    PUSH32(g_esp, 0xFFFFFFFFu);
    PUSH32(g_esp, 0xFFFFB1E0u);
    PUSH32(g_esp, 0x12b59a8);
    MEM8(0x12b595b) = 0x84;
    MEM8(0x12b59d0) = 3;
    recomp_call(MEM32(0x42ce60), 0x004293ED);
    g_esp += 12; /* ret 8 */
}

static void func_00429846(void)
{
    uint32_t urb = MEM32(g_esp + 4);
    uint32_t dev = MEM32(g_esp + 8);
    uint32_t saved_esi = g_esi;

    PUSH32(g_esp, 0x12b59a8);
    recomp_call(MEM32(0x42ce64), 0x00429851);

    g_eax = urb;
    MEM8(0x12b595b) = 3;
    if ((int32_t)MEM32(g_eax + 4) >= 0 && MEM8(0x12b5959) == 0) {
        MEM32(0x12b5964) = 0x004293C5;
    } else {
        MEM32(0x12b5964) = 0x004297D9;
    }

    g_esi = dev;
    MEM8(0x12b595c) = 0x1c;
    MEM8(0x12b595d) = 0x43;
    uint32_t ep = MEM32(g_esi + 0x0c) + 0x18;
    PUSH32(g_esp, 0x12b595c);
    PUSH32(g_esp, ep);
    PUSH32(g_esp, 0x004298A3);
    sub_0042A4BB();

    MEM32(g_esi + 8) = 0;
    g_esi = saved_esi;
    g_esp += 12; /* ret 8 */
}

static void func_00429A6B(void)
{
    uint32_t urb = MEM32(g_esp + 4);
    uint32_t dev = MEM32(g_esp + 8);
    uint32_t saved_ebx = g_ebx;
    uint32_t saved_esi = g_esi;

    PUSH32(g_esp, 0x12b59a8);
    recomp_call(MEM32(0x42ce64), 0x00429A77);

    g_edx = urb;
    g_ebx = 0;
    MEM8(0x12b595b) = 2;

    if ((int32_t)MEM32(g_edx + 4) < 0 || MEM8(0x12b5959) != 0) {
        goto loc_error;
    }

    if (MEM32(g_edx + 0x14) < 8) {
        MEM32(g_edx + 4) = 0x80000600;
        goto loc_error;
    }

    uint8_t max_packet = MEM8(0x12b59e3);
    if (max_packet > 0x40) {
        MEM32(g_edx + 4) = 0x80000600;
        goto loc_error;
    }

    if (MEM8(0x12b59dd) != 1) {
        MEM32(g_edx + 4) = 0x80000600;
        goto loc_error;
    }

    uint8_t desc_len = MEM8(0x12b59dc);
    if (desc_len != 8 && desc_len != 0x12) {
        MEM32(g_edx + 4) = 0x80000600;
        goto loc_error;
    }

    g_esi = dev;
    g_ecx = MEM32(g_esi + 0x0c);
    MEM8(g_esi + 6) = max_packet;

    PUSH32(g_esp, 0x00429AD9);
    func_004293F0();

    uint8_t addr = (uint8_t)g_eax;
    MEM8(g_esi + 5) = addr;

    MEM32(0x12b5964) = 0x00429846;
    MEM32(0x12b5974) = 0;
    MEM32(0x12b5970) = 0;
    MEM8(0x12b5984) = 0;
    MEM8(0x12b5985) = 5;
    MEM16(0x12b5986) = (uint16_t)addr;
    MEM16(0x12b5988) = 0;
    MEM16(0x12b598a) = 0;

    PUSH32(g_esp, 0x00429B1D);
    sub_00429352();

    uint32_t ep = MEM32(g_esi + 0x0c) + 0x18;
    PUSH32(g_esp, 0x12b595c);
    PUSH32(g_esp, ep);
    PUSH32(g_esp, 0x00429B2E);
    sub_0042A4BB();
    goto loc_done;

loc_error:
    PUSH32(g_esp, dev);
    PUSH32(g_esp, g_edx);
    PUSH32(g_esp, 0x00429B42);
    func_00429846();

loc_done:
    g_ebx = saved_ebx;
    g_esi = saved_esi;
    g_esp += 12; /* ret 8 */
}

static void func_00429D8D(void)
{
    uint32_t urb = MEM32(g_esp + 4);
    uint32_t dev = MEM32(g_esp + 8);

    PUSH32(g_esp, 0x12b59a8);
    recomp_call(MEM32(0x42ce64), 0x00429D9B);

    g_eax = urb;
    g_edx = 0;
    MEM8(0x12b595b) = 5;

    if ((int32_t)MEM32(g_eax + 4) < 0 || MEM8(0x12b5959) != 0) {
        goto loc_d8d_error;
    }

    uint16_t wTotalLength = MEM16(0x12b59e6);
    if (wTotalLength > 0x50) {
        MEM8(0x12b595a) = 0;
        MEM32(g_eax + 4) = 0x80000400;
        goto loc_d8d_error;
    }
    if ((uint32_t)wTotalLength != MEM32(g_eax + 0x14)) {
        MEM32(g_eax + 4) = 0x80000000;
        goto loc_d8d_error;
    }

    uint8_t bConfigurationValue = MEM8(0x12b59e9);
    MEM32(0x12b5964) = 0x00429B46;
    MEM32(0x12b5974) = 0;
    MEM32(0x12b5970) = 0;
    MEM8(0x12b5984) = 0;
    MEM8(0x12b5985) = 9;
    MEM16(0x12b5986) = (uint16_t)bConfigurationValue;
    MEM16(0x12b5988) = 0;
    MEM16(0x12b598a) = 0;

    PUSH32(g_esp, 0x00429E30);
    sub_00429352();

    uint32_t ep = MEM32(dev + 0x0c) + 0x18;
    PUSH32(g_esp, 0x12b595c);
    PUSH32(g_esp, ep);
    PUSH32(g_esp, 0x00429E44);
    sub_0042A4BB();
    goto loc_d8d_ret;

loc_d8d_error:
    PUSH32(g_esp, dev);
    PUSH32(g_esp, g_eax);
    PUSH32(g_esp, 0x00429DD7);
    func_00429846();

loc_d8d_ret:
    g_esp += 12; /* ret 8 */
}

static void func_00429B46(void)
{
    uint32_t urb = MEM32(g_esp + 4);
    uint32_t dev = MEM32(g_esp + 8);
    uint32_t saved_ebx = g_ebx;
    uint32_t saved_esi = g_esi;

    PUSH32(g_esp, 0x12b59a8);
    recomp_call(MEM32(0x42ce64), 0x00429B50);

    g_ecx = urb;
    g_ebx = 0;
    MEM8(0x12b595b) = 6;
    if ((int32_t)MEM32(g_ecx + 4) < 0 || MEM8(0x12b5959) != 0) {
        goto loc_b46_error;
    }

    g_esi = dev;
    MEM32(g_esi + 0x18) = 0;

    uint32_t ptr = 0x12b59e4;
    while (1) {
        uint8_t len = MEM8(ptr);
        ptr += len;
        if (ptr >= 0x12b5a34 || MEM8(ptr) == 0) {
            MEM8(0x12b595a) = 0;
            MEM32(g_ecx + 4) = 0x80000400;
            PUSH32(g_esp, g_esi);
            PUSH32(g_esp, g_ecx);
            PUSH32(g_esp, 0x00429C87);
            func_00429846();
            goto loc_b46_done;
        }
        if (MEM8(ptr + 1) == 4) {
            break;
        }
    }

    MEM32(0x12b5a34) = ptr;
    if (MEM8(0x12b59e8) == 1 || MEM8(0x12b59d3) == 0) {
        MEM8(g_esi) = 3;
    } else {
        MEM8(g_esi) = 4;
        MEM8(g_esi + 2) = 0x80;
        uint8_t num_ifaces = MEM8(0x12b59e8);
        uint8_t d3 = MEM8(0x12b59d3);
        while (g_ebx < num_ifaces && g_ebx < d3) {
            g_ecx = 0x12b5958;
            PUSH32(g_esp, 0x00429BE1);
            sub_00429100();
            uint32_t new_dev = g_eax;
            if (!new_dev) break;
            MEM8(new_dev) = 5;
            MEM8(new_dev + 4) = (MEM8(g_esi + 4) & 0x80) | ((uint8_t)g_ebx + 1);
            MEM8(new_dev + 5) = MEM8(g_esi + 5);
            MEM32(new_dev + 8) = MEM32(g_esi + 8);
            MEM32(new_dev + 0x0c) = MEM32(g_esi + 0x0c);
            MEM8(new_dev + 6) = MEM8(g_esi + 6);
            MEM32(new_dev + 0x18) = MEM32(g_esi + 0x18);
            PUSH32(g_esp, new_dev);
            g_ecx = g_esi;
            PUSH32(g_esp, 0x00429C1D);
            sub_00428E16();
            g_ebx++;
        }
        MEM32(g_esi + 8) = 0;
        g_ecx = g_esi;
        PUSH32(g_esp, 0x00429C34);
        sub_00428B94();
        g_esi = g_eax;
    }

    ptr = MEM32(0x12b5a34);
    MEM8(g_esi + 2) = MEM8(ptr + 2);
    uint32_t iface_param = 0x82 | ((uint32_t)MEM8(ptr + 5) << 8) | ((uint32_t)MEM8(ptr + 6) << 16) | ((uint32_t)MEM8(ptr + 7) << 24);
    PUSH32(g_esp, iface_param);
    g_ecx = g_esi;
    PUSH32(g_esp, 0x00429C6B);
    sub_00429A37();
    goto loc_b46_done;

loc_b46_error:
    PUSH32(g_esp, dev);
    PUSH32(g_esp, g_ecx);
    PUSH32(g_esp, 0x00429C87);
    func_00429846();

loc_b46_done:
    g_ebx = saved_ebx;
    g_esi = saved_esi;
    g_esp += 12; /* ret 8 */
}

static void func_00427C95(void)
{
    g_ecx = MEM32(g_esp + 8);
    PUSH32(g_esp, 0x12b58d0);
    PUSH32(g_esp, 0x00427CA3);
    sub_00428BD3();
    g_esp += 20; /* ret 0x10 */
}

static void func_00428BFB(void)
{
    uint32_t arg1 = MEM32(g_esp + 4);
    MEM8(g_ecx + 7) = (uint8_t)arg1;
    g_esp += 8; /* ret 4 */
}

static void func_00427B62(void)
{
    uint8_t cl = (uint8_t)g_ecx;
    uint32_t edx = g_edx;
    uint8_t bl = 0;
    MEM8(edx) = 0;
    g_eax = 0;
    for (uint32_t p = 0x426aa4; p < 0x426aac; p += 4, bl++) {
        uint32_t drv = MEM32(p);
        if (drv != 0 && MEM8(drv) == cl) {
            MEM8(edx) = bl;
            g_eax = drv;
            break;
        }
    }
    g_esp += 4; /* ret */
}

static void func_004286E5(void)
{
    uint32_t saved_ebx = g_ebx;
    uint32_t saved_esi = g_esi;
    uint32_t saved_edi = g_edi;

    uint32_t urb = MEM32(g_esp + 4);
    uint32_t esi = MEM32(g_esp + 8);

    /* Cancel timer 0x12b5908 */
    PUSH32(g_esp, 0x12b5908);
    recomp_call(MEM32(0x42ce64), 0x004286F3);

    int32_t status = (int32_t)MEM32(urb + 4);
    uint32_t xfer_len = MEM32(urb + 0x14);
    uint8_t desc_len = MEM8(0x12b58bc);
    uint8_t desc_type = MEM8(0x12b58bd);
    uint16_t bcd_xid = MEM16(0x12b58be);

    if (status < 0 || xfer_len < 8 || desc_len < 8 || desc_type != 0x42 || bcd_xid == 0) {
        fprintf(stderr, "  [USB] 0x004286E5: descriptor validation error (st=0x%08X len=%u bLength=%u type=0x%02X bcd=0x%04X)\n",
                status, xfer_len, desc_len, desc_type, bcd_xid);
        goto loc_86e5_exit;
    }

    uint8_t cl = MEM8(0x12b58c0);
    uint32_t edi = esi + 0xa;

    uint8_t bl = 0;
    uint32_t driver = 0;
    MEM8(edi) = 0;
    for (uint32_t p = 0x426aa4; p < 0x426aac; p += 4, bl++) {
        uint32_t drv = MEM32(p);
        if (drv != 0 && MEM8(drv) == cl) {
            MEM8(edi) = bl;
            driver = drv;
            break;
        }
    }

    MEM32(esi + 0xe) = driver;
    MEM8(esi + 0xb) = MEM8(0x12b58c1);
    MEM8(esi + 0xc) = MEM8(0x12b58c2);
    MEM8(esi + 0xd) = MEM8(0x12b58c3);

    if (!driver) {
        fprintf(stderr, "  [USB] 0x004286E5: no class driver registered for type %u\n", cl);
        uint32_t dev = MEM32(esi);
        MEM8(esi + 4) &= 0xfe;
        MEM32(esi) = 0;
        MEM16(0x12b58b2)--;
        g_ecx = dev;
        PUSH32(g_esp, 0);
        PUSH32(g_esp, 0x004287BB);
        sub_00428BEA();
        g_ecx = dev;
        PUSH32(g_esp, 0x80000400);
        PUSH32(g_esp, 0x004287C7);
        sub_004298AB();
        goto loc_86e5_exit;
    }

    uint8_t in_size = MEM8(0x12b58c2);
    if (in_size < 2 || in_size > 0x20 || in_size > MEM8(esi + 6)) {
        fprintf(stderr, "  [USB] 0x004286E5: invalid in_size %u (ep max %u)\n", in_size, MEM8(esi + 6));
        goto loc_86e5_exit;
    }
    if (MEM8(esi + 9) != 0 && MEM8(esi + 0xd) > MEM8(esi + 7)) {
        fprintf(stderr, "  [USB] 0x004286E5: invalid out_size %u (ep max %u)\n", MEM8(esi + 0xd), MEM8(esi + 7));
        goto loc_86e5_exit;
    }

    uint32_t dev = MEM32(esi);
    uint8_t drv_idx = MEM8(edi);

    /* Set driver index: dev[7] = drv_idx */
    MEM8(dev + 7) = drv_idx;

    /* sub_004298AB(ecx = dev, arg = 0) */
    g_ecx = dev;
    PUSH32(g_esp, 0);
    PUSH32(g_esp, 0x0042879E);
    sub_004298AB();

    /* Mark interface active */
    MEM8(esi + 4) |= 8;
    fprintf(stderr, "  [USB] 0x004286E5: gamepad device activated successfully! (dev=0x%08X drv=0x%08X drv_idx=%u)\n",
            dev, driver, drv_idx);

loc_86e5_exit:
    g_ebx = saved_ebx;
    g_esi = saved_esi;
    g_edi = saved_edi;
    g_esp += 12; /* ret 8 */
}

static void func_0042BBA0(void)
{
    if (g_edx < 0x80000000 || g_edx >= 0x84000000) {
        fprintf(stderr, "  [USB] 0x0042BBA0: edx=0x%08X out of range, aborting error handler\n", g_edx);
        g_esp += 4; /* ret */
        return;
    }
    uint32_t edi = MEM32(g_edx + 0x14);
    if (edi < 0x80000000 || edi >= 0x84000000) {
        fprintf(stderr, "  [USB] 0x0042BBA0: edi=0x%08X out of range, aborting error handler\n", edi);
        g_esp += 4; /* ret */
        return;
    }
    uint32_t head = MEM32(edi + 8) & 0xFFFFFFF0u;
    if (head == 0 || head == 0xFFFFFFF0u) {
        fprintf(stderr, "  [USB] 0x0042BBA0: edi HeadP=0x%08X empty/halted, aborting error handler\n", MEM32(edi + 8));
        g_esp += 4; /* ret */
        return;
    }
    sub_0042BBA0();
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x003E46E2u) return func_003E46E2;
    if (xbox_va == 0x003E46EDu) return func_003E46ED;
    if (xbox_va == 0x003E76ADu) return func_003E76AD;
    if (xbox_va == 0x003E76B8u) return func_003E76B8;

    if (xbox_va == 0x004293F0u) return func_004293F0;
    if (xbox_va == 0x004293C5u) return func_004293C5;
    if (xbox_va == 0x00429846u) return func_00429846;
    if (xbox_va == 0x00429A6Bu) return func_00429A6B;
    if (xbox_va == 0x00429D8Du) return func_00429D8D;
    if (xbox_va == 0x00429B46u) return func_00429B46;
    if (xbox_va == 0x00427C95u) return func_00427C95;
    if (xbox_va == 0x004286E5u) return func_004286E5;
    if (xbox_va == 0x00428BFBu) return func_00428BFB;
    if (xbox_va == 0x00427B62u) return func_00427B62;
    if (xbox_va == 0x0042BBA0u) return func_0042BBA0;

    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 */
void recomp_icall_fail_log(uint32_t va)
{
    uint32_t ret_va = 0;
    if (g_esp >= 0x00010000 && g_esp < 0x04000000) {
        ret_va = *(uint32_t*)(g_esp + g_xbox_mem_offset);
    }
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu) ret=0x%08X esp=0x%08X ebx=0x%08X ecx=0x%08X\n",
            va, (unsigned long long)g_icall_count, ret_va, g_esp, g_ebx, g_ecx);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    uint32_t ret_va = 0;
    if (g_esp >= 0x00010000 && g_esp < 0x04000000) {
        ret_va = *(uint32_t*)(g_esp + g_xbox_mem_offset);
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu) ret=0x%08X esp=0x%08X ebx=0x%08X ecx=0x%08X\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count, ret_va, g_esp, g_ebx, g_ecx);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    static int stop=-1;
    if(stop<0) {
        const char *trap = getenv("RECOMP_UNIMPL_TRAP");
        stop = trap && *trap && *trap != '0';
    }

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── Frame Rate Pacing (60 FPS gameplay, 30 FPS cutscenes) ──────────────── */

/* Old generated tail probes may call this more than once per frame.
 * The GPU flip callback recomp_present_frame now owns the only frame wait. */
void recomp_pace_frame(void) {}
