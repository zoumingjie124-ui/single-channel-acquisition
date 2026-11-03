/**
  ******************************************************************************
  * @file    test_cmd.c
  * @brief   app_cmd.c 命令分发器的单元测试（编译真实固件源码 + 板级桩）。
  *
  * 为什么这个测试有价值：app_cmd.c 是 SET_DIAG 落地的地方，"非法参数必须被拒、
  * 且不得触碰采样/通信"这条要求只有在真实分发器上跑才算数。
  * 它依赖 HAL 的两个符号（app_tx_write / app_task_next_frame_seq）由 stub_board.c 补齐。
  *
  * 覆盖：SET_DIAG 合法(0/1)、非法取值(2)、非法长度(0/2)、回调拒绝、
  *       未知命令、坏 CRC；并顺带回归 SET_LED/GET_STAT 分发未受影响。
  ******************************************************************************
  */
#include <string.h>
#include "test_util.h"
#include "stub_board.h"
#include "app_cmd.h"
#include "app_proto.h"

/* ---------------------- 捕获型命令回调 ---------------------- */
static int     diag_calls;
static uint8_t diag_last_enable;
static uint8_t diag_ret;      /* 回调返回值：1=接受 0=拒绝，用于测 PARAM 分支 */
static int     led_calls;
static int     stat_calls;

static uint8_t cb_set_thr(uint16_t w, uint16_t c) { (void)w; (void)c; return 1u; }
static void    cb_set_led(uint8_t on)             { (void)on; led_calls++; }
static void    cb_get_stat(void)                  { stat_calls++; }
static void    cb_reset_stats(void)               {}
static void    cb_ping(void)                      {}
static void    cb_text(const char *line)          { (void)line; }

static uint8_t cb_set_diag(uint8_t enable)
{
  diag_calls++;
  diag_last_enable = enable;
  return diag_ret;
}

/* Stage 7C-2：GET_DIAG 回调。真实实现只置"待发快照"标志，这里只记调用次数 */
static int     getdiag_calls;
static uint8_t getdiag_ret;

static uint8_t cb_get_diag(void)
{
  getdiag_calls++;
  return getdiag_ret;
}

static const app_cmd_ops_t s_ops =
{
  cb_set_thr, cb_set_led, cb_get_stat, cb_reset_stats, cb_ping,
  cb_set_diag, cb_get_diag, cb_text
};

static void cb_reset(void)
{
  diag_calls = 0;
  diag_last_enable = 0xFFu;
  diag_ret = 1u;
  getdiag_calls = 0;
  getdiag_ret = 1u;
  led_calls = 0;
  stat_calls = 0;
}

/* 组一帧命令喂进分发器，再 poll 一次 */
static void feed_cmd(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len, uint32_t now)
{
  uint8_t frame[PROTO_MAX_FRAME];
  uint16_t n = proto_encode(type, seq, payload, len, frame, sizeof frame);
  tx_cap_reset();
  app_cmd_rx_isr(frame, n);
  app_cmd_poll(now);
}

/* 断言捕获到的 ACK：req_type 与 result */
static void expect_ack(uint8_t req_type, uint8_t result)
{
  CHECK_EQ(tx_cap_at(2), PROTO_T_ACK);
  CHECK_EQ(tx_cap_at(3), PROTO_LEN_ACK);
  CHECK_EQ(tx_cap_at(5), req_type);
  CHECK_EQ(tx_cap_at(6), result);
}

