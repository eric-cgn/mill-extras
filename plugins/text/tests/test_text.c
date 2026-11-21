/* Host tests for the Modern Text Plug-In's converters (text.c, gemtext.c, markdown.c,
   inline.c): each Gemtext line type (Gemini's gemtext specification), Markdown's blocks and
   inlines (worked out from the CommonMark specification's rules and examples), GFM pipe
   tables, character sets, and the nasty cases: unclosed fences, deep nesting, long lines,
   CRLF and CR, invalid UTF-8, input a byte at a time, held output that overflows, and Mill
   wanting no more. Built and run by run.sh. */
#include "harness.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *md(const char *in) { return convert(TX_MARKDOWN, TX_UTF8, in, (long)strlen(in), 0); }
static const char *gmi(const char *in) { return convert(TX_GEMTEXT, TX_UTF8, in, (long)strlen(in), 0); }

/* out is exactly want */
#define SAME(out, want) do { const char *o_ = (out); if (strcmp(o_, want)) { \
    printf("FAIL %s:%d:\n--- got\n%s\n--- wanted\n%s\n", __FILE__, __LINE__, o_, want); failures++; } } while (0)
/* out has want in it */
#define HAS(out, want) do { const char *o_ = (out); if (!strstr(o_, want)) { \
    printf("FAIL %s:%d: no \"%s\" in\n%s\n", __FILE__, __LINE__, want, o_); failures++; } } while (0)
#define HASNT(out, want) do { const char *o_ = (out); if (strstr(o_, want)) { \
    printf("FAIL %s:%d: \"%s\" in\n%s\n", __FILE__, __LINE__, want, o_); failures++; } } while (0)

static void test_charset(void)
{
    CHECK(tx_charset("text/gemini") == TX_UTF8);
    CHECK(tx_charset("text/markdown; charset=ISO-8859-1") == TX_LATIN1);
    CHECK(tx_charset("text/markdown;charset=\"utf-8\"") == TX_UTF8);
    CHECK(tx_charset("text/gemini; lang=en; charset=macintosh") == TX_MAC);
    CHECK(tx_charset("text/gemini; charset=koi8-r") == TX_GUESS);
    /* the META that tells Mill the page is Mac Roman comes first */
    md("x");
    CHECK(!strncmp(h_out, "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=macintosh\">\n", 72));
    /* UTF-8 (é is 0x8E in Mac Roman), Latin-1, and Mac Roman as it is */
    SAME(convert(TX_GEMTEXT, TX_UTF8, "Caf\xc3\xa9 \xe2\x80\x94 \xe2\x82\xac", 13, 0), "<P>Caf\x8e \xd1 \xdb\n");
    SAME(convert(TX_GEMTEXT, TX_LATIN1, "Caf\xe9", 4, 0), "<P>Caf\x8e\n");
    SAME(convert(TX_GEMTEXT, TX_MAC, "Caf\x8e", 4, 0), "<P>Caf\x8e\n");
    /* invalid UTF-8: each bad byte as Latin-1, never dropped silently or a crash */
    SAME(convert(TX_GEMTEXT, TX_UTF8, "a\xff\xc3(b\xe2\x82", 7, 0), "<P>a\xd8\xcc(b\x89\xe2\n");
    /* a byte order mark draws nothing */
    SAME(gmi("\xef\xbb\xbf# T"), "<TITLE>T</TITLE>\n<H1>T</H1>\n");
    /* control characters are '?'; NUL draws nothing */
    SAME(convert(TX_GEMTEXT, TX_UTF8, "a\001b\000c", 5, 0), "<P>a?bc\n");
}

