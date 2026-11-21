/* mqttutil: MQTT 3.1.1's packets, topics, and addresses, for the MQTT Plug-In (mqttutil.h).
   Written from the OASIS standard, MQTT Version 3.1.1 (29 October 2014). */
#include <string.h>

#include "mqttutil.h"

/* ------------------------------------------------------------ writing */

void mq_out(MqOut *o, unsigned char *buf, long size)
{
    o->p = buf;
    o->n = 0;
    o->size = size;
    o->full = 0;
}

short mq_varint(unsigned char out[4], long v)
{
    short n = 0;
    if (v < 0 || v > 268435455L) return 0;
    do {
        unsigned char b = (unsigned char)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        out[n++] = b;
    } while (v);
    return n;
}

static void put(MqOut *o, const void *p, long n)
{
    if (o->full || o->n + n > o->size) { o->full = 1; return; }
    memcpy(o->p + o->n, p, n);
    o->n += n;
}

static void put_byte(MqOut *o, unsigned char b) { put(o, &b, 1); }

static void put_u16(MqOut *o, unsigned short v)
{
    unsigned char b[2];
    b[0] = (unsigned char)(v >> 8);
    b[1] = (unsigned char)v;
    put(o, b, 2);
}

/* A UTF-8 string (1.5.3): its length, then its bytes */
static void put_str(MqOut *o, const char *s)
{
    long n = strlen(s);
    if (n > 65535L) { o->full = 1; return; }
    put_u16(o, (unsigned short)n);
    put(o, s, n);
}

static long str_len(const char *s) { return 2 + (long)strlen(s); }

/* The fixed header: type and flags, then the Remaining Length */
static void fixed(MqOut *o, unsigned char first, long rest)
{
    unsigned char v[4];
    short n = mq_varint(v, rest);
    if (!n) { o->full = 1; return; }
    put_byte(o, first);
    put(o, v, n);
}

void mq_connect(MqOut *o, const char *clientId, const char *user, const char *password, unsigned short keepAlive)
{
    unsigned char flags = 0x02;     /* Clean Session */
    long rest = 10 + str_len(clientId);
    if (user) {
        flags |= 0x80;
        rest += str_len(user);
        if (password) { flags |= 0x40; rest += str_len(password); }
    }
    fixed(o, kMqConnect << 4, rest);
    put_str(o, "MQTT");
    put_byte(o, 4);                 /* the protocol level: 3.1.1 */
    put_byte(o, flags);
    put_u16(o, keepAlive);
    put_str(o, clientId);
    if (user) {
        put_str(o, user);
        if (password) put_str(o, password);
    }
}

void mq_subscribe(MqOut *o, unsigned short id, const char *filter, short qos)
{
    fixed(o, kMqSubscribe << 4 | 0x02, 2 + str_len(filter) + 1);     /* 3.8.1: flags 0010 */
    put_u16(o, id);
    put_str(o, filter);
    put_byte(o, (unsigned char)qos);
}

void mq_unsubscribe(MqOut *o, unsigned short id, const char *filter)
{
    fixed(o, kMqUnsubscribe << 4 | 0x02, 2 + str_len(filter));
    put_u16(o, id);
    put_str(o, filter);
}

void mq_publish(MqOut *o, const char *topic, const unsigned char *payload, long len, short qos, short retain,
                unsigned short id)
{
    fixed(o, (unsigned char)(kMqPublish << 4 | (qos & 3) << 1 | (retain ? 1 : 0)), str_len(topic) + (qos ? 2 : 0) + len);
    put_str(o, topic);
    if (qos) put_u16(o, id);
    put(o, payload, len);
}

void mq_ack(MqOut *o, short type, unsigned short id)
{
    fixed(o, (unsigned char)(type << 4 | (type == kMqPubrel ? 0x02 : 0)), 2);     /* 3.6.1: PUBREL's flags 0010 */
    put_u16(o, id);
}

void mq_ping(MqOut *o) { fixed(o, kMqPingreq << 4, 0); }
void mq_disconnect(MqOut *o) { fixed(o, kMqDisconnect << 4, 0); }

