# STM32 单通道模拟量采集与故障监测终端

STM32F103C8T6 + HAL + Keil MDK。100Hz 定时器触发 ADC+DMA 采样 → 移动平均 → 三级告警状态机  
→ 二进制帧协议经 DMA 上报 → Python 上位机实时双曲线 / 在线改阈值 / CSV 导出 / 异常统计。

> **验证状态（务必先看）**
>
> - 固件：✅ **已实际编译通过**，`0 Error / 0 Warning`，Code 20576B / RO 976B / RW 108B / ZI 4956B
> - 协议层单测：✅ **24 项全部通过**（`host/tests/`，含固件常量一致性检查）
> - 上位机界面：✅ **已离屏实际运行 3 秒并通过**（含“整帧丢失”恒为 0 的断言 + 注入缺口的反向验证），截图见 `docs/gui_demo.png`
> - **上板实测：❌ 完全没有做过。** 本文所有功能描述都是"设计预期"，不是实测结论。

---

## 1. 与 V1 的差别

| 方面     | V1（DataTerm）                             | V5（本项目）                                 |
| ------ | ---------------------------------------- | --------------------------------------- |
| ADC 采样 | 轮询 `Start/Poll/GetValue/Stop`，CPU 参与每次转换 | **TIM3 TRGO 触发 → ADC → DMA 循环**，CPU 不参与 |
| 串口发送   | `HAL_UART_Transmit` 阻塞，一帧 5.2ms          | **DMA + 环形发送队列**，主循环零等待                 |
| 串口接收   | 逐字节中断 + 环形缓冲                             | **DMA + 空闲中断（IDLE）**，一次拿一批              |
| 上报协议   | ASCII 行                                  | **二进制帧**（帧头 + 类型 + 长度 + 序号 + CRC16）     |
| 上报速率   | 10Hz                                     | **100Hz**                               |
| 丢点     | 布尔标志，不可观测                                | **环形缓冲溢出计数**，上位机可见                      |
| 告警状态机  | 允许跨级跳转                                   | **只允许相邻跳转**，路径可追溯                       |
| 异常统计   | ADC/命令两类                                 | ADC / UART / 接收 / 帧 / 发送丢弃，共 5 类        |
| 上位机    | 无                                        | 双曲线、阈值下发、统计面板、CSV、事件表                   |

---

## 2. 目录结构

```
Data acquisition/
├── Core/                    ← 外设层（CubeMX 风格）
│   ├── Src/main.c                启动顺序、时钟 72MHz
│   │     adc.c                   ADC1 + T3_TRGO 触发
│   │     tim.c                   TIM2(1ms 节拍) / TIM3(100Hz 触发)
│   │     usart.c                 DMA 收发 + IDLE 接收
│   │     dma.c                   DMA1 三通道
│   │     gpio.c                  LED / 按键 EXTI
│   │     stm32f1xx_hal_msp.c     时钟、GPIO、DMA、NVIC 绑定
│   │     stm32f1xx_it.c          中断入口，只做转发
│   └── Inc/
├── App/                     ← 应用层（不直接调 HAL，app_measure 除外）
│   ├── Src/app_task.c            调度、流水线、帧发送、LED
│   │     app_measure.c           ADC+DMA 采样隔离层
│   │     app_filter.c            移动平均 + 可选限幅
│   │     app_alarm.c             三级状态机
│   │     app_proto.c             帧编解码 + CRC16
│   │     app_tx.c                发送环形队列
│   │     app_cmd.c               双通道命令
│   └── Inc/app_config.h          所有可调参数集中在此
├── Drivers/                 ← ST HAL 库 + CMSIS（含 Legacy 与 LL 头文件）
├── MDK-ARM/DataAcq.uvprojx  ← 双击打开
├── host/                    ← PC 上位机
│   ├── protocol.py               帧编解码（与固件一一对应）
│   ├── link.py                   串口读线程
│   ├── main.py                   PySide6 界面
│   ├── tests/                    24 项单测 + 界面冒烟测试
│   └── tools/serial_dump.py      命令行抓帧（只需 pyserial）
└── docs/
    ├── PROTOCOL.md             协议逐字段说明
    ├── CUBEMX_CONFIG.md        CubeMX 配置对照表（本项目无 .ioc，原因见文内）
    └── TEST_REPORT.md          编译/测试记录与待办
```

---

## 3. 编译与下载

**Keil 打开**：双击 `MDK-ARM/DataAcq.uvprojx`

**命令行编译**：

```bat
cd MDK-ARM
D:\K5\UV4\UV4.exe -b DataAcq.uvprojx -j0 -o build.log
```

期望输出：

```
Program Size: Code=20576  RO-data=976  RW-data=108  ZI-data=4956
"DataAcq\DataAcq.axf" - 0 Error(s), 0 Warning(s).
```

**下载配置**（一次性）：  
`Project → Options for Target` → `Debug` 页选 **ST-Link Debugger** → `Settings` → `Port = SW`  
→ `Utilities` 页勾 `Use Target Driver for Flash Programming` → `Settings` → 勾 **Reset and Run**

---

## 4. 接线

