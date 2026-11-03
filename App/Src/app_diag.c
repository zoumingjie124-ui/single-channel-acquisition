/**
  ******************************************************************************
  * @file    app_diag.c
  * @brief   故障诊断实现（Stage 7B：信号卡死）。
  *
  * 算法：1 秒分块 + 块内极值
  * ---------------------------------
  *   每来一个样本：更新本块的 min/max（两次比较，O(1)）。
  *   当 now_ms - blk_start_ms >= APP_DIAG_BLOCK_MS 时，本块结算：
  *       span = blk_max - blk_min
  *       span <  APP_DIAG_STUCK_DELTA_MV  → 记一个"偏死块"
  *       span >= APP_DIAG_STUCK_DELTA_MV  → 记一个"干净块"
  *   然后**必须重置 min/max**，用当前样本开一个新块。
  *
  * 为什么用"分块极值"而不是"滑窗 min/max"：
  *   滑窗极值在样本流出窗口时无法 O(1) 更新（要重新扫描或维护单调队列），
  *   而分块方案每样本固定两次比较。代价是检测延迟为一个块的整数倍，
  *   对"秒级"的卡死判定完全可以接受。
  *
  * 为什么用 mv_raw 而不是滤波值：
  *   移动平均会把变化摊薄（一个阶跃第一拍只出 1/8），群延迟实测 40 ms。
  *   卡死判据是"这一秒内到底动没动"，用原始值最直接；用滤波值会把
  *   "缓慢变化"误判成"没动"（被平均掉了）。
  *
 * 三个必须防住的坑（都写进了单元测试）：
 *   1. 块结束不重置极值 → span 单调增长，故障永远不再触发；
 *   2. 只有一个样本的块 → span 恒为 0，会被误判成卡死（用 MIN_BLOCK_SAMPLES 挡）；
 *   3. 关闭后再打开 → blk_start_ms 是旧值，第一次 update 就会立刻结块（用 blk_started 挡）。
 *
 * 一个刻意的设计行为：**结块的那个样本只属于旧块，新块由下一个样本开启**
 * （block_close 只把 blk_started 清 0，不用旧样本给新块“播种”）。
 * 这样每个样本恰好属于一个块，且"新块的第一个样本"就是新段落的第一个样本，
 * 跨块的电平跳变会被算进它实际发生的那一个块里——语义干净，也让测试可推演。
  ******************************************************************************
  */
#include "app_diag.h"

/* 结块判定：now_ms 与 blk_start_ms 都是单调递增的 uint32，
   用无符号相减比较，即使 tick 回绕（49.7 天）也不会出错 */
#define BLOCK_ELAPSED(d, now) ((uint32_t)((now) - (d)->blk_start_ms) >= APP_DIAG_BLOCK_MS)

static void block_close(app_diag_t *d, uint32_t now_ms);

/**
  * @brief  复位"当前块"的统计，并用刚收到的这个样本开一个新块
  * @note   每个样本都不会被浪费：它既参与旧块结算，又作为新块的第一个点。
  */
static void block_restart(app_diag_t *d, uint16_t mv_raw, uint32_t now_ms)
{
  d->blk_min      = mv_raw;
  d->blk_max      = mv_raw;
  d->blk_samples  = 1u;
  d->blk_start_ms = now_ms;
  d->blk_started  = 1u;
}

/**
  * @brief  块结算：根据 span 推进状态机
  * @param  samples  本块实际计入的样本数
  *
  * 状态机（括号内为进入该状态时的计数）：
  *
  *            偏死块               偏死块×3
  *   CLEAR ──────────────► CONFIRMING ──────────────► ACTIVE (confirm_cnt++)
  *     ▲                        │                        │  │
  *     │                        │ 干净块                  │  │ 干净块
  *     │                        ▼                        ▼  ▼
  *     │                     CLEAR                  RECOVERING ──干净块×2──► CLEAR
  *     │                                                   │  ▲
  *     └───────────────────────────────────────────────────┘  └── 偏死块：回到 ACTIVE
  *
  * 注意 ACTIVE 期间来一个偏死块：直接回 ACTIVE 并把 clean_blocks 清零，
  * 不经过 CONFIRMING（已经确认过的事情不需要再确认一遍）。
  */
