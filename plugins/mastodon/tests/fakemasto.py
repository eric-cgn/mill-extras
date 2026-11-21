#!/usr/bin/env python3
"""A tiny fake Mastodon instance for the Mastodon Plug-In's tests: the client API's read
endpoints, logins by token, and posting, favouriting, and boosting, all in memory. Python's
standard library only; nothing is fetched.

It also acts as an HTTP proxy that does "TLS" for the instance: a request line with a
whole address ("GET https://inst.example/api/v1/... HTTP/1.0") is answered as the instance
would, as Mill's HTTP proxy does for an https: address. So one server tests both of the
Plug-In's ways in.

    fakemasto.py [--port N] [--chunked] [--rails] [--log FILE]

--chunked answers HTTP/1.1 with chunked bodies; --rails escapes < > & in JSON strings as
\\u003c and so on, as Mastodon's own (Rails) JSON does. The access token is "testtoken". """
import argparse
import json
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler
from urllib.parse import parse_qs, urlsplit

TOKEN = "testtoken"
HOST = "inst.example"

ACCOUNTS = {
    "1": {"id": "1", "username": "alice", "acct": "alice", "display_name": "Alice \U0001F4BE",
          "note": "<p>Plus owner. Café regular.</p>", "url": "https://inst.example/@alice",
          "followers_count": 12, "following_count": 3, "statuses_count": 0},
    "2": {"id": "2", "username": "bob", "acct": "bob@other.example", "display_name": "Bob",
          "note": "", "url": "https://other.example/@bob", "followers_count": 1,
          "following_count": 1, "statuses_count": 0},
    "3": {"id": "3", "username": "carol", "acct": "carol", "display_name": "",
          "note": "<p>I write long posts.</p>", "url": "https://inst.example/@carol",
          "followers_count": 0, "following_count": 0, "statuses_count": 0},
}
ME = "1"        # whose the token is

STATUSES = {}   # id -> status
ORDER = []      # ids, newest first
LOCK = threading.Lock()
NEXT_ID = [1000]


def account(aid):
    a = dict(ACCOUNTS[aid])
    a.update({"locked": False, "bot": False, "emojis": [], "fields": [],
              "avatar": "https://inst.example/avatar.png", "created_at": "2026-01-01T00:00:00.000Z"})
    return a


def make(sid, aid, content, when, reply_to=None, spoiler="", visibility="public", media=None, reblog=None,
         local=True):
    s = {
        "id": sid, "created_at": when, "in_reply_to_id": reply_to,
        "in_reply_to_account_id": STATUSES[reply_to]["account"]["id"] if reply_to in STATUSES else None,
        "sensitive": bool(spoiler), "spoiler_text": spoiler, "visibility": visibility, "language": "en",
        "uri": "https://inst.example/users/x/statuses/" + sid, "url": "https://inst.example/@x/" + sid,
        "replies_count": 0, "reblogs_count": 0, "favourites_count": 0, "favourited": False,
        "reblogged": False, "content": content, "reblog": reblog, "account": account(aid),
        "media_attachments": media or [], "mentions": [], "tags": [], "emojis": [], "card": None,
        "poll": None, "_local": local,
    }
    STATUSES[sid] = s
    ORDER.insert(0, sid)
    ACCOUNTS[aid]["statuses_count"] += 1
    if reply_to in STATUSES:
        STATUSES[reply_to]["replies_count"] += 1
    return s


