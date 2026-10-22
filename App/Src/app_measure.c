/**
  ******************************************************************************
  * @file    app_measure.c
  * @brief   ADC + DMA 采样实现。
  *
  * 数据流：
  *   TIM3(TRGO/100Hz) 触发 -> ADC1 转换 -> DMA1_Channel1 写入 s_dma_buf
  *     半满/全满回调 -> 搬进 s_ring -> 主循环 app_measure_pop()
  *
  * 关键点（F1 专有，容易踩）：
  *   1. F1 的 HAL_ADC_Start_DMA 原型是 uint32_t*，但 DMA 实际按半字搬运，
  *      所以这里用 uint16_t 数组再强制转换。若声明成 uint32_t 数组，
  *      两个样本会被塞进同一个元素，索引全错。
  *   2. F1 的 ADC 外部触发只能用 T2_CC2 / T3_TRGO 等固定源（没有通用的 TRGO 输入），
  *      这里选 TIM3 的 TRGO（更新事件），与 1ms 节拍的 TIM2 分开，互不干扰。
  ******************************************************************************
  */
#include "app_measure.h"
#include "app_config.h"
#include "adc.h"

#define RING_MASK ((uint16_t)(APP_SAMPLE_RING_SIZE - 1u))

/* DMA 目标缓冲。半字数组 + 强制转换，见文件头说明第 1 条 */
static uint16_t s_dma_buf[ADC_DMA_BUF_LEN];

/* 采样环形缓冲：ISR 写 head，主循环写 tail */
static uint16_t s_ring[APP_SAMPLE_RING_SIZE];
static volatile uint16_t s_head;
static volatile uint16_t s_tail;

static volatile uint32_t s_drop;   /* 环形缓冲满，丢弃的样本数 */
static volatile uint32_t s_err;    /* ADC / DMA 错误次数 */

/* ---- 故障恢复状态 ---- */
static volatile uint8_t  s_recover_req;    /* ISR 置 1，主循环成功后清 0 */
static volatile uint32_t s_recover_ok;     /* 恢复成功次数 */
static volatile uint32_t s_recover_fail;   /* 恢复尝试失败次数 */
static uint8_t  s_try_started;             /* 是否已经尝试过（用于首次立即执行） */
static uint32_t s_last_try_ms;             /* 上次尝试的时刻，用于限速 */

static void ring_push(uint16_t raw)
{
  uint16_t next = (uint16_t)((s_head + 1u) & RING_MASK);

  if (next == s_tail)
  {
    /* 满了：丢新样本并计数。这是"可观测的丢点"，V1 里完全没有 */
    s_drop++;
    return;
  }
  s_ring[s_head] = raw;
  s_head = next;
}

static void ring_push_range(uint16_t start, uint16_t count)
{
  uint16_t i;
  for (i = 0u; i < count; i++)
  {
    ring_push(s_dma_buf[start + i]);
  }
}

void app_measure_init(void)
{
  uint16_t i;

  for (i = 0u; i < ADC_DMA_BUF_LEN; i++)
  {
    s_dma_buf[i] = 0u;
  }
  s_head = 0u;
  s_tail = 0u;
  s_drop = 0u;
  s_err  = 0u;

  s_recover_req  = 0u;
  s_recover_ok   = 0u;
  s_recover_fail = 0u;
  s_try_started  = 0u;
  s_last_try_ms  = 0u;
}

uint8_t app_measure_start(void)
{
  if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)(void *)s_dma_buf,
                        (uint32_t)ADC_DMA_BUF_LEN) != HAL_OK)
  {
    s_err++;
    return 0u;
  }
  return 1u;
}

void app_measure_on_half(void)
{
  /* 前半段：[0, LEN/2) */
  ring_push_range(0u, (uint16_t)(ADC_DMA_BUF_LEN / 2u));
}

void app_measure_on_cplt(void)
{
  /* 后半段：[LEN/2, LEN) */
  ring_push_range((uint16_t)(ADC_DMA_BUF_LEN / 2u),
                  (uint16_t)(ADC_DMA_BUF_LEN / 2u));
}

void app_measure_on_error(void)
{
  s_err++;
  /* 中断里只置标志：DeInit/Init/校准都是阻塞操作，
     放在 ISR 里会长时间占住中断，还可能和正在收尾的 DMA 抢状态 */
  s_recover_req = 1u;
}

/**
  * @brief  故障恢复（主循环调用）
  * @retval APP_MEAS_RECOVER_NONE / OK / FAIL
  *
  * 限速策略：两次尝试之间至少间隔 APP_ADC_RECOVER_INTERVAL_MS。
  * 首次出错会立刻恢复一次（要求 s_try_started 为 0），之后每次失败
  * 至少等 1 秒再试，避免"失败-重试"打成忙循环把主循环拖死。
  */
uint8_t app_measure_poll_recovery(void)
{
  uint32_t now;

  if (s_recover_req == 0u)
  {
    return APP_MEAS_RECOVER_NONE;
  }

  now = HAL_GetTick();
  if ((s_try_started != 0u) &&
      ((uint32_t)(now - s_last_try_ms) < APP_ADC_RECOVER_INTERVAL_MS))
  {
    return APP_MEAS_RECOVER_NONE;
  }
  s_try_started = 1u;
  s_last_try_ms = now;

  /* 第一步：把 ADC + DMA 复位到上电状态（失败则本次放弃，等下一轮） */
  if (ADC1_RecoverInit() == 0u)
  {
    s_recover_fail++;
    return APP_MEAS_RECOVER_FAIL;
  }

  /* 第二步：重新启动 DMA。返回值必须检查，否则会出现"看起来恢复了、
     其实根本没在搬数据"的假象 */
  if (app_measure_start() == 0u)
  {
    s_recover_fail++;
    return APP_MEAS_RECOVER_FAIL;
  }

  s_recover_req = 0u;
  s_recover_ok++;
  return APP_MEAS_RECOVER_OK;
}

uint32_t app_measure_recover_ok(void)
{
  return s_recover_ok;
}

uint32_t app_measure_recover_fail(void)
{
  return s_recover_fail;
}

uint8_t app_measure_pop(uint16_t *raw)
{
  if (raw == 0)
  {
    return 0u;
  }
  if (s_tail == s_head)
  {
    return 0u;
  }
  *raw = s_ring[s_tail];
  s_tail = (uint16_t)((s_tail + 1u) & RING_MASK);
  return 1u;
}

uint32_t app_measure_drop_count(void)
{
  return s_drop;
}

uint32_t app_measure_err_count(void)
{
  return s_err;
}

uint16_t app_measure_ring_level(void)
{
  return (uint16_t)((s_head - s_tail) & RING_MASK);
}

void app_measure_reset_counters(void)
{
  s_drop = 0u;
  s_err  = 0u;
  s_recover_ok   = 0u;
  s_recover_fail = 0u;
  /* 注意：不清 s_recover_req。若此刻正有待恢复的故障，
     清掉标志会让采样永久停摆，这是比"计数不准"严重得多的问题。 */
}

uint16_t app_measure_raw_to_mv(uint16_t raw)
{
  /* 先乘后除：raw * 3300 最大 13.5M，uint32 装得下，不会溢出 */
  return (uint16_t)(((uint32_t)raw * APP_ADC_VREF_MV) / APP_ADC_FULL_SCALE);
}
