# 验证报告（V5）

> 本文件只记录**真正执行过**的验证。没跑过的一律写在"未验证"里，不做事后美化。

- 日期：2026-10-17（初版）／2026-10-24 复审更新
- 环境：Keil MDK5（ARMCC V5.06，`D:\K5\UV4\UV4.exe`）、Python 3.13.12（受管环境）、Windows

---

## 1. 固件编译（已实际执行）

```
cd MDK-ARM
D:\K5\UV4\UV4.exe -b DataAcq.uvprojx -j0 -o build.log     → exit code 0

Program Size: Code=20584  RO-data=976  RW-data=108  ZI-data=4956
"DataAcq\DataAcq.axf" - 0 Error(s), 0 Warning(s).
```

| 资源 | 占用 | 容量 | 比例 |
|---|---|---|---|
| Flash | 20584 + 976 = 21560 B（21.0 KiB） | 64 KiB | 33% |
| RAM（静态） | 108 + 4956 = 5064 B（4.9 KiB） | 20 KiB | 25% |

编译过程中修掉的问题（均为真实出现过的错误）：

| 现象 | 原因 | 修法 |
|---|---|---|
| `L6200E: Symbol SystemInit multiply defined` | `system_stm32f1xx.c` 同时被 Core 组和 CMSIS 组加入 | 从 Core 组移除，只保留 CMSIS 组里那份 |
| `#223-D: function "Error_Handler" declared implicitly` ×9 | adc.c / tim.c / usart.c 没包含 main.h | 补 `#include "main.h"` |
| `#550-D: variable "s_seq" was set but never used` | app_cmd.c 里遗留的无用变量 | 删除 |

编译**没有**报错但属于真实缺陷、已顺手修掉的：

| 问题 | 影响 | 修法 |
|---|---|---|
| `GPIO_InitTypeDef` / `RCC_*InitTypeDef` 局部变量未清零 | 未赋值的字段（如 `Pull`、`Speed`）会把栈上随机值写进寄存器 | 改为 `= {0}` 初始化 |
| 主循环每轮都写 PC13 | 每秒上万次无效 GPIO 写 | 加状态缓存，只在变化时写 |

---

## 2. 协议与上位机数据通路（已实际执行）

```
cd host
python -m unittest discover -s tests -v
Ran 24 tests — OK
```

| 测试文件 | 数量 | 覆盖内容 |
|---|---|---|
| `tests/test_protocol.py` | 14 | CRC 标准向量（`b"123456789" -> 0x29B1`）、四种载荷往返、命令帧构造、逐字节分包、垃圾前缀重同步、坏 CRC 拒绝并恢复、超长 LEN、半截帧等待、文本行分离 |
| `tests/test_pipeline.py` | 5 | 5 秒 100Hz 数据流回放（500 点）、序号连续性、丢帧缺口检测、坏帧不影响后续、13 字节不规则分包 |
| `tests/test_firmware_sync.py` | 5 | 固件 `app_proto.h` 与 `protocol.py` 常量逐项比对；`app_task.c` 里 STAT/SAMPLE/EVENT 的字段偏移与长度校验 |

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
解析错误      : 0
当前级别      : NORMAL
统计面板采样数: 295
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

---

## 4. 未验证项（必须上板，无法用静态方法替代）

| 项目 | 为什么必须上板 | 怎么验证 |
|---|---|---|
| 实际采样率是否 100Hz | 软件无法自证定时器频率 | 示波器/逻辑分析仪测 PA9 发送间隔，或统计 10 秒内 `n` 的增量 |
| ADC 零点与满量程 | 依赖实际 VDDA 与信号源 | PA1 接 GND 应 ≈0；接 3.3V 应 ≈3300 |
| ADC+DMA 是否真的不丢点 | 需长时间运行 | 跑 30 分钟看 `drops` 是否保持 0 |
| UART DMA 收发是否丢字节 | 需真实串口 | 长跑 + 上位机"序号缺口"统计 |
| 告警滞回手感 | 需可调信号源 | 在阈值附近缓慢来回调 |
| ORE 溢出后能否自恢复 | 需注入噪声 | 带电拔插 TX 线、短接 RX 到 GND |
| 中断优先级是否合理 | 需真实中断竞争 | 高负载下观察是否丢帧 |
| 8 小时稳定性 | 无法模拟 | 长跑 |

---

## 5. 本次实际执行到的程度（逐项打勾，不夸大）

| 内容 | 状态 |
|---|---|
| 固件编译 | ✅ 已实际执行，0 Error / 0 Warning |
| 协议编解码单测 | ✅ 已实际执行，14 项通过 |
| 端到端离线回放 | ✅ 已实际执行，4 项通过（合成 100Hz 流，非真实串口） |
| 固件/上位机常量一致性 | ✅ 已实际执行，6 项通过（**静态检查**） |
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

## 7. 2026-10-24 复审更新

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
| 2 | **seq8 清零回跳** | 上位机下发 `reset` 后，采样序号从 255 跳回 0，被误判成"丢了 255 个样本" | SAMPLE 载荷第 9 字节的 seq8 原来取 `stats.sample & 0xFF`，而 `reset` 命令会把这个计数清零 | `app_task.c` 的 `ctx_reset_stats()` 清 `stats.sample`；修前 `emit_sample()` 直接用它算 seq8 | 新增独立计数器 **`sample_seq`**（`reset` 有意不清），seq8 改取 `sample_seq & 0xFF` | 静态可证 |
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

### 7.4 构建尺寸的变化（为什么是 20584）

| 阶段 | Code | 变化 | 原因 |
|---|---|---|---|
| 整帧丢失检测合入前 | 20548 | — | README 里那个旧数字 |
| 整帧丢失检测合入后 | 20576 | +28 | 帧序号统一（`app_task_next_frame_seq` + `sample_seq`） |
| `volatile` 修复后 | 20576 | **+0** | `volatile` 只约束编译器优化，不生成指令 |
| 竞态修复 + PRIMASK 后 | **20584** | +8 | `__get_PRIMASK` / `__set_PRIMASK` 的 MRS/MSR，以及两条失败路径上的标志回置 |

> "20584 和修复前一模一样"是拿 20584 跟 20584 比：修复前的基数是 **20576**。
> 本文数字取自 2026-10-24 20:29 的全量重编译，不是从旧日志抄的。

### 7.5 本轮实际执行结果

```
UV4.exe -r DataAcq.uvprojx                                 # 2026-10-24 20:29
Program Size: Code=20584  RO-data=976  RW-data=108  ZI-data=4956
"DataAcq\DataAcq.axf" - 0 Error(s), 0 Warning(s).        warning 计数：0

python -m unittest discover -s tests -v                    Ran 24 tests — OK

QT_QPA_PLATFORM=offscreen python tests/gui_smoke.py
  采样点 293 / 告警事件 2 / 序号缺口 0 / 整帧丢失 0 / 解析错误 0
  注入缺口检测  : OK（期望 +2，实得 +2）
  BOOT 重新对齐 : OK
  RESULT: PASS
```
