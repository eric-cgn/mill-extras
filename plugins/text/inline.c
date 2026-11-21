/* inline.c: what's inside a Markdown paragraph, heading, or table cell, as HTML, written
   from the CommonMark specification (0.31.2), section 6: backslash escapes, entities
   (passed on for Mill to read), code spans, emphasis and strong emphasis (with the
   delimiter-run rules and the "multiple of 3" rule), links and images (inline, full,
   collapsed, and shortcut references), autolinks, raw HTML, and hard line breaks.

   The text is read once into marks (Tok), links are matched as each ']' comes, and
   emphasis by the specification's process_emphasis; then it's written out.

   Simplified: link titles are dropped (HTML 3.2 has no place for them); a reference whose
   label isn't defined yet is taken to be a link, and held (text.c) until its definition
   comes or it can't wait any longer, when it goes out as the text it was; a paragraph
   with more than TX_TOKENS marks is read in parts, and nothing spans a part; Unicode
   punctuation and white space are Mac Roman's. */
#include <string.h>

#include "txint.h"

enum { K_DELIM, K_OPEN, K_CLOSE, K_CODE, K_AUTO, K_MAIL, K_HTML, K_ESC, K_ENT, K_BR };

typedef struct {
    short at, len;          /* the bytes it covers */
    char kind;
    char ch;                /* DELIM: '*' or '_'; OPEN: '[' or '!' */
    char n, orig;           /* DELIM: characters left, and at first */
    char opens, closes;     /* DELIM */
    char ntag;
    char tags[6];           /* DELIM: 'e' 's' closed here, in order; 'E' 'S' opened here, in order */
    char link;              /* OPEN: 0 if not a link, else TX_LINK or TX_IMAGE */
    char active, form, pending;
    short prev, next;       /* DELIM: the delimiter stack; OPEN: prev is the bracket below */
    short d_at, d_len;      /* OPEN: the destination in the text; or the ref's slot in d_at */
    short other;            /* OPEN: its CLOSE; CLOSE: its OPEN */
} Tok;

long md_tok_size(void) { return sizeof(Tok) * TX_TOKENS; }

typedef struct {
    Tx *t;
    const char *s;
    long n;
    Tok *k;
    int nk;
    int dtop, btop;         /* the delimiter stack's top, the brackets' */
} Ctx;

