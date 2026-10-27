/**
  ******************************************************************************
  * @file    app_task.c
  * @brief   应用层总调度。V5 的完整数据通路都在这里串起来。
  *
  * 数据流水线：
  *   TIM3(100Hz) -> ADC1 -> DMA -> 采样环
  *        -> 主循环取出 -> mV 换算 -> 移动平均 -> 三级判级
  *        -> SAMPLE 帧（100Hz）/ EVENT 帧（状态变化）/ STAT 帧（1Hz）
  *        -> 发送环 -> DMA -> USART1 -> 上位机
  *
  * 设计要点：
  *   1. 中断只做"搬指针"，滤波/判级/组帧全在主循环，ISR 时间可控；
  *   2. 任何一处都不阻塞：发送走 DMA 队列，采样由硬件触发；
  *   3. 所有计数器都可观测：丢点、ADC 错误、串口错误、帧错误、发送丢弃。
  ******************************************************************************
  */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "main.h"
#include "app_task.h"
#include "app_config.h"
#include "app_types.h"
#include "app_proto.h"
#include "app_tx.h"
#include "app_measure.h"
#include "app_filter.h"
#include "app_alarm.h"
#include "app_cmd.h"
#include "usart.h"

/* LED 闪烁节奏：WARN 用 250ms 翻转一次（2Hz） */
#define LED_BLINK_MS    250u

typedef struct
{
  volatile uint32_t tick_ms;
  volatile uint8_t  key_pending;
  uint32_t  stat_last_ms;
  uint8_t   frame_seq;
  uint32_t  sample_seq;   /* 采样连续序号：清零命令不清它，否则上位机会把清零误判成序号缺口 */
  app_level_t last_level;

  app_filter_t filter;
  app_alarm_t  alarm;
  app_stats_t  stats;

  uint8_t   led_forced;      /* 1 = 被 led 命令接管 */
  uint8_t   led_forced_on;
} app_ctx_t;

static app_ctx_t s_ctx;

/* LED 输出缓存：主循环每秒调用上万次，状态没变就不重复写寄存器 */
static uint8_t s_led_last = 0xFFu;   /* 0xFF = 尚未写过，保证第一次一定写 */
static void led_write(uint8_t on);

/* ---------------------------- 发送辅助 ---------------------------- */
static void tx_text(const char *fmt, ...)
{
  char buf[96];
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (n > 0)
  {
    if (n >= (int)sizeof(buf))
    {
      n = (int)sizeof(buf) - 1;   /* 截断保护：不能把结尾的 '\0' 也发出去 */
    }
    (void)app_tx_write((const uint8_t *)buf, (uint16_t)n);
  }
}

static void emit_frame(uint8_t type, const uint8_t *payload, uint8_t len)
{
  uint8_t frame[PROTO_MAX_FRAME];
  uint16_t n;

  n = proto_encode(type, app_task_next_frame_seq(), payload, len, frame, (uint16_t)sizeof(frame));
  if (n != 0u)
  {
    (void)app_tx_write(frame, n);
  }
}

/* --------------------------- 帧内容构造 --------------------------- */
static void emit_boot(void)
{
  uint8_t p[PROTO_LEN_BOOT];

  proto_put_u16(&p[0], APP_FW_VERSION);
  proto_put_u16(&p[2], APP_SAMPLE_RATE_HZ);
  p[4] = (uint8_t)ADC_DMA_BUF_LEN;
  p[5] = (uint8_t)APP_FILTER_DEPTH;
  p[6] = (uint8_t)APP_REPORT_DECIM;
  emit_frame(PROTO_T_BOOT, p, PROTO_LEN_BOOT);
}

static void emit_sample(uint16_t mv_raw, uint16_t mv_filt, app_level_t level)
{
  uint8_t p[PROTO_LEN_SAMPLE];

  proto_put_u32(&p[0], s_ctx.tick_ms);
  proto_put_u16(&p[4], mv_raw);
  proto_put_u16(&p[6], mv_filt);
  p[8] = (uint8_t)level;
  p[9] = (uint8_t)(s_ctx.sample_seq & 0xFFu);   /* 低 8 位采样序号：独立计数，清零命令不影响连续性 */
  emit_frame(PROTO_T_SAMPLE, p, PROTO_LEN_SAMPLE);
}

