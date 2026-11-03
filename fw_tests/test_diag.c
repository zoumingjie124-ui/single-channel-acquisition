/**
  ******************************************************************************
  * @file    test_diag.c
  * @brief   app_diag.c（信号卡死诊断）的单元测试
  *
  * 被测源码就是仓库里的 ../App/Src/app_diag.c，不复制、不重写。
  * 该模块不依赖 HAL，因此可以原样在 PC 上编译运行。
  *
  * 时间模型：测试用 10 ms 步进模拟 100 Hz 采样（与 APP_SAMPLE_RATE_HZ 一致），
  * 一个块 1000 ms。块语义：**结块样本只属于旧块，新块由下一个样本开启**，
  * 所以"喂满 k 个块"需要 101k 个样本，且新段落的第一个样本就是新块的第一个样本
  * —— 这让跨段切换的 span 完全可推演（见"跨块跳变的归属"一节）。
  ******************************************************************************
  */
#include "test_util.h"
#include "app_diag.h"
#include "app_config.h"

#define STEP_MS   10u                        /* 100 Hz 采样周期 */
#define MAX_FEED  200000u                    /* 喂数据的安全上限，防止死循环 */

/* 以固定值连续喂 n 个样本，返回下一个样本的时刻 */
static uint32_t feed(app_diag_t *d, uint16_t mv, uint32_t t, uint16_t n)
{
  uint16_t i;
  for (i = 0u; i < n; i++)
  {
    app_diag_update(d, mv, t);
    t += STEP_MS;
  }
  return t;
}

/* 喂固定值直到结满 k 个块，返回下一个样本的时刻。
   用"块样本数归零"识别结块，不依赖内部时序细节。 */
static uint32_t feed_blocks(app_diag_t *d, uint16_t mv, uint32_t t, uint8_t blocks)
{
  uint8_t  closed = 0u;
  uint32_t guard  = 0u;

  while ((closed < blocks) && (guard < MAX_FEED))
  {
    app_diag_update(d, mv, t);
    t += STEP_MS;
    if (app_diag_block_samples(d) == 0u)
    {
      closed++;
    }
    guard++;
  }
  return t;
}

/* 交替喂值直到结满 k 个块 */
static uint32_t feed_blocks_alt(app_diag_t *d, uint16_t a, uint16_t b, uint32_t t, uint8_t blocks)
{
  uint8_t  closed = 0u;
  uint32_t guard  = 0u;
  uint16_t i      = 0u;

  while ((closed < blocks) && (guard < MAX_FEED))
  {
    app_diag_update(d, (uint16_t)((i & 1u) ? b : a), t);
    t += STEP_MS;
    i++;
    if (app_diag_block_samples(d) == 0u)
    {
      closed++;
    }
    guard++;
  }
  return t;
}

