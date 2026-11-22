#!/usr/bin/env sh
# Host unit tests for the pure-C protocol/JSON component.
# Usage: test/host/run_tests.sh   (needs cc and python3)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC="$ROOT/firmware/components/seplos"
OUT=${OUT:-$(mktemp -d)}
CC=${CC:-cc}
CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"

$CC $CFLAGS -I"$SRC/include" "$SRC/seplos_proto.c" "$ROOT/test/host/test_seplos_proto.c" -o "$OUT/test_seplos_proto"
$CC $CFLAGS -I"$SRC/include" "$SRC/seplos_proto.c" "$SRC/seplos_json.c" "$SRC/sbuf.c" "$ROOT/test/host/json_dump.c" -o "$OUT/json_dump"

"$OUT/test_seplos_proto"
"$OUT/json_dump" > "$OUT/json.txt"
python3 -I "$ROOT/test/host/check_json.py" < "$OUT/json.txt"
python3 -I "$ROOT/tools/bms_simulator.py" --self-test
echo "host tests passed"
