/**
  ******************************************************************************
  * @file    adc.c
  * @brief   ADC1 初始化：TIM3 TRGO 触发 + DMA 循环搬运。
  *
  * 与 V1（轮询）的区别：
  *   V1: HAL_ADC_Start -> PollForConversion -> GetValue -> Stop，每次转换都占用 CPU
  *   V5: 定时器硬件触发，DMA 自动把结果搬进内存，CPU 只在半满/全满回调里搬指针
  *
  * F1 注意事项：
  *   1. ADC 外部触发源是固定的几个（T1_CC1/T1_CC2/T2_CC2/T3_TRGO/T4_CC4/EXT_IT11），
  *      没有"通用 TRGO 输入"。这里选 T3_TRGO，让 TIM2 继续做 1ms 系统节拍。
  *   2. 校准必须在 ADC 预分频配置之后、启动转换之前调用，否则内部算延时会除零。
  ******************************************************************************
  */
#include "adc.h"
#include "main.h"
#include "app_measure.h"

ADC_HandleTypeDef hadc1;

/**
  * @brief  配置采样通道并执行校准（MX_ADC1_Init 与 ADC1_RecoverInit 共用）
  * @retval 1 成功，0 失败
  * @note   抽出来是为了让"上电初始化"和"故障恢复"走完全相同的配置路径，
     避免两处参数漂移；同时恢复路径需要返回值，不能直接 Error_Handler。
  */
static uint8_t adc1_config_channel(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};

  sConfig.Channel      = ADC_CHANNEL_1;            /* PA1 = ADC1_IN1 */
  sConfig.Rank         = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_55CYCLES_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    return 0u;
  }

  /* F1 必须校准，否则读数有固定偏差。校准前 ADC 需处于关闭状态，HAL 内部已处理 */
  if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK)
  {
    return 0u;
  }
  return 1u;
}

void MX_ADC1_Init(void)
{
  hadc1.Instance                   = ADC1;
  hadc1.Init.ScanConvMode          = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode    = DISABLE;      /* 每次触发只转换一次 */
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv      = ADC_EXTERNALTRIGCONV_T3_TRGO;
  hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion       = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  if (adc1_config_channel() == 0u)
  {
    Error_Handler();
  }
}

/**
  * @brief  故障恢复：把 ADC1 复位到上电状态并重新初始化
  * @retval 1 成功，0 失败
  *
  * 为什么必须先从 DeInit 走一遍：
  *   1. HAL_ADC_Stop_DMA() 只在 DMA 处于 BUSY 时才调用 HAL_DMA_Abort()；
  *      发生传输错误后 DMA 已经是 ERROR 态，这条路径会被跳过，
  *      错误标志和通道状态都清不掉。
  *   2. HAL_ADC_DeInit() 会调用 HAL_ADC_MspDeInit() -> HAL_DMA_DeInit()，
  *      把 DMA 通道、标志、状态真正复位，并把 hadc->State 置为 RESET、
  *      ErrorCode 清零。之后 HAL_ADC_Init() 才会重新执行 HAL_ADC_MspInit()，
  *      也就是重新做一遍 HAL_DMA_Init()。
  *   所以 DeInit + Init 等于"干净重来一次"，与上电路径完全一致。
  *
  * 本函数只允许在主循环调用：内部是阻塞的 DeInit/Init/校准，
  * 绝不能在中断里执行。
  */
uint8_t ADC1_RecoverInit(void)
{
  if (HAL_ADC_DeInit(&hadc1) != HAL_OK)
  {
    return 0u;
  }
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    return 0u;
  }
  return adc1_config_channel();
}

/* ==========================================================================
 * ADC + DMA 回调
 *
 * 这三条是整条采样链路的"最后一公里"。HAL 里它们是 __weak 空函数，
 * 不实现也能编译通过、也能进 main 的 while(1)，但采样环永远是空的，
 * 一帧 SAMPLE 都发不出去。所以必须在这里把 HAL 事件接到 app_measure。
 *
 * 调用路径（已核对 stm32f1xx_hal_adc.c）：
 *   DMA1_Channel1 半传输中断 -> ADC_DMAHalfConvCplt() -> 本文件 ConvHalfCplt
 *   DMA1_Channel1 传输完成   -> ADC_DMAConvCplt()     -> 本文件 ConvCplt
 *   DMA 错误                 -> ADC_DMAError()        -> 本文件 Error
 *
 * 回调里只做一件事：让 app_measure 把"刚写完的那一段"搬进环形缓冲。
 * 滤波、判级、组帧全部留给主循环，中断时间保持在微秒级。
 * ========================================================================== */

/**
  * @brief  半传输完成：DMA 已写满前 ADC_DMA_BUF_LEN/2 个元素
  * @note   ADC_DMA_BUF_LEN=2 时，这里对应 index 0（第 1 个样本）
  */
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    app_measure_on_half();
  }
}

/**
  * @brief  全传输完成：DMA 已写满整个缓冲
  * @note   ADC_DMA_BUF_LEN=2 时，这里对应 index 1（第 2 个样本），
  *         与半传输那次的 index 0 不重叠，不会重复处理同一批数据。
  *         循环模式下 DMA 会自动回卷，本回调周期性重复触发。
  */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    app_measure_on_cplt();
  }
}

/**
  * @brief  ADC / DMA 错误
  * @note   当前只计数（app_measure_on_error 内部自增）。
  *         这里不重启转换：中断上下文里重启 DMA 风险高，
  *         如需自动恢复应由主循环监督完成。
  */
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    app_measure_on_error();
  }
}
