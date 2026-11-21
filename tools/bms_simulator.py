#!/usr/bin/env python3
"""Seplos BMS simulator (protocol V2.0) for testing the controller without a battery.

Answers 0x42 (telemetry), 0x44 (alarms) and 0x45 (shutdown) for one or more pack
addresses. Transport:

  --tcp-listen PORT   act as a TCP server (e.g. QEMU: -serial tcp:127.0.0.1:PORT)
  --tcp-connect H:P   connect to a TCP endpoint
  --serial /dev/ttyUSB0  real RS485 adapter (needs pyserial)

  --self-test         run built-in checks and exit
"""
import argparse
import math
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import seplos_frames as sf  # noqa: E402


class Pack:
    def __init__(self, addr: int, cells: int = 16):
        self.addr = addr
        self.cells = cells
        self.shutdown = False
        self.t0 = time.time()

    def current(self) -> float:
        return round(-5 + 10 * math.sin((time.time() - self.t0) / 20), 2)

    def telemetry(self) -> bytes:
        t = time.time() - self.t0
        base = 3300 + int(40 * math.sin(t / 30))
        cells = [base + ((i * 7 + self.addr * 3) % 9) for i in range(self.cells)]
        current = self.current()
        voltage = round(sum(cells) / 1000, 2)
        return sf.telemetry_info(pack=self.addr, cells_mv=cells, current_a=current, voltage_v=voltage,
                                 soc=round(60 + 30 * math.sin(t / 60), 1), port_v=voltage + 0.05)

    def alarms(self) -> bytes:
        bal = 1 << (int(time.time() / 5) % self.cells)
        i = self.current()
        system = 0x20 if self.shutdown else 0x02 if i > 0.05 else 0x01 if i < -0.05 else 0x10
        return sf.alarm_info(pack=self.addr, cells=self.cells, balancing=bal & 0xFFFF, system=system)


def parse_request(frame: str):
    """Return (addr, cid1, cid2, info bytes) or None if the frame is invalid."""
    if not frame.startswith("~") or len(frame) < 17:
        return None
    body, chk = frame[1:-4], frame[-4:]
    try:
        if sf.checksum(body) != int(chk, 16):
            return None
        length = int(body[8:12], 16)
        lenid = length & 0xFFF
        if sf.length_field(lenid) != length or len(body) != 12 + lenid:
            return None
        return int(body[2:4], 16), int(body[4:6], 16), int(body[6:8], 16), bytes.fromhex(body[12:])
    except ValueError:
        return None


def respond(packs: dict, request: str):
    req = parse_request(request)
    if req is None:
        return None
    addr, cid1, cid2, info = req
    pack = packs.get(addr)
    if pack is None or cid1 != 0x46:
        return None  # a real bus stays silent for unknown addresses
    if cid2 == 0x42:
        return sf.frame(addr, 0x46, 0x00, pack.telemetry())
    if cid2 == 0x44:
        return sf.frame(addr, 0x46, 0x00, pack.alarms())
    if cid2 == 0x45:
        print(f"[sim] SHUTDOWN request for pack {addr}: INFO={info.hex()}", flush=True)
        pack.shutdown = True
        return sf.frame(addr, 0x46, 0x00, b"")
    return sf.frame(addr, 0x46, 0x04, b"")  # RTN 04: CID2 invalid


def serve(read, write, packs, verbose):
    buf = b""
    while True:
        try:
            chunk = read()
        except OSError:  # peer reset (e.g. QEMU restarted)
            return
        if not chunk:
            return
        buf += chunk
        while b"\r" in buf:
            line, buf = buf.split(b"\r", 1)
            req = line[line.rfind(b"~"):].decode("ascii", "replace") if b"~" in line else ""
            if not req:
                continue
            resp = respond(packs, req)
            if verbose:
                print(f"[sim] <- {req!r}\n[sim] -> {resp!r}", flush=True)
            if resp:
                time.sleep(0.02)  # BMS turnaround time
                try:
                    write(resp.encode())
                except OSError:
                    return


def self_test():
    packs = {0: Pack(0), 1: Pack(1, cells=8)}
    r = respond(packs, "~20004642E00200FD37")
    assert r and r.startswith("~200046001096") and r.endswith("\r"), r
    assert parse_request(r[:-1]) is not None
    r = respond(packs, sf.frame(1, 0x46, 0x42, bytes([1]))[:-1])
    assert r and r.startswith("~200146"), r
    assert respond(packs, "~20004642E00200FD38") is None  # bad checksum
    assert respond(packs, sf.frame(5, 0x46, 0x42, bytes([5]))[:-1]) is None  # absent pack
    assert respond(packs, "~20004645A006000400FC70") is not None and packs[0].shutdown
    print("simulator self-test passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--packs", default="0", help="comma separated pack addresses, e.g. 0,1,2")
    ap.add_argument("--cells", type=int, default=16)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--tcp-listen", type=int)
    g.add_argument("--tcp-connect")
    g.add_argument("--serial")
    ap.add_argument("--baud", type=int, default=19200)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    packs = {int(x): Pack(int(x), a.cells) for x in a.packs.split(",")}
    if a.serial:
        import serial  # pyserial
        port = serial.Serial(a.serial, a.baud, timeout=0.1)

        def ser_read():
            while True:
                data = port.read(256)
                if data:
                    return data

        serve(ser_read, port.write, packs, a.verbose)
    elif a.tcp_connect:
        host, port = a.tcp_connect.rsplit(":", 1)
        while True:
            try:
                s = socket.create_connection((host, int(port)))
            except OSError:
                time.sleep(0.5)
                continue
            print(f"[sim] connected to {a.tcp_connect}", flush=True)
            serve(lambda: s.recv(256), s.sendall, packs, a.verbose)
            s.close()
    else:
        srv = socket.create_server(("127.0.0.1", a.tcp_listen or 5555))
        print(f"[sim] listening on 127.0.0.1:{a.tcp_listen or 5555}", flush=True)
        while True:
            s, _ = srv.accept()
            print("[sim] client connected", flush=True)
            serve(lambda: s.recv(256), s.sendall, packs, a.verbose)
            s.close()
            print("[sim] client disconnected", flush=True)


if __name__ == "__main__":
    main()
