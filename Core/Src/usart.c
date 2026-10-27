/**
  ******************************************************************************
  * @file    usart.c
  * @brief   USART1：DMA 发送 + （DMA + 空闲中断）接收。
  *
  * 为什么接收要用 IDLE：
  *   上位机发的帧长度不固定。用 "DMA + IDLE" 可以在"总线静默一个字符时间"时
  *   一次性拿到本批收到的字节数，不需要逐字节中断，也不需要事先知道长度。
  *
  * 中断上下文里只做两件事：
  *   1. 把收到的字节交给 app_cmd_rx_isr()（只是搬进环形缓冲）；
  *   2. 重新挂接收。
  *   解析、组帧、应答全部在主循环完成。
  *
 * 已核对 FW_F1 源码：IDLE 和 DMA 缓冲满（TC）两种事件，HAL 都是先把 DMA
 * abort、RxState 置为 READY，然后才回调 HAL_UARTEx_RxEventCallback()，
 * 所以回调里重新挂接收是安全的。
 * 但 DMA 半传输（HT）事件不同：UART_DMARxHalfCplt() 直接回调，此刻
 * RxState 仍是 BUSY_RX、DMA 还在收——重挂会返回 BUSY，此刻上交的字节
 * 也会在随后的 IDLE/TC 里重复上交。所以 HT 必须直接忽略。
  ******************************************************************************
  */
#include "usart.h"
#include "main.h"
#include "app_config.h"
#include "app_cmd.h"
#include "app_tx.h"

UART_HandleTypeDef huart1;

static uint8_t  s_rx_dma[APP_RX_DMA_BUF_LEN];
static uint32_t s_err_cnt;      /* 硬件错误次数 */
static uint32_t s_rx_fail;      /* 重新武装失败次数 */

void MX_USART1_UART_Init(void)
{
  huart1.Instance          = USART1;
  huart1.Init.BaudRate     = 115200;
  huart1.Init.WordLength   = UART_WORDLENGTH_8B;
  huart1.Init.StopBits     = UART_STOPBITS_1;
  huart1.Init.Parity       = UART_PARITY_NONE;
  huart1.Init.Mode         = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
}

void usart_start_rx(void)
{
  if (HAL_UARTEx_ReceiveToIdle_DMA(&huart1, s_rx_dma, APP_RX_DMA_BUF_LEN) != HAL_OK)
  {
    /* 启动失败必须计数：否则链路静默死亡，没人知道 */
    s_rx_fail++;
  }
}

uint32_t usart_err_count(void)
{
  return s_err_cnt;
}

uint32_t usart_rx_fail_count(void)
{
  return s_rx_fail;
}

void usart_reset_counters(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  s_err_cnt = 0u;
  s_rx_fail = 0u;
  __set_PRIMASK(primask);
}

/**
  * @brief  接收事件回调：IDLE 空闲、DMA 缓冲满（TC）、DMA 半传输（HT）都会进来
  * @param  Size 本次收到的字节数
  */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  if (huart->Instance == USART1)
  {
    /* HT 时接收还在进行（RxState == BUSY_RX）：重挂必然 BUSY，且这批数据
       会随 IDLE/TC 再次上交。留在 DMA 缓冲里等下一个事件即可，无需处理 */
    if (huart->RxEventType == HAL_UART_RXEVENT_HT)
    {
      return;
    }

    if ((Size > 0u) && (Size <= APP_RX_DMA_BUF_LEN))
    {
      app_cmd_rx_isr(s_rx_dma, Size);
    }
    usart_start_rx();
  }
}

/**
  * @brief  发送完成回调：通知发送队列可以继续下一批
  */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    app_tx_on_cplt();
  }
}

/**
  * @brief  错误回调：清标志 + 计数 + 重新挂接收
  * @note   没有这个回调，一次噪声（ORE）就能让接收永久停摆，这是最常见的事故。
  *
  *         两个细节必须区分开：
  *         1. 只有 ORE 置位才调 __HAL_UART_CLEAR_OREFLAG()。
  *            该宏是"读 SR 再读 DR"，无条件调用会吃掉一个刚收到的字节。
  *         2. 不能无条件释放发送队列。接收侧的 ORE/FE/NE 也会进这个回调，
  *            此时 TX DMA 可能仍在运行；若把 s_busy 清零，主循环会立刻
  *            再次调用 HAL_UART_Transmit_DMA() 并拿到 HAL_BUSY，
  *            于是这批已经出队的数据被误记为"丢弃"——真丢数据 + 统计失真。
  *            因此只在 gState 不是 BUSY_TX（发送确实已停）时才释放。
  */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance != USART1)
  {
    return;
  }

  s_err_cnt++;

  if ((huart->ErrorCode & HAL_UART_ERROR_ORE) != 0u)
  {
    __HAL_UART_CLEAR_OREFLAG(huart);
  }

  if (huart->gState != HAL_UART_STATE_BUSY_TX)
  {
    app_tx_on_error();
  }

  usart_start_rx();
}
