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
#include "app_config.h"   /* APP_DEBUG_INJECT：调试注入函数是否可见 */

extern UART_HandleTypeDef huart1;

void     MX_USART1_UART_Init(void);

/* 启动 "DMA + 空闲中断" 接收 */
void     usart_start_rx(void);

/* 统计：硬件错误（ORE/FE/NE/PE）与重新武装失败 */
uint32_t usart_err_count(void);
uint32_t usart_err_ore_count(void);
uint32_t usart_err_fe_count(void);
uint32_t usart_err_ne_count(void);
uint32_t usart_err_pe_count(void);
uint32_t usart_rx_fail_count(void);
void     usart_reset_counters(void);

/* 仅调试构建使用：制造一次真实的硬件 ORE，返回 0/1/2（见实现处说明） */
#if (APP_DEBUG_INJECT != 0u)
uint8_t  usart_ore_inject(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __USART_H */
