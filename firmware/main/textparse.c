#include "textparse.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool tp_parse_float(const char *s, float min, float max, float *out)
{
    if (!s) {
        return false;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    // Only digits, sign and decimal point: strtof would also take "nan", "inf", hex floats and the locale's comma
    for (const char *p = s; *p && !isspace((unsigned char)*p); p++) {
        if (!isdigit((unsigned char)*p) && *p != '.' && *p != '-' && *p != '+') {
            return false;
        }
    }
    char *end;
    float v = strtof(s, &end);
    if (end == s) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end || !isfinite(v) || v < min || v > max) {
        return false;
    }
    *out = v;
    return true;
}

static int hexval(char c)
{
    return isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10);
}

void tp_url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            *o++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

void tp_trim(char *s)
{
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) {
        s[--n] = 0;
    }
}

tp_res_t tp_form_get(const char *body, const char *key, char *out, size_t size)
{
    if (size) {
        out[0] = 0;
    }
    size_t klen = strlen(key);
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t vlen = len - klen - 1;
            // Decoding only shortens, but the raw text needs a buffer: decode in place in a copy
            char *tmp = malloc(vlen + 1);
            if (!tmp) {
                return TP_TOO_LONG;
            }
            memcpy(tmp, v, vlen);
            tmp[vlen] = 0;
            tp_url_decode(tmp);
            tp_trim(tmp);
            tp_res_t r = TP_TOO_LONG;
            if (strlen(tmp) < size) {
                strcpy(out, tmp);
                r = TP_OK;
            }
            free(tmp);
            return r;
        }
        if (len == klen && strncmp(p, key, klen) == 0) {  // "key" without '=': present, empty
            return TP_OK;
        }
        p = amp ? amp + 1 : NULL;
    }
    return TP_MISSING;
}

bool tp_copy_fits(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_size) {
        return false;
    }
    memcpy(dst, src, n + 1);
    return true;
}
