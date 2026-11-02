#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
float noble_wide_guest_adjust(float value);
float noble_wide_guest_adjust_y(float value);
int noble_wide_guest_active(void);
int main(int argc, char **argv)
{
    float expected = 700.0f, actual;
    if (argc != 2) return 1;
    _putenv_s("RECOMP_WIDE_GUEST_PROJECTION", strcmp(argv[1],"default") ? "1" : "");
    _putenv_s("RECOMP_WIDE_EXPERIMENT", !strcmp(argv[1],"wide-off") ? "0" : "1");
    _putenv_s("RECOMP_D3D8GL", !strcmp(argv[1],"renderer-off") ? "0" : "1");
    _putenv_s("RECOMP_WIDE_ASPECT", (!strcmp(argv[1],"invalid") || !strcmp(argv[1],"invalid-fov")) ? "nan" :
        !strcmp(argv[1],"malformed") ? "1.7777778junk" :
        !strcmp(argv[1],"ultrawide") ? "2.3333333" : "1.7777778");
    _putenv_s("RECOMP_WIDE_FOV_SCALE", (!strcmp(argv[1],"fov") || !strcmp(argv[1],"invalid-fov")) ? "1.2" : "1");
    if (!strcmp(argv[1],"enabled")) expected = 525.0f;
    if (!strcmp(argv[1],"fov")) expected = 437.5f;
    if (!strcmp(argv[1],"ultrawide")) expected = 400.0f;
    if (!strcmp(argv[1],"invalid-fov")) expected = 700.0f / 1.2f;
    actual = noble_wide_guest_adjust(700.0f);
    if (noble_wide_guest_active() !=
        (!strcmp(argv[1],"enabled") || !strcmp(argv[1],"fov") ||
         !strcmp(argv[1],"ultrawide") || !strcmp(argv[1],"invalid-fov"))) {
        fprintf(stderr,"incorrect projection activation for %s\n", argv[1]); return 1;
    }
    if (fabsf(actual - expected) > 0.001f) { fprintf(stderr,"projection %g expected %g\n",actual,expected); return 1; }
    actual = noble_wide_guest_adjust_y(600.0f);
    expected = (!strcmp(argv[1],"fov") || !strcmp(argv[1],"invalid-fov")) ? 500.0f : 600.0f;
    if (fabsf(actual - expected) > 0.001f) { fprintf(stderr,"projection y %g expected %g\n",actual,expected); return 1; }
    return 0;
}
