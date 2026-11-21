/* text.c: a run of the Modern Text Plug-In: its memory, its lines, and its output (see
   text.h). The formats are gemtext.c and markdown.c. */
#include <string.h>

#include "txint.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int named(const char *s, long n, const char *name)
{
    long i;
    if ((long)strlen(name) != n) return 0;
    for (i = 0; i < n; i++) if (lower((unsigned char)s[i]) != name[i]) return 0;
    return 1;
}

short tx_charset(const char *type)
{
    const char *s = type ? strchr(type, ';') : NULL;
    for (; s && *s; s++) {
        long n;
        if (lower(s[0]) != 'c' || !named(s, 8, "charset=")) continue;
        s += 8;
        if (*s == '"') s++;
        for (n = 0; s[n] && s[n] != '"' && s[n] != ';' && s[n] != ' '; n++) {}
        if (named(s, n, "utf-8") || named(s, n, "utf8")) return TX_UTF8;
        if (named(s, n, "iso-8859-1") || named(s, n, "latin1") || named(s, n, "us-ascii") ||
            named(s, n, "windows-1252") || named(s, n, "cp1252") || named(s, n, "iso-8859-15"))
            return TX_LATIN1;
        if (named(s, n, "macintosh") || named(s, n, "x-mac-roman") || named(s, n, "mac")) return TX_MAC;
        return TX_GUESS;
    }
    return TX_UTF8;
}

/* ---- memory: one block, carved up ---- */

#define ROUND(n) (((n) + 3) & ~3L)
#define LINE_ROOM (TX_LINE + 64)    /* a line with its leading tabs made spaces */

long tx_memory(int kind)
{
    long n = ROUND(sizeof(Tx)) + ROUND(TX_LINE) + ROUND(TX_LINE + 1) + ROUND(LINE_ROOM + 1);
    if (kind == TX_MARKDOWN)
        n += ROUND(TX_HOLD) + ROUND(sizeof(TxRef) * TX_REFS) + ROUND(TX_POOL) + ROUND(md_size()) + ROUND(md_tok_size());
    return n;
}

void *tx_begin(int kind, short charset, const TxSink *s)
{
    long n = tx_memory(kind);
    char *m = s->alloc(s->ref, n), *p;
    Tx *t = (Tx *)m;
    if (!m) return NULL;
    memset(m, 0, n);
    p = m + ROUND(sizeof(Tx));
    t->s = *s;
    t->kind = (short)kind;
    t->cs = charset;
    t->raw = p; p += ROUND(TX_LINE);
    t->conv = p; p += ROUND(TX_LINE + 1);
    t->line = p; p += ROUND(LINE_ROOM + 1);
    t->gfirst = 1;
    if (kind == TX_MARKDOWN) {
        t->hold = p; p += ROUND(TX_HOLD);
        t->refs = (TxRef *)p; p += ROUND(sizeof(TxRef) * TX_REFS);
        t->pool = p; p += ROUND(TX_POOL);
        t->md = (Md *)p; p += ROUND(md_size());
        t->toks = p;
        md_new(t);
    }
    return t;
}

/* ---- lines ---- */

/* A line (or a piece of one too long) to the format: as Mac Roman, with control
   characters as '?'; for Markdown, tabs in the indentation (up to the first character
   that isn't a space, tab, '>', or a list marker's) as spaces to the next multiple of 4 */
static void deliver(Tx *t, long n, int cut_after)
{
    long len, i, o = 0, col = 0;
    int lead = 1;
    if (t->cs == TX_MAC || !t->s.to_mac) {
        memcpy(t->conv, t->raw, n);
        len = n;
    } else if ((len = t->s.to_mac(t->s.ref, t->raw, n, t->cs, t->conv, TX_LINE + 1)) < 0) {
        len = 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)t->conv[i];
        if (t->kind == TX_MARKDOWN && lead) {
            if (c == '\t' && o + 4 <= LINE_ROOM) {
                do t->line[o++] = ' '; while (++col & 3);
                continue;
            }
            if (!strchr(" >-+*.)0123456789", c) || !c) lead = 0;
        }
        if (o >= LINE_ROOM) break;
        t->line[o++] = (char)(c < 32 && c != '\t' ? '?' : c);
        col++;
    }
    t->line[o] = 0;
    if (t->kind == TX_MARKDOWN) md_line(t, t->line, o, t->cut ? TX_CONT : 0);
    else gem_line(t, t->line, o, t->cut ? TX_CONT : 0);
    t->cut = (char)cut_after;
}

