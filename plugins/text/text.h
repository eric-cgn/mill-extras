/* text.h: Gemtext and Markdown as HTML, for the Modern Text Plug-In. Plain C with no Mac
   calls, so tests/test_text.c runs it on a host: the Plug-In gives it Mill's memory, Write,
   and ToMacRoman through a TxSink.

   The input arrives in pieces of any size and is read a line at a time; the HTML goes out
   as each block ends. What is kept: one line, one Markdown paragraph, the open blocks, the
   reference definitions, and output held while a reference link waits for its definition
   (see inline.c). All of it is allocated at the start, so the most a run takes is known:
   tx_memory(). */
#ifndef TEXT_H
#define TEXT_H

#define TX_LINE 1024        /* a line's bytes; a longer line is cut into pieces */
#define TX_PARA 4096        /* a Markdown paragraph (or table row, or heading) */
#define TX_HOLD 4096        /* output held for reference links defined further on */
#define TX_REFS 48          /* reference definitions (and labels waiting for one) */
#define TX_POOL 2560        /* their labels and destinations */
#define TX_TOKENS 128       /* a paragraph's inline marks at a time */
#define TX_DEPTH 24         /* block quotes and lists inside each other */

enum { TX_GEMTEXT = 0, TX_MARKDOWN = 1 };

/* Character sets, as MillPlugIn.h's ToMacRoman takes them */
enum { TX_GUESS = 0, TX_MAC = 1, TX_LATIN1 = 2, TX_UTF8 = 3 };

typedef struct {
    void *ref;
    void *(*alloc)(void *ref, long n);      /* NULL if there's no room */
    void (*release)(void *ref, void *p);
    int (*write)(void *ref, const char *p, long n);    /* 0: no more wanted */
    /* in (charset cs) as Mac Roman into out; the length, or -1 if it won't fit */
    long (*to_mac)(void *ref, const char *in, long len, short cs, char *out, long size);
} TxSink;

enum { TX_MORE = 0, TX_STOP = 1, TX_NO_MEMORY = -1 };

/* The charset a content type names ("text/gemini; charset=iso-8859-1"): UTF-8 when it
   names none (Gemini's default, and what Markdown files are), TX_GUESS for one Mill
   doesn't know */
short tx_charset(const char *type);

/* A run: NULL if there's no memory. tx_data takes the next piece (TX_MORE, or TX_STOP when
   no more is wanted); tx_end finishes the page, closing what's open, and frees the run. */
void *tx_begin(int kind, short charset, const TxSink *s);
int tx_data(void *run, const char *p, long n);
void tx_end(void *run);

/* The bytes a run of kind allocates */
long tx_memory(int kind);

#endif
