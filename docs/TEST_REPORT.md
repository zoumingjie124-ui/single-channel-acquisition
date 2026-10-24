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

### 7.1 本轮实际改了什么（不限于文档）

| 提交 | 类型 | 内容 |
|---|---|---|
| `b1b1d63` | **固件 + 上位机** | 帧头 SEQ 改为设备全局帧序号；涉及 `App/Src/app_task.c`、`App/Src/app_cmd.c`、`App/Inc/app_task.h`、`Core/Src/usart.c`；上位机新增整帧丢失检测 |
| `8884244` | **固件** | `s_dma_buf` 补 `volatile` |
| `81e1705` | **固件** | 恢复标志竞态修复、计数器清零加临界区、环形缓冲断言补下界 |
| `fa8496c` | 文档 / 测试 | 用例数更正、`gui_smoke` 增强、协议补充 |

> **勘误**：上一版本节写的是"本轮只改文档与测试，固件业务逻辑未改动"。
> 这句是错的 —— 本轮确实改了 `app_task.c`、`app_cmd.c`、`usart.c`。
> 本文档最大的价值是"说过的话都能对得上"，所以这处必须改，而不是悄悄删掉。

### 7.2 四处关键修复（现象 / 根因 / HAL 依据 / 修法）

| # | 现象 | 根因 | HAL 依据 | 修法 |
|---|---|---|---|---|
| 1 | 编译 0 Error、DMA 在跑，但上位机**一帧都收不到**，采样环恒为空 | HAL 只负责把 ADC 结果搬进 `s_dma_buf`；"取走并投递"必须由用户在回调里完成。没有回调，数据被下一次转换直接覆盖 | `ADC_DMAConvCplt()`（`stm32f1xx_hal_adc.c:2333`）只更新状态并调 `HAL_ADC_ConvCpltCallback()`；HAL 不搬运、不缓存样本 | `adc.c` 实现 `ConvHalfCpltCallback` / `ConvCpltCallback` / `ErrorCallback` → 分别调 `app_measure_on_half/_on_cplt/_on_error` |
| 2 | 连续发帧时偶发 `HAL_BUSY`，整帧丢失 | `HAL_UART_Transmit_DMA()` 在上一帧未完成时**不排队，直接返回 BUSY** | `HAL_UART_Transmit_DMA()` 首行 `if (huart->gState == HAL_UART_STATE_READY)`，否则直接返回 `HAL_BUSY`（不排队） | `app_tx.c` 单帧串行：发完（`TxCpltCallback`）才取下一批；`s_busy` 标志由回调清除 |
| 3 | 接收不定长命令时，短帧要**等缓冲填满**才上交，响应延迟大 | F1 的基础 DMA 接收没有"不定长"语义，只能按满长度触发 | `HAL_UARTEx_ReceiveToIdle_DMA()` + `HAL_UARTEx_RxEventCallback()`：IDLE 与 TC 都会先 abort DMA、把 `RxState` 置 READY 再回调；**HT 事件也会进来** | `usart.c` 用 `ReceiveToIdle_DMA`；回调里只处理 `Size`，HT 直接忽略（否则同一批数据会被重复上交） |
| 4 | 一次噪声（ORE）之后，接收/采样**永久停摆**，且无任何提示 | HAL 的错误路径只回调 `HAL_ADC_ErrorCallback` / `HAL_UART_ErrorCallback`，**不会自动重启**传输；`s_busy` 之类标志若不清，队列永远卡死 | `ADC_DMAError()` 只置 `HAL_ADC_STATE_ERROR_DMA` 并回调 `HAL_ADC_ErrorCallback()`（weak 空函数），**不重启任何传输** | ADC：ISR 只置标志 → 主循环 `ADC1_RecoverInit()`（DeInit→Init→配通道→校准）+ `app_measure_start()`，限速 1 s。串口：`ErrorCallback` 清 ORE 并**仅在 `gState != BUSY_TX` 时**释放发送队列 |

第 2、4 条最容易踩的坑是同一处：**错误回调里不能无条件重置发送状态**。
接收侧的 ORE/FE/NE 也会进 `HAL_UART_ErrorCallback`，此时 TX DMA 可能还在跑；
把 `s_busy` 清零会让主循环立刻再调 `HAL_UART_Transmit_DMA()` 并拿到 BUSY，
这批已出队的数据就被误记成"丢弃"——既真丢数据，又让统计失真。

### 7.3 构建尺寸的变化（为什么是 20584）

| 阶段 | Code | 变化 | 原因 |
|---|---|---|---|
| 整帧丢失检测合入前 | 20548 | — | README 里那个旧数字 |
| 整帧丢失检测合入后 | 20576 | +28 | 帧序号统一 + 上位机侧对应改动 |
| `volatile` 修复后 | 20576 | **+0** | `volatile` 只约束编译器优化，不生成指令，尺寸不变是正常的 |
| 竞态修复 + PRIMASK 后 | **20584** | +8 | `__get_PRIMASK` / `__set_PRIMASK` 的 MRS/MSR，以及两条失败路径上的标志回置 |

> 所以"20584 和修复前一模一样"是误读：修复前是 **20576**，不是 20584。
> 本文数字取自 2026-10-24 20:29 的全量重编译（`UV4.exe -r`），不是从旧日志抄的。

### 7.4 本轮实际执行结果

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
