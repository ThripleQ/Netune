#pragma once
/* Minimal <strings.h> for MSVC — maps POSIX names to MSVC equivalents. */

#ifdef _MSC_VER
#include <string.h>
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif
