#pragma once

#include <cstdint>

#include "ui/theme.h"

/* ── Colour contrast utilities (WCAG 2.x) ─────────────
   Pure colour math, no FTXUI / UI dependency — usable from any layer.

   The goal: pick a text colour that is BOTH readable and pleasant on a
   given background. Naive approaches (invert, or scale every channel)
   either destroy the colour's identity (black/white) or drift its hue.

   Strategy (as used by WCAG-aware tools like Colorable):
     1. Keep the TEXT's own hue & saturation (its identity/category).
     2. Binary-search its LIGHTNESS until the WCAG contrast ratio against
        the background reaches the target (default 4.5:1, AA for normal
        text). Bright background → darker text, dark background → lighter.
     3. If no lightness of the same hue/saturation reaches the target,
        progressively desaturate (same hue, softer tone) and retry.
     4. Only as a last resort return pure black / pure white. */

namespace netune {

/* sRGB → linear (WCAG 2.x formula). Input 0..255, output 0..1. */
float srgb_linear(float c);

/* WCAG relative luminance of an sRGB colour. Input channels 0..255. */
float wcag_luminance(const ThemeColor &c);

/* WCAG contrast ratio between two colours (>= 1.0). 4.5:1 is the AA
   minimum for normal text, 7:1 is AAA. */
float wcag_contrast(const ThemeColor &a, const ThemeColor &b);

/* Contrast ratio of `text` over `bg`. */
float wcag_contrast_ratio(const ThemeColor &text, const ThemeColor &bg);

/* HSL (h/s/l in [0,1]) → sRGB. */
ThemeColor hsl_to_rgb(float h, float s, float l);

/* sRGB → HSL (h in [0,1), s/l in [0,1]). */
void rgb_to_hsl(const ThemeColor &c, float &h, float &s, float &l);

/* Pick a readable colour for `text` on `bg` using the WCAG strategy
   above (hue-preserving, sat-fallback, black/white last resort).
   `min_ratio` defaults to 4.5:1. Returns the adjusted colour with
   has_color set. */
ThemeColor color_readable_on_bg(const ThemeColor &text, const ThemeColor &bg,
                                float min_ratio = 4.5f);

}  // namespace netune
