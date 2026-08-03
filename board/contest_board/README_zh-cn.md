# BK7258 DevKit — openvela / NuttX 新硬件平台适配

> 2026 首届 openvela AI 硬件开发者大赛「新硬件平台适配」赛道
> 目标板:博通集成 **BK7258 DevKit**(双核 480MHz Armv8-M STAR-MC1,Wi-Fi AI-SoC,双 GC9D01 圆屏)

## 成果概述

**openvela/NuttX 已在 BK7258 DevKit 上跑通完整的双向交互式 NSH 控制台。**

- 从零建立 BK7258 芯片层(`nuttx/arch/arm/src/bk7258/`)与板级(本目录),内核原本无任何 BK7258 支持。
- NuttX 作为**主核 CPU0** 运行,通过板载 CH340(UART0@115200)提供 NSH,输入(RX)、输出(TX)、命令解析执行全部正常。
- 实测:`help` 打印完整命令表、`uname -a`、`free`、`ps`、`date`、`mb/mh/mw` 等命令均可用。

```
NuttX 0.0.0 dd92bcf4257-dirty <date> arm bk7258-devkit
nsh> help / uname -a / free / ps / date   # 全部正常
```

> 完整的从 0 到通关的调试分析过程(方法、命令、寄存器、踩坑与结论)见
> [`DEBUG_JOURNAL_zh-cn.md`](./DEBUG_JOURNAL_zh-cn.md)(含带颜色标记的「修改记录」)。

## 关键架构决策:NuttX 跑在 CPU0

BK7258 是双核:**CP 核(CPU1)= 主核**(WiFi/BT/RF),**AP 核(CPU0)= 应用核**。
官方文档确认 **CPU1 没有直连的日志 UART**(走 mailbox),因此:

- **方向 A(采用)**:让 NuttX 作为 CPU0 运行,**替换原厂 ARMINO 的 CP app**,直接拥有 UART0(CH340),
  拿到可用的串口控制台。保留原厂 bootloader,app1(原厂 AP)仅占位保持分区完整。
- 拒绝的方案:CPU1 + mailbox(复杂)、CPU1 + UART2(引脚未引出)。

## 内存布局(CPU0 / app 分区)

逆向自原厂 CP app(`build/.../app.elf`),NuttX 沿用该布局替换之:

| 区域 | 地址 | 大小 | 说明 |
| ---- | ---- | ---- | ---- |
| Flash XIP(代码) | `0x02010000` | `0x140000` (1280K) | app 分区,flash 偏移 `0x11000` |
| RAM(.data/.bss/heap/stack) | `0x28010000` | `0x54000` (336K) | AP 专属 SRAM,不越界踩 CP |

链接脚本:[`scripts/ld.script`](./scripts/ld.script)。`_vectors@0x02010000`(XIP),`__start@0x02010140`。

## 启动关键点(踩坑后的正确做法)

复位入口 `__start`(`bk7258_start.c`)由 bootloader **跳转**进入(非真复位),顺序至关重要:

1. `cpsid i` 关中断;
2. **先清 `MSPLIM`**(bootloader 残留栈限制值高于我们的 SP,否则异常压栈即 STKOF);
3. **先使能 FPU**(`CPACR`,否则异常压栈做 FPU 惰性保存时 NOCP);
4. 再设 SP(取自向量表 `vector[0]`)、设 `VTOR`。

> 若把设 SP/VTOR 放在清 MSPLIM + 开 FPU 之前,会触发 STKOF+NOCP 双重故障 → LOCKUP、无任何输出。
> 另:**切勿写 DTCM(`0x20000000`)**,CPU0 冷启动其未使能,访问即 fault。

早期还需:关闭 bootloader 启用的看门狗;自己使能 UART0 时钟 + 复用 GPIO11/10 引脚
(bootloader 交接前可能已拆);**不要 soft-reset UART0**(会破坏 bootloader 已工作的 TX 状态导致丢字符)。

BK7258 在 NVIC 之前还有一道 **SYS 级中断聚合器**(`SYS_CPU0_INT_0_31_EN@0x44010080` / `..._32_63@0x44010084`,
每 bit = 中断源号,UART=bit4):外设中断必须在**聚合器 + NVIC 都使能**才会送达 CPU。
这一点在 `bk7258_irq.c` 的 `up_enable_irq`/`up_disable_irq` 里处理,是 RX 中断能触发、NSH 能回显的关键。

## 目录结构

```
bk7258-devkit/
├── README_zh-cn.md            # 本文件
├── DEBUG_JOURNAL_zh-cn.md     # 完整调试全记录 + 带颜色的修改记录
├── Kconfig                    # 板级配置选项
├── CMakeLists.txt
├── .gitignore                 # 排除 tools/bk_repack_work/(运行时产物)
├── configs/nsh/defconfig      # NSH 配置(savedefconfig 规范最小集)
├── include/board.h            # 时钟/内存/引脚定义
├── scripts/ld.script          # CPU0 app 分区链接脚本(FLASH 0x02010000 / RAM 0x28010000)
├── src/
│   ├── bk7258_boardinitialize.c
│   ├── bk7258_bringup.c        # 挂载 /proc(procfs)等板级初始化
│   ├── bk7258_appinit.c
│   ├── bk7258-devkit.h
│   ├── CMakeLists.txt / Make.defs
├── openocd/                    # SWD 调试(pyOCD/OpenOCD + DAP-Link)配置与手册
│   ├── bk7258.cfg
│   └── README_zh-cn.md
└── tools/
    └── repack.py               # 把 nuttx.bin 打包进 app 分区(复用 ARMINO 线性 CRC 打包器)
```

