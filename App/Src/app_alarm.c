/**
  ******************************************************************************
  * @file    app_alarm.c
  * @brief   三级告警状态机实现。
  ******************************************************************************
  */
#include "app_alarm.h"

/* 计算"当前电平下可能的候选级别"，只允许相邻跳转 */
static app_level_t candidate(app_alarm_t *a, uint16_t mv)
{
  switch (a->level)
  {
    case APP_LEVEL_NORMAL:
      if (mv >= a->thr_warn_mv)
      {
        return APP_LEVEL_WARN;      /* 即便已超过 CRIT，也先确认到 WARN */
      }
      return APP_LEVEL_NORMAL;

    case APP_LEVEL_WARN:
      if (mv >= a->thr_crit_mv)
      {
        return APP_LEVEL_CRIT;
      }
      if (mv < (uint16_t)(a->thr_warn_mv - APP_THR_HYST_MV))
      {
        return APP_LEVEL_NORMAL;
      }
      return APP_LEVEL_WARN;        /* 滞回带内保持不变 */

    case APP_LEVEL_CRIT:
    default:
      if (mv < (uint16_t)(a->thr_crit_mv - APP_THR_HYST_MV))
      {
        return APP_LEVEL_WARN;
      }
      return APP_LEVEL_CRIT;
  }
}

void app_alarm_init(app_alarm_t *a, uint16_t warn_mv, uint16_t crit_mv)
{
  if (a == 0)
  {
    return;
  }
  a->level        = APP_LEVEL_NORMAL;
  a->up_cnt       = 0u;
  a->down_cnt     = 0u;
  a->thr_warn_mv  = warn_mv;
  a->thr_crit_mv  = crit_mv;
  a->change_count = 0u;
}

app_level_t app_alarm_update(app_alarm_t *a, uint16_t mv)
{
  app_level_t cand;

  if (a == 0)
  {
    return APP_LEVEL_NORMAL;
  }

  cand = candidate(a, mv);

  if (cand == a->level)
  {
    a->up_cnt   = 0u;
    a->down_cnt = 0u;
    return a->level;
  }

  /* 方向分开计数，互相打断：避免来回抖动时两个计数器同时累加 */
  if ((uint8_t)cand > (uint8_t)a->level)
  {
    a->down_cnt = 0u;
    a->up_cnt++;
    if (a->up_cnt >= APP_ALARM_DEBOUNCE_CNT)
    {
      a->level = cand;
      a->up_cnt = 0u;
      a->change_count++;
    }
  }
  else
  {
    a->up_cnt = 0u;
    a->down_cnt++;
    if (a->down_cnt >= APP_ALARM_DEBOUNCE_CNT)
    {
      a->level = cand;
      a->down_cnt = 0u;
      a->change_count++;
    }
  }

  return a->level;
}

uint8_t app_alarm_set_thresholds(app_alarm_t *a, uint16_t warn_mv, uint16_t crit_mv)
{
  if (a == 0)
  {
    return 0u;
  }
  /* 必须满足 回差 < WARN < CRIT <= 量程，否则滞回减法会下溢成超大值 */
  if (warn_mv <= APP_THR_HYST_MV)
  {
    return 0u;
  }
  if (crit_mv <= warn_mv)
  {
    return 0u;
  }
  if (crit_mv > APP_ADC_VREF_MV)
  {
    return 0u;
  }

  a->thr_warn_mv = warn_mv;
  a->thr_crit_mv = crit_mv;
  a->up_cnt      = 0u;
  a->down_cnt    = 0u;
  return 1u;
}

const char *app_alarm_name(app_level_t level)
{
  switch (level)
  {
    case APP_LEVEL_NORMAL: return "NORMAL";
    case APP_LEVEL_WARN:   return "WARN";
    case APP_LEVEL_CRIT:   return "CRIT";
    default:               return "UNKNOWN";
  }
}
