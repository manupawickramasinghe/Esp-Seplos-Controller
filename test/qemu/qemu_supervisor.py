#!/usr/bin/env python3
"""Run QEMU and cold-start it again whenever the guest reboots.

Espressif QEMU implements esp_restart() as a CPU-only reset; emulated peripherals
(open_eth, UART) keep their pending interrupts and the next boot faults inside
ESP-IDF's esp_intr_alloc(). Real chips reset those peripherals in esp_restart(),
so for testing we treat every guest reboot like a power cycle: QEMU runs with
-no-reboot (exits on guest reset, before the bootloader can run again), and this
script starts it again on the same flash image.

usage: qemu_supervisor.py <uart0 log> -- <qemu command...>
"""
import subprocess
import sys
import time

MARKERS = ("rst:0xc (SW_CPU_RESET)", "rst:0x3 (SW_RESET)", "Rebooting...")


def count(path):
    try:
        text = open(path, errors="replace").read()
    except FileNotFoundError:
        return 0
    return sum(text.count(m) for m in MARKERS)


def main():
    log = sys.argv[1]
    cmd = sys.argv[sys.argv.index("--") + 1:]
    open(log, "a").close()
    boots = 0
    while True:
        boots += 1
        seen = count(log)
        with open(log, "a") as f:
            f.write(f"\n=== qemu cold start #{boots} ===\n")
        err = open(log + ".qemu-stderr.log", "a")
        proc = subprocess.Popen(cmd, stderr=err)
        # Normally QEMU runs with -no-reboot and simply exits when the guest resets.
        # The log marker is a fallback in case a reset path does not reach QEMU's
        # reset handler.
        while proc.poll() is None:
            time.sleep(0.05)
            if count(log) > seen:
                proc.terminate()
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                break
        err.close()
        with open(log, "a") as f:
            f.write(f"\n=== qemu exited rc={proc.returncode} ===\n")
        if proc.returncode not in (0, -15):
            return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