static void test_gemtext(void)
{
    SAME(gmi("one\ntwo\n\nthree\n"), "<P>one<BR>\ntwo\n<P>three\n");
    SAME(gmi("=> http://a/b Label here  \n=>rel.gmi\n=>\t x.gmi\tTab label\n"),
         "<P><A HREF=\"http://a/b\">Label here</A><BR>\n<A HREF=\"rel.gmi\">rel.gmi</A><BR>\n"
         "<A HREF=\"x.gmi\">Tab label</A>\n");
    SAME(gmi("=>\n"), "<P>=&gt;\n");       /* no URL: text */
    SAME(gmi("=> a\"b <c>\n"), "<P><A HREF=\"a%22b\">&lt;c&gt;</A>\n");
    SAME(gmi("# One\n## Two\n### Three\n#### Four\n"),
         "<TITLE>One</TITLE>\n<H1>One</H1>\n<H2>Two</H2>\n<H3>Three</H3>\n<H3># Four</H3>\n");
    SAME(gmi("* a\n* b <i>\n*c\n"), "<UL>\n<LI>a\n<LI>b &lt;i&gt;</UL>\n<P>*c\n");
    SAME(gmi(">q1\n> q2\ntext\n"), "<BLOCKQUOTE>q1<BR>\nq2</BLOCKQUOTE>\n<P>text\n");
    SAME(gmi("```alt\n  # not a heading\n=> not a link\n\n```\nafter\n"),
         "<PRE>  # not a heading\n=&gt; not a link\n</PRE>\n<P>after\n");
    /* never closed: closed at the end */
    SAME(gmi("```\ncode"), "<PRE>code</PRE>\n");
    SAME(gmi("text\n```"), "<P>text\n<PRE></PRE>\n");
    /* CRLF and CR */
    SAME(gmi("a\r\nb\rc\r\n\r\n# d\r"), "<P>a<BR>\nb<BR>\nc\n<TITLE>d</TITLE>\n<H1>d</H1>\n");
}

/* A line far longer than TX_LINE: none of it lost, in one paragraph */
static void test_long_lines(void)
{
    static char in[60000], want[4];
    const char *o;
    long i, n = 0, letters = 0;
    for (i = 0; i < 3000; i++) n += sprintf(in + n, "w%ld ", i);
    in[n++] = '\n';
    o = gmi(in);
    CHECK(!strncmp(o, "<P>w0 w1 ", 9));
    CHECK(!strstr(o, "<BR>") && !strstr(o, "<P>w1"));
    for (i = 0; o[i]; i++) letters += o[i] == 'w';
    CHECK(letters == 3000);
    CHECK(strstr(o, "w2999") != NULL);
    /* no space to cut at: cut anyway, never inside a UTF-8 character */
    for (i = 0; i < 5000; i += 2) in[i] = '\xc3', in[i + 1] = '\xa9';
    o = convert(TX_GEMTEXT, TX_UTF8, in, 5000, 0);
    for (i = 0, letters = 0; o[i]; i++) letters += (unsigned char)o[i] == 0x8e;
    CHECK(letters == 2500);
    /* the same in Markdown: one paragraph; and in a fenced code block */
    n = 0;
    for (i = 0; i < 3000; i++) n += sprintf(in + n, "*w%ld* ", i);
    o = md(in);
    CHECK(!strncmp(o, "<P><EM>w0</EM> <EM>w1</EM>", 26));
    CHECK(strstr(o, "<EM>w2999</EM></P>") != NULL);
    for (i = 0, letters = 0; o[i]; i++) letters += o[i] == '<' && o[i + 1] == 'P' && o[i + 2] == '>';
    CHECK(letters == 1);
    n = sprintf(in, "```\n");
    for (i = 0; i < 3000; i++) n += sprintf(in + n, "x%ld ", i);
    sprintf(in + n, "\n```\nafter\n");
    o = md(in);
    CHECK(!strncmp(o, "<PRE>x0 x1", 10) && strstr(o, "x2999 </PRE>\n<P>after</P>") != NULL);
    /* a paragraph of many lines, longer than TX_PARA: all of it, in one <P> */
    n = 0;
    for (i = 0; i < 2000; i++) n += sprintf(in + n, "line %ld\n", i);
    o = md(in);
    CHECK(!strncmp(o, "<P>line 0\nline 1\n", 17) && strstr(o, "line 1999</P>") && !strstr(o, "</P>\n<P>"));
    (void)want;
}

