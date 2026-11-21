/* masto.c: addresses, and pages from the API's JSON (masto.h) */
#include <string.h>

#include "masto.h"

/* ---- Addresses ---------------------------------------------------------- */

static int id_char(int b) { return (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || b == '_' || b == '-'; }
static int user_char(int b) { return id_char(b) || b == '.' || b == '@'; }
static int tag_char(int b) { return id_char(b) || b == '%' || b >= 0x80; }

/* s up to a '/' or the end (n bytes), if every byte passes ok and it fits */
static int take(const char *s, long n, int (*ok)(int), char *out, long size)
{
    long i;
    if (n <= 0 || n >= size) return 0;
    for (i = 0; i < n; i++) if (!ok((unsigned char)s[i])) return 0;
    memcpy(out, s, n);
    out[n] = 0;
    return 1;
}

void route_parse(const char *path, Route *rt)
{
    const char *q = strchr(path, '?');
    long n = q ? q - path : (long)strlen(path), k;
    memset(rt, 0, sizeof *rt);
    rt->kind = RT_BAD;
    if (q) {
        /* ?max_id=ID, among others (which are ignored) */
        const char *a = q + 1;
        while (*a) {
            long l = strcspn(a, "&");
            if (l > 7 && !strncmp(a, "max_id=", 7) && !take(a + 7, l - 7, id_char, rt->max_id, sizeof rt->max_id)) return;
            a += l;
            if (*a) a++;
        }
    }
    while (n > 1 && path[n - 1] == '/') n--;    /* "/tags/x/" is "/tags/x" */
    if (n == 0 || (n == 1 && path[0] == '/')) { rt->kind = RT_LOCAL; return; }
    if (path[0] != '/') return;
    path++;
    n--;
    if ((n == 6 && !strncmp(path, "public", 6))) { rt->kind = RT_PUBLIC; return; }
    if (n == 12 && !strncmp(path, "public/local", 12)) { rt->kind = RT_LOCAL; return; }
    if (n == 4 && !strncmp(path, "home", 4)) { rt->kind = RT_HOME; return; }
    if (n == 5 && !strncmp(path, "login", 5)) { rt->kind = RT_LOGIN; return; }
    if (n == 4 && !strncmp(path, "post", 4)) { rt->kind = RT_POST; return; }
    if (n > 5 && !strncmp(path, "tags/", 5)) {
        if (take(path + 5, n - 5, tag_char, rt->tag, sizeof rt->tag)) rt->kind = RT_TAG;
        return;
    }
    if (path[0] != '@') return;
    path++;
    n--;
    for (k = 0; k < n && path[k] != '/'; k++) {}
    /* a user starts with a letter, digit, or _; it may have one @ (not at an end) */
    if (!take(path, k, user_char, rt->user, sizeof rt->user) || rt->user[0] == '@' || rt->user[0] == '.' ||
        rt->user[k - 1] == '@' || (strchr(rt->user, '@') && strchr(rt->user, '@') != strrchr(rt->user, '@')))
        return;
    if (k == n) { rt->kind = RT_ACCOUNT; return; }
    path += k + 1;
    n -= k + 1;
    for (k = 0; k < n && path[k] != '/'; k++) {}
    if (!take(path, k, id_char, rt->id, sizeof rt->id)) return;
    if (k == n) { rt->kind = RT_STATUS; return; }
    if (n - k == 4 && !strncmp(path + k, "/act", 4)) rt->kind = RT_ACT;
}

/* s onto out at *o, if it fits */
static int add(char *out, long size, long *o, const char *s)
{
    long n = strlen(s);
    if (*o + n >= size) return 0;
    memcpy(out + *o, s, n + 1);
    *o += n;
    return 1;
}

long route_api(const Route *rt, const char *id, int limit, char *out, long size)
{
    long o = 0;
    char num[8];
    int ok;
    if (limit < 1) limit = 1;
    if (limit > M_LIMIT) limit = M_LIMIT;
    num[0] = (char)('0' + limit / 10);
    num[1] = (char)('0' + limit % 10);
    num[2] = 0;
    switch (rt->kind) {
    case RT_LOCAL: ok = add(out, size, &o, "/api/v1/timelines/public?local=true&limit="); break;
    case RT_PUBLIC: ok = add(out, size, &o, "/api/v1/timelines/public?limit="); break;
    case RT_HOME: ok = add(out, size, &o, "/api/v1/timelines/home?limit="); break;
    case RT_TAG:
        ok = add(out, size, &o, "/api/v1/timelines/tag/") && add(out, size, &o, rt->tag) && add(out, size, &o, "?limit=");
        break;
    case RT_ACCOUNT:
        ok = id && add(out, size, &o, "/api/v1/accounts/") && add(out, size, &o, id) && add(out, size, &o, "/statuses?limit=");
        break;
    default:
        return -1;
    }
    ok = ok && add(out, size, &o, num[0] == '0' ? num + 1 : num);
    if (ok && rt->max_id[0]) ok = add(out, size, &o, "&max_id=") && add(out, size, &o, rt->max_id);
    return ok ? o : -1;
}

/* ---- Output -------------------------------------------------------------- */

int page_put(const MOut *o, const char *s) { return o->write(o->ref, s, strlen(s)); }

/* Mac Roman text with & < > " as entities */
static int put_mac(const MOut *o, const char *p, long n)
{
    long i, s = 0;
    for (i = 0; i < n; i++) {
        const char *e = p[i] == '&' ? "&amp;" : p[i] == '<' ? "&lt;" : p[i] == '>' ? "&gt;" : p[i] == '"' ? "&quot;" : NULL;
        if (!e) continue;
        if (i > s && !o->write(o->ref, p + s, i - s)) return 0;
        if (!page_put(o, e)) return 0;
        s = i + 1;
    }
    return i > s ? o->write(o->ref, p + s, i - s) : 1;
}

int page_text(const MOut *o, const char *utf8)
{
    char mac[200];
    long n = strlen(utf8);
    while (n > 0) {
        /* a piece at a time, cut between characters */
        long k = n > 160 ? 160 : n, m;
        while (k < n && k > 0 && ((unsigned char)utf8[k] & 0xC0) == 0x80) k--;
        if (k == 0) k = n > 160 ? 160 : n;
        m = o->to_mac(o->ref, utf8, k, mac, sizeof mac);
        if (m > 0 && !put_mac(o, mac, m)) return 0;
        utf8 += k;
        n -= k;
    }
    return 1;
}

/* An address's part (a host, a user, an ID) as it goes in an HREF: only what's safe there */
static int put_url(const MOut *o, const char *s)
{
    for (; *s; s++) {
        unsigned char b = (unsigned char)*s;
        char hex[3];
        if (b == '&') { if (!page_put(o, "&amp;")) return 0; continue; }
        if (b <= ' ' || b >= 0x7F || b == '"' || b == '<' || b == '>') {
            hex[0] = '%';
            hex[1] = "0123456789ABCDEF"[b >> 4];
            hex[2] = "0123456789ABCDEF"[b & 15];
            if (!o->write(o->ref, hex, 3)) return 0;
            continue;
        }
        if (!o->write(o->ref, (const char *)&b, 1)) return 0;
    }
    return 1;
}

/* mastodon://HOST/ and the rest */
static int put_link(const MOut *o, const char *host, const char *a, const char *b, const char *c)
{
    return page_put(o, "<A HREF=\"mastodon://") && put_url(o, host) && page_put(o, "/") &&
           (!a || put_url(o, a)) && (!b || put_url(o, b)) && (!c || put_url(o, c)) && page_put(o, "\">");
}

/* mastodon://HOST/@ACCT/ID, opened */
static int put_post_link(const MOut *o, const char *host, const char *acct, const char *id)
{
    return page_put(o, "<A HREF=\"mastodon://") && put_url(o, host) && page_put(o, "/@") && put_url(o, acct) &&
           page_put(o, "/") && put_url(o, id) && page_put(o, "\">");
}

/* A whole number */
static int put_num(const MOut *o, long v)
{
    char b[12];
    int i = sizeof b - 1;
    b[i] = 0;
    if (v < 0) v = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v && i > 0);
    return page_put(o, b + i);
}

