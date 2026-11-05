/**
  ******************************************************************************
  * @file    app_config.h
  * @brief   全局配置：所有可调参数与硬件假设集中于此。
  *
  * 设计原则：
  *   1. 改采样率 / 阈值 / 缓冲尺寸只改本文件；
  *   2. 尺寸类参数必须是 2 的幂（索引用掩码取模），写错在编译期报错；
  *   3. 阈值顺序错误同样在编译期拦住，不让它跑到运行期才炸。
  ******************************************************************************
  */
#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

/* ------------------------------- 固件版本 ------------------------------- */
#define APP_FW_VERSION          0x0500u   /* 协议大版本 V5（写进 BOOT 帧，上位机显示用）。
                                             只有"帧格式/字段语义"变化才进位；
                                             代码小改动不必动它，否则版本号会失去意义 */

/* ----------------------------- 时间与采样率 ----------------------------- */
/* TIM2 系统节拍：1ms，供 uptime 与调度使用 */
#define APP_TICK_HZ             1000u
/* TIM3 触发 ADC 的频率。10ms 周期 --> 100Hz */
#define APP_SAMPLE_RATE_HZ      100u
/* 每 N 个采样向 PC 发一帧。1 = 100Hz 全量上报（16B/帧 --> 1600B/s） */
#define APP_REPORT_DECIM        1u
/* 定期推送 STAT 帧的周期（ms），让上位机统计面板自动刷新 */
#define APP_STAT_PERIOD_MS      1000u

/* -------------------------------- ADC 参数 ------------------------------ */
#define APP_ADC_VREF_MV         3300u
#define APP_ADC_FULL_SCALE      4095u
/* ADC -> DMA 缓冲长度。2 表示半满/全满各 1 个样本，延迟最小（<=10ms） */
#define ADC_DMA_BUF_LEN         2u
/* ADC 出错后重启的最小间隔（ms）。这是限速阀：失败时每秒最多重试一次，
   避免在主循环里高频 DeInit/Init 把 CPU 占满，也避免掩盖真实硬件故障 */
#define APP_ADC_RECOVER_INTERVAL_MS  1000u

/* 二进制解析器的空闲超时（ms）：半截帧超过这个时间没有新字节，就复位解析状态。
   依据：最长帧 47 B（帧头2 + TYPE/LEN/SEQ 3 + 载荷40 + CRC2）在 115200 下的
   线上时间是 4.08 ms；且 47 B < CH340 的 USB 单包 64 B，一帧必然落在同一包里，
   包内字节连续。取 50 ms => 12.3 倍余量，正常帧不会被误清理。 */
#define APP_RX_FRAME_TIMEOUT_MS      50u

/* RX DMA 重挂失败后的重试间隔（ms）：首次立即重试，其后按此间隔限速，
   避免"接收已死"时在每轮主循环里反复启动 DMA。 */
#define APP_RX_RESTART_INTERVAL_MS   200u

/* ---------------------------- 调试：故障注入开关 ------------------------- */
/* 1 = 编入调试注入文本命令（adcinject / oreinject / rxstop），仅 Stage 1/3 验证使用；
   0 = 这些命令完全不参与编译，固件行为/尺寸与正式版逐字节一致（Release 必须用 0）。
   ⚠ adcinject 注入的是"错误标志"（走 app_measure_on_error()），并非真实 DMA 电气故障；
     测试报告里必须如实区分注入等级（C1/C2），不能当作自然发生的硬件故障。 */
#define APP_DEBUG_INJECT        0u

/* ------------------------------- 采样环形缓冲 --------------------------- */
/* DMA 回调写入，主循环取走。若溢出则丢新样本并计数（可观测的丢点） */
#define APP_SAMPLE_RING_SIZE    64u
/* 主循环单次 poll 最多处理的样本数，防止一次吃太多导致响应变慢 */
#define APP_SAMPLE_MAX_PER_RUN  8u

