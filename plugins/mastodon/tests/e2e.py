#!/usr/bin/env python3
"""The Mastodon Plug-In end to end on a host: e2e.c (Plugin.c with host calls on sockets)
against fakemasto.py, through its fake HTTPS proxy and straight over HTTP. Run by e2e.sh."""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fakemasto  # noqa: E402

BIN = sys.argv[1]
failures = 0
checks = 0


def run(method, url, body=None, **env):
    e = {k: v for k, v in os.environ.items() if not k.startswith("MS_")}
    e.update(env)
    args = [BIN, method, url] + ([body] if body is not None else [])
    p = subprocess.run(args, env=e, capture_output=True, timeout=60)
    out = p.stdout.decode("mac_roman")
    if p.returncode != 0:
        print("e2e exited", p.returncode, p.stderr.decode()[-2000:])
    return out, p.stderr.decode("latin-1")


def check(cond, what, out=""):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", what)
        if out:
            print(out[:3000])


def tail(out):
    return out[out.rindex("\n== "):] if "\n== " in out else ""


def mem_ok(out):
    t = tail(out)
    check(" MEM-LEFT 0" in t, "all memory given back: " + t, out)
    return int(t.split("MEM-MAX ")[1].split()[0]) if "MEM-MAX" in t else 0


px = fakemasto.start()
direct = fakemasto.start(instance=None)
chunk = fakemasto.start(chunked=True, rails=True)
wrap = fakemasto.start()
wrap.wrap = True
P = "127.0.0.1:%d" % px.server_address[1]
D = "127.0.0.1:%d" % direct.server_address[1]
via = {"MS_proxy": P}
most = 0

# the local timeline, through the proxy: no token sent, a next page
out, err = run("GET", "mastodon://inst.example/", **via)
check("<TITLE>Local timeline</TITLE>" in out and "HTTP/1.0 200 OK" in out, "local timeline", out)
check("GET https://inst.example/api/v1/timelines/public?local=true&limit=10 HTTP/1.0" in err, "asked the proxy for https", err)
check("Host: inst.example" in err and "User-Agent: Mill/2.1" in err and "Authorization" not in err, "headers", err)
check("Hello from far away" not in out and "Post number 134" in out, "local only", out)
check("?max_id=" in out and "Next page" in out, "next page", out)
check(px.proxied > 0 and " MARKED" in tail(out), "proxied, and marked as the Plug-In's page", out)
most = max(most, mem_ok(out))
nxt = out.split('mastodon://inst.example/?max_id=')[1].split('"')[0]
out, err = run("GET", "mastodon://inst.example/?max_id=" + nxt, **via)
check("max_id=%s" % nxt in err and "Post number" in out, "the next page", out)

# federated, a tag, an account, and a thread
out, _ = run("GET", "mastodon://inst.example/public", **via)
check("Hello from far away" in out or "Post number 135" in out, "federated", out)
out, _ = run("GET", "mastodon://inst.example/tags/retro", **via)
check("Hello from far away" in out and "<TITLE>#retro</TITLE>" in out, "tag", out)
out, err = run("GET", "mastodon://inst.example/@alice", **via, MS_posts="40")
check("lookup?acct=alice" in err and "/accounts/1/statuses?limit=40" in err, "account requests", err)
check("12 followers" in out and "Plus owner. Café regular." in out and "Bob</A>" not in out, "account page", out)
check("Spoilers" not in out and "Boosted by" in out and "<B>Content warning:</B> Myst spoilers" in out, "boost with warning", out)
check("[Picture]</A> A Mac Plus on a desk" in out and "example.com/a/very/long/pa…</A>" in out, "picture and long link", out)
check("Café “Mill” &amp; friends — über click" in out and "alert" not in out, "text made Mac Roman, script gone", out)
most = max(most, mem_ok(out))
out, err = run("GET", "mastodon://inst.example/@alice/104", **via)
a, b, c, d = (out.find(x) for x in ("Is the Plus the best Mac?", "The SE/30 is.", "for the keyboard alone", "Fair enough!"))
check(0 < a < b < c < d, "thread in order: ancestors, the post, replies", out)
check(err.count("/statuses/104/context") == 2 and "<BLOCKQUOTE>" in out[c:], "thread requests, replies indented", out)
check('<A HREF="mastodon://inst.example/@alice">@alice</A>' in out, "mention to a mastodon: page", out)
check("<FORM" not in out, "no forms logged out", out)
most = max(most, mem_ok(out))
out, _ = run("GET", "mastodon://inst.example/@carol/109", **via)
check("(Cut short.)" in out, "long post cut", out)
out, _ = run("GET", "mastodon://inst.example/@carol/106", **via)
check("<B>Content warning:</B> Myst spoilers<P>Spoilers: the age is in the <B>library</B>." in out, "warning open on its page", out)
out, _ = run("GET", "mastodon://inst.example/@nobody", **via)
check("The instance answered 404: Record not found." in out, "no such account", out)
out, _ = run("GET", "mastodon://inst.example/@alice/9999", **via)
check("The instance answered 404: Record not found." in out, "no such post", out)
out, _ = run("GET", "mastodon://inst.example/foo", **via)
check("404: Mastodon has no such address" in out, "bad address", out)