/* "2026-11-16T21:41:26.000Z" as "2026-11-16 21:41" (UTC, as the API gives it) */
static int put_date(const MOut *o, const char *d)
{
    char s[24];
    if (strlen(d) >= 16 && d[4] == '-' && d[7] == '-' && d[10] == 'T' && d[13] == ':') {
        memcpy(s, d, 10);
        s[10] = ' ';
        memcpy(s + 11, d + 11, 5);
        s[16] = 0;
        return page_put(o, s);
    }
    return page_text(o, d);
}

/* ---- The model: JSON in, posts out --------------------------------------- */

enum {
    R_NONE, R_SKIP, R_LIST, R_CONTEXT, R_TOP,
    R_STATUS0, R_STATUS1, R_ACCT0, R_ACCT1, R_MEDIAS0, R_MEDIAS1, R_MEDIA0, R_MEDIA1
};

/* UTF-8 p (whole characters) onto the field f of size bytes, as much as fits whole */
static void field_add(char *f, long size, const char *p, long n)
{
    long l = strlen(f);
    if (l + n >= size) {
        n = size - 1 - l;
        while (n > 0 && ((unsigned char)p[n] & 0xC0) == 0x80) n--;
        if (n <= 0) return;
    }
    memcpy(f + l, p, n);
    f[l + n] = 0;
}

