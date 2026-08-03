# contest_board — Beken BK7258 DevKit (openvela/NuttX board adaptation)

Team 098 (zhanshangxingguang), track: new hardware platform adaptation.

This directory is mapped by the team manifest to
`vendor/openvela/boards/contest2026_098_board` in the openvela build tree.
It contains the board support package for the **Beken BK7258 DevKit**, on
which openvela/NuttX runs as the CPU0 main core with an interactive NSH
console on UART0 (on-board CH340, 115200 8N1).

The matching SoC (chip) support lives in the `nuttx` repository
(`arch/arm/src/bk7258`, `arch/arm/include/bk7258`, and the
`ARCH_CHIP_BK7258` entry in `arch/arm/Kconfig`), submitted as a separate
pull request to `open-vela/nuttx` on the `dev-ai-contest-2026` branch.

## Layout

- `configs/nsh/defconfig` — NSH configuration (`CONFIG_ARCH_BOARD_CUSTOM`,
  pointing at `../vendor/openvela/boards/contest2026_098_board`).
- `include/board.h` — clocking, memory map, pin notes.
- `scripts/ld.script` — CPU0 app linker script (FLASH XIP + RAM).
- `src/` — board init and bring-up (procfs mount, etc.).
- `openocd/` — SWD (DAP-Link / pyOCD) config and notes.
- `tools/repack.py` — pack nuttx.bin into the CPU0 app partition (Beken
  linear CRC) for flashing with `bk_loader`.

## Build

```bash
./build.sh <path-to>/contest2026_098_board/configs/nsh/ --cmake -j$(nproc)
```

## Details

- Full build / repack / flash / SWD-debug instructions and the complete
  bring-up log: see `README_zh-cn.md` and `DEBUG_JOURNAL_zh-cn.md` in this
  directory.
