/* Mastodon: a Plug-In for Mill that reads Mastodon's client API (JSON over HTTPS) and
   shows timelines, accounts, and threads as pages, at mastodon://INSTANCE/ addresses
   (masto.h lists them). snac, snarc, GoToSocial, Pleroma, and the like serve the same API.

   HTTPS: Mill can't do TLS, so with Use HTTPS on (as it is at first) each API request goes
   to Mill's own HTTP proxy (Settings, HTTP: its host and port, read with GetSetting) as
   "GET https://INSTANCE/api/..." and the proxy makes the TLS connection, as it does for
   Mill's https: pages. A proxy login kept for Basic is sent too. With Use HTTPS off, the
   requests go straight to the instance over plain HTTP (a server on your own network).

   Logins: an access token, made in the instance's settings (Development, New application)
   and pasted into mastodon://INSTANCE/login. It's checked with verify_credentials, then
   kept in Mill's settings as this protocol's own, unlisted (token, token_host, token_acct),
   and sent only to that instance. Pages never show it.

   Memory: about 20K for a request, taken at its start and given back at its end; replies
   are read as they come (json.c), never kept whole. */
#include <string.h>
#include <Memory.h>

#include "MillPlugIn.h"
#include "masto.h"
#include "http11.h"

#define NAME "mastodon"
#define BODY_MAX 4096       /* a form's body, at most (a post is 500 characters) */
#define TOKEN_MAX 200

typedef struct {
    MillPlugIn base;
    MillMethods methods;        /* filled in by PlugInNew (see MillPlugIn.h's rules) */
    MillProtocol protocol;
    MillSetting settings[3];
} Me;

/* One request */
typedef struct {
    const MillHost *h;
    MillRequest *r;
    char host[140];             /* the instance, with :port if the address gives one */
    char hostname[128];         /* without */
    unsigned short port;
    short https;
    char proxy[128];            /* Mill's HTTP proxy, with Use HTTPS */
    unsigned short proxy_port;
    char token[TOKEN_MAX];      /* for this instance, or "" */
    char who[M_ACCT];           /* whose it is */
    char agent[100];
    short limit, flags;
    Route rt;
    MOut out;
    short started;              /* the page's head is written */
    short autostart;            /* api() writes it on a 2xx */
    const char *title;          /* UTF-8 */
    short form;                 /* the page has a post form at its top */
    short code;                 /* the last reply's status */
    short shown;                /* a page's worth was shown: posts, or an account */
    char head[2048];
    char buf[1024];
    char path[300];
    char title_buf[M_ACCT + 4];
    Model model;
} Req;

#define H (q->h)

static int out_write(void *ref, const char *p, long n)
{
    Req *q = ref;
    return H->Write(q->r->ref, p, n) != 0;
}

static long out_to_mac(void *ref, const char *in, long len, char *out, long size)
{
    Req *q = ref;
    return H->ToMacRoman(in, len, millUTF8, out, size);
}

static int put(Req *q, const char *s) { return H->Write(q->r->ref, s, strlen(s)) != 0; }