static int ascii_punct(int c) { return c > 32 && c < 127 && !(c >= '0' && c <= '9') && !((c | 32) >= 'a' && (c | 32) <= 'z'); }
static int letter(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static int digit(int c) { return c >= '0' && c <= '9'; }
static int white(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == 0xCA; }   /* 0xCA: no-break space */

/* ---- raw HTML and autolinks (6.5, 6.6) ---- */

static long skip_ws(const char *s, long i, long n)
{
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i++;
    return i;
}

static long find(const char *s, long i, long n, const char *w)
{
    long m = (long)strlen(w);
    for (; i + m <= n; i++) if (!memcmp(s + i, w, m)) return i + m;
    return 0;
}

long md_raw_html(const char *s, long n, int *kind)
{
    long i = 1, k;
    *kind = 2;
    if (n < 3 || s[0] != '<') return 0;
    if (s[1] == '!') {
        if (n >= 4 && s[2] == '-' && s[3] == '-') {
            if (n >= 5 && s[4] == '>') return 5;
            if (n >= 6 && s[4] == '-' && s[5] == '>') return 6;
            return find(s, 4, n, "-->");
        }
        if (n >= 9 && !memcmp(s, "<![CDATA[", 9)) return find(s, 9, n, "]]>");
        if (letter(s[2])) { for (k = 2; k < n; k++) if (s[k] == '>') return k + 1; }
        return 0;
    }
    if (s[1] == '?') return find(s, 2, n, "?>");
    *kind = 1;
    if (s[1] == '/') {
        i = 2;
        if (i >= n || !letter(s[i])) return 0;
        while (i < n && (letter(s[i]) || digit(s[i]) || s[i] == '-')) i++;
        i = skip_ws(s, i, n);
        return i < n && s[i] == '>' ? i + 1 : 0;
    }
    if (!letter(s[1])) return 0;
    while (i < n && (letter(s[i]) || digit(s[i]) || s[i] == '-')) i++;
    for (;;) {
        long w = skip_ws(s, i, n);
        if (w < n && s[w] == '>') return w + 1;
        if (w + 1 < n && s[w] == '/' && s[w + 1] == '>') return w + 2;
        if (w == i || w >= n) return 0;
        /* an attribute */
        i = w;
        if (!(letter(s[i]) || s[i] == '_' || s[i] == ':')) return 0;
        while (i < n && (letter(s[i]) || digit(s[i]) || strchr("_.:-", s[i]))) i++;
        w = skip_ws(s, i, n);
        if (w < n && s[w] == '=') {
            i = skip_ws(s, w + 1, n);
            if (i >= n) return 0;
            if (s[i] == '"' || s[i] == '\'') {
                char q = s[i];
                for (k = i + 1; k < n && s[k] != q; k++) {}
                if (k >= n) return 0;
                i = k + 1;
            } else {
                k = i;
                while (i < n && !strchr(" \t\n\"'=<>`", s[i])) i++;
                if (i == k) return 0;
            }
        }
    }
}

/* <scheme:...> (1) or <name@example.com> (2), its length in *len */
static int autolink(const char *s, long n, long *len)
{
    long i = 1, k;
    if (letter(s[1])) {
        while (i < n && (letter(s[i]) || digit(s[i]) || s[i] == '+' || s[i] == '.' || s[i] == '-')) i++;
        if (i - 1 >= 2 && i - 1 <= 32 && i < n && s[i] == ':') {
            for (k = i + 1; k < n && s[k] != '>' && s[k] != '<' && (unsigned char)s[k] > ' '; k++) {}
            if (k < n && s[k] == '>') { *len = k + 1; return 1; }
        }
    }
    for (i = 1; i < n && (letter(s[i]) || digit(s[i]) || strchr(".!#$%&'*+/=?^_`{|}~-", s[i])); i++) {}
    if (i == 1 || i >= n || s[i] != '@') return 0;
    k = ++i;
    for (;;) {
        long l = i;
        while (i < n && (letter(s[i]) || digit(s[i]) || s[i] == '-')) i++;
        if (i == l || i - l > 63 || s[l] == '-' || s[i - 1] == '-') return 0;
        if (i < n && s[i] == '.') { i++; continue; }
        break;
    }
    if (i < n && s[i] == '>' && i > k) { *len = i + 1; return 2; }
    return 0;
}

/* ---- links (6.3) ---- */

/* A link label "[...]" at s[i]: its end (after ']'), its text in *a to *b */
static long label_at(const char *s, long i, long n, long *a, long *b)
{
    long k;
    if (i >= n || s[i] != '[') return 0;
    for (k = i + 1; k < n && k - i <= 1000; k++) {
        if (s[k] == '\\' && k + 1 < n) { k++; continue; }
        if (s[k] == '[') return 0;
        if (s[k] == ']') {
            *a = i + 1;
            *b = k;
            return k + 1;
        }
    }
    return 0;
}

static int valid_label(const char *s, long a, long b)
{
    long k;
    if (b - a > 999) return 0;
    for (k = a; k < b; k++) {
        if (s[k] == '\\' && k + 1 < b) { k++; continue; }
        if (s[k] == '[' || s[k] == ']') return 0;
    }
    for (k = a; k < b; k++) if (!white((unsigned char)s[k])) return 1;
    return 0;
}

/* A link destination at s[i]: <...> or text with balanced parentheses. Its end, and
   where it is in *a to *b; -1 if none */
static long destination(const char *s, long i, long n, long *a, long *b)
{
    long k, depth = 0;
    if (i < n && s[i] == '<') {
        for (k = i + 1; k < n && s[k] != '>'; k++) {
            if (s[k] == '\\' && k + 1 < n) { k++; continue; }
            if (s[k] == '<' || s[k] == '\n') return -1;
        }
        if (k >= n) return -1;
        *a = i + 1;
        *b = k;
        return k + 1;
    }
    for (k = i; k < n; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c == '\\' && k + 1 < n && ascii_punct((unsigned char)s[k + 1])) { k++; continue; }
        if (c <= ' ') break;
        if (c == '(' && ++depth > 32) return -1;
        if (c == ')' && depth-- == 0) break;
    }
    if (k == i || depth > 0) return -1;
    *a = i;
    *b = k;
    return k;
}

