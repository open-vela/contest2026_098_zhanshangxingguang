# GC2145 Phase 0 分析与验收记录

> 状态：**Phase 0 已在实板通过**  
> 首次成功读取 ID 日期：2026-08-15  
> 适用工程：`contest2026_098_zhanshangxingguang`  
> 目标板：AIDK AI 玩具开发板（BK7258 QFN88 9x9）

## 1. 结论

`camera id` 已在实板成功读取 GC2145 芯片 ID：

```text
[camera] Config: SDA=43(net=y role=y) SCL=42 RST=28(gpio=y pol=y) PWR=49(net=y pol=y) MCLK=27(conf=y)
[camera] Config OK
[camera] GC2145 ID: 0x2145 (0xF0=0x21 0xF1=0x45)
[camera] Cleanup done — pins restored
```

这条结果同时验证了供电、复位、MCLK 和 SCCB 四条链路。Phase 0 目标达成。

## 2. 文档来源与历史

本文件最初是从 Claude 持久化 JSONL 重建的分析恢复报告，原临时报告已丢失，因此不声称与原文逐字一致。

重建时使用的会话记录：

- 主会话 `b00b72a1-1521-4cf9-bb92-834c7562ef13.jsonl`
- 主会话 `e122743e-7aa1-4e58-806c-d8d48848d9cc.jsonl`
- 上述两个会话下共 7 个子 Agent JSONL

持久化位置：

```text
/home/zhangyan68/.claude/projects/-home-zhangyan68-miwear-main-vendor-openvela/
```

当前版本已根据原理图、datasheet 和实板验证结果全面更新，早期记录的多项「未确认 blocker」均已关闭。

## 3. 板级引脚确认表

依据 `docs/AIDK_AI玩具开发板_原理图.pdf` 与 `docs/BK7258_Datasheet.pdf`，并经实板验证。

| 信号 | MCU 引脚 | 极性 / 说明 | 依据 |
|---|---|---|---|
| SCCB SCL | **P42** | 网络名 `IIC2_SCL`，连接器侧 R38 10K 上拉 | 原理图 MCU 引脚 68 |
| SCCB SDA | **P43** | 网络名 `IIC2_SDA`，连接器侧 R36 10K 上拉 | 原理图 MCU 引脚 67 |
| DVP_MCLK | **P27** | `CIS_MCLK` / `CLK_AUXS_CIS` | 原理图引脚 7 |
| DVP_RST | **P28** | **active-low**，R39 10K 上拉 | 原理图引脚 8 |
| DVP_PWR_CTL | **P49** | **active-high**，R31 100K 下拉默认关断 | 原理图引脚 88 |
| PWDNB | **未接 MCU** | R40 10K 偏置，R41 NC，硬连线 | 原理图 DVP 页 |
| DVP_PCLK | P29 | `CIS_PCLK` | 原理图引脚 9 |
| DVP_HSYNC | P30 | `CIS_HSYNC` | 原理图引脚 10 |
| DVP_VSYNC | P31 | `CIS_VSYNC` | 原理图引脚 11 |
| DVP_D0–D7 | P32–P39 | `CIS_PXD0`–`CIS_PXD7` | 原理图引脚 12–19 |

### 3.1 供电

`DVP_PWR_CTL`（P49）同时使能两路 LDO：

- `U4 ME6211C28M5G-N` → Camera AVDD **2.8V**
- `U5 RS9236-ADJ8YF5` → Camera DVDD，本板 GC2145 档位为 **1.8V**（R28 = 75K）

两路 LDO 的 EN 均为高电平使能，R31 100K 下拉保证默认不上电。因此 `CONFIG_CAMERA_PWR_ACTIVE_HIGH=y`。

不同 sensor 的 DVDD 档位：GC2053 = 1.2V，HM1055 = 1.5V，**GC2145 = 1.8V**。

### 3.2 引脚复用注意事项

