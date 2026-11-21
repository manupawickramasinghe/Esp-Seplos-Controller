# Seplos BMS RS485 protocol V2.0 (as implemented)

Implementation: [`firmware/components/seplos/seplos_proto.c`](../firmware/components/seplos/seplos_proto.c).
Reference encoder used by the tests: [`tools/seplos_frames.py`](../tools/seplos_frames.py).

## Link

RS485, **19200 baud, 8N1**, half duplex, master/slave. The controller is the master; each BMS
answers only to its own address (DIP switches, all OFF = 0).

## Frame

All fields after `~` are ASCII hex, two characters per byte.

```
 ~   VER  ADR  CID1  CID2/RTN  LENGTH  INFO ...  CHKSUM  \r
 1    2    2    2       2         4    LENID       4     1     characters
```

| Field | Meaning |
|---|---|
| `VER` | protocol version `0x20` |
| `ADR` | pack address 0x00..0x0F |
| `CID1` | device type, `0x46` = lithium battery |
| `CID2` | command in requests; in responses this position carries `RTN` (0x00 = OK) |
| `LENGTH` | `LCHKSUM` (4 bits) + `LENID` (12 bits, number of INFO **characters**) |
| `CHKSUM` | 16-bit checksum |

```
LCHKSUM = (~(LENID[11:8] + LENID[7:4] + LENID[3:0]) + 1) & 0xF
CHKSUM  = (~(sum of the ASCII codes of every character between '~' and CHKSUM) + 1) & 0xFFFF
```

The checksum covers the ASCII characters, so it differs for upper- and lower-case hex; the BMS
uses upper case.

## Requests used

| Purpose | CID2 | INFO | Address 0 frame |
|---|---|---|---|
| Telemetry (analog values) | `0x42` | pack address | `~20004642E00200FD37\r` |
| Alarms / status | `0x44` | pack address | `~20004644E00200FD35\r` |
| Shutdown | `0x45` | pack address, `04`, `00` | `~20004645A006000400FC70\r` |

The `0x42` and `0x45` frames for address 0 are byte-identical to the ones hard-coded in the
original firmware (unit-tested). For other addresses ADR and the first INFO byte carry the address.

## 0x42 telemetry response INFO

Offsets in bytes (each byte is two characters on the wire). `M` = cell count, `N` = sensor count.
This layout and the scale factors were recovered from the original firmware.

| Offset | Size | Field | Unit / scale |
|---|---|---|---|
| 0 | 1 | DATAFLAG | |
| 1 | 1 | command value / pack | |
| 2 | 1 | `M` cell count | |
| 3 | 2·M | cell voltages | mV |
| 3+2M | 1 | `N` temperature count (6 on Seplos: 4 cell, ambient, power/MOS) | |
| … | 2·N | temperatures | 0.1 K, °C = (raw − 2731) / 10 |
| … | 2 | current, signed | 10 mA, negative = discharge |
| … | 2 | pack voltage | 10 mV |
| … | 2 | remaining capacity | 10 mAh |
| … | 1 | custom field count `P` (10) | |
| … | 2 | full capacity | 10 mAh |
| … | 2 | state of charge | 0.1 % |
| … | 2 | rated capacity | 10 mAh |
| … | 2 | cycles | |
| … | 2 | state of health | 0.1 % |
| … | 2 | port (bus) voltage | 10 mV |
| … | 4 × 2 | reserved | |

With 16 cells and 6 sensors INFO is 75 bytes (`LENGTH = 1096`).

## 0x44 alarm response INFO

Layout per the Seplos V2.0 protocol document (the original firmware never requested it, so this
part is not cross-checked against the binary; the raw event bytes are exposed as `alarm_raw`).

| Field | Size |
|---|---|
| DATAFLAG, command value | 1 + 1 |
| `M` cells, cell alarm ×M (0 ok, 1 below lower limit, 2 above upper limit, 0xF0 other) | 1 + M |
| `N` sensors, temperature alarm ×N | 1 + N |
| charge current alarm, pack voltage alarm, discharge current alarm | 3 |
| alarm events 1-6 (bit fields below) | 6 |
| switch state: bit0 discharge MOS, bit1 charge MOS, bit2 current limit, bit3 heating | 1 |
| balancing cells 1-8, 9-16 | 2 |
| system state: bit0 discharging, bit1 charging, bit2 float charge, bit4 standby, bit5 shut down | 1 |
| disconnected cells 1-8, 9-16 | 2 |
| alarm events 7, 8 (optional, newer BMS firmware) | 2 |

| Event | bit 0 | bit 1 | bit 2 | bit 3 | bit 4 | bit 5 | bit 6 | bit 7 |
|---|---|---|---|---|---|---|---|---|
| 1 | voltage sensor fault | temperature sensor fault | current sensor fault | key switch fault | cell voltage difference fault | charge switch fault | discharge switch fault | current limit switch fault |
| 2 | cell high voltage alarm | cell over-voltage protection | cell low voltage alarm | cell under-voltage protection | pack high voltage alarm | pack over-voltage protection | pack low voltage alarm | pack under-voltage protection |
| 3 | charge high temp. alarm | charge over-temp. protection | charge low temp. alarm | charge under-temp. protection | discharge high temp. alarm | discharge over-temp. protection | discharge low temp. alarm | discharge under-temp. protection |
| 4 | ambient high temp. alarm | ambient over-temp. protection | ambient low temp. alarm | ambient under-temp. protection | power over-temp. protection | power high temp. alarm | cell low temp. heating | – |
| 5 | charge over-current alarm | charge over-current protection | discharge over-current alarm | discharge over-current protection | transient over-current protection | output short-circuit protection | transient over-current lockout | output short-circuit lockout |
| 6 | charge high voltage protection | intermittent recharge waiting | residual capacity alarm | residual capacity protection | cell low voltage, charging forbidden | output reverse polarity protection | output connection fault | – |
| 7 | – | – | – | – | automatic charging waiting | manual charging waiting | – | – |
| 8 | EEPROM storage fault | RTC clock fault | voltage not calibrated | current not calibrated | zero point not calibrated | – | – | – |

## Validation done by the firmware

A response is accepted only if: it starts with `~` and ends with `\r`, every field is hex,
`LCHKSUM` matches, the actual length equals 18 + `LENID`, `CHKSUM` matches, `RTN` is 0, the
address matches the request, and the INFO payload contains every field its counts announce.
An echo of the request (transceivers without RE control) is skipped. Up to three attempts are
made per poll.
