/* Host tests for the MQTT Plug-In's packets and topics (mqttutil.c): the Remaining Length
   both ways (MQTT 3.1.1 2.2.3, with its table's edges), every packet the Plug-In sends byte
   for byte, PUBLISH read a byte at a time, topic filters and matching (4.7, with its
   examples), addresses, and the text checks. */
#include <stdio.h>
#include <string.h>

#include "../mqttutil.h"
#include "charset.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int hex_is(const unsigned char *b, long n, const char *hex)
{
    char s[4000];
    long i;
    for (i = 0; i < n && i < 1990; i++) snprintf(s + 2 * i, 3, "%02x", b[i]);
    s[2 * i] = 0;
    if (strcmp(s, hex)) { printf("  got %s\n  not %s\n", s, hex); return 0; }
    return 1;
}

static unsigned char buf[70000];
static MqOut o;

/* Input from a buffer, step bytes at a time */
static const unsigned char *src;
static long src_n, src_at, step;
static long fill(MqIn *in, unsigned char *b, long size)
{
    long n = src_n - src_at;
    (void)in;
    if (n > step) n = step;
    if (n > size) n = size;
    memcpy(b, src + src_at, n);
    src_at += n;
    return n;
}
static void from(MqIn *in, const unsigned char *s, long n, long st)
{
    memset(in, 0, sizeof *in);
    in->fill = fill;
    src = s;
    src_n = n;
    src_at = 0;
    step = st;
}

static void test_varint(void)
{
    static const long values[] = { 0, 127, 128, 16383, 16384, 2097151, 2097152, 268435455L };
    static const char *const hex[] = { "00", "7f", "8001", "ff7f", "808001", "ffff7f", "80808001", "ffffff7f" };
    unsigned char v[4];
    short i;
    for (i = 0; i < 8; i++) {
        short n = mq_varint(v, values[i]);
        CHECK(hex_is(v, n, hex[i]));
        /* and back, as a fixed header's Remaining Length */
        {
            unsigned char p[5];
            MqIn in;
            long len;
            p[0] = 0xD0;
            memcpy(p + 1, v, n);
            from(&in, p, n + 1, 1);
            CHECK(mq_head(&in, &len) == 0xD0 && len == values[i]);
        }
    }
    CHECK(mq_varint(v, 268435456L) == 0);
    CHECK(mq_varint(v, -1) == 0);
    /* a fifth byte is malformed */
    {
        static const unsigned char bad[] = { 0x30, 0xff, 0xff, 0xff, 0xff, 0x01 };
        MqIn in;
        long len;
        from(&in, bad, sizeof bad, 6);
        CHECK(mq_head(&in, &len) == -1 && in.bad);
    }
}

