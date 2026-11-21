#!/usr/bin/env python3
'''The Millennium Pack's release files, in dist/: Install-Millennium-Pack-VERSION.bin (MacBinary)
and .hqx (BinHex 4.0), copied from build/installer, beside the disk image ./build.sh makes there,
MillExtras-VERSION.image (the Mill Extras Disk). Checks that all three are there and that none
is a TEST build. Run after ./build.sh, with the same EXTRAS_DEV, if any.
python3 tools/release.py notes     prints the release notes'''
import os, shutil, sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import extras as pack


def files():
    v = pack.pack_version()
    s = pack.SUFFIX
    return v, ['Install-Millennium-Pack-%s%s.bin' % (v, s), 'Install-Millennium-Pack-%s%s.hqx' % (v, s),
               'MillExtras-%s%s.image' % (v, s)]


def notes():
    v, _ = files()
    m = pack.kit.version()
    n = pack.labels()
    what = ', '.join(n[:-1]) + (',' if len(n) > 2 else '') + ' and ' + n[-1] if len(n) > 1 else n[0]
    return '\n'.join([
        'Millennium Pack %s: out-of-period Plug-Ins for Mill %s: %s.' % (v, m, what),
        '',
        '- Install-Millennium-Pack-%s.bin and .hqx: the installer, as MacBinary and BinHex. It puts the' % v,
        '  Plug-Ins in the Mill Plug-Ins folder of an installed Mill.',
        '- MillExtras-%s.image: the Mill Extras Disk (%s), an 800K Disk Copy 4.2 image with the' % (v, pack.part_number()),
        '  Plug-Ins loose, to drag into Mill Plug-Ins.'])


def main():
    v, names = files()
    os.makedirs(pack.dist, exist_ok=True)
    for n in names[:2]:
        p = os.path.join(pack.out, n)
        if not os.path.exists(p):
            sys.exit('%s is missing: ./build.sh' % os.path.relpath(p, pack.root))
        shutil.copyfile(p, os.path.join(pack.dist, n))
    for n in names:
        p = os.path.join(pack.dist, n)
        if not os.path.exists(p):
            sys.exit('%s is missing: ./build.sh' % p)
        print('%s: %d bytes' % (os.path.relpath(p, pack.root), os.path.getsize(p)))
    test = [n for n in os.listdir(pack.dist) if 'TEST' in n]
    if test:
        sys.exit('dist/ has TEST builds (%s): run ./build.sh without EXTRAS_PLACEHOLDER' % ', '.join(test))


if __name__ == '__main__':
    if sys.argv[1:2] == ['notes']:
        print(notes())
    else:
        main()
