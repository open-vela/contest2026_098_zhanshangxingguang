# GC2145 Phase 1 实施计划：DVP 出图

> 前置条件：Phase 0 已通过，实板 `camera id` 读到 `0x2145`  
> 目标：从 GC2145 采集一帧图像并落到 PSRAM，再显示或导出验证  
> 适用工程：`contest2026_098_zhanshangxingguang`

## 1. 目标与非目标

### 1.1 本阶段目标

按风险从低到高分四步：

1. **单帧 dump**：采一帧 RGB565 或 YUV422 到 PSRAM，通过串口 hexdump 或写入文件验证数据非全零、同步正常
2. **连续采集**：VSYNC 中断驱动的双缓冲连续采集，能统计帧率
3. **上屏显示**：缩放后显示到 160x160 GC9D01
4. **可选 JPEG**：使用硬件 JPEG 编码器压缩

### 1.2 明确不做

- H.264 编码
- 摄像头参数动态调节（AE/AWB 手动干预）
- 多 sensor 支持
- V4L2 完整框架对接（可作为后续阶段）

## 2. 现有基础

### 2.1 可复用的 Apache-2.0 参考

ARMino 路径 `armino/bk_avdk_smp/ap/`：

| 文件 | 行数 | 用途 |
|---|---|---|
| `components/bk_dvp/src/bk_dvp.c` | 1828 | DVP 驱动主体，含 DMA 配置、YUV/JPEG 模式切换 |
| `components/bk_dvp/src/dvp_common.c` | 141 | MCLK 与数据引脚 mux，Phase 0 已参考 |
| `components/bk_peripheral/src/dvp/dvp_gc2145.c` | 1797 | GC2145 完整初始化表与分辨率配置 |

关键接口：

```c
bk_err_t bk_dvp_open(camera_handle_t *handle, bk_dvp_config_t *cfg,
                     const bk_dvp_callback_t *cb, uint8_t *encode_buffer);
bk_err_t bk_dvp_close(camera_handle_t handle);
bk_err_t bk_dvp_sensor_write_register(camera_handle_t handle,
                                      dvp_sensor_reg_val_t *reg_val);
```

内部实现可参考：
- `dvp_camera_dma_config()` — DMA 通道配置
- `dvp_camera_yuv_buf_config_init()` — YUV 缓冲初始化
- `dvp_camera_yuv_mode()` / `dvp_camera_jpeg_mode()` — 模式设置

### 2.2 NuttX 侧现状

- `CONFIG_BK7258_PSRAM=y` 已启用
- `nuttx/arch/arm/src/bk7258/bk7258_psram.c` 提供 `bk7258_psram_init()`
- PSRAM 数据窗口 `0x60000000`，支持 APS6408L、APS128XXO、W955D8MKY 三种颗粒
- `CONFIG_EXAMPLES_PSRAM=y`，已有 `psram` 命令可验证读写

### 2.3 已确认的板级信息

来自 `gc2145-phase0-analysis-recovery_zh-cn.md`：

| 信号 | 引脚 |
|---|---|
| MCLK | P27 |
| RST | P28（active-low） |
| PWR_CTL | P49（active-high） |
| SCCB SCL/SDA | P42 / P43 |
| PCLK | P29 |
| HSYNC | P30 |
| VSYNC | P31 |
| D0–D7 | P32–P39 |

GC2145 输出 DVDD 1.8V，AVDD 2.8V，均由 P49 使能。

## 3. 关键设计决策

### 3.1 数据格式选择

优先 **RGB565**，理由：

- datasheet 4.23 节明确 CIS 支持 RGB565 和 YCbCr 4:2:2
- RGB565 可直接送 GC9D01 显示，不需要色彩空间转换
- 单帧 640x480 RGB565 = 614400 字节，PSRAM 8MB 足够放双缓冲

若后续要用硬件 JPEG，再切到 YUV422，因为 JPEG 编码器输入是 YUV。

### 3.2 分辨率选择

第一步用 **640x480**，理由：