static int same_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* The page's start: the HTTP head, the HTML's, and the nav */
static int start(Req *q)
{
    if (q->started) return 1;
    q->started = 1;
    H->PlugInPage(q->r->ref);
    if (!put(q, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/html; charset=macintosh" MILL_CRLF MILL_CRLF)) return 0;
    if (!page_head(&q->out, q->host, q->title, &q->rt, q->token[0] ? q->who : NULL)) return 0;
    if (q->form && q->token[0] && !page_post_form(&q->out, q->host, NULL, NULL)) return 0;
    return 1;
}

/* The server's answer to a failed request, as a page */
static void server_said(Req *q)
{
    long a[1];
    char num[8];
    a[0] = q->code;
    H->Format(num, sizeof num, "%d", a);
    if (!start(q)) return;
    put(q, "<HR>\n<P><B>The instance answered ");
    put(q, num);
    if (q->model.error[0]) {
        put(q, ": ");
        page_text(&q->out, q->model.error);
    }
    put(q, ".</B>\n");
    if ((q->code == 401 || q->code == 403 || q->code == 422) && !q->token[0])
        put(q, "<P>A login may be required: see Log In, above.\n");
}

/* key=value from the form body, %XX and + undone, as raw bytes; -1 if not there */
static long field(const char *body, long len, const char *key, char *out, long size)
{
    long i = 0, kl = strlen(key);
    while (i < len) {
        long e = i, o = 0, k;
        while (e < len && body[e] != '&') e++;
        if (e - i > kl && !memcmp(body + i, key, kl) && body[i + kl] == '=') {
            for (k = i + kl + 1; k < e && o < size - 1; k++) {
                int c = (unsigned char)body[k], hi, lo;
                if (c == '+') c = ' ';
                else if (c == '%' && k + 2 < e) {
                    hi = body[k + 1];
                    lo = body[k + 2];
                    hi = hi >= '0' && hi <= '9' ? hi - '0' : (hi | 32) >= 'a' && (hi | 32) <= 'f' ? (hi | 32) - 'a' + 10 : -1;
                    lo = lo >= '0' && lo <= '9' ? lo - '0' : (lo | 32) >= 'a' && (lo | 32) <= 'f' ? (lo | 32) - 'a' + 10 : -1;
                    if (hi >= 0 && lo >= 0) { c = hi * 16 + lo; k += 2; }
                }
                out[o++] = (char)c;
            }
            out[o] = 0;
            return o;
        }
        i = e + 1;
    }
    return -1;
}

/* Latin-1 text (as Mill sends a form from a Mac Roman page) as a UTF-8 form value, %XX
   encoded, onto out; CR LF as LF. Returns the new length, or -1 if it won't fit. */
static long add_value(char *out, long o, long size, const char *key, const char *v, long n)
{
    long i, kl = strlen(key);
    if (o + kl + 2 >= size) return -1;
    if (o) out[o++] = '&';
    memcpy(out + o, key, kl);
    o += kl;
    out[o++] = '=';
    for (i = 0; i < n; i++) {
        unsigned char b = (unsigned char)v[i], u[2];
        int k, m = 1;
        if (b == '\r' && i + 1 < n && v[i + 1] == '\n') continue;
        if (b == '\r') b = '\n';
        if (b >= 0x80) { u[0] = (unsigned char)(0xC0 | (b >> 6)); u[1] = (unsigned char)(0x80 | (b & 0x3F)); m = 2; }
        else u[0] = b;
        for (k = 0; k < m; k++) {
            unsigned char c = u[k];
            if (o + 4 >= size) return -1;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
                out[o++] = (char)c;
            else {
                out[o++] = '%';
                out[o++] = "0123456789ABCDEF"[c >> 4];
                out[o++] = "0123456789ABCDEF"[c & 15];
            }
        }
    }
    out[o] = 0;
    return o;
}

/* A network error: before the page started, Mill says what it was; after, what's shown
   is marked cut off (and when the person stopped it, it stays as it is) */
static OSErr net_err(Req *q, OSErr err)
{
    if (!q->started) return err;
    if (err != userCanceledErr) H->EndedEarly(q->r->ref);
    return userCanceledErr;
}

/* One API request: method, path (from /api), a form body (or NULL), its reply read into
   q->model as parse. Returns noErr with q->code set, or the network's error. token: the
   one to send (NULL: q->token, if any). */
static OSErr api(Req *q, const char *method, const char *path, const char *body, long blen, short parse, short flags,
                 const char *token)
{
    MillConn c;
    OSErr err;
    long a[6], n, have = 0, head = 0, left = -1;
    HttpHead hh;
    Chunked ck;
    int kind, r = JSON_MORE;
    char auth[300], *data;
    const char *tok = token ? token : q->token;
    if (q->https) err = H->TcpOpen(q->r->ref, q->proxy, q->proxy_port, &c);
    else err = H->TcpOpen(q->r->ref, q->hostname, q->port ? q->port : 80, &c);
    if (err != noErr) return net_err(q, err);
    /* the request: to a proxy, the whole address */
    a[0] = (long)method;
    a[1] = (long)(q->https ? "https://" : "");
    a[2] = (long)(q->https ? q->host : "");
    a[3] = (long)path;
    a[4] = (long)q->host;
    a[5] = (long)q->agent;
    n = H->Format(q->head, sizeof q->head,
                  "%s %s%s%s HTTP/1.0" MILL_CRLF "Host: %s" MILL_CRLF "Accept: application/json" MILL_CRLF "User-Agent: %s" MILL_CRLF, a);
    if (tok[0]) {
        a[0] = (long)tok;
        H->Format(auth, sizeof auth, "Authorization: Bearer %s" MILL_CRLF, a);
        if (n + (long)strlen(auth) < (long)sizeof q->head) { strcpy(q->head + n, auth); n += strlen(auth); }
    }
    if (q->https) {
        /* a proxy login kept for Basic (Mill's http Plug-In's key for it) */
        char key[200], name[64], pass[64], up[130], b64[200];
        a[0] = (long)q->proxy;
        a[1] = q->proxy_port;
        H->Format(key, sizeof key, "proxy %s:%d Basic", a);
        if (H->SavedLogin(key, name, sizeof name, pass, sizeof pass)) {
            a[0] = (long)name;
            a[1] = (long)pass;
            H->Format(up, sizeof up, "%s:%s", a);
            if (H->Base64Encode(up, strlen(up), b64, sizeof b64) > 0) {
                a[0] = (long)b64;
                H->Format(auth, sizeof auth, "Proxy-Authorization: Basic %s" MILL_CRLF, a);
                if (n + (long)strlen(auth) < (long)sizeof q->head) { strcpy(q->head + n, auth); n += strlen(auth); }
            }
            memset(pass, 0, sizeof pass);
            memset(up, 0, sizeof up);
        }
    }
    if (body) {
        a[0] = blen;
        n += H->Format(q->head + n, sizeof q->head - n,
                       "Content-Type: application/x-www-form-urlencoded" MILL_CRLF "Content-Length: %d" MILL_CRLF, a);
    }
    if (n + 2 >= (long)sizeof q->head) { H->TcpClose(c); memset(q->head, 0, sizeof q->head); return paramErr; }
    strcpy(q->head + n, MILL_CRLF);
    n += 2;
    err = H->TcpSend(c, q->head, n);
    memset(q->head, 0, sizeof q->head);     /* the token */
    if (err == noErr && body && blen) err = H->TcpSend(c, body, blen);
    if (err != noErr) { H->TcpClose(c); return net_err(q, err); }
    H->Status(q->r->ref, "Waiting for reply\311");
    /* the head */
    for (;;) {
        long m = sizeof q->head - have;
        if (have > 0 && (head = http_head_end(q->head, have)) > 0) break;
        if (m == 0 || (err = H->TcpRecv(c, q->head + have, &m)) != noErr || m == 0) break;
        have += m;
    }
    if (err != noErr) { H->TcpClose(c); return net_err(q, err); }
    if (head <= 0) {
        H->TcpClose(c);
        if (!have) return net_err(q, connectionClosing);
        q->code = 0;
        H->Fail(q->r->ref, have >= (long)sizeof q->head ? "Mastodon: the instance\325s reply had too many headers." :
                                                         "Mastodon: the instance\325s reply couldn\325t be read.");
        return userCanceledErr;
    }
    http_head(q->head, head, &hh);
    q->code = (short)hh.code;
    kind = http_body(&hh, 0);
    if (kind == HTTP_BAD || hh.code < 200) {
        H->TcpClose(c);
        H->Fail(q->r->ref, "Mastodon: the instance\325s reply couldn\325t be read.");
        return userCanceledErr;
    }
    if (hh.code == 407) {
        H->TcpClose(c);
        H->Fail(q->r->ref, "Mastodon: the proxy requires a login. Open an https: page to log in, then try again.");
        return userCanceledErr;
    }
    /* a 2xx's posts as parse; anything else is read for its "error" */
    if (hh.code >= 200 && hh.code < 300) {
        model_begin(&q->model, &q->out, q->host, parse, flags);
        if (q->autostart && !start(q)) { H->TcpClose(c); return noErr; }
    } else model_begin(&q->model, &q->out, q->host, MP_ACCOUNT, 0);
    memset(&ck, 0, sizeof ck);
    left = kind == HTTP_LENGTH ? hh.length : -1;
    /* the body, a piece at a time: what came with the head first */
    data = q->head + head;
    n = have - head;
    for (;;) {
        long used;
        if (n > 0) {
            if (kind == HTTP_CHUNKED && (n = chunked_run(&ck, data, n, &used)) < 0) { r = JSON_ERROR; break; }
            if (left >= 0 && n > left) n = left;
            if (left >= 0) left -= n;
            if (kind != HTTP_NONE && n > 0 && (r = model_data(&q->model, data, n)) != JSON_MORE) break;
        }
        if (kind == HTTP_NONE || left == 0 || (kind == HTTP_CHUNKED && chunked_done(&ck))) break;
        data = q->buf;
        n = sizeof q->buf;
        if ((err = H->TcpRecv(c, q->buf, &n)) != noErr) break;
        if (n == 0) break;
    }
    H->TcpClose(c);
    if (err != noErr) return net_err(q, err);
    if (r == JSON_MORE) r = model_end(&q->model);
    if (q->model.stopped) return userCanceledErr;
    if (r == JSON_STOP && parse == MP_ANCESTORS) r = JSON_DONE;
    if (r != JSON_DONE && hh.code >= 200 && hh.code < 300) {
        /* nothing shown yet: an alert (Mill drops the page's start); else a note on the page */
        if (!q->model.count && !q->shown) {
            H->Fail(q->r->ref, "Mastodon: the reply wasn\325t Mastodon JSON. Proxies that "
                               "rewrite pages, like MacProxy, don\325t work.");
            return userCanceledErr;
        }
        put(q, "<HR>\n<P><B>Reply cut short, or not Mastodon JSON.</B>\n");
    }
    if (q->model.count) q->shown = 1;
    return noErr;
}

static int ok(Req *q) { return q->code >= 200 && q->code < 300; }

/* A timeline: local, federated, home, or a hashtag's */
static OSErr timeline(Req *q)
{
    OSErr err;
    if (route_api(&q->rt, NULL, q->limit, q->path, sizeof q->path) < 0) return paramErr;
    q->autostart = 1;
    q->form = q->rt.kind != RT_TAG;
    if ((err = api(q, "GET", q->path, NULL, 0, MP_STATUSES, q->flags, NULL)) != noErr) return err;
    if (!ok(q)) server_said(q);
    else page_next(&q->out, q->host, q->r->path, &q->model, q->limit);
    page_end(&q->out);
    return noErr;
}

/* An account's posts: its ID by lookup, then its statuses */
static OSErr account(Req *q)
{
    OSErr err;
    long a[1];
    char id[M_ID];
    a[0] = (long)q->rt.user;
    H->Format(q->path, sizeof q->path, "/api/v1/accounts/lookup?acct=%s", a);
    if ((err = api(q, "GET", q->path, NULL, 0, MP_ACCOUNT, 0, NULL)) != noErr) return err;
    if (!ok(q) || !q->model.acct.id[0]) {
        if (ok(q)) q->code = 404;
        server_said(q);
        page_end(&q->out);
        return noErr;
    }
    if (!start(q) || !page_account(&q->out, &q->model)) return noErr;
    q->shown = 1;
    strcpy(id, q->model.acct.id);
    if (route_api(&q->rt, id, q->limit, q->path, sizeof q->path) < 0) return paramErr;
    if ((err = api(q, "GET", q->path, NULL, 0, MP_STATUSES, q->flags, NULL)) != noErr) return err;
    if (!ok(q)) server_said(q);
    else page_next(&q->out, q->host, q->r->path, &q->model, q->limit);
    page_end(&q->out);
    return noErr;
}

/* A post and its thread: what it replies to (the context's ancestors), the post, and the
   replies (its descendants), each read as it comes */
static OSErr thread(Req *q)
{
    OSErr err;
    long a[1];
    char acct[M_ACCT], id[M_ID];
    a[0] = (long)q->rt.id;
    H->Format(q->path, sizeof q->path, "/api/v1/statuses/%s/context", a);
    q->autostart = 1;
    if ((err = api(q, "GET", q->path, NULL, 0, MP_ANCESTORS, q->flags, NULL)) != noErr) return err;
    if (!ok(q)) { server_said(q); page_end(&q->out); return noErr; }
    H->Format(q->path, sizeof q->path, "/api/v1/statuses/%s", a);
    if ((err = api(q, "GET", q->path, NULL, 0, MP_STATUS, q->flags | MF_FOCUS, NULL)) != noErr) return err;
    if (!ok(q)) { server_said(q); page_end(&q->out); return noErr; }
    if (q->token[0]) {
        int k = q->model.st[1].any;
        strcpy(acct, q->model.st[k].acct);
        strcpy(id, q->model.st[k].id);
        if (!page_post_form(&q->out, q->host, id, acct)) return noErr;
    }
    H->Format(q->path, sizeof q->path, "/api/v1/statuses/%s/context", a);
    if ((err = api(q, "GET", q->path, NULL, 0, MP_DESCENDANTS, q->flags | MF_INDENT, NULL)) != noErr) return err;
    if (!ok(q)) server_said(q);
    page_end(&q->out);
    return noErr;
}

/* A POST's body, BODY_MAX at most; NULL (and the request failed) if it can't be read */
static char *read_body(Req *q, long *len)
{
    char *b = H->NewMem(BODY_MAX + 1);
    long n = 0;
    if (!b) return NULL;
    for (;;) {
        long m = BODY_MAX - n;
        if (H->ReadBody(q->r->ref, b + n, &m) != noErr) { H->FreeMem(b); return NULL; }
        if (m == 0) break;
        n += m;
        if (n >= BODY_MAX) {
            char x[64];
            m = sizeof x;
            if (H->ReadBody(q->r->ref, x, &m) == noErr && m > 0) {
                H->FreeMem(b);
                H->Fail(q->r->ref, "Mastodon: that\325s too long to send.");
                return NULL;
            }
            break;
        }
    }
    b[n] = 0;
    *len = n;
    return b;
}

static int token_ok(const char *t)
{
    if (!*t) return 0;
    for (; *t; t++) {
        char c = *t;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
              c == '~' || c == '+' || c == '/' || c == '='))
            return 0;
    }
    return 1;
}

