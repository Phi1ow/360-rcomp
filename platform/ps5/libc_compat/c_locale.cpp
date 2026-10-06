// C-locale and absent-service functions for PS5 titles (owner: Agent 2,
// platform/). libc++'s locale.cpp (pulled by iostream users such as glslang)
// is built against FreeBSD's xlocale API; neither the title libc module nor
// libSceLibcInternal exports it.
//
// A title never calls setlocale(): the process locale is "C". newlocale()
// therefore only knows "C" / "POSIX" / "" (all the C locale), and every
// *_l(..., locale) function given that locale is exactly its plain version
// under the process locale. Anything else fails the documented way:
//   newlocale(other name)  -> NULL, errno ENOENT (libc++ then reports it)
//   catopen                -> (nl_catd)-1, errno ENOENT (no message catalogs;
//                             catgets returns its default string, as specified)
//   getpwuid_r             -> ENOENT, *result = NULL (no user database)
//   utimensat              -> -1, errno ENOSYS (file times are not settable)
// All definitions are weak: a library of the title that brings its own wins.
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <nl_types.h>
#include <runetype.h>
#include <_ctype.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <xlocale.h>

#define WEAK extern "C" __attribute__((weak))

namespace {
// The one locale object: never dereferenced by the functions below.
struct _xlocale_c {
    int unused;
} g_c_locale;
locale_t c_locale() { return reinterpret_cast<locale_t>(&g_c_locale); }
}  // namespace

// FreeBSD's ctype_l inlines (isalpha_l, toupper_l, ...) read the locale's
// rune table through __runes_for_locale. The C locale's table: ASCII
// classification, low 8 bits = digit value, bytes 128..255 unclassified.
namespace {
_RuneLocale make_c_runes() {
    _RuneLocale r;
    memset(&r, 0, sizeof r);
    memcpy(r.__magic, _RUNE_MAGIC_1, sizeof r.__magic);
    memcpy(r.__encoding, "NONE", 5);
    r.__invalid_rune = 0xFFFD;
    for (int c = 0; c < _CACHED_RUNES; ++c) {
        unsigned long t = 0;
        const bool upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z', digit = c >= '0' && c <= '9';
        if (c < 128) {
            if (c < 32 || c == 127) t |= _CTYPE_C;
            if ((c >= 9 && c <= 13) || c == ' ') t |= _CTYPE_S;
            if (c == '\t' || c == ' ') t |= _CTYPE_B;
            if (c >= 32 && c <= 126) t |= _CTYPE_R | _CTYPE_SW1;
            if (c >= 33 && c <= 126) {
                t |= _CTYPE_G;
                if (!upper && !lower && !digit) t |= _CTYPE_P;
            }
            if (upper) t |= _CTYPE_U | _CTYPE_A;
            if (lower) t |= _CTYPE_L | _CTYPE_A;
            if (digit) t |= _CTYPE_D | _CTYPE_X | _CTYPE_N | (unsigned long)(c - '0');
            if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) t |= _CTYPE_X | (unsigned long)((c | 0x20) - 'a' + 10);
        }
        r.__runetype[c] = t;
        r.__maplower[c] = upper ? c + 32 : c;
        r.__mapupper[c] = lower ? c - 32 : c;
    }
    return r;
}
_RuneLocale& c_runes() {
    static _RuneLocale r = make_c_runes();
    return r;
}
}  // namespace

WEAK _RuneLocale* __runes_for_locale(locale_t, int* mb_sb_limit) {
    if (mb_sb_limit) *mb_sb_limit = _CACHED_RUNES;
    return &c_runes();
}
WEAK unsigned long ___runetype_l(__ct_rune_t c, locale_t) {
    return (c >= 0 && c < _CACHED_RUNES) ? c_runes().__runetype[c] : 0;
}
WEAK __ct_rune_t ___tolower_l(__ct_rune_t c, locale_t) {
    return (c >= 0 && c < _CACHED_RUNES) ? c_runes().__maplower[c] : c;
}
WEAK __ct_rune_t ___toupper_l(__ct_rune_t c, locale_t) {
    return (c >= 0 && c < _CACHED_RUNES) ? c_runes().__mapupper[c] : c;
}
WEAK int ___mb_cur_max_l(locale_t) { return 1; }

WEAK locale_t newlocale(int, const char* name, locale_t) {
    if (name && (!*name || !strcmp(name, "C") || !strcmp(name, "POSIX"))) return c_locale();
    errno = ENOENT;
    return nullptr;
}
WEAK int freelocale(locale_t) { return 0; }  // FreeBSD: int
WEAK struct lconv* localeconv_l(locale_t) { return localeconv(); }

WEAK double strtod_l(const char* s, char** e, locale_t) { return strtod(s, e); }
WEAK float strtof_l(const char* s, char** e, locale_t) { return strtof(s, e); }
WEAK long double strtold_l(const char* s, char** e, locale_t) { return strtold(s, e); }
WEAK long long strtoll_l(const char* s, char** e, int b, locale_t) { return strtoll(s, e, b); }
WEAK unsigned long long strtoull_l(const char* s, char** e, int b, locale_t) { return strtoull(s, e, b); }

