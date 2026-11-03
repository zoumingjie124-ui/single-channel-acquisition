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
static uint32_t s_err_cnt;      /* 硬件错误总次数 */
static uint32_t s_err_ore;      /* 其中：溢出（Overrun） */
static uint32_t s_err_fe;       /* 其中：帧错误（Framing） */
static uint32_t s_err_ne;       /* 其中：噪声（Noise） */
static uint32_t s_err_pe;       /* 其中：校验（Parity，8N1 下正常为 0） */
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

/* 分类计数：只靠一个总数看不出"到底哪种错误"，
   而 ORE 与 FE/NE 的处理路径并不完全相同（ORE 多一步清标志）。 */
uint32_t usart_err_ore_count(void) { return s_err_ore; }
uint32_t usart_err_fe_count(void)  { return s_err_fe;  }
uint32_t usart_err_ne_count(void)  { return s_err_ne;  }
uint32_t usart_err_pe_count(void)  { return s_err_pe;  }

uint32_t usart_rx_fail_count(void)
{
  return s_rx_fail;
}

void usart_reset_counters(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  s_err_cnt = 0u;
  s_err_ore = 0u;
  s_err_fe  = 0u;
  s_err_ne  = 0u;
  s_err_pe  = 0u;
  s_rx_fail = 0u;
  __set_PRIMASK(primask);
}

#if (APP_DEBUG_INJECT != 0u)
/**
  * @brief  制造一次真实的硬件 ORE，并立刻恢复接收（仅调试构建使用）
  * @retval 1 = 硬件确实置起了 ORE；0 = 没能置起来
  *
  * @note   为什么不能靠"短接 RX 到 GND"来测 ORE：
  *         RX 被拉低只会产生**一次**帧错误（之后线路不回高电平就检测不到新的
  *         起始位），而 ORE 的定义是"DR 里的字节没被取走又来了新字节"。
  *         这里换一种做法——先停掉接收让 DMA 不再取走 DR，主机在随后的
  *         30 ms 里继续发字节，第 2 个字节到达时硬件就会置 ORE。
  *
  * @note   实测结论（2026-11-03）：ORE 一旦产生，只要重新挂接收，
  *         DMA 读走 DR 的同时就会把 ORE 清掉，**错误中断来不及执行**，
  *         所以 `HAL_UART_ErrorCallback` 不会因此被调用、`uart_err` 也不会增加。
  *         这本身说明：本设计里 ORE 不会造成链路停摆。
  *         错误回调路径由 FE/NE（波特率不匹配）那组用例覆盖。
  */
uint8_t usart_ore_inject(void)
{
  uint8_t saw_ore;

  if (HAL_UART_AbortReceive(&huart1) != HAL_OK)
  {
    return 0u;
  }

  HAL_Delay(30u);          /* 主机在这 30 ms 内持续发字节 */

  saw_ore = ((USART1->SR & USART_SR_ORE) != 0u) ? 1u : 0u;

  /* 先清硬件标志再重挂，否则 ReceiveToIdle_DMA 会因"启动时已有错误挂起"返回失败 */
  __HAL_UART_CLEAR_OREFLAG(&huart1);
  usart_start_rx();

  return saw_ore;
}

#endif /* APP_DEBUG_INJECT */

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

  /* 按类型分开记：HAL 每次进中断前会把 ErrorCode 累加进来、出中断前清零，
     所以这里读到的就是"本次事件"的位组合，不会重复计数。 */
  if ((huart->ErrorCode & HAL_UART_ERROR_ORE) != 0u)
  {
    s_err_ore++;
    __HAL_UART_CLEAR_OREFLAG(huart);
  }
  if ((huart->ErrorCode & HAL_UART_ERROR_FE) != 0u)
  {
    s_err_fe++;
  }
  if ((huart->ErrorCode & HAL_UART_ERROR_NE) != 0u)
  {
    s_err_ne++;
  }
  if ((huart->ErrorCode & HAL_UART_ERROR_PE) != 0u)
  {
    s_err_pe++;
  }

  if (huart->gState != HAL_UART_STATE_BUSY_TX)
  {
    app_tx_on_error();
  }

  usart_start_rx();
}
