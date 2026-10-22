/**
  ******************************************************************************
  * @file    app_filter.c
  * @brief   移动平均 + 限幅滤波实现。
  ******************************************************************************
  */
#include "app_filter.h"

void app_filter_init(app_filter_t *f)
{
  uint16_t i;

  if (f == 0)
  {
    return;
  }
  for (i = 0u; i < APP_FILTER_DEPTH; i++)
  {
    f->buf[i] = 0u;
  }
  f->sum      = 0u;
  f->index    = 0u;
  f->count    = 0u;
  f->last     = 0u;
  f->has_last = 0u;
}

static uint16_t rate_limit(app_filter_t *f, uint16_t value)
{
#if (APP_RATE_LIMIT_MV > 0u)
  int32_t diff;

  if (f->has_last != 0u)
  {
    diff = (int32_t)value - (int32_t)f->last;
    if (diff > (int32_t)APP_RATE_LIMIT_MV)
    {
      value = (uint16_t)((uint32_t)f->last + APP_RATE_LIMIT_MV);
    }
    else if (diff < -(int32_t)APP_RATE_LIMIT_MV)
    {
      /* 注意：last 小于限幅值时减法会下溢，这里用 int32 计算后再判断，
         只有在结果非负时才做 uint16 截断 */
      int32_t v = (int32_t)f->last - (int32_t)APP_RATE_LIMIT_MV;
      value = (v > 0) ? (uint16_t)v : 0u;
    }
    else
    {
      /* 在允许的变化率内，原样通过 */
    }
  }
#else
  (void)f;
#endif
  f->last     = value;
  f->has_last = 1u;
  return value;
}

uint16_t app_filter_push(app_filter_t *f, uint16_t value)
{
  if (f == 0)
  {
    return 0u;
  }

  value = rate_limit(f, value);

  /* 环形覆盖：先减掉被挤出的旧值，再加上新值 */
  f->sum -= f->buf[f->index];
  f->buf[f->index] = value;
  f->sum += value;

  f->index = (uint16_t)((f->index + 1u) & (APP_FILTER_DEPTH - 1u));
  if (f->count < APP_FILTER_DEPTH)
  {
    f->count++;
  }

  return app_filter_value(f);
}

uint16_t app_filter_value(const app_filter_t *f)
{
  if ((f == 0) || (f->count == 0u))
  {
    return 0u;
  }

  /* 这里必须用除法：启动阶段 count 可能小于深度，右移会得到偏小的结果。
     稳定后 count == APP_FILTER_DEPTH（2 的幂），编译器会自动优化成移位。 */
  return (uint16_t)(f->sum / f->count);
}

uint16_t app_filter_count(const app_filter_t *f)
{
  return (f == 0) ? 0u : f->count;
}