/* -------------------------------- 滤波参数 ------------------------------ */
/* 移动平均深度：8 点 = 80ms 窗口 */
#define APP_FILTER_DEPTH        8u
/* 限幅滤波：单步允许的最大变化量（mV）。0 = 关闭。
   必须大于信号正常最大变化率，否则会把真实跳变削平。 */
#define APP_RATE_LIMIT_MV       0u

/* -------------------------------- 告警参数 ------------------------------ */
#define APP_THR_WARN_MV         2400u
#define APP_THR_CRIT_MV         3000u
#define APP_THR_HYST_MV         50u
#define APP_ALARM_DEBOUNCE_CNT  3u

/* ----------------------------- 故障诊断参数 ---------------------------- */
/* Stage 7B：信号卡死检测。**默认关闭** —— 打开前需先用真实传感器标定
   APP_DIAG_STUCK_DELTA_MV，否则稳定的正常输入会被误判（见 STAGE7 设计文档 §2.2.4）。 */
#define APP_DIAG_STUCK_ENABLE_DEFAULT   0u

/* 卡死判定阈值：一个块内 (max-min) 小于它即记为"偏死块"。
   依据：实测静置峰峰 11 mV（σ 2.40 mV）、电位器接触噪声峰峰 18 mV，
   50 mV 约为前者的 4.5 倍；≈ 满量程 1/66。
   ⚠ 待确定：真实传感器若本来就很稳，3 s 内变化 < 50 mV 属正常。 */
#define APP_DIAG_STUCK_DELTA_MV         50u

/* 块长度。1 s 与 STAT 周期对齐，便于人工比对日志 */
#define APP_DIAG_BLOCK_MS               1000u

/* 连续多少个偏死块才确认卡死。3 × 1 s = 3 s（用户要求的 N 秒） */
#define APP_DIAG_STUCK_CONFIRM_BLOCKS   3u

/* 连续多少个干净块才恢复。取 2 而不是 1：
   实测接触噪声最大单步 +38 mV，一个毛刺就能造出一个"干净块"，
   要求连续 2 块（2 s）可避免故障被瞬间清掉。 */
#define APP_DIAG_STUCK_RECOVER_BLOCKS   2u

/* 一个块至少要有这么多样本才参与判定。
   依据：100 Hz 下 1 s = 100 个样本，取一半（50）意味着"这个块至少有一半数据"，
   其 span 才有统计意义。样本不足的块会被计入 uneval_blocks 并跳过——
   只有一个样本时 span 恒为 0，那是"没数据"不是"没变化"。 */
#define APP_DIAG_MIN_BLOCK_SAMPLES      50u

/* Stage 7C-2：故障状态帧提交失败后的最小重试间隔（ms）。
   作用：发送环长期满时，不能每 10 ms 采样周期都去试一次（那会把主循环的
   空闲时间全吃掉，也会在日志里刷屏）。100 ms 的依据是：
   环容量 1024 B、100 Hz 下 16 B/帧 ≈ 1.6 KB/s，正常 640 ms 才能写满；
   给 100 ms 已经比"正常排空一帧"快一个数量级，足够及时。
   ⚠ 待确定：如果将来把上报帧率调高或环改小，这个值需要重新算。 */
#define APP_FAULT_RETRY_MIN_MS          100u

/* ------------------------------ 串口与协议 ------------------------------ */
/* 发送环形缓冲：100Hz × 16B = 1600B/s，1024B 可缓冲 640ms，足够 */
#define APP_TX_RING_SIZE        1024u
/* DMA 发送的线性暂存区长度（HAL 的 DMA 需要连续内存） */
#define APP_TX_STAGE_SIZE       128u
/* 接收：DMA + IDLE，单次最多收这么多字节 */
#define APP_RX_DMA_BUF_LEN      64u
/* 接收环形缓冲：DMA 回调写入，主循环解析 */
#define APP_RX_RING_SIZE        256u
/* 文本命令行（调试通道）单行最大长度 */
#define APP_LINE_BUF_SIZE       64u