static long to_mac_content(void *ref, const char *in, long len, char *out, long size)
{
    const MOut *o = ref;
    return o->to_mac(o->ref, in, len, out, size);
}

static void reset_status(Model *m, int k)
{
    memset(&m->st[k], 0, sizeof m->st[k]);
    memset(m->media[k], 0, sizeof m->media[k]);
    m->media_at[k] = -1;
    if (m->owner == k) { m->owner = -1; m->cbuf[0] = 0; }
}

static int render(Model *m);

static int m_begin(void *ref, int kind, const char *key)
{
    Model *m = ref;
    int parent = m->role[m->depth], r = R_SKIP;
    if (m->depth >= JSON_DEPTH) return 1;
    switch (parent) {
    case R_NONE:
        if (kind == JSON_ARRAY && m->parse == MP_STATUSES) r = R_LIST;
        else if (kind == JSON_OBJECT) {
            r = m->parse == MP_STATUS ? R_STATUS0 : m->parse == MP_ANCESTORS || m->parse == MP_DESCENDANTS ? R_CONTEXT : R_TOP;
        }
        break;
    case R_CONTEXT:
        if (kind == JSON_ARRAY && !strcmp(key, "ancestors")) r = m->parse == MP_ANCESTORS ? R_LIST : R_SKIP;
        else if (kind == JSON_ARRAY && !strcmp(key, "descendants")) {
            if (m->parse == MP_ANCESTORS) return 1;     /* all that's wanted is read */
            r = R_LIST;
        }
        break;
    case R_LIST:
        if (kind == JSON_OBJECT) r = R_STATUS0;
        break;
    case R_STATUS0: case R_STATUS1: {
        int k = parent == R_STATUS1;
        if (kind == JSON_OBJECT && !strcmp(key, "reblog") && !k) r = R_STATUS1;
        else if (kind == JSON_OBJECT && !strcmp(key, "account")) r = k ? R_ACCT1 : R_ACCT0;
        else if (kind == JSON_ARRAY && !strcmp(key, "media_attachments")) r = k ? R_MEDIAS1 : R_MEDIAS0;
        break;
    }
    case R_MEDIAS0: case R_MEDIAS1: {
        int k = parent == R_MEDIAS1;
        if (kind != JSON_OBJECT) break;
        if (m->st[k].nmedia >= M_MEDIA) { m->st[k].moremedia++; break; }
        m->media_at[k] = m->st[k].nmedia++;
        r = k ? R_MEDIA1 : R_MEDIA0;
        break;
    }
    }
    if (r == R_STATUS0) reset_status(m, 0), reset_status(m, 1);
    if (r == R_STATUS1) reset_status(m, 1);
    m->role[++m->depth] = (unsigned char)r;
    return 0;
}

static int m_end(void *ref, int kind)
{
    Model *m = ref;
    int r = m->role[m->depth];
    (void)kind;
    if (m->depth > 0) m->depth--;
    if (r == R_STATUS0) return render(m);
    return 0;
}

static long number(const char *t)
{
    long v = 0;
    for (; *t >= '0' && *t <= '9'; t++) if (v < 100000000L) v = v * 10 + (*t - '0');
    return v;
}

static int m_scalar(void *ref, int kind, const char *key, const char *text)
{
    Model *m = ref;
    int r = m->role[m->depth];
    if (r == R_STATUS0 || r == R_STATUS1) {
        MStatus *s = &m->st[r == R_STATUS1];
        if (kind == JSON_NUMBER) {
            if (!strcmp(key, "replies_count")) s->replies = number(text);
            else if (!strcmp(key, "reblogs_count")) s->boosts = number(text);
            else if (!strcmp(key, "favourites_count")) s->favs = number(text);
            else if (!strcmp(key, "id")) { field_add(s->id, sizeof s->id, text, strlen(text)); s->any = 1; }
        } else if (!strcmp(key, "favourited")) s->faved = kind == JSON_TRUE;
        else if (!strcmp(key, "reblogged")) s->boosted = kind == JSON_TRUE;
    } else if (r == R_TOP && kind == JSON_NUMBER) {
        MAccount *a = &m->acct;
        if (!strcmp(key, "followers_count")) a->followers = number(text);
        else if (!strcmp(key, "following_count")) a->following = number(text);
        else if (!strcmp(key, "statuses_count")) a->statuses = number(text);
        else if (!strcmp(key, "id")) field_add(a->id, sizeof a->id, text, strlen(text));
    }
    return 0;
}

