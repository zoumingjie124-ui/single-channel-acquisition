/**
  ******************************************************************************
  * @file    main.h
  * @brief   全局引脚定义与错误处理声明。
  ******************************************************************************
  */
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f1xx_hal.h"

void Error_Handler(void);

/* 板载 LED：PC13，低电平点亮 */
#define LED_Pin            GPIO_PIN_13
#define LED_GPIO_Port      GPIOC

/* 按键：PA0，内部上拉，按下接地（下降沿） */
#define KEY_Pin            GPIO_PIN_0
#define KEY_GPIO_Port      GPIOA

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