static void test_packets(void)
{
    mq_out(&o, buf, sizeof buf);
    mq_connect(&o, "mill-1", NULL, NULL, 300);
    /* 10, length 18: "MQTT", level 4, flags 02 (clean), keep alive 300, client ID */
    CHECK(hex_is(buf, o.n, "1012" "00044d515454" "04" "02" "012c" "00066d696c6c2d31"));
    mq_out(&o, buf, sizeof buf);
    mq_connect(&o, "c", "u", "pw", 0);
    CHECK(hex_is(buf, o.n, "1014" "00044d515454" "04" "c2" "0000" "000163" "000175" "00027077"));
    mq_out(&o, buf, sizeof buf);
    mq_connect(&o, "c", "u", NULL, 60);        /* a user with no password: 3.1.2.9 */
    CHECK(hex_is(buf, o.n, "1010" "00044d515454" "04" "82" "003c" "000163" "000175"));
    mq_out(&o, buf, sizeof buf);
    mq_subscribe(&o, 10, "a/#", 2);
    CHECK(hex_is(buf, o.n, "8208" "000a" "0003612f23" "02"));
    mq_out(&o, buf, sizeof buf);
    mq_unsubscribe(&o, 11, "a/#");
    CHECK(hex_is(buf, o.n, "a207" "000b" "0003612f23"));
    mq_out(&o, buf, sizeof buf);
    mq_publish(&o, "a/b", (const unsigned char *)"hi", 2, 0, 0, 0);
    CHECK(hex_is(buf, o.n, "3007" "0003612f62" "6869"));
    mq_out(&o, buf, sizeof buf);
    mq_publish(&o, "a/b", (const unsigned char *)"hi", 2, 1, 1, 5);
    CHECK(hex_is(buf, o.n, "3309" "0003612f62" "0005" "6869"));
    mq_out(&o, buf, sizeof buf);
    mq_ack(&o, kMqPuback, 0x1234);
    mq_ack(&o, kMqPubrec, 1);
    mq_ack(&o, kMqPubrel, 2);
    mq_ack(&o, kMqPubcomp, 3);
    CHECK(hex_is(buf, o.n, "40021234" "50020001" "62020002" "70020003"));
    mq_out(&o, buf, sizeof buf);
    mq_ping(&o);
    mq_disconnect(&o);
    CHECK(hex_is(buf, o.n, "c000" "e000"));
    /* a publish whose length takes two bytes */
    {
        static unsigned char pay[300];
        memset(pay, 'x', sizeof pay);
        mq_out(&o, buf, sizeof buf);
        mq_publish(&o, "t", pay, 300, 0, 1, 0);
        CHECK(o.n == 1 + 2 + 3 + 300 && buf[0] == 0x31 && buf[1] == ((303 & 0x7F) | 0x80) && buf[2] == 303 >> 7);
    }
    /* too small: full, never past the end */
    mq_out(&o, buf, 5);
    mq_publish(&o, "a/b", (const unsigned char *)"hi", 2, 0, 0, 0);
    CHECK(o.full && o.n <= 5);
}

static void test_read(void)
{
    /* a SUBACK, a QoS 1 retained PUBLISH, a PINGRESP, each read 1 byte at a time and all at once */
    static const unsigned char in_bytes[] = {
        0x90, 0x03, 0x00, 0x0a, 0x02,
        0x33, 0x0c, 0x00, 0x03, 'a', '/', 'b', 0x00, 0x07, 'h', 'e', 'l', 'l', 'o',
        0xd0, 0x00
    };
    long st;
    for (st = 1; st <= (long)sizeof in_bytes; st += sizeof in_bytes - 1) {
        MqIn in;
        long len;
        char topic[16], pay[16];
        MqMsg m;
        short head;
        from(&in, in_bytes, sizeof in_bytes, st);
        CHECK(mq_head(&in, &len) == 0x90 && len == 3);
        CHECK(mq_u16(&in) == 10 && mq_byte(&in) == 2);
        head = mq_head(&in, &len);
        CHECK(head == 0x33 && len == 12);
        CHECK(mq_publish_head(&in, head, len, topic, sizeof topic, &m));
        CHECK(!strcmp(topic, "a/b") && m.topicLen == 3 && m.qos == 1 && m.retain && m.id == 7 && m.size == 5);
        CHECK(mq_bytes(&in, m.size, pay, 3) == 3 && !memcmp(pay, "hel", 3));    /* kept 3, the rest skipped */
        CHECK(mq_head(&in, &len) == 0xd0 && len == 0);
        CHECK(mq_head(&in, &len) == -1 && in.bad);      /* the end */
    }
    /* a topic longer than the room: cut, its whole length given, the payload still found */
    {
        static const unsigned char p[] = { 0x30, 0x0a, 0x00, 0x06, 'a', 'b', 'c', 'd', 'e', 'f', 'x', 'y' };
        MqIn in;
        long len;
        char topic[4], pay[4];
        MqMsg m;
        short head;
        from(&in, p, sizeof p, 3);
        head = mq_head(&in, &len);
        CHECK(mq_publish_head(&in, head, len, topic, sizeof topic, &m));
        CHECK(!strcmp(topic, "abc") && m.topicLen == 6 && m.qos == 0 && m.size == 2);
        CHECK(mq_bytes(&in, m.size, pay, sizeof pay) == 2 && !memcmp(pay, "xy", 2));
    }
    /* a topic that runs past the packet, and QoS 3: not a PUBLISH */
    {
        static const unsigned char p[] = { 0x30, 0x03, 0x00, 0x09, 'a' };
        static const unsigned char q[] = { 0x36, 0x03, 0x00, 0x01, 'a' };
        MqIn in;
        long len;
        char topic[8];
        MqMsg m;
        short head;
        from(&in, p, sizeof p, 5);
        head = mq_head(&in, &len);
        CHECK(!mq_publish_head(&in, head, len, topic, sizeof topic, &m));
        from(&in, q, sizeof q, 5);
        head = mq_head(&in, &len);
        CHECK(!mq_publish_head(&in, head, len, topic, sizeof topic, &m));
    }
}

