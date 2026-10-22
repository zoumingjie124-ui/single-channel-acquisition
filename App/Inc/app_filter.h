/**
  ******************************************************************************
  * @file    app_filter.h
  * @brief   移动平均 + 可选限幅滤波。
  *
  * 移动平均用"环形覆盖 + 维护累加和"实现，每个新样本只需一次减法和一次加法，
  * 复杂度 O(1)，与窗口深度无关（不是每来一个点就把 8 个元素加一遍）。
  ******************************************************************************
  */
#ifndef __APP_FILTER_H
#define __APP_FILTER_H

#include <stdint.h>
#include "app_config.h"

typedef struct
{
  uint16_t buf[APP_FILTER_DEPTH];
  uint32_t sum;       /* 窗口内元素之和，避免每次重新累加 */
  uint16_t index;     /* 下一个要覆盖的位置 */
  uint16_t count;     /* 已填入的样本数，启动阶段小于深度 */
  uint16_t last;      /* 限幅滤波参考值 */
  uint8_t  has_last;
} app_filter_t;

void     app_filter_init(app_filter_t *f);

/* 推入一个新样本，返回当前平均值。限幅开启时先对输入做变化率限制 */
uint16_t app_filter_push(app_filter_t *f, uint16_t value);

/* 当前平均值（不推入新样本）。窗口为空返回 0 */
uint16_t app_filter_value(const app_filter_t *f);

uint16_t app_filter_count(const app_filter_t *f);

#endif /* __APP_FILTER_H */
