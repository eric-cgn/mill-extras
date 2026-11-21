/* Host tests for the MQTT Plug-In's requests (mqtt.c), end to end: a MillHost of its own
   (sockets for MacTCP; Format, ToMacRoman, FormField, and the rest as Mill's own do them),
   against tests/broker.py on 127.0.0.1, ports 18830 (anyone) and 18831 (tester / secret),
   seeded with tests/seed.txt. Each test is a request, and its page or what it said. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "MillPlugIn.h"
#include "../mqtt.h"
#include "../mqttutil.h"
#include "charset.h"
#include "fmt.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

unsigned long TickCount(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)t.tv_sec * 60 + (unsigned long)t.tv_nsec / 16666667L;
}

/* ------------------------------------------------------------ the request, and what came of it */

struct MillRequestRef { int unused; };
static struct MillRequestRef the_ref;
static char page[200000];
static long page_n;
static char failed[300], asked[300], asked_key[300];
static short marked, from_own, opened, kept_given;
static const char *body;
static long body_at;
static char set_wait[8] = "1", set_rows[8] = "100";
static char saved_user[64], saved_pass[64], saved_key[128];
static long mem_now, mem_most;

/* the kept connection: one, as for one window */
static int keep_fd = -1;
static char keep_login[64], keep_note[64];
static int fds[8];

static pascal void *h_new_mem(long n)
{
    long *p = malloc(n + sizeof(long) * 2);
    if (!p) return NULL;
    p[0] = n;
    mem_now += n;
    if (mem_now > mem_most) mem_most = mem_now;
    return p + 2;
}
static pascal void h_free_mem(void *p)
{
    if (!p) return;
    mem_now -= ((long *)p)[-2];
    free((long *)p - 2);
}
static pascal void h_status(MillReqRef r, const char *t) { (void)r; (void)t; }
static pascal short h_busy(MillReqRef r) { (void)r; usleep(2000); return 0; }