# home: refused without a token; with one, sent, and the actions shown
out, err = run("GET", "mastodon://inst.example/home", **via)
check("The instance answered 401: The access token is invalid." in out and "A login may be required" in out, "home needs a login", out)
tok = {"MS_token": "testtoken", "MS_token_host": "inst.example", "MS_token_acct": "alice"}
out, err = run("GET", "mastodon://inst.example/home", **via, **tok)
check("Authorization: Bearer testtoken" in err and "Post number" in out, "home with a token", out + err)
check("testtoken" not in out, "the token isn't in the page", out)
check('/act"><INPUT TYPE=SUBMIT NAME=do VALUE="Favorite">' in out and "<TEXTAREA NAME=status" in out, "actions and post form", out)
check(">@alice</A>" in out, "who is logged in, in the nav", out)
# a token for another instance isn't sent
out, err = run("GET", "mastodon://inst.example/", **via, MS_token="testtoken", MS_token_host="other.example")
check("Authorization" not in err and "<FORM" not in out, "token kept to its own instance", err)

# posting, favouriting, boosting: only from the Plug-In's own pages
out, err = run("POST", "mastodon://inst.example/post", "status=Hello+%E9t%E9%0D%0Aline+two&spoiler_text=&visibility=unlisted", **via, **tok)
check("FAIL Mastodon: only its own pages can send this." in tail(out), "POST from elsewhere refused", out)
out, err = run("POST", "mastodon://inst.example/post", "status=Hello+%E9t%E9%0D%0Aline+two&spoiler_text=&visibility=unlisted", **via, **tok, FROM="own")
check("<TITLE>Posted</TITLE>" in out and "Hello été<BR>line two" in out, "posted", out)
check(px.bodies and px.bodies[-1] == "status=Hello%20%C3%A9t%C3%A9%0Aline%20two&visibility=unlisted", "the API's body: " + repr(px.bodies[-1:]))
check("| unlisted" in out, "visibility", out)
out, _ = run("POST", "mastodon://inst.example/post", "status=+%0D%0A&visibility=public", **via, **tok, FROM="own")
check("FAIL Mastodon: nothing to post." in tail(out), "empty post refused", out)
out, _ = run("POST", "mastodon://inst.example/post", "status=x&visibility=evil&in_reply_to_id=1%262", **via, **tok, FROM="own")
check(px.bodies[-1] == "status=x", "bad visibility and reply ID left out: " + px.bodies[-1])
out, _ = run("POST", "mastodon://inst.example/post", "status=" + "x" * 5000, **via, **tok, FROM="own")
check("FAIL Mastodon: that’s too long to send." in tail(out), "too long", out)
out, _ = run("POST", "mastodon://inst.example/post", "status=reply&in_reply_to_id=104", **via, **tok, FROM="own")
check(px.bodies[-1] == "status=reply&in_reply_to_id=104" and "In reply to</A> an earlier post" in out, "reply", out)
out, _ = run("POST", "mastodon://inst.example/@alice/100/act", "do=Favorite", **via, **tok, FROM="own")
check("<TITLE>Favorited</TITLE>" in out and 'VALUE="Unfavorite"' in out and "1 favorite" in out, "favourite", out)
out, _ = run("POST", "mastodon://inst.example/@alice/100/act", "do=Boost", **via, **tok, FROM="own")
check("<TITLE>Boosted</TITLE>" in out and 'VALUE="Unboost"' in out, "boost", out)
out, _ = run("POST", "mastodon://inst.example/@alice/100/act", "do=Unboost", **via, **tok, FROM="own")
check('VALUE="Boost"' in out, "unboost", out)
out, _ = run("POST", "mastodon://inst.example/@alice/100/act", "do=Delete", **via, **tok, FROM="own")
check("FAIL Mastodon: the submitted data was invalid." in tail(out), "unknown action", out)
out, _ = run("POST", "mastodon://inst.example/@alice/100/act", "do=Favorite", **via, FROM="own")
check("FAIL Mastodon: log in to this instance first." in tail(out), "action without a token", out)
out, _ = run("GET", "mastodon://inst.example/post", **via)
check("405: That address is for a form" in out, "GET of a form address", out)
out, _ = run("POST", "mastodon://inst.example/", "x=1", **via, FROM="own")
check("405" in out, "POST to a page", out)

