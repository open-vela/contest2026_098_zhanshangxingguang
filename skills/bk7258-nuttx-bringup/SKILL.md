---
name: bk7258-nuttx-bringup
description: >
  Build, package, flash and debug openvela/NuttX on the Beken BK7258 DevKit.
  Use when working on the BK7258 board port: building the NSH image,
  repacking it into the CPU0 app partition, flashing over the on-board CH340
  UART, or debugging early boot with SWD. Also captures the hard-won boot
  gotchas (MSPLIM/FPU ordering, no UART soft-reset, the SYS interrupt
  aggregator) so they are not rediscovered.
---

# BK7258 NuttX bring-up

## Board facts

- SoC: BK7258, dual-core Armv8-M STAR-MC1 (Cortex-M33 compatible, FPU).
- NuttX runs on CPU0; console = UART0 @ 0x44820000 (CH340), 115200 8N1.
- Flash XIP app at 0x02010000 (partition offset 0x11000); RAM 0x28010000.
- Chip layer: `nuttx/arch/arm/src/bk7258`; board: `bk7258-devkit`.

## Build

```bash
cd <openvela-root>
./build.sh vendor/beken/boards/bk7258/bk7258-devkit/configs/nsh/ --cmake -j$(nproc)
```

Output: `cmake_out/bk7258-devkit_nsh/{nuttx,nuttx.bin}`.

If a defconfig change makes cmake reconfigure fail, remove
`cmake_out/bk7258-devkit_nsh` and rebuild.

## Repack into the flashable image

```bash
cd vendor/beken/boards/bk7258/bk7258-devkit/tools && python3 repack.py
```

Reuses ARMINO's linear-CRC packer so the CRC/partition layout matches the
factory. Output: `tools/bk_repack_work/all-app-nuttx.bin` (this is flashed).

Requires ARMINO to have produced `bootloader.bin`, `app1.bin`, and
`bk_package.json` first.

## Flash (user terminal)

```bash
sudo fuser -k /dev/ttyUSB0
cd <BEKEN_BKFIL dir>
N=$(ls /dev/ttyUSB* | grep -oE '[0-9]+$' | head -1)
./bk_loader download -p $N -b 1500000 -i <.../bk_repack_work/all-app-nuttx.bin>
```

Timing: tap RST when "Getting Bus" appears; release at "Erasing".

Then connect the console:

```bash
picocom -b 115200 /dev/ttyUSB0   # press Enter -> nsh>
```

## SWD debug (optional, for early LOCKUP)

Use DAP-Link on GPIO20=SWCLK / GPIO21=SWDIO with pyOCD.

> OpenOCD 0.11 fails on this chip (no MEM-AP); use pyOCD instead.

See `boards/.../openocd/{bk7258.cfg, README_zh-cn.md}` for configuration.
Read CFSR/PC after halt to classify the fault.

## Known gotchas (do not rediscover)

1. **`__start` ordering**: clear MSPLIM and enable the FPU **before** setting
   SP/VTOR. A stale MSPLIM + FPU-off causes STKOF+NOCP double fault → LOCKUP
   with no output (measured CFSR=0x00180000).

2. **Never write DTCM** (`0x20000000`) on a CPU0 cold boot: it is not enabled
   and any access faults.

3. **Do NOT soft-reset UART0**: the bootloader already drives it; a soft reset
   breaks TX and drops characters. Only set data bits/baud/TX-enable.

4. **RX needs the SYS interrupt aggregator**: enable the source bit in
   `SYS_CPU0_INT_0_31_EN` (`0x44010080`) in addition to the NVIC, or
   peripheral interrupts never reach the CPU (UART = bit4).

5. **Disable the bootloader watchdog** early or it resets into a boot loop.

## Pre-PR checklist (open-vela/nuttx upstream)

- English comments only; no CJK in source or commit messages.
- Lines ≤ 78 cols; run `tools/checkpatch.sh -f <file>` → 0 error.
- CMakeLists: `cmake-format --check` must pass.
- Commit: English author name + Signed-off-by; sign the CLA with the same
  real (non-noreply) email.
