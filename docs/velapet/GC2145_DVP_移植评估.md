# GC2145 + BK7258 DVP 出图 —— 现成实现调研与工作量评估

> 目标：评估「摄像头能出一帧图」这步（M3 里程碑）的可行性与人力。
>
> ## 🎉 重大更新：本地 ARMino AVDK SDK 里有 BK7258 的完整 DVP + GC2145 实现，且为 Apache-2.0
>
> 路径：`vendor/armino/bk_avdk_smp/`。这把原本最大的风险（Part B）**直接消掉了**。
> 详见下方「零、ARMino SDK 实勘」。原结论中"需向原厂索取 DVP 资料"**已不再是阻塞项**。

---

## 零、ARMino SDK 实勘（决定性发现）

### 0.1 许可：Apache-2.0 ✅ 完美兼容
```
vendor/armino/bk_avdk_smp/LICENSE            → Apache License 2.0
ap/components/bk_peripheral/src/dvp/dvp_gc2145.c 文件头：
  // Copyright 2020-2021 Beken
  // Licensed under the Apache License, Version 2.0
```
→ 与 openvela / 大赛要求（Apache-2.0）**完全一致，可合法移植与再分发**，获奖后 PR 到上游无许可障碍（保留版权声明与出处）。

### 0.2 现成的 DVP sensor 驱动（`ap/components/bk_peripheral/src/dvp/`）
| 文件 | 行数 | 说明 |
|---|---|---|
| **`dvp_gc2145.c`** | **1797** | ★ **正是我们这颗摄像头的 BK7258 驱动** |
| `dvp_ov2640.c` | 1254 | 可与 NuttX 的 `ov2640.c` 对照，摸清两边接口映射 |
| `dvp_hm1055.c` | 906 | — |
| `dvp_gc0328c.c` / `dvp_gc0308.c` / `dvp_sc101.c` | 899 / 488 / 604 | 其它 sensor |
| `dvp_sensor_devices.c` | 75 | sensor 注册表 |

### 0.3 关键参数（直接可用，省掉摸索）
- **I2C 地址**：`GC2145_WRITE_ADDRESS 0x78` / `READ 0x79`（即 **7-bit 0x3C**）
- **Chip ID 寄存器**：`0xF0`(HB) / `0xF1`(LB) → 读回 `0x2145`
- **输出格式**：`PIXEL_FMT_YUYV` —— **原始未压缩 YUV422** ✅
- **支持分辨率**：480×320 / 480×480 / **640×480（默认）** / 800×480 / 864×480 / 1280×720 / 1600×1200
- **测试环境**（README）：核心板 `BK7258_QFN88_9X9_V3.2`、**PSRAM 8M/16M**；注明 GC2145 最大 1280×720、输出 YUV422

### 0.4 ✅ rPPG 命门已解除
`dvp_example/README_CN.md` 原文：**"DVP输出的格式是YUV422"**，MJPEG / H.264 是**可选**的额外编码输出（且两种编码不能同时输出）。
`media_types.h` 也定义了 `IMAGE_YUV = (1<<0)` / `IMAGE_RGB = (1<<1)` / `IMAGE_MJPEG` / `IMAGE_H264`。
→ **原始像素通路确实存在且是默认通路**。rPPG 可以拿到未压缩 YUV422，此前担心的"JPEG 有损压缩淹没脉搏信号"**不成立**。
→ 注：YUV422 色度水平 2:1 子采样，但 rPPG 是在 ROI 内做**空间平均**，子采样无实质影响；指尖模式信号更强，完全够用。

### 0.5 顺带解决：屏幕型号确认为 **GC9D01**
`ap/components/bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c`：
```c
const lcd_device_t lcd_device_gc9d01 = {
    .name = "gc9d01",  .type = LCD_TYPE_SPI,
    .width = 160,      .height = 160,
};
// .frame_len = 160 * 160 * CONFIG_LCD_SPI_COLOR_DEPTH_BYTE
```
→ **160×160 SPI 圆屏 = GC9D01**，与方框图（160×160）和原理图（CS/SCL/SDA/DC/RESET 的 SPI 形态）完全吻合。这就回答了"屏型号怎么确认"——**不用看丝印，SDK 里有**。
（`dvp_example` 的 config 里也确有 `CONFIG_LCD_SPI_GC9D01` 与 `CONFIG_DVP_GC2145=y` 开关。）

