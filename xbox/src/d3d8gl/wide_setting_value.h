#ifndef NOBLE_WIDE_SETTING_VALUE_H
#define NOBLE_WIDE_SETTING_VALUE_H
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

static float noble_wide_parse_float(const char *text, float minimum, float maximum, float fallback)
{
    char *end;
    float value;
    if (!text || !*text) return fallback;
    errno = 0;
    value = strtof(text, &end);
    if (end == text || errno == ERANGE) return fallback;
    while (*end && isspace((unsigned char)*end)) ++end;
    if (*end || !(value >= minimum && value <= maximum)) return fallback;
    return value;
}
#endif
