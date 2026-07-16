#pragma once

#include "core/lyric.h"

/* Load a .lrc file from disk and parse it.
   Returns heap-allocated Lyrics, or NULL on error. */
Lyrics* lrc_load_file(const char *path);
