// request_origin.h
//
// Decides whether an HTTP request to the diagnostics server came from this
// device's own page, using only the Host, Origin and Sec-Fetch-Site headers.
// Header-only and free of ESP-IDF types so tests/test_request_origin.py can
// compile it on the host.
//
// Why: /admin/set-password is deliberately unauthenticated (it is how the
// first password gets set) and takes a text/plain body, which browsers send
// cross-origin without a preflight. Any web page open on a device on the LAN
// could therefore claim a brand-new blaQ, and with it firmware install. The
// other mutating endpoints require the X-GDO-Token header, which forces a
// preflight the device never answers, so they are not reachable this way.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline bool request_origin_ieq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// Length of the host part of an authority ("host", "host:port",
// "[v6]:port"), ignoring one trailing '.' on a name.
static inline size_t request_origin_host_len(const char *authority)
{
    size_t n = strlen(authority);
    if (n && authority[0] == '[') {
        const char *close = strchr(authority, ']');
        return close ? (size_t)(close - authority) + 1 : 0;
    }
    const char *colon = strchr(authority, ':');
    if (colon) n = (size_t)(colon - authority);
    if (n && authority[n - 1] == '.') n--;
    return n;
}

// True for a dotted-quad IPv4 literal such as 192.168.1.168.
static inline bool request_origin_is_ipv4(const char *s, size_t n)
{
    int groups = 0, digits = 0, value = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (++digits > 3) return false;
            value = value * 10 + (c - '0');
            if (value > 255) return false;
        } else if (c == '.') {
            if (!digits) return false;
            groups++; digits = 0; value = 0;
        } else {
            return false;
        }
    }
    return groups == 3 && digits > 0;
}

// The Host header must name this device in a way a DNS-rebinding page
// cannot: an IP literal, or the mDNS name (anything under .local, which only
// resolves on the local link). A public DNS name pointed at a LAN address
// would otherwise pass the Origin check below with a matching Host.
static inline bool request_host_is_local(const char *host)
{
    if (!host) return false;
    size_t n = request_origin_host_len(host);
    if (n == 0) return false;
    if (host[0] == '[') return true;                       // IPv6 literal
    if (request_origin_is_ipv4(host, n)) return true;
    static const char suffix[] = ".local";
    const size_t sl = sizeof(suffix) - 1;
    return n > sl && request_origin_ieq(host + n - sl, suffix, sl);
}

// Origin is "scheme://authority" with no path. It must name exactly the
// authority the request was sent to (same host and port, case-insensitive).
// "null" (sandboxed frames, file:// pages) never matches.
static inline bool request_origin_matches_host(const char *origin, const char *host)
{
    if (!origin || !host || !*host) return false;
    const char *sep = strstr(origin, "://");
    if (!sep) return false;
    const char *authority = sep + 3;
    size_t n = strlen(authority);
    if (n && authority[n - 1] == '/') n--;
    size_t hn = strlen(host);
    return n == hn && n > 0 && request_origin_ieq(authority, host, n);
}

// origin and sec_fetch_site may be NULL or "" when the header was absent
// (non-browser clients such as curl send neither; browsers send at least
// one of them on any cross-site POST).
static inline bool request_from_own_page(const char *host, const char *origin,
                                         const char *sec_fetch_site)
{
    if (!request_host_is_local(host)) return false;
    if (origin && *origin && !request_origin_matches_host(origin, host)) return false;
    if (sec_fetch_site && *sec_fetch_site &&
        strcmp(sec_fetch_site, "same-origin") != 0 && strcmp(sec_fetch_site, "none") != 0) {
        return false;
    }
    return true;
}