/* The line is as long as is kept: give what's there up to its last space (or, if none
   near the end, all of it but a UTF-8 character's start) and keep the rest */
static void cut_line(Tx *t)
{
    long k = t->nraw, i;
    for (i = t->nraw - 1; i > t->nraw - 160; i--)
        if (t->raw[i] == ' ') { k = i + 1; break; }
    if (k == t->nraw && t->cs != TX_MAC && t->cs != TX_LATIN1)
        while (k > t->nraw - 4 && ((unsigned char)t->raw[k - 1] & 0xC0) == 0x80) k--;
    if (k == t->nraw - 4 || k <= 0) k = t->nraw;
    if (k > 0 && k < t->nraw && ((unsigned char)t->raw[k - 1] & 0xC0) == 0xC0) k--;   /* a lead byte goes with the rest */
    deliver(t, k, 1);
    memmove(t->raw, t->raw + k, t->nraw - k);
    t->nraw -= k;
}

int tx_data(void *run, const char *p, long n)
{
    Tx *t = run;
    long i;
    for (i = 0; i < n && !t->stopped; i++) {
        char c = p[i];
        if (t->cr) {
            t->cr = 0;
            if (c == '\n') continue;
        }
        if (c == '\r' || c == '\n') {
            deliver(t, t->nraw, 0);
            t->nraw = 0;
            t->cr = (char)(c == '\r');
            continue;
        }
        if (t->nraw == TX_LINE) cut_line(t);
        t->raw[t->nraw++] = c;
    }
    return t->stopped ? TX_STOP : TX_MORE;
}

void tx_end(void *run)
{
    Tx *t = run;
    int i;
    if (!t) return;
    if (t->nraw || t->cut) deliver(t, t->nraw, 0);
    if (t->kind == TX_MARKDOWN) {
        md_end(t);
        tx_hold_flush(t, 1);
        /* references defined after held output had to go without them */
        for (i = 0; i < t->nrefs; i++) {
            TxRef *r = &t->refs[i];
            if (!r->missed || !r->defined) continue;
            if (r->missed == 1) tx_outs(t, "<HR>\n<P>");
            r->missed = 2;
            tx_outs(t, "<A HREF=\"");
            tx_url(t, t->pool + r->url, r->nurl, 1);
            tx_outs(t, "\">");
            tx_text(t, t->pool + r->label, r->nlabel);
            tx_outs(t, "</A><BR>\n");
        }
    } else {
        gem_end(t);
    }
    t->s.release(t->s.ref, t);
}

/* ---- output ---- */

static void emit(Tx *t, const char *p, long n)
{
    static const char meta[] = "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=macintosh\">\n";
    if (t->stopped || n <= 0) return;
    if (!t->started) {
        /* first, that the page is Mac Roman (what ToMacRoman made it). Not in tx_begin:
           Mill takes a Write during millFormatBegin, before it knows where the output
           goes, as the end of the run. */
        t->started = 1;
        if (!t->s.write(t->s.ref, meta, sizeof meta - 1)) { t->stopped = 1; return; }
    }
    if (!t->s.write(t->s.ref, p, n)) t->stopped = 1;
}

void tx_out(Tx *t, const char *p, long n)
{
    if (n <= 0) return;
    if (t->cap) {
        if (n > (long)sizeof t->capbuf - 1 - t->ncap) n = sizeof t->capbuf - 1 - t->ncap;
        memcpy(t->cap + t->ncap, p, n);
        t->ncap += n;
        return;
    }
    if (t->waiting) {
        if (t->nhold + n > TX_HOLD) tx_hold_flush(t, 1);
        else {
            memcpy(t->hold + t->nhold, p, n);
            t->nhold += n;
            return;
        }
    }
    emit(t, p, n);
}

