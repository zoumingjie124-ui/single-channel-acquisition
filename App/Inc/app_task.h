/**
  ******************************************************************************
  * @file    app_task.h
  * @brief   应用层总调度：采样流水线、帧上报、LED 指示、文本命令。
  ******************************************************************************
  */
#ifndef __APP_TASK_H
#define __APP_TASK_H

#include <stdint.h>

void     app_task_init(void);

/* 主循环调用，非阻塞 */
void     app_task_run(void);

/* TIM2 1ms 中断调用 */
void     app_task_tick_isr(void);

/* EXTI 按键中断调用 */
void     app_task_on_key(void);

uint32_t app_task_uptime_ms(void);

/* 取当前帧序号并 +1。设备发出的所有帧（含 ACK）共用这一个计数器，
   上位机按“逐帧 +1”检测整帧丢失，不能各发各的 */
uint8_t  app_task_next_frame_seq(void);

#endif /* __APP_TASK_H */
