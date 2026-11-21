#!/usr/bin/env python3
"""What the Millennium Pack is built from, pinned by version and SHA-256: Mill's source (its
GitHub tag archive), for its Plug-In SDK, the core code the Plug-Ins build by path, its tools
(tools/disk.py, kit.py, installer.py), and its app (for the Plug-Ins' icons); and EUP's source
release, the version that Mill pins, for Install Millennium Pack.

Fetches each once into build/ and checks it, then unpacks Mill into build/mill/ (again
whenever the pin changes), with EUP's tarball in build/mill/build/, where Mill's
tools/installer.py looks for it. build.sh and the tests run it first, on the host; the builds
themselves run without a network."""
import hashlib, os, re, shutil, sys, tarfile, urllib.request

# Mill 2.1.2: the Plug-Ins are built for its interface, and Mill refuses Plug-Ins built for another
MILL_VERSION = '2.1.2'
MILL_URL = 'https://github.com/eric-cgn/mill/archive/refs/tags/v%s.tar.gz' % MILL_VERSION
MILL_SHA256 = '538ace9f32ce621abe4771d3c5191d8e5f2f32914cad599c5c01ae18f1130871'

# EUP 1.4, as Mill 2.1.2's tools/installer.py pins it (checked below)
EUP_VERSION = '1.4'
EUP_URL = 'https://github.com/eric-cgn/eup/releases/download/v%s/eup-%s.tar.gz' % (EUP_VERSION, EUP_VERSION)
EUP_SHA256 = '964b6b05ac0a9d2efbb085998a1ba3ce3aff9bcf89ba67836b877d67733e49bc'

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
build = os.path.join(root, 'build')
mill = os.path.join(build, 'mill')


def sha256(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def fetch(url, digest, path):
    """The file at url, in path, once; checked by SHA-256 every time"""
    if not os.path.exists(path):
        print('deps: fetching %s' % url)
        with urllib.request.urlopen(url) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != digest:
            sys.exit('%s: not the expected file (SHA-256)' % url)
        open(path, 'wb').write(data)
    if sha256(path) != digest:
        sys.exit('%s: not the expected file (SHA-256); delete it to fetch it again' % os.path.relpath(path, root))
    return path


def unpack_mill(tgz):
    """build/mill/, from Mill's tag archive: its files and folders only (no links)"""
    stamp = os.path.join(mill, '.pin')
    if os.path.exists(stamp) and open(stamp).read().strip() == MILL_SHA256:
        return
    shutil.rmtree(mill, ignore_errors=True)
    top = 'mill-%s/' % MILL_VERSION
    with tarfile.open(tgz) as t:
        for m in t.getmembers():
            if m.name.rstrip('/') + '/' == top or not (m.isfile() or m.isdir()):
                continue
            rel = m.name[len(top):] if m.name.startswith(top) else None
            if not rel or rel.startswith('/') or '..' in rel.split('/'):
                sys.exit('%s: unexpected item %s' % (tgz, m.name))
            dst = os.path.join(mill, rel)
            if m.isdir():
                os.makedirs(dst, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with t.extractfile(m) as f, open(dst, 'wb') as o:
                shutil.copyfileobj(f, o)
            if m.mode & 0o111:
                os.chmod(dst, 0o755)
    open(stamp, 'w').write(MILL_SHA256 + '\n')


def check_mill():
    h = open(os.path.join(mill, 'src', 'version.h')).read()
    v = re.search(r'#define MILL_VERSION "([^"]+)"', h).group(1)
    if v != MILL_VERSION:
        sys.exit('build/mill is Mill %s, not %s' % (v, MILL_VERSION))
    inst = open(os.path.join(mill, 'tools', 'installer.py')).read()
    theirs = (re.search(r"EUP_VERSION = '([^']+)'", inst).group(1), re.search(r"EUP_SHA256 = '([^']+)'", inst).group(1))
    if theirs != (EUP_VERSION, EUP_SHA256):
        sys.exit('Mill %s pins EUP %s (%s); tools/deps.py pins %s (%s)' % ((MILL_VERSION,) + theirs + (EUP_VERSION, EUP_SHA256)))


def main():
    os.makedirs(build, exist_ok=True)
    unpack_mill(fetch(MILL_URL, MILL_SHA256, os.path.join(build, 'mill-%s.tar.gz' % MILL_VERSION)))
    check_mill()
    eup = fetch(EUP_URL, EUP_SHA256, os.path.join(build, 'eup-%s.tar.gz' % EUP_VERSION))
    os.makedirs(os.path.join(mill, 'build'), exist_ok=True)
    shutil.copyfile(eup, os.path.join(mill, 'build', os.path.basename(eup)))


if __name__ == '__main__':
    main()
