/**
  ******************************************************************************
  * @file    stm32f1xx_it.c
  * @brief   中断服务程序：只做转发，不含业务逻辑。
  ******************************************************************************
  */
#include "main.h"
#include "stm32f1xx_it.h"

extern TIM_HandleTypeDef  htim2;
extern UART_HandleTypeDef huart1;
extern DMA_HandleTypeDef  hdma_adc1;
extern DMA_HandleTypeDef  hdma_usart1_tx;
extern DMA_HandleTypeDef  hdma_usart1_rx;

void NMI_Handler(void)
{
  while (1)
  {
  }
}

void HardFault_Handler(void)
{
  while (1)
  {
  }
}

void MemManage_Handler(void)
{
  while (1)
  {
  }
}

void BusFault_Handler(void)
{
  while (1)
  {
  }
}

void UsageFault_Handler(void)
{
  while (1)
  {
  }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void EXTI0_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0);
}

void TIM2_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&htim2);
}

void USART1_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart1);
}

void DMA1_Channel1_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_adc1);
}

void DMA1_Channel4_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_usart1_tx);
}

void DMA1_Channel5_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_usart1_rx);
}
