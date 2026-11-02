/* Windows page-write watcher contract for the opt-in texture version path. */
#include <windows.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLATFORM_CONTIGUOUS_BASE 0x80000000UL

ptrdiff_t noble_guest_memory_offset;

int page_watch_enabled(void);
void page_watch_initialize(void);
void page_watch_protect(unsigned long address, unsigned long size);
unsigned long page_watch_generation(unsigned long address, unsigned long size);
unsigned long page_watch_serial(void);
void page_watch_forget(void *address, unsigned long size);

int main(void)
{
    void *memory = VirtualAlloc((LPVOID)0x30000000, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void *other = VirtualAlloc((LPVOID)0x30010000, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    unsigned long serial, generation;
    volatile unsigned int *page;

    assert(memory && other);
    noble_guest_memory_offset = (ptrdiff_t)((uintptr_t)memory - PLATFORM_CONTIGUOUS_BASE);
    _putenv("RECOMP_TEXTURE_PAGE_VERSION=1");
    page_watch_initialize();
    page = (volatile unsigned int *)memory;

    page_watch_protect((unsigned long)memory, 0x2000);
    serial = page_watch_serial();
    *page = 1; /* page 0 write faults and records generation */
    generation = page_watch_generation((unsigned long)memory, 4);
    assert(generation != 0);
    assert(page_watch_serial() > serial);

    /* Multiple writes before re-protection are part of the same dirty page. */
    *page = 2;
    assert(page_watch_generation((unsigned long)memory, 4) == generation);

    /* Re-protection must make the next write observable again. */
    page_watch_protect((unsigned long)memory, 0x1000);
    serial = page_watch_serial();
    *page = 3;
    assert(page_watch_serial() > serial);
    page_watch_protect((unsigned long)memory, 0x1000);
    serial = page_watch_serial();
    *page = 4;
    assert(page_watch_serial() > serial);

    /* A write crossing the protected page boundary faults on both pages. */
    page_watch_protect((unsigned long)memory, 0x2000);
    serial = page_watch_serial();
    *(volatile unsigned long long *)((unsigned char *)memory + 0x0ffc) = 0x1122334455667788ull;
    assert(page_watch_serial() > serial);
    assert(page_watch_generation((unsigned long)memory + 0x0ffc, 8) != 0);

    /* Unrelated writable pages must not be claimed by the watcher. */
    *(volatile unsigned int *)other = 0x55;
    assert(*(volatile unsigned int *)other == 0x55);

    /* Forgetting a range restores writable access and clears its watch. */
    page_watch_forget(memory, 0x2000);
    *page = 5;

    VirtualFree(other, 0, MEM_RELEASE);
    VirtualFree(memory, 0, MEM_RELEASE);
    _putenv("RECOMP_TEXTURE_PAGE_VERSION=");
    puts("texture page-version write tracking passed");
    return 0;
}
