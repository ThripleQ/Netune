#pragma once

#include <stdint.h>

/* ── Lyric line ────────────────────────────────────── */
typedef struct {
    int   time_ms;   /* timestamp in milliseconds */
    char *text;      /* lyric text (heap-allocated) */
} LyricLine;

/* ── Parsed lyrics ─────────────────────────────────── */
typedef struct {
    LyricLine *lines;
    int        count;
} Lyrics;

/* ── API ────────────────────────────────────────────── */

/* Parse LRC text into Lyrics.
   Returns heap-allocated Lyrics, or NULL on empty/error.
   Lines are sorted by time_ms. */
Lyrics* lyric_parse(const char *lrc_text);

/* Given playback time in ms, return index of the active line.
   Returns -1 if before first line or no lyrics loaded. */
int lyric_find_line(const Lyrics *ly, int time_ms);

/* Free all resources. */
void lyric_free(Lyrics *ly);
