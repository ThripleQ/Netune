#pragma once

/*
 * Portable wcwidth() wrapper for platforms that lack it (MSVC).
 *
 * On POSIX systems we use libc's wcwidth().  On MSVC we roll our own:
 *   - Most CJK / fullwidth ranges
 *   - The hyphen-minus workaround (MSDN U+2010)
 *   - Zero-width characters (combining marks, control chars)
 *
 * Implementation based on Markus Kuhn's public-domain wcwidth().
 */

#include <wchar.h>

#ifdef _MSC_VER

static int compat_wcwidth(wchar_t wc)
{
    /* Zero-width code points (C0, C1, combining, zero-width chars) */
    if (wc == 0)
        return 0;
    if (wc < 0x20 || (wc >= 0x7F && wc < 0xA0))
        return -1;
    if (wc >= 0x0300 && wc <= 0x036F)   return 0; /* combining diacritical marks */
    if (wc >= 0x0483 && wc <= 0x0489)   return 0; /* Cyrillic combining */
    if ((wc >= 0x0591 && wc <= 0x05BD) ||
        wc == 0x05BF)                   return 0; /* Hebrew combining */
    if (wc >= 0x05C1 && wc <= 0x05C2)   return 0;
    if (wc >= 0x05C4 && wc <= 0x05C5)   return 0;
    if (wc == 0x05C7)                   return 0;
    if (wc >= 0x0610 && wc <= 0x061A)   return 0;
    if (wc >= 0x064B && wc <= 0x065F)   return 0;
    if (wc == 0x0670)                   return 0;
    if (wc >= 0x06D6 && wc <= 0x06DC)   return 0;
    if (wc >= 0x06DF && wc <= 0x06E4)   return 0;
    if (wc >= 0x06E7 && wc <= 0x06E8)   return 0;
    if (wc >= 0x06EA && wc <= 0x06ED)   return 0;
    if (wc == 0x0711)                   return 0;
    if (wc >= 0x0730 && wc <= 0x074A)   return 0;
    if (wc >= 0x07A6 && wc <= 0x07B0)   return 0;
    if (wc >= 0x0901 && wc <= 0x0903)   return 0;
    if (wc == 0x093C)                   return 0;
    if (wc >= 0x093E && wc <= 0x094D)   return 0;
    if (wc >= 0x0951 && wc <= 0x0954)   return 0;
    if (wc >= 0x0962 && wc <= 0x0963)   return 0;
    if (wc >= 0x0981 && wc <= 0x0983)   return 0;
    if (wc == 0x09BC)                   return 0;
    if (wc >= 0x09BE && wc <= 0x09C4)   return 0;
    if (wc >= 0x09C7 && wc <= 0x09C8)   return 0;
    if (wc >= 0x09CB && wc <= 0x09CD)   return 0;
    if (wc == 0x09D7)                   return 0;
    if (wc >= 0x09E2 && wc <= 0x09E3)   return 0;
    if (wc >= 0x0A01 && wc <= 0x0A03)   return 0;
    if (wc == 0x0A3C)                   return 0;
    if (wc >= 0x0A3E && wc <= 0x0A42)   return 0;
    if (wc >= 0x0A47 && wc <= 0x0A48)   return 0;
    if (wc >= 0x0A4B && wc <= 0x0A4D)   return 0;
    if (wc >= 0x0A70 && wc <= 0x0A71)   return 0;
    if (wc >= 0x0A81 && wc <= 0x0A83)   return 0;
    if (wc == 0x0ABC)                   return 0;
    if (wc >= 0x0ABE && wc <= 0x0AC5)   return 0;
    if (wc >= 0x0AC7 && wc <= 0x0AC9)   return 0;
    if (wc >= 0x0ACB && wc <= 0x0ACD)   return 0;
    if (wc >= 0x0AE2 && wc <= 0x0AE3)   return 0;
    if (wc >= 0x0B01 && wc <= 0x0B03)   return 0;
    if (wc == 0x0B3C)                   return 0;
    if (wc >= 0x0B3E && wc <= 0x0B43)   return 0;
    if (wc >= 0x0B47 && wc <= 0x0B48)   return 0;
    if (wc >= 0x0B4B && wc <= 0x0B4D)   return 0;
    if (wc >= 0x0B56 && wc <= 0x0B57)   return 0;
    if (wc >= 0x0B82 && wc <= 0x0B83)   return 0;
    if (wc >= 0x0BBE && wc <= 0x0BC2)   return 0;
    if (wc >= 0x0BC6 && wc <= 0x0BC8)   return 0;
    if (wc >= 0x0BCA && wc <= 0x0BCD)   return 0;
    if (wc == 0x0BD7)                   return 0;
    if (wc >= 0x0C01 && wc <= 0x0C03)   return 0;
    if (wc >= 0x0C3E && wc <= 0x0C44)   return 0;
    if (wc >= 0x0C46 && wc <= 0x0C48)   return 0;
    if (wc >= 0x0C4A && wc <= 0x0C4D)   return 0;
    if (wc >= 0x0C55 && wc <= 0x0C56)   return 0;
    if (wc >= 0x0C82 && wc <= 0x0C83)   return 0;
    if (wc >= 0x0CBE && wc <= 0x0CC4)   return 0;
    if (wc >= 0x0CC6 && wc <= 0x0CC8)   return 0;
    if (wc >= 0x0CCA && wc <= 0x0CCD)   return 0;
    if (wc >= 0x0CD5 && wc <= 0x0CD6)   return 0;
    if (wc >= 0x0D02 && wc <= 0x0D03)   return 0;
    if (wc >= 0x0D3E && wc <= 0x0D43)   return 0;
    if (wc >= 0x0D46 && wc <= 0x0D48)   return 0;
    if (wc >= 0x0D4A && wc <= 0x0D4D)   return 0;
    if (wc == 0x0D57)                   return 0;
    if (wc == 0x0E31)                   return 0;
    if (wc >= 0x0E34 && wc <= 0x0E3A)   return 0;
    if (wc >= 0x0E47 && wc <= 0x0E4E)   return 0;
    if (wc == 0x0EB1)                   return 0;
    if (wc >= 0x0EB4 && wc <= 0x0EB9)   return 0;
    if (wc >= 0x0EBB && wc <= 0x0EBC)   return 0;
    if (wc >= 0x0EC8 && wc <= 0x0ECD)   return 0;
    if (wc >= 0x0F18 && wc <= 0x0F19)   return 0;
    if (wc == 0x0F35)                   return 0;
    if (wc == 0x0F37)                   return 0;
    if (wc == 0x0F39)                   return 0;
    if (wc >= 0x0F3E && wc <= 0x0F3F)   return 0;
    if (wc >= 0x0F71 && wc <= 0x0F84)   return 0;
    if (wc >= 0x0F86 && wc <= 0x0F87)   return 0;
    if (wc >= 0x0F90 && wc <= 0x0F97)   return 0;
    if (wc >= 0x0F99 && wc <= 0x0FBC)   return 0;
    if (wc == 0x0FC6)                   return 0;
    if (wc >= 0x102D && wc <= 0x1030)   return 0;
    if (wc >= 0x1032 && wc <= 0x1037)   return 0;
    if (wc >= 0x1039 && wc <= 0x103A)   return 0;
    if (wc >= 0x103D && wc <= 0x103E)   return 0;
    if (wc >= 0x1058 && wc <= 0x1059)   return 0;
    if (wc >= 0x105E && wc <= 0x1060)   return 0;
    if (wc >= 0x1071 && wc <= 0x1074)   return 0;
    if (wc == 0x1082)                   return 0;
    if (wc >= 0x1085 && wc <= 0x1086)   return 0;
    if (wc == 0x108D)                   return 0;
    if (wc == 0x109D)                   return 0;
    if (wc >= 0x1100 && wc <= 0x1159)   return 2; /* Hangul Jamo */
    if (wc >= 0x115F && wc <= 0x1160)   return 0; /* filler */
    if (wc >= 0x1161 && wc <= 0x11A2)   return 2;
    if (wc >= 0x11A8 && wc <= 0x11F9)   return 2;
    if (wc >= 0x135F && wc <= 0x1360)   return 0; /* Ethiopic combining */
    if (wc >= 0x1712 && wc <= 0x1714)   return 0;
    if (wc >= 0x1732 && wc <= 0x1734)   return 0;
    if (wc >= 0x1752 && wc <= 0x1753)   return 0;
    if (wc >= 0x1772 && wc <= 0x1773)   return 0;
    if (wc >= 0x17B4 && wc <= 0x17C8)   return 0; /* Khmer vowel signs */
    if (wc >= 0x17CD && wc <= 0x17D1)   return 0;
    if (wc >= 0x17D3 && wc <= 0x17D3)   return 0;
    if (wc >= 0x17DD && wc <= 0x17E0)   return 0;
    if (wc >= 0x180B && wc <= 0x180D)   return 0; /* Mongolian variation selectors */
    if (wc >= 0x18A9 && wc <= 0x18A9)   return 0;
    if (wc >= 0x200B && wc <= 0x200F)   return 0; /* zero-width spaces & direction marks */
    if (wc >= 0x2028 && wc <= 0x2029)   return 0; /* line/paragraph separator */
    if (wc >= 0x202A && wc <= 0x202E)   return 0; /* bidi control chars */
    if (wc >= 0x2060 && wc <= 0x2063)   return 0; /* word joiner, invisible operators */
    if (wc >= 0x206A && wc <= 0x206F)   return 0; /* deprecated formatting */
    if (wc == 0x20D0)                   return 0; /* combining marks for symbols */
    if (wc >= 0x20D1 && wc <= 0x20E4)   return 0;
    if (wc >= 0x20E5 && wc <= 0x20F0)   return 0;
    if (wc >= 0x2329 && wc <= 0x232A)   return 2; /* CJK angle brackets */
    /* CJK Unified Ideographs */
    if ((wc >= 0x2E80 && wc <= 0x2FFF)   ||
        (wc >= 0x3000 && wc <= 0x303E)   ||
        (wc >= 0x3041 && wc <= 0x33FF)   ||
        (wc >= 0x3400 && wc <= 0x4DB5)   ||
        (wc >= 0x4E00 && wc <= 0x9FBB)   ||
        (wc >= 0xA000 && wc <= 0xA48F)   ||
        (wc >= 0xA490 && wc <= 0xA4CF)   ||
        (wc >= 0xAC00 && wc <= 0xD7A3)   ||
        (wc >= 0xF900 && wc <= 0xFAFF)   ||
        (wc >= 0xFE10 && wc <= 0xFE19)   ||
        (wc >= 0xFE30 && wc <= 0xFE6F)   ||
        (wc >= 0xFF01 && wc <= 0xFF60)   ||
        (wc >= 0xFFE0 && wc <= 0xFFE6))
        return 2;

    /* CJK Compatibility Ideographs Supplement */
    if (wc >= 0x20000 && wc <= 0x2FFFD)
        return 2;
    if (wc >= 0x30000 && wc <= 0x3FFFD)
        return 2;
    if (wc >= 0x1F300 && wc <= 0x1F9FF)  /* emoji / misc symbols */
        return 2;

    return 1;
}
#else
/* POSIX: use libc wcwidth */
static inline int compat_wcwidth(wchar_t wc) { return wcwidth(wc); }
#endif
