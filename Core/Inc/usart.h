/**
  ******************************************************************************
  * @file    usart.h
  ******************************************************************************
  */
#ifndef __USART_H
#define __USART_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f1xx_hal.h"

extern UART_HandleTypeDef huart1;

void     MX_USART1_UART_Init(void);

/* 启动 "DMA + 空闲中断" 接收 */
void     usart_start_rx(void);

/* 统计：硬件错误（ORE/FE/NE/PE）与重新武装失败 */
uint32_t usart_err_count(void);
uint32_t usart_rx_fail_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __USART_H */
