# BK7258 SWD 调试 (DAP-Link + OpenOCD) 使用指南

无 JTAG、纯串口盲调走到尽头后，用 SWD 把核 halt 住、读 PC 和故障寄存器，直接定位
"CPU1 跑 NuttX 时在第一条 store 前就 fault" 究竟崩在哪。这份文档是**到货即用**的操作手册。

调试器：DAP-Link（USB SWD/JTAG 版，CMSIS-DAP 固件），淘宝约 ¥14.75。

---

## 0. 一句话流程

```
接线(4根) → 插 USB → openocd -f bk7258.cfg → 另开终端 telnet :4444 → bkhalt → faultinfo
```

---

## 1. 接线（DAP-Link → BK7258）

DAP-Link 侧针脚（手册确认）：`TCK/CK`=SWCLK、`TMS/IO`=SWDIO、`nRST`=复位、`GND`、
`3V3/5V`=对外供电、`U_TX/U_RX`=虚拟串口。

| DAP-Link 针脚 | 接到 BK7258 | 芯片位置 | 说明 |
|---|---|---|---|
| **TMS/IO** | SWDIO = **P21** | QFN **pin 84** | 数据线，也可试专用 `SWD`(pin43) 旁串阻 R2 焊盘 |
| **TCK/CK** | SWCLK = **P20** | QFN **pin 83** | 时钟线 |
| **GND** | GND | 任意地焊盘 | 必接，务必共地 |
| **nRST** | CEN = **pin 44** | 复位脚 | **必接**，connect-under-reset 要用 |
| ~~3V3 / 5V~~ | — | — | **不接**，板子自己 USB 供电，两电源顶牛会出事 |

飞线技巧：QFN 细脚难焊，优先找 **P20/P21 走线上的串阻焊盘**（比引脚好焊 10 倍）。
到货后拍照给我核对丝印方向再动烙铁。

> 附带福利：`U_TX→板RXD`、`U_RX→板TXD`、共地，可当**又一路 115200 串口**用。

---

## 2. Linux 端准备

DAP-Link 是标准 CMSIS-DAP（USB HID class-compliant），**Linux 免驱**。厂商手册写
"不支持 Linux" 只是指他们的 Keil 工具链，我们用 OpenOCD，不受影响。

```bash
# 安装 openocd (需 0.11+ 才较好支持 Cortex-M33; 建议 0.12)
sudo apt install openocd
openocd --version

# 插上 DAP-Link, 确认枚举 (应看到 CMSIS-DAP / DAPLink 字样)
lsusb

# 虚拟串口应出现:
ls -l /dev/ttyACM*
```

免 sudo（可选）：把当前用户加入 `plugdev`，或加 udev 规则
`/etc/udev/rules.d/50-cmsis-dap.rules`：

```
# CMSIS-DAP (通用)
ATTRS{product}=="*CMSIS-DAP*", MODE="0666"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0d28", ATTRS{idProduct}=="0204", MODE="0666"
```

改完 `sudo udevadm control --reload && sudo udevadm trigger`。

---

## 3. 启动 OpenOCD

在本目录下：

```bash
cd vendor/beken/boards/bk7258/bk7258-devkit/openocd
openocd -f bk7258.cfg
```

飞线信号差先探活，可强制更低速：

```bash
openocd -f bk7258.cfg -c "adapter speed 200"
```

**成功标志**：看到类似
`Info : SWD DPIDR 0x...` + `Info : bk7258.cpu: hardware has N breakpoints` +
`bk7258.cpu halted due to debug-request`。此时说明 **SWD 没被锁**。

**失败标志**：`Error: Failed to connect ...` / `No DAP found` / DPIDR 读不出 →
见第 5 节排查。

---

## 4. 定位 CPU1 崩溃点（核心目的）

另开一个终端连 OpenOCD 命令行：

```bash
telnet localhost 4444
```

然后按顺序敲（这些是 bk7258.cfg 里预置的辅助命令）：

```
bkhalt          # 复位并停在入口, 打印 PC/SP/LR/xPSR
vt              # 核对 flash 头向量表: 初始 SP 应在 0x28010000~0x28064000, 复位向量 LSB 应为 1
```

想抓"跑起来后崩溃"的现场：

```
reset halt      # 停在入口
resume          # 放它跑 (它会崩到 HardFault)
halt            # 手动停住
faultinfo       # 解码故障寄存器 -> 直接告诉你是取指错/总线错/非法状态/落哪个地址
reg pc          # 崩溃时 PC
reg lr
reg sp
reg xpsr
```

