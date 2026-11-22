#!/usr/bin/env bash
# Run the firmware in QEMU with a simulated BMS and open the web UI at
# http://127.0.0.1:${PORT:-18080}/ - no hardware needed. Ctrl+C to stop.
# Needs the QEMU build (see run_e2e.sh). PACKS=0,1 simulates two packs.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IDF_IMAGE=${IDF_IMAGE:-espressif/idf:v6.1.1}
PORT=${PORT:-18080}
PACKS=${PACKS:-0,1}
OUT=${OUT:-$(mktemp -d)}
DOCKER_FLAGS=${DOCKER_FLAGS:--it}   # e.g. DOCKER_FLAGS="-d --name seplos-demo" to run in the background
chmod 777 "$OUT"
echo "UI: http://127.0.0.1:$PORT/   logs: $OUT/uart0.log"
exec docker run --rm $DOCKER_FLAGS --cpus 2 -u "$(id -u):$(id -g)" -e HOME=/tmp -p "127.0.0.1:$PORT:8080" \
  -v "$ROOT:/project:ro" -v "$OUT:/out" "$IDF_IMAGE" bash -c "
    set -e
    cd /project/firmware/build-qemu
    esptool --chip esp32 merge-bin --fill-flash-size 4MB -o /out/flash.bin @flash_args >/out/merge.log 2>&1
    python3 -I /project/tools/bms_simulator.py --tcp-listen 5555 --packs $PACKS > /out/sim.log 2>&1 &
    sleep 1
    exec python3 -I /project/test/qemu/qemu_supervisor.py /out/uart0.log -- \
      qemu-system-xtensa -nographic -no-reboot -M esp32 -m 4M \
      -drive file=/out/flash.bin,if=mtd,format=raw \
      -nic user,model=open_eth,hostfwd=tcp::8080-:80 \
      -global driver=timer.esp32.timg,property=wdt_disable,value=true \
      -chardev file,id=u0,path=/out/uart0.log,append=on -serial chardev:u0 \
      -serial tcp:127.0.0.1:5555 -monitor none"
