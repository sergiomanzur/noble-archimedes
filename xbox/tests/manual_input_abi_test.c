/* Standalone native-hook regression; no game executable or devices needed.
 * gcc -std=c11 -O0 xbox/tests/manual_input_abi_test.c -o output/manual_input_abi_test.exe
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <windows.h>
#include <stdlib.h>
static unsigned environment_reads;
static char *count_environment_read(const char *name) {
    (void)name;environment_reads++;return NULL;
}
#define getenv count_environment_read

#include "../src/recomp_manual.c"
#undef getenv

RECOMP_TLS uint32_t g_eax, g_esp, g_ecx, g_edx, g_ebx, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
volatile uint32_t g_icall_trace[16], g_icall_trace_idx;
volatile uint64_t g_icall_count;

/* Generated callees are outside this native-hook test. Keep their guest
 * return-stack effects, so a bad hook cleanup remains observable. */
void sub_00429352(void) { g_esp += 4; }
void sub_0042A4BB(void) { g_esp += 12; }
void sub_00429100(void) { g_esp += 4; g_eax = 0; }
void sub_00428E16(void) { g_esp += 8; }
void sub_00428B94(void) { g_esp += 4; }
void sub_00429A37(void) { g_esp += 8; }
void sub_00428BD3(void) { g_esp += 8; }
void sub_00428BEA(void) { g_esp += 8; }
void sub_004298AB(void) { g_esp += 8; }
void sub_0042BBA0(void) { g_esp += 4; }
static void cancel_timer(void) { g_esp += 8; }
recomp_func_t recomp_lookup(uint32_t va)
{
    return va == 0x00F00001u ? cancel_timer : NULL;
}
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return NULL; }

static int check(int condition, const char *message)
{
    if (!condition) fprintf(stderr, "FAIL: %s (offset=0x%llX)\n", message,
                            (unsigned long long)g_xbox_mem_offset);
    return condition ? 0 : 1;
}

static int test_stack_argument(void)
{
    const uint32_t sp = 0x01000000, device = 0x01100000;
    g_esp = sp;
    g_ecx = device;
    /* A valid raw-address decoy makes the old bug a deterministic wrong
     * result instead of an access violation. */
    *(uint32_t *)(uintptr_t)(sp + 4) = 0x11;
    MEM32(sp + 4) = 0x7C;
    MEM8(device + 7) = 0;
    recomp_lookup_manual(0x00428BFB)();
    return check(MEM8(device + 7) == 0x7C, "USB driver index uses translated stack argument")
         + check(g_esp == sp + 8, "USB driver index returns with ret 4 cleanup");
}

static int test_xid_activation(void)
{
    const uint32_t sp = 0x01000000, urb = 0x01110000, iface = 0x01120000;
    const uint32_t device = 0x01130000, driver = 0x01140000, bad_urb = 0x01150000;
    g_esp = sp;
    g_ebx = 0xABCD1234; g_esi = 0xBCDE2345; g_edi = 0xCDEF3456;
    *(uint32_t *)(uintptr_t)(sp + 4) = bad_urb;
    *(uint32_t *)(uintptr_t)(sp + 8) = iface;
    MEM32(bad_urb + 4) = 0x80000000;
    MEM32(sp + 4) = urb; MEM32(sp + 8) = iface;
    MEM32(urb + 4) = 0; MEM32(urb + 0x14) = 16;
    MEM32(0x42CE64) = 0x00F00001;
    MEM8(0x12B58BC) = 16; MEM8(0x12B58BD) = 0x42;
    MEM16(0x12B58BE) = 0x0100;
    MEM8(0x12B58C0) = 1; MEM8(0x12B58C1) = 0;
    MEM8(0x12B58C2) = 20; MEM8(0x12B58C3) = 6;
    MEM32(0x426AA4) = driver; MEM32(0x426AA8) = 0;
    MEM8(driver) = 1;
    MEM32(iface) = device; MEM8(iface + 4) = 0;
    MEM8(iface + 6) = 32; MEM8(iface + 7) = 8; MEM8(iface + 9) = 1;
    recomp_lookup_manual(0x004286E5)();
    return check((MEM8(iface + 4) & 8) != 0, "valid XID descriptor activates the interface")
         + check(MEM32(iface + 0xE) == driver, "XID interface receives class driver")
         + check(g_esp == sp + 12, "XID callback returns with ret 8 cleanup")
         + check(g_ebx == 0xABCD1234 && g_esi == 0xBCDE2345 && g_edi == 0xCDEF3456,
                 "XID callback preserves nonvolatile registers");
}

int main(void)
{
    const size_t size = 0x02000000;
    void *memory = VirtualAlloc((void *)(uintptr_t)0x10000, size,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    int failures = 0;
    if (memory != (void *)(uintptr_t)0x10000) {
        fprintf(stderr, "Cannot reserve regression guest mapping: %lu\n", GetLastError());
        return 2;
    }
    /* Nonzero relocation reproduces the host configuration that lost input.
     * Zero relocation checks compatibility with identity-mapped hosts. */
    for (int mode = 0; mode < 2; mode++) {
        memset(memory, 0, size);
        g_xbox_mem_offset = mode == 0 ? 0x10000 : 0;
        failures += test_stack_argument();
        failures += test_xid_activation();
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    environment_reads=0;
    for(int i=0;i<1000;i++) recomp_unimpl("cli",0);
    failures+=check(environment_reads==1,"no repeated environment locking for nontrapping instructions");
    if (failures) return 1;
    puts("manual input ABI: translated stack and XID activation passed");
    return 0;
}
