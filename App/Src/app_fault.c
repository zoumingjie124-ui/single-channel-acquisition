/**
  ******************************************************************************
  * @file    app_fault.c
  * @brief   故障上报登记表实现。纯逻辑，不依赖 HAL。
  *
  * 上报规则（只有两条，其余情况一律不发帧）：
  *   未上报 -> 已确认 ：发 event=CONFIRM，置位
  *   已上报 -> 未确认 ：发 event=RECOVER ，清位
  *
  * 这条规则天然满足三个要求：
  *   1. 不逐样本重复上报——ACTIVE 期间每样本都会调进来，但位已经置上了，直接返回；
  *   2. app_diag 在 ACTIVE<->RECOVERING 之间抖动时不会重复发确认帧，
  *      因为对上报层而言"故障还在"这件事没有变化；
  *   3. 多故障并存时，恢复只清自己那一位，掩码里其余位原样带出去，
  *      上位机因此能看出"还有别的故障没恢复"。
  ******************************************************************************
  */
#include "app_fault.h"

uint8_t app_fault_mask_of(uint8_t fault_id)
{
  switch (fault_id)
  {
    case APP_FAULT_ID_STUCK:
      return PROTO_FAULT_MASK_STUCK;
    default:
      return 0u;      /* 未登记的故障编号：不上报，也不改掩码 */
  }
}

void app_fault_init(app_fault_t *f)
{
  if (f == 0)
  {
    return;
  }
  f->active = 0u;
}

void app_fault_build_payload(uint8_t *p, uint32_t t_ms, uint8_t event,
                             uint8_t fault_id, uint8_t active_mask, uint16_t value)
{
  if (p == 0)
  {
    return;
  }
  proto_put_u32(&p[0], t_ms);
  p[4] = event;
  p[5] = fault_id;
  p[6] = active_mask;
  proto_put_u16(&p[7], value);
}

uint8_t app_fault_update(app_fault_t *f, uint8_t fault_id, uint8_t is_active,
                         uint32_t t_ms, uint16_t value,
                         app_fault_emit_fn emit, void *user)
{
  uint8_t bit;
  uint8_t event;
  uint8_t payload[PROTO_LEN_FAULT];

  if (f == 0)
  {
    return 0u;
  }

  bit = app_fault_mask_of(fault_id);
  if (bit == 0u)
  {
    return 0u;
  }

  if (is_active != 0u)
  {
    if ((f->active & bit) != 0u)
    {
      return 0u;      /* 已经报过了：抑制重复 */
    }
    f->active |= bit;
    event = PROTO_FAULT_EV_CONFIRM;
  }
  else
  {
    if ((f->active & bit) == 0u)
    {
      return 0u;      /* 本来就没报过：没有"恢复"可言 */
    }
    f->active &= (uint8_t)~bit;   /* 只清自己这一位 */
    event = PROTO_FAULT_EV_RECOVER;
  }

  if (emit == 0)
  {
    return 0u;
  }

  app_fault_build_payload(payload, t_ms, event, fault_id, f->active, value);
  emit(payload, PROTO_LEN_FAULT, user);
  return 1u;
}

uint8_t app_fault_active_mask(const app_fault_t *f)
{
  return (f == 0) ? 0u : f->active;
}

uint8_t app_fault_is_reported(const app_fault_t *f, uint8_t fault_id)
{
  uint8_t bit = app_fault_mask_of(fault_id);
  return (uint8_t)((f != 0) && (bit != 0u) && ((f->active & bit) != 0u));
}
