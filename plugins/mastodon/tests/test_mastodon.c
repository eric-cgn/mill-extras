/* Host tests for the Mastodon Plug-In's plain C parts: the JSON reader (json.c), post HTML
   as HTML 3.2 (content.c), and addresses and pages (masto.c). The nasty cases: broken
   JSON, huge strings, deep nesting, invalid UTF-8 and lone surrogates, input a byte at a
   time, output that won't fit, and Mill wanting no more. ToMacRoman is Mill's own
   (src/core/charset.c), as in src/mac_plugin.c. Built and run by run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../json.h"
#include "../content.h"
#include "../masto.h"
#include "charset.h"

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define SAME(out, want) do { const char *o_ = (out), *w_ = (want); checks++; if (strcmp(o_, w_)) { \
    printf("FAIL %s:%d:\n--- got\n%s\n--- wanted\n%s\n", __FILE__, __LINE__, o_, w_); failures++; } } while (0)
#define HAS(out, want) do { const char *o_ = (out); checks++; if (!strstr(o_, want)) { \
    printf("FAIL %s:%d: no \"%s\" in\n%s\n", __FILE__, __LINE__, want, o_); failures++; } } while (0)
#define HASNT(out, want) do { const char *o_ = (out); checks++; if (strstr(o_, want)) { \
    printf("FAIL %s:%d: \"%s\" in\n%s\n", __FILE__, __LINE__, want, o_); failures++; } } while (0)

/* Mill's ToMacRoman for UTF-8 */
static long to_mac(void *ref, const char *in, long len, char *out, long size)
{
    const unsigned char *s = (const unsigned char *)in;
    long i = 0, o = 0;
    (void)ref;
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

/* ---- JSON: events as a trace ---------------------------------------------- */

static char trace[1 << 16];
static long tlen, pieces, biggest, stop_at = -1;
static int split_char;      /* a piece ended inside a UTF-8 character */

static void tr(const char *s)
{
    long n = strlen(s);
    if (tlen + n < (long)sizeof trace - 1) { memcpy(trace + tlen, s, n); tlen += n; trace[tlen] = 0; }
}

static int count_stop(void) { return stop_at >= 0 && --stop_at < 0; }

static int t_begin(void *ref, int kind, const char *key)
{
    (void)ref;
    if (*key) { tr(key); tr(":"); }
    tr(kind == JSON_OBJECT ? "{" : "[");
    return count_stop();
}
static int t_end(void *ref, int kind) { (void)ref; tr(kind == JSON_OBJECT ? "}" : "]"); return count_stop(); }
static int t_scalar(void *ref, int kind, const char *key, const char *text)
{
    (void)ref;
    (void)kind;
    if (*key) { tr(key); tr(":"); }
    tr(text);
    tr(",");
    return count_stop();
}
static int in_str;
static int t_string(void *ref, const char *key, const char *p, long n, int last)
{
    char b[JSON_PIECE + 16];
    (void)ref;
    if (!in_str) { if (*key) { tr(key); tr(":"); } tr("\""); }
    in_str = !last;
    pieces++;
    if (n > biggest) biggest = n;
    /* a piece is whole characters: it never ends after a lead byte or mid-character */
    if (n > 0) {
        long i = n - 1, k = 0;
        while (i > 0 && ((unsigned char)p[i] & 0xC0) == 0x80) { i--; k++; }
        if ((unsigned char)p[i] >= 0xC0 && k + 1 != ((unsigned char)p[i] >= 0xF0 ? 4 : (unsigned char)p[i] >= 0xE0 ? 3 : 2)) split_char = 1;
    }
    if (tlen + n < (long)sizeof trace - 64) { memcpy(b, p, n); b[n] = 0; tr(b); }
    if (last) tr("\",");
    return count_stop();
}

static const JsonSink tsink = { NULL, t_begin, t_end, t_scalar, t_string };

/* in, chunk bytes at a time (0: all at once): the trace, and json_feed's answer then json_end's */
static int parse(const char *in, long len, long chunk)
{
    JsonReader j;
    long i, k;
    int r = JSON_MORE;
    tlen = 0;
    trace[0] = 0;
    pieces = biggest = 0;
    split_char = in_str = 0;
    json_begin(&j, &tsink);
    if (chunk <= 0) chunk = len ? len : 1;
    for (i = 0; i < len && r == JSON_MORE; i += k) {
        k = len - i < chunk ? len - i : chunk;
        r = json_feed(&j, in + i, k);
    }
    if (r == JSON_MORE) r = json_end(&j);
    return r;
}
static int ps(const char *in) { return parse(in, strlen(in), 0); }

static void test_json(void)
{
    const char *doc = "{\"a\": [1, -2.5e3, true, false, null], \"b\": {\"c\": \"d\"}, \"e\": [], \"f\": {}}";
    CHECK(ps(doc) == JSON_DONE);
    SAME(trace, "{a:[1,-2.5e3,true,false,null,]b:{c:\"d\",}e:[]f:{}}");
    /* a byte at a time, and in threes: the same */
    CHECK(parse(doc, strlen(doc), 1) == JSON_DONE);
    SAME(trace, "{a:[1,-2.5e3,true,false,null,]b:{c:\"d\",}e:[]f:{}}");
    CHECK(parse(doc, strlen(doc), 3) == JSON_DONE);
    SAME(trace, "{a:[1,-2.5e3,true,false,null,]b:{c:\"d\",}e:[]f:{}}");
    /* scalars at the top; a number is finished by the end */
    CHECK(ps("123") == JSON_DONE);
    SAME(trace, "123,");
    CHECK(ps(" \"x\" ") == JSON_DONE);
    CHECK(ps("null") == JSON_DONE);
    /* what follows the value is ignored */
    CHECK(ps("[1] garbage") == JSON_DONE);
    /* escapes */
    CHECK(ps("[\"a\\\"b\\\\c\\/d\\n\\t\\u00e9\\u20AC\"]") == JSON_DONE);
    SAME(trace, "[\"a\"b\\c/d\n\t\xc3\xa9\xe2\x82\xac\",]");
    /* a surrogate pair; lone halves, and \u0000, are U+FFFD */
    CHECK(ps("[\"\\ud83d\\ude00\"]") == JSON_DONE);
    SAME(trace, "[\"\xf0\x9f\x98\x80\",]");
    CHECK(ps("[\"\\ud83dx\\ude00y\\u0000\\ud83d\\ud83d\\ude00\"]") == JSON_DONE);
    SAME(trace, "[\"\xef\xbf\xbdx\xef\xbf\xbdy\xef\xbf\xbd\xef\xbf\xbd\xf0\x9f\x98\x80\",]");
    CHECK(ps("[\"\\ud83d\\n\"]") == JSON_DONE);
    SAME(trace, "[\"\xef\xbf\xbd\n\",]");
    CHECK(ps("[\"\\ud83d\"]") == JSON_DONE);
    SAME(trace, "[\"\xef\xbf\xbd\",]");
    /* invalid UTF-8: a stray continuation, an overlong, a surrogate, a cut-short character,
       bytes that never start one, and a character cut by the string's end */
    CHECK(ps("[\"a\x80" "b\xc0\x80" "c\xed\xa0\x80" "d\xe2\x82" "e\xff\xfe\xc3\"]") == JSON_DONE);
    SAME(trace, "[\"a\xef\xbf\xbd" "b\xef\xbf\xbd\xef\xbf\xbd" "c\xef\xbf\xbd" "d\xef\xbf\xbd" "e\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\",]");
    CHECK(ps("[\"\xf4\x90\x80\x80\xe0\x80\xaf\"]") == JSON_DONE);
    SAME(trace, "[\"\xef\xbf\xbd\xef\xbf\xbd\",]");
    /* valid four-byte UTF-8 is kept as it is */
    CHECK(ps("[\"\xf0\x9f\x92\xbe\"]") == JSON_DONE);
    SAME(trace, "[\"\xf0\x9f\x92\xbe\",]");
    /* broken documents */
    CHECK(ps("[1,]") == JSON_ERROR);
    CHECK(ps("{\"a\" 1}") == JSON_ERROR);
    CHECK(ps("{\"a\":1,}") == JSON_ERROR);
    CHECK(ps("{1:2}") == JSON_ERROR);
    CHECK(ps("[tru]") == JSON_ERROR);
    CHECK(ps("[nul") == JSON_ERROR);
    CHECK(ps("[01]") == JSON_ERROR);
    CHECK(ps("[-]") == JSON_ERROR);
    CHECK(ps("[1e]") == JSON_ERROR);
    CHECK(ps("[1.]") == JSON_ERROR);
    CHECK(ps("[.5]") == JSON_ERROR);
    CHECK(ps("[1-2]") == JSON_ERROR);
    CHECK(ps("[\"a\nb\"]") == JSON_ERROR);
    CHECK(ps("[\"\\x\"]") == JSON_ERROR);
    CHECK(ps("[\"\\u12G4\"]") == JSON_ERROR);
    CHECK(ps("[1}") == JSON_ERROR);
    CHECK(ps("{]") == JSON_ERROR);
    CHECK(ps("]") == JSON_ERROR);
    CHECK(ps("") == JSON_ERROR);
    CHECK(ps("[\"open") == JSON_ERROR);
    CHECK(ps("[1, 2") == JSON_ERROR);
    CHECK(ps("<html><body>[1]</body></html>") == JSON_ERROR);
    CHECK(ps("[12345678901234567890123456789012345678901234567890]") == JSON_ERROR);
    CHECK(ps("\xef\xbb\xbf[1]") == JSON_ERROR);
    /* nesting: JSON_DEPTH deep is fine, one more isn't; and a flood of [ ends quickly */
    {
        char deep[JSON_DEPTH * 2 + 4];
        long i, n = 1 << 20;
        char *flood = malloc(n);
        for (i = 0; i < JSON_DEPTH; i++) { deep[i] = '['; deep[JSON_DEPTH + i] = ']'; }
        deep[JSON_DEPTH * 2] = 0;
        CHECK(ps(deep) == JSON_DONE);
        deep[0] = 0;
        for (i = 0; i <= JSON_DEPTH; i++) strcat(deep, "[");
        CHECK(ps(deep) == JSON_TOO_DEEP);
        memset(flood, '[', n);
        CHECK(parse(flood, n, 4096) == JSON_TOO_DEEP);
        memset(flood, '{', n);
        CHECK(parse(flood, n, 4096) == JSON_ERROR);    /* { wants a key */
        free(flood);
    }
    /* a long key matches nothing; one that fits is whole */
    CHECK(ps("{\"0123456789012345678901234567890123456789\":1,\"0123456789012345678901234567890\":2}") == JSON_DONE);
    SAME(trace, "{?:1,0123456789012345678901234567890:2,}");
    /* a huge string (1 MB of mixed characters), in pieces of whole characters */
    {
        long n = 1 << 20, i;
        char *big = malloc(n + 8);
        const char *pat = "ab\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\\n";
        long pl = strlen(pat);
        big[0] = '[';
        big[1] = '"';
        for (i = 2; i + pl < n - 3; i += pl) memcpy(big + i, pat, pl);
        big[i] = '"';
        big[i + 1] = ']';
        CHECK(parse(big, i + 2, 777) == JSON_DONE);
        CHECK(biggest <= JSON_PIECE + 4 && pieces > 1000 && !split_char);
        CHECK(parse(big, i + 2, 1) == JSON_DONE);
        CHECK(!split_char);
        free(big);
    }
    /* the sink says stop: the reading stops there */
    stop_at = 2;
    CHECK(ps("[1,2,3,4]") == JSON_STOP);
    SAME(trace, "[1,2,");
    stop_at = -1;
    /* json_utf8 */
    {
        char b[4];
        CHECK(json_utf8(0x41, b) == 1 && b[0] == 'A');
        CHECK(json_utf8(0xE9, b) == 2 && !memcmp(b, "\xc3\xa9", 2));
        CHECK(json_utf8(0x20AC, b) == 3 && !memcmp(b, "\xe2\x82\xac", 3));
        CHECK(json_utf8(0x1F600, b) == 4 && !memcmp(b, "\xf0\x9f\x98\x80", 4));
    }
}

/* ---- Post HTML ----------------------------------------------------------- */

static char cbuf[70000];

static const char *conv_in(const char *in, long len, long cap, long chunk, int *cut)
{
    Content c;
    long i, k;
    content_begin(&c, cbuf, cap, "inst.example", NULL, to_mac);
    if (chunk <= 0) chunk = len ? len : 1;
    for (i = 0; i < len; i += k) {
        k = len - i < chunk ? len - i : chunk;
        content_data(&c, in + i, k);
    }
    content_end(&c);
    if (cut) *cut = c.cut;
    CHECK((long)strlen(cbuf) == c.len && c.len < cap);
    return cbuf;
}
static const char *conv(const char *in) { return conv_in(in, strlen(in), sizeof cbuf, 0, NULL); }

static void test_content(void)
{
    int cut;
    SAME(conv("<p>Hello <a href=\"https://x.example/z\" rel=\"nofollow\">link</a></p><p>Two<br>lines</p>"),
         "Hello <A HREF=\"https://x.example/z\">link</A><P>Two<BR>lines");
    /* Mastodon's long links: the hidden parts dropped, an ellipsis where it's cut */
    SAME(conv("<a href=\"https://example.com/very/long/path\"><span class=\"invisible\">https://</span><span class=\"ellipsis\">example.com/very/lo</span><span class=\"invisible\">ng/path</span></a>"),
         "<A HREF=\"https://example.com/very/long/path\">example.com/very/lo\xc9</A>");
    /* hashtags and mentions go to mastodon: pages */
    SAME(conv("<a href=\"https://m.example/tags/retro\" class=\"mention hashtag\" rel=\"tag\">#<span>retro</span></a>"),
         "<A HREF=\"mastodon://inst.example/tags/retro\">#retro</A>");
    SAME(conv("<span class=\"h-card\"><a href=\"https://other.example/@bob\" class=\"u-url mention\">@<span>bob</span></a></span>"),
         "<A HREF=\"mastodon://inst.example/@bob@other.example\">@bob</A>");
    SAME(conv("<a href=\"https://inst.example/users/al.ice\" class=\"u-url mention\">@al.ice</a>"),
         "<A HREF=\"mastodon://inst.example/@al.ice\">@al.ice</A>");
    SAME(conv("<a href=\"https://snarc.example/eric\" class=\"mention\">@eric</a>"),
         "<A HREF=\"mastodon://inst.example/@eric@snarc.example\">@eric</A>");
    /* a mention or tag that can't be read stays a web link */
    SAME(conv("<a href=\"https://m.example/tags/\" class=\"hashtag\">#</a>"), "<A HREF=\"https://m.example/tags/\">#</A>");
    /* links that aren't the web's are text */
    SAME(conv("<a href=\"javascript:alert(1)\">x</a> <a href=\"data:text/html,hi\">y</a> <a>z</a>"), "x y z");
    SAME(conv("<a href=\"mailto:a@b.example\">mail</a>"), "<A HREF=\"mailto:a@b.example\">mail</A>");
    /* attribute quoting: > in a quoted value, &amp; read, " and < > made safe */
    SAME(conv("<a href=\"https://a.example/?q=1>2&amp;r=3\">t</a>"), "<A HREF=\"https://a.example/?q=1&gt;2&amp;r=3\">t</A>");
    SAME(conv("<a href='https://a.example/\"x\"'>t</a>"), "<A HREF=\"https://a.example/&quot;x&quot;\">t</A>");
    SAME(conv("<a href=https://a.example/x>t</a>"), "<A HREF=\"https://a.example/x\">t</A>");
    SAME(conv("<a href=\"https://a.example/caf\xc3\xa9 x\">t</a>"), "<A HREF=\"https://a.example/caf%C3%A9%20x\">t</A>");
    /* scripts and styles go, with what's in them */
    SAME(conv("<script>alert(1)</script>ok<style>p{}</style>!"), "ok!");
    /* entities, and & that isn't one */
    SAME(conv("&amp; &lt;b&gt; &#233; &#xE9; &quot;&apos; &bogus; & a&b"), "&amp; &lt;b&gt; \x8e \x8e \"' &amp;bogus; &amp; a&amp;b");
    SAME(conv("&#0; &#xD800; &#99999999; &#; &#x;"), "? ? ? &amp;#; &amp;#x;");
    SAME(conv("x &amp"), "x &amp;amp");
    /* text: UTF-8 made Mac Roman, < > & made entities */
    SAME(conv("Caf\xc3\xa9 \xe2\x80\x9cMill\xe2\x80\x9d 1 < 2 > 0"), "Caf\x8e \xd2Mill\xd3 1 &lt; 2 &gt; 0");
    /* elements kept, and closed at the end if left open */
    SAME(conv("<strong>b</strong><em>i</em><code>c</code><del>s</del>"), "<B>b</B><I>i</I><CODE>c</CODE><STRIKE>s</STRIKE>");
    SAME(conv("<blockquote><p>q</p></blockquote><ul><li>one</li><li>two</li></ul><ol><li>x</ol>"),
         "<BLOCKQUOTE>q</BLOCKQUOTE><UL><LI>one<LI>two</UL><OL><LI>x</OL>");
    SAME(conv("<pre>a\n  b</pre>"), "<PRE>a\n  b</PRE>");
    SAME(conv("<b><i>open"), "<B><I>open</I></B>");
    SAME(conv("<b><i>x</b>y</i>"), "<B><I>x</I></B>y");
    SAME(conv("</b></a></span>x"), "x");
    SAME(conv("<h1>Head</h1>text"), "<B>Head</B>text");
    SAME(conv("x<h2>H</h2>"), "x<P><B>H</B>");
    SAME(conv("a <3 b<"), "a &lt;3 b&lt;");
    SAME(conv("<p><p>x</p><p></p><p>y"), "x<P>y");
    /* other tags go, keeping their text; comments go */
    SAME(conv("<div><table><tr><td>cell</td></tr></table></div><!-- c --><font color=red>f</font>"), "cellf");
    SAME(conv("<img src=\"x\" alt=\":blob:\"> <img src=\"y\">"), ":blob: ");
    SAME(conv("<p>One</p><p>Two</p>"), "One<P>Two");
    /* nesting past CONTENT_OPEN: the rest aren't opened, all are closed */
    SAME(conv("<b><i><b><i><b><i><b><i><b><i>deep"), "<B><I><B><I><B><I><B><I>deep</I></B></I></B></I></B></I></B>");
    /* spans nested deep, hidden and not */
    {
        char in[4000] = "";
        int i;
        for (i = 0; i < 40; i++) strcat(in, "<span class=\"invisible\">");
        strcat(in, "hidden");
        for (i = 0; i < 40; i++) strcat(in, "</span>");
        strcat(in, "shown");
        SAME(conv(in), "shown");
    }
    /* a tag longer than CONTENT_TAG is still ended by its > */
    {
        char in[3000] = "<a class=\"";
        memset(in + 10, 'x', 2000);
        strcpy(in + 2010, "\" href=\"https://a.example/\">t</a>after");
        SAME(conv(in), "tafter");
    }
    /* an unended tag at the end: dropped */
    SAME(conv("text<a href=\"https://a.example"), "text");
    /* a byte at a time: the same */
    {
        const char *in = "<p>Caf\xc3\xa9 &amp; <a href=\"https://a.example/x\" class=\"u-url mention\">@<span>x</span></a> <span class=\"invisible\">h</span><b>b</p>";
        char whole[400];
        strcpy(whole, conv(in));
        SAME(conv_in(in, strlen(in), sizeof cbuf, 1, NULL), whole);
    }
    /* too much: cut, below the buffer's end, with what's open closed */
    {
        long n = 200000, i;
        char *big = malloc(n + 16);
        strcpy(big, "<blockquote><b>");
        for (i = 15; i < n; i++) big[i] = "a\xc3\xa9 &lt;"[(i - 15) % 8 == 1 ? 1 : 0];
        for (i = 15; i + 3 < n; i += 3) { big[i] = 'a'; big[i + 1] = '\xc3'; big[i + 2] = '\xa9'; }
        big[i] = 0;
        conv_in(big, strlen(big), 1000, 333, &cut);
        CHECK(cut);
        CHECK(strlen(cbuf) < 1000);
        HAS(cbuf, "</B></BLOCKQUOTE>");
        CHECK(!strcmp(cbuf + strlen(cbuf) - 17, "</B></BLOCKQUOTE>"));
        free(big);
    }
    /* invalid UTF-8 in the text (json.c makes it U+FFFD first, but still) never crashes */
    conv("\xff\xfe<\xc3>\xe2\x82");
    /* a tiny buffer: only the reserve is too much, so nothing goes in, but it's ended */
    conv_in("<b>x</b>", 8, CONTENT_RESERVE + 1, 0, &cut);
    SAME(cbuf, "");
}

/* ---- Addresses ------------------------------------------------------------ */

static void test_routes(void)
{
    Route rt;
    char api[300];
    route_parse("/", &rt); CHECK(rt.kind == RT_LOCAL);
    route_parse("", &rt); CHECK(rt.kind == RT_LOCAL);
    route_parse("/public", &rt); CHECK(rt.kind == RT_PUBLIC);
    route_parse("/public/local", &rt); CHECK(rt.kind == RT_LOCAL);
    route_parse("/home", &rt); CHECK(rt.kind == RT_HOME);
    route_parse("/home/", &rt); CHECK(rt.kind == RT_HOME);
    route_parse("/login", &rt); CHECK(rt.kind == RT_LOGIN);
    route_parse("/post", &rt); CHECK(rt.kind == RT_POST);
    route_parse("/tags/retro", &rt); CHECK(rt.kind == RT_TAG && !strcmp(rt.tag, "retro"));
    route_parse("/tags/caf%C3%A9?max_id=5", &rt); CHECK(rt.kind == RT_TAG && !strcmp(rt.tag, "caf%C3%A9") && !strcmp(rt.max_id, "5"));
    route_parse("/@eric", &rt); CHECK(rt.kind == RT_ACCOUNT && !strcmp(rt.user, "eric"));
    route_parse("/@eric@example.com", &rt); CHECK(rt.kind == RT_ACCOUNT && !strcmp(rt.user, "eric@example.com"));
    route_parse("/@eric/113500000000000003", &rt); CHECK(rt.kind == RT_STATUS && !strcmp(rt.id, "113500000000000003"));
    route_parse("/@bob@other.example/d5f3832f", &rt); CHECK(rt.kind == RT_STATUS && !strcmp(rt.user, "bob@other.example"));
    route_parse("/@eric/123/act", &rt); CHECK(rt.kind == RT_ACT && !strcmp(rt.id, "123"));
    route_parse("/@eric?max_id=99", &rt); CHECK(rt.kind == RT_ACCOUNT && !strcmp(rt.max_id, "99"));
    route_parse("/?limit=3&max_id=abc&x", &rt); CHECK(rt.kind == RT_LOCAL && !strcmp(rt.max_id, "abc"));
    /* not addresses */
    route_parse("/@", &rt); CHECK(rt.kind == RT_BAD);
    route_parse("/@@x", &rt); CHECK(rt.kind == RT_BAD);
    route_parse("/@x@", &rt); CHECK(rt.kind == RT_BAD);
    route_parse("/@a@b@c", &rt); CHECK(rt.kind == RT_BAD);
    route_parse("/@eric/12 3", &rt); CHECK(rt.kind == RT_BAD);
    route_parse("/@eric/1/2", &rt); CHECK(rt.kind == RT_BAD);
    route_parse("/@e<ric", &rt); CHECK(rt.kind == RT_BAD);
    route_parse("/tags/", &rt); CHECK(rt.kind == RT_BAD);
    route_parse("/tags/a/b", &rt); CHECK(rt.kind == RT_BAD);
    route_parse("/foo", &rt); CHECK(rt.kind == RT_BAD);
    route_parse("/?max_id=<x>", &rt); CHECK(rt.kind == RT_BAD);
    route_parse("/@../x", &rt); CHECK(rt.kind == RT_BAD);
    {
        char longu[400] = "/@";
        memset(longu + 2, 'a', 300);
        longu[302] = 0;
        route_parse(longu, &rt);
        CHECK(rt.kind == RT_BAD);
    }
    /* their API paths */
    route_parse("/", &rt);
    CHECK(route_api(&rt, NULL, 10, api, sizeof api) > 0);
    SAME(api, "/api/v1/timelines/public?local=true&limit=10");
    route_parse("/public?max_id=77", &rt);
    route_api(&rt, NULL, 5, api, sizeof api);
    SAME(api, "/api/v1/timelines/public?limit=5&max_id=77");
    route_parse("/home", &rt);
    route_api(&rt, NULL, 99, api, sizeof api);
    SAME(api, "/api/v1/timelines/home?limit=40");
    route_parse("/tags/caf%C3%A9", &rt);
    route_api(&rt, NULL, 20, api, sizeof api);
    SAME(api, "/api/v1/timelines/tag/caf%C3%A9?limit=20");
    route_parse("/@eric?max_id=12", &rt);
    route_api(&rt, "d5f3", 10, api, sizeof api);
    SAME(api, "/api/v1/accounts/d5f3/statuses?limit=10&max_id=12");
    CHECK(route_api(&rt, NULL, 10, api, sizeof api) < 0);
    CHECK(route_api(&rt, "d5f3", 10, api, 20) < 0);
    route_parse("/@eric/1", &rt);
    CHECK(route_api(&rt, "1", 10, api, sizeof api) < 0);
}

/* ---- Pages ---------------------------------------------------------------- */

static char page[1 << 20];
static long plen, plimit = -1;

static int p_write(void *ref, const char *p, long n)
{
    (void)ref;
    if (plimit >= 0 && plen + n > plimit) return 0;
    if (plen + n >= (long)sizeof page) return 0;
    memcpy(page + plen, p, n);
    plen += n;
    page[plen] = 0;
    return 1;
}
static const MOut pout = { NULL, p_write, to_mac };

static char *slurp(const char *dir, const char *name, long *len)
{
    char path[400];
    FILE *f;
    char *b;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!(f = fopen(path, "rb"))) { printf("can't read %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(*len + 1);
    if (fread(b, 1, *len, f) != (size_t)*len) exit(1);
    b[*len] = 0;
    fclose(f);
    return b;
}

static Model model;

static int show(const char *in, long len, short parse_as, short flags, long chunk)
{
    long i, k;
    int r = JSON_MORE;
    plen = 0;
    page[0] = 0;
    model_begin(&model, &pout, "inst.example", parse_as, flags);
    if (chunk <= 0) chunk = len;
    for (i = 0; i < len && r == JSON_MORE; i += k) {
        k = len - i < chunk ? len - i : chunk;
        r = model_data(&model, in + i, k);
    }
    if (r == JSON_MORE) r = model_end(&model);
    return r;
}

static void test_pages(const char *dir)
{
    long len;
    char *tl = slurp(dir, "timeline.json", &len), *whole;
    Route rt;
    CHECK(show(tl, len, MP_STATUSES, 0, 0) == JSON_DONE);
    CHECK(model.count == 3);
    SAME(model.last_id, "113500000000000001");
    /* the first: author (UTF-8 name, the emoji made '?'), date, links, content, picture as a link */
    HAS(page, "<A HREF=\"mastodon://inst.example/@alice\">Alice ?</A> @alice<BR>");
    HAS(page, "<A HREF=\"mastodon://inst.example/@alice/113500000000000003\">2026-11-16 21:41</A>");
    HAS(page, "Booted my Mac Plus today. Caf\x8e \xd2Mill\xd3 works! <A HREF=\"mastodon://inst.example/tags/macplus\">#macplus</A>");
    HAS(page, "<A HREF=\"https://www.example.com/a/very/long/path/to/a/page\">example.com/a/very/long/pa\xc9</A>");
    HAS(page, "<A HREF=\"mastodon://inst.example/@bob@other.example\">@bob</A>");
    HAS(page, "<A HREF=\"https://retro.example/media/original.jpg\">[Picture]</A> A Mac Plus on a desk");
    HAS(page, "2 replies, 1 boost, 5 favorites");
    HASNT(page, "<IMG");
    HASNT(page, "<FORM");      /* not logged in */
    /* the boost: who boosted it, the boosted post's author and counts; its warning collapsed */
    HAS(page, "<P>Boosted by <A HREF=\"mastodon://inst.example/@alice\">Alice ?</A><BR>");
    HAS(page, "<A HREF=\"mastodon://inst.example/@carol@far.example\">Carol</A> @carol@far.example");
    HAS(page, "<B>Content warning:</B> Spoilers for &lt;Myst&gt; (<A HREF=\"mastodon://inst.example/@carol@far.example/113400000000000001\">Show</A>)");
    HASNT(page, "library");
    HAS(page, "In reply to</A> an earlier post");
    HAS(page, "| unlisted");
    HAS(page, "1 reply, 7 boosts, 9 favorites");
    /* the third: no display name (its acct), followers only, script and javascript: gone */
    HAS(page, "<A HREF=\"mastodon://inst.example/@dave\">dave</A> @dave");
    HAS(page, "| followers only");
    HAS(page, "Replying &amp; quoting: click");
    HASNT(page, "alert");
    HASNT(page, "javascript");
    /* a byte at a time, and in sevens: the same page */
    whole = malloc(plen + 1);
    memcpy(whole, page, plen + 1);
    CHECK(show(tl, len, MP_STATUSES, 0, 1) == JSON_DONE);
    SAME(page, whole);
    CHECK(show(tl, len, MP_STATUSES, 0, 7) == JSON_DONE);
    SAME(page, whole);
    /* logged in, with pictures: the actions, and an IMG */
    CHECK(show(tl, len, MP_STATUSES, MF_LOGGED | MF_PICTURES, 0) == JSON_DONE);
    HAS(page, "<FORM METHOD=POST ACTION=\"mastodon://inst.example/@alice/113500000000000003/act\"><INPUT TYPE=SUBMIT NAME=do VALUE=\"Favorite\"> <INPUT TYPE=SUBMIT NAME=do VALUE=\"Boost\"></FORM>");
    HAS(page, "VALUE=\"Unfavorite\"");
    HAS(page, "<A HREF=\"https://retro.example/media/original.jpg\"><IMG SRC=\"https://retro.example/media/small.jpg\" ALT=\"[Picture]\"></A> A Mac Plus on a desk");
    /* Mill wants no more: it stops, and says so */
    plimit = 300;
    CHECK(show(tl, len, MP_STATUSES, 0, 0) == JSON_STOP);
    CHECK(model.stopped && plen <= 300);
    plimit = -1;
    /* the next page's link */
    show(tl, len, MP_STATUSES, 0, 0);
    plen = 0;
    page[0] = 0;
    page_next(&pout, "inst.example", "/@alice?max_id=1", &model, 3);
    SAME(page, "<HR>\n<P><A HREF=\"mastodon://inst.example/@alice?max_id=113500000000000001\">Next page</A>\n");
    plen = 0;
    page_next(&pout, "inst.example", "/", &model, 3);
    SAME(page, "<HR>\n<P><A HREF=\"mastodon://inst.example/?max_id=113500000000000001\">Next page</A>\n");
    plen = 0;
    page[0] = 0;
    page_next(&pout, "inst.example", "/", &model, 20);
    SAME(page, "");
    /* one post, its own page: the warning open */
    {
        const char *one = "{\"id\":\"7\",\"created_at\":\"2026-01-02T03:04:05Z\",\"spoiler_text\":\"cw\",\"content\":\"<p>behind</p>\","
                          "\"account\":{\"acct\":\"x\",\"display_name\":\"X\"},\"media_attachments\":[]}";
        CHECK(show(one, strlen(one), MP_STATUS, MF_FOCUS, 0) == JSON_DONE);
        HAS(page, "<B>Content warning:</B> cw<P>behind");
        HASNT(page, "Show</A>");
        HASNT(page, "Thread</A>");
    }
    /* a context: the ancestors (stopping at the descendants), or the descendants */
    {
        const char *ctx = "{\"ancestors\":[{\"id\":\"1\",\"content\":\"<p>first</p>\",\"account\":{\"acct\":\"a\"}}],"
                          "\"descendants\":[{\"id\":\"3\",\"content\":\"<p>third</p>\",\"account\":{\"acct\":\"c\"}},"
                          "{\"id\":\"4\",\"content\":\"<p>fourth</p>\",\"account\":{\"acct\":\"d\"}}]}";
        CHECK(show(ctx, strlen(ctx), MP_ANCESTORS, 0, 0) == JSON_STOP);
        CHECK(model.count == 1 && !model.stopped);
        HAS(page, "first");
        HASNT(page, "third");
        CHECK(show(ctx, strlen(ctx), MP_DESCENDANTS, MF_INDENT, 0) == JSON_DONE);
        CHECK(model.count == 2);
        HASNT(page, "first");
        HAS(page, "<BLOCKQUOTE><P><A HREF=\"mastodon://inst.example/@c\">c</A>");
        HAS(page, "fourth");
    }
    /* an account, and an error */
    {
        const char *acct = "{\"id\":\"d5f3\",\"acct\":\"eric@example.com\",\"display_name\":\"eric\",\"note\":\"<p>Hi &amp; <b>bye</b></p>\","
                           "\"url\":\"https://social.example.com/eric\",\"followers_count\":3,\"following_count\":4,\"statuses_count\":5,"
                           "\"fields\":[{\"name\":\"x\",\"value\":\"<a href='https://e'>e</a>\"}],\"emojis\":[]}";
        CHECK(show(acct, strlen(acct), MP_ACCOUNT, 0, 0) == JSON_DONE);
        SAME(model.acct.id, "d5f3");
        SAME(model.acct.acct, "eric@example.com");
        plen = 0;
        page_account(&pout, &model);
        HAS(page, "<B>eric</B> @eric@example.com");
        HAS(page, "<BLOCKQUOTE>Hi &amp; <B>bye</B></BLOCKQUOTE>");
        HAS(page, "5 posts, 4 following, 3 followers | <A HREF=\"https://social.example.com/eric\">On the web</A>");
        /* a note of nothing but a line break isn't shown */
        CHECK(show("{\"acct\":\"e\",\"note\":\"<br> \"}", 27, MP_ACCOUNT, 0, 0) == JSON_DONE);
        plen = 0;
        page[0] = 0;
        page_account(&pout, &model);
        HASNT(page, "BLOCKQUOTE");
        CHECK(show("{\"error\":\"Record not found\"}", 29, MP_ACCOUNT, 0, 0) == JSON_DONE);
        SAME(model.error, "Record not found");
        CHECK(show("{\"error\":\"This method requires an authenticated user\"}", 55, MP_STATUSES, 0, 0) == JSON_DONE);
        SAME(model.error, "This method requires an authenticated user");
        CHECK(model.count == 0);
    }
    /* a post whose content is over M_CONTENT: cut, said so, the rest of the page fine */
    {
        long n = M_CONTENT * 3, i;
        char *big = malloc(n + 400);
        long o = sprintf(big, "[{\"id\":\"1\",\"content\":\"<p>");
        for (i = 0; i < n; i++) big[o++] = "word "[i % 5];
        o += sprintf(big + o, "</p>\",\"account\":{\"acct\":\"a\"}},{\"id\":\"2\",\"content\":\"next\",\"account\":{\"acct\":\"b\"}}]");
        CHECK(show(big, o, MP_STATUSES, 0, 100) == JSON_DONE);
        CHECK(model.count == 2);
        HAS(page, "(Cut short.)");
        HAS(page, "next");
        free(big);
    }
    /* posts with junk where things should be: types wrong, extra nesting, more pictures */
    {
        const char *junk = "[{\"id\":5,\"content\":[\"x\"],\"account\":\"nobody\",\"spoiler_text\":null,\"media_attachments\":"
                           "[{\"type\":\"image\",\"url\":\"https://a/1\"},{\"type\":\"video\",\"url\":\"https://a/2\"},"
                           "{\"type\":\"gifv\",\"url\":\"javascript:x\"},{\"type\":\"audio\",\"url\":\"https://a/4\"},"
                           "{\"type\":\"image\",\"url\":\"https://a/5\"},7],\"reblog\":{\"reblog\":{\"id\":\"deep\"}}},"
                           "{},[],1,\"x\",{\"id\":\"9\",\"account\":{\"acct\":\"<b>&\"}}]";
        CHECK(show(junk, strlen(junk), MP_STATUSES, 0, 0) == JSON_DONE);
        HAS(page, "[Video]");
        HAS(page, "[Sound]");
        HASNT(page, "javascript");
        HASNT(page, "https://a/5");
        HAS(page, "(More attachments");
        HAS(page, "@&lt;b&gt;&amp;");
        HASNT(page, "deep");
    }
    /* the page's head and post form */
    route_parse("/", &rt);
    plen = 0;
    page_head(&pout, "inst.example", "Local timeline", &rt, NULL);
    HAS(page, "<TITLE>Local timeline</TITLE>");
    HAS(page, "<B>Local</B> | <A HREF=\"mastodon://inst.example/public\">Federated</A> | <A HREF=\"mastodon://inst.example/login\">Log In</A>");
    HASNT(page, "Home");
    route_parse("/home", &rt);
    plen = 0;
    page_head(&pout, "inst.example", "Home", &rt, "eric@example.com");
    HAS(page, "<B>Home</B> | <A HREF=\"mastodon://inst.example/login\">@eric@example.com</A>");
    plen = 0;
    page_post_form(&pout, "inst.example", "123", "bob@x");
    HAS(page, "<INPUT TYPE=HIDDEN NAME=in_reply_to_id VALUE=\"123\">");
    HAS(page, "<TEXTAREA NAME=status ROWS=3 COLS=40>@bob@x </TEXTAREA>");
    HAS(page, "VALUE=\"Reply\"");
    free(whole);
    free(tl);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    test_json();
    test_content();
    test_routes();
    test_pages(dir);
    printf("%d of %d checks failed\n", failures, checks);
    printf("Memory for a page: Model %lu bytes\n", (unsigned long)sizeof(Model));
    return failures != 0;
}
