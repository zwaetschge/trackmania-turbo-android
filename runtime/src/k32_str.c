/*
 * k32_str.c - code pages, locale and string APIs (kernel32 + a few user32).
 * The guest runs with ACP/OEMCP 1252 / 437 and locale en-US (0x0409).
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

static const uint16_t cp1252_hi[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178 };

static uint16_t b2w_1252(uint8_t b) { return b >= 0x80 && b < 0xA0 ? cp1252_hi[b - 0x80] : b; }
static int w2b_1252(uint16_t w) {
    if (w < 0x80 || (w >= 0xA0 && w < 0x100)) return w;
    for (int i = 0; i < 32; i++) if (cp1252_hi[i] == w) return 0x80 + i;
    return -1;
}

#define CP_ACP 0
#define CP_OEMCP 1
#define CP_UTF8 65001

static void shim_GetACP(void) { RET(1252, 0); }
static void shim_GetOEMCP(void) { RET(437, 0); }
static void shim_AreFileApisANSI(void) { RET(1, 0); }
static void shim_IsValidCodePage(void) { uint32_t c = ARG(0); RET(c == 1252 || c == 437 || c == 65001 || c == 20127 || c == 28591, 1); }
static void shim_GetCPInfo(void) {
    uint32_t cp = ARG(0), p = ARG(1);
    memset(G2H(p), 0, 20);
    wr32(p, cp == 65001 ? 4 : 1);   /* MaxCharSize */
    wr8(p + 4, '?');                 /* DefaultChar */
    RET(1, 2);
}

/* MultiByteToWideChar(cp, flags, src, srclen, dst, dstlen) */
static void shim_MultiByteToWideChar(void) {
    uint32_t cp = ARG(0), src = ARG(2), dst = ARG(4);
    int32_t slen = (int32_t)ARG(3), dlen = (int32_t)ARG(5);
    if (slen < 0) slen = (int32_t)strlen((const char *)G2H(src)) + 1;
    const uint8_t *s = G2H(src);
    int n = 0;
    uint16_t tmp;
    for (int32_t i = 0; i < slen;) {
        uint32_t c;
        if (cp == CP_UTF8) {
            c = s[i++];
            if (c >= 0xF0 && i + 2 < slen + 0) { c = ((c & 7) << 18) | ((s[i] & 0x3F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 3; }
            else if (c >= 0xE0 && i + 1 < slen) { c = ((c & 15) << 12) | ((s[i] & 0x3F) << 6) | (s[i + 1] & 0x3F); i += 2; }
            else if (c >= 0xC0 && i < slen) { c = ((c & 31) << 6) | (s[i] & 0x3F); i += 1; }
            else if (c >= 0x80) c = 0xFFFD;
        } else {
            c = b2w_1252(s[i++]);
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            if (dlen) { if (n + 2 > dlen) { tm_set_last_error(122); RET(0, 6); } wr16(dst + 2 * n, (uint16_t)(0xD800 + (c >> 10))); wr16(dst + 2 * n + 2, (uint16_t)(0xDC00 + (c & 0x3FF))); }
            n += 2;
        } else {
            tmp = (uint16_t)c;
            if (dlen) { if (n + 1 > dlen) { tm_set_last_error(122); RET(0, 6); } wr16(dst + 2 * n, tmp); }
            n++;
        }
    }
    RET(n, 6);
}

/* WideCharToMultiByte(cp, flags, src, srclen, dst, dstlen, defchar, useddef) */
static void shim_WideCharToMultiByte(void) {
    uint32_t cp = ARG(0), src = ARG(2), dst = ARG(4), useddef = ARG(7);
    int32_t slen = (int32_t)ARG(3), dlen = (int32_t)ARG(5);
    if (slen < 0) slen = (int32_t)tm_wcslen(src) + 1;
    int n = 0, used = 0;
    for (int32_t i = 0; i < slen; i++) {
        uint32_t c = rd16(src + 2 * i);
        uint8_t out[4]; int k;
        if (cp == CP_UTF8) {
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < slen) {
                uint32_t d = rd16(src + 2 * i + 2);
                if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
            }
            if (c < 0x80) { out[0] = (uint8_t)c; k = 1; }
            else if (c < 0x800) { out[0] = (uint8_t)(0xC0 | (c >> 6)); out[1] = (uint8_t)(0x80 | (c & 0x3F)); k = 2; }
            else if (c < 0x10000) { out[0] = (uint8_t)(0xE0 | (c >> 12)); out[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); out[2] = (uint8_t)(0x80 | (c & 0x3F)); k = 3; }
            else { out[0] = (uint8_t)(0xF0 | (c >> 18)); out[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F)); out[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); out[3] = (uint8_t)(0x80 | (c & 0x3F)); k = 4; }
        } else {
            int b = w2b_1252((uint16_t)c);
            if (b < 0) { b = ARG(6) ? rd8(ARG(6)) : '?'; used = 1; }
            out[0] = (uint8_t)b; k = 1;
        }
        if (dlen) {
            if (n + k > dlen) { tm_set_last_error(122); RET(0, 8); }
            memcpy(G2H(dst + n), out, (size_t)k);
        }
        n += k;
    }
    if (useddef) wr32(useddef, (uint32_t)used);
    RET(n, 8);
}

