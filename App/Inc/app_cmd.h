/**
  ******************************************************************************
  * @file    app_cmd.h
  * @brief   上位机命令通道：二进制帧 + 文本调试命令 双通道。
  *
  * 为什么保留文本通道：
  *   二进制帧方便上位机，但调试时你没法用串口助手手敲。
  *   所以保留 "help / stat / thr / led / reset" 这套 ASCII 命令，
  *   靠首字节区分：0xAA 交给二进制解析器，可打印字符走文本通道，互不干扰。
  ******************************************************************************
  */
#ifndef __APP_CMD_H
#define __APP_CMD_H

#include <stdint.h>

typedef struct
{
  /* 二进制命令回调。返回 1 表示执行成功，0 表示参数非法 */
  uint8_t (*set_thr)(uint16_t warn_mv, uint16_t crit_mv);
  void    (*set_led)(uint8_t on);
  void    (*get_stat)(void);
  void    (*reset_stats)(void);
  void    (*ping)(void);

  /* 文本命令回调，line 已去掉行尾 */
  void    (*text_line)(const char *line);
} app_cmd_ops_t;

void app_cmd_init(const app_cmd_ops_t *ops);

/* 由 UART 接收回调调用（中断上下文）：只搬运，不解析 */
void app_cmd_rx_isr(const uint8_t *data, uint16_t len);

/* 主循环调用：解析并分发 */
void app_cmd_poll(void);

uint32_t app_cmd_frame_err_count(void);
uint32_t app_cmd_ok_count(void);
uint32_t app_cmd_err_count(void);
uint32_t app_cmd_rx_overflow_count(void);

/* 清零统计计数（不影响解析器状态与缓冲内数据） */
void     app_cmd_reset_counters(void);

#endif /* __APP_CMD_H */