- 是 `dvp_gc2145.c` 的默认分辨率，初始化表最成熟
- 便于和 ARMino 参考行为对比

上屏时再缩放到 160x160。若带宽或内存吃紧，可退到 480x320。

### 3.3 缓冲策略

```
PSRAM 0x60000000 起
  frame_buf[0]  614400 B   (640x480 RGB565)
  frame_buf[1]  614400 B
```

单帧 dump 阶段只用 `frame_buf[0]`。连续采集阶段做 ping-pong，VSYNC 中断切换。

### 3.4 引脚保护互斥

Phase 0 为保护摄像头，`lcdtest` 的 8 个 GPIO 循环会跳过 P29–P39。Phase 1 需要把这些引脚配成 DVP 功能，两者必须互斥：

- 保留 `lcdtest_is_dvp_reserved_pin()` 不变，继续跳过 P29–P39
- DVP 初始化时独占这些引脚，并在 `camera close` 时恢复
- 不允许 DVP 打开期间运行 `lcdtest` 的扫描类命令，通过运行时标志拒绝

## 4. 实施步骤

### 步骤 1：DVP 数据引脚 mux（低风险）

新增 `dvp_io_config()` / `dvp_io_deconfig()`，参考 ARMino `dvp_camera_io_init()`。

对 P29–P39 逐个：
1. 保存原 CFG 与共享 function register 的 4-bit 字段
2. 关闭 output enable、input enable、pull enable
3. 设置 function select 为对应 CIS 功能
4. 开启 second function

引脚到功能的映射需从 `gpio_map.h` 查 P29–P39 复用数组的下标，方法与 Phase 0 确认 P27 = 1 相同。

**验收**：新增 `camera io` 子命令，只做 mux 再立刻恢复，读回寄存器确认字段正确。不接摄像头也能测。

### 步骤 2：GC2145 初始化（中风险）

扩展 SCCB 为支持写寄存器，然后移植初始化表。

```c
static int sccb_write_reg(uint8_t reg, uint8_t val);
static int gc2145_write_table(const gc2145_reg_t *tbl, size_t n);
```

初始化表来源必须是 ARMino `dvp_gc2145.c`（Apache-2.0），保留出处注释和 SPDX 头。

**许可证红线**：禁止使用 Linux `drivers/media/i2c/gc2145.c`，包括只复制寄存器表。

**验收**：新增 `camera init` 子命令，执行初始化后回读若干关键寄存器确认写入生效。此时还没有 DVP 采集，sensor 会自己输出信号，可用示波器测 P29 PCLK 和 P31 VSYNC 是否有波形。这是很好的中间验证点。

### 步骤 3：PSRAM 帧缓冲（低风险）

```c
static int camera_framebuf_alloc(void);
```

调用 `bk7258_psram_init()`，在 `0x60000000` 窗口分配两块 614400 字节缓冲，做写入读回校验。

**验收**：新增 `camera buf` 子命令，分配后写入测试图案再读回比对。可先用现有 `psram` 命令确认 PSRAM 本身正常。

### 步骤 4：DVP 控制器与 DMA（高风险）

这是最复杂的一步，参考 ARMino `dvp_camera_dma_config()` 和 `dvp_camera_yuv_buf_config_init()`。

需要处理：
- CIS 控制器寄存器：分辨率、数据格式、PCLK/HSYNC/VSYNC 极性
- DMA 通道申请与配置，目标地址指向 PSRAM
- VSYNC 中断注册，帧完成回调
- 错误处理：DMA 超时、帧不完整

datasheet 4.23 节提到 CIS 支持 programmable polarity，极性配置错误是常见故障点，需要能通过配置项调整以便调试。

**验收**：新增 `camera grab` 子命令，采一帧后 hexdump 前 256 字节。判据：
- 数据不是全 0 或全 0xFF
- 遮住镜头再采一次，数据应明显变暗
- 对着亮处采，数据应变亮

### 步骤 5：连续采集（中风险）

双缓冲 ping-pong，VSYNC 中断切换，统计帧率。

