/* Catches guest mapping/TLS mistakes, chroma/block traversal and ret 36 cleanup. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
__declspec(thread) uint32_t g_esp;
ptrdiff_t g_xbox_mem_offset = 0x10000;
extern int noble_yuv_hook(void);
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s\n", #x); return 1; } } while(0)
int main(void) {
    uint8_t *m = VirtualAlloc((void *)0x10000000, 0x10000, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    uint32_t *args;
    CHECK(m != NULL);
    g_esp = (uint32_t)((uintptr_t)m - g_xbox_mem_offset);
    args = (uint32_t *)m;
    args[0] = 0x12345678; args[1] = 1; args[2] = 1;
    args[3] = g_esp + 0x100; args[4] = g_esp + 0x200; args[5] = g_esp + 0x240;
    args[6] = 13; args[7] = 11; args[8] = g_esp + 0x1000; args[9] = 64;
    memset(m + 0x100, 235, 256); memset(m + 0x200, 128, 128);
    memset(m + 0x1000, 0xA5, 1024);
    CHECK(noble_yuv_hook() == 1);
    CHECK(g_esp == (uint32_t)((uintptr_t)m - g_xbox_mem_offset) + 40);
    for (unsigned y=0; y<16; y++) for (unsigned x=0; x<16; x++) {
        uint32_t pixel; memcpy(&pixel,m+0x1000+y*64+x*4,4);
        /* Reference white: floor((235-16)*38139 / 32768) = 254. */
        CHECK(pixel == (y<11 && x<13 ? 0xFFFEFEFEu : 0xA5A5A5A5u));
    }
    {
        /* Four independently specified reference colors, spanning two block
         * rows/columns and cropped edges with a padded destination stride. */
        const uint8_t luma[4]={16,81,145,235};
        const uint8_t chroma_u[4]={128,90,54,128}, chroma_v[4]={128,240,34,128};
        const uint32_t colors[4]={0xFF000000,0xFFFE0000,0xFF00FF00,0xFFFEFEFE};
        g_esp=(uint32_t)((uintptr_t)m-g_xbox_mem_offset);
        args[1]=2; args[2]=2;
        args[3]=g_esp+0x1000; args[4]=g_esp+0x1400; args[5]=g_esp+0x1500;
        args[6]=29; args[7]=27; args[8]=g_esp+0x2000; args[9]=144;
        for (unsigned y=0;y<32;y++) for(unsigned x=0;x<32;x++)
            m[0x1000+y*32+x]=luma[(y/16)*2+x/16];
        for (unsigned y=0;y<16;y++) for(unsigned x=0;x<16;x++) {
            unsigned block=(y/8)*2+x/8;
            m[0x1400+y*16+x]=chroma_u[block];
            m[0x1500+y*16+x]=chroma_v[block];
        }
        memset(m+0x2000,0xA5,144*32);
        CHECK(noble_yuv_hook()==1);
        CHECK(g_esp==(uint32_t)((uintptr_t)m-g_xbox_mem_offset)+40);
        for(unsigned y=0;y<32;y++) for(unsigned x=0;x<36;x++) {
            uint32_t pixel; memcpy(&pixel,m+0x2000+y*144+x*4,4);
            CHECK(pixel==(y<27 && x<29 ? colors[(y/16)*2+x/16] : 0xA5A5A5A5));
        }
    }
    VirtualFree(m,0,MEM_RELEASE);
    puts("XMV YUV: mapped TLS stack, color blocks, crop/stride guards, ret 36 passed");
    return 0;
}

