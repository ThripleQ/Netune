#pragma once
/* Minimal <strings.h> for MSVC — maps POSIX names to MSVC equivalents. */

#ifdef _MSC_VER
#include <string.h>
#include <ctype.h>
#include <stddef.h>

#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

/* strcasestr: case-insensitive substring search (GNU extension, not in MSVC). */
static inline char *strcasestr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (_strnicmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
    }
    return NULL;
}
#endif
