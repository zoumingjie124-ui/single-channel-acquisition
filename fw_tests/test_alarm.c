/**
  ******************************************************************************
  * @file    test_alarm.c
  * @brief   app_alarm.c 的单元测试（编译真实固件源码）
  *
  * 覆盖：三级判级、相邻跳转、滞回去抖、阈值校验（含 uint16 回绕回归）、NULL 保护。
  ******************************************************************************
  */
#include <string.h>
#include "test_util.h"
#include "app_alarm.h"

/* 连续喂同一个值 n 次，返回最终级别 */
static app_level_t feed_n(app_alarm_t *a, uint16_t mv, int n)
{
  app_level_t lv = a->level;
  int i;
  for (i = 0; i < n; i++)
  {
    lv = app_alarm_update(a, mv);
  }
  return lv;
}

void test_alarm(void)
{
  app_alarm_t a;

  /* ---------------- 初始化 ---------------- */
  SECTION("alarm: 初始化");
  {
    app_alarm_init(&a, APP_THR_WARN_MV, APP_THR_CRIT_MV);
    CHECK_EQ(a.level, APP_LEVEL_NORMAL);
    CHECK_EQ(a.change_count, 0u);
    CHECK_EQ(a.thr_warn_mv, APP_THR_WARN_MV);
    CHECK_EQ(a.thr_crit_mv, APP_THR_CRIT_MV);
  }

  /* ---------------- 正常区不跳变 ---------------- */
  SECTION("alarm: 正常区保持");
  {
    app_alarm_init(&a, 2400u, 3000u);
    CHECK_EQ(feed_n(&a, 0u, 10), APP_LEVEL_NORMAL);
    CHECK_EQ(feed_n(&a, 2399u, 10), APP_LEVEL_NORMAL);
    CHECK_EQ(a.change_count, 0u);
  }

  /* ---------------- 去抖：必须连续 3 次 ---------------- */
  SECTION("alarm: 进入 WARN 需连续 3 次（APP_ALARM_DEBOUNCE_CNT）");
  {
    app_alarm_init(&a, 2400u, 3000u);
    CHECK_EQ(feed_n(&a, 2400u, 1), APP_LEVEL_NORMAL);   /* 第 1 次 */
    CHECK_EQ(feed_n(&a, 2400u, 1), APP_LEVEL_NORMAL);   /* 第 2 次 */
    CHECK_EQ(feed_n(&a, 2400u, 1), APP_LEVEL_WARN);     /* 第 3 次才切 */
    CHECK_EQ(a.change_count, 1u);
  }

  /* ---------------- 计数器互相打断 ---------------- */
  SECTION("alarm: 抖动会清零计数");
  {
    app_alarm_init(&a, 2400u, 3000u);
    feed_n(&a, 2400u, 2);            /* up_cnt = 2 */
    feed_n(&a, 1000u, 1);            /* 回落，up_cnt 归零 */
    CHECK_EQ(feed_n(&a, 2400u, 2), APP_LEVEL_NORMAL);   /* 重新计数只到 2 */
    CHECK_EQ(a.level, APP_LEVEL_NORMAL);
    CHECK_EQ(feed_n(&a, 2400u, 1), APP_LEVEL_WARN);
  }

  /* ---------------- 相邻跳转：NORMAL 不会直接跳 CRIT ---------------- */
  SECTION("alarm: 只允许相邻级别跳转");
  {
    app_alarm_init(&a, 2400u, 3000u);
    CHECK_EQ(feed_n(&a, 3300u, 3), APP_LEVEL_WARN);     /* 先到 WARN，不直接 CRIT */
    CHECK_EQ(feed_n(&a, 3300u, 3), APP_LEVEL_CRIT);     /* 再 3 次才到 CRIT */
    CHECK_EQ(a.change_count, 2u);
  }

  /* ---------------- WARN 的滞回退出 ---------------- */
  SECTION("alarm: WARN -> NORMAL 必须低于 warn-hyst");
  {
    app_alarm_init(&a, 2400u, 3000u);
    feed_n(&a, 2400u, 3);
    CHECK_EQ(a.level, APP_LEVEL_WARN);

    /* 2350 恰好等于门槛：不算低于，必须保持 WARN */
    CHECK_EQ(feed_n(&a, 2350u, 10), APP_LEVEL_WARN);
    /* 2399 在滞回带内，同样保持 */
    CHECK_EQ(feed_n(&a, 2399u, 10), APP_LEVEL_WARN);
    /* 2349 低于门槛：连续 3 次后回 NORMAL */
    CHECK_EQ(feed_n(&a, 2349u, 2), APP_LEVEL_WARN);
    CHECK_EQ(feed_n(&a, 2349u, 1), APP_LEVEL_NORMAL);
    CHECK_EQ(a.change_count, 2u);
  }

  /* ---------------- CRIT 的滞回退出：只退回 WARN ---------------- */
  SECTION("alarm: CRIT -> WARN 必须低于 crit-hyst，且只能退一级");
  {
    app_alarm_init(&a, 2400u, 3000u);
    feed_n(&a, 2400u, 3);
    feed_n(&a, 3000u, 3);
    CHECK_EQ(a.level, APP_LEVEL_CRIT);

    CHECK_EQ(feed_n(&a, 2950u, 10), APP_LEVEL_CRIT);    /* 等于门槛，保持 */
    CHECK_EQ(feed_n(&a, 2949u, 2), APP_LEVEL_CRIT);
    CHECK_EQ(feed_n(&a, 2949u, 1), APP_LEVEL_WARN);     /* 先退到 WARN */
    CHECK_EQ(a.level, APP_LEVEL_WARN);
    /* 2949 远高于 2350，不会继续退到 NORMAL */
    CHECK_EQ(feed_n(&a, 2949u, 10), APP_LEVEL_WARN);
  }

  /* ---------------- 阈值校验 ---------------- */
  SECTION("alarm: set_thresholds 合法性");
  {
    app_alarm_init(&a, 2400u, 3000u);

    CHECK_EQ(app_alarm_set_thresholds(&a, 0u, 3000u), 0u);        /* warn 太小 */
    CHECK_EQ(app_alarm_set_thresholds(&a, 50u, 3000u), 0u);       /* warn == hyst */
    CHECK_EQ(app_alarm_set_thresholds(&a, 51u, 100u), 0u);        /* crit < warn+hyst (101) */
    CHECK_EQ(app_alarm_set_thresholds(&a, 51u, 101u), 1u);        /* 恰好等于，允许 */
    CHECK_EQ(app_alarm_set_thresholds(&a, 1000u, 3301u), 0u);     /* crit 超量程 */

    /* 回归：warn = 65500 时 warn + hyst 在 uint16 下会回绕成 14，
       旧实现在这里会误判为合法。改成 uint32 比较后必须拒绝 */
    CHECK_EQ(app_alarm_set_thresholds(&a, 65500u, 100u), 0u);

    /* 非法调用不能改变已生效的阈值 */
    CHECK_EQ(a.thr_warn_mv, 51u);
    CHECK_EQ(a.thr_crit_mv, 101u);

    /* 合法调用：立即生效，且清空去抖计数 */
    feed_n(&a, 2400u, 2);                    /* up_cnt = 2（相对新阈值 51 已经超了）*/
    CHECK_EQ(app_alarm_set_thresholds(&a, 1000u, 2000u), 1u);
    CHECK_EQ(a.thr_warn_mv, 1000u);
    CHECK_EQ(a.thr_crit_mv, 2000u);
    /* 计数被清零：再来 2 次高值不应切换（若没清零，这里就会切） */
    CHECK_EQ(feed_n(&a, 1500u, 2), APP_LEVEL_NORMAL);
    CHECK_EQ(feed_n(&a, 1500u, 1), APP_LEVEL_WARN);
  }

  /* ---------------- NULL 保护 ---------------- */
  SECTION("alarm: NULL 保护");
  {
    app_alarm_init((app_alarm_t *)0, 2400u, 3000u);              /* 不应崩溃 */
    CHECK_EQ(app_alarm_update((app_alarm_t *)0, 3000u), APP_LEVEL_NORMAL);
    CHECK_EQ(app_alarm_set_thresholds((app_alarm_t *)0, 2400u, 3000u), 0u);
  }

  /* ---------------- 名称映射 ---------------- */
  SECTION("alarm: 级别名称");
  {
    CHECK(strcmp(app_alarm_name(APP_LEVEL_NORMAL), "NORMAL") == 0);
    CHECK(strcmp(app_alarm_name(APP_LEVEL_WARN), "WARN") == 0);
    CHECK(strcmp(app_alarm_name(APP_LEVEL_CRIT), "CRIT") == 0);
    CHECK(strcmp(app_alarm_name((app_level_t)9), "UNKNOWN") == 0);
  }
}
