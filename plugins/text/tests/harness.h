/* harness.h: a TxSink for host tests: output into a buffer, memory from malloc (counted),
   and ToMacRoman as Mill's own (src/mac_plugin.c's h_to_mac_roman, on src/core/charset.c) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../text.h"
#include "charset.h"

static char *h_out;
static long h_len, h_cap, h_mem, h_limit = -1;

static void *h_alloc(void *ref, long n) { (void)ref; h_mem += n; return malloc(n); }
static void h_release(void *ref, void *p) { (void)ref; free(p); }
static int h_write(void *ref, const char *p, long n)
{
    (void)ref;
    if (h_limit >= 0 && h_len + n > h_limit) return 0;
    if (h_len + n + 1 > h_cap) { h_cap = (h_len + n + 1) * 2; h_out = realloc(h_out, h_cap); }
    memcpy(h_out + h_len, p, n);
    h_len += n;
    h_out[h_len] = 0;
    return 1;
}
static long h_to_mac(void *ref, const char *in, long len, short cs, char *out, long size)
{
    const unsigned char *s = (const unsigned char *)in;
    long i = 0, o = 0;
    (void)ref;
    if (cs != TX_LATIN1 && cs != TX_UTF8) cs = charset_sniff(in, len) == CHARSET_UTF8 ? TX_UTF8 : TX_LATIN1;
    while (i < len) {
        unsigned char c;
        long u;
        int k;
        if (cs == TX_UTF8 && s[i] >= 0x80 && (k = utf8_char(s + i, len - i, &u)) > 0) { c = unicode_to_mac(u); i += k; }
        else c = s[i] < 0x80 ? s[i] : latin1_to_mac(s[i]), i++;
        if (!c) continue;
        if (o + 1 >= size) return -1;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return o;
}

/* in as kind, given chunk bytes at a time (0: all at once): the page, without the META line */
static const char *convert(int kind, short cs, const char *in, long len, long chunk)
{
    TxSink s = { NULL, h_alloc, h_release, h_write, h_to_mac };
    void *t;
    long i, k;
    char *body;
    h_len = 0;
    if (h_out) h_out[0] = 0;
    t = tx_begin(kind, cs, &s);
    if (!t) return NULL;
    if (chunk <= 0) chunk = len ? len : 1;
    for (i = 0; i < len; i += k) {
        k = len - i < chunk ? len - i : chunk;
        if (tx_data(t, in + i, k) != TX_MORE) break;
    }
    tx_end(t);
    if (!h_out) h_write(NULL, "", 0);
    body = strchr(h_out, '\n');
    return body ? body + 1 : h_out;
}
