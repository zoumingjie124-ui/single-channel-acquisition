/**
  ******************************************************************************
  * @file    app_fault.c
  * @brief   故障状态登记表实现。纯逻辑，不依赖 HAL。
  *
  * 模型：状态差驱动（不是事件队列）
  * ---------------------------------
  *   set_state() 每样本更新 active；
  *   service()   只在 active != reported 时尝试提交一帧，提交成功才把 reported 拉平。
  *
  * 为什么不用"事件队列"：
  *   1. 队列要定长，满了照样丢；而且丢了以后没有"当前状态"可依；
  *   2. 链路断 5 分钟后补发 300 条历史边沿，对操作员没有意义；
  *   3. 状态差模型天然满足"不发错误事件"——永远不会出现"恢复"盖过"已确认"。
  * 代价是队列阻塞期间**中间边沿会被合并**（只保留最终状态）。
  * 这个取舍已获确认，配套的兜底机制是 GET_DIAG 快照。
  *
  * 三个坑（都有用例守着）：
  *   1. 先取帧序号再发现写不进去 → 白烧序号，上位机记一次"整帧丢失"。
  *      所以 tx_has_room 必须在取号之前判。
  *   2. 队列长期满时每 10 ms 试一次 → 节流 APP_FAULT_RETRY_MIN_MS。
  *   3. 快照若顺手改 reported，就等于"假装已经报过"，下次真故障反而不发了。
  *      所以 build_snapshot 是只读的（const）。
  ******************************************************************************
  */
#include "app_fault.h"
#include "app_config.h"

/* 取最低的置位位。diff 保证非 0 */
static uint8_t lowest_bit(uint8_t diff)
{
  uint8_t bit = 1u;
  while ((diff & bit) == 0u)
  {
    bit = (uint8_t)(bit << 1);
  }
  return bit;
}

/* 位掩码 -> 故障编号。未登记返回 0 */
static uint8_t id_of_mask(uint8_t bit)
{
  return (bit == PROTO_FAULT_MASK_STUCK) ? (uint8_t)PROTO_FAULT_ID_STUCK : 0u;
}

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
  f->active      = 0u;
  f->reported    = 0u;
  f->has_tried   = 0u;
  f->last_try_ms = 0u;
  f->retry_fail  = 0u;
  f->commit_cnt  = 0u;
  f->value       = 0u;
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

void app_fault_set_state(app_fault_t *f, uint8_t fault_id, uint8_t is_active, uint16_t value)
{
  uint8_t bit;

  if (f == 0)
  {
    return;
  }

  bit = app_fault_mask_of(fault_id);
  if (bit == 0u)
  {
    return;      /* 未知故障：不登记，也就永远不会被上报 */
  }

  if (is_active != 0u)
  {
    f->active |= bit;
  }
  else
  {
    f->active &= (uint8_t)~bit;
  }
  f->value = value;
}

uint8_t app_fault_service(app_fault_t *f, uint32_t now_ms, uint8_t tx_has_room,
                          app_fault_emit_fn emit, void *user)
{
  uint8_t diff;
  uint8_t bit;
  uint8_t event;
  uint8_t payload[PROTO_LEN_FAULT];

  if (f == 0)
  {
    return 0u;
  }

  /* 闸门①：已经对齐，什么都不用做（绝大多数样本走这里） */
  diff = (uint8_t)(f->active ^ f->reported);
  if (diff == 0u)
  {
    return 0u;
  }

  /* 闸门②：队列没空间就不发。**在取帧序号之前**，否则白烧一个序号 */
  if (tx_has_room == 0u)
  {
    return 0u;
  }

  /* 闸门③：节流。队列长期满时不能每 10 ms 试一次 */
  if ((f->has_tried != 0u) &&
      ((uint32_t)(now_ms - f->last_try_ms) < APP_FAULT_RETRY_MIN_MS))
  {
    return 0u;
  }
  f->has_tried   = 1u;
  f->last_try_ms = now_ms;

  if (emit == 0)
  {
    return 0u;    /* 没给出口：状态照旧欠着，reported 不动 */
  }

  /* 一次只提交一个位；多故障并存时下一次 service 再提交下一个。
     这样"提交成功才拉平"是按位成立的，不会被别的位带偏。 */
  bit   = lowest_bit(diff);
  event = ((f->active & bit) != 0u) ? (uint8_t)PROTO_FAULT_EV_CONFIRM
                                    : (uint8_t)PROTO_FAULT_EV_RECOVER;

  app_fault_build_payload(payload, now_ms, event, id_of_mask(bit), f->active, f->value);

  if (emit(payload, PROTO_LEN_FAULT, user) == 0u)
  {
    /* 提交失败：reported 保持原样，下次再来。
       注意 active 不回滚——设备的真实状态不受发送成败影响。 */
    f->retry_fail++;
    return 0u;
  }

  f->reported = (uint8_t)((f->reported & (uint8_t)~bit) | (f->active & bit));
  f->commit_cnt++;
  return 1u;
}

uint8_t app_fault_build_snapshot(const app_fault_t *f, uint8_t *p, uint32_t t_ms)
{
  uint8_t  fid = 0u;
  uint16_t val = 0u;

  if (f == 0)
  {
    return 0u;
  }

  if (f->active != 0u)
  {
    /* 只有"恰好一个故障"时才填具体编号与观测量。
       `active & (active-1) == 0` 即"只有一个位"的经典判断。
       多个故障并存时按约定填 fault_id=0、value=0，只靠 active 掩码表达。 */
    if ((f->active & (uint8_t)(f->active - 1u)) == 0u)
    {
      fid = id_of_mask(f->active);
      val = f->value;
    }
  }

  app_fault_build_payload(p, t_ms, PROTO_FAULT_EV_SNAPSHOT, fid, f->active, val);
  return fid;
}

uint8_t app_fault_active_mask(const app_fault_t *f)
{
  return (f == 0) ? 0u : f->active;
}

uint8_t app_fault_reported_mask(const app_fault_t *f)
{
  return (f == 0) ? 0u : f->reported;
}

uint8_t app_fault_is_active(const app_fault_t *f, uint8_t fault_id)
{
  uint8_t bit = app_fault_mask_of(fault_id);
  return (uint8_t)((f != 0) && (bit != 0u) && ((f->active & bit) != 0u));
}

uint32_t app_fault_retry_fail_count(const app_fault_t *f)
{
  return (f == 0) ? 0u : f->retry_fail;
}

uint32_t app_fault_commit_count(const app_fault_t *f)
{
  return (f == 0) ? 0u : f->commit_cnt;
}