- P42/P43 在 datasheet 中是 `I2C1_SCL/SDA`。原理图把摄像头这一路标为 `IIC2`，G-Sensor 那一路标为 `IIC1`，但两者都落在芯片 I2C1 的不同复用引脚上，命名容易误导。
- `I2C1` 还可复用到 P38/P39，但本板 P38/P39 已用于 `DVP_D6/D7`，不可占用。
- BK7258 QFN88 的 GPIO 范围是 **P0–P55**，不是 P0–P47。`DVP_PWR_CTL` = P49 即在此范围内。

## 4. GC2145 身份读取信息

- 7-bit SCCB 地址：`0x3c`
- 8-bit write / read：`0x78` / `0x79`
- ID 高字节寄存器 `0xF0`，期望 `0x21`
- ID 低字节寄存器 `0xF1`，期望 `0x45`
- 组合 ID：`0x2145`

Phase 0 只访问这两个寄存器，不包含任何初始化寄存器表。

## 5. MCLK 实现

### 5.1 配置来源

参考 ARMino（Apache-2.0）`ap/components/bk_dvp/src/dvp_common.c`：

```c
gpio_dev_unmap(GPIO_27);
gpio_dev_map(GPIO_27, GPIO_DEV_CLK_AUXS_CIS);
sys_drv_set_auxs_cis(3, 19);   /* MCLK_24M */
sys_drv_set_cis_auxs_clk_en(1);
```

`ap/middleware/soc/bk7258_ap/soc/gpio_map.h` 中 P27 的复用数组为：

```c
{GPIO_27, {GPIO_DEV_JPEG_MCLK, GPIO_DEV_CLK_AUXS_CIS, ...}, true}
```

`GPIO_DEV_CLK_AUXS_CIS` 位于下标 1，而 `gpio_hal_func_map()` 以数组下标作为 `perial_mode`，因此 P27 的 function select 值为 **1**。datasheet 的 AF1/AF2 从 1 开始编号，寄存器字段从 0 开始，AF2 对应寄存器值 1，两者一致。

`gpio_hal_func_map()` 在设置 perial_mode 后还会关闭 output enable、input enable、pull enable，最后开启 second function。本工程实现已对齐这四个动作。

### 5.2 本工程的启用顺序

1. 关闭 CIS AUXS clock gate，避免切换时产生毛刺
2. 清除 P27 function select 字段（等价 `gpio_dev_unmap`）
3. 设置 P27 function select = 1（等价 `gpio_dev_map`）
4. 单次写入 P27 CFG：关 output enable、关 input enable、关 pull enable、开 second function
5. 设置 `cksel=3`、`ckdiv=19`
6. 开启 CIS AUXS clock gate

所有共享寄存器均为字段级 read-modify-write。

### 5.3 恢复顺序

`mclk_restore_state()` 顺序为：

1. 字段级关闭 gate
2. 恢复 `cksel/ckdiv` 字段
3. 恢复 P27 function select 字段
4. 恢复 P27 GPIO CFG
5. 恢复调用前的 gate 状态

### 5.4 时钟语义

datasheet 中 P27 描述为：

```text
CLK_AUXS_CIS: CIS master clock
 (derived from DCO/APLL/CLK_320M/CLK_480M)
```

若 `cksel=3` 对应 `CLK_480M`，则 `480 / (19 + 1) = 24 MHz`，不存在前级 `/2`。

实板已成功读取 sensor ID，说明该配置产出的时钟可被 GC2145 正常使用。`cksel` 编码表的权威文档确认仍可向 FAE 索取，但已不构成阻塞项。

## 6. 软件实现要点

### 6.1 安全设计

`camera_check_config()` 分四个阶段，全部在任何 MMIO 访问之前完成：

1. 检查 7 个 confirmed sentinel
2. 校验 GPIO 范围（0–55）与唯一性
3. 校验与板级保留引脚冲突：UART0 P10/P11、SWD P20/P21、LCD P2–P7/P22–P25/P45、DVP 保留范围 P29–P39
4. 强制 MCLK 引脚为 P27

任一项不满足即返回错误，不产生任何寄存器写入。

