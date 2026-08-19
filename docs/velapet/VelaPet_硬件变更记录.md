# VelaPet 硬件变更记录

> 记录对开发板/外设的物理改动,及其对软件/驱动/方案的影响。最新在上。

## 2026-08-14 晚 · 系统 tick 从未使能(重大缺陷)+ 主频校准 + S4 收尾 ⭐

### 一句话
做 S4 验证时发现 `fbtest` 吞吐恒为 0,顺着查出**这块板的系统 tick 从开机起就没走过**;
修好后又发现**主频标错 4 倍**(480MHz 实为 120MHz);两个时钟缺陷都修掉,并重测了 PSRAM 带宽,
作废了此前 5 份文档里引用的错误数字。

### 缺陷 1:SysTick 从未使能

`up_timer_initialize()` 原来是:

```c
putreg32(SYSTICK_RELOAD, NVIC_SYSTICK_RELOAD);
up_timer_set_lowerhalf(systick_initialize(true, BOARD_SYSTICK_CLOCK, -1));
```

`CONFIG_TIMER_ARCH` 未开时 `up_timer_set_lowerhalf(lower)` 是**空宏**,
**函数式宏连实参一起丢弃** → `systick_initialize()` 这次调用在预处理阶段就消失了。
结果:只写了 RELOAD 寄存器,SysTick 没使能、没挂处理函数,`g_system_timer` 恒为 0。

- 为什么一直没发现:系统照常启动、NSH 照常可用(控制台是中断驱动的)。
- 症状只有两个:`sleep 3` **永久挂住**;所有耗时测量恒为 0。
- 修法:不依赖 `CONFIG_TIMER_ARCH`,直接挂 SysTick 异常 → `nxsched_process_timer()`;
  并加 24 位 RELOAD 的编译期检查。
- 验证:`arm-none-eabi-objdump -d --disassemble=up_timer_initialize` 核对生成指令
  (`RELOAD=0x124f7f`、`CTRL=7`、`irq_attach(15,...)`)。**源码看着对不等于编译结果对**。

### 缺陷 2:主频标错 4 倍

修好 tick 后 `sleep 3` 要 12s、`sleep 10` 要 40s,比值精确 4.0。
SysTick 用处理器时钟(CTRL.CLKSOURCE=1)→ 真实核心时钟 = 480/4 = **120 MHz**。
`board.h` 原写 `BOARD_CPU_FREQUENCY 480000000 /* DPLL output */`,旁边还留着 `TODO(calibrate)`
—— 480MHz 是 DPLL 输出/最大额定值,不是复位后的核心时钟。改成 120MHz 后 `sleep 10` 实测 10s。
480MHz 保留为 `BOARD_DPLL_FREQUENCY`。

> 方法论:**没有独立时基时,用 `sleep N` + 秒表反推主频**;比值是整数就基本能确认是分频关系。

### 缺陷 3~6:S4 同批修掉的小问题

| 缺陷 | 后果 |
|---|---|
| `bk7258_psram_memalign(0, n)` | `mm_memalign` 的幂判断放行 0 → `DEBUGASSERT(ptr % 0)` 除零 |
| NSH `psram align 0 64` | 应用侧 `ptr % align` 同样除零 |
| `clock_systime_ticks()` 隐式声明 | 按返回 `int` 处理;且是 OS 内部接口,应用侧应改用 `clock_gettime` |
| `arm_boardinitialize()` 无原型 | 隐式 int 声明,类型不检查(实体在板级目录,链接能过) |

> ⚠️ 教训:S4 的功能代码上一轮就写好并提交了,但**从没编译过**——一编译掉出 4 个问题。
> 「提交了」不等于「编过」,更不等于「跑过」。

### PSRAM 带宽重测(作废旧数据)

| 访问方式 | 写 | 读 | 条件 |
|---|---|---|---|
| 32 位字 `psram test 16` | **11.3 MB/s** | **8.5 MB/s** | 16MB 全范围、4194304 words、0 错误、3.30 s |
| 8 位字节 `psram fbtest` | **4.0-4.3 MB/s** | **3.5 MB/s** | 614KB×2、32/64 对齐各一轮 |

- **访问宽度影响巨大**:32 位吞吐是 8 位的 2.4-2.7 倍 → 别逐字节 memcpy。
- 旧记录的 1768/1802 KB/s 是 tick 死掉时测的,**无效**;已同步修正 5 份文档。
- **对 M3 摄像头的新约束**:8.5MB/s 读 → 614KB 全帧 CPU 遍历一次 **72ms**,
  即**全帧处理天花板约 14 fps**。AI 推理应 ① DVP 直接出 320×240(154KB/18ms),
  或 ② 只处理 ROI,或 ③ 降到 5-10fps。**这是设计前提,不是优化项**。

### S4 硬件验收(13 项全过)

`align 32/64` mod=0;`align 0/48` 干净报错不崩;`fbtest` 两轮通过;
`alias`/`width`/`test` 被 `-EBUSY` 堆护栏挡住;4 块 = 64+614+614+614 = 1906KB 与 `heap` 显示吻合;
`freeall` 回落 376B/2 chunks 无碎片;`sleep 10` = 10s;`uptime` 递增。

### 提交与上游 PR

| 仓 | 分支 | 提交 | 状态 |
|---|---|---|---|
| 专属仓 | `feat-psram-s4` | `3635c05` 主频校准 | **PR #9 已合并** ✅ |
| nuttx | `bk7258-psram` | `53bc637` SysTick + `426cfed` memalign 校验 | PR **#344** 待 review |
| nuttx-apps | `bk7258-psram` | `abe826a` clock_gettime + 参数校验 | PR **#116** 待 review |
| vendor_beken | `bk7258-psram` | `f46d057` DEBUG_JOURNAL 第 22 节 | PR **#2** 待 review |