**验收**：`camera stream 100` 采 100 帧，打印实际帧率和丢帧数。

### 步骤 6：上屏显示（中风险）

640x480 RGB565 缩放到 160x160，送 GC9D01。

最简做法是每 4 行取 1 行、每 4 列取 1 列的抽取降采样，先跑通再考虑质量。BK7258 有硬件 Scaling 模块，可作为后续优化。

**验收**：`camera preview` 在左屏连续显示画面。

### 步骤 7：JPEG（可选）

切 YUV422 输入，使用硬件 JPEG 编码器，输出写 SD NAND 或串口导出。

## 5. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| P29–P39 与 LCD 冲突 | 显示异常或摄像头无数据 | 运行时互斥标志，DVP 打开期间拒绝 lcdtest 扫描 |
| PCLK/HSYNC/VSYNC 极性错误 | 图像撕裂、全黑、行错位 | 极性做成 Kconfig 可配，便于逐项试 |
| PSRAM 带宽不足 | 丢帧 | 先降到 480x320 验证，再逐步提升 |
| DMA 配置错误 | 数据错位或 hardfault | 先做单帧单缓冲，确认后再上双缓冲 |
| 初始化表移植出错 | sensor 不出图 | 分段写入，每段后回读校验 |
| 误用 GPL 代码 | 无法开源与参赛 | 只用 ARMino/Zephyr Apache-2.0 源，保留出处 |

## 6. 每步的安全约束

沿用 Phase 0 的原则：

- 任何新增 GPIO 或寄存器操作前，先做配置校验
- 所有被修改的引脚与共享寄存器字段都要保存并按字段恢复
- 每个子命令结束后恢复调用前状态
- 新增引脚必须加入冲突检查表
- 失败路径统一 cleanup

新增的 DVP 数据引脚 P29–P39 需要从 camera 冲突表中移除「保留范围」判定，改为「DVP 专用」，但要保留对 LCD、UART、SWD 的冲突检查。

## 7. 命令设计

分步验证，每步一个子命令，便于隔离问题：

```
camera id                读 sensor ID              (Phase 0 已完成)
camera io                DVP 引脚 mux 自测
camera init              执行 GC2145 初始化
camera buf               PSRAM 帧缓冲自测
camera grab              采单帧并 hexdump
camera stream <n>        连续采 n 帧，报帧率
camera preview           上屏预览
camera stop              停止并恢复所有状态
```

## 8. 建议的实施顺序

第一轮先做步骤 1、2、3，这三步风险低且可独立验证，完成后能确认引脚、sensor 初始化和内存都正常。

第二轮做步骤 4，这是核心难点，建议单独一轮完成并充分验证。

第三轮做步骤 5、6。

步骤 7 视时间决定。

## 9. 构建与烧录

流程与 Phase 0 相同，注意必须 repack：

```bash
cd /home/zhangyan68/miwear-main/vendor/openvela
./build.sh vendor/openvela/boards/contest2026_098_board/configs/nsh --cmake -j$(nproc)
python3 vendor/beken/boards/bk7258/bk7258-devkit/tools/repack.py \
  --nuttx-bin cmake_out/contest2026_098_board_nsh/nuttx.bin
```

烧录 `bk_repack_work/all-app-nuttx.bin`，不要烧 `nuttx.bin`。

## 10. 内存预算

当前占用：Flash 217796 B / 1280 KB = 16.62%，SRAM 38608 B / 336 KB = 11.22%。

Phase 1 新增预估：
- GC2145 初始化表：约 2–4 KB Flash
- DVP 驱动代码：约 8–15 KB Flash
- 帧缓冲：PSRAM，不占 SRAM
- DMA 描述符与控制块：约 1–2 KB SRAM

Flash 和 SRAM 都有充足余量。

## 11. 遗留事项

在开始 Phase 1 之前建议先处理：

1. 把 Phase 0 的 untracked 文件提交：`app/camera/`、`bk7258_camera.c/.h`
2. 可选：向 FAE 确认 `cksel` 编码表，用于文档完整性
