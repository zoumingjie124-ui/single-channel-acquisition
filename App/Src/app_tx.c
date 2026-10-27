/**
  ******************************************************************************
  * @file    app_tx.c
  * @brief   发送环形缓冲 + DMA 发送实现。
  *
  * 并发模型：
  *   head  —— 只在主循环改（app_tx_write）
  *   tail  —— 只在主循环改（app_tx_poll 交出数据时推进）
  *   busy  —— 主循环写 1，中断写 0（单字节，Cortex-M3 上是原子的）
  *   因此不需要关中断，也不需要临界区。
  ******************************************************************************
  */
#include "app_tx.h"
#include "app_config.h"

static UART_HandleTypeDef *s_huart;
static uint8_t  s_ring[APP_TX_RING_SIZE];
static uint16_t s_head;
static uint16_t s_tail;
static volatile uint8_t s_busy;
static uint8_t  s_stage[APP_TX_STAGE_SIZE];
static uint32_t s_drop_bytes;

#define TX_MASK ((uint16_t)(APP_TX_RING_SIZE - 1u))

void app_tx_init(UART_HandleTypeDef *huart)
{
  s_huart      = huart;
  s_head       = 0u;
  s_tail       = 0u;
  s_busy       = 0u;
  s_drop_bytes = 0u;
}

uint16_t app_tx_pending(void)
{
  return (uint16_t)((s_head - s_tail) & TX_MASK);
}

uint8_t app_tx_is_busy(void)
{
  return s_busy;
}

uint32_t app_tx_drop_bytes(void)
{
  return s_drop_bytes;
}

uint16_t app_tx_write(const uint8_t *data, uint16_t len)
{
  uint16_t free_cnt;
  uint16_t i;
  uint16_t written;

  if ((data == 0) || (len == 0u))
  {
    return 0u;
  }

  /* 留一个空位用于区分"满"和"空" */
  free_cnt = (uint16_t)((TX_MASK + 1u) - 1u - app_tx_pending());
  if (len > free_cnt)
  {
    /* 空间不足以容纳完整数据时整包丢弃，避免产生半截残帧破坏接收端解析 */
    s_drop_bytes += (uint32_t)len;
    return 0u;
  }
  written = len;

  for (i = 0u; i < written; i++)
  {
    s_ring[(uint16_t)((s_head + i) & TX_MASK)] = data[i];
  }
  s_head = (uint16_t)((s_head + written) & TX_MASK);

  return written;
}

void app_tx_poll(void)
{
  uint16_t pending;
  uint16_t n;
  uint16_t i;

  if (s_huart == 0)
  {
    return;
  }
  if (s_busy != 0u)
  {
    return;
  }

  pending = app_tx_pending();
  if (pending == 0u)
  {
    return;
  }

  n = (pending > APP_TX_STAGE_SIZE) ? (uint16_t)APP_TX_STAGE_SIZE : pending;
  for (i = 0u; i < n; i++)
  {
    s_stage[i] = s_ring[(uint16_t)((s_tail + i) & TX_MASK)];
  }
  s_tail = (uint16_t)((s_tail + n) & TX_MASK);

  s_busy = 1u;
  if (HAL_UART_Transmit_DMA(s_huart, s_stage, n) != HAL_OK)
  {
    /* 启动失败：数据已经出队，只能记为丢弃，避免卡死在这里 */
    s_busy = 0u;
    s_drop_bytes += n;
  }
}

void app_tx_on_cplt(void)
{
  s_busy = 0u;
}

void app_tx_on_error(void)
{
  s_busy = 0u;
}

void app_tx_reset_drop(void)
{
  s_drop_bytes = 0u;
}