/* ------------------------------------------------------------ reading */

short mq_byte(MqIn *in)
{
    if (in->bad) return -1;
    if (in->at >= in->have) {
        in->at = 0;
        in->have = in->fill(in, in->buf, sizeof in->buf);
        if (in->have <= 0) { in->have = 0; in->bad = 1; return -1; }
    }
    return in->buf[in->at++];
}

short mq_head(MqIn *in, long *len)
{
    short first = mq_byte(in), i;
    long v = 0, mult = 1;
    *len = 0;
    if (first < 0) return -1;
    for (i = 0; i < 4; i++) {
        short b = mq_byte(in);
        if (b < 0) return -1;
        v += (b & 0x7F) * mult;
        mult <<= 7;
        if (!(b & 0x80)) { *len = v; return first; }
    }
    in->bad = 1;        /* a fifth byte: malformed (2.2.3) */
    return -1;
}

long mq_bytes(MqIn *in, long len, void *out, long size)
{
    unsigned char *o = out;
    long kept = 0;
    while (len > 0 && !in->bad) {
        long n;
        if (in->at >= in->have && mq_byte(in) >= 0) in->at--;    /* refill */
        if (in->bad) break;
        n = in->have - in->at;
        if (n > len) n = len;
        if (kept < size) {
            long k = n < size - kept ? n : size - kept;
            memcpy(o + kept, in->buf + in->at, k);
            kept += k;
        }
        in->at += n;
        len -= n;
    }
    return kept;
}

void mq_skip(MqIn *in, long len) { mq_bytes(in, len, NULL, 0); }

long mq_u16(MqIn *in)
{
    short a = mq_byte(in), b = mq_byte(in);
    if (a < 0 || b < 0) return -1;
    return (long)a << 8 | b;
}

short mq_publish_head(MqIn *in, short head, long len, char *topic, long size, MqMsg *m)
{
    long tl, kept;
    memset(m, 0, sizeof *m);
    if ((head >> 4) != kMqPublish) return 0;
    m->retain = head & 1;
    m->qos = (head >> 1) & 3;
    m->dup = (head >> 3) & 1;
    if (m->qos == 3 || len < 2) return 0;
    tl = mq_u16(in);
    if (tl < 0 || tl + 2 + (m->qos ? 2 : 0) > len) return 0;
    kept = mq_bytes(in, tl, topic, size - 1);
    topic[kept] = 0;
    m->topicLen = tl;
    if (m->qos) {
        long id = mq_u16(in);
        if (id < 0) return 0;
        m->id = (unsigned short)id;
    }
    m->size = len - 2 - tl - (m->qos ? 2 : 0);
    return !in->bad;
}

/* ------------------------------------------------------------ topics */

short mq_name_ok(const char *t)
{
    long n = strlen(t);
    if (!n || n > 65535L) return 0;
    return !strpbrk(t, "+#");
}

short mq_filter_ok(const char *f)
{
    long n = strlen(f), i;
    if (!n || n > 65535L) return 0;
    for (i = 0; i < n; i++) {
        short start = i == 0 || f[i - 1] == '/', end = f[i + 1] == 0 || f[i + 1] == '/';
        if (f[i] == '+' && !(start && end)) return 0;
        if (f[i] == '#' && !(start && f[i + 1] == 0)) return 0;
    }
    return 1;
}

short mq_wild(const char *f) { return strpbrk(f, "+#") != NULL; }

short mq_match(const char *f, const char *t)
{
    if ((*f == '+' || *f == '#') && *t == '$') return 0;
    for (;;) {
        if (*f == '#') return 1;        /* this level and all below, or the parent itself (4.7.1.2) */
        if (*f == '+') {
            while (*t && *t != '/') t++;
            f++;
        } else {
            while (*f && *f != '/' && *f == *t) f++, t++;
            if ((*f && *f != '/') || (*t && *t != '/')) return 0;
        }
        /* both at a level's end */
        if (!*f && !*t) return 1;
        if (!*f) return 0;
        if (!*t) return f[0] == '/' && f[1] == '#' && !f[2];   /* "a/#" matches "a" */
        f++, t++;
    }
}

