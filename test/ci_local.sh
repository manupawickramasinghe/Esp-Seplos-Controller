#!/usr/bin/env bash
# Run the same steps as .github/workflows/ci.yml locally (Docker + cc + python3).
# Builds are capped at 2 CPUs. Usage: test/ci_local.sh [targets...]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IDF_IMAGE=${IDF_IMAGE:-espressif/idf:v6.1.1}
TARGETS=${*:-esp32c3 esp32 esp32s3}
cd "$ROOT"

idf() {
  docker run --rm --cpus 2 -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT:/project" -w /project/firmware "$IDF_IMAGE" bash -c "$1"
}

echo "== host tests"
test/host/run_tests.sh

for t in $TARGETS; do
  echo "== build $t"
  idf "set -e
    idf.py -B build-$t -D SDKCONFIG=build-$t/sdkconfig set-target $t >/dev/null
    idf.py -B build-$t -D SDKCONFIG=build-$t/sdkconfig build > build-$t/build.log 2>&1 || { tail -40 build-$t/build.log; exit 1; }
    if grep -E 'warning:' build-$t/build.log | grep -E '/(main|components/seplos)/'; then echo 'compiler warnings in project code'; exit 1; fi
    grep 'binary size' build-$t/build.log
    cd build-$t
    esptool --chip $t merge-bin -o seplos-controller-$t-full.bin @flash_args >/dev/null
    cp seplos_controller.bin seplos-controller-$t-ota.bin"
done

echo "== QEMU build"
idf 'set -e
  idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" set-target esp32 >/dev/null
  idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" build > build-qemu/build.log 2>&1 || { tail -40 build-qemu/build.log; exit 1; }
  if grep -E "warning:" build-qemu/build.log | grep -E "/(main|components/seplos)/"; then echo "compiler warnings in project code"; exit 1; fi'

echo "== QEMU end-to-end"
test/qemu/run_e2e.sh
echo "== all CI steps passed"
