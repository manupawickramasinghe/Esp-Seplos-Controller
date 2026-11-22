#!/usr/bin/env bash
# End-to-end test: firmware in QEMU (esp32 + open_eth) <-> BMS simulator on UART1,
# Mosquitto broker for MQTT, HTTP API driven by e2e.py.
#
# Prerequisite: QEMU build in firmware/build-qemu (see sdkconfig.qemu), e.g.
#   idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig \
#          -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" set-target esp32 build
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IDF_IMAGE=${IDF_IMAGE:-espressif/idf:v6.1.1}
OUT=${OUT:-$(mktemp -d)}
NAME=seplos-e2e-$$
chmod 777 "$OUT"

cleanup() { docker rm -f "$NAME" "$NAME-mqtt" >/dev/null 2>&1 || true; }
trap cleanup EXIT

# 1. flash image + QEMU + simulator in the IDF container
docker run -d --name "$NAME" --cpus 2 -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$ROOT:/project:ro" -v "$OUT:/out" "$IDF_IMAGE" bash -c '
    set -e
    cd /project/firmware/build-qemu
    esptool --chip esp32 merge-bin --fill-flash-size 4MB -o /out/flash.bin @flash_args >/out/merge.log 2>&1
    python3 -I /project/tools/bms_simulator.py --tcp-listen 5555 --packs 0,1 > /out/sim.log 2>&1 &
    sleep 1
    exec python3 -I /project/test/qemu/qemu_supervisor.py /out/uart0.log -- \
      qemu-system-xtensa -nographic -no-reboot -M esp32 -m 4M \
      -drive file=/out/flash.bin,if=mtd,format=raw \
      -nic user,model=open_eth,hostfwd=tcp:127.0.0.1:8080-:80 \
      -global driver=timer.esp32.timg,property=wdt_disable,value=true \
      -chardev file,id=u0,path=/out/uart0.log,append=on -serial chardev:u0 \
      -serial tcp:127.0.0.1:5555 -monitor none' >/dev/null

# 2. MQTT broker sharing the network namespace (guest reaches it as 10.0.2.2:1883)
printf 'listener 1883 127.0.0.1\nallow_anonymous true\n' > "$OUT/mosquitto.conf"
docker run -d --name "$NAME-mqtt" --network "container:$NAME" -v "$OUT/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro" \
  eclipse-mosquitto:latest >/dev/null
sleep 2

# 3. test driver (inside the IDF container: python3 + network namespace with QEMU)
set +e
docker exec "$NAME" python3 -I /project/test/qemu/e2e.py --out /out
RC=$?
set -e
echo "logs in $OUT (uart0.log, sim.log, mqtt.log)"
exit $RC
