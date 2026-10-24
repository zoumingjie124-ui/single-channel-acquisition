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

## 9. 2026-10-24 复审更新

本轮**只改文档与测试**，固件业务逻辑未改动。

| 项 | 说明 |
|---|---|
| 重新编译 | `Code=20584  RO=976  RW=108  ZI=4956`，0 Error 0 Warning。原 README 引用的 20548 是"整帧丢失检测"合入之前的数字，已更正 |
| 用例数更正 | 原报告写 14 / 5 / 5，实测为 **14 / 4 / 6**（`test_protocol` 14、`test_pipeline` 4、`test_firmware_sync` 6），合计仍是 24 |
| 界面测试增强 | `tests/gui_smoke.py` 新增三项检查：① 演示流跑 3 秒后"整帧丢失"必须为 0；② 注入 `seq 10 → 13` 后计数必须 +2；③ 注入 `BOOT` 后重新对齐且不计缺口。第 ②③ 项是反向验证——防止"恒为 0"其实是功能没实现 |
| 协议补充 | `PROTOCOL.md` 4.4 增加说明：ACK 的帧头 `SEQ` 是设备全局帧序号，不回显命令序号；上位机做丢帧判断时不能跳过 ACK |

### 本轮实际执行结果

```
UV4.exe -r DataAcq.uvprojx
Program Size: Code=20584  RO-data=976  RW-data=108  ZI-data=4956
"DataAcq\DataAcq.axf" - 0 Error(s), 0 Warning(s).        warning 计数：0

python -m unittest discover -s tests -v                    Ran 24 tests — OK

QT_QPA_PLATFORM=offscreen python tests/gui_smoke.py
  采样点 293 / 告警事件 2 / 序号缺口 0 / 整帧丢失 0 / 解析错误 0
  注入缺口检测  : OK（期望 +2，实得 +2）
  BOOT 重新对齐 : OK
  RESULT: PASS
```
