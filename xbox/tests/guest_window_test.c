/*
 * Regression for the frame-source contract (fb_present.c):
 *
 * A frame the window may read is a guest surface: [fb_va, fb_va + bytes) must
 * lie wholly inside one guest window the runtime maps at guest_va +
 * xbox_GetMemoryOffset(), or inside committed pages of guest_vmem. Anything
 * else has no host translation.
 *
 * The crash this pins came from a desynced pushbuffer: the surface-colour
 * offset became the data word 0xFF1003E0, which is 0x3E0 past the end of the
 * 1 MB flash aperture and in no other window. With the gate's host base
 * (0x10000) the old present path added the memory offset and read
 * 0xFF1103E0, a canonical x64 but unmapped host address, and took an access
 * violation in the copy. The offset is the chosen base, not a fixed 0x10000;
 * the predicate works in guest VAs, so it does not depend on the base.
 *
 * The builder below is the same pure function the production window list
 * uses; the layout inputs are the runtime's constants, so a change to the
 * mirror count, RAM size, or an aperture fails here. Missing mirrors and
 * absent apertures must remain holes, not be bridged.
 */
#include "kernel/xbox_memory_layout.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define ALL_MIRRORS ((1u << XBOX_NUM_MIRRORS) - 1u)
#define RAM64 XBOX_TOTAL_RAM
#define RAM128 XBOX_DEVKIT_RAM

static unsigned build(struct xbox_guest_window *windows, unsigned capacity,
                      uint64_t ram_size, unsigned mirror_mask, uint64_t contig_size,
                      uint64_t tiled_size, uint64_t nv2a_size, uint64_t mcpx_size,
                      uint64_t flash_size)
{
    return xbox_GuestWindowsBuild(windows, capacity, ram_size, mirror_mask,
                                  contig_size, tiled_size, nv2a_size, mcpx_size,
                                  flash_size);
}

static int contained(const struct xbox_guest_window *windows, unsigned count,
                     uint32_t va, uint64_t bytes)
{
    return xbox_GuestWindowsContain(windows, count, va, bytes);
}

static int window_is(const struct xbox_guest_window *window, uint64_t lo, uint64_t hi)
{
    return window->lo == lo && window->hi == hi;
}

/* guest_vmem_query's shape, with one controllable region. */
static struct { uint32_t base, size, state, protect; } fake_region;
static unsigned fake_calls;
static int fake_query(uint32_t address, uint32_t info[7])
{
    fake_calls++;
    if (address < fake_region.base ||
        (uint64_t)address >= (uint64_t)fake_region.base + fake_region.size)
        return 0;
    info[0] = fake_region.base;
    info[1] = fake_region.base;
    info[2] = fake_region.protect;
    info[3] = fake_region.size;
    info[4] = fake_region.state;
    info[5] = fake_region.protect;
    info[6] = 0;
    return 1;
}

