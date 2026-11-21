# Millennium Pack

Plug-Ins for [Mill](https://github.com/eric-cgn/mill), a 68000 web browser for classic Macs,
that are out of period: formats and services from after Christmas Day, 1997, which Mill is set
on (Markdown, Gemtext, Mastodon, MQTT, and the like). Older formats belong in Mill or its own
Plug-Ins, not here. Mill doesn't depend on anything here.

The Pack ships as Install Millennium Pack (a download, as MacBinary and BinHex), and as the Mill
Extras Disk, with its Plug-Ins loose, to drag into Mill Plug-Ins.

## The Plug-Ins

| Folder | Plug-In | What it does |
|---|---|---|
| `text` | Modern Text | Markdown and Gemtext |
| `mastodon` | Mastodon | Mastodon timelines, accounts, and threads (`mastodon://`), through the HTTP proxy |
| `mqtt` | MQTT | MQTT topics as pages, and publishing |

Each has a `Read Me` in its folder.

## Which Mill

Mill loads only Plug-Ins built for its own Plug-In interface (`kMillInterface`, in its
`sdk/Interfaces/MillInterface.h`), and lists the others as "made for another version of Mill".
So the Pack is built against one Mill release, pinned in `tools/deps.py`: now Mill 2.1.2
(interface 4). A Mill with a new interface needs a new Pack, built against it.

| Release file | What it is |
|---|---|
| `Install-Millennium-Pack-VERSION.bin`, `.hqx` | the installer. It puts the Plug-Ins in the Mill Plug-Ins folder of an installed Mill; with no Mill there, it installs nothing. |
| `MillExtras-VERSION.image` | the Mill Extras Disk, #1E6-DM03-V212 (for Mill 2.1.2): an 800K Disk Copy 4.2 image, the Plug-Ins loose at its top |

The part number follows Mill's scheme (Mill's `tools/disk.py`): DM03 is the Extras Disk, and
the last part is the version of the Mill it is for. VERSION is the Pack's own, in
`installer/extras.ini`.

## Building

Needs Docker, Python 3, and the network once (for `tools/deps.py`).

    ./build.sh               the Plug-Ins, the installer, and the disk
    ./test.sh                every host test
    python3 tools/release.py the release files, in dist/

`tools/deps.py` fetches what the Pack is built from into `build/` and checks each by SHA-256:

- Mill 2.1.2's source, its GitHub tag archive, unpacked into `build/mill/`. The Plug-Ins use its
  SDK (`sdk/Interfaces`, `sdk/Glue/MillGlue.c`) and some of its core code by path
  (`src/core/http11.c`, `charset.c`, `fmt.c`); the installer and disk use its tools
  (`tools/disk.py`, `kit.py`, `installer.py`) as they are, its installer icon, and Mill itself,
  built once, for the Plug-Ins' icons on the disk.
- EUP 1.4's source release, the version Mill 2.1.2 pins (it checks that they agree), which makes
  Install Millennium Pack.

Everything else runs in the pinned Retro68 image
(`ghcr.io/autc04/retro68@sha256:dd92396910…`, as Mill), with no network:

- `plugins/*/build.sh`: `plugins/<dir>/build/<Name>.bin`;
- `tools/extras.py`: `build/installer/Install-Millennium-Pack-VERSION.bin` and `.hqx`, made with
  EUP from `installer/extras.ini`, and `dist/MillExtras-VERSION.image`.

To build for another Mill, change the pins in `tools/deps.py` (the version and SHA-256 of its
tag archive, and EUP's, as that Mill's `tools/installer.py` has them), and the "Requires" line
in `installer/Easy Install.txt`.

`EXTRAS_PLACEHOLDER=1 ./build.sh` adds `test/placeholder`, a Plug-In that does nothing but answer
`placeholder://`, and marks the disk and installer TEST, for testing the disk and installer
alone. Never ship that build; a plain `./build.sh` removes it.

## Layout

    build.sh              builds the Plug-Ins, then the installer and the disk
    test.sh               runs every host test
    installer/
      extras.ini          Install Millennium Pack, for EUP
      Easy Install.txt    the installer's Easy Install text
    plugins/<dir>/        one Plug-In each
    test/placeholder/     a Plug-In for testing the disk and installer only; never shipped
    tools/deps.py         the pinned Mill and EUP
    tools/extras.py       makes the installer and the Extras Disk
    tools/release.py      gathers the release files in dist/; `notes` prints the notes
    licences/             third-party notices for the built programs

## A Plug-In

Each Plug-In is self-contained in `plugins/<dir>/` and built like Mill's own (see Mill's
`plugins/images/`):

- `build.sh`: builds `build/<Name>.bin`, a MacBinary file of type `MPlg` and creator `MiLL`
  (`FFIL` for a font), in the pinned Retro68 image, with `docker run --network none`.
- `Plugin.c` and whatever else it needs.
- `<Name>.r`: the code as `MPlg` 128, `MillPlugIn.r` for `MIfc` 128, the About text as
  `TEXT` 128, and the license as `TEXT` 129.
- `Read Me` (the repo's only; no disk carries one) and `about.txt`, plain and short.
- `tests/run.sh`: its host tests, which `./test.sh` runs.

It uses Mill's SDK and may build Mill's core code by path, from `build/mill/`. It never needs a
change in Mill to suit itself: if it needs one, the change is made in Mill on its own terms, and
the Pack moves to the Mill release that has it.

Third-party code must be MIT-compatible and carry its license text in the file and in the
Plug-In's folder.

To add a Plug-In to the Extras Disk and the installer, give it a `[component]` in
`installer/extras.ini` whose ID is its folder's name and whose `source` is its `.bin`.

## Releases

`main` has the releases and `dev` the work in progress. Changes come into `dev` by pull request;
a pull request from `dev` into `main` is a release.

- `.github/workflows/ci.yml`: every pull request, and every push to `main`: the host tests, the
  build, and the release files, kept as an artifact. A pull request into `main` fails unless it
  comes from `dev`.
- `.github/workflows/dev.yml`: every push to `dev`: the same, published as the "dev"
  pre-release (files named `1.0-dev-abc1234`), replaced each time.
- `.github/workflows/release.yml`: every push to `main`: if `installer/extras.ini` names a
  version with no release yet, the release `vVERSION`, "Millennium Pack VERSION", with the
  three files.

## License

MIT (`LICENSE`). The built Plug-Ins and installer contain parts of newlib, Retro68's runtime,
and GCC's libgcc; see `NOTICE` and `licences/`.