def seed():
    t = 0

    def when():
        nonlocal t
        t += 1
        return "2026-11-%02dT%02d:%02d:00.000Z" % (10 + t // 24, t % 24, (t * 7) % 60)
    make("100", "1", "<p>Booted my Mac Plus today. It still works!</p>", when())
    make("101", "2", '<p>Hello from far away. <a href="https://inst.example/tags/retro" class="mention hashtag" '
         'rel="tag">#<span>retro</span></a></p>', when(), local=False)
    make("102", "1", '<p>Is the Plus the best Mac?</p>', when())
    make("103", "2", '<p><span class="h-card"><a href="https://inst.example/@alice" class="u-url mention">'
         '@<span>alice</span></a></span> The SE/30 is.</p>', when(), reply_to="102", local=False)
    make("104", "3", "<p>The Plus, for the keyboard alone.</p>", when(), reply_to="103")
    make("105", "1", "<p>Fair enough!</p>", when(), reply_to="104")
    make("106", "3", "<p>Spoilers: the age is in the <b>library</b>.</p>", when(), spoiler="Myst spoilers")
    make("107", "1", '<p>My desk: <a href="https://www.example.com/a/very/long/path/to/a/page" rel="nofollow">'
         '<span class="invisible">https://www.</span><span class="ellipsis">example.com/a/very/long/pa</span>'
         '<span class="invisible">th/to/a/page</span></a></p>', when(),
         media=[{"id": "9", "type": "image", "url": "https://inst.example/media/desk.jpg",
                 "preview_url": "https://inst.example/media/desk-small.jpg", "description": "A Mac Plus on a desk"}])
    make("108", "1", "", when(), reblog=dict(STATUSES["106"]))
    # carol's long post: more than the Plug-In shows
    make("109", "3", "<p>" + "Long post. " * 2000 + "</p>", when())
    make("110", "1", "<p>Café “Mill” &amp; friends — über <script>alert(1)</script>"
         '<a href="javascript:alert(2)">click</a></p>', when(), visibility="unlisted")
    for i in range(111, 136):
        make(str(i), "2" if i % 3 == 0 else "1", "<p>Post number %d. #filler</p>" % i, when(), local=i % 3 != 0)


def public(st):
    return {k: v for k, v in st.items() if not k.startswith("_")}


def page(ids, q):
    limit = min(int(q.get("limit", ["20"])[0]), 40)
    max_id = q.get("max_id", [None])[0]
    out = []
    for sid in ids:
        if max_id and int(sid) >= int(max_id):
            continue
        out.append(public(STATUSES[sid]))
        if len(out) >= limit:
            break
    return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    server_version = "fakemasto/1.0"

    def log_message(self, fmt, *args):
        if self.server.log:
            self.server.log.write("%s\n" % (fmt % args))
            self.server.log.flush()

    def send_json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False, indent=1)
        if self.server.rails:
            body = body.replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
        data = body.encode("utf-8")
        if self.server.wrap:
            # as MacProxy does to everything: wrapped as an HTML page
            data = b"<html><head></head><body>" + data + b"</body></html>"
        if self.server.chunked:
            self.protocol_version = "HTTP/1.1"
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        if self.server.chunked and self.request_version == "HTTP/1.1":
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            for i in range(0, len(data), 1000):
                part = data[i:i + 1000]
                self.wfile.write(b"%x;ext=1\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\nX-Trailer: 1\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        self.close_connection = True

    def authed(self):
        return self.headers.get("Authorization", "") == "Bearer " + TOKEN

    def route(self, method):
        url = urlsplit(self.path)
        if url.scheme:
            # a proxy's request for https://HOST/...: answered as the instance would be
            if url.scheme != "https" or url.netloc != self.server.instance:
                return self.send_json(502, {"error": "fake proxy: no such host " + url.netloc})
            self.server.proxied += 1
        p, q = url.path, parse_qs(url.query)
        if self.server.instance and self.headers.get("Host") != self.server.instance:
            return self.send_json(400, {"error": "wrong Host"})
        self.server.auths.append(self.headers.get("Authorization", ""))
        body = {}
        if method == "POST":
            n = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(n).decode("utf-8") if n else ""
            if self.headers.get("Content-Type", "").startswith("application/x-www-form-urlencoded"):
                body = {k: v[0] for k, v in parse_qs(raw, keep_blank_values=True).items()}
            self.server.bodies.append(raw)
        with LOCK:
            return self.answer(method, p, q, body)

    def answer(self, method, p, q, body):
        parts = p.strip("/").split("/")
        if method == "GET":
            if p == "/api/v1/timelines/public":
                local = q.get("local", ["false"])[0] == "true"
                ids = [s for s in ORDER if STATUSES[s]["_local"] or not local]
                return self.send_json(200, page(ids, q))
            if p == "/api/v1/timelines/home":
                if not self.authed():
                    return self.send_json(401, {"error": "The access token is invalid"})
                return self.send_json(200, page(ORDER, q))
            if p.startswith("/api/v1/timelines/tag/"):
                tag = parts[-1].lower()
                ids = [s for s in ORDER if "#" + tag in STATUSES[s]["content"].lower()
                       or ">" + tag + "<" in STATUSES[s]["content"].lower()]
                return self.send_json(200, page(ids, q))
            if p == "/api/v1/accounts/lookup":
                acct = q.get("acct", [""])[0]
                for a in ACCOUNTS:
                    if ACCOUNTS[a]["acct"] == acct or ACCOUNTS[a]["acct"] == acct.split("@")[0] and acct.endswith("@" + HOST):
                        return self.send_json(200, account(a))
                return self.send_json(404, {"error": "Record not found"})
            if p == "/api/v1/accounts/verify_credentials":
                if not self.authed():
                    return self.send_json(401, {"error": "The access token is invalid"})
                return self.send_json(200, account(ME))
            if len(parts) == 5 and parts[2] == "accounts" and parts[4] == "statuses" and parts[3] in ACCOUNTS:
                ids = [s for s in ORDER if STATUSES[s]["account"]["id"] == parts[3]]
                return self.send_json(200, page(ids, q))
            if len(parts) in (4, 5) and parts[2] == "statuses":
                st = STATUSES.get(parts[3])
                if not st:
                    return self.send_json(404, {"error": "Record not found"})
                if len(parts) == 4:
                    return self.send_json(200, public(st))
                if parts[4] == "context":
                    anc, s = [], st
                    while s["in_reply_to_id"] in STATUSES:
                        s = STATUSES[s["in_reply_to_id"]]
                        anc.insert(0, public(s))
                    desc, todo = [], [st["id"]]
                    while todo:
                        cur = todo.pop(0)
                        kids = sorted(x for x in STATUSES if STATUSES[x]["in_reply_to_id"] == cur)
                        desc += [public(STATUSES[k]) for k in kids]
                        todo += kids
                    return self.send_json(200, {"ancestors": anc, "descendants": desc})
        if method == "POST":
            if not self.authed():
                return self.send_json(401, {"error": "The access token is invalid"})
            if p == "/api/v1/statuses":
                text = body.get("status", "")
                if not text.strip():
                    return self.send_json(422, {"error": "Validation failed: Text can't be blank"})
                NEXT_ID[0] += 1
                html = "<p>" + text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\n", "<br>") + "</p>"
                st = make(str(NEXT_ID[0]), ME, html, time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime()),
                          reply_to=body.get("in_reply_to_id"), spoiler=body.get("spoiler_text", ""),
                          visibility=body.get("visibility", "public"))
                return self.send_json(200, public(st))
            if len(parts) == 5 and parts[2] == "statuses" and parts[3] in STATUSES:
                st = STATUSES[parts[3]]
                act = parts[4]
                if act in ("favourite", "unfavourite"):
                    on = act == "favourite"
                    if st["favourited"] != on:
                        st["favourites_count"] += 1 if on else -1
                    st["favourited"] = on
                elif act in ("reblog", "unreblog"):
                    on = act == "reblog"
                    if st["reblogged"] != on:
                        st["reblogs_count"] += 1 if on else -1
                    st["reblogged"] = on
                else:
                    return self.send_json(404, {"error": "Not found"})
                return self.send_json(200, public(st))
        return self.send_json(404, {"error": "Not found"})

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


def start(port=0, chunked=False, rails=False, log=None, instance=HOST):
    """The server, running in a thread; its port is server.server_address[1]"""
    if not STATUSES:
        seed()
    s = Server(("127.0.0.1", port), Handler)
    s.chunked, s.rails, s.log, s.instance, s.wrap = chunked, rails, log, instance, False
    s.proxied, s.auths, s.bodies = 0, [], []
    threading.Thread(target=s.serve_forever, daemon=True).start()
    return s


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--instance", default=HOST)
    ap.add_argument("--chunked", action="store_true")
    ap.add_argument("--rails", action="store_true")
    args = ap.parse_args()
    seed()
    srv = Server((args.bind, args.port), Handler)
    srv.chunked, srv.rails, srv.log, srv.instance, srv.wrap = args.chunked, args.rails, sys.stderr, args.instance, False
    srv.proxied, srv.auths, srv.bodies = 0, [], []
    print("fakemasto on %s:%d as %s" % (args.bind, args.port, args.instance), flush=True)
    srv.serve_forever()