int main(void)
{
    struct xbox_guest_window windows[64];
    const uint64_t frame = 2560u * 480u;               /* pitch * height */
    const uint64_t ram_end64 = (uint64_t)(1 + XBOX_NUM_MIRRORS) * RAM64;
    const uint64_t mcpx_flash_hi = (uint64_t)XBOX_FLASH_BASE + XBOX_FLASH_SIZE;
    unsigned count;

    /* 64 MB retail, every mirror and every aperture. */
    count = build(windows, 64, RAM64, ALL_MIRRORS, XBOX_GUEST_CONTIG_SIZE,
                  XBOX_GUEST_CONTIG_SIZE, XBOX_NV2A_SIZE, XBOX_MCPX_SIZE,
                  XBOX_FLASH_SIZE);
    assert(count == 5);
    assert(window_is(&windows[0], 0, ram_end64));
    assert(window_is(&windows[1], XBOX_GUEST_CONTIG_BASE,
                     (uint64_t)XBOX_GUEST_CONTIG_BASE + XBOX_GUEST_CONTIG_SIZE));
    assert(window_is(&windows[2], XBOX_TILED_BASE,
                     (uint64_t)XBOX_TILED_BASE + XBOX_GUEST_CONTIG_SIZE));
    assert(window_is(&windows[3], XBOX_NV2A_BASE,
                     (uint64_t)XBOX_NV2A_BASE + XBOX_NV2A_SIZE));
    assert(window_is(&windows[4], XBOX_MCPX_BASE, mcpx_flash_hi));

    /* Physical addresses are ordinary guest VAs in the identity-mapped RAM,
     * including the 26-bit wrap the mirrors model; a read that crosses a
     * mirror boundary is still within one merged RAM+mirrors window. */
    assert(contained(windows, count, 0x001003E0u, frame));
    assert(contained(windows, count, 0x031003E0u, frame));
    assert(contained(windows, count, (uint32_t)(RAM64 - 4), 8)); /* crosses 64 MB */
    assert(contained(windows, count, (uint32_t)RAM64, 4));       /* mirror 1 */
    assert(contained(windows, count, (uint32_t)ram_end64 - 1, 1));
    assert(!contained(windows, count, (uint32_t)ram_end64, 4));  /* hole above mirrors */

    /* Guest VAs through the contiguous window: the movie/contiguous surfaces
     * the executor resolves from physical offsets (XBOX_CONTIG_BASE + P). */
    assert(contained(windows, count, XBOX_GUEST_CONTIG_BASE, 4));
    assert(contained(windows, count, XBOX_GUEST_CONTIG_BASE + 0x1003E0u, 1200u * 1024u));
    assert(contained(windows, count,
                     XBOX_GUEST_CONTIG_BASE + XBOX_GUEST_CONTIG_SIZE - (1u * 1024u * 1024u),
                     1u * 1024u * 1024u));               /* ends at window end */
    assert(!contained(windows, count,
                      XBOX_GUEST_CONTIG_BASE + XBOX_GUEST_CONTIG_SIZE - (1u * 1024u * 1024u),
                      2u * 1024u * 1024u));              /* crosses the end */

    /* The tiled alias is a second view of the same bytes; still a guest VA. */
    assert(contained(windows, count, XBOX_TILED_BASE + 0x1003E0u, frame));
    assert(!contained(windows, count,
                      XBOX_TILED_BASE + XBOX_GUEST_CONTIG_SIZE - (1u * 1024u * 1024u),
                      2u * 1024u * 1024u));

    /* Device apertures are mapped, the holes between them are not. */
    assert(contained(windows, count, XBOX_NV2A_BASE, XBOX_NV2A_SIZE));
    assert(!contained(windows, count, XBOX_NV2A_BASE + XBOX_NV2A_SIZE, 4));
    assert(contained(windows, count, XBOX_MCPX_BASE, XBOX_MCPX_SIZE));
    assert(contained(windows, count, XBOX_FLASH_BASE, XBOX_FLASH_SIZE));

    /* The crash: 0x3E0 past the 1 MB flash aperture, in no window. The host
     * address the old copy used (0xFF1103E0 with the gate's 0x10000 base) is
     * canonical and unmapped; it is not accepted as a guest VA either. */
    assert(!contained(windows, count, 0xFF1003E0u, frame));
    assert(!contained(windows, count, 0xFF1103E0u, frame));

    /* Degenerate/overflow ranges are not surfaces. */
    assert(!contained(windows, count, XBOX_GUEST_CONTIG_BASE + 0x1003E0u, 0));
    assert(!contained(windows, count, 0xFFFFFFFFu, 2));
    assert(!contained(windows, count, 0xFFFFFF00u, 0x200));

    /* A missing mirror is a real hole: no merge for RAM+mirrors across it. */
    count = build(windows, 64, RAM64, ALL_MIRRORS & ~(1u << 5),
                  XBOX_GUEST_CONTIG_SIZE, XBOX_GUEST_CONTIG_SIZE,
                  XBOX_NV2A_SIZE, XBOX_MCPX_SIZE, XBOX_FLASH_SIZE);
    assert(count == 6);
    assert(contained(windows, count, (uint32_t)(5 * RAM64), 4));
    assert(!contained(windows, count, (uint32_t)(6 * RAM64), 4));
    assert(!contained(windows, count, (uint32_t)(6 * RAM64 + RAM64 - 4), 4));
    assert(contained(windows, count, (uint32_t)(7 * RAM64), 4));

    /* 128 MB devkit: the 16th mirror would overlap the contiguous window, so
     * the mapping skips it. The builder is given that mask and must merge the
     * windows on either side of the skip only where they really meet. */
    {
        const unsigned mask128 = ALL_MIRRORS & ~(1u << 15);

        count = build(windows, 64, RAM128, mask128, XBOX_GUEST_CONTIG_SIZE,
                      XBOX_GUEST_CONTIG_SIZE, XBOX_NV2A_SIZE, XBOX_MCPX_SIZE,
                      XBOX_FLASH_SIZE);
        assert(count == 5);
        /* RAM+m0..m14 and the contiguous window meet at 0x80000000; m15 is
         * skipped, so the merged range ends at the contiguous window's end. */
        assert(window_is(&windows[0], 0,
                         (uint64_t)XBOX_GUEST_CONTIG_BASE + XBOX_GUEST_CONTIG_SIZE));
        assert(!contained(windows, count,
                          XBOX_GUEST_CONTIG_BASE + XBOX_GUEST_CONTIG_SIZE, 4));
        /* m16 starts at 17 * 128 MB = 0x88000000; through m27 it ends at
         * 29 * 128 MB = 0xE8000000. */
        assert(window_is(&windows[1], 17 * (uint64_t)RAM128,
                         29 * (uint64_t)RAM128));
        assert(!contained(windows, count, (uint32_t)(29 * (uint64_t)RAM128), 4));
        assert(contained(windows, count, (uint32_t)(17 * (uint64_t)RAM128), 4));
        assert(window_is(&windows[2], XBOX_TILED_BASE,
                         (uint64_t)XBOX_TILED_BASE + XBOX_GUEST_CONTIG_SIZE));
        assert(window_is(&windows[3], XBOX_NV2A_BASE,
                         (uint64_t)XBOX_NV2A_BASE + XBOX_NV2A_SIZE));
        assert(window_is(&windows[4], XBOX_MCPX_BASE, mcpx_flash_hi));
    }

    /* Absent apertures are absent, not zero-length windows. */
    count = build(windows, 64, RAM64, ALL_MIRRORS, 0, 0, 0, 0, 0);
    assert(count == 1);
    assert(window_is(&windows[0], 0, ram_end64));
    assert(!contained(windows, count, XBOX_GUEST_CONTIG_BASE, 4));
    assert(!contained(windows, count, XBOX_TILED_BASE, 4));
    assert(!contained(windows, count, XBOX_NV2A_BASE, 4));

    /* A capacity smaller than the raw list truncates safely (no overflow). */
    count = build(windows, 2, 0, 0, XBOX_GUEST_CONTIG_SIZE,
                  XBOX_GUEST_CONTIG_SIZE, XBOX_NV2A_SIZE, XBOX_MCPX_SIZE,
                  XBOX_FLASH_SIZE);
    assert(count == 2);
    assert(windows[0].lo <= windows[1].lo);

    /* guest_vmem: only committed, non-guarded pages are readable. */
    fake_region.base = 0x74000000u;
    fake_region.size = 0x2000u;
    fake_region.state = XBOX_GUEST_MEM_COMMIT;
    fake_region.protect = 0x04u; /* PAGE_READWRITE */
    assert(xbox_GuestPagesCommitted(0x74000100u, 0x100u, fake_query));
    fake_calls = 0;
    assert(!xbox_GuestPagesCommitted(0x74000100u, 0x100u, NULL));
    assert(fake_calls == 0);
    fake_region.state = 0x2000u; /* MEM_RESERVE */
    assert(!xbox_GuestPagesCommitted(0x74000100u, 0x100u, fake_query));
    fake_region.state = XBOX_GUEST_MEM_COMMIT;
    fake_region.protect = XBOX_GUEST_PAGE_NOACCESS;
    assert(!xbox_GuestPagesCommitted(0x74000100u, 0x100u, fake_query));
    fake_region.protect = XBOX_GUEST_PAGE_GUARD;
    assert(!xbox_GuestPagesCommitted(0x74000100u, 0x100u, fake_query));
    fake_region.protect = 0x04u;
    fake_region.size = 0x1000u; /* the second page is not in the fake region */
    assert(!xbox_GuestPagesCommitted(0x74000F00u, 0x200u, fake_query));
    assert(!xbox_GuestPagesCommitted(0xFFFFFF00u, 0x200u, fake_query));
    assert(!xbox_GuestPagesCommitted(0x74000000u, 0, fake_query));

    puts("guest window contract checks passed");
    return 0;
}