/* A link title at s[i] ("...", '...', or (...)): its end, or 0 */
static long title_at(const char *s, long i, long n)
{
    char close;
    long k;
    if (i >= n) return 0;
    close = s[i] == '"' ? '"' : s[i] == '\'' ? '\'' : s[i] == '(' ? ')' : 0;
    if (!close) return 0;
    for (k = i + 1; k < n; k++) {
        if (s[k] == '\\' && k + 1 < n) { k++; continue; }
        if (s[k] == close) return k + 1;
        if (close == ')' && s[k] == '(') return 0;
    }
    return 0;
}

/* "(dest "title")" at s[i]: its end, the destination in *a to *b; 0 if it isn't one */
static long inline_link(const char *s, long i, long n, long *a, long *b)
{
    long k = skip_ws(s, i + 1, n), e, t;
    if (k < n && s[k] == ')') { *a = *b = k; return k + 1; }
    if ((e = destination(s, k, n, a, b)) < 0) return 0;
    k = skip_ws(s, e, n);
    if (k > e && (t = title_at(s, k, n)) != 0) k = skip_ws(s, t, n);
    return k < n && s[k] == ')' ? k + 1 : 0;
}

long md_refdefs(Tx *t, const char *s, long n)
{
    long done = 0;
    for (;;) {
        long a, b, k, da, db, e, line_end, tt;
        int slot;
        k = done;
        while (k < n && k - done < 4 && s[k] == ' ') k++;
        if ((k = label_at(s, k, n, &a, &b)) == 0 || !valid_label(s, a, b) || k >= n || s[k] != ':') return done;
        k = skip_ws(s, k + 1, n);
        if ((e = destination(s, k, n, &da, &db)) < 0) return done;
        /* the rest of the line: a title, or nothing */
        for (line_end = e; line_end < n && (s[line_end] == ' ' || s[line_end] == '\t'); line_end++) {}
        k = skip_ws(s, e, n);
        if (k > e && (tt = title_at(s, k, n)) != 0) {
            long r = tt;
            while (r < n && (s[r] == ' ' || s[r] == '\t')) r++;
            if (r >= n || s[r] == '\n') line_end = r;
            else if (line_end < n && s[line_end] != '\n') return done;
        } else if (line_end < n && s[line_end] != '\n') {
            return done;
        }
        slot = tx_ref_find(t, s + a, b - a);
        if (slot < 0) slot = tx_ref_add(t, s + a, b - a);
        if (slot >= 0) tx_ref_define(t, slot, s + da, db - da);
        done = line_end < n ? line_end + 1 : n;
        if (done >= n) return done;
    }
}

/* ---- the marks ---- */

static Tok *add(Ctx *c, int kind, long at, long len)
{
    Tok *k = &c->k[c->nk++];
    memset(k, 0, sizeof *k);
    k->kind = (char)kind;
    k->at = (short)at;
    k->len = (short)len;
    k->prev = k->next = -1;
    return k;
}

static void unlink_delim(Ctx *c, int i)
{
    Tok *k = &c->k[i];
    if (k->prev >= 0) c->k[k->prev].next = k->next;
    if (k->next >= 0) c->k[k->next].prev = k->prev;
    else c->dtop = k->prev;
    k->prev = k->next = -1;
}