void tx_outs(Tx *t, const char *s) { tx_out(t, s, (long)strlen(s)); }

void tx_text(Tx *t, const char *p, long n)
{
    long i, from = 0;
    for (i = 0; i < n; i++) {
        const char *e = p[i] == '&' ? "&amp;" : p[i] == '<' ? "&lt;" : p[i] == '>' ? "&gt;" : p[i] == '"' ? "&quot;" : NULL;
        if (!e) continue;
        tx_out(t, p + from, i - from);
        tx_outs(t, e);
        from = i + 1;
    }
    tx_out(t, p + from, n - from);
}

static int ascii_punct(int c) { return c > 32 && c < 127 && !(c >= '0' && c <= '9') && !((c | 32) >= 'a' && (c | 32) <= 'z'); }

int tx_entity(const char *s, long n)
{
    long i = 1, k;
    if (n < 3 || s[0] != '&') return 0;
    if (s[1] == '#') {
        int hex = n > 2 && (s[2] == 'x' || s[2] == 'X');
        i = hex ? 3 : 2;
        for (k = i; k < n && k - i < (hex ? 6 : 7); k++) {
            char c = s[k];
            if (!(c >= '0' && c <= '9') && !(hex && (c | 32) >= 'a' && (c | 32) <= 'f')) break;
        }
        return k > i && k < n && s[k] == ';' ? (int)k + 1 : 0;
    }
    for (k = 1; k < n && k < 33; k++) {
        char c = s[k];
        if (!((c | 32) >= 'a' && (c | 32) <= 'z') && !(c >= '0' && c <= '9')) break;
    }
    return k > 1 && k < n && s[k] == ';' && ((s[1] | 32) >= 'a' && (s[1] | 32) <= 'z') ? (int)k + 1 : 0;
}

void tx_url(Tx *t, const char *p, long n, int md)
{
    long i;
    for (i = 0; i < n; i++) {
        char c = p[i];
        int e;
        if (md && c == '\\' && i + 1 < n && ascii_punct((unsigned char)p[i + 1])) c = p[++i];
        else if (md && c == '&' && (e = tx_entity(p + i, n - i)) > 0) { tx_out(t, p + i, e); i += e - 1; continue; }
        switch (c) {
        case '&': tx_outs(t, "&amp;"); break;
        case '"': tx_outs(t, "%22"); break;
        case ' ': tx_outs(t, "%20"); break;
        case '<': tx_outs(t, "%3C"); break;
        case '>': tx_outs(t, "%3E"); break;
        default: tx_out(t, &c, 1);
        }
    }
}

void tx_title(Tx *t, const char *p, long n)
{
    if (t->titled) return;
    t->titled = 1;
    while (n > 0 && p[n - 1] == ' ') n--;
    if (n <= 0) return;
    tx_outs(t, "<TITLE>");
    tx_text(t, p, n);
    tx_outs(t, "</TITLE>\n");
}

int tx_punct(unsigned char c)
{
    if (c < 128) return ascii_punct(c);
    /* Mac Roman's punctuation and symbols */
    return (c >= 0xA0 && c <= 0xAD && c != 0xA7) || (c >= 0xB0 && c <= 0xB4) || (c >= 0xC0 && c <= 0xC2) ||
           (c >= 0xC7 && c <= 0xC9) || (c >= 0xD0 && c <= 0xD7) || (c >= 0xDA && c <= 0xDD) || (c >= 0xE0 && c <= 0xE4);
}

/* ---- references ---- */

/* Labels match without case (ASCII letters only) and with runs of white space as one
   space, leading and trailing white space aside */
static int same_label(const char *a, long na, const char *b, long nb)
{
    long i = 0, k = 0;
    while (i < na && TX_SPACE(a[i])) i++;
    while (k < nb && TX_SPACE(b[k])) k++;
    while (na > i && TX_SPACE(a[na - 1])) na--;
    while (nb > k && TX_SPACE(b[nb - 1])) nb--;
    while (i < na && k < nb) {
        if (TX_SPACE(a[i]) && TX_SPACE(b[k])) {
            while (i < na && TX_SPACE(a[i])) i++;
            while (k < nb && TX_SPACE(b[k])) k++;
            continue;
        }
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[k])) return 0;
        i++, k++;
    }
    return i == na && k == nb;
}