static void block_close(app_diag_t *d, uint32_t now_ms)
{
  uint16_t span;
  uint16_t samples;
  uint8_t  evaluable;

  span    = (uint16_t)(d->blk_max - d->blk_min);
  samples = d->blk_samples;      /* 先取下来：下面要清零，顺序不能反 */

  /* 观测值先记下来，即使这块不可评估也有参考意义 */
  d->last_span = span;
  d->last_min  = d->blk_min;
  d->last_max  = d->blk_max;
  d->blk_samples = 0u;   /* 本块已结算，等待下一个样本开新块 */

  /* 样本不足的块不参与判定：只有一个样本时 span 恒为 0，
     那是"没数据"，不是"没变化"——后者属于采样停摆，不在本模块职责内 */
  evaluable = (samples >= APP_DIAG_MIN_BLOCK_SAMPLES) ? 1u : 0u;
  if (evaluable == 0u)
  {
    d->uneval_blocks++;
    d->blk_started = 0u;    /* 同样不开新块，交给下一个样本 */
    return;
  }

  if (span < APP_DIAG_STUCK_DELTA_MV)
  {
    /* ---- 偏死块 ----
       干净链断开：恢复要求"连续"干净块，来一个偏死块就重新数。
       放在分支最前面，四种状态统一处理。 */
    d->clean_blocks = 0u;

    if (d->stuck_blocks < 0xFFu)
    {
      d->stuck_blocks++;      /* 理论上限被 APP_DIAG_STUCK_CONFIRM_BLOCKS 钳住，不会溢出 */
    }

    switch (d->state)
    {
      case APP_DIAG_ST_CLEAR:
      case APP_DIAG_ST_CONFIRMING:
        if (d->stuck_blocks >= APP_DIAG_STUCK_CONFIRM_BLOCKS)
        {
          d->state       = APP_DIAG_ST_ACTIVE;
          d->stuck_blocks = 0u;   /* 清掉，恢复后要从零开始重新数 */
          d->confirm_cnt++;
          d->last_change_ms = now_ms;
        }
        else
        {
          d->state = APP_DIAG_ST_CONFIRMING;
        }
        break;

      case APP_DIAG_ST_ACTIVE:
      case APP_DIAG_ST_RECOVERING:
      default:
        /* 已确认过的状态：偏死块只是"取消恢复"，不需要再确认 */
        d->state = APP_DIAG_ST_ACTIVE;
        break;
    }
  }
  else
  {
    /* ---- 干净块 ---- */
    d->stuck_blocks = 0u;     /* 确认链断开：必须"连续"偏死块 */

    switch (d->state)
    {
      case APP_DIAG_ST_ACTIVE:
      case APP_DIAG_ST_RECOVERING:
        if (d->clean_blocks < 0xFFu)
        {
          d->clean_blocks++;
        }
        if (d->clean_blocks >= APP_DIAG_STUCK_RECOVER_BLOCKS)
        {
          d->state        = APP_DIAG_ST_CLEAR;
          d->clean_blocks = 0u;
          d->last_change_ms = now_ms;
        }
        else
        {
          d->state = APP_DIAG_ST_RECOVERING;
        }
        break;

      case APP_DIAG_ST_CLEAR:
      case APP_DIAG_ST_CONFIRMING:
      default:
        /* 还没确认过：一个干净块就回到 CLEAR，确认链作废 */
        d->state        = APP_DIAG_ST_CLEAR;
        d->clean_blocks = 0u;
        break;
    }
  }

  /* 不开新块：下一个样本负责开。理由见文件头"刻意的设计行为"。
     好处是每个样本恰好属于一个块，且新段落的第一个样本就是新块的第一个样本。 */
  d->blk_started = 0u;
}