/* The specification's process_emphasis, for the delimiters after bottom */
static void emphasis(Ctx *c, int bottom)
{
    short obot[2][2][3];
    int i, a, b, cur = c->dtop;
    for (i = 0; i < 12; i++) obot[i / 6][(i / 3) & 1][i % 3] = (short)bottom;
    if (cur < 0) return;
    while (cur >= 0 && c->k[cur].prev > bottom) cur = c->k[cur].prev;
    if (cur <= bottom) return;
    while (cur >= 0) {
        Tok *T = &c->k[cur];
        int o, found = -1;
        short *ob;
        if (!T->closes) { cur = T->next; continue; }
        ob = &obot[T->ch == '_'][T->opens ? 1 : 0][T->orig % 3];
        for (o = T->prev; o >= 0 && o > bottom && o > *ob; o = c->k[o].prev) {
            Tok *O = &c->k[o];
            if (O->ch != T->ch || !O->opens) continue;
            if ((O->closes || T->opens) && (O->orig + T->orig) % 3 == 0 && !(O->orig % 3 == 0 && T->orig % 3 == 0)) continue;
            if (O->ntag >= 6 || T->ntag >= 6) continue;
            found = o;
            break;
        }
        if (found >= 0) {
            Tok *O = &c->k[found];
            int use = O->n >= 2 && T->n >= 2 ? 2 : 1;
            O->tags[(int)O->ntag++] = use == 2 ? 'S' : 'E';
            T->tags[(int)T->ntag++] = use == 2 ? 's' : 'e';
            O->n = (char)(O->n - use);
            T->n = (char)(T->n - use);
            for (a = O->next; a >= 0 && a != cur; a = b) { b = c->k[a].next; unlink_delim(c, a); }
            if (!O->n) unlink_delim(c, found);
            if (!T->n) { int nx = T->next; unlink_delim(c, cur); cur = nx; }
        } else {
            int nx = T->next;
            *ob = T->prev;
            if (!T->opens) unlink_delim(c, cur);
            cur = nx;
        }
    }
    /* what's left above bottom is text */
    while (c->dtop > bottom) unlink_delim(c, c->dtop);
}

/* ']' at s[i]: a link or image, if the bracket it closes makes one. Where to go on. */
static long close_bracket(Ctx *c, long i)
{
    const char *s = c->s;
    long n = c->n, end = 0, a = 0, b = 0, la, lb;
    int bi = c->btop, slot = -1, form = 0;
    Tok *O, *C;
    if (bi < 0) return i + 1;
    O = &c->k[bi];
    c->btop = O->prev;
    if (!O->active) return i + 1;
    if (i + 1 < n && s[i + 1] == '(' && (end = inline_link(s, i + 1, n, &a, &b)) != 0) {
        form = 0;
    } else {
        long ta = O->at + O->len, tb = i, e;
        if ((e = label_at(s, i + 1, n, &la, &lb)) != 0 && lb > la) {
            form = TX_FULL;
            end = e;
        } else {
            form = e ? TX_COLLAPSED : TX_SHORTCUT;
            end = e ? e : i + 1;
            la = ta;
            lb = tb;
        }
        if (!valid_label(s, la, lb)) return i + 1;
        slot = tx_ref_find(c->t, s + la, lb - la);
        if (slot < 0) slot = tx_ref_add(c->t, s + la, lb - la);
        if (slot < 0) return i + 1;
    }
    O->link = O->ch == '!' ? TX_IMAGE : TX_LINK;
    O->form = (char)form;
    if (slot >= 0) {
        O->d_at = (short)slot;
        O->pending = 1;     /* written by tx_mark: as a link now if it's defined */
    } else {
        O->d_at = (short)a;
        O->d_len = (short)(b - a);
    }
    C = add(c, K_CLOSE, i, end - i);
    C->other = (short)bi;
    O->other = (short)(c->nk - 1);
    emphasis(c, bi);
    if (O->link == TX_LINK) {
        int k;
        for (k = c->btop; k >= 0; k = c->k[k].prev) if (c->k[k].ch == '[') c->k[k].active = 0;
    }
    return end;
}

