/* txint.h: what text.c, gemtext.c, markdown.c, and inline.c share */
#ifndef TXINT_H
#define TXINT_H

#include "text.h"

/* A line handed to a format: flags */
enum { TX_CONT = 1 };       /* it continues the line before, which was too long and cut */

/* Marks in held output (TX_HOLD): a reference link's start and end, then its slot + 1
   and a byte that says what to write */
#define TX_MARK_OPEN 1
#define TX_MARK_CLOSE 2
enum { TX_LINK = 'L', TX_IMAGE = 'I' };                         /* after TX_MARK_OPEN */
enum { TX_FULL = 'F', TX_COLLAPSED = 'C', TX_SHORTCUT = 'S' };  /* after TX_MARK_CLOSE */

/* A link reference definition, or a label that waits for one */
typedef struct {
    short label, nlabel;    /* in pool: the label as first written */
    short url, nurl;        /* in pool: the destination, as written (escapes and all) */
    char defined;
    char waiting;           /* used in held output, not yet defined */
    char missed;            /* held output went out without it: listed at the end */
} TxRef;

typedef struct Md Md;       /* markdown.c's */

typedef struct {
    TxSink s;
    short kind, cs;
    short stopped;
    short started;          /* the META is out */
    /* the line being read */
    char *raw;
    long nraw;
    char cr;                /* the last piece ended with CR: an LF next is part of it */
    char cut;               /* the line before was cut: this one continues it */
    char *line;             /* the line as Mac Roman, tabs at its start made spaces */
    char *conv;
    /* output */
    char *hold;             /* NULL for Gemtext */
    long nhold;
    short waiting;          /* labels waiting: held while nonzero */
    char *cap;              /* a title being made (capbuf), or NULL */
    long ncap;
    char capbuf[128];
    short titled;
    TxRef *refs;
    short nrefs, npool;
    char *pool;
    /* Gemtext */
    short gmode, gpre, gfirst;
    Md *md;
    void *toks;             /* inline.c's marks, TX_TOKENS of them */
} Tx;

/* text.c: output. tx_out writes as it is; tx_text escapes < > & "; tx_url writes an
   address for an attribute (backslash escapes undone when md, & kept for an entity,
   space and " as %20 and %22). */
void tx_out(Tx *t, const char *p, long n);
void tx_outs(Tx *t, const char *s);
void tx_text(Tx *t, const char *p, long n);
void tx_url(Tx *t, const char *p, long n, int md);
void tx_title(Tx *t, const char *p, long n);     /* <TITLE>, for the first heading only */
/* whether s[0..n) starts an entity: &name; &#123; &#x1F; (its length, else 0) */
int tx_entity(const char *s, long n);

/* References: the slot for a label (compared without case, runs of white space as one),
   -1 if none; tx_ref_add makes one (for a definition, or one to wait for), -1 if full */
int tx_ref_find(Tx *t, const char *label, long n);
int tx_ref_add(Tx *t, const char *label, long n);
void tx_ref_define(Tx *t, int slot, const char *url, long n);
void tx_hold_flush(Tx *t, int all);   /* write what's held; all: with the waiting ones as text */
/* a reference link's start (open) or end, for slot: held until it's defined, or written
   as it stands (a link if defined, else the text) */
void tx_mark(Tx *t, int open, int slot, char what, char form);

/* gemtext.c, markdown.c */
void gem_line(Tx *t, const char *s, long n, int flags);
void gem_end(Tx *t);
Md *md_new(Tx *t);
void md_line(Tx *t, char *s, long n, int flags);
void md_end(Tx *t);
long md_size(void);
long md_tok_size(void);  /* inline.c: the room for TX_TOKENS marks */

/* inline.c: a paragraph's (or heading's, or cell's) text as HTML */
void md_inline(Tx *t, const char *s, long n);
/* the same, as plain text for a title */
void md_inline_title(Tx *t, const char *s, long n);
/* raw HTML at s[0] ('<'): its length, or 0; *kind 1 for an open or closing tag, 2 for
   a comment, processing instruction, declaration, or CDATA */
long md_raw_html(const char *s, long n, int *kind);
/* link reference definitions at the start of a paragraph: what they take up */
long md_refdefs(Tx *t, const char *s, long n);

/* Characters, Mac Roman */
#define TX_SPACE(c) ((c) == ' ' || (c) == '\t' || (c) == '\n')
int tx_punct(unsigned char c);

#endif
