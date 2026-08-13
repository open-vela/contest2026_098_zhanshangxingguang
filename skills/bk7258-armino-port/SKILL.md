---
name: bk7258-armino-port
description: >
  Port register-level peripheral drivers from the Beken ARMino SDK into
  openvela/NuttX on BK7258. Use when a peripheral has no NuttX driver yet and
  the only reference is ARMino source, when a ported driver silently does
  nothing (no output / empty FIFO / no interrupt), or when deciding whether a
  third-party driver is license-compatible with the contest deliverable.
  Covers the analog-register serial-bus trap, the SYS interrupt aggregator,
  clock/pin-mux prerequisites, and the ARMino->NuttX interface mapping method.
---

# ARMino → openvela/NuttX 寄存器级移植方法

## 适用前提

BK7258 的 **Datasheet 只到功能级，没有寄存器映射**。因此寄存器细节的唯一可靠来源是
**ARMino SDK 源码**（本地：`vendor/armino/bk_avdk_smp/`，许可 **Apache-2.0**）。

三方资料分工：

| 资料 | 提供什么 | 不提供什么 |
|---|---|---|
| `vendor/armino/bk_avdk_smp/` | 寄存器地址/位定义/初始化序列/时序 | — |
| `AIDK_AI玩具开发板_原理图.pdf` | 引脚连接、器件型号、外接/板载 | 寄存器 |
| `BK7258 Datasheet.pdf` | 外设能力、内存映射、格式支持 | **寄存器映射** |

## 步骤 0：许可判定（先做，避免白干）

```bash
head -20 <候选源文件>          # 看 SPDX / 版权头
cat <SDK根>/LICENSE
```

| 来源 | 许可 | 可否移植进本作品 |
|---|---|---|
| Beken ARMino SDK | **Apache-2.0** | ✅ 首选（且已针对本芯片调好） |
| Zephyr | **Apache-2.0** | ✅ 可用（跨平台参考） |
| Espressif esp_cam_sensor 等 | Apache-2.0 | ✅ 可用 |
| **Linux 内核驱动** | **GPL-2.0** | ⛔ **禁止**，会污染 Apache-2.0 作品、上游 PR 必被拒 |

移植时在文件头保留出处，例如：
```c
/* Source: Armino aud_common_driver.c + aud_adc_driver.c + sys_ll.h
 * (Beken, Apache-2.0)
 */
```

## 步骤 1：在 ARMino 里定位参考

```bash
cd vendor/armino/bk_avdk_smp
# 外设驱动
ls ap/components/bk_peripheral/src/{dvp,lcd,tp}/
# 可运行示例（含 CLI 验证命令，价值极高）
ls projects/ | grep -iE "dvp|lcd|audio|asr|encoder|jpeg"
# 类型定义与格式枚举
grep -rn "IMAGE_YUV\|PIXEL_FMT\|media_ppi_t" ap/include/components/
```

**技巧**：优先跑通原厂示例（如 `dvp_example`）确认硬件与参数无误，再移植。
这样调试时能区分"硬件/参数问题"与"移植问题"，省大量时间。

## 步骤 2：找 ARMino ↔ NuttX 的接口映射套路

**关键手法**：找**同一器件在两种框架下的实现**，对照即得映射规律，再套用到目标器件。

已验证的对照对：

| 器件 | ARMino 实现 | NuttX 实现 |
|---|---|---|
| OV2640 摄像头 | `ap/components/bk_peripheral/src/dvp/dvp_ov2640.c` | `nuttx/drivers/video/ov2640.c` |
| GC9A01/GC9D01 屏 | `.../lcd/spi/lcd_spi_gc9d01.c` | `nuttx/drivers/lcd/gc9a01.c` |

NuttX 侧常用落点：

| 外设 | NuttX 接口 |
|---|---|
| 屏 | `struct lcd_dev_s`（+ `LCD_FRAMEBUFFER` 接 fb/LVGL） |
| 摄像头 sensor | `struct imgsensor_ops_s`（`nuttx/include/nuttx/video/imgsensor.h`） |
| 摄像头 DVP 控制器 | `struct imgdata_ops_s`（`.../imgdata.h`） |
| 音频 | `struct audio_lowerhalf_s` |
| I2C 传感器 | `struct sensor_lowerhalf_s`（+ uORB） |

## 步骤 3：移植（剥离 RTOS 依赖）

ARMino 驱动通常缠着它自己的 OS/消息/媒体框架。移植时**只取三样**：
1. 寄存器地址与位定义
2. 初始化序列（顺序极重要，勿重排）
3. 时序等待/轮询逻辑

