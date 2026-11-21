#!/usr/bin/env python3
"""Reference Seplos V2.0 frame encoder (independent of the C implementation).

Used by the host tests and the BMS simulator. Values mirror the screenshot of
the original firmware's web page (16 cells, 6 temperatures).
"""
import struct


def checksum(s: str) -> int:
    return (~sum(s.encode()) + 1) & 0xFFFF


def length_field(lenid: int) -> int:
    lchk = (~((lenid & 0xF) + ((lenid >> 4) & 0xF) + ((lenid >> 8) & 0xF)) + 1) & 0xF
    return (lchk << 12) | lenid


def frame(addr: int, cid1: int, cid2: int, info: bytes, ver: int = 0x20) -> str:
    body = f"{ver:02X}{addr:02X}{cid1:02X}{cid2:02X}{length_field(len(info) * 2):04X}{info.hex().upper()}"
    return f"~{body}{checksum(body):04X}\r"


def telemetry_info(pack=0, cells_mv=None, temps_c=None, current_a=-1.87, voltage_v=53.41,
                   remain_ah=194.61, capacity_ah=200.0, soc=97.3, rated_ah=200.0, cycles=86,
                   soh=100.0, port_v=53.46) -> bytes:
    cells_mv = cells_mv or [3336, 3335, 3337, 3338, 3341, 3333, 3338, 3337,
                            3341, 3337, 3341, 3340, 3340, 3339, 3340, 3337]
    temps_c = temps_c or [22.3, 22.2, 21.7, 21.9, 26.6, 23.8]
    b = bytes([0x00, pack, len(cells_mv)])
    b += b"".join(struct.pack(">H", v) for v in cells_mv)
    b += bytes([len(temps_c)])
    b += b"".join(struct.pack(">H", round(t * 10) + 2731) for t in temps_c)
    b += struct.pack(">h", round(current_a * 100))
    b += struct.pack(">H", round(voltage_v * 100))
    b += struct.pack(">H", round(remain_ah * 100))
    b += bytes([10])
    b += struct.pack(">H", round(capacity_ah * 100))
    b += struct.pack(">H", round(soc * 10))
    b += struct.pack(">H", round(rated_ah * 100))
    b += struct.pack(">H", cycles)
    b += struct.pack(">H", round(soh * 10))
    b += struct.pack(">H", round(port_v * 100))
    b += b"\x00\x00" * 4
    return b


def alarm_info(pack=0, cells=16, temps=6, events=(0, 0, 0, 0, 0, 0), switch=0x03,
               balancing=0x0000, system=0x01, disconnected=0x0000, ev7=0, ev8=0,
               cell_alarms=None) -> bytes:
    ca = cell_alarms or [0] * cells
    b = bytes([0x00, pack, cells]) + bytes(ca) + bytes([temps]) + bytes(temps)
    b += bytes([0, 0, 0]) + bytes(events)
    b += bytes([switch, balancing & 0xFF, balancing >> 8, system,
                disconnected & 0xFF, disconnected >> 8, ev7, ev8])
    return b


if __name__ == "__main__":
    print(repr(frame(0, 0x46, 0x42, bytes([0]))))
    print(repr(frame(0, 0x46, 0x45, bytes([0, 4, 0]))))
    print(repr(frame(0, 0x46, 0x00, telemetry_info())))
    print(repr(frame(0, 0x46, 0x00, alarm_info(events=(0, 0x01, 0, 0, 0, 0x04), balancing=0x0020))))
