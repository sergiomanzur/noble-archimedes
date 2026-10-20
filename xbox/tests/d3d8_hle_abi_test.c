/* Exercise the real bridge at a nonzero guest mapping offset, without a GPU. */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
__declspec(thread) uint32_t g_esp, g_eax, g_ecx, g_edx;
__declspec(thread) uint32_t g_ebx,g_esi,g_edi,g_ebp,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
uint32_t xbox_VblankCount(void) { return 0; }
static unsigned published_va, published_pitch, published_count;
void xbox_FramebufferWindowSet(uint32_t a,uint32_t b) { (void)a;(void)b; }
void xbox_FramebufferWindowPresent(uint32_t a,uint32_t b) { published_va=a;published_pitch=b;published_count++; }
void xbox_FramebufferWindowSetAA(uint32_t a) { (void)a; }
void xbox_FramebufferWindowStart(void) {}
static unsigned init_calls, present_calls, target_calls, draw_calls, constant_calls;
static unsigned draw_args[5], target_args[2], constant_reg;
static unsigned constant_data, constant_count;
static int init_ok = 1;
static int readback_ok;
static int copy_ok=1;
static unsigned copy_args[5];
static unsigned bound_shader;
static int fake_init(void) { init_calls++; return init_ok; }
static void fake_offset(ptrdiff_t offset) { (void)offset; }
static void fake_present(void) { present_calls++; }
static void fake_target(unsigned a, unsigned b) { target_calls++; target_args[0]=a; target_args[1]=b; }
static void fake_draw(unsigned a,unsigned b,unsigned c) { draw_calls++; draw_args[0]=a; draw_args[1]=b; draw_args[2]=c; }
static void fake_up(unsigned a,unsigned b,unsigned c,unsigned d) { fake_draw(a,b,c); draw_args[3]=d; }
static void fake_indexed_up(unsigned a,unsigned b,unsigned c,unsigned d,unsigned e) { fake_up(a,b,c,d); draw_args[4]=e; }
static void fake_constants(int reg,unsigned data,unsigned count) { constant_calls++; constant_reg=(unsigned)reg; constant_data=data;constant_count=count; draw_args[0]=data; draw_args[1]=count; }
static unsigned fake_create(unsigned d,unsigned f,unsigned u) { (void)d;(void)f;(void)u;return 0x90001000; }
static void fake_shader(unsigned handle) { bound_shader=handle; }
static int fake_readback(unsigned surface) { (void)surface;return readback_ok; }
static void fake_noop(void) {}
static int fake_fixed(unsigned data,unsigned words,unsigned modes) {
    (void)data;(void)words;(void)modes;return 1;
}
static int fake_copy(unsigned a,unsigned b,unsigned c,unsigned d,unsigned e) {
    copy_args[0]=a;copy_args[1]=b;copy_args[2]=c;copy_args[3]=d;copy_args[4]=e;
    return copy_ok;
}
static FARPROC WINAPI fake_proc(HMODULE module,LPCSTR name) {
    (void)module;
#define EXPORT(n,f) if (!strcmp(name,"noble_d3d8_" n)) return (FARPROC)(f)
    EXPORT("init",fake_init); EXPORT("set_guest_offset",fake_offset);
    EXPORT("present",fake_present); EXPORT("render_target",fake_target);
    EXPORT("draw",fake_draw); EXPORT("draw_indexed",fake_draw);
    EXPORT("stream",fake_draw); EXPORT("indices",fake_target);
    EXPORT("draw_up",fake_up); EXPORT("draw_indexed_up",fake_indexed_up);
    EXPORT("vertex_constants",fake_constants); EXPORT("create_vertex_shader",fake_create);
    EXPORT("vertex_shader",fake_shader); EXPORT("readback",fake_readback);
    EXPORT("fixed_function",fake_fixed);
    EXPORT("copy_rects",fake_copy);
    return (FARPROC)fake_noop;
#undef EXPORT
}
static HMODULE WINAPI fake_load(LPCSTR path) { (void)path; return (HMODULE)1; }
#define GetProcAddress fake_proc
#define LoadLibraryA fake_load
#include "../src/d3d8_hle.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static uint32_t ram[0x500000/4];
static void setup(unsigned a,unsigned b,unsigned c,unsigned d,unsigned e) {
    g_esp=0x10000; MEM32(g_esp)=0x12345678;
    MEM32(g_esp+4)=a;MEM32(g_esp+8)=b;MEM32(g_esp+12)=c;
    MEM32(g_esp+16)=d;MEM32(g_esp+20)=e;
}
/* The real guest implementation must still create guest resources/handles. */
void sub_003BB4C0(void) { MEM32(0x3C6F98)=0x20000; g_eax=0;g_esp+=28; }
void sub_003B86E0(void) { MEM32(MEM32(g_esp+12))=0x30001;g_eax=0;g_esp+=20; }
void sub_003B8AE0(void) { MEM32(0x3C5EE0)=0x12345678;g_esp+=12; }
void sub_003C3D10(void) {
    uint32_t dev=MEM32(g_esp+4),packet=MEM32(dev);
    MEM32(packet)=0x41E60;MEM32(packet+4)=1;MEM32(dev)=packet+8;
    g_esp+=12;
}
int main(void) {
    g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)ram;
    _putenv_s("RECOMP_D3D8GL","1");
    setup(0x30000,0x40000,0,0,0);
    CHECK(noble_d3d8_guest_call(0x003B6FB0,12)==0);
    CHECK(present_calls==0 && target_calls==0); /* Before device creation. */
    setup(0,0,0,0,0);
    CHECK(noble_d3d8_guest_call(0x003BB4C0,28)==1);
    CHECK(init_calls==1 && g_esp==0x1001C);
    target_calls=0;
    setup(0x30000,0x40000,0,0,0);
    CHECK(noble_d3d8_guest_call(0x003B6FB0,12)==0); /* Mirror, keep guest refs. */
    CHECK(target_calls==1 && target_args[0]==0x30000 && target_args[1]==0x40000);
    CHECK(present_calls==0 && g_esp==0x10000);
    setup(2,0x32000,36,0,0);
    CHECK(noble_d3d8_guest_call(0x003B8A50,16)==0);
    CHECK(draw_args[0]==2 && draw_args[1]==0x32000 && draw_args[2]==36);
    setup(0x30000,0x31000,2,0x40000,0x41000);
    CHECK(noble_d3d8_guest_call(0x003B7280,24)==1);
    CHECK(g_esp==0x10018 && copy_args[0]==0x30000 && copy_args[1]==0x31000 &&
        copy_args[2]==2 && copy_args[3]==0x40000 && copy_args[4]==0x41000);
    copy_ok=0;setup(0x30000,0,0,0x40000,0);
    CHECK(noble_d3d8_guest_call(0x003B7280,24)==0 && g_esp==0x10000);
    setup(8,4,0x34000,28,0);
    constant_calls=0;
    CHECK(noble_d3d8_guest_call(0x003BA740,20)==1);
    CHECK(draw_args[0]==8 && draw_args[1]==4 && draw_args[2]==0x34000 && draw_args[3]==28);
    CHECK(constant_calls==0); /* Draw must preserve raw uploads absent from the shadow. */
    CHECK(g_esp==0x10014);
    setup(5,6,0x35000,0x36000,36);
    CHECK(noble_d3d8_guest_call(0x003BA8B0,24)==1);
    CHECK(draw_args[2]==0x35000 && draw_args[3]==0x36000 && draw_args[4]==36);
    CHECK(g_esp==0x10018);
    setup(0,0,0,0,0);g_ecx=96;g_edx=0x38000;constant_calls=0;
    CHECK(noble_d3d8_guest_call(0x003B8820,4)==0);
    CHECK(constant_calls==1 && constant_reg==0 && draw_args[0]==0x38000 && draw_args[1]==1);
    setup(0,0,0,0,0);
    CHECK(noble_d3d8_guest_call(0x003B8FD0,4)==0); /* Raw pushbuffer method writer. */
    CHECK(noble_d3d8_guest_call(0x003B7B60,12)==0); /* SetTile, never a stream. */
    CHECK(noble_d3d8_guest_call(0x003BAD40,8)==0); /* Keep guest device flags/waits. */
    CHECK(noble_d3d8_guest_call(0x003BAD80,4)==0);
    setup(0x37000,0x38000,0x39000,0,0);
    CHECK(noble_d3d8_guest_call(0x003B86E0,20)==1);
    CHECK(MEM32(0x39000)==0x30001 && g_esp==0x10014);
    setup(0x30001,0,0,0,0);
    CHECK(noble_d3d8_guest_call(0x003B8E30,8)==0);
    CHECK(bound_shader==0x90001000 && g_esp==0x10000);
    MEM32(0x20000+0x1A04)=0x40000;
    MEM32(0x40000+4)=0x204000;
    MEM32(0x40000+16)=0x271DF27F; /* 640x480, pitch2560. */
    setup(0,0,0,0,0);g_eax=123;readback_ok=1;
    CHECK(noble_d3d8_guest_call(0x003B6520,8)==0);
    CHECK(present_calls==1 && g_eax==123 && g_esp==0x10000);
    CHECK(published_count==1 && published_va==0x80204000 && published_pitch==2560);
    readback_ok=0;
    CHECK(noble_d3d8_guest_call(0x003B6520,8)==0);
    CHECK(published_count==1); /* Failed readback must not publish stale pixels. */
    puts("D3D8 bridge: lifecycle, guest ABI and state preservation pass");
    return 0;
}
