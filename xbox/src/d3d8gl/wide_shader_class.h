#ifndef NOBLE_WIDE_SHADER_CLASS_H
#define NOBLE_WIDE_SHADER_CLASS_H
#include <string.h>

/* This recognizes the two matrix-register layouts observed in the title's
 * captured world shaders. It is not a general NV2A shader classifier. */
static int noble_wide_projection_shader(const char *source)
{
    if (!source) return 0;
    return (strstr(source,"B = c[164].") && strstr(source,"B = c[165].") &&
            strstr(source,"B = c[166].") && strstr(source,"B = c[167].")) ||
           (strstr(source,"B = c[181].") && strstr(source,"B = c[182].") &&
            strstr(source,"B = c[183].") && strstr(source,"B = c[184]."));
}
#endif
