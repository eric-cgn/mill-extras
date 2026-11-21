/* json.c: the streaming JSON reader (json.h) */
#include <string.h>

#include "json.h"

enum {
    S_VALUE,        /* a value is wanted */
    S_FIRST,        /* just after '[': a value or ']' */
    S_KEY,          /* in an object: a member name (after ',') */
    S_KEY_FIRST,    /* just after '{': a member name or '}' */
    S_COLON,
    S_AFTER,        /* after a value: ',', or the close */
    S_STRING,
    S_ESC,          /* after '\' */
    S_HEX,          /* in \uXXXX */
    S_NUMBER,
    S_LIT,          /* in true, false, or null */
    S_DONE
};

#define REPLACEMENT 0xFFFDL

int json_utf8(long u, char *out)
{
    unsigned char *o = (unsigned char *)out;
    if (u < 0x80) { o[0] = (unsigned char)u; return 1; }
    if (u < 0x800) { o[0] = (unsigned char)(0xC0 | (u >> 6)); o[1] = (unsigned char)(0x80 | (u & 0x3F)); return 2; }
    if (u < 0x10000) {
        o[0] = (unsigned char)(0xE0 | (u >> 12));
        o[1] = (unsigned char)(0x80 | ((u >> 6) & 0x3F));
        o[2] = (unsigned char)(0x80 | (u & 0x3F));
        return 3;
    }
    o[0] = (unsigned char)(0xF0 | (u >> 18));
    o[1] = (unsigned char)(0x80 | ((u >> 12) & 0x3F));
    o[2] = (unsigned char)(0x80 | ((u >> 6) & 0x3F));
    o[3] = (unsigned char)(0x80 | (u & 0x3F));
    return 4;
}

void json_begin(JsonReader *j, const JsonSink *sink)
{
    memset(j, 0, sizeof *j);
    j->sink = sink;
    j->state = S_VALUE;
}

static int fail(JsonReader *j, int why)
{
    j->state = S_DONE;
    return j->result = why;
}

/* The string's piece so far to the sink (or, for a key, nothing: it's kept) */
static int flush(JsonReader *j, int last)
{
    int r;
    if (j->inkey) return 0;
    r = j->sink->string ? j->sink->string(j->sink->ref, j->key, j->piece, j->plen, last) : 0;
    j->plen = 0;
    return r;
}

/* One whole character of a string, as UTF-8 */
static int put(JsonReader *j, const char *p, int n)
{
    if (j->inkey) {
        if (j->keylen + n < JSON_KEY) {
            memcpy(j->key + j->keylen, p, n);
            j->keylen += n;
        } else j->keylong = 1;
        return 0;
    }
    memcpy(j->piece + j->plen, p, n);
    j->plen += n;
    return j->plen >= JSON_PIECE ? flush(j, 0) : 0;
}

static int put_u(JsonReader *j, long u)
{
    char b[4];
    if (u == 0 || (u >= 0xD800 && u <= 0xDFFF) || u > 0x10FFFF) u = REPLACEMENT;
    return put(j, b, json_utf8(u, b));
}

/* A raw UTF-8 character cut short: U+FFFD for it */
static int bad_utf(JsonReader *j)
{
    j->utfneed = j->utflen = 0;
    return put_u(j, REPLACEMENT);
}

/* A \u high surrogate left waiting for its low half: U+FFFD for it */
static int lone(JsonReader *j)
{
    if (!j->u2) return 0;
    j->u2 = 0;
    return put_u(j, REPLACEMENT);
}

/* A value has ended */
static int after(JsonReader *j)
{
    j->state = j->depth == 0 ? S_DONE : S_AFTER;
    if (j->depth == 0) j->result = JSON_DONE;
    return j->depth == 0 ? JSON_DONE : JSON_MORE;
}

static int opens(JsonReader *j, int kind)
{
    if (j->depth >= JSON_DEPTH) return fail(j, JSON_TOO_DEEP);
    if (j->sink->begin && j->sink->begin(j->sink->ref, kind, j->key)) return fail(j, JSON_STOP);
    j->stack[j->depth++] = (unsigned char)kind;
    j->key[0] = 0;
    j->keylen = 0;
    j->state = kind == JSON_OBJECT ? S_KEY_FIRST : S_FIRST;
    return JSON_MORE;
}

