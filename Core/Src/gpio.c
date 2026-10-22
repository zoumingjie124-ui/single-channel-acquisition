/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   LED（PC13 输出）、按键（PA0 EXTI 下降沿）。
  *
  * 按键消抖：EXTI 只负责"置标志"，不在这里做延时消抖（中断里不能等）。
  * 真正的消抖靠"两次中断间隔小于 20ms 就忽略"，用 HAL_GetTick() 判断，
  * 不阻塞、不占用中断时间。
  ******************************************************************************
  */
#include "gpio.h"
#include "main.h"
#include "app_task.h"

#define KEY_DEBOUNCE_MS   20u

static uint32_t s_last_key_ms;

void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  /* LED：推挽输出，初始熄灭（PC13 低电平点亮，所以初始置高） */
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
  gpio.Pin   = LED_Pin;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &gpio);

  /* 按键：上拉输入 + 下降沿中断 */
  gpio.Pin  = KEY_Pin;
  gpio.Mode = GPIO_MODE_IT_FALLING;
  gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(KEY_GPIO_Port, &gpio);

  HAL_NVIC_SetPriority(EXTI0_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);

  s_last_key_ms = 0u;
}

/**
  * @brief  EXTI 回调（中断上下文）
  * @note   软件消抖：距上次有效按下不足 20ms 视为抖动，直接丢弃。
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  uint32_t now;

  if (GPIO_Pin != KEY_Pin)
  {
    return;
  }

  now = HAL_GetTick();
  if ((uint32_t)(now - s_last_key_ms) < KEY_DEBOUNCE_MS)
  {
    return;
  }
  s_last_key_ms = now;

  app_task_on_key();
}