static void emit_event(app_level_t from, app_level_t to, uint16_t mv_filt)
{
  uint8_t p[PROTO_LEN_EVENT];

  proto_put_u32(&p[0], s_ctx.tick_ms);
  p[4] = (uint8_t)from;
  p[5] = (uint8_t)to;
  proto_put_u16(&p[6], mv_filt);
  emit_frame(PROTO_T_EVENT, p, PROTO_LEN_EVENT);
}

static void emit_stat(void)
{
  uint8_t p[PROTO_LEN_STAT];

  proto_put_u32(&p[0],  s_ctx.tick_ms);
  proto_put_u32(&p[4],  s_ctx.stats.sample);
  proto_put_u32(&p[8],  app_measure_drop_count());
  proto_put_u32(&p[12], app_measure_err_count());
  proto_put_u32(&p[16], usart_err_count());
  proto_put_u32(&p[20], usart_rx_fail_count() + app_cmd_rx_overflow_count());
  proto_put_u32(&p[24], app_cmd_frame_err_count());
  proto_put_u32(&p[28], s_ctx.alarm.change_count);
  proto_put_u16(&p[32], s_ctx.alarm.thr_warn_mv);
  proto_put_u16(&p[34], s_ctx.alarm.thr_crit_mv);
  p[36] = (uint8_t)s_ctx.alarm.level;
  emit_frame(PROTO_T_STAT, p, PROTO_LEN_STAT);
}

/* --------------------------- 命令回调实现 --------------------------- */
static uint8_t ctx_set_thr(uint16_t warn_mv, uint16_t crit_mv)
{
  uint8_t ok = app_alarm_set_thresholds(&s_ctx.alarm, warn_mv, crit_mv);
  if (ok != 0u)
  {
    emit_stat();     /* 立即回推新阈值，上位机不用等下一个周期 */
  }
  return ok;
}

static void ctx_set_led(uint8_t on)
{
  s_ctx.led_forced    = 1u;
  s_ctx.led_forced_on = (uint8_t)(on != 0u);
  led_write(s_ctx.led_forced_on);
}

static void ctx_get_stat(void)
{
  emit_stat();
}

static void ctx_reset_stats(void)
{
  /* sample_seq 有意不清：上位机靠它的连续性查缺口，清零命令不能打断 */
  s_ctx.stats.sample  = 0u;
  s_ctx.stats.drop    = 0u;
  s_ctx.stats.adc_err = 0u;
  s_ctx.stats.uart_err = 0u;
  s_ctx.stats.rx_err  = 0u;
  s_ctx.stats.frame_err = 0u;
  s_ctx.stats.cmd_ok  = 0u;
  s_ctx.stats.cmd_err = 0u;
  s_ctx.stats.tx_drop = 0u;
  s_ctx.stats.changes = 0u;

  s_ctx.alarm.change_count = 0u;
  app_measure_reset_counters();
  app_cmd_reset_counters();
  app_tx_reset_drop();
  usart_reset_counters();
}

static void ctx_ping(void)
{
  /* PING 只回 ACK，由 app_cmd 统一发送，这里无需额外动作 */
}

/* ---------------------------- 文本命令 ---------------------------- */
static uint8_t text_eq(const char *a, const char *b)
{
  while ((*a != '\0') && (*b != '\0'))
  {
    if (*a != *b)
    {
      return 0u;
    }
    a++;
    b++;
  }
  return (uint8_t)((*a == '\0') && (*b == '\0'));
}

static const char *skip_sp(const char *p)
{
  while ((*p == ' ') || (*p == '\t'))
  {
    p++;
  }
  return p;
}

static uint8_t parse_u16(const char **pp, uint16_t *out)
{
  const char *p = skip_sp(*pp);
  uint32_t v = 0u;
  uint8_t digits = 0u;

  while ((*p >= '0') && (*p <= '9'))
  {
    v = (v * 10u) + (uint32_t)(*p - '0');
    if (v > 65535u)
    {
      return 0u;
    }
    digits++;
    p++;
  }
  if (digits == 0u)
  {
    return 0u;
  }
  *out = (uint16_t)v;
  *pp = p;
  return 1u;
}