`faultinfo` 会把 CFSR/HFSR/MMFAR/BFAR 逐位翻译成中文，例如：
- `PRECISERR + BFARVALID` → 精确总线错，去看 BFAR 是哪个地址（多半访问了没使能的外设/内存）
- `INVSTATE` → 跳到了非 Thumb 地址（复位向量 LSB 没置 1）
- `UNDEFINSTR` → 取到垃圾指令（XIP 映射/加载地址不对）
- `NOCP` → 碰了 FPU 但没使能 CPACR

用 gdb 配符号看源码级现场：

```bash
# nuttx ELF 在 cmake_out/bk7258-devkit_nsh/nuttx
arm-none-eabi-gdb cmake_out/bk7258-devkit_nsh/nuttx \
    -ex "target extended-remote :3333" \
    -ex "monitor reset halt"
# (gdb) continue  → 崩溃后  bt / info registers / list
```

---

## 5. 连不上时的排查（对照 DAP-Link 手册 + 我们的处境）

| 现象 | 可能原因 | 处理 |
|---|---|---|
| `No DAP found` / DPIDR 读不出 | 接线错/没共地/速度太高 | 查 SWDIO/SWCLK/GND，`adapter speed 100` 再试 |
| 偶尔能连、常掉线 | 飞线太长信号差 | 降速；SWDIO 就近加 10k 上拉；缩短线 |
| **一直连不上，核在跑** | 固件把 P20/P21 复用成 GPIO，SWD 被禁 | **靠 nRST connect-under-reset**（本配置已默认开启）；确认 CEN 已接 |
| reset halt 后仍连不上 | 芯片读保护/安全 bootloader 锁 SWD | 基本无解，走备选（换单核带调试座的板子）|
| halt 住但 PC 是乱值 | 抓到的是 bootloader/CP 核，不是 AP | 用 `vt` 核对；必要时 `dap info` 看有几个 AP |

> **关于 SWD 是否被锁**：这是买这个调试器最大的不确定性。第 3 节启动后能读出 DPIDR
> 就说明没锁，成功一大半；连 connect-under-reset 都进不去，就是被锁，那就果断转备选方案，
> 别在这死磕。

---

## 6. 与主调试文档的关系

- 完整盲调历程见同目录 `DEBUG_JOURNAL_zh-cn.md`。
- 本目录只放 SWD 上手后的工具与操作；SWD 抓到的结论（PC/fault 类型/是否被锁）
  请回填到 `DEBUG_JOURNAL_zh-cn.md` 对应小节。

---

# 完整命令链清单：重编 → repack → 烧录 → setjtagmode → attach

> 目标：让 SWD 够到跑 NuttX 的 ARM 核。当前卡点 `Could not find MEM-AP` 是因为板上固件
> 没打开 `SWD_DEBUG_MODE`、调试口没路由到 ARM 核。下面这条链把它解开。照着一步步来。

**涉及到的固定路径**（迁移机器时改这里）：
- ARMINO：`/home/zhangyan68/miwear-main/vendor/armino/bk_avdk_smp`
- openvela：`/home/zhangyan68/miwear-main/vendor/openvela`
- bk_loader：`/home/zhangyan68/openvela/BEKEN_BKFIL_V2.1.11.8_20240509/`
- repack 脚本：`vendor/beken/boards/bk7258/bk7258-devkit/tools/repack.py`
- 打包产物：`.../tools/bk_repack_work/all-app-nuttx.bin`

## 步骤 1｜重编 ARMINO（带调试开关的新 CP）

CP/AP 的 `SWD_DEBUG_MODE`、`DUMP_ENABLE` 已在 config 里打开（见调试日志 18.9）。
清生成配置后重编，确保读到新开关：

```bash
cd /home/zhangyan68/miwear-main/vendor/armino/bk_avdk_smp
export COMPILER_TOOLCHAIN_PATH=/home/zhangyan68/openvela/gcc-arm-none-eabi-10.3-2021.10/bin
rm -rf build/bk7258/spi_lcd_example          # 关键：不清会用旧缓存配置
make bk7258 PROJECT=spi_lcd_example
# 校验开关已生效（应为 true / false）：
grep -E "SWD_DEBUG_MODE|DUMP_ENABLE|\"JTAG\"" \
     build/bk7258/spi_lcd_example/bk7258/config/sdkconfig.json
```
期望：`SWD_DEBUG_MODE: true`、`DUMP_ENABLE: true`、`JTAG: false`。