**三个上游 PR 的 CI 全红,但不是我们的问题**(已在 PR 里附证据):失败步骤是
`check cherry-pick pr result`,`REPO_SYNC_CODE=0` 而 `CODE_BASE_CHECK=1` ——
CI 工作区按 **`dev`** 同步(manifest 里 `upstream="dev" dest-branch="dev"`,
日志中 HEAD = `322ad9f11c3` 即 dev tip),而我们的 PR 按专属仓 README 要求
targets `dev-ai-contest-2026`;`git merge-base` 在浅克隆里找不到两条分支 4 月的分叉点
(完整历史下 nuttx 是 `053a3c06c9d` 2026-04-28)→ 判定"无共同基底"退出,**在编译之前**。
故 5 个目标(含 arm64 的 goldfish)在两个仓里以完全相同方式失败。已请组委会确认
应改投 `dev` 还是让 CI 同步 PR 的 base 分支。

> 正式交付物是专属仓 #9,已合并,**参赛评审不受这三个上游 PR 阻塞**。

### 待办
- [ ] 等组委会答复上游 PR 的 base 策略(改投 `dev` 则 rebase 重开)
- [ ] PSRAM 改 DMA 搬运拿真实带宽(当前数字是 CPU 逐个访问的最坏情况)
- [ ] 启用 D-cache 后必须补 invalidate(已记入代码注释)

---

## 2026-08-14 · PSRAM 打通:16MB 可用 + 独立堆(头号阻塞项解除)✅

> 无物理改动;记在此处是因为它**改变了内存资源的事实**与**帧缓冲的归属决策**。

### 结论速览
- **实测容量 = 16MB**(不是 8MB)。`board.h` 原写 `BOARD_PSRAM_SIZE = 8MB` 有误,已修正。
- **16MB 全范围读写零错误**、无地址别名、**8/16/32 位访问全可用**(16 位可用 → **RGB565 帧缓冲安全**)。
- **独立 PSRAM 堆可用**,`614KB`(GC2145 一帧)可成功分配 → **摄像头的内存前提已就绪**。
- 主堆未被污染:`malloc(1024)` 仍返回 SRAM 地址。

### 实测数据(三阶段,均实机验证)
```
S1  psram id     → psram: init OK, ID=0x8d08, size=16384 KB
    psram probe   → data path OK (0x60000000 verified)
S2  psram width  → 32-bit OK / 16-bit OK (RGB565 safe) / 8-bit OK
    psram alias  → no aliasing detected (16 MB usable)
    psram test   → 4194304 words, 0 errors        ← 全 16MB 零错误
                   写 11.3 MB/s、读 8.5 MB/s (32 位访问, 3.30 s)
                   ※ 旧记录的 1768/1802 KB/s 无效: 当时 SysTick 从未使能、
                     计时恒为 0; 2026-08-14 修好后重测
S3  psram heap   → arena 16384 KB, allocated 376 B(堆元数据)
    psram alloc 1024 → 1024 KB @ 0x60000178
    psram alloc 614  → 614 KB @ 0x60100180        ← 摄像头一帧尺寸 OK
    psram heap   → allocated 1638 KB = 1024+614   ← 精确吻合
    psram freeall→ released 2 blocks;heap 回落 376 B、free chunks 1(无碎片)
    verify       → malloc(1024)=0x2801c5a8 (SRAM OK)  ← 主堆未污染
```
芯片 ID → 容量对照(来自 ARMino `psram_hal.h`):`0x8d09`=8MB、**`0x8d08`=16MB**、`0x1c8f`=4MB。

### 🔑 架构决策:帧缓冲留 SRAM,PSRAM 专供摄像头
实测(2026-08-14, SysTick 修复 + 主频校准后): **CPU 32 位访问 写 11.3 / 读 8.5 MB/s**、
**逐字节访问 写 4.0-4.3 / 读 3.5 MB/s**(无 D-cache、无 burst,每次访问吃满延迟)。
关键是折算成帧时间: 双眼 100KB 帧缓冲若放 PSRAM,仅读出来就要 11.8 ms/帧、
占 30fps 预算的 36%; 而 614KB 摄像头帧在 336K SRAM 里根本放不下。
据此重算归属:

| 用途 | 大小 | 归属 | 理由 |
|---|---|---|---|
| 双眼 LCD 单/双缓冲 | 100 / 200 KB | **SRAM** | 高频访问要快;336K 放得下 |
| GC2145 640×480 YUYV 一帧 | 614 KB | **PSRAM** | 大块、低频、DMA 写入 |

⇒ **原计划"把 LCD 帧缓冲迁到 PSRAM"作废**。注意准确表述是"CPU 逐字访问比 SRAM 慢一个量级",
而非"PSRAM 慢"——DMA + burst 的真实带宽会显著更高。
另注: **访问宽度影响巨大**,32 位吞吐是 8 位的 2.4-2.7 倍,能用 32 位就别逐字节 memcpy。

### 为什么用独立堆而不是 mm_addregion(答辩会被问)
没有采用官方基线建议的 `CONFIG_MM_REGIONS=2` + `mm_addregion()` 把 PSRAM 并入主堆,
而是用 `mm_initialize()` 建**独立 PSRAM 堆** + 显式 API
(`bk7258_psram_malloc/calloc/free/meminfo`,类比 ESP32 `heap_caps_malloc(MALLOC_CAP_SPIRAM)`)。
理由就是上面的带宽:并入主堆会让 `malloc()` 把小对象也分到慢速 PSRAM,造成难定位的整体性能退化。
`CONFIG_MM_REGIONS` 保持 1 是**刻意的**,不是漏配。

### bring-up 踩的坑(与音频驱动同源,值得记住)
1. **🔴 模拟寄存器写协议(重犯一次)** —— PSRAM LDO 在 `ana_reg13`(`0x44010134`),是**模拟寄存器**:
   写完必须轮询 `0x440100E8` 对应位;且要**逐 bit 读改写**,整寄存器盲写会破坏同寄存器内其它模拟块。
   ⚠️ 这个坑我们在音频驱动阶段已记进 `bk7258-armino-port` Skill,**但本轮仍然踩了** ——
   说明原措辞只讲了现象和原理、缺少可机械执行的判据。已在 Skill 里补硬规则:
   「凡地址落在 `0x440101xx` 的 `ana_reg*`,一律走 `ana_write()`,禁止裸 putreg」。