static void test_blocks(void)
{
    SAME(md("# A\n## B ##\n###### F\n####### seven\n#no\n"),
         "<TITLE>A</TITLE>\n<H1>A</H1>\n<H2>B</H2>\n<H6>F</H6>\n<P>####### seven\n#no</P>\n");
    SAME(md("Title\n===\n\nSub\n---\n"), "<TITLE>Title</TITLE>\n<H1>Title</H1>\n<H2>Sub</H2>\n");
    SAME(md("a\nb\n---\n"), "<TITLE>a\nb</TITLE>\n<H2>a\nb</H2>\n");      /* both lines are the heading */
    SAME(md("***\n- - -\n_ _ _ _\n"), "<HR>\n<HR>\n<HR>\n");
    SAME(md("Foo\n***\nbar"), "<P>Foo</P>\n<HR>\n<P>bar</P>\n");
    /* code */
    SAME(md("    a\n\n\n    b\n\nc"), "<PRE>a\n\n\nb</PRE>\n<P>c</P>\n");
    SAME(md("para\n    not code"), "<P>para\nnot code</P>\n");
    SAME(md("``` c\n<x>\n  ```\nz"), "<PRE>&lt;x&gt;</PRE>\n<P>z</P>\n");
    SAME(md("  ```\n  a\n b\nc\n```"), "<PRE>a\nb\nc</PRE>\n");          /* the fence's indentation comes off */
    SAME(md("````\n```\n````"), "<PRE>```</PRE>\n");
    SAME(md("```\nnever closed\n# still code"), "<PRE>never closed\n# still code</PRE>\n");
    SAME(md("``` a`b\n"), "<P>``` a`b</P>\n");                          /* not a fence */
    /* HTML blocks */
    SAME(md("<div>\n*x*\n\n*y*"), "<div>\n*x*\n<P><EM>y</EM></P>\n");
    SAME(md("<!-- one\ntwo -->\n*z*"), "<!-- one\ntwo -->\n<P><EM>z</EM></P>\n");
    SAME(md("<pre>\n\n*x*\n</pre>\n*y*"), "<pre>\n\n*x*\n</pre>\n<P><EM>y</EM></P>\n");
    SAME(md("<a href=\"x\">\n*k*\n"), "<a href=\"x\">\n*k*\n");          /* kind 7 */
    SAME(md("text\n<a href=\"x\">\n"), "<P>text\n<a href=\"x\"></P>\n");  /* kind 7 can't break in */
    /* quotes */
    SAME(md("> a\nb\n> c"), "<BLOCKQUOTE>\n<P>a\nb\nc</P>\n</BLOCKQUOTE>\n");
    SAME(md("> a\n\n> b"), "<BLOCKQUOTE>\n<P>a</P>\n</BLOCKQUOTE>\n<BLOCKQUOTE>\n<P>b</P>\n</BLOCKQUOTE>\n");
    SAME(md("> # H\n> ```\n> code\n"), "<BLOCKQUOTE>\n<TITLE>H</TITLE>\n<H1>H</H1>\n<PRE>code</PRE>\n</BLOCKQUOTE>\n");
    SAME(md(">     code\n"), "<BLOCKQUOTE>\n<PRE>code</PRE>\n</BLOCKQUOTE>\n");
    SAME(md("> a\n---"), "<BLOCKQUOTE>\n<P>a</P>\n</BLOCKQUOTE>\n<HR>\n");  /* not a setext heading */
}