## 步骤 2｜编 NuttX（带 fault-spin 的新 AP）

```bash
cd /home/zhangyan68/miwear-main/vendor/openvela
./build.sh vendor/beken/boards/bk7258/bk7258-devkit/configs/nsh/ --cmake -j$(nproc)
# 产出：cmake_out/bk7258-devkit_nsh/nuttx.bin
```

## 步骤 3｜repack：把 nuttx.bin 换进 app1（加 CRC）

```bash
cd /home/zhangyan68/miwear-main/vendor/openvela/vendor/beken/boards/bk7258/bk7258-devkit/tools
python3 repack.py
# 产出：bk_repack_work/all-app-nuttx.bin
# 自检 AP 区头部应是 NuttX 向量表（SP 在 0x28010000~0x28064000，复位向量 LSB=1）：
xxd -s 0x165000 -l 16 bk_repack_work/all-app-nuttx.bin
```

## 步骤 4｜烧录（bk_loader，注意 RST 时序）

```bash
cd /home/zhangyan68/openvela/BEKEN_BKFIL_V2.1.11.8_20240509
IMG=/home/zhangyan68/miwear-main/vendor/openvela/vendor/beken/boards/bk7258/bk7258-devkit/tools/bk_repack_work/all-app-nuttx.bin
N=$(ls /dev/ttyUSB* | grep -oE '[0-9]+$' | head -1)   # CH340 的端口号
./bk_loader download -p $N -b 1500000 -i "$IMG"
```
时序（关键）：
- 看到 `Getting Bus...` 时**点按 1-2 次 RST** 拿总线；
- 一旦出现 `Erasing/Writing %` 就**彻底松手别碰**（再按会复位、传输从头再来）；
- 成功的判据是 `All Finished Successfully`（bk_loader 失败也返回 0，别只看退出码）。

排错：`/dev/ttyUSB*` 不出现 → `sudo apt remove -y brltty` 再拔插 USB；
端口号会变 → 烧录期间**只按 RST、别拔 USB**。

## 步骤 5｜上电（路由已写进 CP 代码，无需敲命令）

> 说明（见调试日志 18.12）：CP 卡在 `cal:E(108)` 到不了命令行，`setjtagmode` 命令敲不了。
> 已改为在 `cp/cp_main.c` 的 `user_app_main()` 里 `bk_set_jtag_mode(1, 0)`，
> CP 一上电就自动把 SWD 路由到 CPU1(AP/NuttX 核)。所以这一步只需上电：

1. 板子插它自己的 USB(CH340)供电，**按一下 RST** 让它启动。
2. 不用敲任何命令，直接进步骤 6 attach。
3. 若步骤 6 报 `Could not find MEM-AP`：改 `cp_main.c` 里 `bk_set_jtag_mode(1, 0)` 的第一个参数——
   `0` = 先验证能连 CP(CPU0，一定在跑)，`2` = 另一个 AP 核；改完重编 CP、repack、重烧。

## 步骤 6｜OpenOCD attach + 看崩溃点

```bash
cd /home/zhangyan68/miwear-main/vendor/openvela/vendor/beken/boards/bk7258/bk7258-devkit/openocd
openocd -f bk7258.cfg -c "adapter speed 200"
```
- 成功：出现 `bk7258.cpu: hardware has N breakpoints` / examine 成功（不再 `Could not find MEM-AP`）。
- 另开终端：
  ```bash
  telnet localhost 4444
  #  halt
  #  faultinfo          # 解码 CFSR/HFSR/MMFAR/BFAR -> 崩溃类型 + 地址
  #  reg pc ; reg lr ; reg sp ; reg xpsr
  ```
- 源码级：
  ```bash
  arm-none-eabi-gdb ../../../../../cmake_out/bk7258-devkit_nsh/nuttx \
      -ex "target extended-remote :3333" -ex "monitor halt" -ex bt
  ```

## 若步骤 6 仍 `Could not find MEM-AP`

1. 三个 cpu 都试过 `setjtagmode` 了吗？换 `cpu1`/`cpu2`。
2. 确认步骤 1 的 `SWD_DEBUG_MODE: true` 且**确实烧进去了**（重烧 all-app-nuttx.bin）。
3. 可能是 DPv2 multidrop 需要 `TARGETSEL`（见调试日志 18.11 备选）——把 openocd 完整输出发出来再定。
4. 用我在 `bk7258.cfg` 里加的 `jtag_core_sel <0|1|2>` 尝试（需先能 attach 才有效）。