/* A string field's place, by the object it's in and its key; NULL if it's not kept */
static char *field(Model *m, int r, const char *key, long *size)
{
    MStatus *s;
    MMedia *md;
    MAccount *a = &m->acct;
#define F(name, f) if (!strcmp(key, name)) { *size = sizeof f; return f; }
    switch (r) {
    case R_STATUS0: case R_STATUS1:
        s = &m->st[r == R_STATUS1];
        F("id", s->id) F("created_at", s->created) F("spoiler_text", s->spoiler) F("in_reply_to_id", s->reply_to)
        F("visibility", s->visibility) F("url", s->url)
        break;
    case R_ACCT0: case R_ACCT1:
        s = &m->st[r == R_ACCT1];
        F("acct", s->acct) F("display_name", s->name)
        break;
    case R_MEDIA0: case R_MEDIA1: {
        int k = r == R_MEDIA1;
        if (m->media_at[k] < 0) break;
        md = &m->media[k][m->media_at[k]];
        F("type", md->type) F("url", md->url) F("preview_url", md->preview) F("description", md->desc)
        break;
    }
    case R_TOP:
        F("id", a->id) F("acct", a->acct) F("display_name", a->name) F("url", a->url)
        break;
    }
#undef F
    return NULL;
}

static int m_string(void *ref, const char *key, const char *p, long n, int last)
{
    Model *m = ref;
    int r = m->role[m->depth];
    long size;
    char *f;
    /* an error reply: {"error": "..."} */
    if (m->depth == 1 && !strcmp(key, "error")) {
        field_add(m->error, sizeof m->error, p, n);
        return 0;
    }
    /* a post's content, or an account's note: made HTML 3.2 as it comes */
    if (((r == R_STATUS0 || r == R_STATUS1) && !strcmp(key, "content")) || (r == R_TOP && !strcmp(key, "note"))) {
        if (!m->in_string) {
            m->owner = r == R_TOP ? 2 : r == R_STATUS1;
            content_begin(&m->content, m->cbuf, sizeof m->cbuf, m->host, (void *)m->out, to_mac_content);
            m->in_string = 1;
        }
        content_data(&m->content, p, n);
        if (last) {
            content_end(&m->content);
            m->in_string = 0;
        }
        return 0;
    }
    if ((f = field(m, r, key, &size)) != NULL) {
        if (r == R_STATUS0 || r == R_STATUS1) m->st[r == R_STATUS1].any = 1;
        if (r == R_TOP) m->acct.any = 1;
        field_add(f, size, p, n);
    }
    return 0;
}

void model_begin(Model *m, const MOut *out, const char *host, short parse, short flags)
{
    memset(m, 0, sizeof *m);
    m->out = out;
    m->host = host;
    m->parse = parse;
    m->flags = flags;
    m->owner = -1;
    m->media_at[0] = m->media_at[1] = -1;
    m->sink.ref = m;
    m->sink.begin = m_begin;
    m->sink.end = m_end;
    m->sink.scalar = m_scalar;
    m->sink.string = m_string;
    json_begin(&m->json, &m->sink);
}

int model_data(Model *m, const char *p, long n) { return json_feed(&m->json, p, n); }

int model_end(Model *m)
{
    int r = json_end(&m->json);
    if (r == JSON_STOP && m->parse == MP_ANCESTORS && !m->stopped) return JSON_DONE;
    return r;
}

/* ---- Pages ---------------------------------------------------------------- */

#define P(s) do { if (!page_put(o, s)) return 0; } while (0)
#define T(s) do { if (!page_text(o, s)) return 0; } while (0)