static void test_lists(void)
{
    SAME(md("- a\n- b\n"), "<UL>\n<LI>a\n\n<LI>b\n\n</UL>\n");
    SAME(md("- a\n\n- b\n"), "<UL>\n<LI>a\n\n<LI><P>b</P>\n\n</UL>\n");
    SAME(md("- a\n+ b\n"), "<UL>\n<LI>a\n\n</UL>\n<UL>\n<LI>b\n\n</UL>\n");
    SAME(md("3. a\n4. b\n1) c\n"), "<OL START=3>\n<LI>a\n\n<LI>b\n\n</OL>\n<OL>\n<LI>c\n\n</OL>\n");
    SAME(md("- a\n  - b\n    - c\n"), "<UL>\n<LI>a\n<UL>\n<LI>b\n<UL>\n<LI>c\n\n</UL>\n\n</UL>\n\n</UL>\n");
    SAME(md("- a\n\n  more\n- b"), "<UL>\n<LI>a\n<P>more</P>\n\n<LI><P>b</P>\n\n</UL>\n");
    SAME(md("- a\nlazy\n"), "<UL>\n<LI>a\nlazy\n\n</UL>\n");
    SAME(md("-\n  foo\n"), "<UL>\n<LI>foo\n\n</UL>\n");
    SAME(md("-\n\n  foo\n"), "<UL>\n<LI>\n</UL>\n<P>foo</P>\n");        /* an item starts with one blank line at most */
    SAME(md("para\n2. no\n- yes\n"), "<P>para\n2. no</P>\n<UL>\n<LI>yes\n\n</UL>\n");
    SAME(md("para\n-\n"), "<TITLE>para</TITLE>\n<H2>para</H2>\n");
    SAME(md("-     code\n"), "<UL>\n<LI><PRE>code</PRE>\n\n</UL>\n");
    SAME(md("1.  a\n\n    b\n"), "<OL>\n<LI>a\n<P>b</P>\n\n</OL>\n");
    SAME(md("- # head\n- > quote\n"), "<UL>\n<LI><TITLE>head</TITLE>\n<H1>head</H1>\n\n<LI><BLOCKQUOTE>\n<P>quote</P>\n</BLOCKQUOTE>\n\n</UL>\n");
    SAME(md("* * *\n"), "<HR>\n");
    SAME(md("- a\n  - b\n\n  c\n"), "<UL>\n<LI>a\n<UL>\n<LI>b\n\n</UL>\n<P>c</P>\n\n</UL>\n");
    /* tabs in the indentation */
    SAME(md("-\tone\n\tcontinued\n"), "<UL>\n<LI>one\ncontinued\n\n</UL>\n");
    SAME(md("\tcode\twith tab\n"), "<PRE>code\twith tab</PRE>\n");
}

static void test_deep(void)
{
    static char in[30000];
    const char *o;
    long i, n = 0, opens = 0, closes = 0;
    /* 200 quotes deep: TX_DEPTH of them, then '>' is text; every one opened is closed */
    for (i = 0; i < 200; i++) in[n++] = '>';
    strcpy(in + n, " deep\n");
    o = md(in);
    for (i = 0; o[i]; i++) {
        if (!strncmp(o + i, "<BLOCKQUOTE>", 12)) opens++;
        if (!strncmp(o + i, "</BLOCKQUOTE>", 13)) closes++;
    }
    CHECK(opens == TX_DEPTH && closes == opens);
    HAS(o, "&gt; deep");
    /* lists 100 deep */
    for (i = n = 0; i < 100; i++) n += sprintf(in + n, "%*s- l%ld\n", (int)(i * 2), "", i);
    o = md(in);
    for (i = opens = closes = 0; o[i]; i++) {
        if (!strncmp(o + i, "<UL>", 4)) opens++;
        if (!strncmp(o + i, "</UL>", 5)) closes++;
    }
    CHECK(opens == closes && opens >= 10 && opens <= TX_DEPTH / 2);
    HAS(o, "l99");
    /* emphasis nested deep */
    for (i = n = 0; i < 300; i++) in[n++] = '*';
    strcpy(in + n, "x");
    n++;
    for (i = 0; i < 300; i++) in[n++] = '*';
    in[n] = 0;
    o = md(in);
    HAS(o, "x");
    /* brackets */
    for (i = n = 0; i < 1000; i++) in[n++] = '[';
    in[n] = 0;
    o = md(in);
    CHECK(strlen(o) == 1000 + 8 && !strncmp(o, "<P>[[[", 6));
}

