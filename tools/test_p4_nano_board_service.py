#!/usr/bin/env python3
"""Static scope and API checks for the P4-NANO A3.1 board service."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "firmware/components/p4_nano_board"
CPP = (BOARD / "p4_nano_board.cpp").read_text(encoding="utf-8")
HEADER = (BOARD / "include/p4_nano_board/p4_nano_board.hpp").read_text(
    encoding="utf-8"
)
CMAKE = (BOARD / "CMakeLists.txt").read_text(encoding="utf-8")

assert '#include "driver/i2c_master.h"' in CPP
assert '#include "driver/i2c.h"' not in CPP
assert CPP.count("i2c_new_master_bus(") == 1
assert CPP.count("i2c_master_bus_add_device(") == 1
assert CPP.count("i2c_del_master_bus(") == 1
assert CPP.count("i2c_master_bus_rm_device(") == 1
assert "GPIO_NUM_7" in CPP and "GPIO_NUM_8" in CPP
assert "kPaControlGpioNumber = 51" in HEADER
assert all(word not in CPP.lower() for word in ("i2s", "es8311", "mclk", "bclk", "lrck"))
assert all(word not in CMAKE.lower() for word in ("i2s", "es8311", "audio"))
assert "esp_driver_gpio" in CMAKE and "esp_driver_i2c" in CMAKE
for symbol in (
    "shared_i2c_acquire_device",
    "shared_i2c_release_device",
    "shared_i2c_shutdown",
    "pa_service_init",
    "pa_service_enable",
    "pa_service_disable",
    "pa_service_shutdown",
):
    assert symbol in HEADER

print("P4_NANO_BOARD_STATIC_CONTRACT=PASS")
