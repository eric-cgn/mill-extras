/* e2e.c: the Mastodon Plug-In's Plugin.c run on a host, as Mill would run it, against a
   fake instance (fakemasto.py). The host calls are the ones it uses: TCP on sockets, the
   response to stdout, settings in memory, and Mill's own Format (src/core/fmt.c) and
   ToMacRoman (src/core/charset.c). One request per run:

     e2e METHOD URL [BODY]      settings from the environment: MS_https, MS_posts,
                                MS_pictures, MS_token, MS_token_host, MS_token_acct,
                                MS_proxy ("host:port", with proxy_on yes), FROM ("own")

   It prints the response, then a line "== " with what happened: FAIL text, ENDED-EARLY,
   the settings SetSetting made, and the memory at most in use. */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "MillPlugIn.h"
#include "fmt.h"
#include "charset.h"

static char reqtext[2048], *body;
static long body_at;
static long mem_now, mem_max;
static char failed[300];
static int ended_early, marked;

typedef struct { long size; } MemHead;
static pascal void *h_new(long n)
{
    MemHead *m = malloc(sizeof *m + n);
    if (!m) return NULL;
    m->size = n;
    mem_now += n;
    if (mem_now > mem_max) mem_max = mem_now;
    memset(m + 1, 0xA5, n);     /* not zeroed, as NewPtr's aren't */
    return m + 1;
}
static pascal void h_free(void *p)
{
    MemHead *m;
    if (!p) return;
    m = (MemHead *)p - 1;
    mem_now -= m->size;
    free(m);
}
static pascal void h_status(MillReqRef r, const char *t) { (void)r; fprintf(stderr, "status: %s\n", t); }
static pascal void h_progress(MillReqRef r, long d, long t) { (void)r; (void)d; (void)t; }
static pascal short h_busy(MillReqRef r) { (void)r; return 0; }

static pascal OSErr h_tcp_open(MillReqRef r, const char *host, unsigned short port, MillConn *conn)
{
    struct addrinfo hints, *ai;
    char ps[8];
    int s;
    (void)r;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%u", port);
    fprintf(stderr, "connect: %s:%u\n", host, port);
    if (getaddrinfo(host, ps, &hints, &ai)) return -23041;  /* MacTCP's "no name" */
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, ai->ai_addr, ai->ai_addrlen) < 0) { freeaddrinfo(ai); close(s); return -23008; }
    freeaddrinfo(ai);
    *conn = s;
    return noErr;
}
static pascal OSErr h_tcp_send(MillConn c, const void *d, long n)
{
    fprintf(stderr, "send: %.*s\n", (int)(n > 400 ? 400 : n), (const char *)d);
    return write((int)c, d, n) == n ? noErr : -23000;
}
static pascal OSErr h_tcp_recv(MillConn c, void *buf, long *n)
{
    long k = read((int)c, buf, *n > 333 ? 333 : *n);     /* odd pieces, to cut things up */
    if (k < 0) return -23000;
    *n = k;
    return noErr;
}
static pascal void h_tcp_close(MillConn c) { close((int)c); }

static int stopped;
static long written;
static pascal short h_write(MillReqRef r, const void *d, long n)
{
    (void)r;
    if (stopped) return 0;
    fwrite(d, 1, n, stdout);
    written += n;
    if (getenv("LIMIT") && written > atol(getenv("LIMIT"))) { stopped = 1; return 0; }
    return 1;
}