/* mastodon://HOST/login: the form, or (POST) the token checked and kept, or forgotten */
static OSErr login(Req *q, int post)
{
    char other[140];
    q->title = "Log In";
    if (post) {
        long len, n;
        char *body = read_body(q, &len), tok[TOKEN_MAX];
        OSErr err;
        if (!body) return noErr;
        if (field(body, len, "forget", tok, sizeof tok) >= 0) {
            H->FreeMem(body);
            H->SetSetting(NAME, "token", "");
            H->SetSetting(NAME, "token_host", "");
            H->SetSetting(NAME, "token_acct", "");
            q->token[0] = 0;
            start(q);
            put(q, "<P>Logged out. To revoke the token, use the instance\325s settings.\n");
            page_end(&q->out);
            return noErr;
        }
        n = field(body, len, "token", tok, sizeof tok);
        memset(body, 0, len);
        H->FreeMem(body);
        /* trimmed: a pasted token may come with spaces */
        while (n > 0 && (tok[n - 1] == ' ' || tok[n - 1] == '\r' || tok[n - 1] == '\n')) tok[--n] = 0;
        if (n <= 0 || n >= TOKEN_MAX - 1 || !token_ok(tok)) {
            memset(tok, 0, sizeof tok);
            H->Fail(q->r->ref, "Mastodon: that isn\325t a token. Copy \322Your access token\323 from the application\325s page.");
            return noErr;
        }
        err = api(q, "GET", "/api/v1/accounts/verify_credentials", NULL, 0, MP_ACCOUNT, 0, tok);
        if (err != noErr) { memset(tok, 0, sizeof tok); return err; }
        if (!ok(q) || !q->model.acct.acct[0]) {
            memset(tok, 0, sizeof tok);
            server_said(q);
            put(q, "<P>Token not accepted.\n");
            page_end(&q->out);
            return noErr;
        }
        H->SetSetting(NAME, "token", tok);
        H->SetSetting(NAME, "token_host", q->host);
        H->SetSetting(NAME, "token_acct", q->model.acct.acct);
        strcpy(q->token, tok);
        memset(tok, 0, sizeof tok);
        strcpy(q->who, q->model.acct.acct);
        start(q);
        put(q, "<P>Logged in as @");
        page_text(&q->out, q->who);
        put(q, ". See your <A HREF=\"mastodon://");
        put(q, q->host);
        put(q, "/home\">home timeline</A>.\n");
        page_end(&q->out);
        return noErr;
    }
    start(q);
    if (q->token[0]) {
        put(q, "<P>Logged in to ");
        page_text(&q->out, q->host);
        put(q, " as @");
        page_text(&q->out, q->who);
        put(q, ".\n"
               "<FORM METHOD=POST ACTION=\"mastodon://");
        put(q, q->host);
        put(q, "/login\"><INPUT TYPE=HIDDEN NAME=forget VALUE=1><INPUT TYPE=SUBMIT VALUE=\"Log Out\"></FORM>\n");
    } else {
        if (H->GetSetting(NAME, "token_host", other, sizeof other) > 0) {
            put(q, "<P>Logged in to ");
            page_text(&q->out, other);
            put(q, ". Logging in here logs out there.\n");
        }
        put(q, "<P>Public timelines don\325t need a login. To post, favorite, or boost, log in with "
               "an access token: in the instance\325s settings, under Development, create a New "
               "Application with read and write scopes, open it, and copy \322Your access token\323 "
               "here. Kept in Mill\325s settings, sent only to this instance");
        put(q, q->https ? ", through the HTTP proxy.\n" : ", over plain HTTP.\n");
        put(q, "<FORM METHOD=POST ACTION=\"mastodon://");
        put(q, q->host);
        put(q, "/login\"><P>Access token: <INPUT TYPE=PASSWORD NAME=token SIZE=30> "
               "<INPUT TYPE=SUBMIT VALUE=\"Log In\"></FORM>\n");
    }
    page_end(&q->out);
    return noErr;
}

