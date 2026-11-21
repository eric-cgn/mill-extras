#!/usr/bin/env python3
"""Install Millennium Pack and the Mill Extras Disk, from the Plug-Ins that are built
(plugins/*/build/*.bin). build.sh runs it in the Retro68 container, without a network, after it
has run tools/deps.py, built Mill and the Plug-Ins, and Rezzed "Install icon.bin" into the stage.

- build/installer/Install-Millennium-Pack-VERSION.bin and .hqx: Install Millennium Pack, made
  with EUP from installer/extras.ini, without the [component]s whose Plug-In isn't built.
- dist/MillExtras-VERSION.image: a Disk Copy 4.2 image of an 800K HFS disk named
  "Mill Extras Disk" with the same Plug-Ins loose at its top, to drag into Mill Plug-Ins. No
  Read Me, and no installer.

It uses Mill's own tools (tools/disk.py, kit.py, installer.py, from the Mill that tools/deps.py
pins, in build/mill) as they are, and Mill's build (build/mill/build/Mill.bin, for the Plug-Ins'
icons). EUP is Mill's: build/mill/build/eup-VERSION.tar.gz, the version Mill's
tools/installer.py names, checked and built as it does.

EXTRAS_PLACEHOLDER=1 adds test/placeholder, a Plug-In that only tests this path, and marks the
disk and installer TEST. Never ship that. EXTRAS_DEV=abc1234 marks them as a dev build
(VERSION-dev-abc1234)."""
import os, re, shutil, struct, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
sys.path.insert(0, os.path.join(root, 'build', 'mill', 'tools'))
import disk, kit, installer          # Mill's tools, used as they are

VOLUME = 'Mill Extras Disk'
build = os.path.join(root, 'build')
out = os.path.join(build, 'installer')
stage = os.path.join(out, 'stage')
dist = os.path.join(root, 'dist')
TEST = os.environ.get('EXTRAS_PLACEHOLDER') == '1'
DEV = os.environ.get('EXTRAS_DEV', '')
SUFFIX = '-TEST' if TEST else '-dev-' + DEV if DEV else ''    # in the file names
PLACEHOLDER = os.path.join(root, 'test', 'placeholder')

PLACEHOLDER_INI = """
[component placeholder]
group = plugins
label = TEST Placeholder
description = Test only. Not for release.
source = TEST Placeholder.bin
destination = %chosen%:Mill Plug-Ins
types = easy, custom
"""


def sections(text):
    """[(header line or None, [lines])] of an INI file, so components can be left out whole"""
    out = [(None, [])]
    for line in text.splitlines(True):
        if re.match(r'\s*\[', line):
            out.append((line.strip(), []))
        out[-1][1].append(line)
    return out


def keys(lines):
    return {m.group(1).lower(): m.group(2).strip() for m in
            (re.match(r'\s*(\w+)\s*=\s*(.*?)(\s+;.*)?$', l) for l in lines) if m}


def ini_text():
    return open(os.path.join(root, 'installer', 'extras.ini'), encoding='utf-8').read()


def pack_version():
    """The Millennium Pack's version, from installer/extras.ini"""
    for head, lines in sections(ini_text()):
        if head and head.lower() == '[installer]':
            return keys(lines)['version']


def labels():
    """The Plug-Ins' names, from installer/extras.ini's [component]s"""
    return [keys(lines)['label'] for head, lines in sections(ini_text())
            if re.match(r'\[component\s', head or '')]


def built():
    """{folder name: [MacBinary paths]} of the Plug-Ins built in plugins/*/build"""
    found = {}
    top = os.path.join(root, 'plugins')
    dirs = sorted(d for d in os.listdir(top) if os.path.isfile(os.path.join(top, d, 'build.sh')))
    if TEST:
        dirs.append('placeholder')
    for d in dirs:
        b = os.path.join(PLACEHOLDER if d == 'placeholder' else os.path.join(top, d), 'build')
        bins = sorted(os.path.join(b, f) for f in os.listdir(b) if f.endswith('.bin')) if os.path.isdir(b) else []
        found[d] = [p for p in bins if disk.macbinary_fields(p)[:2] in ((b'MPlg', b'MiLL'), (b'FFIL', b'MiLL'))]
        if not found[d]:
            sys.exit('plugins/%s: its build.sh made no Plug-In in build/' % d)
    return found