`GPIO = -1` 且对应 confirmed = y 表示「该控制线不存在或硬连线」，安全跳过，例如本板的 PWDNB。

### 6.2 状态保存与恢复

- 通用 GPIO：保存 CFG 全值，以及该引脚在共享 function register 中的 4-bit 字段；恢复时对共享寄存器做字段级 RMW
- MCLK：保存 `cksel/ckdiv` 字段、gate 位、P27 function 字段、P27 CFG
- PWR_CTL、PWDNB、RST、SDA、SCL：均在修改前保存，cleanup 统一恢复

### 6.3 SCCB 实现

- 开漏 bit-bang，START 前检查总线空闲
- SDA 被拉低时最多发 9 个恢复时钟
- 每个时钟沿检查 SCL clock stretch，超时返回 `-ETIMEDOUT`
- 写字节检查 ACK，读最后一字节发 NACK
- STOP 失败会传播到最终返回值，不会把失败掩盖成成功
- 所有失败路径汇入统一 cleanup

### 6.4 LCD 引脚保护

`bk7258_gc9d01.c` 中新增 `lcdtest_is_dvp_reserved_pin()`，对 P29–P39 返回 true，并在以下全部 8 个大范围 GPIO 循环中于写入前调用：

`lcdtest_scan`、`lcdtest_pwr`、`lcdtest_go`、`lcdtest_anim`、`lcdtest_emo`、`lcdtest_hear`、`lcdtest_oeye`、`lcdtest_blink`

左屏 LCD reset 已由 P29 改为 P45。

## 7. 构建与烧录

### 7.1 构建

```bash
cd /home/zhangyan68/miwear-main/vendor/openvela
./build.sh vendor/openvela/boards/contest2026_098_board/configs/nsh --cmake -j$(nproc)
```

注意必须从 `openvela` 根目录执行。从 `vendor` 目录调用的 `build.sh` 是 Android 顶层脚本，不接受 `--cmake`。

当前产物：`nuttx.bin` 217796 字节，Flash 占用 16.62%，SRAM 38608 字节 / 11.22%。

### 7.2 打包

`nuttx.bin` 只是 CPU0 的 app 分区镜像，链接地址为 `FLASH 0x02010000`，**不能直接烧录**。必须先 repack 合入 ARMino 的 `all-app.bin`：

```bash
cd /home/zhangyan68/miwear-main/vendor/openvela
python3 vendor/beken/boards/bk7258/bk7258-devkit/tools/repack.py \
  --nuttx-bin cmake_out/contest2026_098_board_nsh/nuttx.bin
```

产物：

```text
vendor/beken/boards/bk7258/bk7258-devkit/tools/bk_repack_work/all-app-nuttx.bin
```

### 7.3 烧录

```bash
cd /home/zhangyan68/openvela/BEKEN_BKFIL_V2.1.11.8_20240509
./bk_loader download -p $N -b 1500000 \
  -i /home/zhangyan68/miwear-main/vendor/openvela/vendor/beken/boards/bk7258/bk7258-devkit/tools/bk_repack_work/all-app-nuttx.bin
```

**曾经踩过的坑**：直接烧 `nuttx.bin` 会覆盖 bootloader 和 CPU1 固件，导致设备无法启动、串口无输出。恢复方法是重新烧录 repack 后的完整镜像。

### 7.4 串口

控制台为 UART0（P10/P11），波特率 115200。原理图中烧录口与调试扩展口分开，调试扩展口是 UART1，接错看不到日志。

```bash
fuser -k /dev/ttyUSB0; picocom -b 115200 /dev/ttyUSB0
```

## 8. 当前配置

`board/contest_board/configs/nsh/defconfig` 中的摄像头相关项：

