/* content.h: a post's HTML (Mastodon's "content", an account's "note") as the HTML 3.2
   Mill shows, in Mac Roman, into a buffer of the caller's. Plain C, tested on a host.

   Kept: paragraphs and line breaks; links to http:, https:, and mailto: (hashtags and
   mentions go to the Plug-In's own mastodon: pages); bold, italic, code, preformatted text,
   quotes, lists, and struck text. Dropped, keeping their text: every other tag. Dropped
   with their text: Mastodon's "invisible" spans (the hidden parts of long links, with an
   ellipsis where its "ellipsis" spans end), scripts, and styles. A custom emoji's IMG
   becomes its ALT text. Entities are read (the HTML 4 basics and numbers), and the text is
   made Mac Roman by the caller's to_mac.

   The input comes in pieces of whole UTF-8 characters (as json.c gives them). Output that
   won't fit is cut, at a tag or between characters, and cut is set; what's open is closed
   either way, by content_end. */
#ifndef CONTENT_H
#define CONTENT_H

#define CONTENT_TAG 320     /* a tag's text, at most: href and class are read from it */
#define CONTENT_OPEN 8      /* elements open inside each other */
#define CONTENT_SPANS 16
#define CONTENT_RESERVE 128 /* kept free at the end, to close what's open */

/* UTF-8 at in as Mac Roman into out: its length, or -1 if it won't fit */
typedef long (*ContentToMac)(void *ref, const char *in, long len, char *out, long size);

typedef struct {
    char *out;
    long cap, len;
    int cut;                    /* some didn't fit */
    void *ref;
    ContentToMac to_mac;
    const char *host;           /* the instance, for mastodon: links */
    short state;
    char tag[CONTENT_TAG];
    short taglen;
    char quote;                 /* in a tag: the quote an attribute is in */
    char ent[12];
    short entlen;
    char text[72];              /* UTF-8 waiting to be made Mac Roman */
    short textlen;
    unsigned char open[CONTENT_OPEN];
    short nopen;
    unsigned char spans[CONTENT_SPANS];
    short nspans, spanover;
    short hide;                 /* inside this many hidden spans (or a script) */
    char block;                 /* at a block's start: a <P> there would be empty */
    char seen;                  /* some text (not only spaces) went out */
} Content;

void content_begin(Content *c, char *buf, long cap, const char *host, void *ref, ContentToMac to_mac);
void content_data(Content *c, const char *p, long n);
/* Finish: close what's open. The result is c->out, c->len bytes, ended with a 0. */
void content_end(Content *c);

#endif