def make_ini(plugins):
    """The stage's INI: extras.ini with only the built components. Returns (version, [(label,
    description, path)]) of what it installs"""
    text = ini_text()
    if TEST:
        text += PLACEHOLDER_INI
    parts, comps, used = [], [], set()
    version = None
    for head, lines in sections(text):
        m = re.match(r'\[component\s+([\w-]+)\]', head or '')
        k = keys(lines)
        if head and head.lower() == '[installer]':
            version = k['version']
            lines = [l for l in lines if not re.match(r'\s*output\s*=', l)]
            lines.append('output = Install-Millennium-Pack-%s%s\n' % (version, SUFFIX))
            if DEV and not TEST:
                lines = [l for l in lines if not re.match(r'\s*(title|get_info)\s*=', l)]
                lines += ['title = Install Millennium Pack %s-dev-%s\n' % (version, DEV),
                          'get_info = Install Millennium Pack %s-dev-%s, a dev build. Not a release.\n' % (version, DEV)]
            if TEST:
                lines = [l for l in lines if not re.match(r'\s*(file|title|get_info)\s*=', l)]
                lines += ['title = Install Millennium Pack %s TEST\n' % version,
                          'file = Install Millennium Pack TEST\n',
                          'get_info = TEST build of Install Millennium Pack %s. Not for release.\n' % version]
        if m:
            d = m.group(1)
            if d not in plugins:
                print('extras: no %s Plug-In built (plugins/%s): left out' % (k['label'], d))
                continue
            names = {os.path.basename(p): p for p in plugins[d]}
            if k['source'] not in names:
                sys.exit('installer/extras.ini: [component %s] installs %s; plugins/%s/build has %s'
                         % (d, k['source'], d, ', '.join(names)))
            used.add(d)
            comps.append((k['label'], k.get('description', ''), names[k['source']]))
        parts.extend(lines)
    missing = set(plugins) - used
    if missing:
        sys.exit('plugins/%s is built but installer/extras.ini has no [component %s]'
                 % (sorted(missing)[0], sorted(missing)[0]))
    open(os.path.join(stage, 'extras.ini'), 'w', encoding='utf-8').write(''.join(parts))
    return version, comps


def make_installer(comps, version):
    """eup check and build on the stage; the installer's MacBinary path"""
    for _, _, p in comps:
        shutil.copyfile(p, os.path.join(stage, os.path.basename(p)))
    shutil.copyfile(os.path.join(root, 'installer', 'Easy Install.txt'), os.path.join(stage, 'Easy Install.txt'))
    disk.need(os.path.join(stage, 'Install icon.bin'), './build.sh')
    tgz = os.path.join(kit.root, 'build', 'eup-%s.tar.gz' % installer.EUP_VERSION)
    disk.need(tgz, 'tools/deps.py (this build has no network to fetch it)')
    tool = installer.eup()
    ini = os.path.join(stage, 'extras.ini')
    subprocess.run([tool, 'check', ini], check=True)
    subprocess.run([tool, 'build', ini, '-o', out], check=True)
    name = 'Install-Millennium-Pack-%s%s.bin' % (version, SUFFIX)
    made = os.path.join(out, name)
    if not os.path.exists(made):
        bins = [f for f in os.listdir(out) if f.endswith('.bin')]
        sys.exit('eup build made %s, not %s' % (', '.join(bins), name))
    return made


