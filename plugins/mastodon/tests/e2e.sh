#!/bin/sh
# The Mastodon Plug-In end to end on this host (cc and python3): Plugin.c, with host calls on
# sockets (e2e.c), against the fake instance and proxy (fakemasto.py), on 127.0.0.1 only.
set -e
cd "$(dirname "$0")"
mkdir -p out
python3 ../../../tools/deps.py      # Mill 2.1.2's SDK and core code, in ../../../build/mill
cc -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-pointer-sign -Wno-unused-parameter -Wno-unknown-attributes -fsanitize=address,undefined -g \
  -Istub -I../../../build/mill/sdk/Interfaces -I../../../build/mill/src/core -o out/e2e \
  e2e.c ../Plugin.c ../json.c ../content.c ../masto.c ../../../build/mill/src/core/http11.c \
  ../../../build/mill/src/core/charset.c ../../../build/mill/src/core/fmt.c
exec python3 e2e.py out/e2e
