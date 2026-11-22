#!/usr/bin/env python3
"""End-to-end checks against the firmware running in QEMU (driven by run_e2e.sh).

HTTP goes to 127.0.0.1:8080 (QEMU hostfwd), MQTT to the Mosquitto broker on
127.0.0.1:1883 (the guest reaches the same broker as 10.0.2.2:1883).
Only the Python standard library is used.
"""
import argparse
import base64
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = "http://127.0.0.1:8080"
RESULTS = []


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + (f"  [{detail}]" if detail and not cond else ""), flush=True)
    return cond


def http(path, data=None, auth=None, raw=None, ctype="application/x-www-form-urlencoded", timeout=20):
    body = raw if raw is not None else (urllib.parse.urlencode(data).encode() if data is not None else None)
    req = urllib.request.Request(BASE + path, data=body)
    if body is not None:
        req.add_header("Content-Type", ctype)
    if auth:
        req.add_header("Authorization", "Basic " + base64.b64encode(auth.encode()).decode())
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)


def jget(path, **kw):
    st, body, _ = http(path, **kw)
    return st, json.loads(body) if body[:1] in (b"{", b"[") else body


def wait_for(fn, timeout, interval=1.0):
    end = time.time() + timeout
    last = None
    while time.time() < end:
        try:
            last = fn()
            if last:
                return last
        except Exception as e:  # noqa: BLE001 - device rebooting
            last = e
        time.sleep(interval)
    return None


def uart(out):
    try:
        return open(os.path.join(out, "uart0.log"), errors="replace").read()
    except FileNotFoundError:
        return ""


def sim_log(out):
    try:
        return open(os.path.join(out, "sim.log"), errors="replace").read()
    except FileNotFoundError:
        return ""


# ---- minimal MQTT 3.1.1 client ------------------------------------------------------------

