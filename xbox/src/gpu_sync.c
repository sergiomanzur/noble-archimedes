/* D3D's GPU fence wait, bridged to the executor's actual semaphore writes. */
#include <windows.h>
#include <stdlib.h>
#include <recomp/gen/recomp_types.h>

extern int nv2a_pb_dma_walk(void);
extern void sub_003BCB00(void);
extern void sub_003BC9E0(void);

int xbox_RecompBlockOnGpuFence(void)
{
    uint32_t device, semaphore, target, saved_sp;
    uint32_t saved_bx, saved_si, saved_di, saved_bp, saved_seh;

    if (!nv2a_pb_dma_walk())
        return 0;
    /* Device-level D3D8 HLE owns the GL draw and never submits an NV2A
     * semaphore release.  The guest fence therefore has no producer and must
     * be considered complete; waiting here is the first-3D-transition stall. */
    if (getenv("RECOMP_D3D8GL")) {
        g_esp += 12;                 /* guest RET 8 */
        return 1;
    }
    device = MEM32(0x3C6F98);
    if (!device || !(semaphore = MEM32(device + 0x30)))
        return 0;

    saved_sp = g_esp;
    target = MEM32(saved_sp + 4);
    if ((int32_t)(*(volatile uint32_t *)&MEM32(semaphore) - target) >= 0) {
        g_esp = saved_sp + 12;
        return 1;
    }
    saved_bx = g_ebx; saved_si = g_esi; saved_di = g_edi;
    saved_bp = g_ebp; saved_seh = g_seh_ebp;

    /* If this is the current unissued fence, append its genuine release using
     * D3D's existing packet writer. Kick pending commands before waiting. */
    if (target == MEM32(device + 0x2C)) {
        PUSH32(g_esp, 0);
        PUSH32(g_esp, 0x003BCBEFu);
        sub_003BCB00();
    }
    g_ecx = device;
    PUSH32(g_esp, 0x003BCBB1u);
    sub_003BC9E0();

    /* D3D compares these sequence numbers modulo 2^32. Only the executor's
     * BACK_END_WRITE_SEMAPHORE_RELEASE moves this word forward. */
    while ((int32_t)(*(volatile uint32_t *)&MEM32(semaphore) - target) < 0)
        Sleep(1);

    g_ebx = saved_bx; g_esi = saved_si; g_edi = saved_di;
    g_ebp = saved_bp; g_seh_ebp = saved_seh;
    g_esp = saved_sp + 12;                 /* guest RET 8 */
    return 1;
}
