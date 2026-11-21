# Reverse engineering the ESP8266 firmware v1.0.1.2

The repository only shipped `legacy/esp8266/espflasher/firmware_v1.0.1.2.bin` (893 680 bytes,
no source). This document records what was recovered from it and how, so the ESP-IDF rewrite
can be checked against it. All statements are from static analysis of the binary; the original
firmware was not executed.

## Reproducing the analysis

```sh
mkdir -p /tmp/re && cp legacy/esp8266/espflasher/firmware_v1.0.1.2.bin /tmp/re/
cd /tmp/re
esptool --chip esp8266 image-info firmware_v1.0.1.2.bin   # eboot at 0x0
dd if=firmware_v1.0.1.2.bin of=app.bin bs=4096 skip=1     # application image at 0x1000
esptool --chip esp8266 image-info app.bin                 # IROM 0x40201010, IRAM, DRAM segments
python3 -I $REPO/tools/re/find_gzip.py app.bin gz         # extracts the two embedded web pages
python3 -I $REPO/tools/re/esp8266_to_elf.py app.bin app.elf
$GHIDRA/support/analyzeHeadless /tmp/re/proj seplos -import app.elf -processor Xtensa:LE:32:default \
  -scriptPath $REPO/tools/re -postScript DecompByStrings.java /tmp/re/decomp.c \
  "~20004642E00200FD37" "Frame Checksum Error" " , \"SOC\" : " "Seplos/" "Bms Data wrong, count:"
python3 -I $REPO/tools/re/peek.py app.bin 4020c574 4020c578 4020c584   # literal-pool constants
```

`esp8266_to_elf.py` wraps the raw segments into an ELF so Ghidra 12 (Xtensa module) can load
it; `DecompByStrings.java` decompiles every function that references a given string through an
`l32r` literal pool. ROM helpers are called through literal-pool pointers into `0x4000xxxx`; they
were identified with the ESP8266 ROM symbol table (`__floatsidf` 0x4000e2f0, `__divdf3` 0x4000cb94,
`__muldf3` 0x4000c8f0, `__truncdfsf2` 0x4000cd5c, `__floatsisf` 0x4000e2ac, `__extendsfdf2`
0x4000cdfc, `__subsf3` 0x4000c268, `__mulsf3` 0x4000c3dc).

## Build and libraries

| Evidence | Meaning |
|---|---|
| `2.2.2-dev(38a443e)` | ESP8266 NONOS SDK used by the Arduino core |
| `ESPAsync_WiFiManager v1.9.2`, `/wifi_cred.dat` | khoih-prog ESPAsync_WiFiManager (config portal, multi-Wi-Fi) |
| `/drd.dat` | ESP_DoubleResetDetector (reset twice within 10 s → portal) |
| `LittleFS`, `/ConfigMQTT.json` | MQTT settings stored as JSON on LittleFS |
| `Publish acknowledged.`, `Session present:` | AsyncMqttClient |
| `ElegantOTA on SEPLOS`, `/update/identity` | AsyncElegantOTA |
| `text/event-stream`, `/events` | ESPAsyncWebServer + AsyncEventSource |
| `pool.ntp.org` | SNTP for the "online since" time |

Serial ports: `Serial.begin(115200)` for debug and `Serial.begin(19200, SERIAL_8N1)` for the BMS
(UART0 TX/RX), `GPIO0` drives DE/RE high while sending. Controller id =
`"SEPLOS_" + String(ESP.getChipId(), HEX)`, which is also the access-point SSID (password `12345678`).

## HTTP endpoints

`/` (gzipped page, 235 kB, mostly two inline images), `/AllJsonData`, `/dataonce`,
`/System?BatName=`, `/ShutDown`, `/ReSeT`, `/heap`, `/debug`, `/mqttreconnect`, `/events`,
`/update` (ElegantOTA). Every endpoint is a plain GET without authentication.

## MQTT

Base topic `Seplos/<controller id>/`; publishes `Info` (text) and `AllJsonData` (JSON, QoS 0);
subscribes `ShutDown` and shuts down on payload `true`, `1` or `on`. The main loop runs every
333 ms as a three-step state machine: poll BMS → build JSON → publish, i.e. one publish per second.

## BMS polling and decoding (function at 0x4020c5a8)

1. Send the constant `~20004642E00200FD37` + CR, read the reply; retry up to 19 times while the
   reply is shorter than 4 characters (publishing `Bms Data wrong, count:<n>` each time).
2. `LENGTH` = characters 9..12; compare with the computed length checksum → `Laenge crc wrong`.
3. Checksum at `13 + LENID` → `Frame Checksum Error`; last character must be CR →
   `Format error(EOI) <r> fehlt`.
4. Decode from character 13 with these scale factors (doubles from the literal pool):

| Field | Code | Scale |
|---|---|---|
| address | chars 3..4 | |
| data flag, pack, cell count | 1 byte each | |
| cells | `hex / 1000.0` | V |
| min / max | initial min 4.0 V, max 0.0 V; diff `(max − min) * 1000` → int | mV |
| temperatures | **cursor reset to char 0x53** then `(hex − 2731) / 10.0` | °C |
| current | `> 0x7FFF` → `−((−v) & 0xFFFF) * 0.01`, else `v / 100.0` | A |
| voltage, remaining, capacity, rated, port voltage | `/ 100.0` | V / Ah |
| SOC, SOH | `/ 10.0` | % |
| cycles, custom count, 4 reserved words | integer | |

The shutdown frame is the constant `~20004645A006000400FC70` (function 0x402091b0).

The JSON builder (0x40208c48) prints SOC, capacities, voltages, current, temperatures with 2
decimals and cells with 3; temperature 5 is `AmbientTemp`, 6 is `PowerTemp`, others `CellTemp<n>`.
The rewrite reproduces this format exactly for `/AllJsonData` and MQTT `AllJsonData`
(checked by `test/host/check_json.py`).

## Bugs and limitations found in the original

1. **Temperatures assume 16 cells.** After the cell loop the parser jumps to the fixed offset
   0x53 (= 19 + 16·4). With 8, 15 or 24 cells every value after the cells is read from the wrong
   position.
2. **Return code ignored.** The `RTN` field is never checked, so an error reply from the BMS is
   decoded as data.
3. **Length only checked through its nibble checksum**, not against the received frame size;
   truncated frames can be partly decoded.
4. **Only address 0** is ever queried (constant request frame).
5. **Alarms are never read** (no 0x44 request) - the README lists them as missing.
6. **Shutdown via `GET /ShutDown`** without confirmation or authentication; any prefetch or
   crawler hitting the URL switches the battery off. `/ReSeT` is a GET as well.
7. **No authentication** anywhere, including firmware upload.
8. **MQTT stalls** after a while (documented in the original README). When `publish()` returns 0
   the code logs `mqtt result ist 0 , clearqueue:` and calls into the client to drop its queue,
   a workaround for messages piling up in AsyncMqttClient.
9. Up to 19 blocking retries inside `loop()` when the BMS does not answer.
10. Floating point everywhere (soft-float on the ESP8266) for values that are fixed-point integers
    on the wire.

All of these are addressed in the ESP-IDF version (see the comparison table in the
[README](../README.md#what-is-new-compared-to-the-esp8266-firmware)); points 1-3 are covered by
unit tests.
