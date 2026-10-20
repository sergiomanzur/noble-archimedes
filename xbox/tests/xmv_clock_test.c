/* A progressive field must carry the live 60 Hz clock, even without guest GPU flips. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
__declspec(thread) uint32_t g_esp, g_eax, g_ecx, g_edx;
__declspec(thread) uint32_t g_ebx,g_esi,g_edi,g_ebp,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
static uint32_t ticks;
uint32_t xbox_VblankCount(void) { return ticks; }
/* Rendering is disabled in this clock-only fixture. */
void sub_003BB4C0(void) {}
void sub_003B86E0(void) {}
void sub_003B8AE0(void) {}
void sub_003C3D10(void) {}
void xbox_FramebufferWindowSet(uint32_t a,uint32_t b) { (void)a;(void)b; }
void xbox_FramebufferWindowPresent(uint32_t a,uint32_t b) { (void)a;(void)b; }
void xbox_FramebufferWindowSetAA(uint32_t a) { (void)a; }
void xbox_FramebufferWindowStart(void) {}
extern int noble_field_status_hook(void);
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failed: %s\n",#x); return 1; } } while(0)
int main(void) {
    uint8_t *m=VirtualAlloc((void *)0x10000,0x500000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(m!=NULL);
    *(uint32_t *)0x003C6F98=0x20000; /* Guest device's vblank counter stays zero. */
    for (ticks=60;ticks<=120;ticks+=60) {
        g_esp=0x10000; *(uint32_t *)0x10004=0x10100;
        CHECK(noble_field_status_hook()==1);
        CHECK(*(uint32_t *)0x10100==3);
        CHECK(*(uint32_t *)0x10104==ticks);
        CHECK(g_esp==0x10008);
    }
    VirtualFree(m,0,MEM_RELEASE);
    puts("XMV clock: progressive fields advance with host vblanks");
    return 0;
}

