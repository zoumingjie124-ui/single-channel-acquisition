# 验证报告（V5）

> 本文件只记录**真正执行过**的验证。没跑过的一律写在"未验证"里，不做事后美化。

- 日期：2026-10-17（初版）／2026-10-27 复审更新
- 环境：Keil MDK5（ARMCC V5.06，`D:\K5\UV4\UV4.exe`）、Python 3.13.12（受管环境）、Windows

---

## 1. 固件编译（已实际执行）

```
cd MDK-ARM
D:\K5\UV4\UV4.exe -b DataAcq.uvprojx -j0 -o build.log     → exit code 0

Program Size: Code=20700  RO-data=996  RW-data=108  ZI-data=4916
"DataAcq\DataAcq.axf" - 0 Error(s), 0 Warning(s).
```

| 资源 | 占用 | 容量 | 比例 |
|---|---|---|---|
| Flash | 20700 + 996 = 21696 B（21.2 KiB） | 64 KiB | 33% |
| RAM（静态） | 108 + 4916 = 5024 B（4.9 KiB） | 20 KiB | 25% |

编译过程中修掉的问题（均为真实出现过的错误）：

| 现象 | 原因 | 修法 |
|---|---|---|
| `L6200E: Symbol SystemInit multiply defined` | `system_stm32f1xx.c` 同时被 Core 组和 CMSIS 组加入 | 从 Core 组移除，只保留 CMSIS 组里那份 |
| `#223-D: function "Error_Handler" declared implicitly` ×9 | adc.c / tim.c / usart.c 没包含 main.h | 补 `#include "main.h"` |
| `#550-D: variable "s_seq" was set but never used` | app_cmd.c 里遗留的无用变量 | 删除 |

编译**没有**报错但属于真实缺陷、已顺手修掉的：

| 问题 | 影响 | 修法 |
|---|---|---|
| `GPIO_InitTypeDef` / `RCC_*InitTypeDef` / `TIM_*ConfigTypeDef` 局部变量未清零 | 未赋值的字段（如 `Pull`、`Speed`）会把栈上随机值写进寄存器 | 全部改为 `= {0}` 初始化（含 `tim.c`） |
| 主循环每轮都写 PC13 | 每秒上万次无效 GPIO 写 | 加状态缓存，只在变化时写 |

---

## 2. 协议与上位机数据通路（已实际执行）

```
cd host
python -m unittest discover -s tests -v
Ran 28 tests — OK
```

| 测试文件 | 数量 | 覆盖内容 |
|---|---|---|
| `tests/test_protocol.py` | 16 | CRC 标准向量（`b"123456789" -> 0x29B1`）、四种载荷往返、命令帧构造、阈值合法性及 16 位溢出边界校验、逐字节分包、垃圾前缀重同步、连续 `0xAA` 帧头容错、坏 CRC 拒绝并恢复、超长 LEN、半截帧等待、文本行分离 |
| `tests/test_pipeline.py` | 4 | 5 秒 100Hz 数据流回放（500 点）、序号连续性、丢帧缺口检测、坏帧不影响后续、13 字节不规则分包 |
| `tests/test_firmware_sync.py` | 8 | 固件 `app_proto.h` / `app_config.h` 与 `protocol.py` 常量逐项比对；`app_alarm.c` 阈值校验防 16 位溢出静态检查；`app_task.c` 里 STAT/SAMPLE/EVENT 的字段偏移与长度校验 |

**`test_firmware_sync.py` 的性质要说清**：它是**源码级静态检查**（正则解析 C 头文件与 C 函数体），
不是联调。它能抓到"改了一边忘了另一边"，但**不能证明运行期两端的字节流真的一致**——
后者只能靠真机联调。

---

## 3. 上位机界面（已实际执行）

用 Qt 的 offscreen 平台真实创建窗口、启动内置演示数据源跑 3 秒，然后断言：

```
cd host
set QT_QPA_PLATFORM=offscreen
python tests/gui_smoke.py

界面字体      : Microsoft YaHei
采样点        : 295
曲线缓冲点数  : 295
告警事件行数  : 2
序号缺口      : 0
整帧丢失      : 0
解析错误      : 0
当前级别      : NORMAL
统计面板采样数: 295
清零同步验证  : OK
注入缺口检测  : OK（期望 +2，实得 +2）
BOOT 重新对齐 : OK
RESULT: PASS
```

