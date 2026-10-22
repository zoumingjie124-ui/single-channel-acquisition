/**
  ******************************************************************************
  * @file    app_alarm.h
  * @brief   三级告警状态机：NORMAL -> WARN -> CRIT，带滞回与连续 N 点确认。
  *
  * 迁移规则（只允许相邻跳转，不允许跨级）：
  *
  *            mv >= WARN                   mv >= CRIT
  *   NORMAL ------------> WARN ---------------------> CRIT
  *          <------------      <---------------------
  *            mv < WARN-HYST        mv < CRIT-HYST
  *
  * 为什么只允许相邻：
  *   1. 与"正常 -> 预警 -> 确诊"的业务语义一致，上位机能画出清晰的三段过程；
  *   2. 每个状态都有独立的进入条件，故障升级路径可追溯。
  *
  * 代价：电压瞬间从 0 跳到 3200mV 时，需要 2 轮确认（2 × 3 点 = 60ms）
  *       才能到达 CRIT。100Hz 下 60ms 对电压监测是可接受的。
  *
  * 滞后（滞回）只作用在"降级"方向：升级看是否越过阈值，降级要低于阈值-回差，
  * 这样电压在阈值附近抖动时不会来回跳。
  ******************************************************************************
  */
#ifndef __APP_ALARM_H
#define __APP_ALARM_H

#include <stdint.h>
#include "app_types.h"
#include "app_config.h"

typedef struct
{
  app_level_t level;
  uint8_t     up_cnt;        /* 升级方向的连续满足计数 */
  uint8_t     down_cnt;      /* 降级方向的连续满足计数 */
  uint16_t    thr_warn_mv;
  uint16_t    thr_crit_mv;
  uint32_t    change_count;
} app_alarm_t;

void        app_alarm_init(app_alarm_t *a, uint16_t warn_mv, uint16_t crit_mv);

/* 输入滤波后的电压，返回确认后的级别。每个采样点调用一次 */
app_level_t app_alarm_update(app_alarm_t *a, uint16_t mv);

/* 在线修改阈值。返回 1 成功，0 参数非法 */
uint8_t     app_alarm_set_thresholds(app_alarm_t *a, uint16_t warn_mv, uint16_t crit_mv);

const char *app_alarm_name(app_level_t level);

#endif /* __APP_ALARM_H */