static pascal OSErr h_tcp_open(MillReqRef r, const char *host, unsigned short port, MillConn *conn)
{
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_STREAM, 0), i;
    (void)r;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -23048; }
    for (i = 0; i < 8 && fds[i]; i++) {}
    fds[i] = fd;
    *conn = fd;
    opened++;
    return noErr;
}
static pascal OSErr h_tcp_send(MillConn c, const void *d, long n) { return send((int)c, d, n, 0) == n ? noErr : -23008; }
static pascal OSErr h_tcp_recv(MillConn c, void *b, long *n)
{
    long got = recv((int)c, b, *n, 0);
    *n = got > 0 ? got : 0;
    return got >= 0 ? noErr : -23008;
}
static void drop(int fd)
{
    int i;
    for (i = 0; i < 8; i++) if (fds[i] == fd) fds[i] = 0;
}
static pascal void h_tcp_close(MillConn c) { drop((int)c); close((int)c); }
static pascal OSErr h_tcp_kept(MillReqRef r, const char *login, MillConn *conn, void *note, long *len)
{
    int i;
    (void)r;
    *conn = 0;
    if (keep_fd < 0 || strcmp(login, keep_login)) { *len = 0; return fnfErr; }
    *len = strlen(keep_note);
    memcpy(note, keep_note, *len);
    *conn = keep_fd;
    for (i = 0; i < 8 && fds[i]; i++) {}
    fds[i] = keep_fd;
    keep_fd = -1;
    kept_given++;
    return noErr;
}
static int keeping = -1;
static char keeping_login[64], keeping_note[64];
static pascal void h_tcp_keep(MillReqRef r, MillConn c, const char *login, const void *note, long n, const void *bye, long bn)
{
    (void)r;
    CHECK(bn == 2 && !memcmp(bye, "\xe0\x00", 2));      /* DISCONNECT */
    keeping = (int)c;
    snprintf(keeping_login, sizeof keeping_login, "%s", login);
    snprintf(keeping_note, sizeof keeping_note, "%.*s", (int)n, (const char *)note);
}
static pascal short h_write(MillReqRef r, const void *d, long n)
{
    (void)r;
    if (page_n + n >= (long)sizeof page) return 0;
    memcpy(page + page_n, d, n);
    page_n += n;
    page[page_n] = 0;
    return 1;
}
static pascal OSErr h_proxy(MillReqRef r) { (void)r; return millNoProxy; }
static pascal long h_get_setting(const char *scheme, const char *key, char *out, long size)
{
    const char *v = !strcmp(key, "wait") ? set_wait : !strcmp(key, "rows") ? set_rows : NULL;
    if (strcmp(scheme, "mqtt") || !v) return -1;
    snprintf(out, size, "%s", v);
    return strlen(out);
}
static pascal OSErr h_set_setting(const char *s, const char *k, const char *v) { (void)s; (void)k; (void)v; return noErr; }
static pascal long h_format(char *out, long size, const char *fmt, const long *args) { return mill_format(out, size, fmt, args); }
static pascal short h_printf(MillReqRef r, const char *fmt, const long *args)
{
    char s[4096];
    long n = mill_format(s, sizeof s, fmt, args);
    return h_write(r, s, n < (long)sizeof s ? n : (long)sizeof s - 1);
}
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
/* as Mill's form_field (src/mac_local.c) */
static pascal long h_form_field(const char *form, long len, const char *name, char *out, long size)
{
    long i = 0, nl = strlen(name);
    while (i < len) {
        long end = i;
        while (end < len && form[end] != '&') end++;
        if (end - i > nl && !memcmp(form + i, name, nl) && form[i + nl] == '=') {
            long k, o = 0;
            for (k = i + nl + 1; k < end && o < size - 1; k++) {
                int c = (unsigned char)form[k];
                if (c == '+') c = ' ';
                else if (c == '%' && k + 2 < end && hexval(form[k + 1]) >= 0 && hexval(form[k + 2]) >= 0) {
                    c = hexval(form[k + 1]) * 16 + hexval(form[k + 2]);
                    k += 2;
                }
                out[o++] = (char)latin1_to_mac((unsigned char)c);
            }
            out[o] = 0;
            return o;
        }
        i = end + 1;
    }
    return -1;
}
static pascal long h_html_escape(const char *in, char *out, long size)
{
    long o = 0;
    for (; *in; in++) {
        const char *e = *in == '&' ? "&amp;" : *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '"' ? "&quot;" : NULL;
        long n = e ? (long)strlen(e) : 1;
        if (o + n >= size) return -1;
        if (e) memcpy(out + o, e, n);
        else out[o] = *in;
        o += n;
    }
    out[o] = 0;
    return o;
}
static pascal short h_from(MillReqRef r) { (void)r; return from_own ? millFromOwnPage : millFromOther; }
static pascal long h_url_encode(const char *in, char *out, long size)
{
    static const char digits[] = "0123456789ABCDEF";
    long o = 0;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (c <= ' ' || c >= 127 || strchr("%\"<>#?", c)) {
            if (o + 3 >= size) return -1;
            out[o++] = '%';
            out[o++] = digits[c >> 4];
            out[o++] = digits[c & 15];
        } else {
            if (o + 1 >= size) return -1;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return o;
}
/* as Mill's h_to_mac_roman (src/mac_plugin.c) */
static pascal long h_to_mac_roman(const char *in, long len, short cs, char *out, long size)
{
    const unsigned char *s = (const unsigned char *)in;
    long i = 0, o = 0;
    if (cs != millLatin1 && cs != millUTF8) cs = charset_sniff(in, len) == CHARSET_UTF8 ? millUTF8 : millLatin1;
    while (i < len) {
        unsigned char c;
        long u;
        int k;
        if (cs == millUTF8 && s[i] >= 0x80 && (k = utf8_char(s + i, len - i, &u)) > 0) { c = unicode_to_mac(u); i += k; }
        else c = s[i] < 0x80 ? s[i] : latin1_to_mac(s[i]), i++;
        if (!c) continue;
        if (o + 1 >= size) return -1;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return o;
}
static pascal void h_plugin_page(MillReqRef r) { (void)r; marked = 1; }
static pascal OSErr h_read_body(MillReqRef r, void *buf, long *len)
{
    long n = body ? (long)strlen(body) - body_at : 0;
    (void)r;
    if (n > *len) n = *len;
    if (n > 0) memcpy(buf, body + body_at, n);
    body_at += n > 0 ? n : 0;
    *len = n > 0 ? n : 0;
    return noErr;
}
static pascal void h_fail(MillReqRef r, const char *t) { (void)r; snprintf(failed, sizeof failed, "%s", t); }
static pascal void h_ask_login(MillReqRef r, const char *prompt, const char *nl, const char *pl, const char *key,
                               const char *dn, const char *dp)
{
    (void)r; (void)nl; (void)pl; (void)dn; (void)dp;
    snprintf(asked, sizeof asked, "%s", prompt);
    snprintf(asked_key, sizeof asked_key, "%s", key);
}
static pascal short h_saved_login(const char *key, char *name, long ns, char *pw, long ps)
{
    if (strcmp(key, saved_key)) return 0;
    snprintf(name, ns, "%s", saved_user);
    snprintf(pw, ps, "%s", saved_pass);
    return 1;
}

static MillHost host;

static void host_init(void)
{
    host.size = sizeof host;
    host.version = kMillInterface;
    host.NewMem = h_new_mem;
    host.FreeMem = h_free_mem;
    host.Status = h_status;
    host.Busy = h_busy;
    host.TcpOpen = h_tcp_open;
    host.TcpSend = h_tcp_send;
    host.TcpRecv = h_tcp_recv;
    host.TcpClose = h_tcp_close;
    host.TcpKept = h_tcp_kept;
    host.TcpKeep = h_tcp_keep;
    host.Write = h_write;
    host.ProxyRequest = h_proxy;
    host.GetSetting = h_get_setting;
    host.SetSetting = h_set_setting;
    host.Format = h_format;
    host.Printf = h_printf;
    host.FormField = h_form_field;
    host.HtmlEscape = h_html_escape;
    host.From = h_from;
    host.UrlEncode = h_url_encode;
    host.ToMacRoman = h_to_mac_roman;
    host.PlugInPage = h_plugin_page;
    host.ReadBody = h_read_body;
    host.Fail = h_fail;
    host.AskLogin = h_ask_login;
    host.SavedLogin = h_saved_login;
}

/* One request, as Mill makes it: GET, or POST with a form body; then the request's end, as
   Mill's: what it keeps is kept, the rest closed */
static OSErr get(const char *method, unsigned short port, const char *path, const char *user, const char *pass,
                 const char *form)
{
    MillRequest r;
    char text[512], url[512];
    OSErr err;
    int i;
    memset(&r, 0, sizeof r);
    snprintf(text, sizeof text, "%s %s HTTP/1.0\r\n", method, path);
    snprintf(url, sizeof url, "mqtt://127.0.0.1:%u%s", port, path);
    r.ref = &the_ref;
    r.text = text;
    r.len = strlen(text);
    r.url = url;
    r.scheme = "mqtt";
    r.user = user ? user : "";
    r.password = pass ? pass : "";
    r.host = "127.0.0.1";
    r.port = port;
    r.path = path;
    page_n = 0;
    page[0] = 0;
    failed[0] = asked[0] = 0;
    marked = 0;
    opened = kept_given = 0;
    body = form;
    body_at = 0;
    keeping = -1;
    mem_now = mem_most = 0;
    err = mqtt_request(&host, &r);
    if (getenv("SHOW")) printf("==== %s %u %s: %d %s%s\n%s\n", method, port, path, err, failed, asked, page);
    CHECK(mem_now == 0);        /* all given back */
    if (getenv("SHOW")) printf("(memory: %ld at most)\n", mem_most);
    if (keeping >= 0 && !failed[0] && !asked[0] && err == noErr) {
        if (keep_fd >= 0 && keep_fd != keeping) close(keep_fd);
        keep_fd = keeping;
        strcpy(keep_login, keeping_login);
        strcpy(keep_note, keeping_note);
        drop(keeping);
    }
    for (i = 0; i < 8; i++) if (fds[i]) { close(fds[i]); fds[i] = 0; }
    return err;
}

static void forget_kept(void)
{
    if (keep_fd >= 0) close(keep_fd);
    keep_fd = -1;
}

static int has(const char *s)
{
    if (strstr(page, s)) return 1;
    printf("  not in the page: %s\n", s);
    return 0;
}

/* ------------------------------------------------------------ the tests */

static void test_topic(void)
{
    CHECK(get("GET", 18830, "/home/kitchen/temperature", NULL, NULL, NULL) == noErr && !failed[0]);
    CHECK(marked);
    CHECK(has("HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=macintosh\r\n\r\n"));
    CHECK(has("<TITLE>MQTT: home/kitchen/temperature</TITLE>"));
    CHECK(has("<A HREF=\"mqtt://127.0.0.1:18830/home/kitchen/\">home/kitchen/</A>"));
    CHECK(has("<PRE>21.5</PRE>"));
    CHECK(has("<B>QoS:</B> 1<BR><B>Retained:</B> yes<BR><B>Size:</B> 4 bytes"));
    CHECK(has("<FORM METHOD=POST ACTION=\"mqtt://127.0.0.1:18830/home/kitchen/temperature\">"));
    CHECK(has("<TEXTAREA NAME=payload ROWS=3 COLS=40>21.5</TEXTAREA>"));
    CHECK(opened == 1 && keep_fd >= 0 && !strcmp(keep_login, ""));
    /* the next page on the kept connection, at QoS 2 */
    CHECK(get("GET", 18830, "/home/office/door", NULL, NULL, NULL) == noErr && !failed[0]);
    CHECK(kept_given == 1 && opened == 0);
    CHECK(has("<PRE>closed</PRE>") && has("<B>QoS:</B> 2"));
    /* UTF-8 as Mac Roman, and made safe for HTML */
    CHECK(get("GET", 18830, "/home/garden/note", NULL, NULL, NULL) == noErr);
    CHECK(has("<PRE>Caf\x8e au lait \xd1 21\xa1" "C, na\x95ve \xaa</PRE>"));
    CHECK(get("GET", 18830, "/home/garden/motd", NULL, NULL, NULL) == noErr);
    CHECK(has("<PRE>Line one\nLine two\n\tindented &lt;b&gt;not bold&lt;/b&gt; &amp; more</PRE>"));
    /* binary, as hex */
    CHECK(get("GET", 18830, "/home/garden/camera", NULL, NULL, NULL) == noErr);
    CHECK(has("<P>Binary, 600 bytes; the first bytes:\n<PRE>0000  00 01 02 03"));
    CHECK(has("00f0  f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 fa fb fc fd fe ff\n</PRE>"));
    CHECK(has("<TEXTAREA NAME=payload ROWS=3 COLS=40></TEXTAREA>"));
    /* none */
    CHECK(get("GET", 18830, "/home/nothing", NULL, NULL, NULL) == noErr && !failed[0]);
    CHECK(has("<P>No retained message."));
}

static void test_tables(void)
{
    char *a, *b;
    CHECK(get("GET", 18830, "/home/", NULL, NULL, NULL) == noErr && !failed[0]);
    CHECK(has("<TITLE>MQTT: home/#</TITLE>"));
    CHECK(has("<A HREF=\"mqtt://127.0.0.1:18830/\">127.0.0.1</A>"));
    CHECK(has("<TR><TD VALIGN=TOP><A HREF=\"mqtt://127.0.0.1:18830/home/kitchen/temperature\">kitchen/temperature</A></TD>"
              "<TD VALIGN=TOP>21.5</TD><TD ALIGN=CENTER VALIGN=TOP>1</TD><TD ALIGN=CENTER VALIGN=TOP>yes</TD></TR>"));
    CHECK(has(">garden/camera</A></TD><TD VALIGN=TOP>(binary, 600 bytes)</TD>"));
    CHECK(has(">{&quot;temp&quot;: 8.5, &quot;wind&quot;: &quot;NW 12 km/h&quot;, &quot;sky&quot;: &quot;overcast&quot;}</TD>"));
    CHECK(has("<P>11 topics."));
    CHECK(has("Topic <INPUT NAME=topic SIZE=30 VALUE=\"home/\">"));
    CHECK(has("<FORM METHOD=POST ACTION=\"mqtt://127.0.0.1:18830/home/%23\">"));
    CHECK(!strstr(page, "other/thing") && !strstr(page, "$SYS"));
    /* in topic order */
    a = strstr(page, ">garden/camera<");
    b = strstr(page, ">office/door<");
    CHECK(a && b && a < b);
    /* # leaves out $SYS (4.7.2); $SYS/# shows it */
    CHECK(get("GET", 18830, "/", NULL, NULL, NULL) == noErr && has("<P>12 topics.") && !strstr(page, "$SYS"));
    CHECK(get("GET", 18830, "/$SYS/%23", NULL, NULL, NULL) == noErr && has(">broker/version</A>"));
    /* + */
    CHECK(get("GET", 18830, "/home/+/temperature", NULL, NULL, NULL) == noErr);
    CHECK(has(">home/kitchen/temperature</A>") && has(">home/office/temperature</A>") && has("<P>2 topics."));
    CHECK(has("<A HREF=\"mqtt://127.0.0.1:18830/home/+/\">home/+/</A>"));
    /* nothing */
    CHECK(get("GET", 18830, "/nowhere/", NULL, NULL, NULL) == noErr && has("<P>No messages in 1 second."));
    /* capped */
    strcpy(set_rows, "10");
    CHECK(get("GET", 18830, "/%23", NULL, NULL, NULL) == noErr && has("<P>The first 10 topics."));
    strcpy(set_rows, "100");
}

/* A message published while a table waits comes in the table, not retained */
static void test_live(void)
{
    pid_t child;
    strcpy(set_wait, "2");
    child = fork();
    if (!child) {
        unsigned char b[256];
        MqOut o;
        struct sockaddr_in a;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        usleep(600000);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(18830);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) _exit(1);
        mq_out(&o, b, sizeof b);
        mq_connect(&o, "live", NULL, NULL, 0);
        mq_publish(&o, "home/live", (const unsigned char *)"now", 3, 0, 0, 0);
        mq_disconnect(&o);
        send(fd, b, o.n, 0);
        usleep(200000);
        _exit(0);
    }
    CHECK(get("GET", 18830, "/home/", NULL, NULL, NULL) == noErr);
    CHECK(has(">live</A></TD><TD VALIGN=TOP>now</TD><TD ALIGN=CENTER VALIGN=TOP>0</TD><TD ALIGN=CENTER VALIGN=TOP>no</TD>"));
    waitpid(child, NULL, 0);
    strcpy(set_wait, "1");
}

static void test_publish(void)
{
    /* from another page: refused */
    from_own = 0;
    CHECK(get("POST", 18830, "/home/test/value", NULL, NULL, "payload=x&retain=yes&qos=1") == noErr);
    CHECK(strstr(failed, "Only its own page"));
    from_own = 1;
    /* retained, at QoS 1: back to the page, which shows it */
    CHECK(get("POST", 18830, "/home/test/value", NULL, NULL, "payload=caf%E9+%3C%3E&retain=yes&qos=1") == noErr && !failed[0]);
    CHECK(has("HTTP/1.0 303 See Other\r\nLocation: mqtt://127.0.0.1:18830/home/test/value\r\n\r\n"));
    CHECK(get("GET", 18830, "/home/test/value", NULL, NULL, NULL) == noErr);
    CHECK(has("<PRE>caf\x8e &lt;&gt;</PRE>") && has("<B>QoS:</B> 1<BR><B>Retained:</B> yes<BR><B>Size:</B> 8 bytes"));
    /* the line breaks a form sends (CR LF) go as LF */
    CHECK(get("POST", 18830, "/home/test/value", NULL, NULL, "payload=a%0D%0Ab&retain=yes&qos=0") == noErr && !failed[0]);
    CHECK(get("GET", 18830, "/home/test/value", NULL, NULL, NULL) == noErr && has("<PRE>a\nb</PRE>") && has("<B>QoS:</B> 0"));
    /* not retained: a page says so */
    CHECK(get("POST", 18830, "/home/test/value", NULL, NULL, "payload=hello&qos=0") == noErr && !failed[0]);
    CHECK(has("<H2>Published</H2>\n<P>5 bytes at QoS 0, not retained, to <A HREF=\"mqtt://127.0.0.1:18830/home/test/value\">home/test/value</A>."));
    /* from a table, to the topic named */
    CHECK(get("POST", 18830, "/home/%23", NULL, NULL, "topic=home/test/other&payload=42&retain=yes&qos=1") == noErr && !failed[0]);
    CHECK(has("Location: mqtt://127.0.0.1:18830/home/%23\r\n"));
    CHECK(get("GET", 18830, "/home/", NULL, NULL, NULL) == noErr && has(">test/other</A></TD><TD VALIGN=TOP>42</TD>"));
    /* an empty retained message takes the retained one away (3.3.1.3) */
    CHECK(get("POST", 18830, "/home/test/other", NULL, NULL, "payload=&retain=yes&qos=1") == noErr && !failed[0]);
    CHECK(get("GET", 18830, "/home/test/other", NULL, NULL, NULL) == noErr && has("No retained message."));
    /* no topic, or a wildcard */
    CHECK(get("POST", 18830, "/home/%23", NULL, NULL, "topic=&payload=1&qos=0") == noErr && strstr(failed, "Enter a topic"));
    CHECK(get("POST", 18830, "/home/%23", NULL, NULL, "topic=home/%2B&payload=1&qos=0") == noErr && strstr(failed, "Enter a topic"));
    /* too long */
    {
        static char big[8000];
        memset(big, 0, sizeof big);
        strcpy(big, "payload=");
        memset(big + 8, 'x', 2100);
        CHECK(get("POST", 18830, "/home/test/value", NULL, NULL, big) == noErr && strstr(failed, "too long"));
        memset(big + 8, 'x', 7000);
        CHECK(get("POST", 18830, "/home/test/value", NULL, NULL, big) == noErr && strstr(failed, "too long"));
    }
    from_own = 0;
}

static void test_errors(void)
{
    CHECK(get("GET", 18830, "/a%23b", NULL, NULL, NULL) == noErr && strstr(failed, "Not a valid topic"));
    CHECK(get("GET", 18830, "/a/b+", NULL, NULL, NULL) == noErr && strstr(failed, "Not a valid topic"));
    CHECK(get("GET", 18830, "/denied/x", NULL, NULL, NULL) == noErr && !strcmp(failed, "The broker refused to subscribe to denied/x."));
    /* nothing listening: Mill words the error */
    forget_kept();
    CHECK(get("GET", 18839, "/x", NULL, NULL, NULL) != noErr);
}

static void test_logins(void)
{
    forget_kept();
    /* none: Mill asks */
    CHECK(get("GET", 18831, "/home/kitchen/light", NULL, NULL, NULL) == noErr);
    CHECK(!strcmp(asked, "Log in to 127.0.0.1.") && !strcmp(asked_key, "mqtt://127.0.0.1:18831"));
    /* kept, and right */
    strcpy(saved_key, "mqtt://127.0.0.1:18831");
    strcpy(saved_user, "tester");
    strcpy(saved_pass, "secret");
    CHECK(get("GET", 18831, "/home/kitchen/light", NULL, NULL, NULL) == noErr && !asked[0] && !failed[0]);
    CHECK(has("<PRE>on</PRE>") && !strcmp(keep_login, "tester"));
    /* kept, and wrong: the 401 page and the dialog again */
    forget_kept();
    strcpy(saved_pass, "wrong");
    CHECK(get("GET", 18831, "/home/kitchen/light", NULL, NULL, NULL) == noErr);
    CHECK(strstr(asked, "was wrong") && has("401 Unauthorized"));
    saved_key[0] = 0;
    /* in the address */
    CHECK(get("GET", 18831, "/home/kitchen/light", "tester", "secret", NULL) == noErr && !asked[0] && has("<PRE>on</PRE>"));
    forget_kept();
}

int main(void)
{
    int i;
    host_init();
    /* wait for the broker */
    for (i = 0; i < 50; i++) {
        struct sockaddr_in a;
        int fd = socket(AF_INET, SOCK_STREAM, 0), ok;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(18830);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
        close(fd);
        if (ok) break;
        usleep(100000);
    }
    test_topic();
    test_tables();
    test_live();
    test_publish();
    test_errors();
    test_logins();
    forget_kept();
    if (failures) { printf("%d failed\n", failures); return 1; }
    printf("mqtt broker tests: all passed\n");
    return 0;
}