3 秒理论 300 点，实收 294~298 点（差额是启动阶段的暖机，属正常）。缺口 0、解析错误 0，
说明「组帧 → 解析 → 绘图 → 事件表 → 统计面板」整条链路是通的。

截图见 `docs/gui_demo.png`（由同一次测试自动保存，不是手绘的示意图）。

**过程中发现并修掉的真实问题**：

| 现象 | 原因 | 修法 |
|---|---|---|
| 界面所有中文显示成方框 □□□□ | Qt 没选中含中文字形的字体；离屏/远程桌面环境下 `QFontDatabase.families()` 可能为空 | 改为用 `addApplicationFont()` 直接注册 `C:\Windows\Fonts\msyh.ttc`，注册失败才退回枚举 |
| 事件表"变化"列被截断成 `NORMAL -> ...` | 默认列宽太窄 | 显式设置列宽 |
| 演示模式"状态切换"恒为 0 | 演示源没有累加计数 | 补上计数 |
| `DemoSource.__init__` 残留 `on_text` 参数 | 早期版本改造时漏删 | 删除 |

**注意**：这是"界面 + 数据通路"的验证，不是"固件联调"。演示数据由上位机自己生成，
不经过串口，所以它证明不了固件发出来的字节一定正确。

---

## 4. 上板验证情况

### 4.1 已验证（2026-11-02，首次上板）

| 项目 | 结论 | 证据 |
|---|---|---|
| 实际采样率 | **100.01 Hz**（60117 帧 / 601.1 s） | §8，上位机侧逐帧统计 |
| ADC+DMA 是否丢点 | **drops = 0**，连续 10 分钟 | §8 |
| UART DMA 是否丢字节 | 序号缺口 0、整帧丢失 0、解析错误 0 | §8 |
| ADC 零点（PA1→GND） | **+1.75 mV（≈2.2 LSB）**，PASS | §9.2 |
| ADC 满量程（PA1→3.3V） | **3299.90 mV（4094.9 LSB）**，PASS | §9.3 |
| 长命令 HT 重复交付 | 10 条长命令 → 10 行响应，`rxf` 增量 0，PASS | §9.5 |

> 注意口径：这些是**串口链路上统计到的**结果，等价于"设备发出什么、上位机收到什么"；
> ADC 的**电压读数准不准**不在其中（见 4.2）。

### 4.2 仍未验证（必须上板，且需要额外仪器/更长时间）

| 项目 | 为什么必须上板 | 怎么验证 |
|---|---|---|
| 多点线性（中间量程） | 只测了 0 和满量程两点，中间段未验证 | 用可调源取 25% / 50% / 75% 三点，拟合直线 |
| 告警滞回手感 | 需可调信号源 | 在阈值附近缓慢来回调，观察是否抖动 |
| ORE 溢出后能否自恢复 | 需注入噪声 | 带电短接 RX 到 GND（建议串 100Ω） |
| 中断优先级是否合理 | 需真实中断竞争 | 高负载下观察是否丢帧 |
| 8 小时稳定性 | 无法模拟 | 长跑（目前只跑了 10 分钟） |
| 零点噪声来源 | 已知峰峰 9 mV，但未定位是板级纹波还是接线 | 换短接线/换电源后对比 |

---

## 5. 本次实际执行到的程度（逐项打勾，不夸大）

| 内容 | 状态 |
|---|---|
| 固件编译 | ✅ 已实际执行，0 Error / 0 Warning |
| 协议编解码单测 | ✅ 已实际执行，16 项通过 |
| 端到端离线回放 | ✅ 已实际执行，4 项通过（合成 100Hz 流，非真实串口） |
| 固件/上位机常量一致性 | ✅ 已实际执行，8 项通过（**静态检查**） |
| 上位机脚本语法 | ✅ `py_compile` 全部通过 |
| 上位机界面实际运行 | ✅ 已离屏实际运行 3 秒，PASS，并自动保存截图 |
| 上位机串口连接 | ❌ **未测**（需要板子与 USB-TTL） |
| 固件与上位机真实联调 | ❌ **未做**（需要板子和串口） |
| 上板实测 | ❌ **完全未做** |
| PC-Lint / Cppcheck / MISRA | ❌ **未运行**，不作任何"通过 MISRA"的声称 |

---

## 6. 复现步骤

