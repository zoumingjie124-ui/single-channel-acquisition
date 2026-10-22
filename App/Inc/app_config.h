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
#define APP_FW_VERSION          0x0500u   /* V5.0 */

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
#if ((APP_SAMPLE_RING_SIZE & (APP_SAMPLE_RING_SIZE - 1u)) != 0u)
#error "APP_SAMPLE_RING_SIZE must be a power of two"
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
#if (APP_THR_CRIT_MV <= APP_THR_WARN_MV)
#error "APP_THR_CRIT_MV must be greater than APP_THR_WARN_MV"
#endif
#if (APP_THR_CRIT_MV > APP_ADC_VREF_MV)
#error "APP_THR_CRIT_MV must not exceed APP_ADC_VREF_MV"
#endif
#if (APP_REPORT_DECIM == 0u)
#error "APP_REPORT_DECIM must be >= 1"
#endif

#endif /* __APP_CONFIG_H */
