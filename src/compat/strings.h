#pragma once
/* Minimal <strings.h> for MSVC — maps POSIX names to MSVC equivalents. */

#ifdef _MSC_VER
#include <string.h>
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

static inline char *strcasestr(const char *haystack, const char *needle)
{
    if (*needle == '\0')
        return (char *)haystack;

    for (; *haystack != '\0'; haystack++) {
        if (strncasecmp(haystack, needle, strlen(needle)) == 0)
            return (char *)haystack;
    }
    return NULL;
}
#endif
