/**
  ******************************************************************************
  * @file    app_cmd.c
  * @brief   命令通道实现：接收环形缓冲 -> 双通道分发。
  *
  * 并发：head 只在 ISR 改，tail 只在主循环改，无需临界区。
  ******************************************************************************
  */
#include <string.h>
#include "app_cmd.h"
#include "app_config.h"
#include "app_proto.h"
#include "app_tx.h"

#define RX_MASK ((uint16_t)(APP_RX_RING_SIZE - 1u))

/* 命令执行结果码，与上位机约定 */
#define CMD_RES_OK      0u
#define CMD_RES_PARAM   1u
#define CMD_RES_UNKNOWN 2u

static const app_cmd_ops_t *s_ops;

static uint8_t  s_rx_ring[APP_RX_RING_SIZE];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;

static proto_parser_t s_parser;
static uint32_t s_frame_err;
static uint32_t s_ok_cnt;
static uint32_t s_err_cnt;
static uint32_t s_rx_ovf;

static char     s_line[APP_LINE_BUF_SIZE];
static uint8_t  s_line_len;

static void send_ack(uint8_t req_type, uint8_t req_seq, uint8_t result)
{
  uint8_t payload[PROTO_LEN_ACK];
  uint8_t frame[PROTO_MAX_FRAME];
  uint16_t n;

  payload[0] = req_type;
  payload[1] = result;
  n = proto_encode(PROTO_T_ACK, req_seq, payload, PROTO_LEN_ACK,
                   frame, (uint16_t)sizeof(frame));
  if (n != 0u)
  {
    (void)app_tx_write(frame, n);
  }
}

/* --------------------------- 二进制帧分发 --------------------------- */
static void on_frame(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len)
{
  uint8_t res = CMD_RES_OK;

  if (s_ops == 0)
  {
    return;
  }

  switch (type)
  {
    case PROTO_T_SET_THR:
      if (len != 4u)
      {
        res = CMD_RES_PARAM;
      }
      else if ((s_ops->set_thr == 0) ||
               (s_ops->set_thr(proto_get_u16(&payload[0]),
                               proto_get_u16(&payload[2])) == 0u))
      {
        res = CMD_RES_PARAM;
      }
      break;

    case PROTO_T_SET_LED:
      if (len != 1u)
      {
        res = CMD_RES_PARAM;
      }
      else if (s_ops->set_led != 0)
      {
        s_ops->set_led((uint8_t)(payload[0] != 0u));
      }
      else
      {
        res = CMD_RES_UNKNOWN;
      }
      break;

    case PROTO_T_GET_STAT:
      if (s_ops->get_stat != 0)
      {
        s_ops->get_stat();
      }
      break;

    case PROTO_T_RESET:
      if (s_ops->reset_stats != 0)
      {
        s_ops->reset_stats();
      }
      break;

    case PROTO_T_PING:
      if (s_ops->ping != 0)
      {
        s_ops->ping();
      }
      break;

    default:
      res = CMD_RES_UNKNOWN;
      break;
  }

  if (res == CMD_RES_OK)
  {
    s_ok_cnt++;
  }
  else
  {
    s_err_cnt++;
  }

  /* 所有 PC->MCU 的命令都回一帧 ACK，上位机据此确认执行结果 */
  send_ack(type, seq, res);
}

/* ---------------------------- 文本通道 ---------------------------- */
static void text_byte(uint8_t b)
{
  if ((b == '\r') || (b == '\n'))
  {
    if (s_line_len > 0u)
    {
      s_line[s_line_len] = '\0';
      if (s_ops != 0 && s_ops->text_line != 0)
      {
        s_ops->text_line(s_line);
      }
      s_line_len = 0u;
    }
    return;
  }

  if ((b == 0x08u) || (b == 0x7Fu))   /* 退格 */
  {
    if (s_line_len > 0u)
    {
      s_line_len--;
    }
    return;
  }

  if ((b < 0x20u) || (b > 0x7Eu))
  {
    return;   /* 其它控制字符忽略 */
  }

  if (s_line_len < (uint8_t)(APP_LINE_BUF_SIZE - 1u))
  {
    s_line[s_line_len] = (char)b;
    s_line_len++;
  }
  /* 超长则丢弃后续字符，等行尾再处理，不会越界 */
}

/* ----------------------------- 对外接口 ----------------------------- */
void app_cmd_init(const app_cmd_ops_t *ops)
{
  s_ops       = ops;
  s_rx_head   = 0u;
  s_rx_tail   = 0u;
  s_frame_err = 0u;
  s_ok_cnt    = 0u;
  s_err_cnt   = 0u;
  s_rx_ovf    = 0u;
  s_line_len  = 0u;
  proto_parser_init(&s_parser);
}

void app_cmd_rx_isr(const uint8_t *data, uint16_t len)
{
  uint16_t i;
  uint16_t next;

  if (data == 0)
  {
    return;
  }

  for (i = 0u; i < len; i++)
  {
    next = (uint16_t)((s_rx_head + 1u) & RX_MASK);
    if (next == s_rx_tail)
    {
      s_rx_ovf++;      /* 满了：丢新字节并计数，绝不覆盖未解析的数据 */
      return;
    }
    s_rx_ring[s_rx_head] = data[i];
    s_rx_head = next;
  }
}

void app_cmd_poll(void)
{
  uint8_t b;
  uint8_t r;

  while (s_rx_tail != s_rx_head)
  {
    b = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1u) & RX_MASK);

    if (proto_parser_is_idle(&s_parser) != 0u)
    {
      if (b != PROTO_SOF0)
      {
        text_byte(b);          /* 非帧头字节走文本通道 */
        continue;
      }
    }

    r = proto_parser_feed(&s_parser, b, on_frame);
    if (r == PROTO_R_ERROR)
    {
      s_frame_err++;
      s_err_cnt++;
    }
  }
}

uint32_t app_cmd_frame_err_count(void)
{
  return s_frame_err;
}

uint32_t app_cmd_ok_count(void)
{
  return s_ok_cnt;
}

uint32_t app_cmd_err_count(void)
{
  return s_err_cnt;
}

uint32_t app_cmd_rx_overflow_count(void)
{
  return s_rx_ovf;
}

void app_cmd_reset_counters(void)
{
  s_frame_err = 0u;
  s_ok_cnt    = 0u;
  s_err_cnt   = 0u;
  s_rx_ovf    = 0u;
}
