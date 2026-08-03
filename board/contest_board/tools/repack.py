#!/usr/bin/env python3
# ============================================================================
# vendor/beken/boards/bk7258/bk7258-devkit/tools/repack.py
#
# Pack the NuttX nuttx.bin into the AP app (app1) of ARMINO's all-app.bin and
# add the Beken 34/32 linear CRC.  Reuses ARMINO's own packer
# (bk_packager_linear_crc) so the CRC/partition layout matches the factory
# exactly.
#
# Prerequisites:
#   1) Build NuttX  -> cmake_out/bk7258-devkit_nsh/nuttx.bin
#   2) Build ARMINO -> build/bk7258/spi_lcd_example/package/tmp/
#      {bootloader,app}.bin and partitions/bk_package.json
#
# Usage:
#   python3 repack.py
# Output:
#   <this dir>/bk_repack_work/all-app-nuttx.bin   (flash this)
#
# NOTE: ARMINO_ROOT / OPENVELA_ROOT below are hard-coded for one machine;
#       adjust them when moving to another environment.
# ============================================================================

import shutil
import sys
from pathlib import Path

# ---- Paths (adjust when moving to another machine) ------------------------
ARMINO_ROOT = Path("/home/zhangyan68/miwear-main/vendor/armino/bk_avdk_smp")
OPENVELA_ROOT = Path("/home/zhangyan68/miwear-main/vendor/openvela")

PROJECT_BUILD = ARMINO_ROOT / "build/bk7258/spi_lcd_example"
PKG_TMP = PROJECT_BUILD / "package/tmp"    # bootloader.bin / app.bin / app1.bin
PART_JSON = PROJECT_BUILD / "partitions/bk_package.json"
BK_PY_LIBS = ARMINO_ROOT / "tools/env_tools/bk_py_libs"

NUTTX_BIN = OPENVELA_ROOT / "cmake_out/bk7258-devkit_nsh/nuttx.bin"

WORKDIR = Path(__file__).resolve().parent / "bk_repack_work"
OUTPUT_NAME = "all-app-nuttx.bin"
# ---------------------------------------------------------------------------


def die(msg):
    print(f"[repack] error: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    # 1) Check inputs
    for p in (NUTTX_BIN, PKG_TMP / "bootloader.bin", PKG_TMP / "app.bin",
              PART_JSON):
        if not p.is_file():
            die(f"missing input: {p}\n   (did you rebuild NuttX / ARMINO?)")

    # 2) Prepare workdir: bootloader.bin + app.bin(CP) + app1.bin(=nuttx) + json
    if WORKDIR.exists():
        shutil.rmtree(WORKDIR)
    WORKDIR.mkdir(parents=True)

    shutil.copy2(PKG_TMP / "bootloader.bin", WORKDIR / "bootloader.bin")
    # CPU0 = our NuttX (replaces the ARMINO CP)
    shutil.copy2(NUTTX_BIN, WORKDIR / "app.bin")
    # Factory AP: no longer started, kept only to preserve the partition
    shutil.copy2(PKG_TMP / "app1.bin", WORKDIR / "app1.bin")
    shutil.copy2(PART_JSON, WORKDIR / "bk_package.json")

    nuttx_size = (WORKDIR / "app.bin").stat().st_size
    print(f"[repack] nuttx.bin -> app.bin (CPU0, {nuttx_size} bytes)")
    print(f"[repack] workdir: {WORKDIR}")

    # 3) Call the ARMINO packer (linear CRC); layout/CRC match the factory
    sys.path.insert(0, str(BK_PY_LIBS))
    try:
        from bk_packager.bk_packager_linear_crc import bk_packager_linear_crc
    except Exception as e:  # noqa: BLE001
        die(f"failed to import bk_packager: {e}\n   check path {BK_PY_LIBS}")

    packer = bk_packager_linear_crc(
        WORKDIR, (WORKDIR / "bk_package.json"), (WORKDIR / OUTPUT_NAME)
    )
    packer.pack()

    out = WORKDIR / OUTPUT_NAME
    if not out.is_file():
        die("packer produced no output file")
    print(f"\n[repack] done -> {out} ({out.stat().st_size} bytes)")
    print("[repack] flash it: bk_loader download -p 0 -b 1500000 -i "
          f"{out}")


if __name__ == "__main__":
    main()