WEAK int snprintf_l(char* s, size_t n, locale_t, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}
WEAK int asprintf_l(char** s, locale_t, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vasprintf(s, fmt, ap);
    va_end(ap);
    return r;
}
WEAK int sscanf_l(const char* s, locale_t, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
WEAK size_t strftime_l(char* s, size_t n, const char* fmt, const struct tm* t, locale_t) {
    return strftime(s, n, fmt, t);
}

WEAK int strcoll_l(const char* a, const char* b, locale_t) { return strcoll(a, b); }
WEAK size_t strxfrm_l(char* d, const char* s, size_t n, locale_t) { return strxfrm(d, s, n); }
WEAK int wcscoll_l(const wchar_t* a, const wchar_t* b, locale_t) { return wcscoll(a, b); }
WEAK size_t wcsxfrm_l(wchar_t* d, const wchar_t* s, size_t n, locale_t) { return wcsxfrm(d, s, n); }
WEAK int iswctype_l(wint_t c, wctype_t t, locale_t) { return iswctype(c, t); }
WEAK wint_t btowc_l(int c, locale_t) { return btowc(c); }
WEAK int wctob_l(wint_t c, locale_t) { return wctob(c); }

WEAK size_t mbrtowc_l(wchar_t* w, const char* s, size_t n, mbstate_t* ps, locale_t) { return mbrtowc(w, s, n, ps); }
WEAK size_t mbrlen_l(const char* s, size_t n, mbstate_t* ps, locale_t) { return mbrlen(s, n, ps); }
WEAK int mbtowc_l(wchar_t* w, const char* s, size_t n, locale_t) { return mbtowc(w, s, n); }
WEAK size_t wcrtomb_l(char* s, wchar_t w, mbstate_t* ps, locale_t) { return wcrtomb(s, w, ps); }
WEAK size_t mbsrtowcs_l(wchar_t* d, const char** s, size_t n, mbstate_t* ps, locale_t) {
    return mbsrtowcs(d, s, n, ps);
}

// mbsnrtowcs / wcsnrtombs (POSIX 2008): not exported to titles either.
// Converts at most nms source bytes (resp. nwc wide characters), with the
// same stop rules as mbsrtowcs / wcsrtombs.
WEAK size_t mbsnrtowcs(wchar_t* dst, const char** src, size_t nms, size_t len, mbstate_t* ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    const char* s = *src;
    size_t count = 0;
    while (nms > 0 && (!dst || count < len)) {
        wchar_t wc;
        const size_t r = mbrtowc(&wc, s, nms, ps);
        if (r == (size_t)-1) {
            *src = s;
            return (size_t)-1;
        }
        if (r == (size_t)-2) {  // incomplete character: consumed, nothing written
            s += nms;
            break;
        }
        if (r == 0) {  // terminating NUL
            if (dst) {
                dst[count] = L'\0';
                *src = nullptr;
            }
            return count;
        }
        if (dst) dst[count] = wc;
        s += r;
        nms -= r;
        ++count;
    }
    if (dst) *src = s;
    return count;
}
WEAK size_t mbsnrtowcs_l(wchar_t* d, const char** s, size_t nms, size_t n, mbstate_t* ps, locale_t) {
    return mbsnrtowcs(d, s, nms, n, ps);
}

WEAK size_t wcsnrtombs(char* dst, const wchar_t** src, size_t nwc, size_t len, mbstate_t* ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    const wchar_t* s = *src;
    size_t count = 0;
    char buf[MB_LEN_MAX];
    while (nwc > 0) {
        mbstate_t save = *ps;
        const size_t r = wcrtomb(buf, *s, ps);
        if (r == (size_t)-1) {
            *src = s;
            return (size_t)-1;
        }
        if (dst && count + r > len) {  // does not fit: stop before it
            *ps = save;
            break;
        }
        if (dst) memcpy(dst + count, buf, r);
        if (*s == L'\0') {
            if (dst) *src = nullptr;
            return count + r - 1;
        }
        count += r;
        ++s;
        --nwc;
    }
    if (dst) *src = s;
    return count;
}
WEAK size_t wcsnrtombs_l(char* d, const wchar_t** s, size_t nwc, size_t n, mbstate_t* ps, locale_t) {
    return wcsnrtombs(d, s, nwc, n, ps);
}

WEAK nl_catd catopen(const char*, int) {
    errno = ENOENT;
    return (nl_catd)-1;
}
WEAK char* catgets(nl_catd, int, int, const char* s) { return const_cast<char*>(s); }
WEAK int catclose(nl_catd) {
    errno = EBADF;
    return -1;
}

WEAK int getpwuid_r(uid_t, struct passwd*, char*, size_t, struct passwd** result) {
    *result = nullptr;
    return ENOENT;
}

WEAK int utimensat(int, const char*, const struct timespec*, int) {
    errno = ENOSYS;
    return -1;
}
