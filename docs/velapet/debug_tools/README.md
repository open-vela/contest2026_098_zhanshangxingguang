# VelaPet / BK7258 调试工具留底

M3 点屏调试中沉淀的两个小工具(SWD 定位供电脚、还原原厂固件用)。留档备用。

## 环境
- 调试器:板载 CMSIS-DAP / DAPLink(lsusb 里 `0d28:0204 NXP ARM mbed`)。
- 串口:CH340(`1a86:7523`),NuttX 控制台 = UART0 = GPIO11(TX)/GPIO10(RX)。
- 工具:`pyocd 0.45`、`openocd 0.11` 已装。BK7258 = Cortex-M33,用通用目标 `cortex_m`。

## gpio.py —— 用 SWD 直接读写 AON GPIO(不复位、不打断固件)
AON GPIO 基址 `0x44000400`,每脚 4 字节;CFG 位:bit1=输出值、bit2=输入使能、
bit3=输出使能(**低有效**,0=使能)、bit6=功能选择(0=GPIO)。

```
python3 gpio.py read                 # dump 全部 56 脚,标出"输出高"的
python3 gpio.py high <p1> <p2> ...    # 把这些脚驱动为输出高(写 0x2)
python3 gpio.py low  <p1> <p2> ...    # 驱动为输出低(写 0x0)
python3 gpio.py pwr                   # 驱动默认 LCD 供电脚集(跳过保留脚)
```
底层:`ConnectHelper.session_with_chosen_probe(target_override="cortex_m", connect_mode="attach")`。

**用它定位到的关键脚**(见 ../VelaPet_硬件变更记录.md):
- P52 = LCD 背光/共用 LDO 总闸;P46 等 = 面板逻辑供电(总线式)。
- P20=SWCLK / P21=SWDIO(**别驱动**,会断 SWD)。
- 左眼 LCD:CLK2 CS3 MOSI4 DC5 RST29;右眼:CLK22 CS23 MOSI24 DC7 RST6。
- 马达 P7/P8、红灯 P38、绿灯 P39(共用 LDO 连带)。

反向二分定位法:从"工作态(全拉高+lcdtest 出图)"出发,用 `low` 分批关脚,看屏何时灭 → 缩小到必需脚。

## strip_crc.py —— 剥 BK flash 的 32+2 CRC
`bk_loader read` 出来的整片镜像是"每 32 字节数据 + 2 字节 CRC"的物理格式。
本脚本去掉每 34 字节里的 2 个 CRC 字节,还原逻辑镜像(可反汇编;理论上可 `download` 回写)。
```
python3 strip_crc.py <raw_dump.bin> <logical_out.bin>
```
注意:直接 `bk_loader download` 原始 raw dump 会**双重加 CRC 写坏**;要先剥 CRC。
(实测:整片逻辑镜像 download 末尾 4K 对齐处 CheckCRC 会失败、原厂起不来 —— 还原原厂固件不走这条,
改用静态挖 + SWD 运行时读寄存器更靠谱。)
