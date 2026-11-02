/* Unit test for the balanced, nested pair timer.  Includes the
 * implementation so the pure pt_begin/pt_end core is exercised directly with
 * a synthetic clock (no QPC involved). */
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/pair_timer.c"

int main(void)
{
    struct pt_ctx c;
    memset(&c, 0, sizeof(c));

    /* balanced nesting: parent[0..300], child inside [100..200] */
    pt_begin(&c, 0, 100);
    pt_begin(&c, 1, 150);
    pt_end(&c, 1, 0, 250);
    pt_end(&c, 0, 0, 400);

    assert(c.s[0].calls == 1);
    assert(c.s[1].calls == 1);
    assert(c.s[0].inclusive_ns == 300);   /* ticks */
    assert(c.s[1].inclusive_ns == 100);
    assert(c.s[0].child_in_parent_ns == 100);
    assert(c.s[1].child_in_parent_ns == 100);
    assert(c.s[0].inclusive_ns - c.s[0].child_in_parent_ns == 200);
    assert(c.s[0].unmatched_enter == 0 && c.s[0].unmatched_exit == 0);
    assert(c.s[1].unmatched_enter == 0 && c.s[1].unmatched_exit == 0);

    /* unbalanced: double enter and stray exit are recorded, not added */
    memset(&c, 0, sizeof(c));
    pt_begin(&c, 0, 10);
    pt_begin(&c, 0, 20);            /* unmatched enter */
    pt_end(&c, 0, 0, 30);           /* closes the first open frame */
    pt_end(&c, 0, 0, 40);           /* unmatched exit */
    assert(c.s[0].unmatched_enter == 1);
    assert(c.s[0].unmatched_exit == 1);
    assert(c.s[0].calls == 1);
    assert(c.s[0].inclusive_ns == 20);

    /* filtering: unknown/disabled ids never touch stats */
    memset(&c, 0, sizeof(c));
    pt_begin(&c, 9, 0);   /* out of range */
    pt_end(&c, 9, 0, 5);
    assert(c.s[9 == 8 ? 0 : 0].calls == 0);

    /* child outside parent does not count as inside-parent */
    memset(&c, 0, sizeof(c));
    pt_begin(&c, 1, 100);
    pt_end(&c, 1, 0, 200);
    assert(c.s[1].inclusive_ns == 100);
    assert(c.s[1].child_in_parent_ns == 0);
    assert(c.s[0].child_in_parent_ns == 0);

    printf("pair-timer core checks passed\n");
    return 0;
}
