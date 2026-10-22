/**
  ******************************************************************************
  * @file    app_tx.h
  * @brief   串口发送队列：主循环只负责"塞数据"，DMA 负责搬运。
  *
  * 为什么需要它：
  *   HAL_UART_Transmit() 是阻塞的，一条 16B 帧在 115200 下要 1.4ms。
  *   100Hz 上报时每 10ms 就要发一帧，阻塞会直接和采样抢时间。
  *   改成 "环形缓冲 + DMA" 后，主循环把帧塞进缓冲就走，耗时只有一次 memcpy。
  *
  * 结构：
  *   [写入] app_tx_write()  -->  ring[1024]
  *                                   |
  *                                   v  app_tx_poll()（主循环，非阻塞）
  *                              stage[128] --DMA--> USART1
  *                                   ^
  *                                   |  HAL_UART_TxCpltCallback --> app_tx_on_cplt()
  ******************************************************************************
  */
#ifndef __APP_TX_H
#define __APP_TX_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

void     app_tx_init(UART_HandleTypeDef *huart);

/* 写入待发数据。返回实际写入字节数；缓冲满的部分计入 drop_bytes */
uint16_t app_tx_write(const uint8_t *data, uint16_t len);

/* 主循环周期调用：若空闲且有数据，则启动一次 DMA 发送 */
void     app_tx_poll(void);

/* 由 HAL 回调调用 */
void     app_tx_on_cplt(void);
void     app_tx_on_error(void);

uint32_t app_tx_drop_bytes(void);
uint16_t app_tx_pending(void);
uint8_t  app_tx_is_busy(void);

/* 清零丢弃计数 */
void     app_tx_reset_drop(void);

#endif /* __APP_TX_H */
