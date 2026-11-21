#!/bin/sh
# Build the Millennium Pack for the Mill that tools/deps.py pins (2.1.2):
# - tools/deps.py fetches Mill's source and EUP's source release once into build/, checks them
#   by SHA-256, and unpacks Mill into build/mill/;
# - Mill itself (build/mill/build/Mill.bin), once, for the Plug-Ins' icons on the Extras Disk;
# - every plugins/*/ that has a build.sh;
# - then, without a network: Install Millennium Pack (build/installer/Install-Millennium-Pack-*.bin
#   and .hqx, made with EUP from installer/extras.ini) and the "Mill Extras Disk"
#   (dist/MillExtras-*.image), by tools/extras.py.
# Every container is the pinned Retro68 image, and only tools/deps.py uses the network.
# EXTRAS_PLACEHOLDER=1 ./build.sh adds the test-only placeholder Plug-In (test/placeholder) and
# marks the disk and installer TEST: never ship that. EXTRAS_DEV=abc1234 ./build.sh names the
# installer and disk for a dev build (1.0-dev-abc1234).
set -e
cd "$(dirname "$0")"
IMAGE=ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c
python3 tools/deps.py
if [ ! -f build/mill/build/Mill.bin ]; then
	echo "extras: building Mill (for its icons)"
	docker run --rm --platform linux/amd64 --network none -v "$PWD/build/mill":/src -w /src "$IMAGE" sh -c '
		mkdir -p build && cd build &&
		cmake .. -DCMAKE_TOOLCHAIN_FILE=/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake >/dev/null && make'
fi
n=0
for b in plugins/*/build.sh; do
	[ -f "$b" ] || continue
	echo "extras: building ${b%/build.sh}"
	sh "$b"
	n=$((n + 1))
done
if [ "$n" = 0 ]; then echo "extras: no Plug-Ins in plugins/ yet"; fi
if [ "${EXTRAS_PLACEHOLDER:-}" = 1 ]; then
	echo "extras: building the TEST placeholder (not for release)"
	sh test/placeholder/build.sh
fi
# the installer's icon is Install Mill's (Mill's installer/icons.r), Rezzed into the stage
exec docker run --rm --platform linux/amd64 --network none -v "$PWD":/src -w /src \
	-e EXTRAS_PLACEHOLDER="${EXTRAS_PLACEHOLDER:-}" -e EXTRAS_DEV="${EXTRAS_DEV:-}" "$IMAGE" sh -c '
	set -e
	mkdir -p build/installer/stage
	Rez build/mill/installer/icons.r -t rsrc -c RSED -o "build/installer/stage/Install icon.bin"
	python3 -u tools/extras.py'
