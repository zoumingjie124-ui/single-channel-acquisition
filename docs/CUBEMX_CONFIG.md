# CubeMX 配置对照表（本项目没有 .ioc）

## 先说清楚：为什么没有 `DataAcq.ioc`

这个工程的 `Core/` 初始化代码是**手写**的，不是 CubeMX 生成后保留的产物。

`.ioc` 在项目早期就被**主动删除**了，原因很具体：

> 只要有人双击 `.ioc` 打开 CubeMX、点一次 "GENERATE CODE"，CubeMX 就会按它自己的模板
> 重新生成 `Core/Src/*.c` 和 `stm32f1xx_hal_msp.c`，**覆盖掉手写的初始化逻辑**
> （比如 ADC 的三个 DMA 回调、USART 的 IDLE 接收、以及 App/ 层的所有接线）。
> 另一个项目 `DataTerm` 就是这么丢过一次代码，所以这个项目从一开始就不留 `.ioc`。

**补一个 `.ioc` 进仓库反而是风险**：它和手写代码之间没有"谁说了算"的机制，
一旦不一致，下一个人会信 `.ioc`，然后重新生成，代码就没了。

## 替代方案：把配置逐项列出来（本文件）

下面每个值都能在代码里逐字核对，出处写在最后一列。
**面试官关心的两个核心问题，答案在这两行**：

- DMA 模式 = **Circular（循环）**，不是 Normal
- 数据宽度 = **Half Word（16 位）**，不是 Word

### 时钟

| 项 | 值 | 出处 |
|---|---|---|
| HSE | 8 MHz 晶体，`RCC_HSE_ON` | `main.c` SystemClock_Config |
| PLL | ×9 → SYSCLK 72 MHz | 同上 |
| AHB / APB2 / APB1 | /1 → 72 MHz / /1 → 72 MHz / /2 → 36 MHz | 同上 |
| ADC 时钟 | PCLK2 / 6 = **12 MHz**（F1 上限 14 MHz） | `main.c: __HAL_RCC_ADC_CONFIG(RCC_ADCPCLK2_DIV6)` |

### ADC1

| 项 | 值 | 出处 |
|---|---|---|
| 通道 | `ADC_CHANNEL_1`（**PA1**，单通道） | `adc.c` |
| 扫描模式 | Disable（单通道无需扫描） | `adc.c` |
| 连续转换 | **Disable**（靠外部触发，一次一个） | `adc.c` |
| 外部触发 | `ADC_EXTERNALTRIGCONV_T3_TRGO` | `adc.c` |
| 数据对齐 | Right（右对齐） | `adc.c` |
| 转换个数 | 1 | `adc.c` |
| 分辨率 | 12 位（F1 硬件固定，无分辨率寄存器） | — |
| 采样时间 | 55.5 cycles | `adc.c` |
| 校准 | 上电执行 `HAL_ADCEx_Calibration_Start` | `adc.c` |

### DMA1_Channel1（ADC）

| 项 | 值 | 出处 |
|---|---|---|
| 方向 | Peripheral → Memory | `stm32f1xx_hal_msp.c` |
| 模式 | **`DMA_CIRCULAR`（循环）** | 同上 |
| 外设/内存数据宽度 | **`DMA_PDATAALIGN_HALFWORD` / `DMA_MDATAALIGN_HALFWORD`** | 同上 |
| 外设地址自增 | Disable | 同上 |
| 内存地址自增 | Enable | 同上 |
| 优先级 | High | 同上 |
| 缓冲长度 | 2（半满/全满各 1 个样本，延迟 ≤10 ms） | `app_config.h: ADC_DMA_BUF_LEN` |

> 注意：`HAL_ADC_Start_DMA()` 的原型是 `uint32_t*`，但 DMA 实际按**半字**搬运，
> 所以缓冲必须声明为 `volatile uint16_t`，并在调用处强制转换。这是 F1 HAL 的已知设计。

### TIM3（ADC 触发源）

| 项 | 值 | 出处 |
|---|---|---|
| PSC / ARR | 7199 / 99 | `tim.c` |
| 计数频率 | 72 MHz / 7200 = 10 kHz | 同上 |
| 更新频率 | 10 kHz / 100 = **100 Hz** | 同上 |
| TRGO | `TIM_TRGO_UPDATE`（把更新事件输出成 TRGO） | 同上 |

> 为什么不用 TIM2 的 CC2 触发：F1 的 ADC 外部触发源是固定的几个
> （`T1_CC1 / T2_CC2 / T3_TRGO / T4_CC4`），用 TIM2 的 CC2 会让 1 ms 节拍
> 和 10 ms 采样互相牵制，所以拆成两个定时器各司其职。

### TIM2（系统节拍）

| 项 | 值 | 出处 |
|---|---|---|
| PSC / ARR | 71 / 999 | `tim.c` |
| 频率 | 72 MHz / 72 / 1000 = **1 kHz（1 ms）** | 同上 |

### USART1

| 项 | 值 | 出处 |
|---|---|---|
| 引脚 | PA9 = TX，PA10 = RX | `usart.c` / `msp.c` |
| 参数 | 115200 · 8 数据位 · 无校验 · 1 停止位 | `usart.c` |
| TX | **DMA1_Channel4**（非阻塞） | `msp.c` |
| RX | **DMA1_Channel5 + 空闲中断（IDLE）** | `msp.c` / `usart.c` |

### GPIO

| 引脚 | 用途 | 模式 |
|---|---|---|
| PA1 | 模拟输入 | Analog，无上下拉 |
| PA0 | 按键 | 上拉输入 + 下降沿中断（EXTI0） |
| PA9 / PA10 | USART1 | 复用推挽 / 浮空输入 |
| PA13 / PA14 | SWD | 调试口，禁止挪用 |
| PC13 | 板载 LED | 推挽输出，低电平点亮 |

## 如果你仍然要一个 .ioc

可以按上表在 CubeMX 里重建，但**生成前务必**：

1. 先把 `Core/Src`、`Core/Inc`、`App/` 整目录备份；
2. 在 CubeMX 里把 `Project Manager → Code Generator` 的
   "Delete previously generated files" 关掉；
3. 生成后 `git diff` 逐行确认，别整目录覆盖。

不推荐，但如果你确实需要给面试官看 CubeMX 界面，这是唯一安全的做法。