static void on_text_line(const char *line)
{
  const char *p = skip_sp(line);
  uint16_t a;
  uint16_t b;

  if (*p == '\0')
  {
    return;
  }

  if (text_eq(p, "help") != 0u)
  {
    tx_text("---- commands ----\r\n");
    tx_text(" help              : this text\r\n");
    tx_text(" stat              : push STAT frame\r\n");
    tx_text(" thr <warn> <crit> : set thresholds (mV)\r\n");
    tx_text(" led on|off        : force LED\r\n");
    tx_text(" reset             : clear all counters\r\n");
    tx_text(" note: binary frames start with 0xAA 0x55\r\n");
    return;
  }

  if (text_eq(p, "stat") != 0u)
  {
    emit_stat();
    /* 同时打一份人可读的，方便没有上位机时排查 */
    tx_text("STAT t=%lums n=%lu drop=%lu adc=%lu uart=%lu rxf=%lu frm=%lu chg=%lu\r\n",
            (unsigned long)s_ctx.tick_ms,
            (unsigned long)s_ctx.stats.sample,
            (unsigned long)app_measure_drop_count(),
            (unsigned long)app_measure_err_count(),
            (unsigned long)usart_err_count(),
            (unsigned long)(usart_rx_fail_count() + app_cmd_rx_overflow_count()),
            (unsigned long)app_cmd_frame_err_count(),
            (unsigned long)s_ctx.alarm.change_count);
    return;
  }

  if (strncmp(p, "thr", 3) == 0u)
  {
    const char *q = p + 3;
    if ((parse_u16(&q, &a) == 0u) || (parse_u16(&q, &b) == 0u))
    {
      tx_text("ERR usage: thr <warn> <crit>\r\n");
      return;
    }
    if (ctx_set_thr(a, b) == 0u)
    {
      tx_text("ERR invalid thresholds\r\n");
      return;
    }
    tx_text("OK thr=%u/%u\r\n", (unsigned)a, (unsigned)b);
    return;
  }

  if (strncmp(p, "led", 3) == 0u)
  {
    const char *q = skip_sp(p + 3);
    if (text_eq(q, "on") != 0u)
    {
      ctx_set_led(1u);
      tx_text("OK led on\r\n");
    }
    else if (text_eq(q, "off") != 0u)
    {
      ctx_set_led(0u);
      tx_text("OK led off\r\n");
    }
    else
    {
      tx_text("ERR usage: led on|off\r\n");
    }
    return;
  }

  if (text_eq(p, "reset") != 0u)
  {
    ctx_reset_stats();
    tx_text("OK counters cleared\r\n");
    return;
  }

  tx_text("ERR unknown command: %s\r\n", p);
}

static const app_cmd_ops_t s_ops =
{
  ctx_set_thr,
  ctx_set_led,
  ctx_get_stat,
  ctx_reset_stats,
  ctx_ping,
  on_text_line
};

/* ---------------------------- 采样处理 ---------------------------- */
static void process_sample(uint16_t raw)
{
  uint16_t mv_raw  = app_measure_raw_to_mv(raw);
  uint16_t mv_filt = app_filter_push(&s_ctx.filter, mv_raw);
  app_level_t level = app_alarm_update(&s_ctx.alarm, mv_filt);

  s_ctx.stats.sample++;
  s_ctx.sample_seq++;

  if (level != s_ctx.last_level)
  {
    emit_event(s_ctx.last_level, level, mv_filt);
    s_ctx.last_level = level;
  }

#if (APP_REPORT_DECIM <= 1u)
  emit_sample(mv_raw, mv_filt, level);
#else
  if ((s_ctx.stats.sample % APP_REPORT_DECIM) == 0u)
  {
    emit_sample(mv_raw, mv_filt, level);
  }
#endif
}

