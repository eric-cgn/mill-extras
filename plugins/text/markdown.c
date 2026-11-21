/* markdown.c: Markdown's blocks as HTML, a line at a time, written from the CommonMark
   specification (0.31.2), with GitHub's pipe tables. inline.c does what's inside them.

   Each line is matched against the open block quotes and list items, then may open new
   ones, then goes to the leaf block: a paragraph, fenced or indented code, an HTML block,
   or a table (headings and thematic breaks are single lines). Container tags go out as
   blocks open and close; code and HTML lines go out as they come; a paragraph is kept
   until it ends (it may become a setext heading or a table), up to TX_PARA bytes.

   Simplified, compared with the specification:
   - A list is tight until a blank line separates two of its items, or two blocks in an
     item; from there on its paragraphs are <P>s. (The specification decides for the whole
     list, which would mean keeping all of it.)
   - Tabs are made spaces (to the next multiple of 4) only in a line's indentation, before
     its first character that isn't a space, '>', or a list marker's.
   - A paragraph longer than TX_PARA goes out in parts, cut at a line end: emphasis or a
     link can't span the cut, and such a paragraph can't become a setext heading or table.
   - The rest of a line longer than TX_LINE is more text in the same paragraph or code
     block; after a heading or table row, it's a paragraph of its own.
   - Labels and HTML block names match without case for ASCII letters only.
   - Lists and quotes nest TX_DEPTH deep; deeper markers are text. */
#include <string.h>

#include "txint.h"

enum { C_QUOTE, C_LIST, C_ITEM };

typedef struct {
    char type;
    char ordered, mark;     /* list: '-' '+' '*', or '.' ')' after a number */
    char loose, blank;      /* list: <P>s from here on; a blank line came in it */
    char empty;             /* item: began with a blank line, nothing in it yet */
    short width;            /* item: the column its content starts at */
} Cont;

enum { L_NONE, L_PARA, L_FENCE, L_CODE, L_HTML, L_TABLE };

struct Md {
    Cont c[TX_DEPTH];
    short nc;
    short leaf;
    long npara, plast;      /* the paragraph's length, and where its last line starts */
    short popen;            /* 1: part of it went out (cut), 2: as a <P> */
    char fch;               /* fence: '`' or '~', its length, and its indentation */
    short flen, find;
    short pre_first;        /* code: no line yet */
    short code_blank;       /* indented code: blank lines kept until more code comes */
    short html_end;         /* HTML block: which of the specification's seven */
    short ncol;             /* table */
    char align[16];
    char para[TX_PARA];
};

long md_size(void) { return sizeof(Md); }

Md *md_new(Tx *t) { return t->md; }     /* tx_begin zeroed it */

/* ---- recognising lines ---- */

static long indent(const char *s, long i, long n)
{
    long k = 0;
    while (i + k < n && s[i + k] == ' ') k++;
    return k;
}

