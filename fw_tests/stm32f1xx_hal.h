/**
  ******************************************************************************
  * @file    stm32f1xx_hal.h  （fw_tests 专用的最小桩，非真实 HAL）
  * @brief   只为了让 app_cmd.c 能在 PC/GCC 上编译。
  *
  * app_cmd.c -> app_tx.h -> "stm32f1xx_hal.h"。真实 HAL 在 PC 上不存在，
  * 这里只补出 app_tx.h 唯一用到的类型 UART_HandleTypeDef。
  * 位于 fw_tests/，不在 App/Inc，Keil 的包含路径指向别处，绝不会误用到它。
  ******************************************************************************
  */
#ifndef __STM32F1XX_HAL_H
#define __STM32F1XX_HAL_H

#include <stdint.h>

typedef struct
{
  int dummy;
} UART_HandleTypeDef;

#endif /* __STM32F1XX_HAL_H */
