#!/bin/sh
# Build Mastodon with Retro68 (the same pinned image as Mill): build/Mastodon.bin, a
# MacBinary Plug-In file for the "Mill Plug-Ins" folder. json.c, content.c, and masto.c are
# plain C, tested on a host by tests/run.sh; HTTP replies are read with Mill's own
# src/core/http11.c, built here by path. The check at the end shows nothing but the
# host's calls is linked (no malloc, no printf).
set -e
cd "$(dirname "$0")"
mkdir -p build
python3 ../../tools/deps.py      # Mill 2.1.2's SDK and core code, in ../../build/mill
tr '\n' '\r' < ../../LICENSE > build/License.txt     # a Mac TEXT resource has CR line ends
tr '\n' '\r' < about.txt > build/About.txt
cp Mastodon.r build/
exec docker run --rm --platform linux/amd64 --network none -v "$PWD/../..":/src -w /src/plugins/mastodon \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -Wextra -Wno-pointer-sign -Wno-unused-parameter -I/src/build/mill/sdk/Interfaces -I/src/build/mill/src/core -ffunction-sections -fdata-sections"
	$CC -c /src/build/mill/sdk/Glue/MillGlue.c -o build/glue.o
	$CC -c /src/build/mill/src/core/http11.c -o build/http11.o
	O="build/glue.o build/http11.o"
	for f in Plugin json content masto; do $CC -Werror -c $f.c -o build/$f.o; O="$O build/$f.o"; done
	m68k-apple-macos-gcc -m68000 $O -o build/Mastodon.flt -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles \
		-Wl,-Map,build/mastodon.map
	sed -n "/^Linker script and memory map/,\$p" build/mastodon.map > build/linked.map
	if grep -q -E "malloc|printf" build/linked.map; then echo "malloc or printf is linked in"; exit 1; fi
	cd build && Rez -I/Retro68-build/toolchain/m68k-apple-macos/RIncludes -I/src/build/mill/sdk/Interfaces Mastodon.r -o Mastodon.bin -t MPlg -c MiLL --cc Mastodon.bin
	ls -l Mastodon.flt *.bin'