```text
CONFIG_EXAMPLES_GC2145_ID=y

CONFIG_CAMERA_GPIO_SCL=42
CONFIG_CAMERA_GPIO_SDA=43
CONFIG_CAMERA_GPIO_RST=28
CONFIG_CAMERA_GPIO_PWR_CTL=49
CONFIG_CAMERA_GPIO_PWDNB=-1
CONFIG_CAMERA_MCLK_GPIO=27

CONFIG_CAMERA_RST_ACTIVE_LOW=y
CONFIG_CAMERA_PWR_ACTIVE_HIGH=y

CONFIG_CAMERA_SCCB_CONFIRMED=y
CONFIG_CAMERA_SDA_SCL_ROLE_CONFIRMED=y
CONFIG_CAMERA_RST_GPIO_CONFIRMED=y
CONFIG_CAMERA_RST_POLARITY_CONFIRMED=y
CONFIG_CAMERA_PWR_CONFIRMED=y
CONFIG_CAMERA_PWR_POLARITY_CONFIRMED=y
CONFIG_CAMERA_MCLK_CONFIRMED=y
```

应用通过 manifest linkfile 接入，使用独立符号避免与上游冲突：

```xml
<linkfile src="app/camera" dest="apps/examples/gc2145_id"/>
```

不得使用 `CONFIG_EXAMPLES_CAMERA`，该符号属于 openvela 上游 `apps/examples/camera`，且依赖 `DRIVERS_VIDEO`。

## 9. 许可证边界

- 本工程新增代码为 Apache-2.0，保留 ASF 头
- MCLK 与 pinmux 实现参考 ARMino（Apache-2.0）
- GC2145 地址与 ID 寄存器属于硬件事实
- **禁止**引入 Linux 内核 GPL-2.0 文件 `drivers/media/i2c/gc2145.c` 的代码或寄存器表，包括仅复制寄存器表

Phase 1 若需要完整初始化表，应以 ARMino `ap/components/bk_peripheral/src/dvp/dvp_gc2145.c`（Apache-2.0）或 Zephyr（Apache-2.0）为参考。

## 10. Phase 0 验收清单

- [x] P29–P39 不再被任何 LCD 全 GPIO 测试循环误驱动
- [x] 板级 GPIO 与极性均有独立 confirmed sentinel
- [x] 未确认配置时在首次 MMIO 前硬失败
- [x] GPIO 范围（0–55）、唯一性、板级冲突校验完整
- [x] MCLK 强制 P27，配置与 ARMino 生产值一致
- [x] MCLK source/divider/gate/pinmux/CFG 完整保存并按字段恢复
- [x] SCCB 与 GPIO 所有失败路径汇入统一 cleanup
- [x] 传感器访问仅限 `0xF0` 与 `0xF1`
- [x] 无完整 sensor init、DVP data mux、DMA、PSRAM 帧缓冲、YUV/JPEG
- [x] clean build 通过，camera 代码零 warning
- [x] 无 GPL-2.0 代码或寄存器表进入工程
- [x] 实板读到 `0x2145`

## 11. 遗留事项

1. `cksel` 编码表的权威文档确认（可选，实板已验证配置有效）
2. `app/camera/`、`bk7258_camera.c/.h` 仍为 untracked，需纳入版本控制
3. Phase 1 出图工作，包括 DVP 数据引脚配置、sensor 初始化、DMA 与帧缓冲

## 12. 排查参考

若后续出现读不到 ID 的情况，按此顺序检查：

1. 烧录的是否为 repack 后的 `all-app-nuttx.bin`
2. 串口是否接在 UART0
3. 万用表确认 P49 拉高后 2.8V 与 1.8V 是否建立
4. 示波器测 P27 是否有 24MHz，探头地线尽量短，就近接模组地
5. PWDNB 的 R40 偏置方向
6. 模组是否插紧，是否确为 GC2145（DVDD 1.8V 档）

常见错误码：

| 输出 | 含义 |
|---|---|
| `BLOCKER: ...` | sentinel 未确认，固件可能不是最新版本 |
| `pin N out of range` | GPIO 超出 0–55 |
| `ID high read fail: -5` | SCCB NACK，检查接线与供电 |
| `SCL stuck low` | 总线被拉死，检查引脚冲突 |
| `ID: 0x0000` | SCCB 通但 sensor 未正常响应，查 MCLK 与 PWDNB |