int page_head(const MOut *o, const char *host, const char *title, const Route *rt, const char *who)
{
    int k = rt ? rt->kind : -1;
    P("<HTML><HEAD><TITLE>");
    T(title);
    P("</TITLE></HEAD><BODY>\n<P>");
    /* the instance's pages: the one shown is plain */
    if (k == RT_LOCAL) P("<B>Local</B>"); else { if (!put_link(o, host, NULL, NULL, NULL)) return 0; P("Local</A>"); }
    P(" | ");
    if (k == RT_PUBLIC) P("<B>Federated</B>"); else { if (!put_link(o, host, "public", NULL, NULL)) return 0; P("Federated</A>"); }
    if (who) {
        P(" | ");
        if (k == RT_HOME) P("<B>Home</B>"); else { if (!put_link(o, host, "home", NULL, NULL)) return 0; P("Home</A>"); }
    }
    P(" | ");
    if (!put_link(o, host, "login", NULL, NULL)) return 0;
    if (who) { P("@"); T(who); P("</A>"); }
    else P("Log In</A>");
    P("\n<H2>");
    T(title);
    P("</H2>\n");
    return 1;
}

int page_post_form(const MOut *o, const char *host, const char *reply_to, const char *reply_acct)
{
    P("<FORM METHOD=POST ACTION=\"mastodon://");
    if (!put_url(o, host)) return 0;
    P("/post\">");
    if (reply_to) {
        P("<INPUT TYPE=HIDDEN NAME=in_reply_to_id VALUE=\"");
        if (!put_url(o, reply_to)) return 0;
        P("\">");
    }
    P("<TEXTAREA NAME=status ROWS=3 COLS=40>");
    if (reply_acct && *reply_acct) { P("@"); T(reply_acct); P(" "); }
    P("</TEXTAREA><BR>\nWarning: <INPUT NAME=spoiler_text SIZE=20> "
      "<SELECT NAME=visibility><OPTION VALUE=public>Public<OPTION VALUE=unlisted>Unlisted"
      "<OPTION VALUE=private>Followers<OPTION VALUE=direct>Mentioned</SELECT> "
      "<INPUT TYPE=SUBMIT VALUE=\"");
    P(reply_to ? "Reply" : "Post");
    P("\"></FORM>\n");
    return 1;
}

int page_account(const MOut *o, Model *m)
{
    MAccount *a = &m->acct;
    P("<P><B>");
    T(a->name[0] ? a->name : a->acct);
    P("</B> @");
    T(a->acct);
    P("<BR>\n");
    if (m->owner == 2 && m->cbuf[0] && m->content.seen) {
        P("<BLOCKQUOTE>");
        if (!o->write(o->ref, m->cbuf, strlen(m->cbuf))) return 0;
        P("</BLOCKQUOTE>\n");
    }
    P("<P>");
    if (!put_num(o, a->statuses)) return 0;
    P(" posts, ");
    if (!put_num(o, a->following)) return 0;
    P(" following, ");
    if (!put_num(o, a->followers)) return 0;
    P(" followers");
    if (!strncmp(a->url, "https://", 8) || !strncmp(a->url, "http://", 7)) {
        P(" | <A HREF=\"");
        if (!put_url(o, a->url)) return 0;
        P("\">On the web</A>");
    }
    P("\n");
    return 1;
}

int page_next(const MOut *o, const char *host, const char *path, Model *m, int limit)
{
    long n = strcspn(path, "?");
    char p[160];
    if (m->count < limit || !m->last_id[0]) {
        if (m->count == 0) P("<P><I>No posts.</I>\n");
        return 1;
    }
    if (n >= (long)sizeof p - 1) return 1;
    memcpy(p, path[0] == '/' ? path + 1 : path, n);
    p[path[0] == '/' ? n - 1 : n] = 0;
    P("<HR>\n<P>");
    if (!put_link(o, host, p, "?max_id=", m->last_id)) return 0;
    P("Next page</A>\n");
    return 1;
}

int page_end(const MOut *o)
{
    P("</BODY></HTML>\n");
    return 1;
}

