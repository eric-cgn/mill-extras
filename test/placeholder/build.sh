#!/bin/sh
# NOT FOR RELEASE. Build the placeholder Plug-In with Retro68 (the same pinned image as Mill):
# build/TEST Placeholder.bin. Only EXTRAS_PLACEHOLDER=1 ./build.sh uses it.
set -e
cd "$(dirname "$0")"
mkdir -p build
python3 ../../tools/deps.py      # Mill 2.1.2's SDK and core code, in ../../build/mill
tr '\n' '\r' < about.txt > build/About.txt
cp Placeholder.r build/
exec docker run --rm --platform linux/amd64 --network none -v "$PWD/../..":/src -w /src/test/placeholder \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -I/src/build/mill/sdk/Interfaces -ffunction-sections"
	$CC -c /src/build/mill/sdk/Glue/MillGlue.c -o build/glue.o
	$CC -c Plugin.c -o build/Plugin.o
	m68k-apple-macos-gcc -m68000 build/glue.o build/Plugin.o -o build/Placeholder.flt -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles
	cd build && Rez -I/Retro68-build/toolchain/m68k-apple-macos/RIncludes -I/src/build/mill/sdk/Interfaces Placeholder.r -o "TEST Placeholder.bin" -t MPlg -c MiLL --cc "TEST Placeholder.bin"
	ls -l Placeholder.flt *.bin'