static void test_topics(void)
{
    /* filters (4.7.1) */
    CHECK(mq_filter_ok("#"));
    CHECK(mq_filter_ok("a/#"));
    CHECK(mq_filter_ok("+"));
    CHECK(mq_filter_ok("+/+"));
    CHECK(mq_filter_ok("/+"));
    CHECK(mq_filter_ok("sport/+/player1"));
    CHECK(mq_filter_ok("a/b"));
    CHECK(mq_filter_ok("/"));
    CHECK(!mq_filter_ok(""));
    CHECK(!mq_filter_ok("a#"));
    CHECK(!mq_filter_ok("a/#/b"));
    CHECK(!mq_filter_ok("sport+"));
    CHECK(!mq_filter_ok("a/b+/c"));
    CHECK(mq_name_ok("a/b") && mq_name_ok("/") && !mq_name_ok("") && !mq_name_ok("a/+") && !mq_name_ok("#"));
    CHECK(mq_wild("a/+") && mq_wild("#") && !mq_wild("a/b"));
    /* matching: 4.7.1.2's and 4.7.1.3's examples */
    CHECK(mq_match("sport/tennis/player1/#", "sport/tennis/player1"));
    CHECK(mq_match("sport/tennis/player1/#", "sport/tennis/player1/ranking"));
    CHECK(mq_match("sport/tennis/player1/#", "sport/tennis/player1/score/wimbledon"));
    CHECK(mq_match("sport/#", "sport"));
    CHECK(mq_match("#", "sport/tennis"));
    CHECK(mq_match("sport/tennis/+", "sport/tennis/player1"));
    CHECK(mq_match("sport/tennis/+", "sport/tennis/player2"));
    CHECK(!mq_match("sport/tennis/+", "sport/tennis/player1/ranking"));
    CHECK(!mq_match("sport/+", "sport"));
    CHECK(mq_match("sport/+", "sport/"));
    CHECK(mq_match("+/+", "/finance"));
    CHECK(mq_match("/+", "/finance"));
    CHECK(!mq_match("+", "/finance"));
    CHECK(mq_match("a/b", "a/b") && !mq_match("a/b", "a/bc") && !mq_match("a/bc", "a/b") && !mq_match("a/b", "a"));
    CHECK(mq_match("+/b/+", "a/b/c") && !mq_match("+/b/+", "a/x/c"));
    /* 4.7.2: wildcards first never match $ topics */
    CHECK(!mq_match("#", "$SYS/broker"));
    CHECK(!mq_match("+/monitor/Clients", "$SYS/monitor/Clients"));
    CHECK(mq_match("$SYS/#", "$SYS/monitor/Clients"));
    CHECK(mq_match("$SYS/monitor/+", "$SYS/monitor/Clients"));
}

