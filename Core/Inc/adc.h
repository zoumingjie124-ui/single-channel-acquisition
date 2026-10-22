/**
  ******************************************************************************
  * @file    adc.h
  ******************************************************************************
  */
#ifndef __ADC_H
#define __ADC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f1xx_hal.h"

extern ADC_HandleTypeDef hadc1;

void MX_ADC1_Init(void);

/* 故障恢复专用：把 ADC1 彻底复位后重新初始化。
   与 MX_ADC1_Init() 的区别是失败时返回 0，不调用 Error_Handler()，
   这样主循环可以决定重试策略，而不是直接死机。 */
uint8_t ADC1_RecoverInit(void);

#ifdef __cplusplus
}
#endif

#endif /* __ADC_H */