2. **漏电源域与时钟门控** —— 必须同时:`pwd_ahbp`(`0x44010040` bit5,清 0 上电)+
   `psram_cken`(`0x44010030` bit19 置 1)。少任一项,控制器寄存器可读写但外设不工作。
3. **漏 psram bypass 位** —— `REG2`(`0x46080008`)bit1,`config_init` 开头就要设。
4. **只识别 ID 不配颗粒 MR 会失败** —— 读到 ID 只证明命令通路(REG9/REGA/REGB)可用,
   数据窗口(`0x60000000`,AXI 通路)要靠颗粒的 MR0/MR4/MR8 配好 latency/drive/burst 才通。
5. **接口契约不能靠注释约束** —— `heap_init()` 注释写"调用方须先调 `psram_init()`",
   结果 app 侧漏调 → `psram heap` 报 "PSRAM not initialized"。**正确做法是被调方内部幂等自保**。
6. **repack 打包旧产物(连踩三次)** —— 构建目录名随 defconfig 路径变化,`repack.py` 硬编码旧路径,
   导致烧进去的是几天前的固件,现象是"新命令在 NSH 里不存在",极易误判成代码没编进去。
   已把 `repack.py` 参数化(命令行/环境变量/自动挑最新 mtime)并加陈旧告警,另加 `build_and_pack.sh`。

### 对软件的影响
- 摄像头(M3)的内存前提就绪:`bk7258_psram_malloc(614*1024)` 可用。
- ⚠️ **待补(S4)**:`mm_malloc` 只保证 8 字节对齐(实测首块 `0x60000178`),而 **DVP DMA 通常要求
  32/64 字节对齐** → 需 `bk7258_psram_memalign()`。不补这一步,摄像头移植时会踩坑。
- ⚠️ **待补**:`alias`/`width` 缺堆护栏。堆元数据在 `0x60000000` 起始处,建堆后跑这两个命令会破坏它
  (`test` 已有 `-EBUSY` 护栏)。**临时规避:测完堆之前不要跑 `psram alias`/`psram width`,要跑先重启。**
- **cache 前提**:当前无 D-cache,不需 clean/invalidate;**一旦启用 D-cache,DMA 写 PSRAM 后必须 invalidate**。

### 待办
- [ ] S4:`bk7258_psram_memalign()` + `psram align/fbtest` 子命令(32/64 字节对齐验证)。
- [ ] 给 `alias`/`width` 补 `-EBUSY` 堆护栏(抽 `psram_check_no_heap()` helper)。
- [ ] 修错误文案:提示里的 `psram_heap_deinit` 函数并不存在(建议改为提示 reboot)。

---

## 2026-08-14 · SWD 处置策略修正:改"接口化"而非"调完割断" ⭐

### 原计划的问题
此前记录的处置是「**外设全调完 → 拆掉 CPU 上的 SWD → 再重焊 M2 麦座**」。这个安排有两个隐患:
1. **不可逆**:SWD 是直接从 CPU 引脚飞线,一旦割断,后面若还需调试,要在 **0.4mm pitch** 的
   引脚上重新飞线,难度和风险都很高。
2. **把 3D 外壳串行化**:会让人误以为"必须等 SWD 拆完才能打壳",而实际上
   **3D 打印的前置只是量尺寸,完全不依赖 SWD**。

### 修正后的策略

**① SWD 改成可插拔接口(而非割断)**
- 在引线末端焊 **1.25mm 座或 2.54mm 排针**,并在板上做**应力释放固定点**。
- 平时**拔掉** → 线不受力、不怕扯断;需要时**插上** → SWD 照常可用。
- 外壳只需一个**可堵的小盖片**,不牺牲外观。
- ⇒ "拆不拆 SWD"从**单向不可逆决定**变成**随时可切换**。

**② 3D 外壳走两轮,与软件调试并行(不串行化)**

| 轮次 | 时机 | SWD 处理 |
|---|---|---|
| 第一版「验证壳」 | 量完尺寸即可(现在就能做) | 留 SWD 出线缝 + 应力释放;只验证尺寸/干涉 |
| 第二版「正式壳」 | SWD 接口化、M2 重焊后 | 出线口改可堵盖片,完整造型 |

大赛发的 **3D 打印券正好 2 张**,与两轮一一对应。

**③ SWD 密集使用期 = 摄像头 DVP + QSPI 提速**
- 🔴 摄像头 DVP + GC2145:DMA + PSRAM 组合,配错易 hard fault/挂死 → 需要 SWD
- 🟡 QSPI 硬件刷新:时序配错可能挂死 → 中等
- 🟢 `psram_memalign` / I2C(SC7A20H、MFRC522) / 应用层:低风险,不依赖 SWD
- ⛔ 寻声收尾:必须先拔 SWD 才能重焊 M2 麦座

⇒ **这两块调完即可拔线、重焊 M2、打正式壳。**

### 待办
- [ ] 给 SWD 引线末端加可插拔接口(1.25mm 座或 2.54 排针)+ 板上应力释放固定点。
- [ ] 量 3D 外壳所需 17 项尺寸(不依赖 SWD,可立即做)。
- [ ] 摄像头 + QSPI 调完 → 拔 SWD → 重焊 M2 → 寻声收尾。

## 2026-08-08 · M3 点屏 bring-up:GPIO 实测硬件地图 + 左眼点亮

### 关键突破
- **左眼 GC9D01 屏点亮成功**:背光亮 + 我们 NuttX 驱动画的 40×40 红块正常显示
  → **自研 bit-bang GC9D01 驱动(SPI/DC/CS 时序、寻址、填色)验证可用**。