```bat
:: 1. 固件编译
cd "Data acquisition\MDK-ARM"
D:\K5\UV4\UV4.exe -b DataAcq.uvprojx -j0 -o build.log
type build.log | findstr "Program Size Error"

:: 2. 单测（不需要硬件，不需要 PySide6）
cd "..\host"
python -m unittest discover -s tests -v

:: 3. 无硬件看界面
pip install -r requirements.txt
python main.py --demo

:: 4. 接板子
python main.py
```

---

## 7. 2026-10-24 / 2026-10-27 复审更新

> **证据强度声明**：本项目**尚未上板、也未与上位机联机**。
> 本节所有"现象"均来自**代码与 HAL 源码的静态分析**，不是实测复现。
> 每行都标了证据强度：`静态可证` = 读代码即可确认；`推演` = 由代码路径推断，需实测验证。

### 7.1 本轮实际改了什么（不限于文档）

| 提交 | 类型 | 内容 |
|---|---|---|
| `b1b1d63` | **固件 + 上位机** | 修掉 4 个缺陷（见 7.2）：HT 重复上交、seq8 清零回跳、帧头 SEQ 无人检查、命令 SEQ 恒为 0 |
| `8884244` | **固件** | `s_dma_buf` 补 `volatile` |
| `81e1705` | **固件** | 恢复标志竞态修复、计数器清零加临界区、环形缓冲断言补下界 |
| `fa8496c` | 文档 / 测试 | 用例数更正、`gui_smoke` 增强、协议补充 |

> **勘误（2026-10-24 修订）**：本节初版曾声称这一轮没有触碰固件 —— 这个说法不成立。
> 本轮实际改动了 `app_task.c`、`app_cmd.c`、`app_task.h`、`usart.c` 以及 `app_measure.c`。
> 本文档的价值在于"写下的每一句都能对得上代码"，所以错的地方要留痕改正，而不是悄悄抹掉。

### 7.2 本轮真正修掉的四个缺陷

| # | 缺陷 | 现象（存在该缺陷时） | 根因 | HAL / 代码证据 | 修法 | 强度 |
|---|---|---|---|---|---|---|
| 1 | **HT 回调重复上交** | 一次 IDLE 收到的字节被上交两次；回调里重挂接收直接拿到 `HAL_BUSY` | `HAL_UARTEx_RxEventCallback` 在**半传输**时也会被调用，此刻接收仍在进行，`Size` 只是"填到一半"，不是"收完了" | `UART_DMARxHalfCplt()`（`stm32f1xx_hal_uart.c:3104`）只置 `RxEventType = HAL_UART_RXEVENT_HT` 后**直接回调**，不 abort DMA、不改 `RxState`；IDLE/TC 路径才会先 abort 再回调 | `usart.c` 回调首行判 `huart->RxEventType == HAL_UART_RXEVENT_HT` → 直接 `return`，数据留到 IDLE/TC 再上交 | 静态可证 |
| 2 | **seq8 清零回跳** | 上位机下发 `reset` 后，采样序号从 255 跳回 0，被误判成"丢了 255 个样本" | SAMPLE 载荷第 9 字节的 seq8 原来取 `stats.sample & 0xFF`，而 `reset` 命令会把这个计数清零 | `app_task.c` 的 `ctx_reset_stats()` 清采样计数；修前 `emit_sample()` 直接用它算 seq8 | 新增独立计数器 **`sample_seq`**（`reset` 有意不清），seq8 改取 `sample_seq & 0xFF` | 静态可证 |
| 3 | **帧头 SEQ 无人检查** | 整帧丢失**完全不可见**：CRC 只能发现"内容被改坏"，发现不了"帧根本没到" | 上位机只解析载荷里的 seq8，从不看帧头 SEQ；且帧头 SEQ 修前是各模块各发各的，ACK 还回显命令序号，根本无法用于连续性判断 | 修前 `main.py` 无 `expected_frame_seq` 字段；固件 `emit_frame()` 各自 `s_ctx.frame_seq++` | 固件：所有出站帧共用 `app_task_next_frame_seq()`；上位机：`expected_frame_seq` + `frame_gap_count`，遇到 BOOT 帧重新对齐 | 静态可证 |
| 4 | **命令 SEQ 恒为 0** | 上位机发出的每一条命令，帧头 SEQ 都是 0 | `_next_seq()` 取 `self.parser.frames`，但真实串口的数据由 `LinkWorker` 内部**另一个** `FrameParser` 解析，`MainWindow.self.parser` 只在演示模式被喂数据 | `main.py` 中 `self.parser.feed()` 仅出现在 `_feed_demo()`；`link.py:37` 在子线程里自建 `FrameParser` | 上位机改用独立计数器 `_tx_seq` | 静态可证 |