static void test_inline(void)
{
    SAME(md("*a* **b** _c_ __d__"), "<P><EM>a</EM> <STRONG>b</STRONG> <EM>c</EM> <STRONG>d</STRONG></P>\n");
    SAME(md("a * b * c"), "<P>a * b * c</P>\n");
    SAME(md("snake_case_name and *a*b"), "<P>snake_case_name and <EM>a</EM>b</P>\n");
    SAME(md("***strong emph***"), "<P><EM><STRONG>strong emph</STRONG></EM></P>\n");
    SAME(md("***strong** in emph*"), "<P><EM><STRONG>strong</STRONG> in emph</EM></P>\n");
    SAME(md("*foo**bar**baz*"), "<P><EM>foo<STRONG>bar</STRONG>baz</EM></P>\n");
    SAME(md("*foo**bar*"), "<P><EM>foo**bar</EM></P>\n");          /* the rule of 3 */
    SAME(md("**foo*"), "<P>*<EM>foo</EM></P>\n");
    SAME(md("*foo**"), "<P><EM>foo</EM>*</P>\n");
    SAME(md("_foo_bar_"), "<P><EM>foo_bar</EM></P>\n");
    SAME(md("*(*foo*)*"), "<P><EM>(<EM>foo</EM>)</EM></P>\n");
    SAME(md("`a` `` b`c `` ` `"), "<P><CODE>a</CODE> <CODE>b`c</CODE> <CODE> </CODE></P>\n");
    SAME(md("`*no*` *`yes`*"), "<P><CODE>*no*</CODE> <EM><CODE>yes</CODE></EM></P>\n");
    SAME(md("``unclosed `"), "<P>``unclosed `</P>\n");
    SAME(md("`a\nb`"), "<P><CODE>a b</CODE></P>\n");
    SAME(md("\\*\\_\\`\\\\ \\a"), "<P>*_`\\ \\a</P>\n");
    SAME(md("&copy; &#169; &#xA9; &nope &;"), "<P>&copy; &#169; &#xA9; &amp;nope &amp;;</P>\n");
    SAME(md("a  \nb\\\nc \nd"), "<P>a<BR>\nb<BR>\nc\nd</P>\n");
    SAME(md("trailing  "), "<P>trailing</P>\n");
    /* links */
    SAME(md("[a](/u) [b](</my url>) [c](/u \"t\") [d]( /u 'x' ) [e]()"),
         "<P><A HREF=\"/u\">a</A> <A HREF=\"/my%20url\">b</A> <A HREF=\"/u\">c</A> <A HREF=\"/u\">d</A> <A HREF=\"\">e</A></P>\n");
    SAME(md("[a](/p(q)r) [b](/p\\)q) [c](/x&amp;y)"),
         "<P><A HREF=\"/p(q)r\">a</A> <A HREF=\"/p)q\">b</A> <A HREF=\"/x&amp;y\">c</A></P>\n");
    SAME(md("[link [nested](/in)](/out)"), "<P>[link <A HREF=\"/in\">nested</A>](/out)</P>\n");
    SAME(md("[*em* link](/u)"), "<P><A HREF=\"/u\"><EM>em</EM> link</A></P>\n");
    SAME(md("*[a*](/u)"), "<P>*<A HREF=\"/u\">a*</A></P>\n");
    SAME(md("[a] (/u)"), "<P>[a] (/u)</P>\n");
    SAME(md("![alt **b** `c`](/i.gif)"), "<P><IMG SRC=\"/i.gif\" ALT=\"alt b c\"></P>\n");
    SAME(md("[![img](/i)](/l)"), "<P><A HREF=\"/l\"><IMG SRC=\"/i\" ALT=\"img\"></A></P>\n");
    SAME(md("<http://a.b/c?d=e&f> <x@y.org> < not> <a+b:c>"),
         "<P><A HREF=\"http://a.b/c?d=e&amp;f\">http://a.b/c?d=e&amp;f</A> <A HREF=\"mailto:x@y.org\">x@y.org</A> &lt; not&gt; <A HREF=\"a+b:c\">a+b:c</A></P>\n");
    SAME(md("<b class=\"x\">b</b> <a\nhref='y'>z</a> <?pi?> <!x> 1 < 2"),
         "<P><b class=\"x\">b</b> <a\nhref='y'>z</a> <?pi?> <!x> 1 &lt; 2</P>\n");
    SAME(md("<a b=c d>x</a><c d=\"e>"), "<P><a b=c d>x</a>&lt;c d=&quot;e&gt;</P>\n");
}

