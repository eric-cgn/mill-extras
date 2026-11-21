/* mqtt: in the MQTT Plug-In: MQTT 3.1.1 brokers over TCP (port 1883), with addresses
     mqtt://broker/topic       the topic's retained value as a page, and a form to publish
     mqtt://broker/a/+/c       a filter with wildcards: each topic it matches and its value,
                               as a table (a snapshot), collected for the Wait setting
     mqtt://broker/a/b/        a/b/# (a path that ends with "/"); mqtt://broker/ is #
   "%23" stands for #, since a bare # starts a fragment. A page subscribes at QoS 2, takes
   what comes, unsubscribes, and keeps the connection for the next page (TcpKeep). There is
   no live page: Mill shows a page only once its reply has ended.
   The form POSTs to the page's own address: PUBLISH at QoS 0 or 1, retained or not, and
   for a retained one, back to the page, which shows it. Only the Plug-In's own pages may
   publish.
   Anonymous unless a login is kept for the broker or the address gives one; a broker that
   refuses (CONNACK 4 or 5) gets Mill's login dialog. The client ID is made up each time. */
#include <string.h>
#include <Events.h>

#include "MillPlugIn.h"
#include "mqtt.h"
#include "mqttutil.h"
#include "charset.h"            /* Mill's (src/core/charset.h) */

enum {
    NAMEN = 128,        /* a host, a user name, a password */
    TOPIC = 256,        /* a topic or filter kept, UTF-8 */
    PAYLOAD = 4096,     /* a topic page's payload kept */
    SHOWN = 2048,       /* the most published, in bytes of UTF-8, and the most put in the form */
    HEXN = 256,         /* the bytes of a binary payload shown as hex */
    ROWT = 80,          /* a table row's topic kept */
    ROWV = 64,          /* a table row's value kept */
    CHUNK = 160         /* text converted for the page at a time */
};
enum { kLost = -30211, kGarbled = -30212, kDone = 1 };
enum { kKeepAlive = 300 };      /* seconds; the broker drops a kept connection after 1.5 times it */

/* One topic in a table */
typedef struct {
    char topic[ROWT];
    long topicLen;
    char value[ROWV];
    long size;              /* the whole payload's */
    short kept;             /* bytes of value */
    char qos, retain, binary;
} Row;

typedef struct {
    const MillHost *h;
    MillRequest *req;
    MillReqRef r;
    MillConn c;
    short stopped;
    MqIn in;
    unsigned short id;          /* the last packet ID used */
    char host[NAMEN], key[NAMEN + 24], user[NAMEN], pass[NAMEN];
    char filter[TOPIC];         /* UTF-8 */
    short wild;
    /* a topic page's message */
    short got;
    MqMsg msg;
    long kept;
    unsigned char payload[PAYLOAD];
    /* a table's */
    Row *rows;
    short nrows, most, more;
    /* each PUBLISH's topic as it's read */
    char mtopic[TOPIC];
    /* for writing pages */
    char work[CHUNK * 3 + 4];
    char esc[CHUNK * 3 * 6 + 8];
    unsigned char pkt[TOPIC + 64];
} Mq;

#define H (me->h)

/* ------------------------------------------------------------ the page */

static void out(Mq *me, const char *s, long n)
{
    if (!me->stopped && n > 0 && !H->Write(me->r, s, n)) me->stopped = 1;
}

static void outs(Mq *me, const char *s) { out(me, s, strlen(s)); }

static void outf(Mq *me, const char *fmt, long a, long b)
{
    long args[2];
    args[0] = a;
    args[1] = b;
    if (!me->stopped && !H->Printf(me->r, fmt, args)) me->stopped = 1;
}

/* n bytes of UTF-8 as Mac Roman made safe for HTML, a piece at a time */
static void out_text(Mq *me, const char *s, long n)
{
    while (n > 0 && !me->stopped) {
        long k = mq_utf8_cut((const unsigned char *)s, n, CHUNK);
        if (k <= 0) k = n < CHUNK ? n : CHUNK;
        if (H->ToMacRoman(s, k, millUTF8, me->work, sizeof me->work) >= 0 &&
            H->HtmlEscape(me->work, me->esc, sizeof me->esc) >= 0)
            outs(me, me->esc);
        s += k;
        n -= k;
    }
}

