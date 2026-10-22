/**
  ******************************************************************************
  * @file    stm32f1xx_hal_msp.c
  * @brief   MSP = MCU Support Package：HAL 初始化外设时会回调这里，
  *          由它负责"板级资源"——时钟、GPIO、DMA 通道、NVIC 优先级。
  *
  * 这样 HAL 库本身保持与板子无关，换板子只改这个文件。
  ******************************************************************************
  */
#include "main.h"

DMA_HandleTypeDef hdma_adc1;
DMA_HandleTypeDef hdma_usart1_tx;
DMA_HandleTypeDef hdma_usart1_rx;

void HAL_MspInit(void)
{
  __HAL_RCC_AFIO_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();

  /* 关闭 JTAG、保留 SWD：释放 PB3/PB4/PA15，同时保住 PA13/PA14 下载口 */
  __HAL_AFIO_REMAP_SWJ_NOJTAG();
}

/**
  * @brief ADC1 的板级资源：时钟、PA1 模拟输入、DMA1_Channel1
  */
void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
  GPIO_InitTypeDef gpio = {0};

  if (hadc->Instance != ADC1)
  {
    return;
  }

  __HAL_RCC_ADC1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /* PA1 作为模拟输入：不能配上拉/下拉，否则会改变被测电压 */
  gpio.Pin  = GPIO_PIN_1;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &gpio);

  /* ADC1 固定走 DMA1_Channel1 */
  hdma_adc1.Instance                 = DMA1_Channel1;
  hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
  hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
  hdma_adc1.Init.Mode                = DMA_CIRCULAR;   /* 循环：采样永不停止 */
  hdma_adc1.Init.Priority            = DMA_PRIORITY_HIGH;
  if (HAL_DMA_Init(&hdma_adc1) != HAL_OK)
  {
    Error_Handler();
  }

  __HAL_LINKDMA(hadc, DMA_Handle, hdma_adc1);
}

void HAL_ADC_MspDeInit(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance != ADC1)
  {
    return;
  }
  __HAL_RCC_ADC1_CLK_DISABLE();
  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_1);
  HAL_DMA_DeInit(hadc->DMA_Handle);
}

/**
  * @brief USART1 的板级资源：时钟、PA9/PA10、DMA 收发通道
  */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
  GPIO_InitTypeDef gpio = {0};

  if (huart->Instance != USART1)
  {
    return;
  }

  __HAL_RCC_USART1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /* PA9 = TX，复用推挽 */
  gpio.Pin   = GPIO_PIN_9;
  gpio.Mode  = GPIO_MODE_AF_PP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOA, &gpio);

  /* PA10 = RX，上拉输入（悬空时不会读到随机噪声） */
  gpio.Pin  = GPIO_PIN_10;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &gpio);

  /* TX: DMA1_Channel4 */
  hdma_usart1_tx.Instance                 = DMA1_Channel4;
  hdma_usart1_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
  hdma_usart1_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_usart1_tx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_usart1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_usart1_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_usart1_tx.Init.Mode                = DMA_NORMAL;   /* 每批发完即停，由队列再启动 */
  hdma_usart1_tx.Init.Priority            = DMA_PRIORITY_MEDIUM;
  if (HAL_DMA_Init(&hdma_usart1_tx) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(huart, hdmatx, hdma_usart1_tx);

  /* RX: DMA1_Channel5 */
  hdma_usart1_rx.Instance                 = DMA1_Channel5;
  hdma_usart1_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
  hdma_usart1_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_usart1_rx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_usart1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_usart1_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_usart1_rx.Init.Mode                = DMA_NORMAL;
  hdma_usart1_rx.Init.Priority            = DMA_PRIORITY_MEDIUM;
  if (HAL_DMA_Init(&hdma_usart1_rx) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(huart, hdmarx, hdma_usart1_rx);

  HAL_NVIC_SetPriority(USART1_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
  if (huart->Instance != USART1)
  {
    return;
  }
  __HAL_RCC_USART1_CLK_DISABLE();
  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9 | GPIO_PIN_10);
  HAL_DMA_DeInit(huart->hdmatx);
  HAL_DMA_DeInit(huart->hdmarx);
}

/**
  * @brief TIM2 的板级资源：时钟 + 更新中断
  * @note  TIM3 只做 ADC 触发，不需要中断，因此不需要 MspInit。
  */
void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim_base)
{
  if (htim_base->Instance == TIM2)
  {
    __HAL_RCC_TIM2_CLK_ENABLE();
    HAL_NVIC_SetPriority(TIM2_IRQn, 4, 0);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);
  }
  else if (htim_base->Instance == TIM3)
  {
    __HAL_RCC_TIM3_CLK_ENABLE();
  }
}

void HAL_TIM_Base_MspDeInit(TIM_HandleTypeDef *htim_base)
{
  if (htim_base->Instance == TIM2)
  {
    __HAL_RCC_TIM2_CLK_DISABLE();
    HAL_NVIC_DisableIRQ(TIM2_IRQn);
  }
  else if (htim_base->Instance == TIM3)
  {
    __HAL_RCC_TIM3_CLK_DISABLE();
  }
}