芯片层(在 nuttx 仓):`nuttx/arch/arm/src/bk7258/` 与 `nuttx/arch/arm/include/bk7258/`,
以及在 `nuttx/arch/arm/Kconfig` 注册 `ARCH_CHIP_BK7258`。

## 编译

```bash
cd <openvela-root>
./build.sh vendor/beken/boards/bk7258/bk7258-devkit/configs/nsh/ --cmake -j$(nproc)
```

产物:`cmake_out/bk7258-devkit_nsh/{nuttx, nuttx.bin}`(约 175KB)。工具链用
`prebuilts/gcc/linux-x86_64/arm-none-eabi`(GCC 13)。

> 注:defconfig 采用 `savedefconfig` 规范最小格式(纯 `CONFIG_` 行,无散文注释)。
> openvela 的 CMake 预解析器会逐行读 defconfig,**散文注释会让全量(clean)构建配置失败**,
> 故 defconfig 保持规范格式;说明性内容集中在本 README 与调试日志中。

## 打包与烧录

NuttX 由原厂 bootloader 加载,`nuttx.bin` 需打包进 app 分区并加上 Beken 线性 CRC。
[`tools/repack.py`](./tools/repack.py) 复用 ARMINO 自带打包器,保证 CRC/分区布局与官方一致。

**前置**:需要 ARMINO SDK 已编好一个工程(如 spi_lcd_example)产出
`bootloader.bin` / `app1.bin` / `partitions/bk_package.json` 作为输入。
`repack.py` 顶部的 `ARMINO_ROOT` / `OPENVELA_ROOT` 路径按本机固定,迁移时需修改。

```bash
cd tools && python3 repack.py
# 产出: tools/bk_repack_work/all-app-nuttx.bin  (该目录被 .gitignore 排除)
```

烧录(工具:`BEKEN_BKFIL`):

```bash
sudo fuser -k /dev/ttyUSB0                       # 释放串口
cd <BEKEN_BKFIL 目录>
N=$(ls /dev/ttyUSB* | grep -oE '[0-9]+$' | head -1)
./bk_loader download -p $N -b 1500000 -i <.../tools/bk_repack_work/all-app-nuttx.bin>
# 时序: 出现 "Getting Bus" 点按 RST, 出现 "Erasing" 松手
```

烧完用 picocom 连:`picocom -b 115200 /dev/ttyUSB0`,回车后即见 `nsh>`。

> 已知现象:连上串口后**第一条**命令偶发被插入一个杂散字节
> (如 `nsh: ◆help: command not found`),重敲即恢复。系开端口瞬间的一次性 RX 线路杂散,非缺陷。

## SWD 调试(可选)

真机 bring-up 阶段用 SWD 定位早期 LOCKUP:DAP-Link(GPIO20=SWCLK / GPIO21=SWDIO)+ pyOCD。
配置与操作步骤见 [`openocd/README_zh-cn.md`](./openocd/README_zh-cn.md) 与 [`openocd/bk7258.cfg`](./openocd/bk7258.cfg)。
OpenOCD 0.11 连不上(no MEM-AP),用 pyOCD。故障自旋可选(`CONFIG_BK7258_DEBUG_FAULT_SPIN`,默认关)。

## 芯片参数参考(逆向自 ARMINO SDK)

**内存映射**(Secure 视图;Non-secure = +0x10000000):

| 区域 | 基址 | 大小 |
| ---- | ---- | ---- |
| 共享 SRAM(SRAM0~5) | `0x28000000` | 640 KB |
| DTCM / ITCM | `0x20000000` / `0x00000000` | 每核私有 |
| Flash(XIP) | `0x02000000` | 8 MB |
| PSRAM | `0x60000000` | 8/16 MB |

**外设寄存器基址**:UART0=`0x44820000`,UART1=`0x45830000`,UART2=`0x45840000`,
SYS=`0x44010000`,GPIO=`0x44000400`,LCD_DISP=`0x48060000`,QSPI0/1=`0x46040000`/`0x46060000`。

**UART0 寄存器**(控制台):config@`0x10`(TX_EN bit0 / RX_EN bit1 / data_bits[4:3] / clk_div[23:8]),
fifo_status@`0x18`,fifo_cfg@`0x14`(tx_thr[7:0] / rx_thr[15:8] / rx_stop_detect[17:16]),
fifo_port@`0x1c`,int_enable@`0x20`,int_status@`0x24`。UART clk = 26MHz XTAL,`clk_div=225`→115200。

**Flash 分区**:bootloader(68K) / primary_cp_app(1360K,← NuttX 替换目标)/ primary_ap_app(1156K,占位) / ota / config。

## 后续可做

- 点屏:双 GC9D01 SPI LCD + framebuffer(openvela 自带 `nuttx/drivers/lcd/gc9a01.c` 可作基底)+ LVGL。
- 消除首字符杂散:UART setup 使能前 flush 一次 RX FIFO。
- `repack.py` 路径参数化(env / 自动推导)以提升可移植性。