def _enc_len(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def _str(s):
    b = s.encode()
    return struct.pack(">H", len(b)) + b


class Mqtt:
    def __init__(self, client_id):
        self.s = socket.create_connection(("127.0.0.1", 1883), timeout=10)
        var = _str("MQTT") + bytes([4, 0x02]) + struct.pack(">H", 60)
        pl = _str(client_id)
        self.s.sendall(bytes([0x10]) + _enc_len(len(var) + len(pl)) + var + pl)
        assert self._read_packet()[0] >> 4 == 2  # CONNACK
        self.messages = []
        self.lock = threading.Lock()

    def _recv(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise ConnectionError
            b += c
        return b

    def _read_packet(self):
        h = self._recv(1)[0]
        mult, length = 1, 0
        while True:
            d = self._recv(1)[0]
            length += (d & 127) * mult
            mult *= 128
            if not d & 128:
                break
        return h, self._recv(length)

    def subscribe(self, topic):
        pl = struct.pack(">H", 1) + _str(topic) + b"\x00"
        self.s.sendall(bytes([0x82]) + _enc_len(len(pl)) + pl)
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        self.s.settimeout(None)
        while True:
            try:
                h, body = self._read_packet()
            except Exception:  # noqa: BLE001
                return
            if h >> 4 == 3:
                tl = struct.unpack(">H", body[:2])[0]
                topic = body[2:2 + tl].decode()
                off = 2 + tl + (2 if (h >> 1) & 3 else 0)
                with self.lock:
                    self.messages.append((time.time(), topic, body[off:].decode(errors="replace")))

    def publish(self, topic, payload, retain=False):
        pl = _str(topic) + payload.encode()
        self.s.sendall(bytes([0x30 | (1 if retain else 0)]) + _enc_len(len(pl)) + pl)

    def find(self, pred, since=0.0):
        with self.lock:
            return [m for m in self.messages if m[0] >= since and pred(m[1], m[2])]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    # ---- boot -----------------------------------------------------------------------------
    ok = wait_for(lambda: "web server started" in uart(a.out) and "connected, IP" in uart(a.out), 90)
    check("firmware boots, gets IP, starts web server", ok, uart(a.out)[-1500:])
    if not ok:
        return finish(a)

    st, body, hdr = http("/")
    check("GET / serves gzipped UI", st == 200 and hdr.get("Content-Encoding") == "gzip" and body[:2] == b"\x1f\x8b")

    d = wait_for(lambda: (lambda r: r[1] if r[0] == 200 and r[1]["packs"] and r[1]["packs"][0]["online"] else None)(jget("/api/data")), 30)
    check("/api/data: pack 0 online via RS485 simulator", d, uart(a.out)[-800:])
    if not d:
        return finish(a)
    p = d["packs"][0]
    dev_id = d["device"]["id"]
    check("controller id format SEPLOS_<hex>", dev_id.startswith("SEPLOS_"), dev_id)
    check("telemetry decoded (16 cells, 6 temps)", len(p["cells"]) == 16 and len(p["temps"]["cells"]) == 4 and "ambient" in p["temps"], p)
    check("values plausible", 40 < p["voltage"] < 60 and 0 <= p["soc"] <= 100 and p["cycles"] == 86 and p["rated_ah"] == 200.0, p)
    check("cell min/max/diff consistent", abs(p["cell_max_v"] - p["cell_min_v"] - p["cell_diff_mv"] / 1000) < 0.0015, p)
    wait_for(lambda: jget("/api/data")[1]["packs"][0].get("status"), 10)
    p = jget("/api/data")[1]["packs"][0]
    check("alarm frame decoded (status + balancing)", p.get("status", {}).get("discharge_switch") is True and len(p.get("balancing", [])) == 1, p)

    st, legacy = jget("/AllJsonData")
    check("legacy /AllJsonData keeps original keys", st == 200 and list(legacy)[:3] == ["EspError", "BatName", "BMSAddress"]
          and legacy["CellCount"] == 16 and "Cell16" in legacy["CellVoltages"] and "PowerTemp" in legacy["Temperatures"], legacy)
    st, once = jget("/dataonce")
    check("legacy /dataonce", st == 200 and once["CONTROLLERID"] == dev_id and once["Topic_ShutDown"].endswith("/ShutDown"), once)
    st, _, _ = http("/ShutDown")
    check("legacy GET /ShutDown is disabled by default (404)", st == 404, st)
    st, _ = jget("/api/config")
    check("GET /api/config open while no admin password", st == 200)

    # ---- shutdown via HTTP --------------------------------------------------------------
    st, r = jget("/api/shutdown", data={"address": "0"})
    check("POST /api/shutdown without confirm -> 400", st == 400, r)
    st, r = jget("/api/shutdown", data={"address": "7", "confirm": "SHUTDOWN"})
    check("shutdown of unconfigured pack -> 404", st == 404, r)
    st, r = jget("/api/shutdown", data={"address": "0", "confirm": "SHUTDOWN"})
    ok = st == 200 and wait_for(lambda: "SHUTDOWN request for pack 0: INFO=000400" in sim_log(a.out), 10)
    check("POST /api/shutdown sends original 0x45 frame to BMS", ok, sim_log(a.out)[-500:])

    # ---- settings validation + MQTT + second pack -------------------------------------------
    st, r = jget("/api/config", data={"mqtt_uri": "http://nope"})
    check("invalid MQTT URI rejected", st == 400 and "mqtt://" in r.get("error", ""), r)
    st, r = jget("/api/config", data={"name": "Darth Vader"})
    check("rename without restart", st == 200 and r == {"ok": True, "restart": False}, r)
    check("rename applied", jget("/api/data")[1]["device"]["name"] == "Darth Vader")

    mq = Mqtt("e2e-test")
    mq.subscribe("#")
    t_cfg = time.time()
    st, r = jget("/api/config", data={"mqtt_uri": "mqtt://10.0.2.2:1883", "pack_count": "2"})
    check("MQTT + 2 packs saved, restart requested", st == 200 and r.get("restart") is True, r)
    time.sleep(3)
    d = wait_for(lambda: (lambda r: r[1] if r[0] == 200 and len(r[1]["packs"]) == 2 and all(x["online"] for x in r[1]["packs"]) else None)(jget("/api/data")), 90)
    check("after restart: 2 packs online", d, uart(a.out)[-800:])
    if d:
        check("pack 1 has 16 cells", len(d["packs"][1]["cells"]) == 16)
        check("MQTT connected", wait_for(lambda: jget("/api/data")[1]["device"]["mqtt"]["connected"], 30))
    base = f"Seplos/{dev_id}"
    time.sleep(4)
    check("MQTT status online (retained LWT topic)", mq.find(lambda t, m: t == base + "/status" and m == "online", t_cfg))
    check("MQTT Info ready message", mq.find(lambda t, m: t == base + "/Info" and m.startswith("Seplos Controller is Ready"), t_cfg))
    leg = mq.find(lambda t, m: t == base + "/AllJsonData", t_cfg)
    check("MQTT AllJsonData (original payload)", leg and json.loads(leg[-1][2])["BatName"] == "Darth Vader", leg[-1:] if leg else None)
    s1 = mq.find(lambda t, m: t == base + "/pack/1/state", t_cfg)
    check("MQTT pack/1/state JSON", s1 and json.loads(s1[-1][2])["address"] == 1)
    disc = mq.find(lambda t, m: t.startswith("homeassistant/sensor/") and t.endswith("/config"), t_cfg)
    ok = len(disc) >= 2 * (15 + 16 + 4)
    if disc:
        cfg = json.loads(disc[0][2])
        ok = ok and cfg["state_topic"].startswith(base + "/pack/") and cfg["availability_topic"] == base + "/status"
    check("Home Assistant discovery for both packs", ok, len(disc))
    n0 = len(mq.find(lambda t, m: t == base + "/pack/0/state", t_cfg))
    time.sleep(5)
    n1 = len(mq.find(lambda t, m: t == base + "/pack/0/state", t_cfg))
    check("telemetry published about every second", n1 - n0 >= 3, n1 - n0)

    # ---- shutdown via MQTT ------------------------------------------------------------------
    mq.publish(base + "/pack/1/shutdown", "true")
    ok = wait_for(lambda: "SHUTDOWN request for pack 1: INFO=010400" in sim_log(a.out), 10)
    check("MQTT pack/1/shutdown reaches BMS 1", ok, sim_log(a.out)[-500:])
    mq.publish(base + "/ShutDown", "on")
    ok = wait_for(lambda: sim_log(a.out).count("SHUTDOWN request for pack 0") >= 2, 10)
    check("legacy MQTT ShutDown topic (payload 'on')", ok)
    mq.publish(base + "/ShutDown", "false")
    time.sleep(3)
    check("payload 'false' ignored", sim_log(a.out).count("SHUTDOWN request for pack 0") == 2)
    st, r = jget("/api/config", data={"remote_shutdown": "0"})
    check("disable remote shutdown (no restart)", st == 200 and r.get("restart") is False, r)
    t0 = time.time()
    mq.publish(base + "/pack/1/shutdown", "true")
    ok = wait_for(lambda: mq.find(lambda t, m: t == base + "/Info" and "refused" in m, t0), 10)
    check("MQTT shutdown refused while disabled", ok and sim_log(a.out).count("SHUTDOWN request for pack 1") == 1)
    jget("/api/config", data={"remote_shutdown": "1"})

    # ---- admin password ---------------------------------------------------------------------
    st, r = jget("/api/config", data={"admin_pass": "s3cret-pass"})
    check("set admin password (no restart)", st == 200 and r.get("restart") is False, r)
    st, _, hdr = http("/api/config", data={"name": "x"})
    check("unauthenticated POST -> 401 + WWW-Authenticate", st == 401 and "Basic" in hdr.get("WWW-Authenticate", ""), st)
    st, _, _ = http("/api/config", data={"name": "x"}, auth="admin:wrong")
    check("wrong password -> 401", st == 401)
    st, _ = jget("/api/config", data={"name": "Darth Vader"}, auth="admin:s3cret-pass")
    check("correct password accepted", st == 200)
    st, _ = jget("/api/data")
    check("read-only data stays public", st == 200)
    st, cfg = jget("/api/config", auth="admin:s3cret-pass")
    check("config GET never returns secrets", st == 200 and "admin_pass" not in cfg and cfg["admin_pass_set"] is True, cfg)

    # ---- OTA ----------------------------------------------------------------------------------
    st, r = jget("/api/ota", raw=b"\x00" * 4096, ctype="application/octet-stream", auth="admin:s3cret-pass")
    check("OTA rejects non-firmware upload", st == 400 and "not an ESP firmware" in r.get("error", ""), r)
    fw = open("/project/firmware/build-qemu/seplos_controller.bin", "rb").read()
    mark = len(uart(a.out))
    st, r = jget("/api/ota", raw=fw, ctype="application/octet-stream", auth="admin:s3cret-pass", timeout=300)
    check("OTA upload accepted", st == 200 and r.get("restart"), r)
    ok = wait_for(lambda: "new firmware confirmed" in uart(a.out)[mark:], 120)
    check("new image boots from ota_1 (0x200000) and is confirmed (rollback cancelled)", ok and "0x200000" in uart(a.out)[mark:], uart(a.out)[mark:][-1200:])
    check("settings survive OTA", wait_for(lambda: jget("/api/data")[1]["device"]["name"] == "Darth Vader", 30))

    st, _, _ = http("/does-not-exist")
    check("unknown URL -> 404 when not in setup mode", st == 404)
    log = uart(a.out)
    check("no crash / panic / watchdog in UART log", not any(x in log for x in ("Guru Meditation", "abort()", "panic", "task_wdt")))
    return finish(a)


def finish(a):
    failed = [n for n, ok in RESULTS if not ok]
    print(f"\n{len(RESULTS) - len(failed)}/{len(RESULTS)} checks passed")
    for n in failed:
        print("  failed:", n)
    return 1 if failed or not RESULTS else 0


if __name__ == "__main__":
    sys.exit(main())