- 当前背景"花屏":显存开机未清,init 后补一次清屏即可解决(非驱动 bug)。
- 右眼为第二块屏(黑屏),需单独 CS/init,后续再加。

### 实测 GPIO 硬件地图(用 NuttX `mw` 直接戳 AON GPIO 寄存器 0x44000400+脚×4 得到)
| GPIO | 功能 | 备注 |
|------|------|------|
| P2 / P3 / P4 / P5 | LCD SPI:SCLK / CS / SDA(MOSI) / DC | 与原理图一致 |
| P25 | LCD 背光开关(经 Q3 NPN,高=通) | 阴极侧开关 |
| P29 | LCD RST | |
| **P52** | **LCD 背光/面板电源使能(共用 LDO 总闸)** | ✅ SWD 反向二分确认;拉高→双眼背光亮(+马达震,同一 rail) |
| P7 / P8 | 震动马达 | 拉高即震(累积);对应固件 `MOTOR_LDO_CTRL_GPIO` |
| P38 | 红色指示灯 | 拉高即亮 |
| P39 | 绿色指示灯 | 拉高即亮 |
| P10 / P11 | UART0 控制台 RX / TX(CH340) | 我们 NuttX 控制台,勿动 |
| **右眼 LCD2**:CLK=P22, CS=P23, MOSI=P24, DC=P7, RST=P6 | 第二块 GC9D01,独立一套 SPI(=QSPI0/FL_QSPI + LCD_QSPI_D2/D3) | DC=P7 与马达 PWM 复用;背光/电源与左眼共用 P52。go 已驱动 22/23/24 未崩→非启动flash,安全 |
| P20 / P21 | SWCLK / SWDIO(SWD 调试口) | ⚠️ 任何上电流程务必跳过,驱动它俩会断 SWD |

### 定位方法论(可复用)
- 关键教训:**光靠 GPIO 拉背光脚(P25)不亮 —— 屏阳极那路 LDO 需要另一个"电源使能脚"先拉高**。
  原厂 ARMINO 用 `bk_pm_module_vote_ctrl_external_ldo` 干这事,本质就是拉高一个 GPIO(在 P48~P52)。
- 排查手段:NuttX 里用 `mw <addr>[=<val>]` 直接读写 AON GPIO CFG 寄存器,当场验证引脚受控;
  再写 `lcdtest scan` 累积拉高 GPIO 0~47(跳过控制台/LCD 信号脚)扫外设 → 扫出马达/灯;
  背光电源脚在 48~55 段,补测点亮。
- CFG 位:bit1=输出值、bit3=输出使能(**低有效**,0=使能)、bit6=功能选择(0=GPIO)。

### ✅ 结果:左眼稳定出图(2026-08-08)
- 有效配方:复位 → `lcdtest`(跑一遍 init)→ `lcdtest pwr`(拉高 GPIO 0~52 供电候选脚
  + 再 init + 整屏填黑 + 画红块)→ **左眼屏干净显示"黑底 + 红方块"**。
- init 序列已逐条核对与 ARMINO `lcd_spi_gc9d01.c` 完全一致(含 0x11 退睡眠 +120ms + 0x29 开显示、
  0x3a=0x05 RGB565、CASET/RASET/RAMWR 填色)。→ **自研 GC9D01 bit-bang 驱动确认可用。**
- 副作用:`pwr` 拉高一大堆脚会连带把马达/红绿灯点亮(存在共用 LDO 使能),需收窄到最小供电脚集。

### 可靠配方(已稳定复现)
**冷启动 → `lcdtest` → `lcdtest pwr`** → 左眼干净显示"黑底红块"。
- `pwr` 拉高 41 个脚(0 1 6 9 12~24 26~28 30~... 40~52 等,跳过 LCD 信号/控制台/马达/灯),
  这里面某个(些)脚打开了共用 LDO,面板/背光才通电。
- 单独 `lcdtest`(不 pwr)背光不亮、无内容 → **面板非默认供电,pwr 必需**。

### 供电脚 SWD 反向二分结论(2026-08-08,已查透)
用 SWD(CMSIS-DAP/DAPLink + pyOCD,attach 模式,不复位不打断固件)从"工作态"逐脚改回低,
配合串口 `lcdtest` 重跑,精确定位:
- **P52 = 背光/共用 LDO 总闸**:拉高→双眼背光亮。单独 {P52} 只有背光、无内容。
- **面板内容(能显示)需要 0~46 区间的一大批脚同时高**(≈一条总线,非单个使能);
  {P46,P52} 不够,{0~46 去保留脚 + P52} 才出内容。
- **马达/红绿灯的使能混在 0~46 这批脚里,和内容脚重叠** → 无法用 GPIO 干净地"要内容不要马达"。
- ⚠️ **关键修正:P20=SWCLK、P21=SWDIO**。之前的 `lcdtest pwr` 驱动 0~52 时**没跳过 20/21**,
  把 SWD 调试口打断了(No ACK)。**任何上电流程都必须跳过 P20/P21**,否则丢 SWD、也会加剧飘忽。
- 可靠上电集(实测 SWD 驱动 + lcdtest 出内容,且 SWD 存活):
  **GPIO 0~52 全拉高,跳过 {2,3,4,5,25,29(LCD信号)、10,11(控制台)、20,21(SWD)、7,8,38,39(马达/灯,可选)}**。
