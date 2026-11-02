/*
 * Windows opt-in page-write watcher for the D3D8GL texture cache.
 *
 * The POSIX memory_watch.c uses mprotect/SIGSEGV and is not built here.
 * This prototype enables the same idea on Windows: watched guest pages are
 * PAGE_READONLY, a write raises an access violation, the VEH records a new
 * page generation, makes the page writable, and execution retries.  It is
 * deliberately opt-in; the content-comparison path remains the default.
 */
#include <windows.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PLATFORM_CONTIGUOUS_BASE 0x80000000UL
#define PLATFORM_CONTIGUOUS_SIZE 0x20000000UL
extern ptrdiff_t noble_guest_memory_offset;

#define WATCH_PAGE_SIZE 0x1000UL
#define WATCH_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / WATCH_PAGE_SIZE)

static unsigned char watched[WATCH_PAGE_COUNT]; /* 0 unwatched, 1 read-only, 2 dirty-writable */
static volatile LONG page_generation[WATCH_PAGE_COUNT];
static volatile LONG current_generation = 1;
static uintptr_t watch_base;
static volatile LONG installed;
static volatile LONG protecting;

static uintptr_t page_base(unsigned long address)
{
    return (uintptr_t)address & ~(uintptr_t)(WATCH_PAGE_SIZE - 1);
}

static int page_index(unsigned long address)
{
    uintptr_t base = watch_base;
    uintptr_t value = page_base(address);
    if (!base || value < base || value >= base + PLATFORM_CONTIGUOUS_SIZE)
        return -1;
    return (int)((value - base) / WATCH_PAGE_SIZE);
}

int page_watch_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_TEXTURE_PAGE_VERSION");
        enabled = value && *value && strcmp(value, "0");
    }
    return enabled;
}

static LONG CALLBACK watch_veh(PEXCEPTION_POINTERS info)
{
    EXCEPTION_RECORD *record = info->ExceptionRecord;
    uintptr_t fault;
    int index;
    DWORD old;

    if (!page_watch_enabled() || record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (record->NumberParameters < 2 || record->ExceptionInformation[0] == 0)
        return EXCEPTION_CONTINUE_SEARCH;
    fault = (uintptr_t)record->ExceptionInformation[1];
    index = page_index((unsigned long)fault);
    if (index < 0 || !watched[index])
        return EXCEPTION_CONTINUE_SEARCH;
    page_generation[index] = InterlockedIncrement(&current_generation);
    watched[index] = 2;
    VirtualProtect((LPVOID)(watch_base + (uintptr_t)index * WATCH_PAGE_SIZE),
                   WATCH_PAGE_SIZE, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

void page_watch_initialize(void)
{
    if (!page_watch_enabled() || installed)
        return;
    watch_base = (uintptr_t)PLATFORM_CONTIGUOUS_BASE + (uintptr_t)noble_guest_memory_offset;
    AddVectoredExceptionHandler(1, watch_veh);
    installed = 1;
}

void page_watch_protect(unsigned long address, unsigned long size)
{
    uintptr_t first, last, page;
    DWORD old;

    if (!page_watch_enabled() || !address || !size)
        return;
    page_watch_initialize();
    first = page_base(address);
    last = page_base(address + size - 1);
    for (page = first; page <= last; page += WATCH_PAGE_SIZE) {
        int index = page_index((unsigned long)page);
        if (index < 0 || watched[index] == 1)
            continue;
        watched[index] = 1;
        VirtualProtect((LPVOID)page, WATCH_PAGE_SIZE, PAGE_READONLY, &old);
    }
}

unsigned long page_watch_generation(unsigned long address, unsigned long size)
{
    unsigned long newest = 0;
    uintptr_t first, last, page;

    if (!page_watch_enabled() || !address || !size)
        return 0;
    first = page_base(address);
    last = page_base(address + size - 1);
    for (page = first; page <= last; page += WATCH_PAGE_SIZE) {
        int index = page_index((unsigned long)page);
        if (index < 0)
            continue;
        if ((unsigned long)page_generation[index] > newest)
            newest = (unsigned long)page_generation[index];
    }
    return newest;
}

unsigned long page_watch_serial(void)
{
    if (!page_watch_enabled())
        return 0;
    return (unsigned long)current_generation;
}

void page_watch_prepare_write(void *address, unsigned long size)
{
    uintptr_t first, last, page;
    DWORD old;

    if (!page_watch_enabled() || !address || !size)
        return;
    first = page_base((unsigned long)address);
    last = page_base((unsigned long)address + size - 1);
    for (page = first; page <= last; page += WATCH_PAGE_SIZE) {
        int index = page_index((unsigned long)page);
        if (index < 0)
            continue;
        page_generation[index] = InterlockedIncrement(&current_generation);
        VirtualProtect((LPVOID)page, WATCH_PAGE_SIZE, PAGE_READWRITE, &old);
    }
}

void page_watch_forget(void *address, unsigned long size)
{
    uintptr_t first, last, page;
    DWORD old;

    if (!page_watch_enabled() || !address || !size)
        return;
    first = page_base((unsigned long)address);
    last = page_base((unsigned long)address + size - 1);
    for (page = first; page <= last; page += WATCH_PAGE_SIZE) {
        int index = page_index((unsigned long)page);
        if (index < 0)
            continue;
        watched[index] = 0;
        VirtualProtect((LPVOID)page, WATCH_PAGE_SIZE, PAGE_READWRITE, &old);
    }
}