static int blank_from(const char *s, long i, long n)
{
    for (; i < n; i++) if (s[i] != ' ' && s[i] != '\t') return 0;
    return 1;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* "***", "- - -", "___" */
static int thematic(const char *s, long i, long n)
{
    char c = i < n ? s[i] : 0;
    int k = 0;
    if (c != '*' && c != '-' && c != '_') return 0;
    for (; i < n; i++) {
        if (s[i] == c) k++;
        else if (s[i] != ' ' && s[i] != '\t') return 0;
    }
    return k >= 3;
}

/* "## Heading ##": its level, and its text (*cs to *ce) */
static int atx(const char *s, long i, long n, long *cs, long *ce)
{
    int level = 0;
    long e;
    while (i < n && s[i] == '#' && level < 7) i++, level++;
    if (!level || level > 6 || (i < n && s[i] != ' ' && s[i] != '\t')) return 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    e = n;
    while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    {
        long h = e;
        while (h > i && s[h - 1] == '#') h--;
        if (h == i) e = i;                                       /* only #s */
        else if (h < e && (s[h - 1] == ' ' || s[h - 1] == '\t')) {
            e = h;                                               /* a closing sequence */
            while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
        }
    }
    *cs = i;
    *ce = e;
    return level;
}

/* "```" or "~~~" (3 or more), and for backticks no backtick after */
static int fence_open(const char *s, long i, long n, char *ch)
{
    long k = 0;
    char c = i < n ? s[i] : 0;
    if (c != '`' && c != '~') return 0;
    while (i + k < n && s[i + k] == c) k++;
    if (k < 3) return 0;
    if (c == '`' && memchr(s + i + k, '`', n - i - k)) return 0;
    *ch = c;
    return (int)k;
}

/* "===" (1) or "---" (2): a setext heading's underline */
static int setext(const char *s, long i, long n)
{
    char c = i < n ? s[i] : 0;
    if (c != '=' && c != '-') return 0;
    while (i < n && s[i] == c) i++;
    return blank_from(s, i, n) ? (c == '=' ? 1 : 2) : 0;
}

typedef struct {
    char ordered, mark;
    long start;
    long len;           /* the marker's */
} Marker;

static int list_marker(const char *s, long i, long n, Marker *m)
{
    long k = 0;
    m->start = 0;
    if (i >= n) return 0;
    if (s[i] == '-' || s[i] == '+' || s[i] == '*') {
        m->ordered = 0;
        m->mark = s[i];
        m->len = 1;
    } else {
        m->start = 0;
        while (i + k < n && k < 9 && s[i + k] >= '0' && s[i + k] <= '9') m->start = m->start * 10 + (s[i + k++] - '0');
        if (!k || i + k >= n || (s[i + k] != '.' && s[i + k] != ')')) return 0;
        m->ordered = 1;
        m->mark = s[i + k];
        m->len = k + 1;
    }
    return i + m->len == n || s[i + m->len] == ' ' || s[i + m->len] == '\t';
}

/* Whether s (from i, after "<" or "</") starts with name, then a space, '>', "/>", or the
   end */
static int tag_named(const char *s, long i, long n, const char *names)
{
    long k = 0;
    const char *p;
    char name[16];
    while (i + k < n && k < 15 && (((s[i + k] | 32) >= 'a' && (s[i + k] | 32) <= 'z') || (s[i + k] >= '0' && s[i + k] <= '9')))
        name[k] = (char)lower((unsigned char)s[i + k]), k++;
    if (!k || k >= 15) return 0;
    name[k] = 0;
    if (i + k < n && s[i + k] != ' ' && s[i + k] != '\t' && s[i + k] != '>' && !(s[i + k] == '/' && i + k + 1 < n && s[i + k + 1] == '>'))
        return 0;
    for (p = names; (p = strstr(p, name)) != NULL; p++)
        if (p[-1] == '|' && p[k] == '|') return 1;
    return 0;
}

/* The kinds of HTML block (CommonMark 4.6): which one starts at s[i], or 0 */
static int html_start(const char *s, long i, long n, int in_para)
{
    long k;
    int kind;
    if (i >= n || s[i] != '<') return 0;
    if (tag_named(s, i + 1, n, "|script|pre|style|textarea|")) return 1;
    if (n - i >= 4 && !memcmp(s + i, "<!--", 4)) return 2;
    if (n - i >= 2 && s[i + 1] == '?') return 3;
    if (n - i >= 9 && !memcmp(s + i, "<![CDATA[", 9)) return 5;
    if (n - i >= 3 && s[i + 1] == '!' && ((s[i + 2] | 32) >= 'a' && (s[i + 2] | 32) <= 'z')) return 4;
    if (tag_named(s, i + (n - i > 1 && s[i + 1] == '/' ? 2 : 1), n,
                  "|address|article|aside|base|basefont|blockquote|body|caption|center|col|colgroup|dd|details|"
                  "dialog|dir|div|dl|dt|fieldset|figcaption|figure|footer|form|frame|frameset|h1|h2|h3|h4|h5|"
                  "h6|head|header|hr|html|iframe|legend|li|link|main|menu|menuitem|nav|noframes|ol|optgroup|"
                  "option|p|param|search|section|summary|table|tbody|td|tfoot|th|thead|title|tr|track|ul|"))
        return 6;
    if (in_para) return 0;
    k = md_raw_html(s + i, n - i, &kind);
    if (k > 0 && kind == 1 && blank_from(s, i + k, n) && !tag_named(s, i + (s[i + 1] == '/' ? 2 : 1), n, "|script|style|pre|textarea|"))
        return 7;
    return 0;
}

static int has_ci(const char *s, long n, const char *w)
{
    long i, k, m = (long)strlen(w);
    for (i = 0; i + m <= n; i++) {
        for (k = 0; k < m && lower((unsigned char)s[i + k]) == w[k]; k++) {}
        if (k == m) return 1;
    }
    return 0;
}

/* Whether the line ends HTML block kind (1 to 5) */
static int html_ends(int kind, const char *s, long n)
{
    switch (kind) {
    case 1: return has_ci(s, n, "</script>") || has_ci(s, n, "</pre>") || has_ci(s, n, "</style>") || has_ci(s, n, "</textarea>");
    case 2: return has_ci(s, n, "-->");
    case 3: return has_ci(s, n, "?>");
    case 4: return memchr(s, '>', n) != NULL;
    case 5: return has_ci(s, n, "]]>");
    }
    return 0;
}

/* ---- tables ---- */

/* A row's cells, written as TH (head 1) or TD (0), or only counted (-1): how many it
   has. Writing takes the backslash out of each "\|" first (in place), as GFM does even in
   code spans. */
static int cells(Tx *t, char *s, long n, int head)
{
    Md *m = t->md;
    long i = 0, from;
    int k = 0;
    while (i < n && s[i] == ' ') i++;
    while (n > i && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (i < n && s[i] == '|') i++;
    if (n > i && s[n - 1] == '|' && !(n - 2 >= i && s[n - 2] == '\\')) n--;
    if (i >= n && !head) return 0;
    for (from = i; i <= n; i++) {
        long a, b;
        if (i < n && s[i] == '\\') { i++; continue; }
        if (i < n && s[i] != '|') continue;
        if (head >= 0) {
            a = from, b = i;
            while (a < b && s[a] == ' ') a++;
            while (b > a && s[b - 1] == ' ') b--;
            if (k < m->ncol) {
                char al = m->align[k];
                long r, w;
                for (r = w = a; r < b; r++) {
                    if (s[r] == '\\' && r + 1 < b && s[r + 1] == '|') r++;
                    s[w++] = s[r];
                }
                b = w;
                tx_outs(t, head ? "<TH" : "<TD");
                tx_outs(t, al == 'l' ? " ALIGN=LEFT>" : al == 'r' ? " ALIGN=RIGHT>" : al == 'c' ? " ALIGN=CENTER>" : ">");
                md_inline(t, s + a, b - a);
                tx_outs(t, head ? "</TH>" : "</TD>");
            }
        }
        k++;
        from = i + 1;
    }
    return k;
}

/* A delimiter row ("| :-- | --: |"): its cells' count and alignments, or 0 */
static int delimiter_row(Tx *t, const char *s, long n)
{
    Md *m = t->md;
    long i = 0;
    int k = 0;
    if (!memchr(s, '|', n)) return 0;
    while (i < n && s[i] == ' ') i++;
    if (i < n && s[i] == '|') i++;
    while (i < n) {
        int left = 0, right = 0, dashes = 0;
        while (i < n && s[i] == ' ') i++;
        if (i >= n) break;
        if (s[i] == ':') left = 1, i++;
        while (i < n && s[i] == '-') dashes++, i++;
        if (i < n && s[i] == ':') right = 1, i++;
        while (i < n && s[i] == ' ') i++;
        if (!dashes || (i < n && s[i] != '|') || k >= 16) return 0;
        m->align[k++] = left && right ? 'c' : left ? 'l' : right ? 'r' : 0;
        if (i < n) i++;
    }
    return k;
}

/* ---- output ---- */

static int tight(Md *m)
{
    return m->nc >= 2 && m->c[m->nc - 1].type == C_ITEM && !m->c[m->nc - 2].loose;
}

/* Part of the paragraph out: its definitions first (at its start), then its text */
static void para_part(Tx *t, const char *s, long n, int last)
{
    Md *m = t->md;
    if (!m->popen) {
        long k = md_refdefs(t, s, n);
        s += k;
        n -= k;
    }
    if (last) while (n > 0 && TX_SPACE(s[n - 1])) n--;
    if (n <= 0) return;
    if (!m->popen) {
        m->popen = tight(m) ? 1 : 2;
        if (m->popen == 2) tx_outs(t, "<P>");
    } else {
        tx_outs(t, "\n");
    }
    md_inline(t, s, n);
}

static void para_close(Tx *t)
{
    Md *m = t->md;
    para_part(t, m->para, m->npara, 1);
    if (m->popen == 2) tx_outs(t, "</P>");
    if (m->popen) tx_outs(t, "\n");
    m->npara = m->plast = 0;
    m->popen = 0;
}

static void para_add(Tx *t, const char *s, long n, int newline)
{
    Md *m = t->md;
    if (newline) while (n > 0 && *s == ' ') s++, n--;
    if (m->npara + 1 + n > TX_PARA) {
        /* full: out with it up to its last line, and keep that */
        long k = m->npara;
        while (k > 0 && m->para[k - 1] != '\n') k--;
        k = k ? k - 1 : m->npara;
        para_part(t, m->para, k, 0);
        if (!m->popen) m->popen = 1;
        if (k < m->npara) k++;
        memmove(m->para, m->para + k, m->npara - k);
        m->npara -= k;
        if (n > TX_PARA - 1 - m->npara) n = TX_PARA - 1 - m->npara;
    }
    if (m->npara && newline) m->para[m->npara++] = '\n';
    if (newline) m->plast = m->npara;
    memcpy(m->para + m->npara, s, n);
    m->npara += n;
}

static void code_text(Tx *t, const char *s, long n)
{
    Md *m = t->md;
    if (!m->pre_first) tx_outs(t, "\n");
    m->pre_first = 0;
    tx_text(t, s, n);
}

static void close_leaf(Tx *t)
{
    Md *m = t->md;
    switch (m->leaf) {
    case L_PARA: para_close(t); break;
    case L_FENCE: case L_CODE: tx_outs(t, "</PRE>\n"); break;
    case L_TABLE: tx_outs(t, "</TABLE>\n"); break;
    }
    m->leaf = L_NONE;
}

/* Close containers until k are left (and the leaf, if any go) */
static void close_to(Tx *t, int k)
{
    Md *m = t->md;
    if (m->nc <= k) return;
    close_leaf(t);
    while (m->nc > k) {
        Cont *c = &m->c[--m->nc];
        if (c->type == C_QUOTE) tx_outs(t, "</BLOCKQUOTE>\n");
        else if (c->type == C_LIST) tx_outs(t, c->ordered ? "</OL>\n" : "</UL>\n");
        else tx_outs(t, "\n");
    }
}

static Cont *push(Md *m, int type)
{
    Cont *c = &m->c[m->nc++];
    memset(c, 0, sizeof *c);
    c->type = (char)type;
    return c;
}

/* A new block goes into the container at parent: a list whose item (or which) it is
   in, after a blank line, is loose from here on */
static void new_block(Md *m, int parent)
{
    int i;
    for (i = 0; i < m->nc && i <= parent; i++) {
        Cont *c = &m->c[i];
        if (c->type == C_LIST && c->blank && (parent == i || parent == i + 1)) c->loose = 1;
        if (c->type == C_LIST) c->blank = 0;
    }
}

static void heading(Tx *t, int level, const char *s, long n)
{
    char tag[8];
    while (n > 0 && TX_SPACE(*s)) s++, n--;
    while (n > 0 && TX_SPACE(s[n - 1])) n--;
    md_inline_title(t, s, n);
    memcpy(tag, "<H1>", 5);
    tag[2] = (char)('0' + level);
    tx_outs(t, tag);
    md_inline(t, s, n);
    memcpy(tag, "</H1>\n", 7);
    tag[3] = (char)('0' + level);
    tx_outs(t, tag);
}

/* ---- a line ---- */

/* The rest of a line too long to keep whole */
static void cont_line(Tx *t, const char *s, long n)
{
    Md *m = t->md;
    switch (m->leaf) {
    case L_FENCE: case L_CODE: tx_text(t, s, n); return;
    case L_HTML: tx_out(t, s, n); return;
    case L_PARA: break;
    default:
        close_leaf(t);
        m->leaf = L_PARA;
    }
    para_add(t, s, n, 0);
}

void md_line(Tx *t, char *s, long n, int flags)
{
    Md *m = t->md;
    long pos = 0, ind, p;
    int matched = 0, i, opened = 0, tent = -1, all, blank, level;
    char ch;
    if (flags & TX_CONT) {
        cont_line(t, s, n);
        return;
    }

    /* the open containers this line continues */
    for (i = 0; i < m->nc; i++) {
        Cont *c = &m->c[i], *item;
        ind = indent(s, pos, n);
        blank = pos + ind >= n;
        if (c->type == C_QUOTE) {
            if (ind > 3 || blank || s[pos + ind] != '>') break;
            pos += ind + 1;
            if (pos < n && s[pos] == ' ') pos++;
            matched++;
            continue;
        }
        item = c + 1;   /* a list always has an item open in it */
        if (blank ? !item->empty : ind >= item->width) {
            pos = blank ? n : pos + item->width;
            matched += 2;
            i++;
            continue;
        }
        tent = i;       /* the list goes on if a new item of it starts here */
        break;
    }
    all = matched == m->nc;
    if (all && m->leaf == L_FENCE) {
        ind = indent(s, pos, n);
        if (ind <= 3 && pos + ind < n && s[pos + ind] == m->fch) {
            long k = 0;
            while (pos + ind + k < n && s[pos + ind + k] == m->fch) k++;
            if (k >= m->flen && blank_from(s, pos + ind + k, n)) {
                close_leaf(t);
                return;
            }
        }
        pos += ind < m->find ? ind : m->find;
        code_text(t, s + pos, n - pos);
        return;
    }
    if (all && m->leaf == L_HTML) {
        if (m->html_end >= 6 && blank_from(s, pos, n)) {
            m->leaf = L_NONE;
            return;
        }
        tx_out(t, s + pos, n - pos);
        tx_outs(t, "\n");
        if (m->html_end < 6 && html_ends(m->html_end, s + pos, n - pos)) m->leaf = L_NONE;
        return;
    }

    /* new containers */
    for (;;) {
        Marker mk;
        long start = pos, after, sp;
        int para = m->leaf == L_PARA && !opened;
        ind = indent(s, pos, n);
        p = pos + ind;
        if (ind >= 4) break;
        if (p < n && s[p] == '>') {
            if (matched + 1 > TX_DEPTH) break;     /* too deep: text */
            close_to(t, matched);
            close_leaf(t);
            if (!opened) new_block(m, m->nc - 1);
            push(m, C_QUOTE);
            tx_outs(t, "<BLOCKQUOTE>\n");
            matched = m->nc;
            opened = 1;
            tent = -1;
            pos = p + 1;
            if (pos < n && s[pos] == ' ') pos++;
            continue;
        }
        if ((para && all && setext(s, p, n)) || thematic(s, p, n) || !list_marker(s, p, n, &mk) || matched + 2 > TX_DEPTH)
            break;
        after = p + mk.len;
        sp = indent(s, after, n);
        blank = after + sp >= n;
        if (tent >= 0 && !opened && m->c[tent].ordered == mk.ordered && m->c[tent].mark == mk.mark) {
            close_to(t, tent + 1);  /* the item before ends; the list goes on */
            new_block(m, tent);
        } else {
            Cont *l;
            /* the first item of a list breaks into a paragraph only with something in it,
               and a number only as 1 */
            if (para && (blank || (mk.ordered && mk.start != 1))) break;
            close_to(t, matched);
            close_leaf(t);
            if (!opened) new_block(m, m->nc - 1);
            l = push(m, C_LIST);
            l->ordered = mk.ordered;
            l->mark = mk.mark;
            if (!mk.ordered) tx_outs(t, "<UL>\n");
            else if (mk.start == 1) tx_outs(t, "<OL>\n");
            else {
                char num[24];
                long v = mk.start, k = 0;
                tx_outs(t, "<OL START=");
                do num[k++] = (char)('0' + v % 10); while ((v /= 10) > 0);
                while (k > 0) tx_out(t, &num[--k], 1);
                tx_outs(t, ">\n");
            }
        }
        {
            Cont *it = push(m, C_ITEM);
            if (blank) it->width = (short)(after - start + 1), it->empty = 1, pos = n;
            else if (sp >= 5) it->width = (short)(after - start + 1), pos = after + 1;
            else it->width = (short)(after - start + sp), pos = after + sp;
        }
        tx_outs(t, "<LI>");
        matched = m->nc;
        opened = 1;
        tent = -1;
    }

    ind = indent(s, pos, n);
    p = pos + ind;
    blank = p >= n;
    if (!opened && matched < m->nc && m->leaf == L_PARA && !blank) {
        /* a lazy continuation line: the paragraph goes on, in all its containers */
        long cs, ce;
        int kind = 0;
        if (ind >= 4 || !(thematic(s, p, n) || atx(s, p, n, &cs, &ce) || fence_open(s, p, n, &ch) ||
                          ((kind = html_start(s, p, n, 1)) != 0 && kind < 7))) {
            para_add(t, s + p, n - p, 1);
            return;
        }
    }
    if (!opened) close_to(t, matched);
    if (blank) {
        if (m->leaf == L_CODE) m->code_blank++;
        else close_leaf(t);
        if (!opened) for (i = 0; i < m->nc; i++) if (m->c[i].type == C_LIST) m->c[i].blank = 1;
        return;
    }
    for (i = 0; i < m->nc; i++) m->c[i].empty = 0;

    if (ind >= 4) {
        if (m->leaf == L_PARA) {
            para_add(t, s + p, n - p, 1);
            return;
        }
        if (m->leaf != L_CODE) {
            close_leaf(t);
            if (!opened) new_block(m, m->nc - 1);
            tx_outs(t, "<PRE>");
            m->leaf = L_CODE;
            m->pre_first = 1;
            m->code_blank = 0;
        }
        while (m->code_blank > 0) code_text(t, "", 0), m->code_blank--;
        code_text(t, s + pos + 4, n - pos - 4);
        return;
    }
    if (m->leaf == L_CODE) close_leaf(t);

    if ((m->flen = (short)fence_open(s, p, n, &ch)) != 0) {
        close_leaf(t);
        if (!opened) new_block(m, m->nc - 1);
        tx_outs(t, "<PRE>");
        m->leaf = L_FENCE;
        m->fch = ch;
        m->find = (short)ind;
        m->pre_first = 1;
        return;
    }
    {
        long cs, ce;
        if ((level = atx(s, p, n, &cs, &ce)) != 0) {
            close_leaf(t);
            if (!opened) new_block(m, m->nc - 1);
            heading(t, level, s + cs, ce - cs);
            return;
        }
    }
    if (m->leaf == L_PARA && !opened && !m->popen && (level = setext(s, p, n)) != 0) {
        long k = md_refdefs(t, m->para, m->npara);
        if (k < m->npara && !blank_from(m->para, k, m->npara)) {
            heading(t, level, m->para + k, m->npara - k);
            m->npara = m->plast = 0;
            m->leaf = L_NONE;
            return;
        }
        /* only definitions: no heading; the line is what it would be otherwise */
        m->npara = m->plast = 0;
        m->leaf = L_NONE;
        if (level == 1) {
            m->leaf = L_PARA;
            para_add(t, s + p, n - p, 1);
            return;
        }
    }
    if (thematic(s, p, n)) {
        close_leaf(t);
        if (!opened) new_block(m, m->nc - 1);
        tx_outs(t, "<HR>\n");
        return;
    }
    if ((level = html_start(s, p, n, m->leaf == L_PARA)) != 0) {
        close_leaf(t);
        if (!opened) new_block(m, m->nc - 1);
        m->leaf = L_HTML;
        m->html_end = (short)level;
        tx_out(t, s + pos, n - pos);
        tx_outs(t, "\n");
        if (level < 6 && html_ends(level, s + p + 2, n - p - 2)) m->leaf = L_NONE;
        return;
    }
    if (m->leaf == L_PARA && !opened && !m->popen && m->npara > 0) {
        long hl = m->npara - m->plast;
        int k = delimiter_row(t, s + p, n - p);
        if (k && memchr(m->para + m->plast, '|', hl) && cells(t, m->para + m->plast, hl, -1) == k) {
            /* a table: its head is the paragraph's last line */
            m->ncol = (short)k;
            if (m->plast > 0) {
                para_part(t, m->para, m->plast - 1, 1);
                if (m->popen == 2) tx_outs(t, "</P>");
                if (m->popen) tx_outs(t, "\n");
                m->popen = 0;
            }
            tx_outs(t, "<TABLE BORDER=1 CELLPADDING=2>\n<TR>");
            cells(t, m->para + m->plast, hl, 1);
            tx_outs(t, "</TR>\n");
            m->npara = m->plast = 0;
            m->leaf = L_TABLE;
            return;
        }
    }
    if (m->leaf == L_TABLE) {
        tx_outs(t, "<TR>");
        for (i = cells(t, s + p, n - p, 0); i < m->ncol; i++) tx_outs(t, "<TD></TD>");
        tx_outs(t, "</TR>\n");
        return;
    }
    if (m->leaf != L_PARA) {
        close_leaf(t);
        if (!opened) new_block(m, m->nc - 1);
        m->leaf = L_PARA;
    }
    para_add(t, s + p, n - p, 1);
}

void md_end(Tx *t)
{
    close_leaf(t);
    close_to(t, 0);
}