与 DataTerm 相同，详见 `docs/PROTOCOL.md` 第 1 节与下方要点：

| STM32          | 接到                    | 说明                              |
| -------------- | --------------------- | ------------------------------- |
| PA1 (ADC1_IN1) | 电位器中间抽头               | **只能 0~3.3V**，禁止负压与 5V          |
| PA9            | USB-TTL **RXD**       | 交叉接                             |
| PA10           | USB-TTL **TXD**       | 交叉接                             |
| GND            | USB-TTL GND           | **必须共地**                        |
| PA0            | 按键 → GND              | 内部上拉，下降沿                        |
| PC13           | 板载 LED                | NORMAL 灭 / WARN 2Hz 闪 / CRIT 常亮 |
| PA13 / PA14    | ST-Link SWDIO / SWCLK | GND 必接，3.3V 与 USB 供电只接一路        |

⚠️ 电气红线：PA1 输入超 3.3V 会通过保护二极管灌电流，可能永久损坏 ADC。

---

## 5. 运行上位机

```bat
cd host
pip install -r requirements.txt

python main.py            :: 真实串口
python main.py --demo     :: 无硬件演示（内置 100Hz 信号源，走同一套解析路径）
```

界面功能：

- 串口选择 / 刷新 / 连接断开
- 原始值（灰）与滤波值（蓝）双曲线，叠加 WARN/CRIT 阈值虚线
- 当前级别色块（绿 / 琥珀 / 红）
- 运行时间、采样数、丢点、异常、状态切换、**序号缺口**
- 在线改阈值并下发（带与固件一致的合法性校验）
- 读取统计 / 清零 / LED 开关
- CSV 记录（`pc_time, t_ms, raw_mv, filt_mv, level`）
- 告警事件表 + 文本通道日志

**不在界面里做的事**：断开后不自动重连（当前版本是手动点"连接"），  
避免串口被反复打开关闭导致驱动异常。这是有意的行为，不是缺陷。

---

## 6. 关键设计说明（面试会问）

**为什么用两个定时器**  
F1 的 ADC 外部触发只能用固定的几个源（`T1_CC1` / `T2_CC2` / `T3_TRGO` / `T4_CC4` / `EXT_IT11`），  
没有"通用 TRGO 输入"。若让 TIM2 的 CC2 触发，1ms 节拍与 10ms 采样会互相牵制。  
所以 TIM2 专职 1ms 时基，TIM3 专职 100Hz 触发。

**为什么 ADC DMA 缓冲用 uint16_t 数组却强转 uint32_t***  
F1 的 `HAL_ADC_Start_DMA()` 原型是 `uint32_t*`，但 DMA 实际按**半字**搬运。  
声明成 `uint32_t[]` 会让两个样本挤进同一个元素，索引全错。这是 F1 上很常见的坑。

**为什么发送必须走 DMA**  
16B 帧 @115200 = 1.39ms。阻塞发送在 10ms 采样周期里占 13.9%，  
会造成采样时刻抖动；DMA 后台搬运时主循环耗时只有一次 memcpy。

**为什么状态机不允许跨级**  
"正常→预警→确诊"是业务语义，每级都有独立进入条件，故障升级路径可追溯。  
代价是电压从 0 跳到 3200mV 需要 2 轮确认（60ms），对电压监测可接受。

**为什么 SEQ 和 CRC 都要**  
CRC 只能发现内容被改坏，发现不了整帧丢失；SEQ 不连续才能暴露丢帧。

---

## 7. 验证状态（诚实版）

| 内容                   | 状态                                                 |
| -------------------- | -------------------------------------------------- |
| 固件编译                 | ✅ 已实际编译通过（0 Error / 0 Warning）                     |
| 协议层编解码               | ✅ 已用 24 项单测验证（CRC 标准向量、分包、坏 CRC、超长 LEN、文本混流、端到端回放） |
| 固件与上位机协议一致性          | ⚠️ 两端 `struct` 长度已对齐并各有单测，但**尚未做过真实联调**            |
| 上位机界面                | ✅ 已离屏实际运行并断言（3 秒约 290 点、整帧丢失 0、解析错误 0）；另注入一次序号跳变验证检测确实生效；未接真实串口 |
| ADC 实际精度 / 100Hz 是否准 | ❌ **必须上板**（示波器或逻辑分析仪看 PA9 / 采样点间隔）                 |
| DMA 收发是否真的不丢数据       | ❌ **必须上板**（长跑 + 序号缺口统计）                            |
| 告警滞回手感               | ❌ **必须上板**                                         |
| 8 小时稳定性              | ❌ 未做                                               |

**没有运行过的东西，本文件不会写成"已验证"。**

---

## 8. 已知限制

1. **未上板验证**——这是最大的限制，所有时序数字都是计算值。
2. 断开串口后不自动重连（手动点连接）。
3. `SEQ` 与 `seq8` 都是 8 位，连续丢超过 256 帧时缺口统计会失真（实际不会发生）。
4. 阈值掉电不保存，重启回默认 2400/3000。
5. 单通道；多通道需要改 ADC 扫描模式与 DMA 缓冲布局。
6. 未使用 FreeRTOS，裸机超级循环。