/* GetStringTypeW(infotype, src, len, out) - CT_CTYPE1 classification */
static void shim_GetStringTypeW(void) {
    uint32_t type = ARG(0), src = ARG(1), out = ARG(3);
    int32_t len = (int32_t)ARG(2);
    if (len < 0) len = (int32_t)tm_wcslen(src) + 1;
    for (int32_t i = 0; i < len; i++) {
        wint_t c = rd16(src + 2 * i);
        uint16_t t = 0;
        if (type == 1) {
            if (iswupper(c)) t |= 0x1;
            if (iswlower(c)) t |= 0x2;
            if (iswdigit(c)) t |= 0x4;
            if (iswspace(c)) t |= 0x8;
            if (iswpunct(c)) t |= 0x10;
            if (iswcntrl(c)) t |= 0x20;
            if (c == ' ' || c == '\t') t |= 0x40;
            if (iswxdigit(c)) t |= 0x80;
            if (iswalpha(c)) t |= 0x100;
            if (c && !iswcntrl(c)) t |= 0x200; /* C1_DEFINED */
        }
        wr16(out + 2 * i, t);
    }
    RET(1, 4);
}

static uint16_t wupper(uint16_t c) { return (uint16_t)towupper(c); }
static uint16_t wlower(uint16_t c) { return (uint16_t)towlower(c); }

/* LCMapStringW(lcid, flags, src, srclen, dst, dstlen) */
static void shim_LCMapStringW(void) {
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t slen = (int32_t)ARG(3), dlen = (int32_t)ARG(5);
    if (slen < 0) slen = (int32_t)tm_wcslen(src) + 1;
    if (flags & 0x400) { /* LCMAP_SORTKEY: produce a byte key */
        int need = slen * 2 + 1;
        if (!dlen) RET(need, 6);
        if (dlen < need) RET(0, 6);
        int k = 0;
        for (int32_t i = 0; i < slen; i++) { uint16_t c = wupper(rd16(src + 2 * i)); wr8(dst + k++, (uint8_t)(c >> 8)); wr8(dst + k++, (uint8_t)c); }
        wr8(dst + k++, 0);
        RET(k, 6);
    }
    if (!dlen) RET(slen, 6);
    if (dlen < slen) { tm_set_last_error(122); RET(0, 6); }
    for (int32_t i = 0; i < slen; i++) {
        uint16_t c = rd16(src + 2 * i);
        if (flags & 0x200) c = wupper(c);
        else if (flags & 0x100) c = wlower(c);
        wr16(dst + 2 * i, c);
    }
    RET(slen, 6);
}

/* CompareStringW(lcid, flags, s1, n1, s2, n2): 1 <, 2 =, 3 > */
static void shim_CompareStringW(void) {
    uint32_t flags = ARG(1), a = ARG(2), b = ARG(4);
    int32_t na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    if (na < 0) na = (int32_t)tm_wcslen(a);
    if (nb < 0) nb = (int32_t)tm_wcslen(b);
    int ic = flags & 1;
    int32_t i = 0;
    for (; i < na && i < nb; i++) {
        uint16_t x = rd16(a + 2 * i), y = rd16(b + 2 * i);
        if (ic) { x = wlower(x); y = wlower(y); }
        if (x != y) RET(x < y ? 1 : 3, 6);
    }
    RET(na == nb ? 2 : na < nb ? 1 : 3, 6);
}