> 第 2、3 条是同一个思路的两面：**"序号"必须只表达一件事**。
> `sample_seq` 只表达"第几个样本"（reset 不能动），帧头 SEQ 只表达"第几帧出站"（全体共用）；
> 混用就会让上位机的统计失去意义。

### 7.3 四处关键设计（不是本轮修复，也未经实测）

下表是**项目成型时就定下的设计**，不是这一轮修的东西。
"现象"一列已改为"**如果不这样做会怎样**"，全部属于推演，未上板验证。

| # | 如果不这样做（风险，推演） | 根因 | HAL 依据 | 本项目做法 |
|---|---|---|---|---|
| 1 | DMA 在跑，但采样环恒为空，上位机一帧都收不到 | HAL 只把 ADC 结果搬进 `s_dma_buf`；"取走并投递"必须由用户在回调里完成 | `ADC_DMAConvCplt()`（`stm32f1xx_hal_adc.c:2333`）只更新状态并调 `HAL_ADC_ConvCpltCallback()`；HAL 不搬运、不缓存样本 | `adc.c` 实现 `ConvHalfCpltCallback` / `ConvCpltCallback` / `ErrorCallback` |
| 2 | 连续发帧时偶发 `HAL_BUSY`，整帧丢失 | `HAL_UART_Transmit_DMA()` 上一帧未完成时不排队，直接返回 `HAL_BUSY` | 函数首行 `if (huart->gState == HAL_UART_STATE_READY)`，否则返回 `HAL_BUSY` | `app_tx.c` 单帧串行：发完（`TxCpltCallback`）才取下一批 |
| 3 | 接收不定长命令时，短帧要等缓冲填满才上交 | F1 的基础 DMA 接收没有"不定长"语义 | `HAL_UARTEx_ReceiveToIdle_DMA()` + `RxEventCallback()`：IDLE/TC 先 abort DMA、置 `RxState = READY` 再回调 | `usart.c` 用 `ReceiveToIdle_DMA`，只处理 IDLE/TC，忽略 HT（见 7.2 第 1 条） |
| 4 | 一次噪声（ORE）之后接收/采样永久停摆，且无任何提示 | HAL 的错误路径只回调，**不会自动重启**传输 | `ADC_DMAError()` 只置 `HAL_ADC_STATE_ERROR_DMA` 并回调 weak 空函数 | ADC：ISR 只置标志 → 主循环 `ADC1_RecoverInit()` + `app_measure_start()`，限速 1 s；串口：`ErrorCallback` 清 ORE，且**仅在 `gState != BUSY_TX` 时**释放发送队列 |

第 3、4 条最容易踩的坑是同一处：**错误回调里不能无条件重置发送状态**。
接收侧的 ORE/FE/NE 也会进 `HAL_UART_ErrorCallback`，此时 TX DMA 可能还在跑；
把 `s_busy` 清零会让主循环立刻再调 `HAL_UART_Transmit_DMA()` 并拿到 BUSY，
这批已出队的数据就被误记成"丢弃"——既真丢数据，又让统计失真。

### 7.4 构建尺寸的变化（历史基准记录）

| 阶段 | Code | 变化 | 原因 |
|---|---|---|---|
| 整帧丢失检测合入前 | 20548 | — | README 里那个旧数字 |
| 整帧丢失检测合入后 | 20576 | +28 | 帧序号统一（`app_task_next_frame_seq` + `sample_seq`） |
| `volatile` 修复后 | 20576 | **+0** | `volatile` 只约束编译器优化，不生成指令 |
| 竞态修复 + PRIMASK 后 | 20584 | +8 | `__get_PRIMASK` / `__set_PRIMASK` 的 MRS/MSR，以及两条失败路径上的标志回置 |
| 统计清零 / 整帧原子 / AA 容错 / 阈值校验 | 20620 | +36 | 新增 `usart_reset_counters()`（含 PRIMASK）、`app_tx_write` 提前返回、`SOF1` 多分支、阈值比较改为 `WARN + HYST` |
| 清理 `app_stats_t` 只写不读字段 | 20616 | -4 | 移除无用死字段赋值，ZI 降低 |
| v5.1.1：删整个 `app_stats_t` + 阈值防回绕 + STAT 文本加字段 | **20700** | +84 | RO +20（格式化字符串变长）、ZI −32（去掉 8 个 uint32 字段） |

