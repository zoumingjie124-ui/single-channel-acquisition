/**
  ******************************************************************************
  * @file    test_filter.c
  * @brief   app_filter.c 的单元测试（编译真实固件源码）
  *
  * 覆盖：启动阶段的"部分窗口"、满窗、覆盖式滑窗、极值、计数饱和、NULL 保护，
  *       以及与"最近 N 个输入的算术平均"这一规格的逐点比对。
  ******************************************************************************
  */
#include "test_util.h"
#include "app_filter.h"

/* 按规格（最近 min(n, DEPTH) 个输入的算术平均）独立算期望值 */
static uint16_t ref_mean(const uint16_t *hist, int n)
{
  uint32_t sum = 0u;
  int i;
  if (n == 0) { return 0u; }
  for (i = 0; i < n; i++) { sum += hist[i]; }
  return (uint16_t)(sum / (uint32_t)n);
}

void test_filter(void)
{
  app_filter_t f;
  uint16_t hist[APP_FILTER_DEPTH];
  int n_hist;
  int i;
  const int depth = (int)APP_FILTER_DEPTH;

  /* ---------------- 初始化 ---------------- */
  SECTION("filter: 初始化");
  {
    app_filter_init(&f);
    CHECK_EQ(f.count, 0u);
    CHECK_EQ(app_filter_count(&f), 0u);
    CHECK_EQ(app_filter_value(&f), 0u);
    CHECK_EQ(f.sum, 0u);
    CHECK_EQ(f.index, 0u);
  }

  /* ---------------- 启动阶段用 count 而不是 DEPTH ---------------- */
  SECTION("filter: 启动阶段按已填个数平均");
  {
    app_filter_init(&f);
    CHECK_EQ(app_filter_push(&f, 1000u), 1000u);      /* 1 个点就是它自己 */
    CHECK_EQ(f.count, 1u);
    CHECK_EQ(app_filter_push(&f, 3000u), 2000u);      /* (1000+3000)/2 */
    CHECK_EQ(f.count, 2u);
    CHECK_EQ(app_filter_push(&f, 2000u), 2000u);      /* (1000+3000+2000)/3 = 2000 */
    CHECK_EQ(f.count, 3u);
  }

  /* ---------------- 满窗 ---------------- */
  SECTION("filter: 满窗平均");
  {
    app_filter_init(&f);
    for (i = 0; i < depth; i++)
    {
      (void)app_filter_push(&f, (uint16_t)((i + 1) * 100));
    }
    CHECK_EQ(f.count, depth);
    /* 100..800 的平均 = 450 */
    CHECK_EQ(app_filter_value(&f), 450u);
    CHECK_EQ(f.sum, 3600u);
  }

  /* ---------------- 覆盖式滑窗（先减旧值再加新值） ---------------- */
  SECTION("filter: 第 9 个点开始覆盖最旧值");
  {
    app_filter_init(&f);
    for (i = 0; i < depth; i++)
    {
      (void)app_filter_push(&f, (uint16_t)((i + 1) * 100));
    }
    /* 窗口 100..800，推入 900 -> 窗口 200..900，平均 550 */
    CHECK_EQ(app_filter_push(&f, 900u), 550u);
    CHECK_EQ(f.sum, 4400u);
    CHECK_EQ(f.count, depth);              /* 计数不再增长 */

    /* 再推 1000 -> 窗口 300..1000，平均 650 */
    CHECK_EQ(app_filter_push(&f, 1000u), 650u);

    /* 连续同值：窗口会被同值填满 */
    for (i = 0; i < depth; i++) { (void)app_filter_push(&f, 1234u); }
    CHECK_EQ(app_filter_value(&f), 1234u);
  }

  /* ---------------- 极值：uint32 求和不能溢出 ---------------- */
  SECTION("filter: 满量程 65535 不溢出");
  {
    app_filter_init(&f);
    for (i = 0; i < depth; i++) { (void)app_filter_push(&f, 65535u); }
    CHECK_EQ(app_filter_value(&f), 65535u);
    CHECK_EQ(f.sum, 65535L * depth);       /* 524280，uint32 装得下 */

    /* 高低交替：均值应回到中值附近 */
    for (i = 0; i < depth; i++)
    {
      (void)app_filter_push(&f, (i & 1) ? 65535u : 0u);
    }
    CHECK_EQ(app_filter_value(&f), 32767u);           /* (65535*4)/8 = 32767.5 -> 截断 */
  }

  /* ---------------- 阶跃：滤波不做限幅（APP_RATE_LIMIT_MV = 0） ---------------- */
  SECTION("filter: 限幅关闭时阶跃原样进入窗口");
  {
    app_filter_init(&f);
    (void)app_filter_push(&f, 0u);
    /* 单点阶跃后，第一个输出就是平均值（不是限幅值） */
    CHECK_EQ(app_filter_push(&f, 3300u), 1650u);
#if (APP_RATE_LIMIT_MV == 0u)
    CHECK_EQ(f.last, 3300u);                          /* last 记录的是原值 */
#endif
  }

  /* ---------------- 与规格逐点比对 ---------------- */
  SECTION("filter: 40 点伪随机序列 vs 规格期望值");
  {
    uint32_t seed = 12345u;
    int k;
    app_filter_init(&f);
    n_hist = 0;

    for (k = 0; k < 40; k++)
    {
      uint16_t v;
      seed = seed * 1103515245u + 12345u;             /* 固定种子，可复现 */
      v = (uint16_t)((seed >> 16) & 0x0FFFu);         /* 0..4095，覆盖整个码域 */

      if (n_hist < depth) { n_hist++; }
      for (i = n_hist - 1; i > 0; i--) { hist[i] = hist[i - 1]; }
      hist[0] = v;

      CHECK_EQ(app_filter_push(&f, v), ref_mean(hist, n_hist));
    }
  }

  /* ---------------- NULL 保护 ---------------- */
  SECTION("filter: NULL 保护");
  {
    app_filter_init((app_filter_t *)0);                            /* 不应崩溃 */
    CHECK_EQ(app_filter_push((app_filter_t *)0, 1000u), 0u);
    CHECK_EQ(app_filter_value((app_filter_t *)0), 0u);
    CHECK_EQ(app_filter_count((app_filter_t *)0), 0u);
  }

  /* ---------------- 计数上限 ---------------- */
  SECTION("filter: count 饱和于 DEPTH");
  {
    app_filter_init(&f);
    for (i = 0; i < depth * 3; i++) { (void)app_filter_push(&f, 100u); }
    CHECK_EQ(app_filter_count(&f), depth);
    CHECK_EQ(app_filter_value(&f), 100u);
  }
}