/* -------------------------- 编译期约束检查 -------------------------- */
#if ((APP_FILTER_DEPTH & (APP_FILTER_DEPTH - 1u)) != 0u)
#error "APP_FILTER_DEPTH must be a power of two"
#endif
#if ((APP_SAMPLE_RING_SIZE & (APP_SAMPLE_RING_SIZE - 1u)) != 0u) || (APP_SAMPLE_RING_SIZE < 2u)
#error "APP_SAMPLE_RING_SIZE must be a power of two and >= 2"
#endif
#if ((APP_TX_RING_SIZE & (APP_TX_RING_SIZE - 1u)) != 0u)
#error "APP_TX_RING_SIZE must be a power of two"
#endif
#if ((APP_RX_RING_SIZE & (APP_RX_RING_SIZE - 1u)) != 0u)
#error "APP_RX_RING_SIZE must be a power of two"
#endif
#if ((ADC_DMA_BUF_LEN < 2u) || ((ADC_DMA_BUF_LEN & 1u) != 0u))
#error "ADC_DMA_BUF_LEN must be even and >= 2 (half/full transfer)"
#endif
#if (APP_THR_WARN_MV <= APP_THR_HYST_MV)
#error "APP_THR_WARN_MV must be greater than APP_THR_HYST_MV"
#endif
#if (APP_THR_CRIT_MV < (APP_THR_WARN_MV + APP_THR_HYST_MV))
#error "APP_THR_CRIT_MV must be at least APP_THR_WARN_MV + APP_THR_HYST_MV"
#endif
#if (APP_THR_CRIT_MV > APP_ADC_VREF_MV)
#error "APP_THR_CRIT_MV must not exceed APP_ADC_VREF_MV"
#endif
#if (APP_REPORT_DECIM == 0u)
#error "APP_REPORT_DECIM must be >= 1"
#endif

/* ------------------------- 故障诊断参数的编译期约束 ------------------------- */
#if (APP_DIAG_STUCK_ENABLE_DEFAULT > 1u)
#error "APP_DIAG_STUCK_ENABLE_DEFAULT must be 0 or 1"
#endif
#if (APP_DIAG_STUCK_CONFIRM_BLOCKS < 1u)
#error "APP_DIAG_STUCK_CONFIRM_BLOCKS must be >= 1"
#endif
#if (APP_DIAG_STUCK_RECOVER_BLOCKS < 1u)
#error "APP_DIAG_STUCK_RECOVER_BLOCKS must be >= 1"
#endif
#if (APP_DIAG_BLOCK_MS < 100u)
#error "APP_DIAG_BLOCK_MS too short: one block would hold almost no samples"
#endif
#if (APP_DIAG_STUCK_DELTA_MV == 0u)
#error "APP_DIAG_STUCK_DELTA_MV must be > 0 (0 would make every block look stuck)"
#endif
/* 单块最多能装多少样本：BLOCK_MS / 采样周期。
   周期 = 1000 / APP_SAMPLE_RATE_MS，所以样本数 = BLOCK_MS * RATE / 1000。
   MIN_BLOCK_SAMPLES 超过这个数 => 每个块都"样本不足" => 诊断永远不工作。 */
#if (APP_DIAG_MIN_BLOCK_SAMPLES > ((APP_DIAG_BLOCK_MS * APP_SAMPLE_RATE_HZ) / 1000u))
#error "APP_DIAG_MIN_BLOCK_SAMPLES exceeds max samples per block; diagnosis would never run"
#endif
/* 重试间隔必须明显大于采样周期，否则等于每样本重试一次 */
#if (APP_FAULT_RETRY_MIN_MS < 10u)
#error "APP_FAULT_RETRY_MIN_MS too small: retry would happen almost every sample"
#endif

#endif /* __APP_CONFIG_H */