/* POST mastodon://HOST/post: a new post; /@USER/ID/act: favourite or boost one. The
   answer is the post as the instance has it now. */
static OSErr act(Req *q)
{
    long len, n, o = 0;
    char *body, *dec, *out;
    const char *keys[4];
    int k, said = 0;
    OSErr err;
    if (!q->token[0]) {
        H->Fail(q->r->ref, "Mastodon: log in to this instance first.");
        return noErr;
    }
    if (!(body = read_body(q, &len))) return noErr;
    q->autostart = 1;
    if (q->rt.kind == RT_ACT) {
        char v[16];
        const char *what;
        long a[2];
        n = field(body, len, "do", v, sizeof v);
        H->FreeMem(body);
        if (n > 0 && !strcmp(v, "Favorite")) what = "favourite", q->title = "Favorited";
        else if (n > 0 && !strcmp(v, "Unfavorite")) what = "unfavourite", q->title = "Unfavorited";
        else if (n > 0 && !strcmp(v, "Boost")) what = "reblog", q->title = "Boosted";
        else if (n > 0 && !strcmp(v, "Unboost")) what = "unreblog", q->title = "Unboosted";
        else {
            H->Fail(q->r->ref, "Mastodon: the submitted data was invalid.");
            return noErr;
        }
        a[0] = (long)q->rt.id;
        a[1] = (long)what;
        H->Format(q->path, sizeof q->path, "/api/v1/statuses/%s/%s", a);
        err = api(q, "POST", q->path, "", 0, MP_STATUS, q->flags, NULL);
    } else {
        /* the form's fields, each Latin-1 as Mill sends it, as UTF-8 for the API */
        dec = H->NewMem(BODY_MAX + 1);
        out = dec ? H->NewMem(BODY_MAX * 6 + 64) : NULL;
        if (!out) {
            if (dec) H->FreeMem(dec);
            H->FreeMem(body);
            return memFullErr;
        }
        out[0] = 0;
        keys[0] = "status";
        keys[1] = "spoiler_text";
        keys[2] = "visibility";
        keys[3] = "in_reply_to_id";
        for (k = 0; k < 4 && o >= 0; k++) {
            const char *p;
            if ((n = field(body, len, keys[k], dec, BODY_MAX + 1)) < 0) continue;
            for (p = dec; *p == ' ' || *p == '\r' || *p == '\n'; p++) {}
            if (k == 0 && !*p) break;
            if (k == 0) said = 1;
            if (k == 2 && strcmp(dec, "public") && strcmp(dec, "unlisted") && strcmp(dec, "private") && strcmp(dec, "direct")) continue;
            if (k == 3) {
                Route rt;
                char path[M_ID + 8] = "/@x/";
                if (n >= M_ID) continue;
                strcat(path, dec);
                route_parse(path, &rt);
                if (rt.kind != RT_STATUS) continue;
            }
            if (k == 1 && !*p) continue;
            o = add_value(out, o, BODY_MAX * 6 + 64, keys[k], dec, n);
        }
        H->FreeMem(dec);
        H->FreeMem(body);
        if (o < 0 || !said) {
            H->FreeMem(out);
            H->Fail(q->r->ref, o < 0 ? "Mastodon: that\325s too long to send." : "Mastodon: nothing to post.");
            return noErr;
        }
        q->title = "Posted";
        err = api(q, "POST", "/api/v1/statuses", out, o, MP_STATUS, q->flags, NULL);
        H->FreeMem(out);
    }
    if (err != noErr) return err;
    if (!ok(q)) server_said(q);
    page_end(&q->out);
    return noErr;
}