### 7.5 2026-10-27 深度复审与补齐（v5.1.1）

| 项 | 内容 | 理由 |
|---|---|---|
| 修复溢出漏洞 | `app_alarm.c` 的 `app_alarm_set_thresholds()` 将 `(uint16_t)(warn_mv + APP_THR_HYST_MV)` 改为 `((uint32_t)warn_mv + APP_THR_HYST_MV)` | 原写法在 `warn_mv >= 65486` 时会发生 16 位无符号加法回绕（如 `65500 + 50 -> 14`），导致 `warn=65500, crit=100` 绕过校验被接受 |
| 彻底清理死字段 | 移除 `app_types.h` 中只写不读的 `app_stats_t`，`app_task.c` 的 `s_ctx` 直接维护 `uint32_t samples` | 此前仅删了 `uart_err` / `rx_err`，剩余 7 个字段（`drop` 等）同样只在 `ctx_reset_stats()` 写 0 而从不读取 |
| 补齐可观测输出 | `app_task.c` 的 `stat` 文本命令补打 `cmd=%lu/%lu txd=%luB`；`ctx_reset_stats()` 末尾立即调用 `emit_stat()` | 让 `app_tx_drop_bytes()` 和 `app_cmd_ok/err_count()` 真正可在串口终端观测，且清零后无需再等 1s 周期刷新 |
| 补齐局部结构体清零 | `Core/Src/tim.c` 中 `MX_TIM2_Init` / `MX_TIM3_Init` 的 `sClockSourceConfig` 与 `sMasterConfig` 补 `= {0}` | 与第 1 节“局部 HAL 结构体全部清零”严格对齐 |
| 上位机清零与演示闭环 | `host/main.py` 点击“清零”时同步归零上位机侧 `self.samples / gap_count / frame_gap_count`，并在 `DemoSource` 中完整响应 `SET_THR / SET_LED / GET_STAT / RESET` | 避免清零后 10ms 收到的下一帧 `SAMPLE` 又把界面上的“采样数”刷回旧累计值 |
| 单测与文档同步 | 新增阈值溢出边界单测与 C 源码静态断言（共 28 项单测：16 + 4 + 8），同步 `PROTOCOL.md`、`CUBEMX_CONFIG.md` 与本报告 | 确保文档、测试与源码 100% 严丝合缝 |

### 7.6 合并 v5.1.1 时补的两处（2026-11-02）

| 项 | 说明 |
|---|---|
| 文档尺寸同步 | v5.1.1 交付包里的本报告仍写 `Code=20616`，但该版代码实测为 **20700 / RO 996 / ZI 4916**。已按实测更新 §1 与 §7.4。（README 已改为只写 `0 Error 0 Warning` 不带数字，这个做法更耐放，保留） |
| `tx_text` 缓冲 128 → 192 | 新增的 `stat` 文本行在最坏情况下（全部计数器到 10 位）约 168 字符，128 虽不会溢出（`vsnprintf` 截断），但会把尾部 `txd=` 切掉。改 192 后完整输出 |

> 这两处不影响功能，属于"文档与余量"层面的收尾。

---

## 8. 10 分钟连续运行记录（首次上板实测）

> 时间：2026-11-02 10:22 – 10:32
> 硬件：STM32F103C8T6 + USB-TTL(CH340) · COM5 · 115200 8N1
> 输入：10k 电位器分压，测试期间**未人为调节**
> 统计方：上位机侧协议解析器（不是 Keil 调试窗口，也不是示波器）
> 原始日志：`docs/SOAK_10MIN.txt`

### 8.1 结果

