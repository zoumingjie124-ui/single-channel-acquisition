/**
  ******************************************************************************
  * @file    main.c
  * @brief   STM32 数据采集与故障监测终端 V5
  *
  * 启动顺序（顺序本身就是设计，改错会出怪问题）：
  *   1. HAL_Init()             —— 时基、NVIC 分组
  *   2. SystemClock_Config()   —— 72MHz + ADC 预分频（必须在 ADC 初始化之前）
  *   3. GPIO / DMA / ADC / TIM / USART 外设初始化
  *   4. app_tx_init()          —— 发送队列（必须早于任何组帧）
  *   5. app_task_init()        —— 应用层复位 + 发 BOOT 帧
  *   6. usart_start_rx()       —— 先开接收，避免丢掉上位机的第一帧
  *   7. app_measure_start()    —— ADC + DMA 开始搬运
  *   8. 最后启动定时器         —— 事件源一定最后开，否则中断可能早于缓冲初始化
  ******************************************************************************
  */
#include "main.h"

#include "dma.h"
#include "adc.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

#include "app_task.h"
#include "app_tx.h"
#include "app_measure.h"

static void SystemClock_Config(void);

int main(void)
{
  HAL_Init();

  SystemClock_Config();

  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_USART1_UART_Init();

  app_tx_init(&huart1);
  app_task_init();

  usart_start_rx();

  if (app_measure_start() == 0u)
  {
    Error_Handler();
  }

  HAL_TIM_Base_Start_IT(&htim2);   /* 1ms 系统节拍 */
  HAL_TIM_Base_Start(&htim3);      /* 100Hz ADC 触发（不需要中断） */

  while (1)
  {
    app_task_run();
  }
}

/**
  * @brief  HSE 8MHz -> PLL x9 -> 72MHz
  * @note   APB1 分频后是 36MHz，但定时器时钟会自动 x2 回到 72MHz，
  *         所以 TIM2/TIM3 的预分频都按 72MHz 计算。
  */
static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState            = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue      = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL          = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                     RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }

  /* ADC 时钟 = PCLK2 / 6 = 12MHz，必须 <= 14MHz。
     这一句要在 ADC 初始化之前执行，否则 HAL_ADCEx_Calibration_Start
     内部用 GetPeriphCLKFreq 算延时时会拿到 0 并除零。 */
  __HAL_RCC_ADC_CONFIG(RCC_ADCPCLK2_DIV6);
}

/**
  * @brief  初始化失败时停机。
  * @note   这是"有意为之"的死循环：宁可停下来被调试器发现，
  *         也不带着错误配置继续跑，否则现场表现会非常难以排查。
  *         板子不会因此损坏，断电重启即可。
  */
void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  (void)file;
  (void)line;
  Error_Handler();
}
#endif