static void me_close(MillPlugIn *self) { self->host->FreeMem(self); }

/* The request's settings, token, and route */
static void setup(Req *q, const MillHost *h, MillRequest *r)
{
    char v[TOKEN_MAX];
    long a[2], n;
    q->h = h;
    q->r = r;
    q->out.ref = q;
    q->out.write = out_write;
    q->out.to_mac = out_to_mac;
    if (strlen(r->host) < sizeof q->hostname) strcpy(q->hostname, r->host);
    q->port = r->port;
    a[0] = (long)r->host;
    a[1] = r->port;
    h->Format(q->host, sizeof q->host, r->port ? "%s:%d" : "%s", a);
    q->https = !(h->GetSetting(NAME, "https", v, sizeof v) == 2 && !strcmp(v, "no"));
    q->limit = h->GetSetting(NAME, "posts", v, sizeof v) > 0 ? 0 : 10;
    if (!q->limit) {
        const char *p;
        for (p = v; *p >= '0' && *p <= '9'; p++) q->limit = (short)(q->limit * 10 + (*p - '0'));
        if (q->limit < 1) q->limit = 1;
        if (q->limit > M_LIMIT) q->limit = M_LIMIT;
    }
    if (h->GetSetting(NAME, "pictures", v, sizeof v) == 3 && !strcmp(v, "yes")) q->flags |= MF_PICTURES;
    if (h->GetSetting(NAME, "token_host", v, sizeof v) > 0 && same_ci(v, q->host) &&
        h->GetSetting(NAME, "token", q->token, sizeof q->token) > 0) {
        if (h->GetSetting(NAME, "token_acct", q->who, sizeof q->who) <= 0) strcpy(q->who, "?");
        q->flags |= MF_LOGGED;
    } else q->token[0] = 0;
    if ((n = h->Header(r->ref, "User-Agent", q->agent, sizeof q->agent)) <= 0) strcpy(q->agent, "Mill");
    route_parse(r->path, &q->rt);
}