/* mqtt://host:port/ then the path, %XX where it needs it, made safe for HTML */
static void out_addr(Mq *me, const char *path)
{
    char enc[TOPIC * 3];
    if (me->req->port) outf(me, "mqtt://%s:%u/", (long)me->host, me->req->port);
    else outf(me, "mqtt://%s/", (long)me->host, 0);
    if (H->UrlEncode(path, enc, sizeof enc) >= 0 && H->HtmlEscape(enc, me->esc, sizeof me->esc) >= 0) outs(me, me->esc);
}

static void page_head(Mq *me, const char *title)
{
    H->PlugInPage(me->r);       /* so its form may publish */
    outs(me, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/html; charset=macintosh" MILL_CRLF MILL_CRLF "<TITLE>" MQTT_TITLE ": ");
    out_text(me, title, strlen(title));
    outs(me, "</TITLE>\n");
}

/* The level above a filter's, as a link: a/b/c's is a/b/, a/b/#'s is a/, and # has none */
static void parent_link(Mq *me)
{
    char t[TOPIC], up[TOPIC];
    long n = strlen(me->filter);
    strcpy(t, me->filter);
    if (n && t[n - 1] == '#') t[--n] = 0;
    if (n && t[n - 1] == '/') t[--n] = 0;
    if (!n) return;
    if (!mq_parent(t, up, sizeof up)) return;
    outf(me, "<IMG SRC=\"plugin://" MQTT_NAME "/rsrc?type=ics%%23&id=%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> <A HREF=\"",
         kMillIconParent, 0);
    out_addr(me, up);
    outs(me, "\">");
    if (up[0]) out_text(me, up, strlen(up));
    else out_text(me, me->host, strlen(me->host));
    outs(me, "</A>\n");
}

/* The form: to publish to this topic, or (on a table) to the topic named */
static void form(Mq *me, const char *topic, const char *value, long n)
{
    outs(me, "<HR><FORM METHOD=POST ACTION=\"");
    out_addr(me, topic ? topic : me->filter);
    outs(me, "\">");
    if (!topic) {
        char t[TOPIC];
        long k = strlen(me->filter);
        strcpy(t, me->filter);
        if (k && t[k - 1] == '#') t[k - 1] = 0;     /* a/b/# offers a/b/ */
        if (mq_wild(t)) t[0] = 0;
        outs(me, "Topic <INPUT NAME=topic SIZE=30 VALUE=\"");
        out_text(me, t, strlen(t));
        outs(me, "\"><BR>\n");
    }
    outs(me, "<TEXTAREA NAME=payload ROWS=3 COLS=40>");
    if (value) out_text(me, value, n);
    outs(me, "</TEXTAREA><BR>\n<INPUT TYPE=CHECKBOX NAME=retain VALUE=yes CHECKED> Retain "
             "QoS <SELECT NAME=qos><OPTION>0<OPTION SELECTED>1</SELECT> <INPUT TYPE=SUBMIT VALUE=\"Publish\"></FORM>\n");
}

/* A payload's size, in bytes or K */
static void out_size(Mq *me, long n)
{
    if (n < 1024) outf(me, n == 1 ? "%ld byte" : "%ld bytes", n, 0);
    else outf(me, "%ldK", (n + 1023) / 1024, 0);
}

/* A topic's page: its value, how it came, and the form */
static void topic_page(Mq *me)
{
    long shown = -1;
    page_head(me, me->filter);
    parent_link(me);
    outs(me, "<H2>");
    out_text(me, me->filter, strlen(me->filter));
    outs(me, "</H2>\n");
    if (!me->got) {
        outs(me, "<P>No retained message.\n");
        form(me, me->filter, NULL, 0);
        return;
    }
    if (!me->msg.size) outs(me, "<P>(empty)\n");
    else {
        /* a payload cut short ends where a character starts */
        long n = me->kept < me->msg.size ? mq_utf8_cut(me->payload, me->kept, me->kept - 1) : me->kept;
        if (mq_text(me->payload, n)) {
            outs(me, "<PRE>");
            out_text(me, (const char *)me->payload, n);
            outs(me, "</PRE>\n");
            if (n < me->msg.size) {
                outs(me, "<P>The first ");
                out_size(me, n);
                outs(me, " of ");
                out_size(me, me->msg.size);
                outs(me, ".\n");
            }
            shown = n;
        } else {
            char *hex = me->esc;        /* 16 bytes make a line of 54 */
            n = me->kept < HEXN ? me->kept : HEXN;
            outs(me, "<P>Binary, ");
            out_size(me, me->msg.size);
            outs(me, n < me->msg.size ? "; the first bytes:\n<PRE>" : ":\n<PRE>");
            if (mq_hex(me->payload, n, hex, sizeof me->esc) >= 0) outs(me, hex);
            outs(me, "</PRE>\n");
        }
    }
    outf(me, "<P><B>QoS:</B> %d<BR><B>Retained:</B> %s<BR><B>Size:</B> ", me->msg.qos, (long)(me->msg.retain ? "yes" : "no"));
    out_size(me, me->msg.size);
    outs(me, "\n");
    /* the form starts with the value, when it's text that can be sent back as it is */
    if (shown > 0 && shown == me->msg.size && shown < SHOWN / 3) form(me, me->filter, (const char *)me->payload, shown);
    else form(me, me->filter, NULL, 0);
}

/* rows' order by topic */
static short row_before(const Row *a, const Row *b)
{
    short c = strcmp(a->topic, b->topic);
    return c < 0 || (c == 0 && a->topicLen < b->topicLen);
}

/* A table of what the filter matched */
static void table_page(Mq *me, short secs)
{
    short i, j, *order;
    long plen = 0;
    const char *f = me->filter;
    long n = strlen(f);
    /* topics under a/b/# are shown from below a/b/ */
    if (n && f[n - 1] == '#') {
        long k;
        for (k = 0; k < n - 1 && f[k] != '+'; k++) {}
        if (k == n - 1) plen = n - 1;
    }
    page_head(me, me->filter);
    parent_link(me);
    outs(me, "<H2>");
    out_text(me, me->filter, n);
    outs(me, "</H2>\n");
    if (!me->nrows) {
        outf(me, secs == 1 ? "<P>No messages in %d second.\n" : "<P>No messages in %d seconds.\n", secs, 0);
        form(me, NULL, NULL, 0);
        return;
    }
    /* the order: an insertion sort of the rows' numbers, in the space after them */
    order = (short *)(me->rows + me->most);
    for (i = 0; i < me->nrows; i++) {
        for (j = i; j > 0 && row_before(&me->rows[i], &me->rows[order[j - 1]]); j--) order[j] = order[j - 1];
        order[j] = i;
    }
    outs(me, "<TABLE BORDER=1 CELLSPACING=0 CELLPADDING=2><TR><TH ALIGN=LEFT>Topic</TH><TH ALIGN=LEFT>Value</TH>"
             "<TH>QoS</TH><TH>Retained</TH></TR>\n");
    for (i = 0; i < me->nrows && !me->stopped; i++) {
        Row *w = &me->rows[order[i]];
        short cut = w->topicLen >= ROWT;
        const char *t = w->topic;
        long tl = strlen(t);
        if (plen && tl >= plen && !memcmp(t, f, plen) && tl > plen) t += plen, tl -= plen;
        outs(me, "<TR><TD VALIGN=TOP>");
        if (!cut) {
            outs(me, "<A HREF=\"");
            out_addr(me, w->topic);
            outs(me, "\">");
        }
        out_text(me, t, tl);
        outs(me, cut ? "\311" : "</A>");     /* the ellipsis, in Mac Roman */
        outs(me, "</TD><TD VALIGN=TOP>");
        if (w->binary) {
            outs(me, "(binary, ");
            out_size(me, w->size);
            outs(me, ")");
        } else if (!w->size) outs(me, "(empty)");
        else {
            out_text(me, w->value, w->kept);
            if (w->kept < w->size) outs(me, "\311");
        }
        outf(me, "</TD><TD ALIGN=CENTER VALIGN=TOP>%d</TD><TD ALIGN=CENTER VALIGN=TOP>%s</TD></TR>\n", w->qos,
             (long)(w->retain ? "yes" : "no"));
    }
    outs(me, "</TABLE>\n");
    if (me->more) outf(me, "<P>The first %d topics.\n", me->nrows, 0);
    else outf(me, me->nrows == 1 ? "<P>%d topic.\n" : "<P>%d topics.\n", me->nrows, 0);
    form(me, NULL, NULL, 0);
}

/* ------------------------------------------------------------ the connection */

static long tcp_fill(MqIn *in, unsigned char *buf, long size)
{
    Mq *me = (Mq *)in->ref;
    long n = size;
    OSErr err = H->TcpRecv(me->c, buf, &n);
    if (err == userCanceledErr) me->stopped = 1;
    if (err != noErr) return 0;
    return n;
}

static OSErr send_out(Mq *me, MqOut *o)
{
    OSErr err;
    if (o->full) return kGarbled;
    err = H->TcpSend(me->c, o->p, o->n);
    if (err == userCanceledErr) me->stopped = 1;
    return err == noErr ? noErr : kLost;
}

static OSErr send_ack(Mq *me, short type, unsigned short id)
{
    MqOut o;
    mq_out(&o, me->pkt, sizeof me->pkt);
    mq_ack(&o, type, id);
    return send_out(me, &o);
}

static unsigned short next_id(Mq *me)
{
    if (!++me->id) me->id = 1;
    return me->id;
}

enum { kIgnore = 0, kTopic = 1, kTable = 2 };

/* A PUBLISH's payload, for the page or not */
static void take(Mq *me, short how)
{
    long size = me->msg.size;
    if (how == kTopic && !me->got && me->msg.topicLen < TOPIC && !strcmp(me->mtopic, me->filter)) {
        me->kept = mq_bytes(&me->in, size, me->payload, PAYLOAD);
        me->got = 1;
        return;
    }
    if (how == kTable && mq_match(me->filter, me->mtopic)) {
        short i;
        Row *w = NULL;
        unsigned char peek[ROWV];
        long kept = mq_bytes(&me->in, size, peek, ROWV), n;
        for (i = 0; i < me->nrows; i++) {
            if (me->rows[i].topicLen == me->msg.topicLen && !strncmp(me->rows[i].topic, me->mtopic, ROWT - 1)) {
                w = &me->rows[i];
                break;
            }
        }
        if (!w) {
            if (me->nrows >= me->most) { me->more = 1; return; }
            w = &me->rows[me->nrows++];
            strncpy(w->topic, me->mtopic, ROWT - 1);
            w->topic[ROWT - 1] = 0;
            w->topicLen = me->msg.topicLen;
        }
        /* the latest value wins */
        n = kept < size ? mq_utf8_cut(peek, kept, kept - 1) : kept;
        w->binary = !mq_text(peek, n);
        n = mq_utf8_cut(peek, n, ROWV - 1);
        memcpy(w->value, peek, n);
        w->value[n] = 0;
        w->kept = (short)n;
        w->size = size;
        w->qos = (char)me->msg.qos;
        w->retain = (char)me->msg.retain;
        return;
    }
    mq_skip(&me->in, size);
}

/* The next packet: a PUBLISH is taken (how) and acknowledged, a PUBREL answered. Its type,
   or kLost (and stopped set if it was Stop) */
static short next(Mq *me, short how, long *len, short *first)
{
    long n;
    short head = mq_head(&me->in, &n), type;
    *len = n;
    *first = head;
    if (head < 0) return kLost;
    type = head >> 4;
    if (type == kMqPublish) {
        if (!mq_publish_head(&me->in, head, n, me->mtopic, sizeof me->mtopic, &me->msg)) return kGarbled;
        take(me, how);
        if (me->in.bad) return kLost;
        if (me->msg.qos == 1 && send_ack(me, kMqPuback, me->msg.id) != noErr) return kLost;
        if (me->msg.qos == 2 && send_ack(me, kMqPubrec, me->msg.id) != noErr) return kLost;
        return type;
    }
    if (type == kMqPubrel) {
        long id = mq_u16(&me->in);
        if (id < 0 || n != 2) return kLost;
        if (send_ack(me, kMqPubcomp, (unsigned short)id) != noErr) return kLost;
        return type;
    }
    return type;
}

/* Read packets until one of type wait; its remaining length is left unread (in *len) */
static short until(Mq *me, short how, short wait, long *len)
{
    short t, first;
    for (;;) {
        t = next(me, how, len, &first);
        if (t < 0) return t;
        if (t == wait) return t;
        if (t != kMqPublish && t != kMqPubrel) mq_skip(&me->in, *len);    /* PUBACK and the rest: not ours */
    }
}

/* The rest of a SUBACK, UNSUBACK, PUBACK, or PINGRESP; its first return code for SUBACK */
static short ack_id(Mq *me, long len, unsigned short id, short suback)
{
    long got = mq_u16(&me->in);
    short code = 0;
    if (suback) code = mq_byte(&me->in);
    if (len > (suback ? 3 : 2)) mq_skip(&me->in, len - (suback ? 3 : 2));
    if (me->in.bad || got != id) return -1;
    return code;
}

/* Ask for a name and password: first (wrong 0), or again after the broker refused them */
static void ask(Mq *me, short wrong)
{
    char prompt[NAMEN + 64];
    long a[1];
    a[0] = (long)me->host;
    me->req->loginKey[0] = 0;
    if (wrong) outs(me, "HTTP/1.0 401 Unauthorized" MILL_CRLF "Content-Type: text/html" MILL_CRLF MILL_CRLF
                        "<TITLE>Login failed</TITLE><H2>Login failed</H2><P>The broker refused the name or password.");
    H->Format(prompt, sizeof prompt, wrong ? "The name or password for %s was wrong. Try again?" : "Log in to %s.", a);
    H->AskLogin(me->r, prompt, NULL, NULL, me->key, me->user[0] ? me->user : NULL, NULL);
}

/* Mac Roman text (a typed name, a field) as UTF-8 in place; 0 if it won't fit */
static short to_utf8(char *s, long size)
{
    char t[NAMEN * 3];
    long n = mq_mac_to_utf8(s, strlen(s), t, sizeof t - 1, mac_to_unicode);
    if (n < 0 || n >= size) return 0;
    memcpy(s, t, n);
    s[n] = 0;
    return 1;
}

/* CONNECT and its CONNACK (3.2.2.3): noErr, kDone when it has said why not, or an error */
static OSErr log_in(Mq *me)
{
    MqOut o;
    char id[24], u[NAMEN], p[NAMEN];
    long a[2], len;
    short first, code;
    static unsigned long count;
    a[0] = (long)TickCount();
    a[1] = (long)++count;
    H->Format(id, sizeof id, "mill-%lx-%lx", a);
    strcpy(u, me->user);
    strcpy(p, me->pass);
    if (!to_utf8(u, sizeof u) || !to_utf8(p, sizeof p)) { memset(p, 0, sizeof p); return kGarbled; }
    mq_out(&o, me->pkt, sizeof me->pkt);
    mq_connect(&o, id, u[0] ? u : NULL, u[0] && p[0] ? p : NULL, kKeepAlive);
    memset(p, 0, sizeof p);
    H->Status(me->r, "Logging in\311");
    if (send_out(me, &o) != noErr) { memset(me->pkt, 0, sizeof me->pkt); return kLost; }
    memset(me->pkt, 0, sizeof me->pkt);
    first = mq_head(&me->in, &len);
    if (first < 0) return kLost;
    if (first >> 4 != kMqConnack || len != 2) return kGarbled;
    mq_byte(&me->in);               /* Session Present: 0, a clean session */
    code = mq_byte(&me->in);
    if (code < 0) return kLost;
    switch (code) {
    case 0: return noErr;
    case 1: H->Fail(me->r, "The broker doesn\325t support MQTT 3.1.1."); return kDone;
    case 2: H->Fail(me->r, "The broker refused Mill\325s client ID."); return kDone;
    case 3: H->Fail(me->r, "The broker is unavailable."); return kDone;
    case 4: ask(me, me->user[0] != 0); return kDone;
    case 5:
        if (!me->user[0]) ask(me, 0);
        else H->Fail(me->r, "The broker refused access.");
        return kDone;
    }
    a[0] = code;
    H->Format(u, sizeof u, "The broker refused the connection (code %d).", a);
    H->Fail(me->r, u);
    return kDone;
}

/* Wait ticks, the windmill turning; 0 if the person stopped */
static short wait_ticks(Mq *me, unsigned long ticks)
{
    unsigned long until_t = TickCount() + ticks;
    while (TickCount() < until_t) {
        if (H->Busy(me->r)) { me->stopped = 1; return 0; }
    }
    return 1;
}

/* ------------------------------------------------------------ reading a topic */

static OSErr read_topic(Mq *me, short secs)
{
    MqOut o;
    long len;
    short code, t, how = me->wild ? kTable : kTopic;
    unsigned short id;
    unsigned long deadline;
    char msg[TOPIC + 64];
    long a[1];
    if (me->wild) {
        if (!(me->rows = H->NewMem((long)me->most * (sizeof(Row) + sizeof(short))))) return memFullErr;
        me->nrows = 0;
        me->more = 0;
    }
    /* SUBSCRIBE at QoS 2, so each message comes at the QoS it was published with */
    id = next_id(me);
    mq_out(&o, me->pkt, sizeof me->pkt);
    mq_subscribe(&o, id, me->filter, 2);
    if (send_out(me, &o) != noErr) return kLost;
    H->Status(me->r, "Reading\311");
    if ((t = until(me, how, kMqSuback, &len)) != kMqSuback) return t;
    if ((code = ack_id(me, len, id, 1)) < 0) return kGarbled;
    if (code == 0x80) {
        a[0] = (long)me->filter;
        H->Format(msg, sizeof msg, "The broker refused to subscribe to %s.", a);
        H->Fail(me->r, msg);
        return kDone;
    }
    /* What's retained comes after the SUBACK. A PINGREQ's PINGRESP comes after whatever the
       broker sent before it, so each round reads all that's in so far; then a pause, till
       the topic's value is in, or a filter's time is up, or the table is full. */
    deadline = TickCount() + secs * 60UL;
    for (;;) {
        mq_out(&o, me->pkt, sizeof me->pkt);
        mq_ping(&o);
        if (send_out(me, &o) != noErr) return kLost;
        if ((t = until(me, how, kMqPingresp, &len)) != kMqPingresp) return t;
        mq_skip(&me->in, len);
        if (me->got || me->more || TickCount() >= deadline) break;
        if (!wait_ticks(me, 15)) return userCanceledErr;
    }
    /* UNSUBSCRIBE, and what comes till its UNSUBACK is dropped: then the connection is at rest */
    id = next_id(me);
    mq_out(&o, me->pkt, sizeof me->pkt);
    mq_unsubscribe(&o, id, me->filter);
    if (send_out(me, &o) != noErr) return kLost;
    if ((t = until(me, kIgnore, kMqUnsuback, &len)) != kMqUnsuback) return t;
    if (ack_id(me, len, id, 0) < 0) return kGarbled;
    return noErr;
}

/* ------------------------------------------------------------ publishing */

/* A form field, or "" */
static void field(Mq *me, const char *body, long n, const char *name, char *out_s, long size)
{
    if (H->FormField(body, n, name, out_s, size) < 0) out_s[0] = 0;
}

static OSErr publish(Mq *me, const char *body, long n)
{
    MqOut o;
    char v[8], topic[TOPIC];
    char *text = NULL, *utf = NULL;
    unsigned char *pkt = NULL;
    short retain, qos, t;
    long len, ulen;
    unsigned short id = 0;
    OSErr err = noErr;
    field(me, body, n, "retain", v, sizeof v);
    retain = !strcmp(v, "yes");
    field(me, body, n, "qos", v, sizeof v);
    qos = v[0] == '1';
    field(me, body, n, "topic", topic, sizeof topic);
    if (topic[0]) {
        if (!to_utf8(topic, sizeof topic)) { H->Fail(me->r, "The topic is too long."); return kDone; }
        strcpy(me->filter, topic);
    }
    if (!mq_name_ok(me->filter)) { H->Fail(me->r, "Enter a topic to publish to, without + or #."); return kDone; }
    if (!(text = H->NewMem(SHOWN + 1)) || !(utf = H->NewMem(SHOWN)) || !(pkt = H->NewMem(SHOWN + TOPIC + 16))) {
        err = memFullErr;
        goto done;
    }
    if (H->FormField(body, n, "payload", text, SHOWN + 1) < 0) text[0] = 0;
    if ((long)strlen(text) >= SHOWN || (ulen = mq_mac_to_utf8(text, strlen(text), utf, SHOWN, mac_to_unicode)) < 0) {
        H->Fail(me->r, "The message is too long: 2K maximum.");
        err = kDone;
        goto done;
    }
    if (qos) id = next_id(me);
    mq_out(&o, pkt, SHOWN + TOPIC + 16);
    mq_publish(&o, me->filter, (unsigned char *)utf, ulen, qos, retain, id);
    H->Status(me->r, "Publishing\311");
    if ((err = send_out(me, &o)) != noErr) goto done;
    if (qos) {
        if ((t = until(me, kIgnore, kMqPuback, &len)) != kMqPuback) { err = t; goto done; }
        if (ack_id(me, len, id, 0) < 0) { err = kGarbled; goto done; }
    }
    /* and a PINGREQ: once its PINGRESP is in, the broker has the message (and keeps it, if
       retained, for the page that follows) */
    mq_out(&o, me->pkt, sizeof me->pkt);
    mq_ping(&o);
    if ((err = send_out(me, &o)) != noErr) goto done;
    if ((t = until(me, kIgnore, kMqPingresp, &len)) != kMqPingresp) { err = t; goto done; }
    mq_skip(&me->in, len);
    if (retain) {
        /* back to the page, which shows it */
        outs(me, "HTTP/1.0 303 See Other" MILL_CRLF "Location: ");
        outs(me, me->req->url);
        outs(me, MILL_CRLF MILL_CRLF);
    } else {
        page_head(me, me->filter);
        outs(me, "<H2>Published</H2>\n<P>");
        out_size(me, ulen);
        outf(me, " at QoS %d, not retained, to <A HREF=\"", qos, 0);
        out_addr(me, me->filter);
        outs(me, "\">");
        out_text(me, me->filter, strlen(me->filter));
        outs(me, "</A>.\n");
    }
done:
    if (text) H->FreeMem(text);
    if (utf) H->FreeMem(utf);
    if (pkt) H->FreeMem(pkt);
    return err;
}

/* ------------------------------------------------------------ a request */

static long number(const char *s)
{
    long n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return n;
}

/* The POST's body, whole (it's a form): its length, or -1 if it's over size */
static long read_body(Mq *me, char *buf, long size)
{
    long got = 0, n;
    for (;;) {
        n = size - got;
        if (n <= 0) return -1;
        if (H->ReadBody(me->r, buf + got, &n) != noErr) return -1;
        if (!n) break;
        got += n;
    }
    buf[got] = 0;
    return got;
}

static OSErr request(Mq *me, MillRequest *r)
{
    char v[16];
    long a[2];
    short post = r->len >= 5 && !memcmp(r->text, "POST ", 5), secs;
    OSErr err;
    char *body = NULL;
    long blen = 0;
    me->req = r;
    me->r = r->ref;
    me->in.fill = tcp_fill;
    me->in.ref = me;
    strncpy(me->host, r->host, NAMEN - 1);
    me->host[NAMEN - 1] = 0;
    /* the topic, from the address: UTF-8 if it is, else typed in Mac Roman */
    if (!mq_path_topic(r->path, me->filter, sizeof me->filter) ||
        (!mq_text((unsigned char *)me->filter, strlen(me->filter)) && !to_utf8(me->filter, sizeof me->filter))) {
        H->Fail(me->r, "The topic is too long.");
        return noErr;
    }
    if (!mq_filter_ok(me->filter)) {
        H->Fail(me->r, "Not a valid topic: + and # must be whole levels, and # only at the end.");
        return noErr;
    }
    me->wild = mq_wild(me->filter);
    secs = H->GetSetting("mqtt", "wait", v, sizeof v) > 0 ? (short)number(v) : 2;
    if (secs < 1) secs = 1;
    if (secs > 30) secs = 30;
    me->most = H->GetSetting("mqtt", "rows", v, sizeof v) > 0 ? (short)number(v) : 100;
    if (me->most < 10) me->most = 10;
    if (me->most > 250) me->most = 250;
    if (post) {
        /* only its own page publishes */
        if (H->From(me->r) != millFromOwnPage) { H->Fail(me->r, MQTT_TITLE ": Only its own page can publish."); return noErr; }
        if (!(body = H->NewMem(SHOWN * 3 + 512))) return memFullErr;
        if ((blen = read_body(me, body, SHOWN * 3 + 511)) < 0) {
            H->Fail(me->r, "The message is too long: 2K maximum.");
            H->FreeMem(body);
            return noErr;
        }
    }
    /* who: the address's login, else the one kept for this broker (its key: mqtt://host:port),
       else anonymous */
    a[0] = (long)r->host;
    a[1] = r->port ? r->port : 1883;
    H->Format(me->key, sizeof me->key, "mqtt://%s:%d", a);
    if (r->user[0]) {
        strncpy(me->user, r->user, NAMEN - 1);
        strncpy(me->pass, r->password, NAMEN - 1);
    } else H->SavedLogin(me->key, me->user, sizeof me->user, me->pass, sizeof me->pass);
    /* the connection kept from the last page, logged in as the same user: its note is the
       last packet ID. Not for publishing: a kept connection that's gone is asked again only
       for a GET. */
    if (!post) {
        char note[12];
        long n = sizeof note - 1;
        if (H->TcpKept(me->r, me->user, &me->c, note, &n) == noErr) {
            note[n] = 0;
            me->id = (unsigned short)number(note);
            memset(me->pass, 0, sizeof me->pass);
            goto in;
        }
        me->c = 0;
    }
    if ((err = H->TcpOpen(me->r, r->host, r->port ? r->port : 1883, &me->c)) != noErr) {
        me->c = 0;
        memset(me->pass, 0, sizeof me->pass);
        goto quit;
    }
    err = log_in(me);
    memset(me->pass, 0, sizeof me->pass);
    if (err != noErr) goto quit;
    if (r->user[0]) strcpy(r->loginKey, me->key);     /* the address's login worked: Mill keeps it */
in:
    err = post ? publish(me, body, blen) : read_topic(me, secs);
    if (err == noErr && !post) {
        if (me->wild) table_page(me, secs);
        else topic_page(me);
    }
quit:
    if (me->c && err == noErr && !me->stopped) {
        /* every reply read: kept for the next page, with a DISCONNECT for Mill to send when
           it closes it */
        char note[12];
        MqOut o;
        a[0] = me->id;
        H->Format(note, sizeof note, "%ld", a);
        mq_out(&o, me->pkt, sizeof me->pkt);
        mq_disconnect(&o);
        H->TcpKeep(me->r, me->c, me->user, note, strlen(note), o.p, o.n);
        me->c = 0;
    }
    if (me->c) {
        MqOut o;
        if (!me->stopped) {
            mq_out(&o, me->pkt, sizeof me->pkt);
            mq_disconnect(&o);
            send_out(me, &o);
        }
        H->TcpClose(me->c);
        me->c = 0;
    }
    if (body) H->FreeMem(body);
    if (me->rows) H->FreeMem(me->rows);
    if (me->stopped && (err == kLost || err == kGarbled)) err = userCanceledErr;
    if (err == kLost || err == kGarbled) {
        H->Fail(me->r, err == kLost ? "The connection was lost." : "The broker\325s reply was not understood.");
        err = noErr;
    }
    return err > 0 ? noErr : err;
}

OSErr mqtt_request(const MillHost *host, MillRequest *r)
{
    Mq *me = host->NewMem(sizeof *me);
    OSErr err;
    if (!me) return memFullErr;
    memset(me, 0, sizeof *me);
    me->h = host;
    err = request(me, r);
    memset(me, 0, sizeof *me);      /* the password was here */
    host->FreeMem(me);
    return err;
}

static const MillSetting settings[] = {
    { "wait", "Wait", "Seconds to collect messages for a wildcard table", millNumber, "2", 1, 30, NULL },
    { "rows", "Maximum topics", "Per wildcard table", millNumber, "100", 10, 250, NULL }
};
static const MillProtocol protocols[] = { { "mqtt", "MQTT", "Message Queuing Telemetry Transport (MQTT 3.1.1)", 0, 2, settings } };

const MillProtocol *mqtt_protocol(void) { return protocols; }