void test_cmd(void)
{
  uint32_t now = 1000u;

  app_cmd_init(&s_ops);

  /* ---------------- SET_DIAG enable=1 ---------------- */
  SECTION("cmd: SET_DIAG(1) 合法 -> 回调收到 1，ACK OK");
  {
    uint8_t en = 1u;
    cb_reset();
    feed_cmd(PROTO_T_SET_DIAG, 0x10u, &en, 1u, now); now += 10u;
    CHECK_EQ(diag_calls, 1);
    CHECK_EQ(diag_last_enable, 1u);
    expect_ack(PROTO_T_SET_DIAG, 0u);   /* CMD_RES_OK */
  }

  /* ---------------- SET_DIAG enable=0 ---------------- */
  SECTION("cmd: SET_DIAG(0) 合法 -> 回调收到 0，ACK OK");
  {
    uint8_t en = 0u;
    cb_reset();
    feed_cmd(PROTO_T_SET_DIAG, 0x11u, &en, 1u, now); now += 10u;
    CHECK_EQ(diag_calls, 1);
    CHECK_EQ(diag_last_enable, 0u);
    expect_ack(PROTO_T_SET_DIAG, 0u);
  }

  /* ---------------- SET_DIAG enable=2 非法取值 ---------------- */
  SECTION("cmd: SET_DIAG(2) 非法取值 -> 回调不触发，ACK PARAM");
  {
    uint8_t en = 2u;
    cb_reset();
    feed_cmd(PROTO_T_SET_DIAG, 0x12u, &en, 1u, now); now += 10u;
    CHECK_EQ(diag_calls, 0);            /* 关键：非法值绝不能进回调 */
    expect_ack(PROTO_T_SET_DIAG, 1u);   /* CMD_RES_PARAM */
  }

  /* ---------------- SET_DIAG enable=255 ---------------- */
  SECTION("cmd: SET_DIAG(255) 非法取值 -> ACK PARAM");
  {
    uint8_t en = 255u;
    cb_reset();
    feed_cmd(PROTO_T_SET_DIAG, 0x13u, &en, 1u, now); now += 10u;
    CHECK_EQ(diag_calls, 0);
    expect_ack(PROTO_T_SET_DIAG, 1u);
  }

  /* ---------------- SET_DIAG 长度 0 ---------------- */
  SECTION("cmd: SET_DIAG len=0 -> ACK PARAM，回调不触发");
  {
    cb_reset();
    feed_cmd(PROTO_T_SET_DIAG, 0x14u, (const uint8_t *)0, 0u, now); now += 10u;
    CHECK_EQ(diag_calls, 0);
    expect_ack(PROTO_T_SET_DIAG, 1u);
  }

  /* ---------------- SET_DIAG 长度 2 ---------------- */
  SECTION("cmd: SET_DIAG len=2 -> ACK PARAM，回调不触发");
  {
    uint8_t two[2] = {1u, 0u};
    cb_reset();
    feed_cmd(PROTO_T_SET_DIAG, 0x15u, two, 2u, now); now += 10u;
    CHECK_EQ(diag_calls, 0);
    expect_ack(PROTO_T_SET_DIAG, 1u);
  }

  /* ---------------- 回调拒绝 -> PARAM ---------------- */
  SECTION("cmd: set_diag 回调返回 0 -> ACK PARAM");
  {
    uint8_t en = 1u;
    cb_reset();
    diag_ret = 0u;                       /* 让回调拒绝 */
    feed_cmd(PROTO_T_SET_DIAG, 0x16u, &en, 1u, now); now += 10u;
    CHECK_EQ(diag_calls, 1);
    expect_ack(PROTO_T_SET_DIAG, 1u);
  }

  /* ---------------- 未知命令 ---------------- */
  SECTION("cmd: 未知命令类型 -> ACK UNKNOWN");
  {
    cb_reset();
    feed_cmd(0x7Eu, 0x17u, (const uint8_t *)0, 0u, now); now += 10u;
    expect_ack(0x7Eu, 2u);              /* CMD_RES_UNKNOWN */
  }

  /* ---------------- 坏 CRC ---------------- */
  SECTION("cmd: 坏 CRC -> frame_err++，不发 ACK，回调不触发");
  {
    uint8_t frame[PROTO_MAX_FRAME];
    uint8_t en = 1u;
    uint32_t before;
    uint16_t n = proto_encode(PROTO_T_SET_DIAG, 0x18u, &en, 1u, frame, sizeof frame);
    frame[n - 1u] ^= 0xFFu;             /* 破坏 CRC 高字节 */
    cb_reset();
    tx_cap_reset();
    before = app_cmd_frame_err_count();
    app_cmd_rx_isr(frame, n);
    app_cmd_poll(now); now += 10u;
    CHECK_EQ(app_cmd_frame_err_count(), before + 1u);
    CHECK_EQ(diag_calls, 0);
    CHECK_EQ(tx_cap_len, 0u);           /* 没有任何 ACK 发出 */
  }

  /* ---------------- 回归：其它命令未受影响 ---------------- */
  SECTION("cmd: 回归 SET_LED / GET_STAT 分发正常");
  {
    uint8_t on = 1u;
    cb_reset();
    feed_cmd(PROTO_T_SET_LED, 0x19u, &on, 1u, now); now += 10u;
    CHECK_EQ(led_calls, 1);
    expect_ack(PROTO_T_SET_LED, 0u);

    cb_reset();
    feed_cmd(PROTO_T_GET_STAT, 0x1Au, (const uint8_t *)0, 0u, now); now += 10u;
    CHECK_EQ(stat_calls, 1);
    expect_ack(PROTO_T_GET_STAT, 0u);
  }

  /* ================= Stage 7C-2：GET_DIAG ================= */

  SECTION("cmd: GET_DIAG len=0 合法 -> 回调触发，ACK OK");
  {
    cb_reset();
    feed_cmd(PROTO_T_GET_DIAG, 0x20u, (const uint8_t *)0, 0u, now); now += 10u;
    CHECK_EQ(getdiag_calls, 1);
    expect_ack(PROTO_T_GET_DIAG, 0u);
    /* 关键：app_cmd 自己绝不发 FAULT 帧——快照由 app_task 在主循环发，
       所以 ACK 一定排在快照前面。这里捕获到的必须只有一帧 ACK。 */
    CHECK_EQ(tx_cap_len, (uint16_t)(PROTO_HEADER_LEN + PROTO_LEN_ACK + PROTO_CRC_LEN));
    CHECK_EQ(tx_cap_at(2), PROTO_T_ACK);
  }

  SECTION("cmd: GET_DIAG len=1 非法 -> 回调不触发，ACK PARAM");
  {
    uint8_t one = 0u;
    cb_reset();
    feed_cmd(PROTO_T_GET_DIAG, 0x21u, &one, 1u, now); now += 10u;
    CHECK_EQ(getdiag_calls, 0);
    expect_ack(PROTO_T_GET_DIAG, 1u);
  }

  SECTION("cmd: GET_DIAG len=2 非法 -> 回调不触发，ACK PARAM");
  {
    uint8_t two[2] = {0u, 0u};
    cb_reset();
    feed_cmd(PROTO_T_GET_DIAG, 0x22u, two, 2u, now); now += 10u;
    CHECK_EQ(getdiag_calls, 0);
    expect_ack(PROTO_T_GET_DIAG, 1u);
  }

  SECTION("cmd: GET_DIAG 回调拒绝 -> ACK PARAM");
  {
    cb_reset();
    getdiag_ret = 0u;
    feed_cmd(PROTO_T_GET_DIAG, 0x23u, (const uint8_t *)0, 0u, now); now += 10u;
    CHECK_EQ(getdiag_calls, 1);
    expect_ack(PROTO_T_GET_DIAG, 1u);
  }

  SECTION("cmd: 连续 3 次 GET_DIAG -> 3 个 ACK，各自独立应答");
  {
    int i;
    cb_reset();
    for (i = 0; i < 3; i++)
    {
      feed_cmd(PROTO_T_GET_DIAG, (uint8_t)(0x24u + i), (const uint8_t *)0, 0u, now);
      now += 10u;
      expect_ack(PROTO_T_GET_DIAG, 0u);
    }
    CHECK_EQ(getdiag_calls, 3);
  }
}