| 指标 | 结果 | 判定 |
|---|---|---|
| 运行时长 | 601.1 s（10 分 01 秒） | — |
| SAMPLE 帧 | 60117 帧 → **100.01 Hz** | ✅ 与设计 100Hz 一致 |
| **drops**（设备端采样环溢出） | **0** | ✅ 一个采样点都没丢 |
| **frame_err** | **0** | ✅ 无 CRC / 格式错误 |
| adc_err / uart_err / rx_err | 0 / 0 / 0 | ✅ |
| **序号缺口（seq8）** | **0** | ✅ 样本级连续 |
| **整帧丢失（帧头 SEQ）** | **0** | ✅ 整帧级连续 |
| 解析错误 | 0 | ✅ |
| STAT 帧 | 原始日志**未逐帧计数**（每 60 s 打一次快照）；另跑 61.1 s 复测 **61 帧 → 1.00 Hz**，与 `APP_STAT_PERIOD_MS=1000` 一致 | ✅ 见 8.4 |
| BOOT 帧 | 0 | ✅ 中途未复位 |
| 链路中断 | 0 | ✅ |
| 设备运行时间（结束时） | 1,825,000 ms ≈ 30 min | 开始测试前已稳定运行约 20 min |
| 状态切换 | 36 次 | ⚠️ 见 8.3 |

每分钟采样数：

```
[ 61s] 6575   [122s] 6107   [183s] 6101   [244s] 6107   [301s] 5635
[362s] 6106   [423s] 6107   [484s] 6106   [540s] 5638
```

单分钟数字的波动来自"满 60 秒才打印"的取整（实际间隔 61 秒左右），
**总账 60117 / 601.1 s = 100.01 Hz** 才是权威口径。

### 8.2 这组数据证明了什么

整条链路——TIM3(TRGO) → ADC1 → DMA1 → 采样环 → 组帧 → DMA 发送 → 串口 → 上位机解析——
在 10 分钟内**零丢点、零丢帧、零错误**，并且采样率精确到 100.01 Hz。
设备在测试前已连续运行约 20 分钟，等效于 30 分钟无复位。

### 8.3 这组数据**不能**证明什么

- **ADC 的电压精度/线性**：只证明了"帧在流动"，没有用标准源比对读数
- **8 小时长期稳定性**：只跑了 10 分钟（+ 前 20 分钟）
- **抗干扰**：没有注入噪声、没有带电拔插
- **告警滞回是否合适**：10 分钟内出现 36 次状态切换，若全程未人为调节电位器，
  则说明输入可能卡在阈值附近（50mV 滞回偏窄），建议把 `APP_THR_HYST_MV` 调到 80–100 或把工作点移开阈值

### 8.4 STAT 帧数量的复核（2026-11-02 10:37 追加）

**结论：固件按 1 Hz 发 STAT，没有丢；10 分钟记录里"只有 10 条"是打点方式，不是收帧数量。**

原因在当初那个临时采集脚本：它**每收到一帧 STAT 就覆盖同一个 `stat` 变量，只在整 60 s 的刻度上打印一次**
（`if elapsed >= next_mark: ...; next_mark += 60.0`）。所以 10 分钟只有 10 行输出，
每行是**那一瞬间的 STAT 快照**，不是"10 分钟只来了 10 帧"。

为把这件事钉死，重新做了两次独立复测（串口直连，逐帧计数）：

| 复测 | 时长 | SAMPLE | STAT | STAT 频率 |
|---|---|---|---|---|
| A（`docs/SOAK_STAT_1MIN.txt`） | 61.1 s | 6106（100.00 Hz） | **61** | **1.00 Hz** |
| B（临时探针，仅打印） | 94 s | 9394（99.9 Hz） | **94** | **1.00 Hz** |

对照固件：`app_config.h:26` `APP_STAT_PERIOD_MS = 1000u`，
`app_task.c:449` 以 `tick_ms - stat_last_ms >= APP_STAT_PERIOD_MS` 为条件触发。
10 分钟理论约 600 帧，与"每 60 s 打印一次"完全不是一回事。

**顺带确认的另一点**：复测 A 中 EVENT 帧为 0，而 STAT 里 `changes` 是 40。
这不是 EVENT 丢帧 —— 那 40 是**开机以来的累计值**，复测窗口内 `changes` 增量为 0（窗口内没有状态跳变）。
代码上 `app_alarm_update()` 里 `change_count++` 与"返回新级别"发生在同一分支，
而 `process_sample()` 一旦发现 `level != last_level` 就调用 `emit_event()`，
因此**每一次切换必然对应一帧 EVENT**，不存在只在计数里加、不发帧的情况。

**工具已入库**：`host/tools/soak.py`（本次新增）按帧类型分别计数并输出频率，
不再有"每 60 s 只打一行"的歧义。用法：

```bat
python host\tools\soak.py            :: 默认 10 分钟
python host\tools\soak.py 1 COM5     :: 60 秒
```

