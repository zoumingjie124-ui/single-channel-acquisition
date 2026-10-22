/**
  ******************************************************************************
  * @file    app_proto.h
  * @brief   二进制帧协议的编解码（CRC16 + 增量解析状态机）。
  *
  * 帧格式（小端）：
  *   +------+------+------+-----+-----+--------------+--------+--------+
  *   | 0xAA | 0x55 | TYPE | LEN | SEQ | PAYLOAD[LEN] | CRC_LO | CRC_HI |
  *   +------+------+------+-----+-----+--------------+--------+--------+
  *
  *   CRC16-CCITT（多项式 0x1021，初值 0xFFFF），覆盖 TYPE..PAYLOAD 最后一字节，
  *   不覆盖帧头与 CRC 自身。
  *
  * 为什么要 SEQ：上位机用它发现"丢帧"。SEQ 不连续 = 中间丢了帧，
  * 而 CRC 只能发现"内容坏了"，发现不了"整帧没到"。
  ******************************************************************************
  */
#ifndef __APP_PROTO_H
#define __APP_PROTO_H

#include <stdint.h>

/* ------------------------------- 帧头 ------------------------------- */
#define PROTO_SOF0              0xAAu
#define PROTO_SOF1              0x55u

/* ---------------------------- 消息类型 ---------------------------- */
/* 设备 -> 上位机 */
#define PROTO_T_SAMPLE          0x81u   /* 采样数据 */
#define PROTO_T_EVENT           0x82u   /* 告警状态切换事件 */
#define PROTO_T_STAT            0x83u   /* 运行统计 */
#define PROTO_T_ACK             0x84u   /* 命令应答 */
#define PROTO_T_BOOT            0x85u   /* 上电信息 */

/* 上位机 -> 设备 */
#define PROTO_T_SET_THR         0x10u   /* 设置阈值 */
#define PROTO_T_SET_LED         0x11u   /* 控制 LED */
#define PROTO_T_GET_STAT        0x12u   /* 请求统计 */
#define PROTO_T_RESET           0x13u   /* 清零统计 */
#define PROTO_T_PING            0x14u   /* 心跳 */

/* ------------------------------ 载荷长度 ------------------------------ */
#define PROTO_LEN_SAMPLE        10u     /* t_ms(4) raw(2) filt(2) level(1) seq8(1) */
#define PROTO_LEN_EVENT         8u      /* t_ms(4) from(1) to(1) filt(2) */
#define PROTO_LEN_STAT          37u     /* 见 app_task.c 的 build_stat_payload() */
#define PROTO_LEN_ACK           2u      /* req_type(1) result(1) */
#define PROTO_LEN_BOOT          7u      /* fw_ver(2) rate(2) dma_buf(1) filter(1) decim(1) */

/* ------------------------------ 帧尺寸 ------------------------------ */
#define PROTO_MAX_PAYLOAD       40u
#define PROTO_HEADER_LEN        5u      /* SOF0 SOF1 TYPE LEN SEQ */
#define PROTO_CRC_LEN           2u
#define PROTO_MAX_FRAME         (PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD + PROTO_CRC_LEN)

/* ---------------------------- 解析状态 ---------------------------- */
#define PROTO_P_SOF0            0u
#define PROTO_P_SOF1            1u
#define PROTO_P_TYPE            2u
#define PROTO_P_LEN             3u
#define PROTO_P_SEQ             4u
#define PROTO_P_BODY            5u
#define PROTO_P_CRC_LO          6u
#define PROTO_P_CRC_HI          7u

/* 解析结果 */
#define PROTO_R_NONE            0u      /* 继续收 */
#define PROTO_R_FRAME           1u      /* 收到完整帧，已回调 */
#define PROTO_R_ERROR           2u      /* 帧错误，已自动复位 */

typedef struct
{
  uint8_t  buf[PROTO_MAX_FRAME];
  uint16_t idx;
  uint8_t  state;
  uint8_t  len;
  uint16_t crc_calc;
  uint16_t crc_recv;
} proto_parser_t;

/* 收到完整帧时的回调。payload 指向 parser 内部缓冲，回调返回后即失效 */
typedef void (*proto_frame_cb_t)(uint8_t type, uint8_t seq,
                                 const uint8_t *payload, uint8_t len);

uint16_t proto_crc16(const uint8_t *data, uint16_t len);

/* 组帧。返回帧总长度；cap 不足返回 0 */
uint16_t proto_encode(uint8_t type, uint8_t seq,
                      const uint8_t *payload, uint8_t len,
                      uint8_t *out, uint16_t cap);

void    proto_parser_init(proto_parser_t *p);
/* 解析器是否处于"等待帧头"的空闲态。空闲态下非 0xAA 的字节可交给文本通道 */
uint8_t proto_parser_is_idle(const proto_parser_t *p);
uint8_t proto_parser_feed(proto_parser_t *p, uint8_t byte, proto_frame_cb_t cb);

/* ---------------------------- 小端读写工具 ---------------------------- */
static inline void proto_put_u16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}
static inline void proto_put_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static inline uint16_t proto_get_u16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t proto_get_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif /* __APP_PROTO_H */
