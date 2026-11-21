/* mqttutil.h: the parts of the MQTT Plug-In that need no Mac: MQTT 3.1.1's packets (OASIS
   standard, 2014) as this client sends and reads them, topic names and filters, mqtt:
   addresses, and the text checks the pages use. Plain C, so tests/test_mqtt.c runs them on a
   host. */
#ifndef MQTTUTIL_H
#define MQTTUTIL_H

/* Packet types (3.1.1 2.2.1), as the high four bits of the first byte */
enum {
    kMqConnect = 1, kMqConnack = 2, kMqPublish = 3, kMqPuback = 4, kMqPubrec = 5, kMqPubrel = 6,
    kMqPubcomp = 7, kMqSubscribe = 8, kMqSuback = 9, kMqUnsubscribe = 10, kMqUnsuback = 11,
    kMqPingreq = 12, kMqPingresp = 13, kMqDisconnect = 14
};

/* ---- writing packets into a buffer. A packet that won't fit sets full. */
typedef struct {
    unsigned char *p;
    long n, size;
    short full;
} MqOut;
void mq_out(MqOut *o, unsigned char *buf, long size);

/* The Remaining Length (2.2.3): v (0 to 268,435,455) into out, 1 to 4 bytes; returns how
   many, 0 if v is out of range */
short mq_varint(unsigned char out[4], long v);

/* CONNECT (3.1): a clean session; user and password only if user isn't NULL (3.1.2.9) */
void mq_connect(MqOut *o, const char *clientId, const char *user, const char *password, unsigned short keepAlive);
/* SUBSCRIBE (3.8) for one filter at qos; UNSUBSCRIBE (3.10) */
void mq_subscribe(MqOut *o, unsigned short id, const char *filter, short qos);
void mq_unsubscribe(MqOut *o, unsigned short id, const char *filter);
/* PUBLISH (3.3); id only when qos > 0 */
void mq_publish(MqOut *o, const char *topic, const unsigned char *payload, long len, short qos, short retain,
                unsigned short id);
/* PUBACK, PUBREC, PUBREL, and PUBCOMP (3.4 to 3.7): type and the packet's id */
void mq_ack(MqOut *o, short type, unsigned short id);
/* PINGREQ (3.12) and DISCONNECT (3.14) */
void mq_ping(MqOut *o);
void mq_disconnect(MqOut *o);

/* ---- reading packets as they arrive: fill gets more (0 at the end), with ref its own */
typedef struct MqIn {
    unsigned char buf[512];
    long have, at;
    long (*fill)(struct MqIn *in, unsigned char *buf, long size);
    void *ref;
    short bad;          /* ran out, or wasn't MQTT */
} MqIn;
short mq_byte(MqIn *in);
/* The fixed header (2.2): the first byte (type and flags), -1 if bad; *len the Remaining
   Length */
short mq_head(MqIn *in, long *len);
/* len bytes: up to size kept in out, the rest skipped. Returns how many were kept. */
long mq_bytes(MqIn *in, long len, void *out, long size);
void mq_skip(MqIn *in, long len);
long mq_u16(MqIn *in);

/* A PUBLISH's variable header, after its fixed header (head and len): the topic (UTF-8,
   cut to size - 1 and ended with 0; *topicLen its whole length), its QoS, retain, and
   id, and the payload's length, which follows. 0 if it isn't one. */
typedef struct {
    short qos, retain, dup;
    unsigned short id;
    long topicLen;
    long size;          /* the payload's length */
} MqMsg;
short mq_publish_head(MqIn *in, short head, long len, char *topic, long size, MqMsg *m);

/* ---- topics (4.7) */
/* A topic name to publish to: not empty, no wildcards, no 0 bytes, under 65,536 bytes */
short mq_name_ok(const char *t);
/* A topic filter: + and # alone in their levels, # only last */
short mq_filter_ok(const char *f);
short mq_wild(const char *f);
/* Whether topic matches filter, with 4.7.2's rule: a filter starting with a wildcard
   matches no topic starting with $ */
short mq_match(const char *filter, const char *topic);

/* ---- addresses: mqtt://host:port/topic. The path from its slash, up to any query, %XX
   decoded: the topic filter. An empty path, or one that ends with "/", is that level and
   all below it: / is #, /a/b/ is a/b/#. "%23" is #, since a bare # starts a fragment. 0 if
   it won't fit in size. */
short mq_path_topic(const char *path, char *out, long size);
/* The parent level's address path for topic: a/b/c is a/b/, a is "" (the root, #). Into
   out (size), without the leading slash; 0 if it won't fit */
short mq_parent(const char *topic, char *out, long size);

/* ---- text */
/* Whether n bytes are UTF-8 text: valid, with no control characters but tab, CR, and LF */
short mq_text(const unsigned char *s, long n);
/* How many of n bytes of UTF-8 to keep, at most max, so as not to cut a character */
long mq_utf8_cut(const unsigned char *s, long n, long max);
/* Mac Roman to UTF-8 (each line break, CR LF or CR, as LF): the length written, or -1 if
   it won't fit. to_unicode gives a Mac Roman byte's code point (src/core/charset.c). */
long mq_mac_to_utf8(const char *in, long len, char *out, long size, long (*to_unicode)(unsigned char));
/* n bytes as hex for a dump: 16 to a line, "0000  00 01 ...", each line ended with LF.
   The length written, or -1 if it won't fit. */
long mq_hex(const unsigned char *s, long n, char *out, long size);

#endif
