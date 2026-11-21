/* json.h: a streaming JSON reader (RFC 8259) for the Mastodon Plug-In. Plain C with no
   Mac calls and no memory of its own beyond its struct, so tests/test_mastodon.c runs it
   on a host.

   The input comes in pieces of any size and is never kept: what it says goes to a
   JsonSink as it's read. A string goes out in pieces too, decoded (escapes undone, as
   UTF-8), each piece whole characters only; a string of any length takes no more room
   than one piece. Invalid UTF-8, and \u escapes that aren't characters (a lone
   surrogate), become U+FFFD. Keys longer than JSON_KEY - 1 bytes are given as "?", so
   they match nothing. Containers nest JSON_DEPTH deep at most; deeper is an error. */
#ifndef JSON_H
#define JSON_H

#define JSON_KEY 32         /* a member name's bytes, with its 0 */
#define JSON_DEPTH 32       /* objects and arrays inside each other */
#define JSON_PIECE 128      /* a string's piece, at most */
#define JSON_SCALAR 40      /* a number's text, at most */

/* What a value is */
enum { JSON_OBJECT = 1, JSON_ARRAY, JSON_STRING, JSON_NUMBER, JSON_TRUE, JSON_FALSE, JSON_NULL };

/* Each returns 0 to go on, anything else to stop the reading (json_feed: JSON_STOP).
   key is the value's member name in its object, "" in an array or at the top. */
typedef struct {
    void *ref;
    int (*begin)(void *ref, int kind, const char *key);     /* an object or array opens */
    int (*end)(void *ref, int kind);                        /* and closes */
    /* a number, true, false, or null; text is its JSON text ("12", "true") */
    int (*scalar)(void *ref, int kind, const char *key, const char *text);
    /* a string, a piece at a time: n bytes at p, and last set on its final piece
       (which may be empty) */
    int (*string)(void *ref, const char *key, const char *p, long n, int last);
} JsonSink;

enum { JSON_MORE = 0, JSON_DONE = 1, JSON_STOP = 2, JSON_ERROR = -1, JSON_TOO_DEEP = -2 };

typedef struct {
    const JsonSink *sink;
    short state, depth;
    unsigned char stack[JSON_DEPTH];    /* JSON_OBJECT or JSON_ARRAY */
    char key[JSON_KEY];                 /* the member name being read, then the value's */
    short keylen;
    char inkey;                         /* the string being read is a member name */
    char keylong;                       /* ... and it didn't fit */
    long u, u2;                         /* a \u escape's value, and a high surrogate before it */
    short hex;                          /* its hex digits so far */
    unsigned char utf[4];               /* a raw UTF-8 character being read */
    short utflen, utfneed;
    char piece[JSON_PIECE + 8];
    short plen;
    char scalar[JSON_SCALAR];
    short slen;
    const char *lit;                    /* the rest of true, false, or null */
    int result;                         /* once done or failed, what json_feed returns */
} JsonReader;

void json_begin(JsonReader *j, const JsonSink *sink);
/* The next n bytes. JSON_MORE: go on; JSON_DONE: the value at the top has ended (what
   follows it is ignored); JSON_STOP: the sink said to; JSON_ERROR or JSON_TOO_DEEP. */
int json_feed(JsonReader *j, const char *p, long n);
/* The input has ended: JSON_DONE if the value was whole, else JSON_ERROR (a number at the
   very end is finished here) */
int json_end(JsonReader *j);

/* UTF-8 for the character u into out (4 bytes): its length */
int json_utf8(long u, char *out);

#endif