static void test_references(void)
{
    SAME(md("[Foo Bar]\n\n[foo   BAR]: /url \"title\"\n"), "<P><A HREF=\"/url\">Foo Bar</A></P>\n");
    SAME(md("[x]: /a\n[x]: /b\n\n[x]"), "<P><A HREF=\"/a\">x</A></P>\n");   /* the first counts */
    SAME(md("[a][b] [b][] [b]\n\n[b]: </b c>\n"),
         "<P><A HREF=\"/b%20c\">a</A> <A HREF=\"/b%20c\">b</A> <A HREF=\"/b%20c\">b</A></P>\n");
    SAME(md("[no] [no][] [x][no] ![no]\n"), "<P>[no] [no][] [x][no] ![no]</P>\n");
    SAME(md("[r]: /r\ntext [r]"), "<P>text <A HREF=\"/r\">r</A></P>\n");
    SAME(md("[r]:\n/r\n'title'\n\n[r]"), "<P><A HREF=\"/r\">r</A></P>\n");
    SAME(md("[r]: /r 'title' junk\n"), "<P>[r]: /r 'title' junk</P>\n");
    SAME(md("![i][p]\n\n[p]: /p.gif"), "<P><IMG SRC=\"/p.gif\" ALT=\"i\"></P>\n");
    /* held output goes out where it was: before what follows */
    SAME(md("# H\n\nsee [f]\n\n- item\n\n[f]: /f\n\nafter"),
         "<TITLE>H</TITLE>\n<H1>H</H1>\n<P>see <A HREF=\"/f\">f</A></P>\n<UL>\n<LI>item\n\n</UL>\n<P>after</P>\n");
}

/* Held output with no room to wait: it goes out as text, and a definition that comes
   later is listed at the end */
static void test_hold_overflow(void)
{
    static char in[20000];
    const char *o;
    long i, n = sprintf(in, "first [late] link\n\n");
    for (i = 0; i < 400; i++) n += sprintf(in + n, "Paragraph %ld.\n\n", i);
    sprintf(in + n, "[late]: http://late/\n");
    o = md(in);
    HAS(o, "<P>first [late] link</P>");
    HAS(o, "Paragraph 399.");
    HAS(o, "<HR>\n<P><A HREF=\"http://late/\">late</A><BR>\n");
    /* the same, a byte at a time: the same page */
    {
        static char whole[60000];
        strcpy(whole, o);
        CHECK(!strcmp(convert(TX_MARKDOWN, TX_UTF8, in, (long)strlen(in), 1), whole));
    }
    /* more labels waiting than there are slots: the rest are text */
    for (i = n = 0; i < 100; i++) n += sprintf(in + n, "[l%ld] ", i);
    n += sprintf(in + n, "\n\n");
    for (i = 0; i < 100; i++) n += sprintf(in + n, "[l%ld]: /%ld\n", i, i);
    o = md(in);
    HAS(o, "<A HREF=\"/0\">l0</A>");
    HAS(o, "[l99]");
}

