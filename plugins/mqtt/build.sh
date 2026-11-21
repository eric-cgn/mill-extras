#!/bin/sh
# Build MQTT with Retro68 (the same pinned image as Mill): build/MQTT.bin, a MacBinary
# Plug-In file for the "Mill Plug-Ins" folder. mqttutil.c is plain C, tested on a host by
# tests/run.sh, which also runs mqtt.c against tests/broker.py; the check at the end shows
# nothing but the host's calls is linked (no malloc, no printf).
set -e
cd "$(dirname "$0")"
mkdir -p build
python3 ../../tools/deps.py      # Mill 2.1.2's SDK and core code, in ../../build/mill
{ cat ../../LICENSE; printf '\nnewlib\nThe C library in this Plug-In is newlib, as Retro68 builds it (memory and string\nfunctions). Its notices:\n\n'
  tail -n +6 ../../licences/newlib.txt; } | tr '\n' '\r' > build/License.txt   # a Mac TEXT resource has CR line ends
tr '\n' '\r' < about.txt > build/About.txt
cp MQTT.r build/
exec docker run --rm --platform linux/amd64 --network none -v "$PWD/../..":/src -w /src/plugins/mqtt \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -Wextra -Wno-pointer-sign -Wno-unused-parameter -I/src/build/mill/sdk/Interfaces -I/src/build/mill/src/core -ffunction-sections -fdata-sections"
	$CC -c /src/build/mill/sdk/Glue/MillGlue.c -o build/glue.o
	$CC -c /src/build/mill/src/core/charset.c -o build/charset.o
	O="build/glue.o build/charset.o"
	for f in Plugin mqtt mqttutil; do $CC -Werror -c $f.c -o build/$f.o; O="$O build/$f.o"; done
	m68k-apple-macos-gcc -m68000 $O -o build/MQTT.flt -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles \
		-Wl,-Map,build/mqtt.map
	sed -n "/^Linker script and memory map/,\$p" build/mqtt.map > build/linked.map
	if grep -q -E "malloc|printf" build/linked.map; then echo "malloc or printf is linked in"; exit 1; fi
	cd build && Rez -I/Retro68-build/toolchain/m68k-apple-macos/RIncludes -I/src/build/mill/sdk/Interfaces MQTT.r -o MQTT.bin -t MPlg -c MiLL --cc MQTT.bin
	ls -l MQTT.flt *.bin'
