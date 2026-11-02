#include "../src/d3d8gl/wide_shader_class.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failure line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    const char *world = "B = c[164].xyzw; B = c[165].xyzw; B = c[166].xyzw; B = c[167].xyzw;";
    const char *transformed = "B = c[181].xyzw; B = c[182].xyzw; B = c[183].xyzw; B = c[184].xyzw;";
    CHECK(noble_wide_projection_shader(world));
    CHECK(noble_wide_projection_shader(transformed));
    CHECK(!noble_wide_projection_shader("A = v0.xyzw; B = c[190].xyzw;"));
    CHECK(!noble_wide_projection_shader("B = c[164].xyzw; B = c[167].xyzw;"));
    CHECK(!noble_wide_projection_shader(NULL));
    puts("wide shader classification passed"); return 0;
}
