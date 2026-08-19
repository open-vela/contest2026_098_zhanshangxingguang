#!/usr/bin/env python3
"""SWD GPIO helper for BK7258 AON GPIO (base 0x44000400, 4 bytes/pin).

Usage:
  gpio.py read                 # dump all 56 pins
  gpio.py high <p1> <p2> ...    # drive listed pins output-high (0x2)
  gpio.py low  <p1> <p2> ...    # drive listed pins output-low  (0x0)
  gpio.py pwr                   # drive default LCD power set high (skip reserved)
"""
import sys
from pyocd.core.helpers import ConnectHelper

BASE = 0x44000400
# reserved: LCD signals(2-5,25,29), console(10,11), SWD(20,21), motor(7,8), led(38,39)
SKIP = {2, 3, 4, 5, 7, 8, 10, 11, 20, 21, 25, 29, 38, 39}

def addr(n):
    return BASE + 4 * n

def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    cmd = sys.argv[1]
    session = ConnectHelper.session_with_chosen_probe(
        target_override="cortex_m", connect_mode="attach", options={"resume_on_disconnect": False})
    session.open()
    t = session.target
    try:
        if cmd == "read":
            for n in range(56):
                v = t.read32(addr(n))
                flag = ""
                # driven output-high as GPIO: bit6=0, bit3=0, bit1=1
                if (v & 0x4A) == 0x02:
                    flag = "  <-- OUTPUT HIGH (GPIO)"
                print(f"P{n:<2} @{addr(n):#010x} = {v:#010x}{flag}")
        elif cmd in ("high", "low"):
            val = 0x2 if cmd == "high" else 0x0
            pins = [int(x) for x in sys.argv[2:]]
            for n in pins:
                t.write32(addr(n), val)
            print(f"{cmd}: {pins} done")
        elif cmd == "pwr":
            pins = [n for n in range(53) if n not in SKIP]
            for n in pins:
                t.write32(addr(n), 0x2)
            print(f"pwr: drove {len(pins)} pins high (skip {sorted(SKIP)})")
            print("pins:", pins)
        else:
            print("unknown cmd"); 
    finally:
        session.close()

if __name__ == "__main__":
    main()
