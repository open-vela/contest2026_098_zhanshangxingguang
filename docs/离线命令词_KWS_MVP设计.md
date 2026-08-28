# 离线命令词识别（KWS）—— 最小可行设计（MVP）

> 目标：BK7258(Cortex-M33)**端侧、离线**命令词识别,**全链路 Apache-2.0 可开源**,彻底避开 Wanson 等闭源库。
> 交互:**不做唤醒词**,由**按键/拿起触发**一次识别(合规:规则仅约束"AI 语音唤醒功能")。

## 1. 技术栈(全部 Apache-2.0 / 可开源)
| 层 | 选型 | 许可 |
|---|---|---|
| 推理框架 | **TensorFlow Lite Micro (TFLM)** | Apache-2.0 |
| NN 内核 | **CMSIS-NN**(M33 DSP 扩展加速) | Apache-2.0 |
| 信号前端 | **CMSIS-DSP**(FFT/DCT 做 MFCC) | Apache-2.0 |
| 模型 | **DS-CNN small**(int8 量化) | 自训,权重自有 |
| 训练数据 | Google Speech Commands v2 + 自录命令词 | CC-BY / 自有 |

> 备选(时间紧的 MVP):**MFCC + DTW 模板匹配**(纯自研,零依赖),先顶上,后续再升级到 TFLM。

## 2. 命令词表(MVP,示例,可改)
先做 **6 个**(少而准优先),再扩:
```
过来 / 睡觉 / 眨眼 / 跟我 / 你好 / 停
+ 两个特殊类：_silence_(静音) / _unknown_(其它声音)
```
模
型输出 = 命令数 N + 2 类。

## 3. 音频前端参数
- 采样:**16 kHz 单声道**(复用板载麦克风)。
- 分帧:帧长 **30 ms(480 点)**,帧移 **20 ms(320 点)**。
- 判定窗:**1 s ≈ 49 帧**。
- 特征:每帧 **MFCC 10~13 维**(或 log-mel 40 bins);拼成 `[T=49, F=10~13]` 送模型。
- 前端用 CMSIS-DSP 的 RFFT + mel 滤波 + DCT 实现。

## 4. 模型
- 结构:**DS-CNN small**(depthwise-separable CNN),输入 `[49, 10]`,输出 `N+2` softmax。
- 量化:**int8**(权重+激活),CMSIS-NN 加速。
- 体积:权重约 **20~40 KB**;tensor arena 约 **30~100 KB**。
- 推理:单窗约 **几 ms**(M33@480MHz + CMSIS-NN),远小于实时。
- 资源:模型+arena 放 SRAM 即可(板载 640KB SRAM + 8/16MB PSRAM),压力很小。

## 5. 运行流程(按键/拿起触发,省电)
```
按键/拿起(加速度计) 触发
  → 录音 ~1s(16kHz PCM,环形缓冲)
  → 分帧 + MFCC(CMSIS-DSP)
  → TFLM invoke(DS-CNN, CMSIS-NN)
  → argmax(prob)；prob>=阈值(如 0.7) 且 非 _silence_/_unknown_ → 命中命令
  → 分发到业务(VelaPet 表情/动作等)
```
不常听、只在触发后跑一小段 → 省电,且天然不是"唤醒词",合规。

## 6. 代码模块划分(openvela / NuttX)
```
app/kws/                         # 或并入 camera app 作 "kws" 子命令
  kws_main.c        —— CLI 入口: kws test / kws run
  kws_frontend.c/.h —— PCM→分帧→MFCC(CMSIS-DSP)
  kws_infer.c/.h    —— TFLM 加载 + invoke(CMSIS-NN)
  kws_model_data.cc —— int8 模型 const 数组(xxd 生成,编入固件)
  kws_labels.h      —— 命令词标签表
board/contest_board/src/bk7258_audio.c  # 复用:麦克风取 PCM
```
- 触发:复用现有按键 / `bk7258_accel.c`(拿起检测)。
- 集成:CMSIS-DSP/NN 为纯 C 库;TFLM 为 C++(NuttX 支持 C++)。

## 7. 离线训练流程(PC 侧,一次性)
1. 数据:Google Speech Commands v2 取通用词;自录目标命令词各数百条 + 数据增广(加噪/变速/音量)。
2. 训练:用 ML-Commons / ARM 的 tinyml KWS 脚本训 DS-CNN(GSC 基准模型可直接改)。
3. 量化:训练后 int8 量化(TFLite converter,代表性数据集校准)。
4. 导出:`.tflite` → `xxd -i` → `kws_model_data.cc` 的 `const unsigned char[]`。
5. 校验:PC 端先测准确率达标,再上板。

## 8. MVP 验收标准
- 安静环境、说话人无关:目标命令词 **识别率 > 90%**,非命令词 **误触发 < 5%**。
- `kws test`:喂预置 wav/PCM,打印每类概率 + 判定(离线自测)。
- `kws run`:按键触发,说命令词能正确命中、说别的不误触发。

## 9. 里程碑
1. **M1** 前端跑通:麦克风 PCM → MFCC,`kws test` 打印特征(不接模型)。
2. **M2** 推理跑通:TFLM+CMSIS-NN 加载 int8 模型,喂离线样本出正确 argmax。
3. **M3** 端到端:按键触发 → 录音 → 识别 → 打印命令。
4. **M4** 接业务:命令分发到 VelaPet 表情/动作。
5. **M5** 鲁棒性:加噪声增广重训、加阈值/去抖,降误触发。

## 10. 合规
- 推理/内核/前端库均 **Apache-2.0**;数据集 CC-BY;模型权重自训自有。
- **无任何闭源预编译库**,可随参赛仓库以 Apache-2.0 完整开源分发 —— 彻底规避 Wanson 授权问题。

## 11. 风险 & 兜底
- TFLM 集成/构建耗时超预期 → 先上 **MFCC+DTW 自研版**(说话人登记、少量命令)做 MVP,保证有可演示的离线命令词,再迭代到 DS-CNN。
- 麦克风采集若当前 openvela 移植未跑通 → 先打通 `bk7258_audio.c` 的 PCM 采集(KWS 的前提)。