static void test_tables(void)
{
    SAME(md("| a | b |\n|---|:-:|\n| 1 | 2 |\n| 3 |\n| 4 | 5 | 6 |\n\nafter"),
         "<TABLE BORDER=1 CELLPADDING=2>\n<TR><TH>a</TH><TH ALIGN=CENTER>b</TH></TR>\n"
         "<TR><TD>1</TD><TD ALIGN=CENTER>2</TD></TR>\n<TR><TD>3</TD><TD></TD></TR>\n"
         "<TR><TD>4</TD><TD ALIGN=CENTER>5</TD></TR>\n</TABLE>\n<P>after</P>\n");
    SAME(md("text\na | b\n--|--\nc | `d\\|e`\n"),
         "<P>text</P>\n<TABLE BORDER=1 CELLPADDING=2>\n<TR><TH>a</TH><TH>b</TH></TR>\n"
         "<TR><TD>c</TD><TD><CODE>d|e</CODE></TD></TR>\n</TABLE>\n");
    SAME(md("| a | b |\n|---|\n"), "<P>| a | b |\n|---|</P>\n");     /* counts differ: not a table */
    SAME(md("| a |\n| - |\n> q"), "<TABLE BORDER=1 CELLPADDING=2>\n<TR><TH>a</TH></TR>\n</TABLE>\n<BLOCKQUOTE>\n<P>q</P>\n</BLOCKQUOTE>\n");
}

/* Pieces of any size make the same page */
static void test_pieces(const char *path, int kind)
{
    static char in[100000], whole[200000];
    FILE *f = fopen(path, "rb");
    long n, k;
    CHECK(f != NULL);
    if (!f) return;
    n = (long)fread(in, 1, sizeof in, f);
    fclose(f);
    strcpy(whole, convert(kind, TX_UTF8, in, n, 0));
    for (k = 1; k < 9; k++) CHECK(!strcmp(convert(kind, TX_UTF8, in, n, k), whole));
    CHECK(!strcmp(convert(kind, TX_UTF8, in, n, 511), whole));
}

static void test_stop(void)
{
    static char in[20000];
    void *t;
    TxSink s = { NULL, h_alloc, h_release, h_write, h_to_mac };
    long i, n = 0;
    for (i = 0; i < 1000; i++) n += sprintf(in + n, "line %ld\n\n", i);
    /* nothing is written in tx_begin: Mill takes a Write during millFormatBegin as the
       end of the run */
    h_len = 0;
    t = tx_begin(TX_GEMTEXT, TX_UTF8, &s);
    CHECK(h_len == 0);
    tx_end(t);
    h_len = 0;
    h_limit = 500;      /* Mill wants no more after 500 bytes */
    t = tx_begin(TX_MARKDOWN, TX_UTF8, &s);
    CHECK(tx_data(t, in, n) == TX_STOP);
    tx_end(t);
    h_limit = -1;
    CHECK(h_len <= 500);
}

static void test_memory(void)
{
    long before = h_mem;
    md("x");
    CHECK(h_mem - before == tx_memory(TX_MARKDOWN));
    before = h_mem;
    gmi("x");
    CHECK(h_mem - before == tx_memory(TX_GEMTEXT));
    printf("memory: Markdown %ld bytes, Gemtext %ld bytes a page\n", tx_memory(TX_MARKDOWN), tx_memory(TX_GEMTEXT));
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests";
    char path[300];
    test_charset();
    test_gemtext();
    test_long_lines();
    test_blocks();
    test_lists();
    test_deep();
    test_inline();
    test_references();
    test_hold_overflow();
    test_tables();
    test_stop();
    test_memory();
    sprintf(path, "%s/all.md", dir);
    test_pieces(path, TX_MARKDOWN);
    sprintf(path, "%s/nasty.md", dir);
    test_pieces(path, TX_MARKDOWN);
    sprintf(path, "%s/all.gmi", dir);
    test_pieces(path, TX_GEMTEXT);
    sprintf(path, "%s/nasty.gmi", dir);
    test_pieces(path, TX_GEMTEXT);
    free(h_out);
    printf(failures ? "%d FAILED\n" : "text: all passed\n", failures);
    return failures != 0;
}