- 工具沉淀:`/tmp/gpio.py`(pyOCD 读写 AON GPIO:read/high/low/pwr),`/tmp/strip_crc.py`(剥 BK flash 32+2 CRC)。
- 🔁 **双 init 坑(关键)**:GC9D01 冷上电后**第一遍 init 不生效,必须跑第二遍 RST+init 才吃进去**。
  实测:`go`(单遍 init,即使延时 500ms)黑屏;紧接着**立刻**再敲普通 `lcdtest`(第二遍 init)即出内容
  → 起初以为"与时长无关只需两遍"。**修正:双 init 只隔 50ms(离上电~0.6s)仍黑;根因是时间**——
  能成功的手动流程里,第二遍 init 落在 `go` 跑完(含很慢的 bit-bang 填黑)之后、距上电好几秒。
  即**面板 VCI 上电到内部 POR 完成需数秒,init 必须在其后**。修法:两遍 init 之间 up_mdelay(2000),
  让第二遍 init 落在上电 ~2.5s 后(或首遍 init 前直接等 ~2.5s)。
  ↳ 再修正:`up_mdelay(2000)` 双 init 仍黑;但"`go`(黑)后单独敲 `lcdtest`"稳定出图。
  差异是**第二遍 init 前隔着 go 的整屏填黑(bit-bang 数秒)**。可靠时序 = 
  上电→500ms→背光→init(1)→**整屏填黑**→init(2)→红块(用填黑耗时垫在两遍 init 之间)。
  ❗**最终真因(以上时序推测全部作废)**:`go` **从没把 SPI 脚(SCLK/CS/MOSI/DC=P2/3/4/5)配成输出**。
  `lcdtest` 的 Stage B 在 init 前会 `gpio_set_output(SCLK/CS/MOSI/DC)` 并置空闲电平(CS=1,DC=1,SCLK=0,MOSI=0);
  而 `go` 的 step1 特意跳过 2/3/4/5、之后又没单独配置,导致 init 时 SPI 脚仍是输入态、bit-bang 送不出信号、
  面板收不到 init → 黑屏。所有"能出内容"的情况都恰好跑过 `lcdtest`(它配了 SPI 脚)。
  **修:go 在 init 前补上与 Stage B 相同的 SPI 脚配置。**"双 init/填黑垫时间"皆为被带偏的假象。

### ✅ 步骤2 收官(2026-08-08):单命令 `lcdtest go` 冷启动一步出黑底红块
- 真因修好后(go 补 SPI 脚配置),单条 `lcdtest go` 稳定出图。
- 开机可见"花屏→逐行扫黑→黑底→红块":**正常现象**,是 bit-bang 逐像素填黑很慢 + init 就开显示(0x29)
  先露出随机显存。非 bug,最终画面正确。
- 观感优化 A:把 0x29(开显示)挪到**填黑之后**,开显示时显存已黑 → 不再闪花屏。
- 观感优化 B(后期):bit-bang 慢,换 QSPI 硬件刷新提速。

### 待办
- [x] A:填黑后再开显示,消花屏闪 ✅(单遍 init,开机不闪、干净黑底红块)。
- [x] B:双眼齐亮 ✅(左眼红块、右眼蓝块;右眼用 QSPI0 那组脚 22/23/24/7/6)。
- [x] step4:双眼青色发光萌眼(黑底+青虹膜 0x07FF+白高光 0xFFFF)✅ —— **双眼萌宠脸点亮!**

### 🎉 M3 点屏完成(2026-08-08)
四步全通:①SWD 定位供电脚+硬件地图 ②单命令冷启动一步出图(真因=SPI脚未配置)+消花屏闪
③双眼独立驱动(右眼 QSPI0 组脚 22/23/24/7/6)④双眼青色发光萌眼。
自研 bit-bang GC9D01 驱动 + 双屏 + 静态表情,全程 Claude AI 日志。
后续 polish(非阻塞):QSPI 硬件刷新提速 → 眨眼/转动动画;马达/灯共用 LDO 连带、"兹兹"啸叫;
瞳孔朝向做"看向声源/人脸"。

