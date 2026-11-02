#include "../src/d3d8gl/presentation_geometry.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s at line %d\n", #x, __LINE__); return 1; } } while (0)
int main(void) {
    struct noble_present_rect r;
    CHECK(noble_present_fit(1920,1080,640,480,&r));
    CHECK(r.x==240 && r.y==0 && r.width==1440 && r.height==1080);
    CHECK(noble_present_fit(3440,1440,640,480,&r));
    CHECK(r.x==760 && r.y==0 && r.width==1920 && r.height==1440);
    CHECK(noble_present_fit(800,800,640,480,&r));
    CHECK(r.x==0 && r.y==100 && r.width==800 && r.height==600);
    CHECK(noble_present_fit(1920,1080,1920,1080,&r));
    CHECK(r.x==0 && r.y==0 && r.width==1920 && r.height==1080);
    CHECK(!noble_present_fit(0,1080,640,480,&r));
    CHECK(!noble_present_fit(1920,1080,0,480,&r));
    CHECK(!noble_present_fit(1920,1080,640,480,NULL));
    CHECK(noble_wide_window_width(640, 480, 4.0f / 3.0f) == 640);
    CHECK(noble_wide_window_width(640, 480, 16.0f / 9.0f) == 853);
    CHECK(noble_wide_window_width(640, 480, 21.0f / 9.0f) == 1120);
    CHECK(noble_wide_window_width(640, 720, 16.0f / 9.0f) == 1280);
    CHECK(noble_wide_window_width(640, 0, 16.0f / 9.0f) == 640);
    CHECK(noble_guest_target_x(0,640,853,1.0f) == 106);
    CHECK(noble_guest_target_x(100,640,853,1.0f) == 206);
    CHECK(noble_guest_target_x(640,640,853,1.0f) == 746);
    CHECK(noble_guest_target_x(100,640,1120,1.0f) == 340);
    CHECK(fabsf(noble_wide_clip_scale(4.0f / 3.0f, 1.0f) - 1.0f) < 0.00001f);
    CHECK(fabsf(noble_wide_clip_scale(16.0f / 9.0f, 1.0f) - 0.75f) < 0.00001f);
    CHECK(fabsf(noble_wide_clip_scale(21.0f / 9.0f, 1.0f) - (4.0f / 7.0f)) < 0.00001f);
    CHECK(fabsf(noble_wide_clip_scale(16.0f / 9.0f, 1.25f) - 0.6f) < 0.00001f);
    CHECK(noble_wide_clip_scale(0.0f, 1.0f) == 1.0f);
    {
        struct noble_projection_scale s;
        CHECK(noble_projection_scaling(16.0f / 9.0f, 1.25f, &s));
        CHECK(fabsf(s.x - 0.6f) < 0.00001f);
        CHECK(fabsf(s.y - 0.8f) < 0.00001f);
        CHECK(noble_projection_scaling(4.0f / 3.0f, 1.0f, &s));
        CHECK(fabsf(s.x - 1.0f) < 0.00001f && fabsf(s.y - 1.0f) < 0.00001f);
        CHECK(!noble_projection_scaling(0.0f, 1.0f, &s));
        CHECK(!noble_projection_scaling(16.0f / 9.0f, 0.0f, &s));
        CHECK(!noble_projection_scaling(16.0f / 9.0f, 1.0f, NULL));
    }
    puts("presentation geometry passed"); return 0;
}