/* Settings: from MS_KEY in the environment; SetSetting's in memory, and reported */
static char set_keys[8][32], set_vals[8][256];
static int nset;
static pascal long h_get_setting(const char *scheme, const char *key, char *out, long size)
{
    char name[64];
    const char *v = NULL;
    int i;
    if (!strcmp(scheme, "http")) {
        if (!strcmp(key, "proxy_on")) v = getenv("MS_proxy") ? "yes" : "no";
        else if (!strcmp(key, "proxy")) v = getenv("MS_proxy");
    } else {
        for (i = nset - 1; i >= 0; i--) if (!strcmp(set_keys[i], key)) { v = set_vals[i]; break; }
        if (i < 0) {
            snprintf(name, sizeof name, "MS_%s", key);
            v = getenv(name);
        }
    }
    if (!v || (long)strlen(v) >= size) return -1;
    strcpy(out, v);
    return strlen(v);
}
static pascal OSErr h_set_setting(const char *scheme, const char *key, const char *value)
{
    (void)scheme;
    if (nset < 8) { snprintf(set_keys[nset], 32, "%s", key); snprintf(set_vals[nset++], 256, "%s", value); }
    return noErr;
}
static pascal long h_format(char *out, long size, const char *fmt, const long *args) { return mill_format(out, size, fmt, args); }
static pascal short h_printf(MillReqRef r, const char *fmt, const long *args)
{
    char b[4096];
    long n = mill_format(b, sizeof b, fmt, args);
    return h_write(r, b, n);
}
static pascal long h_header(MillReqRef r, const char *name, char *out, long size)
{
    const char *p = reqtext;
    long nl = strlen(name);
    (void)r;
    while ((p = strstr(p, "\r\n")) != NULL) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1, *e;
            while (*v == ' ') v++;
            e = strstr(v, "\r\n");
            if (e - v >= size) return -1;
            memcpy(out, v, e - v);
            out[e - v] = 0;
            return e - v;
        }
    }
    return -1;
}
static pascal short h_from(MillReqRef r) { (void)r; return getenv("FROM") && !strcmp(getenv("FROM"), "own") ? millFromOwnPage : millFromOther; }
static pascal long h_url_decode(const char *in, char *out, long size)
{
    long o = 0;
    for (; *in && o < size - 1; in++) {
        unsigned int x;
        if (*in == '%' && in[1] && in[2] && sscanf(in + 1, "%2x", &x) == 1) { out[o++] = (char)x; in += 2; }
        else out[o++] = *in;
    }
    out[o] = 0;
    return o;
}
static pascal long h_b64(const void *in, long len, char *out, long size)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *s = in;
    long i, o = 0;
    for (i = 0; i < len; i += 3) {
        unsigned long v = s[i] << 16 | (i + 1 < len ? s[i + 1] << 8 : 0) | (i + 2 < len ? s[i + 2] : 0);
        if (o + 5 > size) return -1;
        out[o++] = t[v >> 18];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = i + 1 < len ? t[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? t[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}
static pascal long h_to_mac(const char *in, long len, short cs, char *out, long size)
{
    const unsigned char *s = (const unsigned char *)in;
    long i = 0, o = 0;
    (void)cs;
    while (i < len) {
        unsigned char c;
        long u;
        int k;
        if (s[i] >= 0x80 && (k = utf8_char(s + i, len - i, &u)) > 0) { c = unicode_to_mac(u); i += k; }
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
    long n;
    (void)r;
    if (!buf) { body_at = 0; return noErr; }
    n = body ? (long)strlen(body) - body_at : 0;
    if (n > *len) n = *len;
    if (n > 7) n = 7;       /* in small pieces */
    if (n > 0) memcpy(buf, body + body_at, n);
    body_at += n;
    *len = n;
    return noErr;
}
static pascal void h_fail(MillReqRef r, const char *t) { (void)r; snprintf(failed, sizeof failed, "%s", t); stopped = 1; }
static pascal void h_ended_early(MillReqRef r) { (void)r; ended_early = 1; stopped = 1; }
static pascal short h_saved_login(const char *key, char *name, long ns, char *pass, long ps)
{
    fprintf(stderr, "saved login asked: %s\n", key);
    if (getenv("PROXY_LOGIN") && !strncmp(key, "proxy ", 6)) {
        snprintf(name, ns, "pat");
        snprintf(pass, ps, "%s", getenv("PROXY_LOGIN"));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    MillHost host;
    MillInfo info;
    MillPlugIn *p;
    MillRequest r;
    const char *url, *rest, *slash;
    char hostport[256], path[1024], *colon;
    OSErr err;
    int i;
    if (argc < 3) { fprintf(stderr, "e2e METHOD URL [BODY]\n"); return 2; }
    url = argv[2];
    body = argc > 3 ? argv[3] : NULL;
    memset(&host, 0, sizeof host);
    host.size = sizeof host;
    host.version = kMillInterface;
    host.NewMem = h_new;
    host.FreeMem = h_free;
    host.Status = h_status;
    host.Progress = h_progress;
    host.Busy = h_busy;
    host.TcpOpen = h_tcp_open;
    host.TcpSend = h_tcp_send;
    host.TcpRecv = h_tcp_recv;
    host.TcpClose = h_tcp_close;
    host.Write = h_write;
    host.GetSetting = h_get_setting;
    host.SetSetting = h_set_setting;
    host.Format = h_format;
    host.Printf = h_printf;
    host.Header = h_header;
    host.From = h_from;
    host.UrlDecode = h_url_decode;
    host.Base64Encode = h_b64;
    host.ToMacRoman = h_to_mac;
    host.PlugInPage = h_plugin_page;
    host.ReadBody = h_read_body;
    host.Fail = h_fail;
    host.EndedEarly = h_ended_early;
    host.SavedLogin = h_saved_login;
    memset(&info, 0, sizeof info);
    if (!(p = PlugInNew(&host, 0, &info))) return 1;
    p->host = &host;
    /* the address taken apart, as Mill does */
    if (strncmp(url, "mastodon://", 11)) return 2;
    rest = url + 11;
    slash = rest + strcspn(rest, "/?");
    snprintf(hostport, sizeof hostport, "%.*s", (int)(slash - rest), rest);
    snprintf(path, sizeof path, "%s%s", *slash == '/' ? "" : "/", slash);
    memset(&r, 0, sizeof r);
    r.protocol = 0;
    r.url = url;
    r.scheme = "mastodon";
    r.user = r.password = "";
    if ((colon = strchr(hostport, ':')) != NULL) { *colon = 0; r.port = (unsigned short)atoi(colon + 1); }
    r.host = hostport;
    r.path = path;
    snprintf(reqtext, sizeof reqtext, "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Mill/2.1 (Macintosh; 68000)\r\n%s\r\n",
             argv[1], path, r.host, body ? "Content-Type: application/x-www-form-urlencoded\r\n" : "");
    r.text = reqtext;
    r.len = strlen(reqtext);
    err = p->methods->request(p, &r);
    p->methods->close(p);
    printf("\n== err %d%s%s%s%s", err, failed[0] ? " FAIL " : "", failed, ended_early ? " ENDED-EARLY" : "", marked ? " MARKED" : "");
    for (i = 0; i < nset; i++) printf(" SET %s=%s", set_keys[i], !strcmp(set_keys[i], "token") && set_vals[i][0] ? "(token)" : set_vals[i]);
    printf(" MEM-MAX %ld MEM-LEFT %ld\n", mem_max, mem_now);
    return 0;
}
