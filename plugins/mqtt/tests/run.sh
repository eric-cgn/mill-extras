#!/bin/sh
# Build and run the MQTT Plug-In's host tests (gcc with warnings as errors and sanitizers), in
# the same pinned Retro68 image that builds Mill, without a network: test_mqtt.c, the packets
# and topics; then test_broker.c, its requests end to end (mqtt.c on a MillHost of the test's
# own) against broker.py on 127.0.0.1. SHOW=1 prints each request's page.
set -e
cd "$(dirname "$0")/../../.."
python3 tools/deps.py      # Mill 2.1.2's core code, in build/mill
exec docker run --rm --platform linux/amd64 --network none -e SHOW -v "$PWD":/src -w /src \
  ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
  set -e
  T=plugins/mqtt
  CC="gcc -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -Ibuild/mill/src/core"
  $CC -o /tmp/tm $T/tests/test_mqtt.c $T/mqttutil.c build/mill/src/core/charset.c && /tmp/tm
  $CC -Wno-unused-parameter -I$T/tests/mac -Ibuild/mill/sdk/Interfaces -o /tmp/tb $T/tests/test_broker.c $T/mqtt.c $T/mqttutil.c \
    build/mill/src/core/charset.c build/mill/src/core/fmt.c
  python3 -u $T/tests/broker.py --host 127.0.0.1 --port 18830 --login-port 18831 --user tester --password secret \
    --seed $T/tests/seed.txt > /tmp/broker.log 2>&1 &
  b=$!
  s=0; /tmp/tb || s=$?
  kill $b
  [ $s = 0 ] || { echo "broker log:"; cat /tmp/broker.log; }
  exit $s'