### 0.6 SDK 里还有大量可复用示例（`projects/`）
与我们路线直接相关的：
- **`dvp_example`** —— DVP 出图 + CLI 控制 + **如何适配新 sensor 的范例**
- **`spi_lcd_example` / `qspi_lcd_example` / `mcu_lcd_example`** —— 屏驱动（我们用 SPI 那条）
- **`lvgl`** —— LVGL 集成
- **`asr_service_example` / `voice_service_example` / `doorbell_asr`** —— 语音唤醒/命令词
- `audio_player_example`、`dma2d_example`（2D 加速）、`jpeg_decode_example`、`video_pipeline_example`、`uvc_example`

### 0.7 架构提示：AP–CP 双核
README 指出工程是 **AP-CP 双核架构**，主代码在 `ap/` 下（SDK 顶层也确有 `ap/` 与 `cp/`）。
→ 移植 openvela 时要规划**双核分工与核间通信**（openvela 侧对应 `cpc` / OpenAMP）。这既是工作量，也是可以在答辩里讲的深度。

---

---

## 一、NuttX / openvela 侧：框架已就绪（大利好）

本地代码树 `nuttx/` 已包含完整的摄像头子系统，**不需要从零搭框架**：

| 文件 | 行数 | 作用 |
|---|---|---|
| `include/nuttx/video/imgsensor.h` | 428 | **传感器抽象层**接口（GC2145 实现这个） |
| `include/nuttx/video/imgdata.h` | 158 | **数据接收层**接口（BK7258 DVP 实现这个） |
| `drivers/video/ov2640.c` | 1721 | **DVP 摄像头驱动现成范例**（最贴近我们的参照） |
| `drivers/video/isx012.c` | 3312 | 完整 imgsensor 实现范例（Sony，Spresense 在用） |
| `drivers/video/v4l2_cap.c` | 3878 | V4L2 采集框架，对上层暴露 `/dev/videoN` |
| `drivers/video/video_framebuff.c` | — | 帧缓冲队列管理 |

**架构分工正好对应我们的两块活：**

```
应用(rPPG / 人脸)
      ↓  V4L2 (/dev/videoN)   ← 已现成
   v4l2_cap.c                  ← 已现成
      ↓
 ┌────────────────┬─────────────────────┐
 │ imgsensor      │ imgdata             │
 │ = GC2145 驱动  │ = BK7258 DVP 控制器 │
 │ (I2C 配寄存器) │ (并行数据+DMA收帧)  │
 │ ★ 可移植       │ ★★★ 主要工作量      │
 └────────────────┴─────────────────────┘
```

需实现的接口数量很有限：
- **imgsensor_ops_s**（11 个）：`is_available / init / uninit / get_driver_name / validate_frame_setting / start_capture / stop_capture / get_frame_interval / get_supported_value / get_value / set_value`
- **imgdata_ops_s**（6 个必需 + 可选内存分配）：`init / uninit / set_buf / validate_frame_setting / start_capture / stop_capture`

---

## 二、GC2145 现成驱动实现（含许可评估）

| 来源 | 规模 | 许可 | 可用性 |
|---|---|---|---|
| **Zephyr** `drivers/video/gc2145.c` | **1438 行** | **Apache-2.0** ✅ | **首选参考**。与 openvela 同为 Apache-2.0，**许可完全兼容**，含完整寄存器初始化序列 |
| **Espressif** `esp_cam_sensor/sensors/gc2145` | — | Apache-2.0 ✅ | 明确支持 **GC2145 1600×1200、MIPI & DVP、RGB565 / YCbCr422 / RAW**；ESP32 DVP 场景与我们最像 |
| `espressif/esp32-camera` | — | Apache-2.0 ✅ | DVP 采集参考 |
| `Camemake/GC2145_ESP32_DVP_DRIVER` | — | 需核对 | ESP32 上的 GC2145 DVP 独立示例 |
| Zephyr DTS binding `galaxycore,gc2145` | — | Apache-2.0 | 引脚/复位语义参考（RESETn 低有效） |
| **Linux mainline** `drivers/media/i2c/gc2145.c` | 1484 行 | **GPL-2.0** ⛔ | **禁止抄入本作品**（见下方红线） |

