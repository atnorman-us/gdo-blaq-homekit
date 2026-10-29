// form_parse.h
//
// Minimal application/x-www-form-urlencoded lookup, header-only so it can be
// compiled by the host tests without any ESP-IDF headers.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline int form_parse_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Finds `key` (whole-name match) in `body` and copies its percent-decoded
// value into `out`. Returns false if the key is missing, the value does not
// fit in out_len (including the terminator), or it decodes to a NUL byte.
// '+' decodes to ' '. An invalid %-escape is kept literally.
static inline bool form_get_param(const char *body, const char *key, char *out, size_t out_len)
{
    if (!body || !key || !out || out_len == 0) return false;
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t o = 0;
            while (*p && *p != '&') {
                char c = *p;
                int hi, lo;
                if (c == '%' && (hi = form_parse_hexval(p[1])) >= 0 &&
                    (lo = form_parse_hexval(p[2])) >= 0) {
                    c = (char)(hi * 16 + lo);
                    p += 2;
                } else if (c == '+') {
                    c = ' ';
                }
                if (c == '\0') return false;
                if (o + 1 >= out_len) return false;
                out[o++] = c;
                p++;
            }
            out[o] = '\0';
            return true;
        }
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}