int tx_ref_find(Tx *t, const char *label, long n)
{
    int i;
    for (i = 0; i < t->nrefs; i++)
        if (same_label(t->pool + t->refs[i].label, t->refs[i].nlabel, label, n)) return i;
    return -1;
}

int tx_ref_add(Tx *t, const char *label, long n)
{
    TxRef *r;
    while (n > 0 && TX_SPACE(*label)) label++, n--;
    while (n > 0 && TX_SPACE(label[n - 1])) n--;
    if (t->nrefs >= TX_REFS || t->npool + n > TX_POOL) return -1;
    r = &t->refs[t->nrefs];
    memset(r, 0, sizeof *r);
    r->label = t->npool;
    r->nlabel = (short)n;
    memcpy(t->pool + t->npool, label, n);
    t->npool += (short)n;
    return t->nrefs++;
}

void tx_ref_define(Tx *t, int slot, const char *url, long n)
{
    TxRef *r = &t->refs[slot];
    if (r->defined) return;                 /* the first definition counts */
    if (t->npool + n > TX_POOL) return;
    r->url = t->npool;
    r->nurl = (short)n;
    memcpy(t->pool + t->npool, url, n);
    t->npool += (short)n;
    r->defined = 1;
    if (r->waiting) {
        r->waiting = 0;
        if (--t->waiting == 0) tx_hold_flush(t, 0);
    }
}

/* What a mark stands for: the link (or image) when its label is defined, else the text
   that was there */
static void mark_text(Tx *t, TxRef *r, int open, char what, char form)
{
    if (open) {
        if (!r->defined) tx_outs(t, what == TX_IMAGE ? "![" : "[");
        else {
            tx_outs(t, what == TX_IMAGE ? "<IMG SRC=\"" : "<A HREF=\"");
            tx_url(t, t->pool + r->url, r->nurl, 1);
            tx_outs(t, what == TX_IMAGE ? "\" ALT=\"" : "\">");
        }
    } else if (r->defined) {
        tx_outs(t, what == TX_IMAGE ? "\">" : "</A>");
    } else if (form == TX_FULL) {
        tx_outs(t, "][");
        tx_text(t, t->pool + r->label, r->nlabel);
        tx_outs(t, "]");
    } else {
        tx_outs(t, form == TX_COLLAPSED ? "][]" : "]");
    }
}

void tx_mark(Tx *t, int open, int slot, char what, char form)
{
    TxRef *r = &t->refs[slot];
    char m[4];
    if (open && !r->defined && !r->waiting) {
        r->waiting = 1;
        t->waiting++;
    }
    if (r->waiting && t->nhold + 4 > TX_HOLD) tx_hold_flush(t, 1);
    if (!r->waiting || t->cap) {
        mark_text(t, r, open, what, form);
        return;
    }
    m[0] = open ? TX_MARK_OPEN : TX_MARK_CLOSE;
    m[1] = (char)(slot + 1);
    m[2] = what;
    m[3] = form;
    memcpy(t->hold + t->nhold, m, 4);
    t->nhold += 4;
}

void tx_hold_flush(Tx *t, int all)
{
    long n = t->nhold, i = 0, from = 0;
    int k;
    for (k = 0; k < t->nrefs; k++)
        if (t->refs[k].waiting) {
            t->refs[k].waiting = 0;
            if (all && !t->refs[k].defined) t->refs[k].missed = 1;
        }
    t->waiting = 0;
    t->nhold = 0;
    while (i < n) {
        char mark = t->hold[i];
        if (mark != TX_MARK_OPEN && mark != TX_MARK_CLOSE) { i++; continue; }
        tx_out(t, t->hold + from, i - from);
        mark_text(t, &t->refs[(unsigned char)t->hold[i + 1] - 1], mark == TX_MARK_OPEN, t->hold[i + 2], t->hold[i + 3]);
        i = from = i + 4;
    }
    tx_out(t, t->hold + from, n - from);
}