/* Read marks from s[from] until the end or there's no room; where it stopped */
static long scan(Ctx *c, long from)
{
    const char *s = c->s;
    long n = c->n, i = from, k, e;
    int kind;
    c->nk = 0;
    c->dtop = c->btop = -1;
    while (i < n) {
        unsigned char ch = (unsigned char)s[i];
        if (c->nk >= TX_TOKENS - 2) break;
        switch (ch) {
        case '\\':
            if (i + 1 < n && s[i + 1] == '\n') { add(c, K_BR, i, 2); i += 2; continue; }
            if (i + 1 < n && ascii_punct((unsigned char)s[i + 1])) { add(c, K_ESC, i, 2); i += 2; continue; }
            break;
        case '`': {
            long run = 0, j;
            while (i + run < n && s[i + run] == '`') run++;
            for (j = i + run; j < n;) {
                long r = 0;
                if (s[j] != '`') { j++; continue; }
                while (j + r < n && s[j + r] == '`') r++;
                if (r == run) break;
                j += r;
            }
            if (j < n) { add(c, K_CODE, i, j + run - i); i = j + run; }
            else i += run;
            continue;
        }
        case '<':
            if ((kind = autolink(s + i, n - i, &e)) != 0) { add(c, kind == 1 ? K_AUTO : K_MAIL, i, e); i += e; continue; }
            if ((e = md_raw_html(s + i, n - i, &kind)) > 0) { add(c, K_HTML, i, e); i += e; continue; }
            break;
        case '&':
            if ((e = tx_entity(s + i, n - i)) > 0) { add(c, K_ENT, i, e); i += e; continue; }
            break;
        case '*': case '_': {
            long run = 0;
            unsigned char before = i > 0 ? (unsigned char)s[i - 1] : '\n', after;
            int left, right;
            Tok *T;
            while (i + run < n && s[i + run] == (char)ch) run++;
            if (run > 100) { i += run; continue; }
            after = i + run < n ? (unsigned char)s[i + run] : '\n';
            left = !white(after) && (!tx_punct(after) || white(before) || tx_punct(before));
            right = !white(before) && (!tx_punct(before) || white(after) || tx_punct(after));
            T = add(c, K_DELIM, i, run);
            T->ch = (char)ch;
            T->n = T->orig = (char)run;
            T->opens = (char)(ch == '*' ? left : left && (!right || tx_punct(before)));
            T->closes = (char)(ch == '*' ? right : right && (!left || tx_punct(after)));
            T->prev = (short)c->dtop;
            if (c->dtop >= 0) c->k[c->dtop].next = (short)(c->nk - 1);
            c->dtop = c->nk - 1;
            i += run;
            continue;
        }
        case '!':
            if (i + 1 < n && s[i + 1] == '[') {
                Tok *T = add(c, K_OPEN, i, 2);
                T->ch = '!';
                T->active = 1;
                T->prev = (short)c->btop;
                c->btop = c->nk - 1;
                i += 2;
                continue;
            }
            break;
        case '[': {
            Tok *T = add(c, K_OPEN, i, 1);
            T->ch = '[';
            T->active = 1;
            T->prev = (short)c->btop;
            c->btop = c->nk - 1;
            i++;
            continue;
        }
        case ']':
            i = close_bracket(c, i);
            continue;
        case '\n':
            for (k = i; k > from && s[k - 1] == ' '; k--) {}
            if (i - k >= 2 && (!c->nk || c->k[c->nk - 1].at + c->k[c->nk - 1].len <= k)) add(c, K_BR, k, i + 1 - k);
            break;
        }
        i++;
    }
    emphasis(c, -1);
    return i;
}

/* ---- writing ---- */

/* Text: spaces at a line's end dropped */
static void text(Tx *t, const char *s, long a, long b)
{
    long k, from = a;
    for (k = a; k < b; k++) {
        long e = k;
        if (s[k] != '\n') continue;
        while (e > from && s[e - 1] == ' ') e--;
        tx_text(t, s + from, e - from);
        tx_outs(t, "\n");
        from = k + 1;
    }
    tx_text(t, s + from, b - from);
}

static void tags(Tx *t, const Tok *T, int closing)
{
    int i;
    if (closing) {
        for (i = 0; i < T->ntag; i++)
            if (T->tags[i] == 'e') tx_outs(t, "</EM>");
            else if (T->tags[i] == 's') tx_outs(t, "</STRONG>");
    } else {
        for (i = T->ntag - 1; i >= 0; i--)
            if (T->tags[i] == 'E') tx_outs(t, "<EM>");
            else if (T->tags[i] == 'S') tx_outs(t, "<STRONG>");
    }
}