### ⛔ 合规红线（必须遵守）
本作品须为 **Apache-2.0**（大赛要求），获奖后还要 PR 到 openvela 上游走 CI。
- **不得**将 Linux 的 `gc2145.c`（GPL-2.0）代码复制/改写进我们的驱动——这会造成许可污染，上游合并时会被拒，且有法务风险。
- **应当**以 **Zephyr（Apache-2.0）** 或 **esp_cam_sensor（Apache-2.0）** 为参考，或直接依据 GC2145 datasheet 自行实现。
- 保留出处注释与 SPDX 头，便于上游 review。

---

## 三、工作量评估

> 假设：1 名有嵌入式经验的开发者，配合官方 `nuttx-driver-development` skill / `driver-workflow` agent。

### Part A — GC2145 传感器驱动（imgsensor）｜**低风险，2–3 人日**
- 工作：以 **ARMino `dvp_gc2145.c`（1797 行，Apache-2.0，已针对 BK7258 调好）** 为主参考，Zephyr 版作对照；I2C 读写对接 NuttX `i2c_master_s`；填充 `imgsensor_ops_s`；按 `ov2640.c` 的 NuttX 惯例组织。
- **参数已知**：I2C 7-bit **0x3C**；Chip ID 读 `0xF0/0xF1` → `0x2145`；输出 **YUYV**；默认 **640×480**。
- 首个验证点：**读芯片 ID** ——通了就说明供电、MCLK、地址都对。
- 加速技巧：ARMino 的 `dvp_ov2640.c` 与 NuttX 的 `ov2640.c` 是**同一颗 sensor 的两种框架实现**，对照它俩即可快速推出"ARMino → NuttX"的接口映射套路，再套用到 GC2145。

### Part B — BK7258 DVP 控制器（imgdata）｜**风险已大幅下降，4–10 人日**
- 工作：把 ARMino 的 DVP/CIS 底层（时序、DMA、帧中断）**翻译**到 NuttX `imgdata_ops_s` 接口下。
- **关键依赖已到手**：`vendor/armino/bk_avdk_smp` 提供 Apache-2.0 的完整实现 + `dvp_example` 可运行范例。
  → 从"**逆寄存器**"降级为"**接口适配 + 去 RTOS 依赖**"，这是本次评估最大的风险削减。
- 剩余难点：① 剥离 ARMino 的 OS/组件依赖（其消息/媒体框架较重），只取寄存器与 DMA 逻辑；② AP–CP 双核下 DVP 归属与核间数据传递；③ PSRAM 缓冲与 cache 一致性。
- **仍是「技术难度 30 分」的核心得分点**——把厂商 SDK 的能力搬进 openvela 标准 V4L2 框架，正是"新硬件平台适配"要的东西。

### Part C — 板级集成与调试｜**2–5 人日**
- **MCLK**：GC2145 需要主时钟（典型 24MHz），板上 `DVP_MCLK` 由 BK7258 输出 → 要配好时钟输出。
- **电源与复位时序**：DVDD（原理图标 GC2053 1.2V / HM1055 1.5V 两档，**按实装 GC2145 规格核对**）、`DVP_RST`、`PWDNB` 的上电顺序。
- **I2C**：确认 GC2145 挂在哪条 I2C、7 位地址是多少。
- **出图验证**：先 RGB565 低分辨率 → dump 一帧存到 SD NAND，或直接显示到 160×160 屏上；也可用 NuttX 的 `nxcamera` 工具验证。

### 合计（已按 ARMino SDK 到手修正）
| 场景 | 人日 |
|---|---|
| **顺利**（ARMino 逻辑可顺利剥离） | **约 8–11 人日** |
| 受阻（双核/PSRAM cache 问题缠身） | 约 15–18 人日 |
| ~~原估（需自行逆寄存器）~~ | ~~20–25 人日~~ ← **已排除** |

> 另有一条**更快的验证捷径**：先直接在 ARMino 上跑通 `dvp_example`（原厂环境，几小时内可出图），确认**硬件本身没问题、参数正确**；再把它移到 openvela。这样调试时能明确区分"硬件/参数问题"还是"移植问题"，能省大量排查时间。

---

