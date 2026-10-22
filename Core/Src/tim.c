/**
  ******************************************************************************
  * @file    tim.c
  * @brief   两个定时器分工明确，互不干扰：
  *
  *   TIM2  72MHz / 72 = 1MHz，ARR=999  -> 1ms 中断，仅用于 uptime 计时
  *   TIM3  72MHz / 7200 = 10kHz，ARR=99 -> 100Hz 更新事件 -> TRGO -> 触发 ADC
  *
  * 为什么不用同一个定时器：
  *   F1 的 ADC 外部触发没有"通用 TRGO"，只能用 T1_CC1 / T2_CC2 / T3_TRGO / T4_CC4 等
  *   固定源。如果拿 TIM2 的 CC2 去触发，就得让 TIM2 的周期同时满足 1ms 节拍和
  *   10ms 采样，两者会互相牵制。拆成两个定时器最干净。
  ******************************************************************************
  */
#include "tim.h"
#include "main.h"
#include "app_task.h"

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;

void MX_TIM2_Init(void)
{
  TIM_ClockConfigTypeDef  sClockSourceConfig;
  TIM_MasterConfigTypeDef sMasterConfig;

  htim2.Instance               = TIM2;
  htim2.Init.Prescaler         = 71;      /* 72MHz / (71+1) = 1MHz */
  htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim2.Init.Period            = 999;     /* 1MHz / 1000 = 1kHz -> 1ms */
  htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }

  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }

  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

void MX_TIM3_Init(void)
{
  TIM_ClockConfigTypeDef  sClockSourceConfig;
  TIM_MasterConfigTypeDef sMasterConfig;

  htim3.Instance               = TIM3;
  htim3.Init.Prescaler         = 7199;    /* 72MHz / 7200 = 10kHz */
  htim3.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim3.Init.Period            = 99;      /* 10kHz / 100 = 100Hz */
  htim3.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }

  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /* 关键：把更新事件输出成 TRGO，ADC 才能被它触发 */
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief  TIM2 更新中断回调（1ms）
  * @note   只累加时基，不做任何耗时操作
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM2)
  {
    app_task_tick_isr();
  }
}