static void write_marks(Ctx *c, long from, long to, int alt)
{
    Tx *t = c->t;
    const char *s = c->s;
    long pos = from;
    int i;
    for (i = 0; i < c->nk; i++) {
        Tok *T = &c->k[i];
        long a = T->at, e = T->at + T->len;
        if (a < pos) continue;
        text(t, s, pos, a);
        pos = e;
        switch (T->kind) {
        case K_DELIM: {
            char run[100];
            memset(run, T->ch, T->n);
            if (!alt) tags(t, T, 1);
            tx_out(t, run, T->n);
            if (!alt) tags(t, T, 0);
            break;
        }
        case K_OPEN:
            if (!T->link) tx_outs(t, T->ch == '!' ? "![" : "[");
            else if (alt) alt += T->link == TX_IMAGE;
            else if (T->pending) {
                tx_mark(t, 1, T->d_at, T->link, T->form);
                alt += T->link == TX_IMAGE;
            } else if (T->link == TX_IMAGE) {
                tx_outs(t, "<IMG SRC=\"");
                tx_url(t, s + T->d_at, T->d_len, 1);
                tx_outs(t, "\" ALT=\"");
                alt = 1;
            } else {
                tx_outs(t, "<A HREF=\"");
                tx_url(t, s + T->d_at, T->d_len, 1);
                tx_outs(t, "\">");
            }
            break;
        case K_CLOSE: {
            Tok *O = &c->k[T->other];
            if (O->link == TX_IMAGE) alt--;
            if (alt) break;
            if (O->pending) tx_mark(t, 0, O->d_at, O->link, O->form);
            else tx_outs(t, O->link == TX_IMAGE ? "\">" : "</A>");
            break;
        }
        case K_CODE: {
            long run = 0, ca, cb, k;
            while (s[a + run] == '`') run++;
            ca = a + run;
            cb = e - run;
            for (k = ca; k < cb && (s[k] == ' ' || s[k] == '\n'); k++) {}
            if (k < cb && cb - ca >= 2 && (s[ca] == ' ' || s[ca] == '\n') && (s[cb - 1] == ' ' || s[cb - 1] == '\n')) ca++, cb--;
            if (!alt) tx_outs(t, "<CODE>");
            for (k = ca; k < cb; k++) tx_text(t, s[k] == '\n' ? " " : s + k, 1);
            if (!alt) tx_outs(t, "</CODE>");
            break;
        }
        case K_AUTO: case K_MAIL:
            if (!alt) {
                tx_outs(t, T->kind == K_MAIL ? "<A HREF=\"mailto:" : "<A HREF=\"");
                tx_url(t, s + a + 1, T->len - 2, 0);
                tx_outs(t, "\">");
            }
            tx_text(t, s + a + 1, T->len - 2);
            if (!alt) tx_outs(t, "</A>");
            break;
        case K_HTML: if (!alt) tx_out(t, s + a, T->len); break;
        case K_ESC: tx_text(t, s + a + 1, 1); break;
        case K_ENT: tx_out(t, s + a, T->len); break;
        case K_BR: tx_outs(t, alt ? " " : "<BR>\n"); break;
        }
    }
    text(t, s, pos, to);
}

static void run(Tx *t, const char *s, long n, int alt)
{
    Ctx c;
    long from = 0;
    c.t = t;
    c.s = s;
    c.n = n;
    c.k = t->toks;
    while (from < n) {
        long to = scan(&c, from);
        write_marks(&c, from, to, alt);
        from = to;
    }
}

void md_inline(Tx *t, const char *s, long n) { run(t, s, n, 0); }

void md_inline_title(Tx *t, const char *s, long n)
{
    long k;
    if (t->titled) return;
    t->titled = 1;
    t->cap = t->capbuf;
    t->ncap = 0;
    run(t, s, n, 1);
    t->cap = NULL;
    /* not ending in the middle of an entity */
    for (k = t->ncap; k > 0 && t->capbuf[k - 1] != ';'; k--)
        if (t->capbuf[k - 1] == '&') { t->ncap = k - 1; break; }
    while (t->ncap > 0 && t->capbuf[t->ncap - 1] == ' ') t->ncap--;
    if (!t->ncap) return;
    tx_outs(t, "<TITLE>");
    tx_out(t, t->capbuf, t->ncap);
    tx_outs(t, "</TITLE>\n");
}