/* ------------------------------- 对外接口 ------------------------------- */

void app_diag_init(app_diag_t *d)
{
  if (d == 0)
  {
    return;
  }

  d->enable         = (uint8_t)(APP_DIAG_STUCK_ENABLE_DEFAULT != 0u);
  d->blk_min        = 0u;
  d->blk_max        = 0u;
  d->blk_samples    = 0u;
  d->blk_start_ms   = 0u;
  d->blk_started    = 0u;
  d->stuck_blocks   = 0u;
  d->clean_blocks   = 0u;
  d->state          = APP_DIAG_ST_CLEAR;
  d->confirm_cnt    = 0u;
  d->uneval_blocks  = 0u;
  d->last_span      = 0u;
  d->last_min       = 0u;
  d->last_max       = 0u;
  d->last_change_ms = 0u;
}

void app_diag_set_enable(app_diag_t *d, uint8_t enable)
{
  if (d == 0)
  {
    return;
  }

  if (enable != 0u)
  {
    /* 打开时强制重置块统计：
       否则 blk_start_ms 可能是很久以前的值，下一个样本会立刻结出一个
       "只有一个样本"的块，虽然会被 MIN_BLOCK_SAMPLES 挡掉，
       但会把 uneval_blocks 弄脏，也让第一个块的时间基准不正确。 */
    d->blk_started = 0u;
    d->enable      = 1u;
  }
  else
  {
    /* 关闭：不改变状态机与计数，只是不再判定。
       正在进行的故障保持原状——关闭诊断不该让一个真实故障消失。 */
    d->enable = 0u;
  }
}

void app_diag_update(app_diag_t *d, uint16_t mv_raw, uint32_t now_ms)
{
  if (d == 0)
  {
    return;
  }
  if (d->enable == 0u)
  {
    return;               /* 关闭时整个函数只剩两次判断，不影响调用方时序 */
  }

  /* 第一个样本（或重新打开后的第一个样本）负责开块 */
  if (d->blk_started == 0u)
  {
    block_restart(d, mv_raw, now_ms);
    return;
  }

  d->blk_samples++;
  if (mv_raw < d->blk_min)
  {
    d->blk_min = mv_raw;
  }
  if (mv_raw > d->blk_max)
  {
    d->blk_max = mv_raw;
  }

  if (BLOCK_ELAPSED(d, now_ms))
  {
    block_close(d, now_ms);
  }
}

void app_diag_reset_counters(app_diag_t *d)
{
  if (d == 0)
  {
    return;
  }
  /* 只清计数，不清 state / 块统计：理由见头文件注释 */
  d->confirm_cnt   = 0u;
  d->uneval_blocks = 0u;
}

/* ------------------------------- 查询接口 ------------------------------- */

app_diag_state_t app_diag_state(const app_diag_t *d)
{
  return (d == 0) ? APP_DIAG_ST_CLEAR : d->state;
}

uint8_t app_diag_is_active(const app_diag_t *d)
{
  return (uint8_t)((d != 0) && (d->state == APP_DIAG_ST_ACTIVE));
}

uint16_t app_diag_last_span(const app_diag_t *d)
{
  return (d == 0) ? 0u : d->last_span;
}

uint16_t app_diag_last_min(const app_diag_t *d)
{
  return (d == 0) ? 0u : d->last_min;
}

uint16_t app_diag_last_max(const app_diag_t *d)
{
  return (d == 0) ? 0u : d->last_max;
}

uint32_t app_diag_confirm_count(const app_diag_t *d)
{
  return (d == 0) ? 0u : d->confirm_cnt;
}

uint32_t app_diag_uneval_count(const app_diag_t *d)
{
  return (d == 0) ? 0u : d->uneval_blocks;
}

uint16_t app_diag_block_samples(const app_diag_t *d)
{
  return (d == 0) ? 0u : d->blk_samples;
}
