"""Validate json_dump output (run by run_tests.sh)."""
import json
import sys

lines = sys.stdin.read().splitlines()
assert len(lines) == 5, lines
legacy = json.loads(lines[0])
pack = json.loads(lines[1])
offline = json.loads(lines[2])
offline_legacy = json.loads(lines[3])

# Keys and order of the original firmware v1.0.1.2 (recovered from the binary).
LEGACY_KEYS = ["EspError", "BatName", "BMSAddress", "SOC", "RemainCapacity", "BatteryVoltage",
               "ChargeDischargeCurrent", "CellMinV", "CellMinIdx", "CellMaxV", "CellMaxIdx",
               "CellDiffMv", "CellCount", "CellVoltages", "TempCount", "Temperatures",
               "BatteryCycles", "SOH", "BatteryCapacity", "RatedCapacity", "BusVoltage"]
assert list(legacy) == LEGACY_KEYS, list(legacy)
assert legacy["BatName"] == 'Darth "Vader"\\ \x01'
assert legacy["SOC"] == 97.3 and "97.30" in lines[0]
assert legacy["ChargeDischargeCurrent"] == -1.87
assert legacy["CellMinV"] == 3.333 and legacy["CellMinIdx"] == 5
assert legacy["CellMaxIdx"] == 4 and legacy["CellDiffMv"] == 8
assert list(legacy["CellVoltages"]) == [f"Cell{i}" for i in range(1, 17)]
assert legacy["CellVoltages"]["Cell16"] == 3.337
assert list(legacy["Temperatures"]) == ["CellTemp1", "CellTemp2", "CellTemp3", "CellTemp4", "AmbientTemp", "PowerTemp"]
assert legacy["Temperatures"]["AmbientTemp"] == 26.6
assert legacy["BusVoltage"] == 53.46 and legacy["SOH"] == 100.0

assert pack["soc"] == 97.3 and pack["current"] == -1.87
assert pack["power"] == -99.9, pack["power"]
assert len(pack["cells"]) == 16 and pack["cell_min_index"] == 6
assert pack["temps"] == {"cells": [22.3, 22.2, 21.7, 21.9], "ambient": 26.6, "power": 23.8}
assert pack["status"]["discharging"] is True and pack["status"]["charge_switch"] is True
assert pack["balancing"] == [6]
assert [a["text"] for a in pack["alarms"]] == ["Cell high voltage alarm", "Residual capacity alarm"]
assert pack["alarm_count"] == 2

assert offline == {"address": 2, "online": False, "has_data": False, "age_ms": 0, "polls_ok": 0,
                   "polls_failed": 0, "error": "no response from BMS"}, offline
assert offline_legacy["EspError"] == "no response from BMS"
assert lines[4] == "-0.05 0.000 -1.9 97.30 -21474836.48", lines[4]
print("json checks passed")
