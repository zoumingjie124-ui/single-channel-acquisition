/**
  ******************************************************************************
  * @file    tim.h
  ******************************************************************************
  */
#ifndef __TIM_H
#define __TIM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f1xx_hal.h"

extern TIM_HandleTypeDef htim2;   /* 1ms 系统节拍 */
extern TIM_HandleTypeDef htim3;   /* 100Hz ADC 触发源 */

void MX_TIM2_Init(void);
void MX_TIM3_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* __TIM_H */
