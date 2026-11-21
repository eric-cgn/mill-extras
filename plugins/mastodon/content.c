/* content.c: a post's HTML as HTML 3.2 (content.h) */
#include <string.h>

#include "content.h"
#include "json.h"

enum { C_TEXT, C_LT, C_TAG, C_ENTITY };

/* Elements that stay open, with their HTML 3.2 names */
enum { E_NONE, E_A, E_B, E_I, E_CODE, E_PRE, E_QUOTE, E_UL, E_OL, E_STRIKE, E_SCRIPT };

/* span flags */
enum { SPAN_HIDE = 1, SPAN_ELLIPSIS = 2 };

static const char *html_name(int e)
{
    switch (e) {
    case E_A: return "A";
    case E_B: return "B";
    case E_I: return "I";
    case E_CODE: return "CODE";
    case E_PRE: return "PRE";
    case E_QUOTE: return "BLOCKQUOTE";
    case E_UL: return "UL";
    case E_OL: return "OL";
    case E_STRIKE: return "STRIKE";
    }
    return "";
}

void content_begin(Content *c, char *buf, long cap, const char *host, void *ref, ContentToMac to_mac)
{
    memset(c, 0, sizeof *c);
    c->out = buf;
    c->cap = cap;
    c->host = host;
    c->ref = ref;
    c->to_mac = to_mac;
    c->block = 1;
    if (cap > 0) buf[0] = 0;
}

/* n bytes of output, if they fit (leaving the reserve, unless closing) */
static int emit(Content *c, const char *p, long n, int closing)
{
    long room = c->cap - 1 - (closing ? 0 : CONTENT_RESERVE);
    if (c->cut && !closing) return 0;
    if (c->len + n > room) { c->cut = 1; return 0; }
    memcpy(c->out + c->len, p, n);
    c->len += n;
    c->out[c->len] = 0;
    return 1;
}

static int emits(Content *c, const char *s) { return emit(c, s, strlen(s), 0); }

/* Mac Roman text, with < > & made entities */
static void emit_mac(Content *c, const char *p, long n)
{
    long i, s = 0;
    c->block = 0;
    for (i = 0; i < n && !c->seen; i++) if (p[i] != ' ' && p[i] != '\t' && p[i] != '\n' && p[i] != '\r') c->seen = 1;
    for (i = 0; i < n; i++) {
        const char *e = p[i] == '&' ? "&amp;" : p[i] == '<' ? "&lt;" : p[i] == '>' ? "&gt;" : NULL;
        if (!e) continue;
        if (i > s && !emit(c, p + s, i - s, 0)) return;
        if (!emits(c, e)) return;
        s = i + 1;
    }
    if (i > s) emit(c, p + s, i - s, 0);
}

/* The UTF-8 text waiting, as Mac Roman */
static void flush_text(Content *c)
{
    char mac[80];
    long n;
    if (!c->textlen) return;
    n = c->hide ? 0 : c->to_mac(c->ref, c->text, c->textlen, mac, sizeof mac);
    c->textlen = 0;
    if (n > 0) emit_mac(c, mac, n);
}

/* One byte of text (UTF-8), cut only between characters */
static void text_byte(Content *c, int b)
{
    if (c->hide) return;
    if ((b & 0xC0) != 0x80 && c->textlen >= 60) flush_text(c);
    if (c->textlen < (short)sizeof c->text) c->text[c->textlen++] = (char)b;
}

static void text_bytes(Content *c, const char *p, long n)
{
    long i;
    for (i = 0; i < n; i++) text_byte(c, (unsigned char)p[i]);
}

static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }

/* An attribute's value in the tag's text (name in lower case): its start and length, or
   NULL. Values may be quoted with " or ', or not. */
