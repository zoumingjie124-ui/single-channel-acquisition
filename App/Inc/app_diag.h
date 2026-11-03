/**
  ******************************************************************************
  * @file    app_diag.h
  * @brief   故障诊断模块（Stage 7B：只实现"信号卡死"检测）。
  *
  * 设计约束（本轮硬性要求，逐条对应）：
  *   1. 不依赖 HAL / main.h / 任何硬件寄存器 —— 本文件与 .c 只包含 stdint 与 app_config，
  *      因此可以用 GCC 在 PC 上直接编译并单元测试（与 Stage 4 的 app_proto 同一套办法）。
  *   2. 输入只有 (mv_raw, now_ms)，都是 process_sample() 现成能拿到的值。
  *   3. 每个样本 O(1)：只做两次比较 + 一次自增，不扫描窗口。
  *   4. 只诊断、不干预：不回写滤波值、不碰告警状态、不改采样节奏。
  *
  * 与其它模块的分工：
  *   app_alarm  = 电平越界（"值太大了"）
  *   app_diag   = 行为异常（"值不动了"）
  *   两者输入同源、状态独立、互不影响。
  ******************************************************************************
  */
#ifndef __APP_DIAG_H
#define __APP_DIAG_H

#include <stdint.h>

/* app_diag_t 里用到的阈值参数集中在 app_config.h */
#include "app_config.h"

/* ------------------------------ 卡死状态机 ------------------------------ */
typedef enum
{
  APP_DIAG_ST_CLEAR       = 0,  /* 正常：没有可疑的块                */
  APP_DIAG_ST_CONFIRMING  = 1,  /* 有 1..N-1 个连续"偏死"块，未确认   */
  APP_DIAG_ST_ACTIVE      = 2,  /* 已确认卡死                       */
  APP_DIAG_ST_RECOVERING  = 3   /* 曾确认过，正在等连续干净块恢复     */
} app_diag_state_t;

typedef struct
{
  /* ---- 配置：默认关闭，由 app_diag_set_enable() 打开 ---- */
  uint8_t enable;

  /* ---- 当前块的极值统计 ---- */
  uint16_t blk_min;        /* 本块最小值 */
  uint16_t blk_max;        /* 本块最大值 */
  uint16_t blk_samples;    /* 本块已计入的样本数（用于判断块是否可评估） */
  uint32_t blk_start_ms;   /* 本块起始时刻 */
  uint8_t  blk_started;    /* 0 = 下一个样本要负责开一个新块 */

  /* ---- 连续块计数 ---- */
  uint8_t  stuck_blocks;   /* 连续 span < 阈值 的块数 */
  uint8_t  clean_blocks;   /* 连续 span >= 阈值 的块数 */

  /* ---- 状态与观测 ---- */
  app_diag_state_t state;
  uint32_t confirm_cnt;    /* 进入 ACTIVE 的次数（含重复触发） */
  uint32_t uneval_blocks;  /* 因样本不足而被跳过的块数（排查用） */
  uint16_t last_span;      /* 最近一个已结算块的 span */
  uint16_t last_min;       /* 最近一个已结算块的 min */
  uint16_t last_max;       /* 最近一个已结算块的 max */
  uint32_t last_change_ms; /* 最近一次状态跳变时刻（观测用，不参与判定） */
} app_diag_t;

/* ------------------------------- 接口 ------------------------------- */

/**
  * @brief  初始化。可重复调用（多次调用结果一致）。
  * @note   enable 会被设为 app_config.h 里的默认值（当前为 0 = 关闭）。
  */
void app_diag_init(app_diag_t *d);

/**
  * @brief  打开/关闭卡死检测
  * @note   关闭时 update() 直接返回，不改变任何状态；
  *         从关闭到打开的那一刻会重置块统计，避免用过期时间算出"只有一个样本的块"。
  */
void app_diag_set_enable(app_diag_t *d, uint8_t enable);

/**
  * @brief  每样本调用一次（在 process_sample() 里，与滤波/告警同层级）
  * @param  mv_raw  原始电压 mV（不是滤波值，理由见实现处注释）
  * @param  now_ms  单调递增的毫秒时基（app_task_uptime_ms()）
  * @note   本函数不做任何阻塞操作，耗时与窗口深度无关。
  */
void app_diag_update(app_diag_t *d, uint16_t mv_raw, uint32_t now_ms);

/**
  * @brief  只清零计数器，不动状态机
  * @note   与 app_measure_reset_counters() 同一原则：若此刻正是 ACTIVE，
  *   清状态会让一个真实存在的故障"凭空消失"，比计数不准严重得多。
  */
void app_diag_reset_counters(app_diag_t *d);

/* ------------------------------ 查询接口 ------------------------------ */
app_diag_state_t app_diag_state(const app_diag_t *d);
uint8_t          app_diag_is_active(const app_diag_t *d);   /* 1 = 已确认卡死 */
uint16_t         app_diag_last_span(const app_diag_t *d);
uint16_t         app_diag_last_min(const app_diag_t *d);
uint16_t         app_diag_last_max(const app_diag_t *d);
uint32_t         app_diag_confirm_count(const app_diag_t *d);
uint32_t         app_diag_uneval_count(const app_diag_t *d);
uint16_t         app_diag_block_samples(const app_diag_t *d);

#endif /* __APP_DIAG_H */