/* The post m read: st[0], or for a boost st[1] with st[0]'s account as who boosted it */
static int show(Model *m)
{
    const MOut *o = m->out;
    int boost = m->st[1].any;
    MStatus *s = &m->st[boost];
    MMedia *md = m->media[boost];
    const char *acct = s->acct[0] ? s->acct : "?";
    int k;
    P("<HR>\n");
    if (m->flags & MF_INDENT) P("<BLOCKQUOTE>");
    if (boost) {
        P("<P>Boosted by ");
        if (!put_link(o, m->host, "@", m->st[0].acct, NULL)) return 0;
        T(m->st[0].name[0] ? m->st[0].name : m->st[0].acct);
        P("</A><BR>\n");
    } else P("<P>");
    if (m->flags & MF_FOCUS) P("<B>");
    if (!put_link(o, m->host, "@", acct, NULL)) return 0;
    T(s->name[0] ? s->name : acct);
    P("</A> @");
    T(acct);
    if (m->flags & MF_FOCUS) P("</B>");
    P("<BR>\n");
    if (!put_post_link(o, m->host, acct, s->id)) return 0;
    if (!put_date(o, s->created)) return 0;
    P("</A>");
    if (s->reply_to[0]) {
        P(" | ");
        if (!put_post_link(o, m->host, acct, s->reply_to)) return 0;
        P("In reply to</A> an earlier post");
    }
    if (s->visibility[0] && strcmp(s->visibility, "public")) {
        P(" | ");
        T(!strcmp(s->visibility, "private") ? "followers only" : !strcmp(s->visibility, "direct") ? "mentioned only" : s->visibility);
    }
    P("\n");
    /* a content warning: the post behind a link, but open on its own page */
    if (s->spoiler[0]) {
        P("<P><B>Content warning:</B> ");
        T(s->spoiler);
        if (!(m->flags & MF_FOCUS)) {
            P(" (");
            if (!put_post_link(o, m->host, acct, s->id)) return 0;
            P("Show</A>)\n");
        }
    }
    if (!s->spoiler[0] || (m->flags & MF_FOCUS)) {
        if (m->owner == boost && m->cbuf[0]) {
            P("<P>");
            if (!o->write(o->ref, m->cbuf, strlen(m->cbuf))) return 0;
            P("\n");
            if (m->content.cut) P("<P><I>(Cut short.)</I>\n");
        }
        for (k = 0; k < s->nmedia; k++) {
            const char *u = md[k].url[0] ? md[k].url : md[k].preview;
            const char *what = !strcmp(md[k].type, "image") ? "Picture" : !strcmp(md[k].type, "video") ? "Video" :
                               !strcmp(md[k].type, "gifv") ? "Animation" : !strcmp(md[k].type, "audio") ? "Sound" : "File";
            if (strncmp(u, "https://", 8) && strncmp(u, "http://", 7)) continue;
            P(k ? "<BR>" : "<P>");
            P("<A HREF=\"");
            if (!put_url(o, u)) return 0;
            P("\">");
            if ((m->flags & MF_PICTURES) && (!strncmp(md[k].preview, "https://", 8) || !strncmp(md[k].preview, "http://", 7))) {
                P("<IMG SRC=\"");
                if (!put_url(o, md[k].preview)) return 0;
                P("\" ALT=\"[");
                P(what);
                P("]\"></A>");
                if (md[k].desc[0]) { P(" "); T(md[k].desc); }
            } else {
                P("[");
                P(what);
                P("]</A>");
                if (md[k].desc[0]) { P(" "); T(md[k].desc); }
            }
            P("\n");
        }
        if (s->moremedia) P("<BR>(More attachments on the web.)\n");
    }
    P("<P><FONT SIZE=-1>");
    if (!put_num(o, s->replies)) return 0;
    P(s->replies == 1 ? " reply, " : " replies, ");
    if (!put_num(o, s->boosts)) return 0;
    P(s->boosts == 1 ? " boost, " : " boosts, ");
    if (!put_num(o, s->favs)) return 0;
    P(s->favs == 1 ? " favorite" : " favorites");
    if (!(m->flags & MF_FOCUS)) {
        P(" | ");
        if (!put_post_link(o, m->host, acct, s->id)) return 0;
        P("Thread</A>");
    }
    P("</FONT>\n");
    if (m->flags & MF_LOGGED) {
        P("<FORM METHOD=POST ACTION=\"mastodon://");
        if (!put_url(o, m->host) || !page_put(o, "/@") || !put_url(o, acct) || !page_put(o, "/") || !put_url(o, s->id)) return 0;
        P("/act\"><INPUT TYPE=SUBMIT NAME=do VALUE=\"");
        P(s->faved ? "Unfavorite" : "Favorite");
        P("\"> <INPUT TYPE=SUBMIT NAME=do VALUE=\"");
        P(s->boosted ? "Unboost" : "Boost");
        P("\"></FORM>\n");
    }
    if (m->flags & MF_INDENT) P("</BLOCKQUOTE>");
    return 1;
}

/* A post is read: show it */
static int render(Model *m)
{
    if (!m->st[0].any) return 0;
    if (!show(m)) { m->stopped = 1; return 1; }
    m->count++;
    memcpy(m->last_id, m->st[0].id, sizeof m->last_id);
    return 0;
}
