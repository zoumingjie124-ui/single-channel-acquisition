/**
  ******************************************************************************
  * @file    app_fault.h
  * @brief   故障上报登记表（Stage 7C）：把"某个诊断结论变成已确认态"这件事，
  *          翻译成协议层的 FAULT 帧，并保证只在跃变时发一帧。
  *
  * 为什么单独一个模块，而不是直接写在 app_task.c 里：
  *   app_task.c 依赖 HAL，没法在 PC 上用 GCC 编译，
  *   而"确认/恢复/多故障并存/重复抑制"这几条恰恰是必须被测到的行为。
  *   本文件与 .c 只依赖 stdint + app_proto.h，和 app_diag 一样可以原样上 PC 测。
  *
  * 与 app_diag 的分工：
  *   app_diag  = 判断"信号是不是卡死了"（状态机，不知道协议存在）
  *   app_fault = 判断"这件事该不该上报、上报成什么字节"（不知道判据是什么）
  *   两者之间只有一个布尔量：某故障当前是否处于已确认态。
  *
  * 支持多故障：active 是位掩码。一个故障恢复只清自己那一位，
  * 其余故障的状态原样保留，也不会替它们发帧。
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
  /* 位掩码：已确认**且已上报**的故障集合。
     两个含义合成一个字段是有意的——本模块只关心"该不该发帧"，
     "确认了但故意不上报"这种状态目前没有需求，留两个字段反而容易走偏。 */
  uint8_t active;
} app_fault_t;

/* 发帧回调。payload 指向调用方栈上的 PROTO_LEN_FAULT 字节，返回后即失效。
   user 原样传回，app_fault 不解释它（app_task 用它接 emit_frame）。 */
typedef void (*app_fault_emit_fn)(const uint8_t *payload, uint8_t len, void *user);

void app_fault_init(app_fault_t *f);

/**
  * @brief  喂入某个故障的当前确认态，必要时产生一帧 FAULT。
  * @param  fault_id   故障种类；未登记的编号会被忽略（不产生任何帧）
  * @param  is_active  1 = 该故障当前处于已确认态，0 = 不处于
  * @param  t_ms       事件时刻（写进载荷）
  * @param  value      与故障相关的观测量（STUCK 用最近结算块的 span，mV）
  * @param  emit/user  发帧回调；emit 为 0 时只更新状态、不发帧
  * @return 1 = 本次调用产生了上报（emit 已被调用），0 = 无事件
  * @note   每样本调用一次是安全的：状态没跃变就返回 0，不会重复发帧。
  */
uint8_t app_fault_update(app_fault_t *f, uint8_t fault_id, uint8_t is_active,
                         uint32_t t_ms, uint16_t value,
                         app_fault_emit_fn emit, void *user);

/* 故障编号 -> 位掩码。未知编号返回 0 */
uint8_t app_fault_mask_of(uint8_t fault_id);

uint8_t app_fault_active_mask(const app_fault_t *f);
uint8_t app_fault_is_reported(const app_fault_t *f, uint8_t fault_id);

/* 按协议布局填 9 字节载荷。单独暴露出来是为了让两端能用同一组字节向量对拍 */
void app_fault_build_payload(uint8_t *p, uint32_t t_ms, uint8_t event,
                             uint8_t fault_id, uint8_t active_mask, uint16_t value);

#endif /* __APP_FAULT_H */