static const char *attr(const char *t, long len, const char *name, long *vlen)
{
    long i = 0, nl = strlen(name);
    /* past the element's name */
    while (i < len && t[i] != ' ' && t[i] != '\t' && t[i] != '\n' && t[i] != '\r' && t[i] != '/') i++;
    while (i < len) {
        long a, an;
        char q;
        while (i < len && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r' || t[i] == '/')) i++;
        a = i;
        while (i < len && t[i] != '=' && t[i] != ' ' && t[i] != '\t' && t[i] != '\n' && t[i] != '\r' && t[i] != '/') i++;
        an = i - a;
        while (i < len && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
        if (i < len && t[i] == '=') {
            long v;
            i++;
            while (i < len && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
            q = i < len && (t[i] == '"' || t[i] == '\'') ? t[i++] : 0;
            v = i;
            while (i < len && (q ? t[i] != q : (t[i] != ' ' && t[i] != '\t' && t[i] != '\n' && t[i] != '\r'))) i++;
            if (an == nl) {
                long k;
                for (k = 0; k < nl && lower(t[a + k]) == name[k]; k++) {}
                if (k == nl) { *vlen = i - v; return t + v; }
            }
            if (q && i < len) i++;
        } else if (an == 0) i++;
    }
    return NULL;
}

/* Whether a class attribute names cls */
static int has_class(const char *v, long n, const char *cls)
{
    long i = 0, cl = strlen(cls);
    while (i < n) {
        long s;
        while (i < n && v[i] == ' ') i++;
        s = i;
        while (i < n && v[i] != ' ') i++;
        if (i - s == cl && !memcmp(v + s, cls, cl)) return 1;
    }
    return 0;
}

/* An attribute's value with its entities read (only &amp; matters in a URL) into out */
static long unescape_attr(const char *v, long n, char *out, long size)
{
    long i, o = 0;
    for (i = 0; i < n && o < size - 1; i++) {
        if (v[i] == '&' && i + 4 < n + 1 && !memcmp(v + i, "&amp;", n - i >= 5 ? 5 : n - i) && n - i >= 5) {
            out[o++] = '&';
            i += 4;
        } else out[o++] = v[i];
    }
    out[o] = 0;
    return i < n ? -1 : o;
}

/* A URL into the page, quoted for an attribute: < > " & as entities, and bytes outside
   printable ASCII as %XX */
static void emit_url(Content *c, const char *u)
{
    for (; *u; u++) {
        unsigned char b = (unsigned char)*u;
        char hex[4];
        const char *e = b == '&' ? "&amp;" : b == '"' ? "&quot;" : b == '<' ? "&lt;" : b == '>' ? "&gt;" : NULL;
        if (e) { emits(c, e); continue; }
        if (b <= ' ' || b >= 0x7F) {
            hex[0] = '%';
            hex[1] = "0123456789ABCDEF"[b >> 4];
            hex[2] = "0123456789ABCDEF"[b & 15];
            emit(c, hex, 3, 0);
            continue;
        }
        emit(c, (const char *)&b, 1, 0);
    }
}

/* The host of an http(s) URL, and where its path starts */
static const char *url_host(const char *u, long *hl)
{
    const char *h, *e;
    if (!strncmp(u, "https://", 8)) h = u + 8;
    else if (!strncmp(u, "http://", 7)) h = u + 7;
    else return NULL;
    e = h + strcspn(h, "/?#:");
    *hl = e - h;
    return h;
}

/* A user name or a tag, as an address can carry it */
static int name_ok(const char *s, long n, int tag)
{
    long i;
    if (n <= 0 || n > 64) return 0;
    for (i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        if (!((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '_' ||
              (!tag && (b == '.' || b == '-')) || (tag && (b == '%' || b >= 0x80))))
            return 0;
    }
    return 1;
}

/* An A's link: a hashtag or a mention to this Plug-In's own pages, a web link as it is */
static int open_link(Content *c, const char *t, long len)
{
    long hl, cl = 0, n;
    char href[300];
    const char *h = attr(t, len, "href", &hl), *cls = attr(t, len, "class", &cl), *host;
    if (!h || unescape_attr(h, hl, href, sizeof href) < 0) return 0;
    if (cls && (host = url_host(href, &n)) != NULL && c->host) {
        const char *path = host + n + strcspn(host + n, "/?#");
        long pl = strcspn(path, "?#");
        if (has_class(cls, cl, "hashtag")) {
            const char *tg = path;
            long k;
            for (k = 0; k + 6 <= pl && memcmp(path + k, "/tags/", 6); k++) {}
            if (k + 6 <= pl) {
                tg = path + k + 6;
                pl = strcspn(tg, "/?#");
                if (name_ok(tg, pl, 1)) {
                    href[0] = 0;
                    emits(c, "<A HREF=\"mastodon://");
                    emit_url(c, c->host);
                    emits(c, "/tags/");
                    memmove(href, tg, pl);
                    href[pl] = 0;
                    emit_url(c, href);
                    emits(c, "\">");
                    return 1;
                }
            }
        } else if (has_class(cls, cl, "mention")) {
            /* https://host/@name, https://host/users/name, or https://host/name */
            const char *seg = path;
            long sl = 0, k;
            char who[160];
            for (k = 0; k < pl; k++)
                if (path[k] == '/' && k + 1 < pl) { seg = path + k + 1; sl = strcspn(seg, "/?#"); }
            if (sl > 0 && *seg == '@') { seg++; sl--; }
            if (name_ok(seg, sl, 0) && n < 100) {
                memcpy(who, seg, sl);
                who[sl] = 0;
                emits(c, "<A HREF=\"mastodon://");
                emit_url(c, c->host);
                emits(c, "/@");
                emit_url(c, who);
                if ((long)strlen(c->host) != n || memcmp(c->host, host, n)) {
                    memcpy(who, host, n);
                    who[n] = 0;
                    emits(c, "@");
                    emit_url(c, who);
                }
                emits(c, "\">");
                return 1;
            }
        }
    }
    if (strncmp(href, "http://", 7) && strncmp(href, "https://", 8) && strncmp(href, "mailto:", 7)) return 0;
    emits(c, "<A HREF=\"");
    emit_url(c, href);
    emits(c, "\">");
    return 1;
}

static void push(Content *c, int e)
{
    if (c->nopen < CONTENT_OPEN) c->open[c->nopen++] = (unsigned char)e;
}

/* Close what's open down to e (if it's open) */
static void close_to(Content *c, int e)
{
    short k;
    for (k = c->nopen - 1; k >= 0 && c->open[k] != e; k--) {}
    if (k < 0) return;
    while (c->nopen > k) {
        int x = c->open[--c->nopen];
        if (x == E_SCRIPT) { c->hide--; continue; }
        emit(c, "</", 2, 1);
        emit(c, html_name(x), strlen(html_name(x)), 1);
        emit(c, ">", 1, 1);
        if (x == E_QUOTE || x == E_UL || x == E_OL || x == E_PRE) c->block = 1;
    }
}

static int is_open(Content *c, int e)
{
    short k;
    for (k = 0; k < c->nopen; k++) if (c->open[k] == e) return 1;
    return 0;
}

static void tag(Content *c)
{
    const char *t = c->tag;
    long len = c->taglen, i = 0, nl = 0;
    int closing = 0, e = E_NONE;
    char name[12];
    if (len > 0 && t[0] == '/') { closing = 1; i = 1; }
    while (i < len && nl < 11 && ((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= '0' && t[i] <= '9')))
        name[nl++] = (char)lower(t[i++]);
    name[nl] = 0;
    if (!nl) return;    /* a comment, a doctype */
    if (!strcmp(name, "b") || !strcmp(name, "strong")) e = E_B;
    else if (!strcmp(name, "i") || !strcmp(name, "em") || !strcmp(name, "cite")) e = E_I;
    else if (!strcmp(name, "code")) e = E_CODE;
    else if (!strcmp(name, "pre")) e = E_PRE;
    else if (!strcmp(name, "blockquote")) e = E_QUOTE;
    else if (!strcmp(name, "ul")) e = E_UL;
    else if (!strcmp(name, "ol")) e = E_OL;
    else if (!strcmp(name, "del") || !strcmp(name, "s") || !strcmp(name, "strike")) e = E_STRIKE;
    else if (!strcmp(name, "script") || !strcmp(name, "style")) e = E_SCRIPT;
    else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        /* a heading: a bold paragraph */
        if (!closing) { if (!c->hide && !c->block) emits(c, "<P>"); e = E_B; }
        else e = E_B;
    }
    if (!strcmp(name, "p") || !strcmp(name, "div")) {
        if (!closing && !c->hide && !c->block && emits(c, "<P>")) c->block = 1;
        return;
    }
    if (!strcmp(name, "br")) { if (!c->hide) emits(c, "<BR>"); return; }
    if (!strcmp(name, "li")) { if (!closing && !c->hide && emits(c, "<LI>")) c->block = 1; return; }
    if (!strcmp(name, "img")) {
        long al;
        const char *alt = attr(t, len, "alt", &al);
        if (alt && !closing) text_bytes(c, alt, al);
        return;
    }
    if (!strcmp(name, "span")) {
        long cl = 0;
        const char *cls = closing ? NULL : attr(t, len, "class", &cl);
        if (closing) {
            int f;
            if (c->spanover) { c->spanover--; return; }
            if (!c->nspans) return;
            f = c->spans[--c->nspans];
            if (f & SPAN_HIDE) c->hide--;
            if ((f & SPAN_ELLIPSIS) && !c->hide) text_bytes(c, "\xE2\x80\xA6", 3);
            return;
        }
        if (c->nspans >= CONTENT_SPANS) { c->spanover++; return; }
        c->spans[c->nspans] = (unsigned char)((cls && has_class(cls, cl, "invisible") ? SPAN_HIDE : 0) |
                                              (cls && has_class(cls, cl, "ellipsis") ? SPAN_ELLIPSIS : 0));
        if (c->spans[c->nspans++] & SPAN_HIDE) c->hide++;
        return;
    }
    if (!strcmp(name, "a")) {
        if (closing) { close_to(c, E_A); return; }
        if (is_open(c, E_A)) close_to(c, E_A);
        if (c->hide || c->nopen >= CONTENT_OPEN) return;
        if (open_link(c, t, len)) push(c, E_A);
        return;
    }
    if (e == E_NONE) return;
    if (closing) { close_to(c, e); return; }
    if (c->nopen >= CONTENT_OPEN) return;
    if (e == E_SCRIPT) { c->hide++; push(c, e); return; }
    if (c->hide) return;
    if (emits(c, "<")) {
        emits(c, html_name(e));
        emits(c, ">");
        push(c, e);
        if (e == E_QUOTE || e == E_UL || e == E_OL || e == E_PRE) c->block = 1;
    }
}

/* An entity's name or number (without & and ;): its character, or -1 */
static long entity(const char *s)
{
    long u = 0;
    if (s[0] == '#') {
        int hex = s[1] == 'x' || s[1] == 'X';
        const char *p = s + 1 + hex;
        if (!*p) return -1;
        for (; *p; p++) {
            int d = *p >= '0' && *p <= '9' ? *p - '0' : hex && lower(*p) >= 'a' && lower(*p) <= 'f' ? lower(*p) - 'a' + 10 : -1;
            if (d < 0) return -1;
            u = u * (hex ? 16 : 10) + d;
            if (u > 0x10FFFF) return 0xFFFD;
        }
        return u == 0 || (u >= 0xD800 && u <= 0xDFFF) ? 0xFFFD : u;
    }
    if (!strcmp(s, "amp")) return '&';
    if (!strcmp(s, "lt")) return '<';
    if (!strcmp(s, "gt")) return '>';
    if (!strcmp(s, "quot")) return '"';
    if (!strcmp(s, "apos")) return '\'';
    if (!strcmp(s, "nbsp")) return 0xA0;
    return -1;
}

static void byte(Content *c, int b)
{
    switch (c->state) {
    case C_TEXT:
        if (b == '<') { c->state = C_LT; return; }
        if (b == '&') { c->state = C_ENTITY; c->entlen = 0; return; }
        text_byte(c, b);
        return;
    case C_LT:
        /* a tag starts with a letter, / or !; else the < is text */
        c->state = C_TEXT;
        if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || b == '/' || b == '!') {
            flush_text(c);
            c->state = C_TAG;
            c->taglen = 0;
            c->quote = 0;
            byte(c, b);
            return;
        }
        text_byte(c, '<');
        byte(c, b);
        return;
    case C_TAG:
        if (c->quote) { if (b == c->quote) c->quote = 0; }
        else if ((b == '"' || b == '\'') && c->taglen > 0 && c->tag[c->taglen - 1] == '=') c->quote = (char)b;
        else if (b == '>') {
            c->tag[c->taglen < CONTENT_TAG ? c->taglen : CONTENT_TAG - 1] = 0;
            tag(c);
            c->state = C_TEXT;
            return;
        }
        if (c->taglen < CONTENT_TAG - 1) c->tag[c->taglen++] = (char)b;
        return;
    case C_ENTITY:
        if (b == ';' || !((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '#') ||
            c->entlen >= (short)sizeof c->ent - 1) {
            long u;
            c->ent[c->entlen] = 0;
            c->state = C_TEXT;
            if (b == ';' && (u = entity(c->ent)) >= 0) {
                char utf[4];
                text_bytes(c, utf, json_utf8(u, utf));
                return;
            }
            /* not one: the text as it was */
            text_byte(c, '&');
            text_bytes(c, c->ent, c->entlen);
            byte(c, b);
            return;
        }
        c->ent[c->entlen++] = (char)b;
        return;
    }
}

void content_data(Content *c, const char *p, long n)
{
    long i;
    for (i = 0; i < n; i++) byte(c, (unsigned char)p[i]);
}

void content_end(Content *c)
{
    if (c->state == C_LT) text_byte(c, '<');
    if (c->state == C_ENTITY) {
        c->state = C_TEXT;
        text_byte(c, '&');
        text_bytes(c, c->ent, c->entlen);
    }
    c->state = C_TEXT;
    flush_text(c);
    while (c->nopen > 0) close_to(c, c->open[c->nopen - 1]);
    c->hide = 0;
}
