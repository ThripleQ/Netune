#include "ui/color_util.h"

#include <algorithm>
#include <cmath>

namespace netune {

float srgb_linear(float c) {
    c /= 255.0f;
    return (c <= 0.03928f) ? (c / 12.92f)
                           : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float wcag_luminance(const ThemeColor &c) {
    return 0.2126f * srgb_linear((float)c.r)
         + 0.7152f * srgb_linear((float)c.g)
         + 0.0722f * srgb_linear((float)c.b);
}

float wcag_contrast(const ThemeColor &a, const ThemeColor &b) {
    float l1 = wcag_luminance(a);
    float l2 = wcag_luminance(b);
    if (l1 < l2) std::swap(l1, l2);
    return (l1 + 0.05f) / (l2 + 0.05f);
}

float wcag_contrast_ratio(const ThemeColor &text, const ThemeColor &bg) {
    return wcag_contrast(text, bg);
}

/* RGB (0..255) → HSL. Hue in [0,1), s/l in [0,1]. */
void rgb_to_hsl(const ThemeColor &c, float &h, float &s, float &l) {
    float rf = c.r / 255.0f, gf = c.g / 255.0f, bf = c.b / 255.0f;
    float mx = std::max(rf, std::max(gf, bf));
    float mn = std::min(rf, std::min(gf, bf));
    l = (mx + mn) / 2.0f;
    float d = mx - mn;
    if (d == 0.0f) { h = 0.0f; s = 0.0f; return; }
    s = (l > 0.5f) ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == rf)      h = (gf - bf) / d + (gf < bf ? 6.0f : 0.0f);
    else if (mx == gf) h = (bf - rf) / d + 2.0f;
    else               h = (rf - gf) / d + 4.0f;
    h /= 6.0f;
}

static uint8_t hue_to_rgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return (uint8_t)((p + (q - p) * 6.0f * t) * 255.0f);
    if (t < 1.0f / 2.0f) return (uint8_t)(q * 255.0f);
    if (t < 2.0f / 3.0f) return (uint8_t)((p + (q - p) * (2.0f / 3.0f - t) * 6.0f) * 255.0f);
    return (uint8_t)(p * 255.0f);
}

ThemeColor hsl_to_rgb(float h, float s, float l) {
    ThemeColor out;
    out.has_color = true;
    if (s == 0.0f) {
        uint8_t v = (uint8_t)(l * 255.0f);
        out.r = out.g = out.b = v;
        return out;
    }
    float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
    float p = 2.0f * l - q;
    out.r = hue_to_rgb(p, q, h + 1.0f / 3.0f);
    out.g = hue_to_rgb(p, q, h);
    out.b = hue_to_rgb(p, q, h - 1.0f / 3.0f);
    return out;
}

ThemeColor color_readable_on_bg(const ThemeColor &text, const ThemeColor &bg,
                                float min_ratio) {
    float th, ts, tl;
    rgb_to_hsl(text, th, ts, tl);
    float bh, bs, bl;
    rgb_to_hsl(bg, bh, bs, bl);
    float blum = wcag_luminance(bg);

    /* When the text colour is itself very light (or very dark) on an
       opposite background, keeping its hue/saturation forces it to black
       or white — ugly and flat. Try progressively desaturated versions of
       the SAME hue first (soft, tinted darks/lights that read much nicer
       than pure black/white) and only fall back to pure black/white when
       even a fully neutral tone cannot reach the target contrast. */
    for (int sat_step = 0; sat_step <= 4; sat_step++) {
        float sat = (sat_step == 0) ? ts : ts * (1.0f - 0.25f * sat_step);
        ThemeColor out;
        if (bl >= 0.5f) {
            /* Bright background → need DARKER text: search [0, tl] for the
               highest lightness that still keeps >= min_ratio (closest to
               the original colour while readable). */
            float lo = 0.0f, hi = tl;
            for (int i = 0; i < 24; i++) {
                float mid = (lo + hi) * 0.5f;
                ThemeColor c = hsl_to_rgb(th, sat, mid);
                float lum = wcag_luminance(c);
                if ((blum + 0.05f) / (lum + 0.05f) >= min_ratio) lo = mid;
                else                                             hi = mid;
            }
            out = hsl_to_rgb(th, sat, lo);
        } else {
            /* Dark background → need LIGHTER text: search [tl, 1] for the
               lowest lightness that keeps >= min_ratio. */
            float lo = tl, hi = 1.0f;
            for (int i = 0; i < 24; i++) {
                float mid = (lo + hi) * 0.5f;
                ThemeColor c = hsl_to_rgb(th, sat, mid);
                float lum = wcag_luminance(c);
                if ((lum + 0.05f) / (blum + 0.05f) >= min_ratio) hi = mid;
                else                                             lo = mid;
            }
            out = hsl_to_rgb(th, sat, hi);
        }
        if (wcag_contrast(out, bg) >= min_ratio) return out;
    }
    ThemeColor last;
    last.has_color = true;
    last.r = last.g = last.b = (bl >= 0.5f) ? 0 : 255;
    return last;
}

}  // namespace netune
