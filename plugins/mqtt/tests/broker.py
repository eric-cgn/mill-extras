#!/usr/bin/env python3
"""A tiny MQTT 3.1.1 broker for testing the MQTT Plug-In: retained messages, + and #
wildcards (and 4.7.2's $ rule), PUBLISH in and out at QoS 0, 1, and 2, SUBSCRIBE and
UNSUBSCRIBE, PINGREQ, keep alive, and logins. Standard library only. Not for real use.

    broker.py [--port 1883] [--login-port 1884 --user NAME --password PW]
              [--seed FILE] [--log]

--port takes anyone. --login-port takes only NAME and PW (CONNACK 4 for a wrong one,
5 for none). --seed is a file of retained messages, one to a line, "topic<TAB>payload",
where the payload may have \\n, \\t, and \\xHH; a line "topic<TAB>@N" retains N bytes of
binary, and a "#" line is a comment. Each publish is logged with --log."""
import argparse, asyncio, itertools, re, sys

retained = {}       # topic -> (payload, qos)
clients = set()


def match(flt, topic):
    if flt[:1] in ('+', '#') and topic.startswith('$'):
        return False
    f, t = flt.split('/'), topic.split('/')
    for i, level in enumerate(f):
        if level == '#':
            return True
        if i >= len(t):
            return False
        if level != '+' and level != t[i]:
            return False
    return len(f) == len(t)


def filter_ok(f):
    if not f:
        return False
    levels = f.split('/')
    for i, level in enumerate(levels):
        if ('+' in level and level != '+') or ('#' in level and (level != '#' or i != len(levels) - 1)):
            return False
    return True


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def packet(first, body=b''):
    return bytes([first]) + varint(len(body)) + body


def s16(b):
    return len(b).to_bytes(2, 'big') + b


class Client:
    def __init__(self, reader, writer, login):
        self.r, self.w, self.login = reader, writer, login
        self.subs = {}      # filter -> qos
        self.ids = itertools.count(1)
        self.inflight = {}  # QoS 2 ids received, awaiting PUBREL
        self.keep = 0
        self.name = '?'

    def send(self, data):
        self.w.write(data)

    def deliver(self, topic, payload, qos, retain):
        qs = [q for f, q in self.subs.items() if match(f, topic)]
        if qs:
            self.deliver_at(topic, payload, min(qos, max(qs)), retain)

    def deliver_at(self, topic, payload, qos, retain):
        first = 0x30 | qos << 1 | (1 if retain else 0)
        body = s16(topic.encode())
        if qos:
            pid = next(self.ids) % 65535 + 1
            body += pid.to_bytes(2, 'big')
        self.send(packet(first, body + payload))

    async def read_packet(self):
        first = (await self.r.readexactly(1))[0]
        n, mult = 0, 1
        for _ in range(4):
            b = (await self.r.readexactly(1))[0]
            n += (b & 0x7F) * mult
            mult <<= 7
            if not b & 0x80:
                break
        else:
            raise ValueError('bad remaining length')
        return first, await self.r.readexactly(n) if n else b''

    async def run(self):
        first, body = await asyncio.wait_for(self.read_packet(), 10)
        if first != 0x10 or body[:6] != b'\x00\x04MQTT' or body[6] != 4:
            if first == 0x10 and body[:6] == b'\x00\x04MQTT':
                self.send(packet(0x20, b'\x00\x01'))
            return
        flags = body[7]
        self.keep = int.from_bytes(body[8:10], 'big')
        at = 10

        def take():
            nonlocal at
            n = int.from_bytes(body[at:at + 2], 'big')
            v = body[at + 2:at + 2 + n]
            at += 2 + n
            return v
        cid = take().decode()
        if flags & 0x04:
            take(); take()          # a will: taken, never sent
        user = take().decode() if flags & 0x80 else None
        pw = take().decode() if flags & 0x40 else None
        self.name = cid
        if self.login:
            if user is None:
                self.send(packet(0x20, b'\x00\x05'))
                return
            if (user, pw) != self.login:
                self.send(packet(0x20, b'\x00\x04'))
                return
        self.send(packet(0x20, b'\x00\x00'))
        clients.add(self)
        try:
            while True:
                limit = self.keep * 1.5 if self.keep else None
                first, body = await asyncio.wait_for(self.read_packet(), limit)
                kind = first >> 4
                if kind == 3:
                    await self.publish(first, body)
                elif kind == 6:     # PUBREL
                    pid = body[:2]
                    self.inflight.pop(pid, None)
                    self.send(packet(0x70, pid))
                elif kind in (4, 5, 7):
                    if kind == 5:   # PUBREC from the client: PUBREL
                        self.send(packet(0x62, body[:2]))
                elif kind == 8:     # SUBSCRIBE
                    pid, at, codes, new = body[:2], 2, bytearray(), []
                    while at < len(body):
                        n = int.from_bytes(body[at:at + 2], 'big')
                        flt = body[at + 2:at + 2 + n].decode()
                        q = body[at + 2 + n]
                        at += 3 + n
                        if not filter_ok(flt) or flt.startswith('denied/'):
                            codes.append(0x80)
                            continue
                        q = min(q, 2)
                        self.subs[flt] = q
                        codes.append(q)
                        new.append((flt, q))
                    self.send(packet(0x90, pid + bytes(codes)))
                    for flt, q in new:      # 3.3.1.3: retained, after the SUBACK
                        for t, (p, pq) in sorted(retained.items()):
                            if match(flt, t):
                                self.deliver_at(t, p, min(q, pq), True)
                elif kind == 10:    # UNSUBSCRIBE
                    pid, at = body[:2], 2
                    while at < len(body):
                        n = int.from_bytes(body[at:at + 2], 'big')
                        self.subs.pop(body[at + 2:at + 2 + n].decode(), None)
                        at += 2 + n
                    self.send(packet(0xB0, pid))
                elif kind == 12:
                    self.send(packet(0xD0))
                elif kind == 14:
                    return
                else:
                    return
                await self.w.drain()
        finally:
            clients.discard(self)

    async def publish(self, first, body):
        qos, retain = first >> 1 & 3, first & 1
        n = int.from_bytes(body[:2], 'big')
        topic = body[2:2 + n].decode()
        at = 2 + n
        pid = b''
        if qos:
            pid = body[at:at + 2]
            at += 2
        payload = body[at:]
        if args.log:
            print(f'publish {topic!r} {payload[:60]!r} qos={qos} retain={retain} from {self.name}', flush=True)
        if qos == 1:
            self.send(packet(0x40, pid))
        elif qos == 2:
            if pid in self.inflight:
                self.send(packet(0x50, pid))
                return
            self.inflight[pid] = True
            self.send(packet(0x50, pid))
        if retain:
            if payload:
                retained[topic] = (payload, qos)
            else:
                retained.pop(topic, None)
        for c in list(clients):
            c.deliver(topic, payload, qos, False)