替换掉：ARMino 的 `rtos_*`/`msg_que`/`media_*` → NuttX 的 `nxsig_usleep`/`up_udelay`、
`nxmutex`、workqueue、`syslog`。

自包含寄存器访问（避免拖入 arch 依赖）：
```c
static inline uint32_t xx_getreg(uintptr_t addr)
{
  return *(volatile uint32_t *)addr;
}

static inline void xx_putreg(uint32_t val, uintptr_t addr)
{
  *(volatile uint32_t *)addr = val;
}
```

## 步骤 4：四个必查前提（"代码没错但没反应"基本都在这）

移植后外设无反应时，**按此顺序**查：

### ① 时钟使能
ARMino 的 `init` 里往往有独立的 clock enable。漏掉则寄存器可写但外设不动。

### ② 引脚复用功能号
对照原理图引脚 + ARMino `gpio_map.h` 的 func 编号。
> 注意：bit-bang 场景要把引脚**设为普通 GPIO 输出**，而不是外设复用功能。

### ③ SYS 级中断聚合器（BK7258 特有，极易漏）
**NVIC 之前还有一道门**：
```c
/* SYS_CPU0_INT_0_31_EN @ 0x44010080 —— 必须置位对应源，
 * 否则外设中断永远到不了 CPU（UART0 = bit4）
 */
```
症状：TX 正常、RX 收不到；或 DMA 完成中断永不触发。

### ④ 模拟寄存器的"串行总线写完成轮询"（音频类必踩）
BK7258 的模拟寄存器（`ana_reg*`）**不是普通 MMIO**——写操作要经一条串行 SPI 总线下发，
**每次写后必须轮询完成标志**才能发下一次写。

漏掉的症状：**模拟前端配置完全不生效，ADC FIFO 恒为空**，而寄存器回读似乎"写进去了"。

```c
/* WRONG */
aud_putreg(val, ANA_REG_X);
aud_putreg(val2, ANA_REG_Y);   /* 前一次还没下发完，被丢弃 */

/* RIGHT */
aud_putreg(val, ANA_REG_X);
ana_wait_write_done();          /* 轮询串行总线完成 */
aud_putreg(val2, ANA_REG_Y);
ana_wait_write_done();
```

## 步骤 5：数据通路选型

| 阶段 | 建议 | 理由 |
|---|---|---|
| bring-up | **polling，无中断无 DMA** | 变量最少，先证明外设活着 |
| 功能可用 | 中断 | 降 CPU 占用 |
| 性能优化 | DMA + 双缓冲 | 摄像头帧流/音频连续采集必需 |

缓冲放置：
- 小缓冲（≤100KB）→ 共享 SRAM `0x28000000`
- **大缓冲（帧缓冲）→ PSRAM `0x60000000`**，需先初始化 PSRAM 控制器，
  并设 `CONFIG_MM_REGIONS≥2` + `mm_addregion()`；DMA 写入后注意 **cache 一致性**

> 实测参考：GC2145 640×480 YUYV 单帧 ≈ **614KB**，放不进 336KB 的 AP SRAM，必须用 PSRAM。

## 步骤 6：bit-bang 提速技巧（已验证）

无硬件控制器时先用 bit-bang 打通。加速要点：**缓存 GPIO 配置寄存器值，避免每次读改写**。

```c
typedef struct
{
  uintptr_t addr;    /* GPIO CFG register address */
  uint32_t  base_lo; /* CFG value with OUTPUT bit cleared */
  uint32_t  base_hi; /* CFG value with OUTPUT bit set */
} gpio_cache_t;

/* 置高 = 一次 putreg32(base_hi)，省掉一次读 + 位运算 */
```
> 长期方案仍应换硬件控制器（QSPI/SPI + DMA），bit-bang 难以支撑动画帧率。

## 步骤 7：验证与排障工具

```bash
# 分阶段 NSH 命令（本项目验证有效的模式）
<cmd>            # A=上电/背光  B=初始化  C=最小可见输出
<cmd> go         # 一步到位生产流程
<cmd> scan/pwr   # GPIO 扫描/二分，定位未知使能脚

# 早期崩溃：SWD（用 pyOCD，OpenOCD 0.11 在本芯片找不到 MEM-AP）
bkhalt / faultinfo   # 见 bk7258-nuttx-bringup skill
```

## 提交前

- 英文注释、行宽 ≤78、`checkpatch.sh` 0 error、文件头路径与实际一致
- 保留移植出处与原始版权声明
- **在 `DEBUG_JOURNAL_zh-cn.md` 补一节**：记录根因、踩坑、验证输出
- `git add logs/` 一并提交 AI Coding 日志