/* ------------------------------------------------------------ addresses */

static short hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

short mq_path_topic(const char *path, char *out, long size)
{
    long o = 0;
    const char *p = path;
    if (*p == '/') p++;
    for (; *p && *p != '?'; p++) {
        char c = *p;
        if (c == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            c = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 2;
        }
        if (o >= size - 1) return 0;
        out[o++] = c;
    }
    if (!o || out[o - 1] == '/') {
        if (o >= size - 1) return 0;
        out[o++] = '#';
    }
    out[o] = 0;
    return 1;
}

short mq_parent(const char *topic, char *out, long size)
{
    const char *slash = strrchr(topic, '/');
    long n = slash ? slash - topic + 1 : 0;
    if (n >= size) return 0;
    memcpy(out, topic, n);
    out[n] = 0;
    return 1;
}

/* ------------------------------------------------------------ text */

/* The length of the UTF-8 character at s (n bytes left), 0 if it isn't one */
static long utf8_len(const unsigned char *s, long n)
{
    long k, i, u;
    if (s[0] < 0x80) return 1;
    if (s[0] < 0xC2) return 0;
    k = s[0] < 0xE0 ? 2 : s[0] < 0xF0 ? 3 : s[0] < 0xF5 ? 4 : 0;
    if (!k || k > n) return 0;
    u = s[0] & (0x7F >> k);
    for (i = 1; i < k; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        u = u << 6 | (s[i] & 0x3F);
    }
    if ((k == 3 && u < 0x800) || (k == 4 && (u < 0x10000 || u > 0x10FFFF)) || (u >= 0xD800 && u <= 0xDFFF)) return 0;
    return k;
}

short mq_text(const unsigned char *s, long n)
{
    long i = 0;
    while (i < n) {
        long k = utf8_len(s + i, n - i);
        if (!k) return 0;
        if (k == 1 && ((s[i] < 0x20 && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') || s[i] == 0x7F)) return 0;
        i += k;
    }
    return 1;
}

long mq_utf8_cut(const unsigned char *s, long n, long max)
{
    long i;
    if (n <= max) return n;
    i = max;
    while (i > 0 && (s[i] & 0xC0) == 0x80) i--;     /* back to the start of the character cut */
    return i;
}

long mq_mac_to_utf8(const char *in, long len, char *out, long size, long (*to_unicode)(unsigned char))
{
    long i, o = 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        long u;
        if (c == '\r') {
            if (i + 1 < len && in[i + 1] == '\n') i++;
            c = '\n';
        }
        u = c < 0x80 ? c : to_unicode(c);
        if (u < 0x80) {
            if (o + 1 > size) return -1;
            out[o++] = (char)u;
        } else if (u < 0x800) {
            if (o + 2 > size) return -1;
            out[o++] = (char)(0xC0 | u >> 6);
            out[o++] = (char)(0x80 | (u & 0x3F));
        } else {
            if (o + 3 > size) return -1;
            out[o++] = (char)(0xE0 | u >> 12);
            out[o++] = (char)(0x80 | (u >> 6 & 0x3F));
            out[o++] = (char)(0x80 | (u & 0x3F));
        }
    }
    return o;
}

long mq_hex(const unsigned char *s, long n, char *out, long size)
{
    static const char digits[] = "0123456789abcdef";
    long i, o = 0;
    for (i = 0; i < n; i++) {
        if (i % 16 == 0) {
            if (o + 6 > size) return -1;
            out[o++] = digits[(i >> 12) & 15];
            out[o++] = digits[(i >> 8) & 15];
            out[o++] = digits[(i >> 4) & 15];
            out[o++] = digits[i & 15];
            out[o++] = ' ';
        }
        if (o + 3 > size) return -1;
        out[o++] = ' ';
        out[o++] = digits[s[i] >> 4];
        out[o++] = digits[s[i] & 15];
        if (i % 16 == 15 || i == n - 1) {
            if (o + 1 > size) return -1;
            out[o++] = '\n';
        }
    }
    if (o >= size) return -1;
    out[o] = 0;
    return o;
}
