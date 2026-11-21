#!/bin/sh
# Build and run the Modern Text Plug-In's host tests (gcc with warnings as errors and
# sanitizers), in the same pinned Retro68 image that builds Mill, as Mill's tests/run.sh does.
set -e
cd "$(dirname "$0")/../../.."
python3 tools/deps.py      # Mill 2.1.2's core code, in build/mill
exec docker run --rm --platform linux/amd64 --network none -v "$PWD":/src -w /src \
  ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
  T=plugins/text
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -Ibuild/mill/src/core -o /tmp/tt \
    $T/tests/test_text.c $T/text.c $T/gemtext.c $T/markdown.c $T/inline.c build/mill/src/core/charset.c && /tmp/tt $T/tests'
