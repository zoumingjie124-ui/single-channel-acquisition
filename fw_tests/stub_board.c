/**
  ******************************************************************************
  * @file    stub_board.c
  * @brief   app_cmd.c 在 PC 上编译时缺的两个外部符号的桩实现：
  *            app_tx_write()          —— 真实固件里塞进 DMA 发送环，这里只捕获字节
  *            app_task_next_frame_seq()—— 真实固件里是全局帧序号，这里用一个计数器
  *
  * 只服务于 test_cmd.c。不碰任何真实固件源码。
  ******************************************************************************
  */
#include "stub_board.h"
#include "app_tx.h"        /* 提供 app_tx_write 的原型，签名必须一致 */
#include "app_task.h"      /* 提供 app_task_next_frame_seq 的原型 */

uint8_t  tx_cap[TX_CAP_MAX];
uint16_t tx_cap_len;

static uint8_t s_frame_seq;

void tx_cap_reset(void)
{
  tx_cap_len = 0u;
}

uint8_t tx_cap_at(uint16_t i)
{
  return (i < tx_cap_len) ? tx_cap[i] : 0u;
}

void frame_seq_reset(uint8_t start)
{
  s_frame_seq = start;
}

/* app_cmd 的 send_ack() 会调用它把 ACK 帧写出去 */
uint16_t app_tx_write(const uint8_t *data, uint16_t len)
{
  uint16_t i;
  if (data == 0)
  {
    return 0u;
  }
  for (i = 0u; (i < len) && (tx_cap_len < TX_CAP_MAX); i++)
  {
    tx_cap[tx_cap_len++] = data[i];
  }
  return len;
}

uint8_t app_task_next_frame_seq(void)
{
  return s_frame_seq++;
}