## 四、降低风险的四条策略（重要）

1. **千万不要追求全分辨率。** GC2145 是 2MP（1600×1200），但我们**完全不需要**：
   - 人脸检测：QVGA（320×240）足够
   - **指尖 rPPG：只需要 ROI 区域的平均色值，几十×几十像素即可**
   → 配置**低分辨率 + RGB565**，大幅降低 DVP 带宽、DMA 与内存压力，出图也更容易稳定。这是最有效的风险控制。
2. **rPPG 优先走"指尖模式"。** 手指贴住摄像头 → ROI 固定、不需人脸检测、不怕光照抖动，**对分辨率和帧率要求最低**（30fps 甚至 15fps 可用）。演示成功率最高。
3. **先出图，再谈 AI。** M3 里程碑的验收标准就一句话：**"能把一帧图显示到 160×160 屏上"**。达成后再叠 rPPG / 人脸。
4. **先问原厂要 DVP 参考。** 进群找《开发板_张晓伟》索要 **BK7258 DVP 章节文档 / ARMino DVP 示例代码及其许可说明**。这一步能省下最多时间。

---

## 四·五、Datasheet 实读结论（本地 `BK7258 Datasheet.pdf`，DS-BK7258-E12 V2.1）

> 文件名含不换行空格（U+00A0），命令行用 `ls *7258*Datasheet*.pdf` 定位。

### CIS DVP 接口（§4.23）确认能力
- **8-bit 并行接口** + MCLK / PCLK / HSYNC / VSYNC
- **极性可编程**（PCLK 与同步信号）→ 时序适配灵活，调试余地大
- **硬件 Crop（裁剪）** ← **对 rPPG 是大利好**：可直接在硬件层只取 ROI，数据量骤降
- **支持格式**：YCbCr 4:2:2（YUYV / UYVY / YYUV / UVYY）与 **RGB565**
- 另有 **硬件 JPEG 编码器 + 解码器**、**720p H.264 编码器**

### ⚠️ 一个必须注意的数据通路设计
Datasheet 原文描述：YUV sensor 的输入**直接送入硬件 JPEG 编码器**，编码结果由**专用 DMA 通道写入内存**。

这说明芯片**优化的主通路是 `DVP → JPEG 编码 → 内存（压缩数据）`**（面向门铃/IPC 场景）。对我们有两点影响：

1. **必须确认是否存在"原始像素旁路"**（`DVP → DMA → 内存`，不经 JPEG）。格式列表里明确列了 RGB565，倾向于**支持**原始采集，但需在 ARMino SDK / TRM 中确认。
2. **rPPG 绝对不能走 JPEG 通路。** rPPG 依赖皮肤颜色 **0.1%–1% 量级**的微弱变化，而 JPEG 是**有损压缩 + 色度子采样**，量化噪声与色度抽取会把脉搏信号直接淹没。
   → **结论：rPPG 必须用未压缩的 RGB565 / YUV422 原始帧**，配合硬件 Crop 把 ROI 压到很小。
   → 若最终只有 JPEG 通路可用，退路是"硬件 JPEG 解码回原始帧"（芯片有硬解码器），但**画质损失仍可能影响 rPPG 精度**，属次优方案。

### 内存（对帧缓冲是好消息）
- **SiP PSRAM：本板实测 16MB**（ID `0x8d08` = APS128XXO；2026-08-14 实机确认）
- **640 KB Share SRAM**、16 KB ITCM + 16 KB DTCM、64 KB ROM
- Flash（XIP）：SiP 最高 8MB，外挂最高 16MB
→ **容量不是问题**：QVGA RGB565 仅 ~150KB，双屏 160×160 各 ~50KB，16MB PSRAM 绰绰有余。

> ⚠️ **2026-08-14 修正：容量够 ≠ 可以随便放。** PSRAM 实测（SysTick 修好后的有效数据）：
> **CPU 32 位访问 写 11.3 / 读 8.5 MB/s；逐字节访问 写 4.0–4.3 / 读 3.5 MB/s**
> （无 D-cache、无 burst）。折算成帧时间：640×480 YUYV 一帧 614KB，CPU 读一遍要 **72 ms**
> → **全帧 CPU 处理天花板约 14 fps**；320×240 一帧 154KB 只要 18 ms。
> 因此帧缓冲归属重新划定：
> - **双眼 LCD 帧缓冲留 SRAM**（100/200KB，336K 放得下，高频访问要快）
> - **摄像头帧缓冲放 PSRAM**（614KB，大块低频、DMA 写入）
>
> 且 `mm_malloc` 只保证 8 字节对齐，**DVP DMA 通常要求 32/64 字节对齐** →
> 移植前需先有 `bk7258_psram_memalign()`。详见《最小系统配置核对报告》缺口 A。