/* Locale info (en-US) */
static const char *locale_value(uint32_t type) {
    switch (type & 0xFFFF) {
    case 0x1: return "0409";               /* LOCALE_ILANGUAGE */
    case 0x2: return "English (United States)";
    case 0x3: return "ENU";                /* LOCALE_SABBREVLANGNAME */
    case 0x59: return "en";                /* LOCALE_SISO639LANGNAME */
    case 0x5A: return "US";                /* LOCALE_SISO3166CTRYNAME */
    case 0x6: return "United States";
    case 0x7: return "USA";
    case 0xB: return "437";                /* IDEFAULTCODEPAGE */
    case 0x1004: return "1252";            /* IDEFAULTANSICODEPAGE */
    case 0xE: return ".";                  /* SDECIMAL */
    case 0xF: return ",";                  /* STHOUSAND */
    case 0x14: return "$";
    case 0x1D: return "/";                 /* SDATE */
    case 0x1E: return ":";                 /* STIME */
    case 0x1F: return "M/d/yyyy";
    case 0x20: return "dddd, MMMM d, yyyy";
    case 0x1003: return "h:mm:ss tt";
    case 0x5C: return "en-US";             /* LOCALE_SNAME */
    case 0x6F: return "English";
    default: return NULL;
    }
}
static void shim_GetLocaleInfoA(void) {
    uint32_t type = ARG(1), buf = ARG(2), n = ARG(3);
    if (type & 0x20000000u) { /* LOCALE_RETURN_NUMBER */
        if (n < 2) RET(0, 4);
        uint32_t v = (type & 0xFFFF) == 0x1004 ? 1252 : (type & 0xFFFF) == 0xB ? 437 : 0;
        wr32(buf, v); RET(2, 4);
    }
    const char *v = locale_value(type);
    if (!v) { tm_set_last_error(1004); RET(0, 4); }
    uint32_t len = (uint32_t)strlen(v) + 1;
    if (!n) RET(len, 4);
    if (n < len) { tm_set_last_error(122); RET(0, 4); }
    memcpy(G2H(buf), v, len); RET(len, 4);
}
static void shim_GetLocaleInfoW(void) {
    uint32_t type = ARG(1), buf = ARG(2), n = ARG(3);
    if (type & 0x20000000u) {
        if (n < 2) RET(0, 4);
        uint32_t v = (type & 0xFFFF) == 0x1004 ? 1252 : (type & 0xFFFF) == 0xB ? 437 : 0;
        wr32(buf, v); RET(2, 4);
    }
    const char *v = locale_value(type);
    if (!v) { tm_set_last_error(1004); RET(0, 4); }
    uint32_t len = (uint32_t)strlen(v) + 1;
    if (!n) RET(len, 4);
    if (n < len) { tm_set_last_error(122); RET(0, 4); }
    tm_u2w(v, buf, (int)n); RET(len, 4);
}
static void shim_GetUserDefaultLCID(void) { RET(0x0409, 0); }
static void shim_IsValidLocale(void) { RET(ARG(0) == 0x0409 || ARG(0) == 0x0400 || ARG(0) == 0x0800, 2); }
static void shim_EnumSystemLocalesW(void) { RET(1, 2); }
static void shim_GetUserDefaultLocaleName(void) {
    uint32_t buf = ARG(0); int n = (int)ARG(1);
    if (n < 6) RET(0, 2);
    tm_u2w("en-US", buf, n); RET(6, 2);
}

static void shim_CharUpperW(void) {
    uint32_t p = ARG(0);
    if (p < 0x10000) RET(wupper((uint16_t)p), 1);
    for (uint32_t q = p; rd16(q); q += 2) wr16(q, wupper(rd16(q)));
    RET(p, 1);
}
static void shim_CharLowerW(void) {
    uint32_t p = ARG(0);
    if (p < 0x10000) RET(wlower((uint16_t)p), 1);
    for (uint32_t q = p; rd16(q); q += 2) wr16(q, wlower(rd16(q)));
    RET(p, 1);
}

static const tm_shim_def k32_str_shims[] = {
#define K(n) TM_SHIM("kernel32.dll", n)
    K(GetACP), K(GetOEMCP), K(AreFileApisANSI), K(IsValidCodePage), K(GetCPInfo), K(MultiByteToWideChar),
    K(WideCharToMultiByte), K(GetStringTypeW), K(LCMapStringW), K(CompareStringW), K(GetLocaleInfoA),
    K(GetLocaleInfoW), K(GetUserDefaultLCID), K(IsValidLocale), K(EnumSystemLocalesW), K(GetUserDefaultLocaleName),
#undef K
    TM_SHIM("user32.dll", CharUpperW), TM_SHIM("user32.dll", CharLowerW),
};
void tm_k32_str_register(void) { tm_register_shims(k32_str_shims, sizeof k32_str_shims / sizeof k32_str_shims[0]); }