# logging in and out
out, _ = run("GET", "mastodon://inst.example/login", **via)
check("TYPE=PASSWORD NAME=token" in out, "login form", out)
out, err = run("POST", "mastodon://inst.example/login", "token=testtoken%0D%0A", **via, FROM="own")
check("Logged in as @alice" in out and "SET token=(token) SET token_host=inst.example SET token_acct=alice" in tail(out), "login", out)
check("testtoken" not in out, "token not shown", out)
out, _ = run("POST", "mastodon://inst.example/login", "token=wrong", **via, FROM="own")
check("401" in out and "Token not accepted" in out and "SET" not in tail(out), "bad token not kept", out)
out, _ = run("POST", "mastodon://inst.example/login", "token=bad%22token", **via, FROM="own")
check("FAIL Mastodon: that isn’t a token." in tail(out), "junk token", out)
out, _ = run("GET", "mastodon://inst.example/login", **via, **tok)
check("Logged in to inst.example as @alice" in out and "Log Out" in out and "testtoken" not in out, "logged-in page", out)
out, _ = run("POST", "mastodon://inst.example/login", "forget=1", **via, **tok, FROM="own")
check("Logged out" in out and "SET token= SET token_host= SET token_acct=" in tail(out), "logout", out)

# straight over HTTP, without the proxy
out, err = run("GET", "mastodon://%s/@alice/104" % D, MS_https="no")
check("GET /api/v1/statuses/104/context HTTP/1.0" in err and "Fair enough!" in out, "plain HTTP", out + err)
check('HREF="mastodon://%s/@alice"' % D in out, "links keep the port", out)
# HTTPS with no proxy set: an alert
out, _ = run("GET", "mastodon://inst.example/")
check("FAIL Mastodon: HTTPS requires an HTTP proxy" in tail(out), "no proxy", out)
# a host that isn't one
out, _ = run("GET", "mastodon://in%0Dst.example/", **via)
check("FAIL Mastodon: that isn\u2019t an instance\u2019s name." in tail(out), "bad host name", out)
# a host the proxy can't reach
out, _ = run("GET", "mastodon://nowhere.example/", **via)
check("502: fake proxy: no such host nowhere.example" in out, "proxy's error", out)
# a proxy login kept for Basic is sent
out, err = run("GET", "mastodon://inst.example/", **via, PROXY_LOGIN="secret")
check("Proxy-Authorization: Basic cGF0OnNlY3JldA==" in err and "saved login asked: proxy 127.0.0.1:%d Basic" % px.server_address[1] in err, "proxy login", err)

# chunked HTTP/1.1, and Rails' < escapes: the same pages
out1, _ = run("GET", "mastodon://inst.example/@alice/104", **via)
out2, _ = run("GET", "mastodon://inst.example/@alice/104", MS_proxy="127.0.0.1:%d" % chunk.server_address[1])
check(out1.split("\n== ")[0] == out2.split("\n== ")[0], "chunked and escaped: the same", out2)
# a proxy that wraps JSON in HTML (MacProxy): said so
out, _ = run("GET", "mastodon://inst.example/", MS_proxy="127.0.0.1:%d" % wrap.server_address[1])
check("FAIL Mastodon: the reply wasn’t Mastodon JSON." in tail(out), "wrapped JSON", out)
# Mill wants no more partway
out, _ = run("GET", "mastodon://inst.example/public", **via, LIMIT="1500")
check(" MEM-LEFT 0" in tail(out), "stopped partway, memory back", out)
out, _ = run("GET", "mastodon://inst.example/@alice/104", **via, LIMIT="2500")
check(" MEM-LEFT 0" in tail(out), "stopped in a thread, memory back", out)
# a closed port
out, _ = run("GET", "mastodon://inst.example/", MS_proxy="127.0.0.1:1")
check("== err -23008" in out, "can't connect: the network's error, for Mill to say", out)

print("e2e: %d of %d checks failed; most memory for a request %d bytes" % (failures, checks, most))
sys.exit(failures != 0)