### 意外收获：芯片自带电容触摸
Datasheet 列有 **touch sensor（TOUCH），最多 16 个触摸感应 I/O**；原理图引脚名里也确实出现 `TS0–TS15`。
→ 若板上有**空闲/外扩的 TS 引脚**（原理图见"外扩焊点"），**"摸摸头"交互有可能复活**——不依赖屏幕触摸，用芯片的触摸感应做一个金属触点即可。值得排查。

---

## 五、待确认清单

- [x] ~~Datasheet 中 DVP 寄存器章节是否完整~~ → Datasheet 仅到功能级；**但已由 ARMino SDK 源码补齐，不再阻塞**
- [x] ~~帧缓冲内存预算~~ → **实测 16MB PSRAM 已打通并可分配 614KB**（2026-08-14，独立堆方案）；
      SRAM 336K 用于 LCD 帧缓冲
- [x] ~~ARMino DVP 源码可否获取、许可为何~~ → **本地已有，Apache-2.0，可合法移植** ✅
- [x] ~~是否存在 DVP 原始像素旁路~~ → **YUV422 就是默认输出，MJPEG/H.264 才是可选** ✅ rPPG 无碍
- [x] ~~GC2145 的 I2C 地址~~ → **7-bit 0x3C**（SDK 中 write 0x78 / read 0x79）
- [x] ~~屏幕型号~~ → **GC9D01，SPI，160×160**
- [x] ~~本板 PSRAM 是 8MB 还是 16MB~~ → **实测 16MB**（ID `0x8d08`）
- [ ] **新增前置项**：`bk7258_psram_memalign()`（DVP DMA 需 32/64 字节对齐，`mm_malloc` 只给 8 字节）
- [ ] GC2145 模块实际 **DVDD 电压**（原理图标 1.2V/1.5V 两档，按实装核对）
- [ ] 板上是否有空闲/外扩的 **TS（触摸感应）引脚** → 决定"摸摸头"能否用芯片 TOUCH 复活
- [ ] AP–CP **双核分工方案**（DVP 归哪个核、如何与 openvela 的 cpc/OpenAMP 对应）

---

## 六、一句话结论（已按 ARMino SDK 到手更新）

**两边都有现成的：openvela 侧有 `imgsensor/imgdata + V4L2 + ov2640` 框架，Beken 侧有 Apache-2.0 的 `dvp_gc2145.c`（1797 行，专为 BK7258 写好）+ 可跑的 `dvp_example`。这件事从"逆寄存器"降级成"把厂商实现翻译到 NuttX 接口"，预计 8–11 人日。**

**关键风险全部解除**：DVP 默认就输出未压缩 **YUV422**（rPPG 无碍）、许可 **Apache-2.0**（可合法移植）、**PSRAM 实测 16MB 且已打通独立堆、614KB 帧缓冲可分配**（2026-08-14 实机验证）、屏是 **GC9D01 160×160**、GC2145 在 **I2C 0x3C**。

**开工前唯一新增前置项**：`bk7258_psram_memalign()` —— DVP DMA 需 32/64 字节对齐，而 `mm_malloc` 只给 8 字节对齐（实测首块 `0x60000178`）。

**建议的最快路径**：先在 ARMino 原厂环境跑通 `dvp_example` 出图（确认硬件与参数无误）→ 再对照 `dvp_ov2640.c`(ARMino) 与 `ov2640.c`(NuttX) 摸清接口映射套路 → 套用到 GC2145 + DVP，落到 openvela 的 V4L2 框架下。

**唯一法务红线**：Linux 的 `gc2145.c` 是 GPL-2.0，**不可**抄入；用 ARMino（首选，已针对本芯片）或 Zephyr 的 Apache-2.0 版本，并保留版权声明。
