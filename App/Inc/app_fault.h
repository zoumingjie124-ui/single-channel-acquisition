/**
  ******************************************************************************
  * @file    app_fault.h
  * @brief   故障状态登记表（Stage 7C-2）：把"诊断结论"变成"该不该发帧、发什么字节"。
  *
  * 三个概念必须分清（7C-2 修正后的定义，别再混用）：
  *   active    —— 设备当前**真实**的故障状态（来自 app_diag）
  *   reported  —— **已经成功提交到 TX 发送队列**的故障状态
  *   上位机已知 —— 本模块**不掌握**任何关于"对端收到没有"的信息
  *
  * 注意第二条：`app_tx_write()` 返回成功只表示"字节进了 MCU 的发送环"，
  * 既不等于 UART/DMA 真的发完，更不等于上位机收到。所以 reported 只能叫
  * "已提交"，不能叫"已送达"。这正是还需要 GET_DIAG 快照来重新对齐的原因。
  *
  * 只有 active != reported 时才需要发帧；提交成功才把 reported 拉平。
  * 三道发送闸门见 app_fault_service() 的注释。
  ******************************************************************************
  */
#ifndef __APP_FAULT_H
#define __APP_FAULT_H

#include <stdint.h>
#include "app_proto.h"

/* 故障种类编号（与 app_proto.h 的 PROTO_FAULT_ID_* 一致） */
#define APP_FAULT_ID_STUCK      PROTO_FAULT_ID_STUCK

typedef struct
{
  uint8_t  active;        /* 设备当前真实状态位                        */
  uint8_t  reported;      /* 已成功提交到 TX 队列的状态位              */
  uint8_t  has_tried;     /* 是否提交过（决定 last_try_ms 是否有效）    */
  uint32_t last_try_ms;   /* 上次尝试提交的时刻（节流用）              */
  uint32_t retry_fail;    /* 连续提交失败次数（观测，不上报）          */
  uint32_t commit_cnt;    /* 成功提交次数（观测，含确认与恢复）        */
  uint16_t value;         /* 最近一次登记的观测量（STUCK = span mV）   */
} app_fault_t;

/* 发帧回调。返回 1 = 已成功提交到发送队列，0 = 未提交。
   payload 指向调用方栈上的 PROTO_LEN_FAULT 字节，返回后即失效。
   user 原样传回，app_fault 不解释它。 */
typedef uint8_t (*app_fault_emit_fn)(const uint8_t *payload, uint8_t len, void *user);

void app_fault_init(app_fault_t *f);

/**
  * @brief  登记某个故障的当前真实状态（每样本调）。
  * @note   只改 active / value，**不发帧**，与发送成败无关。未知 fault_id 被忽略。
  */
void app_fault_set_state(app_fault_t *f, uint8_t fault_id, uint8_t is_active, uint16_t value);

/**
  * @brief  尝试把"未提交的状态"发出去（主循环每轮调一次）。
  * @param  tx_has_room  发送队列是否装得下最坏一帧。由调用方用
  *                      `APP_TX_RING_SIZE-1-app_tx_pending() >= PROTO_MAX_FRAME` 算好传进来。
  *                      **必须在取序号之前判断**——先取号再发现写不进去，会白烧一个帧序号，
  *                      上位机那边就会记一次"整帧丢失"。
  * @param  now_ms       单调毫秒时基
  * @return 1 = 本次成功提交了一帧，0 = 没发（三道闸门见下）
  *
  * 三道闸门（缺一不可）：
  *   ① active == reported      → 无事可做（绝大多数样本走这条）
  *   ② tx_has_room == 0        → 队列没空间，**不取序号**
  *   ③ 距上次尝试 < APP_FAULT_RETRY_MIN_MS → 节流，避免每 10 ms 反复试
  */
uint8_t app_fault_service(app_fault_t *f, uint32_t now_ms, uint8_t tx_has_room,
                          app_fault_emit_fn emit, void *user);

/**
  * @brief  构造一帧"状态快照"载荷（event=0），**只读**，不改任何状态。
  * @param  t_ms 发帧时刻（调用方传入，不是请求到达时刻）
  * @return 写进载荷的 fault_id（0 = 未指定单一故障）
  * @note   无故障：fault_id=0, active=0, value=0
  *         恰好一个故障：fault_id=该编号, active=掩码, value=观测量
  *         多个故障并存：fault_id=0, active=掩码, value=0（不指定单一故障）
  */
uint8_t app_fault_build_snapshot(const app_fault_t *f, uint8_t *p, uint32_t t_ms);

/* 故障编号 -> 位掩码。未知编号返回 0 */
uint8_t app_fault_mask_of(uint8_t fault_id);

uint8_t  app_fault_active_mask(const app_fault_t *f);
uint8_t  app_fault_reported_mask(const app_fault_t *f);
uint8_t  app_fault_is_active(const app_fault_t *f, uint8_t fault_id);
uint32_t app_fault_retry_fail_count(const app_fault_t *f);
uint32_t app_fault_commit_count(const app_fault_t *f);

/* 按协议布局填 9 字节载荷。单独暴露出来是为了让两端能用同一组字节向量对拍 */
void app_fault_build_payload(uint8_t *p, uint32_t t_ms, uint8_t event,
                             uint8_t fault_id, uint8_t active_mask, uint16_t value);

#endif /* __APP_FAULT_H */