def unescape(s):
    return re.sub(rb'\\(n|t|\\|x[0-9a-fA-F]{2})',
                  lambda m: {b'n': b'\n', b't': b'\t', b'\\': b'\\'}.get(m.group(1)) or bytes([int(m.group(1)[1:], 16)]),
                  s.encode())


def seed(path):
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        topic, _, payload = line.partition('\t')
        qos = 0
        m = re.match(r'(.*) q([012])$', topic)
        if m:
            topic, qos = m.group(1), int(m.group(2))
        if payload.startswith('@'):
            retained[topic] = (bytes(range(256)) * (int(payload[1:]) // 256 + 1))[:int(payload[1:])], qos
        else:
            retained[topic] = (unescape(payload), qos)


async def main():
    servers = []

    async def anyone(r, w):
        await serve(r, w, None)

    async def logins(r, w):
        await serve(r, w, (args.user, args.password))

    async def serve(r, w, login):
        try:
            await Client(r, w, login).run()
        except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError, ValueError):
            pass
        finally:
            try:
                w.close()
            except Exception:
                pass

    servers.append(await asyncio.start_server(anyone, args.host, args.port))
    if args.login_port:
        servers.append(await asyncio.start_server(logins, args.host, args.login_port))
    print(f'broker: listening on {args.port}' + (f' and {args.login_port} (login)' if args.login_port else ''),
          f'{len(retained)} retained', flush=True)
    await asyncio.gather(*(s.serve_forever() for s in servers))


ap = argparse.ArgumentParser()
ap.add_argument('--host', default='0.0.0.0')
ap.add_argument('--port', type=int, default=1883)
ap.add_argument('--login-port', type=int)
ap.add_argument('--user', default='tester')
ap.add_argument('--password', default='secret')
ap.add_argument('--seed')
ap.add_argument('--log', action='store_true')
args = ap.parse_args()
if args.seed:
    seed(args.seed)
try:
    asyncio.run(main())
except KeyboardInterrupt:
    sys.exit(0)
