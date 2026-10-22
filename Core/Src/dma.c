/**
  ******************************************************************************
  * @file    dma.c
  * @brief   DMA1 时钟与中断使能。
  *
  * STM32F1 的 DMA 通道映射是固定的，本项目用到三个：
  *   DMA1_Channel1  <- ADC1
  *   DMA1_Channel4  <- USART1_TX
  *   DMA1_Channel5  <- USART1_RX
  * 三个通道互相独立，不会争用。
  ******************************************************************************
  */
#include "dma.h"
#include "stm32f1xx_hal.h"

void MX_DMA_Init(void)
{
  __HAL_RCC_DMA1_CLK_ENABLE();

  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 1, 0);   /* ADC 采样，优先级最高 */
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

  HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 3, 0);   /* 串口发送完成 */
  HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);

  HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 3, 0);   /* 串口接收 */
  HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);
}
