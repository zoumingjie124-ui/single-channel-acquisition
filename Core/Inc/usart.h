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

/* 启动 "DMA + 空闲中断" 接收；返回 1 = 成功，0 = 失败（HAL 返回非 OK） */
uint8_t  usart_start_rx(void);

/* RX 重挂守护：主循环调用，检测到接收没在跑就按限速重挂一次 */
#define USART_RX_GUARD_NONE   0u
#define USART_RX_GUARD_OK     1u
#define USART_RX_GUARD_FAIL   2u
uint8_t  usart_rx_guard(uint32_t now_ms);

/* 重挂统计：尝试 / 成功 / 失败 */
uint32_t usart_rx_restart_try_count(void);
uint32_t usart_rx_restart_ok_count(void);
uint32_t usart_rx_restart_fail_count(void);

/* 仅调试构建：停掉接收且不重挂，用于验证 guard 能否把它救回来 */
#if (APP_DEBUG_INJECT != 0u)
void     usart_rx_stop_for_test(void);
#endif

/* 统计：硬件错误（ORE/FE/NE/PE）与重新武装失败 */
uint32_t usart_err_count(void);
uint32_t usart_err_ore_count(void);
uint32_t usart_err_fe_count(void);
uint32_t usart_err_ne_count(void);
uint32_t usart_err_pe_count(void);
uint32_t usart_rx_fail_count(void);
void     usart_reset_counters(void);

/* 仅调试构建使用：制造一次真实的硬件 ORE，返回 0 = 没造出 ORE，1 = 硬件确实置起了 ORE（见实现处说明） */
#if (APP_DEBUG_INJECT != 0u)
uint8_t  usart_ore_inject(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __USART_H */