/* Mill's HTTP proxy, for HTTPS: 0 if there's none */
static int proxy(Req *q)
{
    char v[200], *colon;
    long port = 0;
    if (q->h->GetSetting("http", "proxy_on", v, sizeof v) != 3 || strcmp(v, "yes")) return 0;
    if (q->h->GetSetting("http", "proxy", v, sizeof v) <= 0) return 0;
    colon = v;
    if (!strncmp(v, "http://", 7)) colon = v + 7;
    memmove(v, colon, strlen(colon) + 1);
    v[strcspn(v, "/")] = 0;
    if ((colon = strrchr(v, ':')) != NULL) {
        const char *p;
        for (p = colon + 1; *p >= '0' && *p <= '9'; p++) port = port * 10 + (*p - '0');
        *colon = 0;
    }
    if (!v[0] || strlen(v) >= sizeof q->proxy) return 0;
    strcpy(q->proxy, v);
    q->proxy_port = (unsigned short)(port > 0 && port < 65536 ? port : 80);
    return 1;
}

static OSErr request(MillPlugIn *self, MillRequest *r)
{
    const MillHost *h = self->host;
    Req *q;
    OSErr err = noErr;
    int post = r->len > 5 && !memcmp(r->text, "POST ", 5);
    if (r->protocol != 0) return millUnknown;
    if (!(q = h->NewMem(sizeof *q))) return memFullErr;
    memset(q, 0, sizeof *q);
    setup(q, h, r);
    {
        /* a host name or address, nothing that could break a request line */
        const char *c;
        for (c = r->host; *c; c++)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '-' ||
                  *c == '_' || *c == ':' || *c == '[' || *c == ']'))
                break;
        if (*c || !r->host[0] || !q->hostname[0]) {
            h->Fail(r->ref, "Mastodon: that isn\325t an instance\325s name.");
            h->FreeMem(q);
            return noErr;
        }
    }
    if (q->https && !proxy(q)) {
        h->Fail(r->ref, "Mastodon: HTTPS requires an HTTP proxy. "
                        "See Settings, under HTTP.");
        h->FreeMem(q);
        return noErr;
    }
    q->title = q->host;
    switch (q->rt.kind) {
    case RT_LOCAL: q->title = "Local timeline"; break;
    case RT_PUBLIC: q->title = "Federated timeline"; break;
    case RT_HOME: q->title = "Home"; break;
    case RT_TAG: {
        long a[1];
        a[0] = (long)q->rt.tag;
        h->Format(q->title_buf, sizeof q->title_buf, "#%s", a);
        h->UrlDecode(q->title_buf, q->title_buf, sizeof q->title_buf);
        q->title = q->title_buf;
        break;
    }
    case RT_ACCOUNT: {
        long a[1];
        a[0] = (long)q->rt.user;
        h->Format(q->title_buf, sizeof q->title_buf, "@%s", a);
        q->title = q->title_buf;
        break;
    }
    case RT_STATUS: q->title = "Thread"; break;
    }
    /* what changes anything is a POST from this Plug-In's own pages, never a GET */
    if (post != (q->rt.kind == RT_POST || q->rt.kind == RT_ACT) && !(q->rt.kind == RT_LOGIN)) {
        q->code = 405;
        strcpy(q->model.error, post ? "That address can\342\200\231t be sent to" : "That address is for a form");
        server_said(q);
        page_end(&q->out);
    } else if (post && h->From(r->ref) != millFromOwnPage) {
        h->Fail(r->ref, "Mastodon: only its own pages can send this.");
    } else {
        switch (q->rt.kind) {
        case RT_LOCAL: case RT_PUBLIC: case RT_HOME: case RT_TAG: err = timeline(q); break;
        case RT_ACCOUNT: err = account(q); break;
        case RT_STATUS: err = thread(q); break;
        case RT_LOGIN: err = login(q, post); break;
        case RT_POST: case RT_ACT: err = act(q); break;
        default:
            q->code = 404;
            strcpy(q->model.error, "Mastodon has no such address");
            server_said(q);
            page_end(&q->out);
        }
    }
    memset(q->token, 0, sizeof q->token);
    h->FreeMem(q);
    return err == userCanceledErr ? noErr : err;
}

