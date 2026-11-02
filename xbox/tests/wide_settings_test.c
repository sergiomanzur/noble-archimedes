#include "../src/d3d8gl/wide_setting_value.h"
#include <math.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failure line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    CHECK(fabsf(noble_wide_parse_float("1.7777778",1.0f,4.0f,1.0f)-1.7777778f)<0.00001f);
    CHECK(noble_wide_parse_float(" 2.0 \t",1.0f,4.0f,1.0f)==2.0f);
    CHECK(noble_wide_parse_float("2.0junk",1.0f,4.0f,1.0f)==1.0f);
    CHECK(noble_wide_parse_float("nan",1.0f,4.0f,1.0f)==1.0f);
    CHECK(noble_wide_parse_float("1e999",1.0f,4.0f,1.0f)==1.0f);
    CHECK(noble_wide_parse_float(NULL,1.0f,4.0f,1.0f)==1.0f);
    return 0;
}