### 提交状态(2026-08-09)
- nuttx 芯片层 **PR #332**:干净(只剩 SoC 支持),等组委会 review。
- nuttx GC9D01 驱动:备份在 fork 分支 `bk7258-lcd`(不 PR,留底)。
- 专属仓 **PR #3**(board: enable GC9D01 dual-eye LCD)✅ **已合并** 进 `dev-ai-contest-2026`。
- 专属仓 **PR #4**(logs: AI coding session logs)✅ **已合并**(AI 日志硬性要求达成)。
- ✅ B(自包含集成,PR #5 已合并 2026-08-09):走 **Path A** —— 驱动挪到 **board src**
  (`board/contest_board/src/bk7258_gc9d01.*` + 自包含 `bk7258_gpio.h`,CMakeLists/Make.defs
  在 `CONFIG_LCD_GC9D01` 下编译);lcdtest app 放团队仓 `app/lcdtest/`,manifest 加
  `linkfile app/lcdtest → apps/examples/lcdtest`。**作品仓 clean clone + repo sync 即可编出双眼**,
  不依赖任何个人 fork 分支。fork 的 `bk7258-lcd`(arch 版)保留作历史备份。
  提交路线:#3 板级配置 + #4 AI日志 + #5 驱动/app/manifest,均已并入 dev-ai-contest-2026。

### 马达"调试时太吵"根因 + 处置(2026-08-09)
- 根因:**马达与 LCD 背光/面板共用同一条 LDO_3V3、同一使能脚**
  (ARMINO:`MOTOR_LDO_CTRL_GPIO == CONFIG_LDO3V3_CTRL_GPIO`)。
  ⇒ 给 LCD 供电必然给马达那条 rail 供电,**软件层无法把两者分开**;拉低 P7/P8 也止不住
  (LDO 供电路径独立于 P7/P8)。
- 项目**不需要震动马达**(核心=双眼显示+音频+摄像头)。
- 处置:**硬件断开马达**(拔其 2pin 连接器/挑开一脚焊点)= 零风险根治,永久安静,无功能损失。
  临时止震可用 SWD 把 go 拉高的脚全部拉低(`debug_tools/gpio.py low ...`),但复位/重跑 go 会再响。
- ✅ **已执行(2026-08-09):物理割断马达红色正极线** → 马达永久不响,LCD/双眼不受影响
  (LDO_3V3 仍给屏供电,仅马达支路断正极无电流)。项目不使用震动,无损失。
  如日后需要马达:重新接回红色正极线即可。
- [ ] 后期:QSPI 硬件刷新提速;马达/灯连带(共用 LDO)与"兹兹"啸叫,非阻塞。

## 2026-xx-xx · 麦克风:更换为双咪头(双麦寻声硬件就绪)

### 变更内容
- **拔掉原装的那 1 颗麦**(此前只装了 M1/M2 座中的一颗,居中引到两眼正中)。
- **新买 2 颗咪头,已接上设备**(两个座 M1 / M2 都装上了)。
  - **两颗为同款**:**6027 ECM 驻极体咪头**,带 **200mm 双绞线 + 1.25mm 插头**,灵敏度 **-25±3dB**。
    同型号 → 相位/灵敏度一致,满足 TDOA 对两麦匹配的要求 ✅。
  - 实物照片确认:红黑**双绞线**(抗干扰 OK),两颗分别插到板上两个 2pin 麦座;线长 200mm,
    **足以拉开到 ≥60mm** 装到外壳两侧"耳朵"位置。

### 状态翻转:双麦寻声(A 方案)从"被卡"→"硬件就绪"
此前作品描述里写的"实物只装 1 颗麦、双麦寻声需自行补麦",**现已不成立** ——
两颗麦已到位,双麦声源定位(左右寻声)在**硬件层面已具备条件**。

### ⚠️ 必须确认的一件事:两颗麦的实际间距
双麦 TDOA 测向对间距很敏感(声速 343 m/s,采样率 48kHz):

| 麦间距 | @48kHz 分辨力 | 能否测向 |
|---|---|---|
| ~9.4mm(挤在座子旁) | 1.3 采样 | ❌ 不行 |
| 40mm | 5.6 采样 | ⚠️ 勉强 |
| **≥60mm(推荐)** | ≥8.4 采样 | ✅ 可用 |

- **若两颗麦是拉开到 ≥60mm(装在外壳两侧"耳朵"位置)**:双麦寻声可正常做,达成
  "听到声音先转头、再用眼睛确认"的听觉引导视觉叙事。
- **若两颗仍靠得很近(<40mm)**:寻声不可用,需重新走线拉开;此时双麦只当"立体声/降噪"
  用,寻声仍走**摄像头人脸跟踪**(基线方案,零额外硬件)。
- 👉 **行动项**:量一下当前两颗麦拾音孔的实际间距,记在这里:实测 = ____ mm。

### 对软件/驱动的影响
1. **音频采集驱动**:需按**双通道(MIC1 + MIC2)**采集,采样率取 **48kHz**(16kHz 分辨力不足)。
   BK7258 芯片支持 MIC1/MIC2 两路,ARMINO SDK 有现成音频/ADC 采集路径可参考。
2. **前端类型 = ECM(模拟驻极体)**,不是数字 PDM/MEMS:走芯片的**模拟麦输入 + MICBIAS 偏置**
   路径(原理图前端为 ECM 偏置网络)。驱动配置不要按 PDM 走。
3. **寻声算法**:GCC-PHAT + 抛物线亚采样插值,输出"左/中/右"3–5 档粗方位驱动眼球转向,
   不追求精确角度(双麦仅左右轴,存在前后镜像模糊,对本设备无影响)。
4. **两颗麦需同型号**,保证相位/灵敏度一致(TDOA 前提);已整体换新,满足。
5. **接线抗干扰**:麦线保持双绞、远离喇叭线与屏幕 SPI/QSPI 高速排线及 2.4G 天线区;
   注意 ECM 极性(金属外壳极通常为地),对齐 `MIC+/MIC-` 丝印。

### 方案取舍(不变)
- **寻声仍非关键路径**:基线用**摄像头人脸跟踪**驱动"转头看你"(零额外硬件、不依赖光照外);
  双麦寻声作为**进阶增强**(补齐黑暗/视野外场景)。硬件既已就绪,可作为"有余力就点亮"的加分项。
- 语音唤醒/命令词:单/双麦都够用,不受影响。

### 待办
- [ ] 量测并回填两颗麦的实际间距。
- [ ] 若 ≥60mm:排期做双麦采集(48kHz 双通道)+ GCC-PHAT 寻声(进阶,不阻塞移植主线)。
- [ ] 记录实际购买的咪头型号/规格(若非 6027)。

## 2026-08-10 · 寻声 Step 1:BK7258 模拟音频 ADC 双通道采集在 NuttX 上点亮 ✅

### 关键突破
- **双麦(M1=左 / M2=右)模拟 ADC 采集打通**:`lcdtest mic` 采 4096 对 16-bit L/R 样点,
  两路 RMS 非零、随声音变化、且相互独立(按住一颗只掉那一路)。
- 从零在 NuttX 上**寄存器级**实现的自研音频采集驱动 `bk7258_audio.c`(不依赖 NuttX audio 框架、
  不用 DMA/中断,纯 FIFO 轮询),参照 Armino SDK 的寄存器序列移植。**又一项"新硬件平台适配"成果。**
- 实测:安静时左≈667,大声放歌左→1838;按住右麦时右稳定≈37(噪声地板)→ 双通道映射正确、无串扰。

### 硬件/麦间距现实
- 两颗麦接在板上 **M1± / M2±** 差分模拟输入(ECM 驻极体,不是数字 PDM);**走模拟 ADC + MICBIAS**。
- 实测**麦头间距很近(1~2cm)**,双绞引线 20cm(引线长度不影响声学时延,只有麦头间距决定 TDOA)。
  → 1.5cm @48kHz 最大时延≈2 样点,寻声需靠 GCC-PHAT **亚样点插值 + 符号**做**左/中/右粗档**(不追求精确角度)。

### 寄存器 bring-up 序列(自研驱动核心,均直写 putreg32/getreg32)
基址:AUD=`0x47800000`、SYS=`0x44010000`、模拟寄存器 ana_regN=`0x44010100 + N*4`。
1. **音频电源域上电**:`0x44010040`(cpu_power_sleep_wakeup)bit6 `pwd_audp` = **0**(0=上电)+ 2ms 稳定。
2. **音频时钟**:`0x44010030` bit30 `aud_cken`=1(模块时钟);`0x44010020` bit25 `cksel_aud`=**1**(选 APLL)。
3. **APLL(必须,ADC 靠它做工作时钟)**:全走 analog-SPI —— ana_reg5 bit13 `pwdaudpll`=0(上电)、
   ana_reg26=`0x8973CA6F`(cal_val,48k 家族含 16k)、ana_reg25=`0xC2A0AE86`(config)、
   ana_reg25 bit18 `spi_trigger` 脉冲 1→0;AUD_CONFIG bit8 `APLL_SEL`=1。
4. **ADC 软复位**:AUD_CLK_CONTROL(`0x47800008`)bit1 `clk_gate`=0(时钟开)、bit0 `soft_reset`=**1 并保持**
   (1=释放复位/运行;若清回 0 = ADC 被摁在复位,FIFO 恒空)。
5. **模拟前端(analog-SPI 写)**:ana_reg18/19/20/21(mic1)/27(mic2)基值 + enmicbias/enadcbias/enaudbias、
   mic1_en(reg19 bit28)/mic2_en(reg27 bit28)、差分模式(单端使能=0)、mic 复位脉冲。
6. **数字 ADC**:AUD_ADC_CONFIG_0 gain=0dB、HPF1/2 bypass、DIG_MIC_SEL=0(模拟);
   AUD_CONFIG:采样率 16k(bit[1:0]=1)、`ADC_ENABLE`(bit3)=1、`LINEIN_ENABLE`(bit5)=1(模拟输入通路使能,非line-in选择)。
7. **采集**:轮询 AUD_FIFO_STATUS(`0x47800038`)bit14 `ADC_FIFO_EMPTY`,非空则读 AUD_ADC_FIFO_PORT(`0x47800044`)
   32-bit(L=[15:0], R=[31:16]),交替存 int16;带超时+寄存器诊断(避免死等)。
- **analog-SPI 写机制(关键)**:ana_reg 不是普通 MMIO —— 写镜像地址后须轮询 `0x440100E8` 的第 N 位
  (N=寄存器号)直到清零(串行传输完成),否则模拟侧根本没配上。

### 调试踩过的坑(定位顺序)
1. 采集死等 → 漏了 SYS 层 `aud_cken`(模块时钟没开)。
2. 补时钟后仍空 → ana_reg 当普通 MMIO 直写、没走 analog-SPI 轮询 → 模拟前端没配上。
3. analog-SPI 修好、寄存器能回读了仍空 → 音频**电源域 pwd_audp** 没上电(致 LINEIN 位都 latch 不住)。
4. 电源域补上、AUD_CONFIG=0x129 全对仍空 → 时钟源用了 XTAL,**模拟 ADC 必须 APLL**(24.576MHz 整除音频率)。
5. APLL 配上仍空 → **soft_reset 被脉冲清回 0**,ADC 一直在复位;改为**停在 1**后出数 ✅。
（每步都靠固件里"超时+寄存器回读诊断"精确定位,而非盲改。）

### 文件/命令
- 驱动:`vendor/beken/boards/bk7258/bk7258-devkit/src/bk7258_audio.c`(+ .h);
  CMakeLists 无条件编入(不依赖 CONFIG_AUDIO);`lcdtest mic` 子命令入口。
- 后续需镜像进 contest 专属仓 `board/contest_board/src/`(和 LCD 驱动同法)。

### 待办(Step 2)
- [ ] 采样率 16k→**48kHz**(AUD_CONFIG bit[1:0]=3;APLL cal 值不变)。
- [ ] **GCC-PHAT** 互相关求 τ(频域 PHAT 加权 + 抛物线亚样点插值),τ 符号定左右、幅度定中心/偏侧。
- [ ] 新命令 `lcdtest hear`:采一段 → 算 τ → 打印方向+τ → 驱动 `eye_look`(左/中/右三档);拍手标定 τ 符号。
- [ ] 稳定后镜像驱动进 contest 仓 + 发 PR。

## 2026-08-10 · 寻声 Step 2:定位算法验证通过,待 M2 麦重焊(硬件)

### 状态:算法/软件闭环通过,卡在右麦硬件接触
- 采样率升到 **48kHz**;新增 `bk7258_mic_locate` + `lcdtest hear`(连续循环 onset 触发 → 时延 → 驱动眼球)。
- 时延估计:去直流 → **一阶差分预加重(高通白化)** → 时域归一化互相关(int64)→ 抛物线亚样点插值,
  搜索范围 ±12(21cm 间距下偏侧声会截断,待改 ±32)。
- **方向符号已标定正确**:麦拉开到 **21cm** 后实测——
  - 左侧出声 → d*=-5、τ≈-4.97 样点 → 判 **LEFT**(dx=-22)✅
  - 右侧出声 → d*=+8、τ≈+7.90 样点 → 判 **RIGHT**(dx=+22)✅
  - 正=右、负=左,眼球联动方向对,不用取反。
- 眼球联动:`hear` 内先 LCD bring-up(eye_neutral 萌眼)一次,循环内静默(g_capture_quiet)、
  仅 onset 触发时打印+eye_look 转眼;循环 80 次(~4-5s)自然结束。

### 唯一卡点:右麦 M2 通道间歇 R=0(硬件接触)
- 现象:`lcdtest mic`/`hear` 中右声道 RMS 间歇为 **0**(去直流后完全为 0 = 电气开路,正常底噪应 ~30+)。
  R=0 时互相关退化为随机峰 → 方向乱跳(同一右侧声一会 LEFT 一会 CENTER 一会 RIGHT)。
- **交换测试**:对调两颗麦到另一座后,**0 仍留在 R(M2)声道**(没跟着麦走)→
  判定 = **M2 座子/焊点/右通道那条路接触不良(冷焊/座松),不是麦本身坏**。

### 处置决定(2026-08-10):M2 重焊延后到"外设全调完、拆 SWD 之后"

> ⚠️ **本条已被 2026-08-14「SWD 处置策略修正」更新** —— SWD 改为**可插拔接口化**而非割断,
> 且 3D 外壳不再等 SWD(走两轮打印)。执行时请以 08-14 那条为准,本条保留作历史记录。
- 原因:**SWD 调试线从 CPU 引出、极脆易断**;现阶段 debug 仍依赖 SWD。
- 计划:等**所有外设驱动调完、不再需要 SWD** → 拆掉 CPU 上的 SWD → 再拿到实验室**重焊 M2 麦座**。
- 因此寻声当前状态 = **算法/软件验证通过,待 M2 硬件重焊即可启用**;暂挂起,先做其它(眼睛风格等)。

### 待办(重焊后)
- [ ] 拆 SWD → 实验室重焊 M2 座 → `lcdtest mic` 确认 L/R 均稳定非零。
- [ ] CORR_LAG_MAX 12→~32(覆盖 21cm 间距最大 ~29 样点时延)。
- [ ] 加"双通道均需 > 最小 RMS 才判向"的兜底(任一通道≈0 → CENTER,不输出随机方向)。
- [ ] 稳定后镜像 bk7258_audio.* 进 contest 仓 board/contest_board/src + 发 PR。
- [ ] 进阶:真·GCC-PHAT(FFT 频域)提升连续声(如慢歌)鲁棒性;间距缩小到成品尺寸后再标定。

## 2026-08-10 · PR #7 合并:官网风格大眼(oeye)定稿 + 双麦音频驱动入库(WIP)

- **PR #7 已 Merged** 进 `open-vela:dev-ai-contest-2026`(commit 1b48180);本地 dev 已 ff 同步。
- 入库内容(镜像自 vendor/beken 活动副本 → contest_board/src):
  - `bk7258_gc9d01.c`:新增官网风格眼 `lcdtest oeye`(白底 + 蓝虹膜 r56 + 白框 + 黑瞳孔 r28 + 双高光 + 黑眼睑;neutral/half/blink/happy;局部重绘=白底刷一次、每帧只重画眼球区,减 bit-bang 撕裂)。
  - 新增 `bk7258_audio.c/.h`:寄存器级 BK7258 模拟音频 ADC 双通道采集(免 DMA);`lcdtest mic`(裸 RMS)、`lcdtest hear`(48kHz+差分+互相关+插值→驱动 eye_look)。
  - `CMakeLists.txt`:无条件编入 bk7258_audio.c。
  - bringup 仅注释差异,未镜像(保留 contest 版)。
- **眼睛主视觉锁定 = 官网白底大眼(oeye)**。顺滑逐行眨眼待 QSPI。
- 寻声状态不变:MIC1 通路 + 左右符号验证通过;**MIC2 待硬件**(官方参考板默认只接 MIC1、MIC2 未接;已写求助文字 `寻声_MIC2求助_给官方.md` 待官方答复)。
- git:contest 仓 origin=open-vela / myfork=yz471686525-eng;分支 feat-official-eye-and-mic-audio 已合并可删。

### 后续待办(按依赖排序)
- [ ] 官方答复 MIC2(是否需软件配置/板改)→ 据此处理右麦。
- [ ] 外设全调完 → 拆 CPU 上 SWD → 实验室重焊 M2 座 → `lcdtest mic` 确认双路稳定。
- [ ] 寻声收尾:CORR_LAG_MAX 12→32、双通道均需 >阈值 才判向、必要时上真·GCC-PHAT(FFT)→ 后续 PR。
- [ ] QSPI 提速 → 顺滑逐行眨眼。
- [ ] 摄像头人脸跟踪(瞳孔跟人)。

## 2026-08-17 · 顺滑眨眼(bit-bang 提速)+ 两处花屏修复

- **官网大眼 `lcdtest oeye` 现已:两眼正常显示、开机干净(无瞬时花屏)、眨眼顺滑(眼睑分步扫合/扫开)。**
- **bit-bang 提速**:GPIO 写改为缓存 CFG(`gpio_set_output_cached`/`gpio_write_fast`,免读-改-写)+ 去掉每 bit 的 NOP 延时 → 刷新快很多,眨眼可分步扫动而不糊。
- **平滑眨眼**:`eye_o_blink_anim` 眼睑分 ~10+ 小步扫动,每步只重绘窄条,bit-bang 也能顺滑(真·逐行整帧丝滑仍留硬件 SPI/QSPI)。
- **修复 1 · 左右眼花屏**(`gpio_recache`):根因=提速用的全局 GPIO 缓存只在 `lcd_setup_pins` 刷新,而绘制循环切左右眼用 `lcd_set_pins`(不刷缓存)→ 缓存停在最后 setup 的那只眼,之后画到错的引脚。修法:新增 `gpio_recache`(只读重算缓存、不写寄存器,避免 CS 毛刺),`lcd_set_pins` 里对 sclk/mosi/cs/dc 重算缓存。
- **修复 2 · 开机瞬时花屏**(oeye 初始化 reorder):根因=背光 + display-on 早于首次填白,背光照着随机显存。修法:改为【两屏 init(显示关)→ 两屏填白 → 两屏 display-on → 最后开背光】,屏幕一亮就是白底。
- 说明:上述两处修复因 Claude 反复未能落盘,由 Kiro 救急直接改入活动驱动 `bk7258_gc9d01.c`;待正式提交时纳入。
- 备注:Claude 另在探索"硬件 SPI + 双屏并行刷新"(Phase 2,更彻底提速),尚未落盘,作为后续。

### 待办
- [ ] 把这版(顺滑眨眼 + 两处花屏修复)镜像进 contest 仓 board/contest_board/src → 发 PR。
- [ ] 后续:硬件 SPI 双屏(全屏高速/丝滑眨眼)、GC2145 摄像头(人脸跟随)、寻声待 M2 重焊。