/* ---------------------------- LED 指示 ---------------------------- */
static void led_write(uint8_t on)
{
  if (s_led_last == on)
  {
    return;      /* 状态没变就不重复写寄存器：主循环每秒会调用上万次 */
  }
  s_led_last = on;
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, (on != 0u) ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void update_led(void)
{
  uint8_t on;

  if (s_ctx.led_forced != 0u)
  {
    led_write(s_ctx.led_forced_on);
    return;
  }

  switch (s_ctx.alarm.level)
  {
    case APP_LEVEL_CRIT:
      on = 1u;                                  /* 常亮 */
      break;
    case APP_LEVEL_WARN:
      on = (uint8_t)(((s_ctx.tick_ms / LED_BLINK_MS) & 1u) != 0u);  /* 2Hz 闪 */
      break;
    case APP_LEVEL_NORMAL:
    default:
      on = 0u;
      break;
  }

  led_write(on);
}

/* ----------------------------- 对外接口 ----------------------------- */
void app_task_init(void)
{
  s_ctx.tick_ms       = 0u;
  s_ctx.key_pending   = 0u;
  s_ctx.stat_last_ms  = 0u;
  s_ctx.frame_seq     = 0u;
  s_ctx.sample_seq    = 0u;
  s_ctx.last_level    = APP_LEVEL_NORMAL;
  s_ctx.led_forced    = 0u;
  s_ctx.led_forced_on = 0u;

  app_filter_init(&s_ctx.filter);
  app_alarm_init(&s_ctx.alarm, APP_THR_WARN_MV, APP_THR_CRIT_MV);
  app_measure_init();
  app_cmd_init(&s_ops);

  emit_boot();
}

void app_task_run(void)
{
  uint16_t raw;
  uint8_t  n;

  /* 1. 命令优先：让上位机的阈值修改尽快生效 */
  app_cmd_poll();

  /* 2. 排空采样（有上限，避免一次处理太多拉长循环时间） */
  for (n = 0u; n < APP_SAMPLE_MAX_PER_RUN; n++)
  {
    if (app_measure_pop(&raw) == 0u)
    {
      break;
    }
    process_sample(raw);
  }

  /* 3. 按键：主动推送一次统计帧，上位机立即刷新面板 */
  if (s_ctx.key_pending != 0u)
  {
    s_ctx.key_pending = 0u;
    emit_stat();
  }

  /* 4. 周期推送统计 */
  if ((uint32_t)(s_ctx.tick_ms - s_ctx.stat_last_ms) >= APP_STAT_PERIOD_MS)
  {
    s_ctx.stat_last_ms = s_ctx.tick_ms;
    emit_stat();
  }

  /* 5. ADC 故障恢复：中断只置标志，真正的 DeInit/Init/校准在这里做。
        内部已按 APP_ADC_RECOVER_INTERVAL_MS 限速，不会打成忙循环。 */
  {
    uint8_t rec = app_measure_poll_recovery();
    if (rec == APP_MEAS_RECOVER_OK)
    {
      tx_text("[rec] adc restarted (ok=%lu fail=%lu)\r\n",
              (unsigned long)app_measure_recover_ok(),
              (unsigned long)app_measure_recover_fail());
    }
    else if (rec == APP_MEAS_RECOVER_FAIL)
    {
      tx_text("[rec] adc restart failed, retry in %ums (fail=%lu)\r\n",
              (unsigned)APP_ADC_RECOVER_INTERVAL_MS,
              (unsigned long)app_measure_recover_fail());
    }
    else
    {
      /* 无动作 */
    }
  }

  /* 6. 推动发送队列 */
  app_tx_poll();

  /* 7. LED 指示 */
  update_led();
}

void app_task_tick_isr(void)
{
  s_ctx.tick_ms++;
}

void app_task_on_key(void)
{
  /* 中断上下文只置标志：不能在 ISR 里组帧写发送队列，
     否则会和主循环的 app_tx_write 抢 s_head */
  s_ctx.key_pending = 1u;
}

uint32_t app_task_uptime_ms(void)
{
  return s_ctx.tick_ms;
}

uint8_t app_task_next_frame_seq(void)
{
  return s_ctx.frame_seq++;
}
