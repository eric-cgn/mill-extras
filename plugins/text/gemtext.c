/* gemtext.c: Gemtext (text/gemini) as HTML, a line at a time, from the Gemini
   specification's section on it ("gemtext"):
   - a text line is shown as it is: lines next to each other are lines of one paragraph
     (<BR> between them), and a blank line ends the paragraph;
   - "=>" URL [label]: a link on a line of its own, the URL as its label if it has none
     (Mill reads a relative URL against the page's own address);
   - "```" starts and ends preformatted text, shown as it is (the alt text after the
     opening "```" isn't shown);
   - "#", "##", "###": headings (the first is the page's title);
   - "* ": list items, one list for the lines next to each other;
   - ">": quoted lines, one block quote for the lines next to each other. */
#include <string.h>

#include "txint.h"

enum { G_NONE, G_TEXT, G_LIST, G_QUOTE };

static void close_block(Tx *t)
{
    switch (t->gmode) {
    case G_TEXT: tx_outs(t, "\n"); break;
    case G_LIST: tx_outs(t, "</UL>\n"); break;
    case G_QUOTE: tx_outs(t, "</BLOCKQUOTE>\n"); break;
    }
    t->gmode = G_NONE;
}

/* A line of mode's block: the block opened, or a line break after the one before */
static void line_of(Tx *t, int mode)
{
    if (t->gmode == mode) {
        tx_outs(t, mode == G_LIST ? "\n<LI>" : "<BR>\n");
        return;
    }
    close_block(t);
    t->gmode = (short)mode;
    tx_outs(t, mode == G_TEXT ? "<P>" : mode == G_LIST ? "<UL>\n<LI>" : "<BLOCKQUOTE>");
}

static long skip_space(const char *s, long i, long n)
{
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

void gem_line(Tx *t, const char *s, long n, int flags)
{
    long i, u, nu;
    if (t->gpre) {
        if (!(flags & TX_CONT) && n >= 3 && !memcmp(s, "```", 3)) {
            tx_outs(t, "</PRE>\n");
            t->gpre = 0;
            return;
        }
        if (!(flags & TX_CONT) && !t->gfirst) tx_outs(t, "\n");
        t->gfirst = 0;
        tx_text(t, s, n);
        return;
    }
    if (flags & TX_CONT) {      /* the rest of a line too long to keep whole */
        tx_text(t, s, n);
        return;
    }
    if (n >= 3 && !memcmp(s, "```", 3)) {
        close_block(t);
        tx_outs(t, "<PRE>");
        t->gpre = 1;
        t->gfirst = 1;
        return;
    }
    if (n >= 2 && s[0] == '=' && s[1] == '>') {
        u = skip_space(s, 2, n);
        for (nu = 0; u + nu < n && s[u + nu] != ' ' && s[u + nu] != '\t'; nu++) {}
        if (nu) {
            i = skip_space(s, u + nu, n);
            while (n > i && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
            line_of(t, G_TEXT);
            tx_outs(t, "<A HREF=\"");
            tx_url(t, s + u, nu, 0);
            tx_outs(t, "\">");
            if (i < n) tx_text(t, s + i, n - i);
            else tx_text(t, s + u, nu);
            tx_outs(t, "</A>");
            return;
        }
    }
    if (n >= 1 && s[0] == '#') {
        int level = n >= 3 && s[1] == '#' && s[2] == '#' ? 3 : n >= 2 && s[1] == '#' ? 2 : 1;
        char tag[6] = "<H1>";
        i = skip_space(s, level, n);
        close_block(t);
        tx_title(t, s + i, n - i);
        tag[2] = (char)('0' + level);
        tx_outs(t, tag);
        tx_text(t, s + i, n - i);
        tag[1] = '/';
        tag[2] = 'H';
        tag[3] = (char)('0' + level);
        tag[4] = '>';
        tag[5] = 0;
        tx_outs(t, tag);
        tx_outs(t, "\n");
        return;
    }
    if (n >= 2 && s[0] == '*' && (s[1] == ' ' || s[1] == '\t')) {
        line_of(t, G_LIST);
        i = skip_space(s, 2, n);
        tx_text(t, s + i, n - i);
        return;
    }
    if (n >= 1 && s[0] == '>') {
        line_of(t, G_QUOTE);
        i = skip_space(s, 1, n);
        tx_text(t, s + i, n - i);
        return;
    }
    for (i = 0; i < n && (s[i] == ' ' || s[i] == '\t'); i++) {}
    if (i == n) {
        close_block(t);
        return;
    }
    line_of(t, G_TEXT);
    tx_text(t, s, n);
}

void gem_end(Tx *t)
{
    if (t->gpre) {
        tx_outs(t, "</PRE>\n");    /* never closed */
        t->gpre = 0;
    }
    close_block(t);
}
