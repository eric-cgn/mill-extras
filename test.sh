#!/bin/sh
# Every host test, each in the pinned Retro68 image without a network: each Plug-In's
# tests/run.sh, then the Mastodon Plug-In end to end (plugins/mastodon/tests/e2e.sh).
set -e
cd "$(dirname "$0")"
IMAGE=ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c
python3 tools/deps.py
for t in plugins/*/tests/run.sh; do
	echo "== $t"
	sh "$t"
done
echo "== plugins/mastodon/tests/e2e.sh"
docker run --rm --platform linux/amd64 --network none -v "$PWD":/src -w /src "$IMAGE" sh plugins/mastodon/tests/e2e.sh