def plugin_desktop(path):
    """The Desktop file a locked disk needs on System 6, for a disk of Plug-Ins and no
    application: Mill's bundle (its ICN#, FREF, and BNDL, from build/mill/build/Mill.bin), so the Plug-Ins
    show Mill's Plug-In and font icons, as tools/disk.py's desktop_file makes it, without the
    'APPL' resource that says where Mill is (it isn't on this disk)"""
    mill = disk.need(os.path.join(kit.root, 'build', 'Mill.bin'), './build.sh')
    disk.desktop_file(path, mill)
    mb = bytearray(open(path, 'rb').read())
    rlen = struct.unpack('>I', mb[87:91])[0]
    res = disk.read_resources(bytes(mb[128:128 + rlen]))
    order = [k for k in res if k[0] != b'APPL']
    fork = disk.resource_fork([(t, i, res[(t, i)]) for t, i in order])
    mb[87:91] = struct.pack('>I', len(fork))
    mb[124:126] = disk.crc16(mb[:124]).to_bytes(2, 'big')
    open(path, 'wb').write(bytes(mb[:128]) + fork + bytes(-len(fork) % 128))


def places(names):
    """Where the Finder shows the disk's icons: the Plug-Ins, four to a row; and the window,
    sized to them"""
    at = {}
    for i, n in enumerate(names):
        at[n] = (12 + 66 * (i // 4), 24 + 80 * (i % 4))
    rows = (len(at) + 3) // 4
    cols = min(len(at), 4)
    return at, (70, 40, 70 + 70 + 66 * rows, 40 + max(270, 40 + 80 * cols))


def part_number():
    """The Mill Extras Disk's part number, #1E6-DM03-V212 for Mill 2.1.2: Mill's scheme
    (tools/disk.py), which follows the version of the Mill it is for"""
    return disk.part_number(kit.version(), 'DM03')


def make_disk(comps, version):
    raw = os.path.join(build, 'MillExtras-disk.dsk')
    image = os.path.join(dist, 'MillExtras-%s%s.image' % (version, SUFFIX))
    os.makedirs(dist, exist_ok=True)
    tmp = tempfile.mkdtemp()
    items, names = [], []
    with open(raw, 'wb') as f:
        f.write(bytes(disk.CAPACITY))
    kit.hfs('hformat', '-l', VOLUME, raw)
    try:
        for label, _, p in comps:
            t, c, dl, rl = disk.macbinary_fields(p)
            kit.hfs('hcopy', '-m', p, ':')
            mb = open(p, 'rb').read(64)
            names.append(mb[2:2 + mb[1]].decode('mac_roman'))
            items.append((names[-1], dl + rl))
        icon = os.path.join(tmp, 'Icon.bin')
        disk.icon_file(icon)
        kit.hfs('hcopy', '-m', icon, ':')
        dt = os.path.join(tmp, 'Desktop.bin')
        plugin_desktop(dt)
        kit.hfs('hcopy', '-m', dt, ':')
        free = disk.free_bytes()
    except subprocess.CalledProcessError as e:
        sys.exit('hfsutils failed (%s): the disk is probably full' % ' '.join(e.cmd))
    finally:
        kit.hfs('humount')
        shutil.rmtree(tmp)
    disk.custom_icon_flag(raw)
    # the period tool's placing, with this disk's names
    disk.PLACES, disk.WINDOW = places(names)
    disk.place_icons(raw)
    open(image, 'wb').write(disk.disk_copy(open(raw, 'rb').read(), VOLUME))
    print('%s: %s, for Mill %s' % (os.path.relpath(image, root), part_number(), kit.version()))
    print('%-42s %9s' % ('item', 'bytes'))
    for n, b in items:
        print('%-42s %9d' % (n, b))
    print('%-42s %9d of %d' % ('free', free, disk.CAPACITY))


def main():
    shutil.rmtree(dist, ignore_errors=True)
    for f in os.listdir(out) if os.path.isdir(out) else []:
        if f != 'stage':
            os.remove(os.path.join(out, f))
    for f in os.listdir(stage) if os.path.isdir(stage) else []:      # all but build.sh's icon
        if f != 'Install icon.bin':
            os.remove(os.path.join(stage, f))
    plugins = built()
    if not plugins:
        print('extras: no Plug-Ins in plugins/ yet: no disk or installer to make')
        return
    os.makedirs(stage, exist_ok=True)
    version, comps = make_ini(plugins)
    if not comps:
        print('extras: none of the built Plug-Ins is in installer/extras.ini: no disk or installer')
        return
    app = make_installer(comps, version)
    print(os.path.relpath(app, root))
    make_disk(comps, version)


if __name__ == '__main__':
    main()
