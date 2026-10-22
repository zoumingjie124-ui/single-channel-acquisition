/**
  ******************************************************************************
  * @file    app_types.h
  * @brief   跨模块共享的类型定义：告警级别、运行统计。
  *
  * 为什么单独一个文件：
  *   app_alarm / app_task / app_cmd / app_proto 都要用到 app_level_t，
  *   放在任意一个模块的头文件里都会造成互相包含，单独抽出来最干净。
  ******************************************************************************
  */
#ifndef __APP_TYPES_H
#define __APP_TYPES_H

#include <stdint.h>

/* 告警级别。数值大小即严重程度，比较时可直接用 */
typedef enum
{
  APP_LEVEL_NORMAL = 0,
  APP_LEVEL_WARN   = 1,
  APP_LEVEL_CRIT   = 2
} app_level_t;

/**
  * @brief 运行统计。所有字段都是"单调递增的计数"，reset 时整体清零。
  *
  * 命名约定（上位机按此显示）：
  *   sample  : 成功进入流水线的采样点数
  *   drop    : 采样环形缓冲溢出而丢弃的点数（可观测的丢点）
  *   adc_err : ADC / DMA 传输错误次数
  *   uart_err: 串口硬件错误次数（ORE/FE/NE/PE）
  *   rx_err  : 接收重新武装失败次数（链路可能已停）
  *   frame_err: 收到的帧 CRC 或格式错误次数
  *   tx_drop : 发送环形缓冲满而丢弃的字节数
  */
typedef struct
{
  uint32_t sample;
  uint32_t drop;
  uint32_t adc_err;
  uint32_t uart_err;
  uint32_t rx_err;
  uint32_t frame_err;
  uint32_t cmd_ok;
  uint32_t cmd_err;
  uint32_t tx_drop;
  uint32_t changes;
} app_stats_t;

#endif /* __APP_TYPES_H */
