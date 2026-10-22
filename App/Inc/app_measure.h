/**
  ******************************************************************************
  * @file    app_measure.h
  * @brief   ADC 采集抽象层：这是应用层唯一允许直接触碰 HAL 的模块。
  *
  * V5 与 V1 的区别：
  *   V1 是 "Start -> Poll -> GetValue -> Stop" 轮询，CPU 参与每一次转换；
  *   V5 是 "TIM3 TRGO 触发 -> ADC -> DMA 循环搬运"，CPU 只在 DMA 半满/全满
  *   回调里把指针区间的数据搬进环形缓冲，采样时刻 100% 由硬件决定。
  *
  * 上层（app_task）只看到 "取一个 mV 值"，不关心后端是轮询还是 DMA，
  * 这就是把 app_measure 单独抽出来的意义。
  ******************************************************************************
  */
#ifndef __APP_MEASURE_H
#define __APP_MEASURE_H

#include <stdint.h>

typedef enum
{
  APP_MEAS_OK = 0,
  APP_MEAS_ERR_PARAM,
  APP_MEAS_ERR_START,
  APP_MEAS_ERR_EMPTY
} app_meas_status_t;

void     app_measure_init(void);

/* 启动 ADC + DMA（须在 TIM3 启动之前调用） */
uint8_t  app_measure_start(void);

/* 由 HAL_ADC_ConvHalfCpltCallback / ConvCpltCallback 调用（中断上下文） */
void     app_measure_on_half(void);
void     app_measure_on_cplt(void);
/* 由 HAL_ADC_ErrorCallback 调用（中断上下文）——只置恢复标志，不做重启 */
void     app_measure_on_error(void);

/* ------------------------- 故障恢复（主循环调用） ------------------------- */
#define APP_MEAS_RECOVER_NONE   0u   /* 没有待恢复事件，或未到重试间隔 */
#define APP_MEAS_RECOVER_OK     1u   /* 本次恢复成功，采样已重新启动 */
#define APP_MEAS_RECOVER_FAIL   2u   /* 本次尝试失败，会按间隔继续重试 */

/* 由主循环周期调用。内部按 APP_ADC_RECOVER_INTERVAL_MS 限速，
   真正执行 ADC 复位与重新初始化。绝不能在中断里调用。 */
uint8_t  app_measure_poll_recovery(void);

uint32_t app_measure_recover_ok(void);
uint32_t app_measure_recover_fail(void);

/* 主循环取一个原始值。返回 1 取到，0 表示当前无新数据 */
uint8_t  app_measure_pop(uint16_t *raw);

uint32_t app_measure_drop_count(void);
uint32_t app_measure_err_count(void);
uint16_t app_measure_ring_level(void);

/* 清零统计计数（不影响环形缓冲内的数据） */
void     app_measure_reset_counters(void);

/* 12 位原始值 -> mV。先乘后除，避免整数除法丢精度 */
uint16_t app_measure_raw_to_mv(uint16_t raw);

#endif /* __APP_MEASURE_H */