static void setting(MillSetting *s, const char *key, const char *label, const char *note, short kind,
                    const char *dflt, long min, long max)
{
    s->key = key;
    s->label = label;
    s->note = note;
    s->kind = kind;
    s->dflt = dflt;
    s->min = min;
    s->max = max;
    s->choices = 0;
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Me *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return 0;
    me->methods.close = me_close;
    me->methods.request = request;
    me->methods.purge = 0;
    me->methods.format = 0;
    me->methods.font = 0;
    me->methods.gzOut = 0;
    me->base.methods = &me->methods;
    setting(&me->settings[0], "https", "Use HTTPS",
            "Through the HTTP proxy. Off: plain HTTP, for local servers.", millCheckbox, "yes", 0, 0);
    setting(&me->settings[1], "posts", "Posts per page", NULL, millNumber, "10", 1, M_LIMIT);
    setting(&me->settings[2], "pictures", "Show pictures", "Shows pictures in posts. Off: links.", millCheckbox, "no", 0, 0);
    me->protocol.scheme = NAME;
    me->protocol.heading = "Mastodon";
    me->protocol.description = "Mastodon\325s client API (timelines, accounts, threads)";
    me->protocol.blockSize = 0;
    me->protocol.settingCount = 3;
    me->protocol.settings = me->settings;
    info->name = NAME;
    info->version = "1.0";
    info->copyright = "Copyright 2026 OneInAMillion Software";
    info->by = "OneInAMillion Software";
    info->description = "Mastodon timelines, accounts, and threads.";
    info->flags = 0;
    info->protocolCount = 1;
    info->protocols = &me->protocol;
    info->formatCount = 0;
    info->formats = 0;
    return &me->base;
}
