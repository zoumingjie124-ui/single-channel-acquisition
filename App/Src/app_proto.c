/**
  ******************************************************************************
  * @file    app_proto.c
  * @brief   帧协议的 CRC、组帧与增量解析。
  *
  * 解析器是"逐字节推进"的状态机，好处是：
  *   - 不需要知道一帧何时结束（对不定长、分包到达都成立）；
  *   - 出错时自动复位，不会因为一个坏字节永久失步；
  *   - 内存固定（PROTO_MAX_FRAME 字节），不使用动态分配。
  *
  * 注意：本文件是纯逻辑，不依赖 HAL，可以直接在 PC 上用 gcc 编译做单元测试
  *      （host/tests/ 下就是这么做的）。
  ******************************************************************************
  */
#include "app_proto.h"

uint16_t proto_crc16(const uint8_t *data, uint16_t len)
{
  uint16_t crc = 0xFFFFu;
  uint16_t i;

  while (len-- != 0u)
  {
    crc ^= (uint16_t)((uint16_t)(*data++) << 8);
    for (i = 0u; i < 8u; i++)
    {
      if ((crc & 0x8000u) != 0u)
      {
        crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
      }
      else
      {
        crc = (uint16_t)(crc << 1);
      }
    }
  }
  return crc;
}

uint16_t proto_encode(uint8_t type, uint8_t seq,
                      const uint8_t *payload, uint8_t len,
                      uint8_t *out, uint16_t cap)
{
  uint16_t total;
  uint16_t crc;

  if ((out == 0) || (len > PROTO_MAX_PAYLOAD))
  {
    return 0u;
  }
  total = (uint16_t)(PROTO_HEADER_LEN + len + PROTO_CRC_LEN);
  if (cap < total)
  {
    return 0u;
  }

  out[0] = PROTO_SOF0;
  out[1] = PROTO_SOF1;
  out[2] = type;
  out[3] = len;
  out[4] = seq;

  if (len != 0u)
  {
    if (payload == 0)
    {
      return 0u;
    }
    {
      uint8_t i;
      for (i = 0u; i < len; i++)
      {
        out[PROTO_HEADER_LEN + i] = payload[i];
      }
    }
  }

  /* CRC 覆盖 TYPE/LEN/SEQ/PAYLOAD，即 buf[2] 起的 3+len 字节 */
  crc = proto_crc16(&out[2], (uint16_t)(3u + len));
  out[PROTO_HEADER_LEN + len]      = (uint8_t)(crc & 0xFFu);
  out[PROTO_HEADER_LEN + len + 1u] = (uint8_t)((crc >> 8) & 0xFFu);

  return total;
}

void proto_parser_init(proto_parser_t *p)
{
  if (p == 0)
  {
    return;
  }
  p->idx      = 0u;
  p->state    = PROTO_P_SOF0;
  p->len      = 0u;
  p->crc_calc = 0u;
  p->crc_recv = 0u;
}

uint8_t proto_parser_is_idle(const proto_parser_t *p)
{
  return (uint8_t)((p != 0) && (p->state == PROTO_P_SOF0));
}

uint8_t proto_parser_feed(proto_parser_t *p, uint8_t byte, proto_frame_cb_t cb)
{
  if (p == 0)
  {
    return PROTO_R_ERROR;
  }

  switch (p->state)
  {
    case PROTO_P_SOF0:
      if (byte != PROTO_SOF0)
      {
        /* 调用方应在 is_idle() 为真时把非 0xAA 字节交给文本通道，
           走到这里说明调用方没判断，直接忽略，避免死锁 */
        return PROTO_R_NONE;
      }
      p->buf[0] = byte;
      p->idx    = 1u;
      p->state  = PROTO_P_SOF1;
      return PROTO_R_NONE;

    case PROTO_P_SOF1:
      if (byte == PROTO_SOF1)
      {
        p->buf[1] = byte;
        p->idx    = 2u;
        p->state  = PROTO_P_TYPE;
        return PROTO_R_NONE;
      }
      else if (byte == PROTO_SOF0)
      {
        /* 连续收到 0xAA（如 0xAA 0xAA 0x55）：保持在 SOF1 状态，
           将当前的 0xAA 视为新的帧头第一字节 */
        p->buf[0] = byte;
        p->idx    = 1u;
        return PROTO_R_NONE;
      }
      else
      {
        /* 0xAA 后面既不是 0x55 也不是 0xAA：可能是噪声，重新找帧头 */
        p->state = PROTO_P_SOF0;
        return PROTO_R_ERROR;
      }

    case PROTO_P_TYPE:
      p->buf[2] = byte;
      p->idx    = 3u;
      p->state  = PROTO_P_LEN;
      return PROTO_R_NONE;

    case PROTO_P_LEN:
      if (byte > PROTO_MAX_PAYLOAD)
      {
        p->state = PROTO_P_SOF0;
        return PROTO_R_ERROR;
      }
      p->buf[3] = byte;
      p->len    = byte;
      p->idx    = 4u;
      p->state  = PROTO_P_SEQ;
      return PROTO_R_NONE;

    case PROTO_P_SEQ:
      p->buf[4] = byte;
      p->idx    = 5u;
      p->state  = (p->len == 0u) ? PROTO_P_CRC_LO : PROTO_P_BODY;
      return PROTO_R_NONE;

    case PROTO_P_BODY:
      p->buf[p->idx] = byte;
      p->idx++;
      if (p->idx >= (uint16_t)(PROTO_HEADER_LEN + p->len))
      {
        p->state = PROTO_P_CRC_LO;
      }
      return PROTO_R_NONE;

    case PROTO_P_CRC_LO:
      p->crc_recv = byte;
      p->state    = PROTO_P_CRC_HI;
      return PROTO_R_NONE;

    case PROTO_P_CRC_HI:
      p->crc_recv |= (uint16_t)((uint16_t)byte << 8);
      p->crc_calc  = proto_crc16(&p->buf[2], (uint16_t)(3u + p->len));
      p->state     = PROTO_P_SOF0;

      if (p->crc_calc != p->crc_recv)
      {
        return PROTO_R_ERROR;
      }
      if (cb != 0)
      {
        cb(p->buf[2], p->buf[4], &p->buf[PROTO_HEADER_LEN], p->len);
      }
      return PROTO_R_FRAME;

    default:
      p->state = PROTO_P_SOF0;
      return PROTO_R_ERROR;
  }
}