输出末尾会直接给出 `STAT 帧 : N (x.xx Hz，设计 1.00 Hz)`，低于 1 Hz 就说明 STAT 真的在丢。

## 9. 零点 / 满量程 / HT 修复 上板验证（2026-11-02 10:50–11:05）

### 9.1 测量方法

| 项 | 说明 |
|---|---|
| 输入源 | PA1 直接接电源轨：GND（零点）、3.3V（满量程），**断电改线后再上电** |
| 采集 | 上位机逐帧解析，每次 4.7 s ≈ 468 帧（99.75 Hz） |
| 工具 | `host/tools/calib.py`（`zero` / `full` / `watch` 三种模式） |
| 口径 | **线上传的是换算后的 mV（`raw_mv`），不是 ADC 原始码**；下文 LSB 由 mV 反算，0.806 mV/LSB |

改线前板子挂了 OLED，测满量程前已全部拔除，避免屏从 3.3V 轨取电把轨压拉低、
被误判成增益误差。

### 9.2 零点（PA1 → GND）

```
真实时长 4.69 s / 468 帧（99.75 Hz）
raw_mv min/mean/max : 0 / 1.75 / 9 mV
反算 LSB（均值）     : 2.2
峰峰值              : 9 mV ≈ 11 LSB
判定                : PASS（阈值：均值 ≤ 3 mV）
```

### 9.3 满量程（PA1 → 3.3V）

```
真实时长 4.69 s / 468 帧（99.75 Hz）
raw_mv min/mean/max : 3299 / 3299.90 / 3300 mV
反算 LSB（均值）     : 4094.9
峰峰值              : 1 mV ≈ 1 LSB
判定                : PASS（阈值：均值 ≥ 3295、最小 ≥ 3290）
```

### 9.4 误差分析（这两点能说明什么、不能说明什么）

| 指标 | 数值 | 说明 |
|---|---|---|
| 零点偏移 | **+1.75 mV（+2.2 LSB）** | ADC 内部偏移 + 地线压降，量级正常 |
| 满量程读数 | 3299.90 mV（4094.9 LSB） | 与理想 3300 差 −0.10 mV（−0.003%） |
| 跨度（span） | 3299.90 − 1.75 = **3298.15 mV** | 相对 3300 偏小 1.85 mV（**−0.056%**） |
| 零点噪声 | 峰峰 **9 mV（11 LSB）** | 约 ±5 mV，占 WARN 阈值 0.2% |

**能说明**：零点偏移约 2 LSB、跨度误差 0.06%，对 2400 / 3000 mV 的阈值判断完全够用；
ADC 与 `raw*3300/4095` 的换算在两端都没有明显偏差。

**不能说明**：

- **不是"全量程线性度 0.06%"** —— 只测了两点，只能算一个端点直线，中间段（25%/50%/75%）
  没测，非线性误差未知。
- **满量程的"峰峰只有 1 mV"不代表噪声更小**：输入顶在 VREF 上时，ADC 输出在 4094/4095
  之间钳位，峰峰被饱和截断。零点那 9 mV 才是真实的噪声水平。两处数字不可比。

### 9.5 HT（半传输）修复验证

| 阶段 | 命令长度 | 是否跨过半传输点（32 B） | 期望回 ERR 行数 | 实测 |
|---|---|---|---|---|
| A | 8 字符 | 否 | 10 | **10** |
| B | 48 字符 | **是，必然触发 HT** | 10 | **10** |

```
基线: uart=0 rxf=0 frm=0
两阶段后: uart_err +0 / rxf +0 / frm +0
RESULT: PASS
```

判读依据：一条未知文本命令必然回一行 `ERR unknown command`。

- 若 HT 分支仍重复上交（修前行为），一条命令会被执行两次 → **20 行 ERR**；
- HT 时若还去 `rearm`，`HAL_UARTEx_ReceiveToIdle_DMA()` 会因 `RxState == BUSY_RX`
  返回 BUSY → `s_rx_fail++` → STAT 里 `rxf` **+10**。

实测两项都是干净值，说明 HT 分支的 early-return 生效。

> **局限**：这证明的是"修复后行为正确"，**不是**"回滚修复后一定失败"——
> 没有做撤销补丁的反向对照实验。HT 事件本身必然发生（48 B > 32 B 半缓冲），
> 但"修前一定会丢"只能从 HAL 源码推断，不能声称实测到了。

工具：`host/tools/htcheck.py`（可直接复跑，含 A/B 对照）。