static int closes(JsonReader *j, int kind)
{
    if (j->depth == 0 || j->stack[j->depth - 1] != kind) return fail(j, JSON_ERROR);
    j->depth--;
    if (j->sink->end && j->sink->end(j->sink->ref, kind)) return fail(j, JSON_STOP);
    j->key[0] = 0;
    return after(j);
}

/* -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? */
static int number_ok(const char *s)
{
    if (*s == '-') s++;
    if (*s == '0') s++;
    else if (*s >= '1' && *s <= '9') while (*s >= '0' && *s <= '9') s++;
    else return 0;
    if (*s == '.') {
        if (!(*++s >= '0' && *s <= '9')) return 0;
        while (*s >= '0' && *s <= '9') s++;
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        if (*s == '+' || *s == '-') s++;
        if (!(*s >= '0' && *s <= '9')) return 0;
        while (*s >= '0' && *s <= '9') s++;
    }
    return *s == 0;
}

static int scalar_done(JsonReader *j, int kind)
{
    j->scalar[j->slen] = 0;
    if (kind == JSON_NUMBER && !number_ok(j->scalar)) return fail(j, JSON_ERROR);
    if (j->sink->scalar && j->sink->scalar(j->sink->ref, kind, j->key, j->scalar)) return fail(j, JSON_STOP);
    return after(j);
}

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One byte; JSON_MORE to go on */
static int step(JsonReader *j, int c)
{
    int r;
    switch (j->state) {
    case S_FIRST:
        if (c == ']') return closes(j, JSON_ARRAY);
        /* fall through */
    case S_VALUE:
        if (is_space(c)) return JSON_MORE;
        if (c == '{') return opens(j, JSON_OBJECT);
        if (c == '[') return opens(j, JSON_ARRAY);
        if (c == '"') {
            j->state = S_STRING;
            j->inkey = 0;
            j->plen = 0;
            return JSON_MORE;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            j->state = S_NUMBER;
            j->scalar[0] = (char)c;
            j->slen = 1;
            return JSON_MORE;
        }
        if (c == 't' || c == 'f' || c == 'n') {
            j->lit = c == 't' ? "rue" : c == 'f' ? "alse" : "ull";
            j->scalar[0] = (char)c;
            j->slen = 1;
            j->state = S_LIT;
            return JSON_MORE;
        }
        return fail(j, JSON_ERROR);
    case S_KEY_FIRST:
        if (c == '}') return closes(j, JSON_OBJECT);
        /* fall through */
    case S_KEY:
        if (is_space(c)) return JSON_MORE;
        if (c != '"') return fail(j, JSON_ERROR);
        j->state = S_STRING;
        j->inkey = 1;
        j->keylong = 0;
        j->keylen = 0;
        return JSON_MORE;
    case S_COLON:
        if (is_space(c)) return JSON_MORE;
        if (c != ':') return fail(j, JSON_ERROR);
        j->state = S_VALUE;
        return JSON_MORE;
    case S_AFTER:
        if (is_space(c)) return JSON_MORE;
        if (c == ',') {
            j->state = j->stack[j->depth - 1] == JSON_OBJECT ? S_KEY : S_VALUE;
            if (j->state == S_VALUE) j->key[0] = 0;
            return JSON_MORE;
        }
        if (c == '}') return closes(j, JSON_OBJECT);
        if (c == ']') return closes(j, JSON_ARRAY);
        return fail(j, JSON_ERROR);
    case S_STRING:
        if (j->utfneed) {
            if ((c & 0xC0) == 0x80) {
                long u;
                int k;
                j->utf[j->utflen++] = (unsigned char)c;
                if (--j->utfneed) return JSON_MORE;
                /* whole: check it's the shortest form, and a character */
                u = j->utf[0] & (j->utflen == 2 ? 0x1F : j->utflen == 3 ? 0x0F : 0x07);
                for (k = 1; k < j->utflen; k++) u = (u << 6) | (j->utf[k] & 0x3F);
                j->utflen = 0;
                if ((u < 0x80) || (u < 0x800 && k > 2) || (u < 0x10000 && k > 3) || u > 0x10FFFF ||
                    (u >= 0xD800 && u <= 0xDFFF))
                    return put_u(j, REPLACEMENT) ? fail(j, JSON_STOP) : JSON_MORE;
                return put(j, (const char *)j->utf, k) ? fail(j, JSON_STOP) : JSON_MORE;
            }
            if (bad_utf(j)) return fail(j, JSON_STOP);
        }
        if (c != '\\' && lone(j)) return fail(j, JSON_STOP);
        if (c == '"') {
            if (j->inkey) {
                j->key[j->keylen] = 0;
                if (j->keylong) strcpy(j->key, "?");
                j->inkey = 0;
                j->state = S_COLON;
                return JSON_MORE;
            }
            if (flush(j, 1)) return fail(j, JSON_STOP);
            return after(j);
        }
        if (c == '\\') { j->state = S_ESC; return JSON_MORE; }
        if (c < 0x20) return fail(j, JSON_ERROR);
        if (c < 0x80) {
            char b = (char)c;
            return put(j, &b, 1) ? fail(j, JSON_STOP) : JSON_MORE;
        }
        if (c >= 0xC2 && c <= 0xF4) {
            j->utf[0] = (unsigned char)c;
            j->utflen = 1;
            j->utfneed = c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
            return JSON_MORE;
        }
        return put_u(j, REPLACEMENT) ? fail(j, JSON_STOP) : JSON_MORE;
    case S_ESC: {
        const char *from = "\"\\/bfnrt", *to = "\"\\/\b\f\n\r\t", *e;
        if (c == 'u') {
            j->state = S_HEX;
            j->hex = 0;
            j->u = 0;
            return JSON_MORE;
        }
        if (lone(j)) return fail(j, JSON_STOP);
        if (!c || !(e = strchr(from, c))) return fail(j, JSON_ERROR);
        j->state = S_STRING;
        return put(j, to + (e - from), 1) ? fail(j, JSON_STOP) : JSON_MORE;
    }
    case S_HEX:
        if ((r = hexval(c)) < 0) return fail(j, JSON_ERROR);
        j->u = (j->u << 4) | r;
        if (++j->hex < 4) return JSON_MORE;
        j->state = S_STRING;
        if (j->u2) {
            if (j->u >= 0xDC00 && j->u <= 0xDFFF) {
                long u = 0x10000 + ((j->u2 - 0xD800) << 10) + (j->u - 0xDC00);
                j->u2 = 0;
                return put_u(j, u) ? fail(j, JSON_STOP) : JSON_MORE;
            }
            if (lone(j)) return fail(j, JSON_STOP);
        }
        if (j->u >= 0xD800 && j->u <= 0xDBFF) { j->u2 = j->u; return JSON_MORE; }
        return put_u(j, j->u) ? fail(j, JSON_STOP) : JSON_MORE;
    case S_NUMBER:
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
            if (j->slen >= JSON_SCALAR - 1) return fail(j, JSON_ERROR);
            j->scalar[j->slen++] = (char)c;
            return JSON_MORE;
        }
        if ((r = scalar_done(j, JSON_NUMBER)) != JSON_MORE) return r;
        return step(j, c);      /* what ended it is read as itself */
    case S_LIT:
        if (c != *j->lit) return fail(j, JSON_ERROR);
        j->scalar[j->slen++] = (char)c;
        if (*++j->lit) return JSON_MORE;
        return scalar_done(j, j->scalar[0] == 't' ? JSON_TRUE : j->scalar[0] == 'f' ? JSON_FALSE : JSON_NULL);
    }
    return JSON_DONE;
}

int json_feed(JsonReader *j, const char *p, long n)
{
    long i;
    if (j->state == S_DONE) return j->result;
    for (i = 0; i < n; i++) {
        int r = step(j, (unsigned char)p[i]);
        if (r != JSON_MORE) return r;
    }
    return JSON_MORE;
}

int json_end(JsonReader *j)
{
    if (j->state == S_NUMBER && j->depth == 0) return scalar_done(j, JSON_NUMBER);
    if (j->state == S_DONE) return j->result;
    return fail(j, JSON_ERROR);
}