static void test_addresses(void)
{
    char t[64];
    CHECK(mq_path_topic("/home/kitchen/temperature", t, sizeof t) && !strcmp(t, "home/kitchen/temperature"));
    CHECK(mq_path_topic("/", t, sizeof t) && !strcmp(t, "#"));
    CHECK(mq_path_topic("", t, sizeof t) && !strcmp(t, "#"));
    CHECK(mq_path_topic("/home/", t, sizeof t) && !strcmp(t, "home/#"));
    CHECK(mq_path_topic("/home/%23", t, sizeof t) && !strcmp(t, "home/#"));
    CHECK(mq_path_topic("/home/+/temperature", t, sizeof t) && !strcmp(t, "home/+/temperature"));
    CHECK(mq_path_topic("//x", t, sizeof t) && !strcmp(t, "/x"));
    CHECK(mq_path_topic("/a%20b/c?live", t, sizeof t) && !strcmp(t, "a b/c"));
    CHECK(mq_path_topic("/caf%C3%A9", t, sizeof t) && !strcmp(t, "caf\xc3\xa9"));
    CHECK(mq_path_topic("/50%", t, sizeof t) && !strcmp(t, "50%"));
    CHECK(!mq_path_topic("/abcdefgh", t, 8));
    CHECK(mq_parent("a/b/c", t, sizeof t) && !strcmp(t, "a/b/"));
    CHECK(mq_parent("a", t, sizeof t) && !strcmp(t, ""));
    CHECK(mq_parent("/a", t, sizeof t) && !strcmp(t, "/"));
}

static void test_text(void)
{
    char out[256];
    long n;
    CHECK(mq_text((const unsigned char *)"21.5", 4));
    CHECK(mq_text((const unsigned char *)"a\tb\r\nc", 6));
    CHECK(mq_text((const unsigned char *)"caf\xc3\xa9 \xe2\x84\xa2 \xf0\x9f\x98\x80", 14));
    CHECK(mq_text((const unsigned char *)"", 0));
    CHECK(!mq_text((const unsigned char *)"a\0b", 3));
    CHECK(!mq_text((const unsigned char *)"\x01\x02", 2));
    CHECK(!mq_text((const unsigned char *)"\x7f", 1));
    CHECK(!mq_text((const unsigned char *)"caf\xe9", 4));             /* Latin-1, not UTF-8 */
    CHECK(!mq_text((const unsigned char *)"\xc0\x80", 2));            /* overlong */
    CHECK(!mq_text((const unsigned char *)"\xed\xa0\x80", 3));        /* a surrogate */
    CHECK(!mq_text((const unsigned char *)"\xe2\x84", 2));            /* cut short */
    CHECK(mq_utf8_cut((const unsigned char *)"ab\xe2\x84\xa2", 5, 3) == 2);
    CHECK(mq_utf8_cut((const unsigned char *)"ab\xe2\x84\xa2", 5, 5) == 5);
    CHECK(mq_utf8_cut((const unsigned char *)"abcdef", 6, 4) == 4);
    /* Mac Roman to UTF-8: e acute (0x8E), the trademark (0xAA), and CR LF and CR as LF */
    n = mq_mac_to_utf8("caf\x8e\r\n\xaa\r", 8, out, sizeof out, mac_to_unicode);
    CHECK(n == 10 && !memcmp(out, "caf\xc3\xa9\n\xe2\x84\xa2\n", 10));
    CHECK(mq_mac_to_utf8("\xaa", 1, out, 2, mac_to_unicode) == -1);
    n = mq_hex((const unsigned char *)"\x00\x01\xff", 3, out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "0000  00 01 ff\n"));
    {
        unsigned char b[17];
        short i;
        for (i = 0; i < 17; i++) b[i] = (unsigned char)i;
        n = mq_hex(b, 17, out, sizeof out);
        CHECK(n > 0 && !strcmp(out, "0000  00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f\n0010  10\n"));
        CHECK(mq_hex(b, 17, out, 20) == -1);
    }
}

int main(void)
{
    test_varint();
    test_packets();
    test_read();
    test_topics();
    test_addresses();
    test_text();
    if (failures) { printf("%d failed\n", failures); return 1; }
    printf("mqtt: all passed\n");
    return 0;
}