/* ===================================================================== */
void test_diag(void)
{
  app_diag_t d;
  uint32_t t;

  SECTION("diag: 初始化与多次初始化");
  app_diag_init(&d);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  CHECK_EQ(app_diag_is_active(&d), 0);
  CHECK_EQ(app_diag_confirm_count(&d), 0);
  CHECK_EQ(app_diag_uneval_count(&d), 0);
  CHECK_EQ(app_diag_last_span(&d), 0);
  CHECK_EQ(app_diag_block_samples(&d), 0);

  /* 重复初始化必须幂等：结果与第一次完全一致 */
  app_diag_init(&d);
  app_diag_init(&d);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  CHECK_EQ(app_diag_confirm_count(&d), 0);
  CHECK_EQ(app_diag_uneval_count(&d), 0);
  CHECK_EQ(app_diag_block_samples(&d), 0);

  SECTION("diag: 默认关闭时不产生故障");
  app_diag_init(&d);                          /* enable = 默认值 0 */
  feed_blocks(&d, 1500u, 0u, 10u);            /* 10 秒完全恒定，若开着早该触发了 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  CHECK_EQ(app_diag_is_active(&d), 0);
  CHECK_EQ(app_diag_confirm_count(&d), 0);
  CHECK_EQ(app_diag_uneval_count(&d), 0);

  SECTION("diag: 稳定输入连续 3 个块才确认");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  t = feed_blocks(&d, 1500u, 0u, 1u);         /* 1 个块：CONFIRMING，不确认 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CONFIRMING);
  CHECK_EQ(app_diag_is_active(&d), 0);
  CHECK_EQ(app_diag_confirm_count(&d), 0);
  CHECK_EQ(app_diag_last_span(&d), 0u);

  t = feed_blocks(&d, 1500u, t, 1u);          /* 第 2 个块：仍未确认 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CONFIRMING);
  CHECK_EQ(app_diag_is_active(&d), 0);

  t = feed_blocks(&d, 1500u, t, 1u);          /* 第 3 个块：确认 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);
  CHECK_EQ(app_diag_is_active(&d), 1);
  CHECK_EQ(app_diag_confirm_count(&d), 1);

  SECTION("diag: 只稳定 1 或 2 个块不触发");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  feed_blocks(&d, 2000u, 0u, 1u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CONFIRMING);
  CHECK_EQ(app_diag_is_active(&d), 0);

  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  feed_blocks(&d, 2000u, 0u, 2u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CONFIRMING);
  CHECK_EQ(app_diag_is_active(&d), 0);
  CHECK_EQ(app_diag_confirm_count(&d), 0);

  SECTION("diag: 正常变化信号不误报");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  /* span = 300 mV，远超阈值 50 mV，且是缓慢的正常波动 */
  feed_blocks_alt(&d, 1000u, 1300u, 0u, 10u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  CHECK_EQ(app_diag_is_active(&d), 0);
  CHECK_EQ(app_diag_confirm_count(&d), 0);
  CHECK_EQ(app_diag_uneval_count(&d), 0);
  CHECK_EQ(app_diag_last_span(&d), 300u);

  /* span 恰好等于阈值算"干净"：50 不小于 50 */
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  feed_blocks_alt(&d, 1000u, 1050u, 0u, 5u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  CHECK_EQ(app_diag_last_span(&d), 50u);

  /* span = 49 小于 50 => 偏死块 */
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  feed_blocks_alt(&d, 1000u, 1049u, 0u, 1u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CONFIRMING);
  CHECK_EQ(app_diag_last_span(&d), 49u);

  SECTION("diag: 块结束后极值必须重置");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  /* 第 1 块：1000/1300 交替 => span 300，干净块 */
  t = feed_blocks_alt(&d, 1000u, 1300u, 0u, 1u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  CHECK_EQ(app_diag_last_span(&d), 300u);
  CHECK_EQ(app_diag_last_min(&d), 1000u);
  CHECK_EQ(app_diag_last_max(&d), 1300u);
  /* 第 2 块：恒定 1000 => span 必须是 0。
     若极值没被重置，min 仍是 1000、max 仍是 1300 => span 300（干净块），
     状态就会停在 CLEAR——所以下面两条断言能真正判别"重置了没有"。 */
  t = feed_blocks(&d, 1000u, t, 1u);
  CHECK_EQ(app_diag_last_span(&d), 0u);
  CHECK_EQ(app_diag_last_min(&d), 1000u);
  CHECK_EQ(app_diag_last_max(&d), 1000u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CONFIRMING);

  SECTION("diag: 跨块跳变的归属（新块由下一个样本开启）");
  /* 设计行为：结块样本只属于旧块，所以新块的 min/max 从新段落第一个样本起算，
     不会继承上一个块的电平。这里用 1300/1600 证明"没继承 1000"。 */
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  t = feed_blocks_alt(&d, 1000u, 1300u, 0u, 1u);   /* 旧块：1000~1300 */
  CHECK_EQ(app_diag_last_min(&d), 1000u);
  CHECK_EQ(app_diag_last_max(&d), 1300u);
  t = feed_blocks_alt(&d, 1300u, 1600u, t, 1u);    /* 新块：1300~1600 */
  CHECK_EQ(app_diag_last_min(&d), 1300u);          /* 不是 1000 => 没继承 */
  CHECK_EQ(app_diag_last_max(&d), 1600u);
  CHECK_EQ(app_diag_last_span(&d), 300u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);

  SECTION("diag: 故障后连续 2 个干净块恢复");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  t = feed_blocks(&d, 1500u, 0u, 3u);              /* 确认卡死 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);
  t = feed_blocks_alt(&d, 1000u, 1300u, t, 1u);    /* 1 个干净块：只能到 RECOVERING */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_RECOVERING);
  CHECK_EQ(app_diag_is_active(&d), 0);
  t = feed_blocks_alt(&d, 1000u, 1300u, t, 1u);    /* 第 2 个干净块：恢复 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  CHECK_EQ(app_diag_is_active(&d), 0);

  SECTION("diag: 恢复过程中来一个偏死块则恢复作废");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  t = feed_blocks(&d, 1500u, 0u, 3u);              /* ACTIVE */
  t = feed_blocks_alt(&d, 1000u, 1300u, t, 1u);    /* RECOVERING */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_RECOVERING);
  t = feed_blocks(&d, 1500u, t, 1u);               /* 偏死块 => 回 ACTIVE，干净链清零 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);
  CHECK_EQ(app_diag_is_active(&d), 1);
  /* 于是又要重新数 2 个干净块 */
  t = feed_blocks_alt(&d, 1000u, 1300u, t, 1u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_RECOVERING);
  t = feed_blocks_alt(&d, 1000u, 1300u, t, 1u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);

  SECTION("diag: 恢复后能再次触发（重复触发）");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  t = feed_blocks(&d, 1500u, 0u, 3u);              /* 第 1 次 ACTIVE */
  CHECK_EQ(app_diag_confirm_count(&d), 1);
  t = feed_blocks_alt(&d, 1000u, 1300u, t, 2u);    /* 恢复 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  t = feed_blocks(&d, 1500u, t, 3u);               /* 第 2 次 ACTIVE */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);
  CHECK_EQ(app_diag_confirm_count(&d), 2);
  /* 再恢复、再触发一次，确认计数持续累加 */
  t = feed_blocks_alt(&d, 1000u, 1300u, t, 2u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  t = feed_blocks(&d, 1500u, t, 3u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);
  CHECK_EQ(app_diag_confirm_count(&d), 3);

  SECTION("diag: 块结算的时间边界");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  app_diag_update(&d, 1000u, 1000u);               /* 开块：start = 1000, samples = 1 */
  CHECK_EQ(app_diag_block_samples(&d), 1u);
  t = feed(&d, 1000u, 1010u, 99u);                 /* 喂到 1990，距 1000 只有 990 ms */
  CHECK_EQ(t, 2000u);
  CHECK_EQ(app_diag_block_samples(&d), 100u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR); /* 还没结账 */
  app_diag_update(&d, 1000u, 1999u);               /* 999 ms：边界内侧，仍不结账 */
  CHECK_EQ(app_diag_block_samples(&d), 101u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  app_diag_update(&d, 1000u, 2000u);               /* 正好 1000 ms：结账 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CONFIRMING);
  CHECK_EQ(app_diag_last_span(&d), 0u);
  CHECK_EQ(app_diag_block_samples(&d), 0u);        /* 已结块，等下一个样本开新块 */

  SECTION("diag: 无有效样本（样本不足的块不判定）");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  app_diag_update(&d, 1000u, 1000u);               /* 开块 */
  app_diag_update(&d, 1000u, 6000u);               /* 隔了 5 s，但本块只有 2 个样本 */
  CHECK_EQ(app_diag_uneval_count(&d), 1);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR); /* 不能把"没数据"当成"没变化" */
  CHECK_EQ(app_diag_confirm_count(&d), 0);
  CHECK_EQ(app_diag_block_samples(&d), 0u);
  /* 这种块也不能打断/推进确认链：先造 2 个偏死块，再插一个不足块 */
  t = feed_blocks(&d, 1000u, 6000u, 2u);           /* 2 个偏死块 => CONFIRMING */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CONFIRMING);
  app_diag_update(&d, 1000u, t);                   /* 开一个新块 */
  app_diag_update(&d, 1000u, t + 2000u);           /* 2 个样本跨 2 s => 不可评估 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CONFIRMING);
  CHECK_EQ(app_diag_uneval_count(&d), 2);
  t = feed_blocks(&d, 1000u, t + 2010u, 1u);       /* 第 3 个偏死块 => 确认 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);
  CHECK_EQ(app_diag_confirm_count(&d), 1);

  SECTION("diag: 关闭后再打开不会误判");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  t = feed_blocks_alt(&d, 1000u, 1300u, 0u, 2u);   /* 变化信号 => CLEAR */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  app_diag_set_enable(&d, 0u);                     /* 关闭 */
  t = feed_blocks(&d, 1000u, t, 3u);               /* 关闭期间照常喂，状态不许动 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  CHECK_EQ(app_diag_uneval_count(&d), 0);
  app_diag_set_enable(&d, 1u);                     /* 重新打开 */
  app_diag_update(&d, 1000u, t);                   /* 第一个样本应开启新块 */
  CHECK_EQ(app_diag_block_samples(&d), 1);
  CHECK_EQ(app_diag_uneval_count(&d), 0);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);

  SECTION("diag: 关闭不影响已存在的故障状态");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  feed_blocks(&d, 1500u, 0u, 3u);                  /* ACTIVE */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);
  app_diag_set_enable(&d, 0u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);/* 关闭诊断不该让故障消失 */
  CHECK_EQ(app_diag_is_active(&d), 1);
  app_diag_set_enable(&d, 1u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);

  SECTION("diag: reset_counters 只清计数不清状态");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  t = feed_blocks(&d, 1500u, 0u, 3u);
  CHECK_EQ(app_diag_confirm_count(&d), 1);
  app_diag_update(&d, 1500u, t);                   /* 开块 */
  app_diag_update(&d, 1500u, t + 2000u);           /* 2 样本跨 2 s => 造一个 uneval */
  CHECK_EQ(app_diag_uneval_count(&d), 1);
  app_diag_reset_counters(&d);
  CHECK_EQ(app_diag_confirm_count(&d), 0);
  CHECK_EQ(app_diag_uneval_count(&d), 0);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);/* 状态必须保留 */
  CHECK_EQ(app_diag_is_active(&d), 1);

  SECTION("diag: 长时间运行与计数上限");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  /* 20 个干净块 + 3 个偏死块：计数链不能因为跑得久而失灵 */
  t = feed_blocks_alt(&d, 1000u, 1300u, 0u, 20u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CLEAR);
  t = feed_blocks(&d, 1500u, t, 3u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);
  CHECK_EQ(app_diag_confirm_count(&d), 1);
  /* ACTIVE 期间继续喂偏死块：confirm_cnt 不能重复累加 */
  t = feed_blocks(&d, 1500u, t, 5u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);
  CHECK_EQ(app_diag_confirm_count(&d), 1);
  /* CONFIRMING 期间喂很多偏死块也不能溢出到误确认（阈值是 3） */
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  t = feed_blocks(&d, 1500u, 0u, 2u);
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_CONFIRMING);
  t = feed_blocks(&d, 1500u, t, 200u);             /* 200 个偏死块 */
  CHECK_EQ(app_diag_state(&d), APP_DIAG_ST_ACTIVE);
  CHECK_EQ(app_diag_confirm_count(&d), 1);         /* 仍然只确认一次 */

  SECTION("diag: 极值统计的正确性（乱序高低）");
  app_diag_init(&d);
  app_diag_set_enable(&d, 1u);
  /* 一个块内先高、再低、再中：min/max 必须取到真极值 */
  app_diag_update(&d, 1200u, 0u);                  /* 开块 */
  t = feed(&d, 1200u, 10u, 40u);                   /* 1200 x40 */
  t = feed(&d, 900u, t, 20u);                      /* 900  x20 -> min */
  t = feed(&d, 1500u, t, 40u);                     /* 1500 x40 -> max */
  t = feed(&d, 1100u, t, 1u);                      /* 凑到 102 个样本，跨过块边界 */
  CHECK_EQ(app_diag_last_min(&d), 900u);
  CHECK_EQ(app_diag_last_max(&d), 1500u);
  CHECK_EQ(app_diag_last_span(&d), 600u);

  SECTION("diag: NULL 安全");
  /* 所有接口传 NULL 都不能崩（返回值合理） */
  app_diag_update(0, 1000u, 0u);
  app_diag_init(0);
  app_diag_set_enable(0, 1u);
  app_diag_reset_counters(0);
  CHECK_EQ(app_diag_state(0), APP_DIAG_ST_CLEAR);
  CHECK_EQ(app_diag_is_active(0), 0);
  CHECK_EQ(app_diag_last_span(0), 0);
  CHECK_EQ(app_diag_last_min(0), 0);
  CHECK_EQ(app_diag_last_max(0), 0);
  CHECK_EQ(app_diag_confirm_count(0), 0);
  CHECK_EQ(app_diag_uneval_count(0), 0);
  CHECK_EQ(app_diag_block_samples(0), 0);
}
