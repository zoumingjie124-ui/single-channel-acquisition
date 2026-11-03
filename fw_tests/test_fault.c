/**
  ******************************************************************************
  * @file    test_fault.c
  * @brief   app_fault.c 的单元测试（编译真实固件源码，不重写逻辑）。
  *
  * Stage 7C-2 改造后，本模块的模型是"状态差驱动"：
  *     set_state() 每样本更新 active；
  *     service()   只在 active != reported 时尝试提交，成功才把 reported 拉平。
  * 所以这里测的不是"事件发没发"，而是"设备状态与已提交状态什么时候对齐"。
  *
  * 契约向量三组仍与 host/tests/test_fault_joint.py、docs/PROTOCOL.md 逐字节对拍。
  ******************************************************************************
  */
#include <string.h>
#include "test_util.h"
#include "app_fault.h"
#include "app_proto.h"
#include "app_config.h"

/* ------------------------- 发帧捕获（可模拟队列满） ------------------------- */
static int     em_attempt;      /* service 调用 emit 的次数 */
static int     em_count;        /* 成功入队的次数 */
static uint8_t em_ok = 1u;      /* 1 = 队列能收（模拟 app_tx_write 成功） */
static uint8_t em_len;
static uint8_t em_buf[PROTO_LEN_FAULT];

static uint8_t cap_emit(const uint8_t *payload, uint8_t len, void *user)
{
  (void)user;
  em_attempt++;
  if (em_ok == 0u)
  {
    return 0u;          /* 模拟发送环满：app_tx_write 返回 0 */
  }
  em_count++;
  em_len = len;
  memcpy(em_buf, payload, (len < PROTO_LEN_FAULT) ? len : PROTO_LEN_FAULT);
  return 1u;
}

static void cap_reset(void)
{
  em_attempt = 0;
  em_count   = 0;
  em_len     = 0u;
  em_ok      = 1u;
  memset(em_buf, 0, sizeof em_buf);
}

static uint8_t  em_event(void)  { return em_buf[4]; }
static uint8_t  em_id(void)     { return em_buf[5]; }
static uint8_t  em_active(void) { return em_buf[6]; }
static uint16_t em_value(void)  { return (uint16_t)(em_buf[7] | (em_buf[8] << 8)); }

/* set_state + service 一步到位，模拟一个采样周期 */
static uint8_t step(app_fault_t *f, uint8_t is_active, uint32_t t, uint16_t val, uint8_t room)
{
  app_fault_set_state(f, APP_FAULT_ID_STUCK, is_active, val);
  return app_fault_service(f, t, room, cap_emit, 0);
}

void test_fault(void)
{
  app_fault_t f;
  uint8_t frame[PROTO_MAX_FRAME];
  uint16_t n;
  uint8_t r;
  uint8_t p[PROTO_LEN_FAULT];

  /* 契约向量（与 host/tests/test_fault_joint.py 完全一致） */
  static const uint8_t V_CONFIRM[16] = {
    0xAA,0x55,0x86,0x09,0x00, 0xB8,0x0B,0x00,0x00, 0x01,0x01,0x01, 0x0C,0x00, 0xF9,0x45 };
  static const uint8_t V_RECOVER[16] = {
    0xAA,0x55,0x86,0x09,0x01, 0x70,0x17,0x00,0x00, 0x02,0x01,0x00, 0x78,0x00, 0x54,0xD2 };
  static const uint8_t V_SETDIAG[8] = {
    0xAA,0x55,0x15,0x01,0x02, 0x01, 0x51,0x62 };

  /* ======================= 载荷布局与契约向量 ======================= */
  SECTION("fault: build_payload 9 字节布局");
  {
    app_fault_build_payload(p, 3000u, PROTO_FAULT_EV_CONFIRM, APP_FAULT_ID_STUCK, 0x01u, 12u);
    CHECK_EQ(p[0], 0xB8); CHECK_EQ(p[1], 0x0B);   /* t_ms=3000 小端 */
    CHECK_EQ(p[2], 0x00); CHECK_EQ(p[3], 0x00);
    CHECK_EQ(p[4], 1u);
    CHECK_EQ(p[5], 1u);
    CHECK_EQ(p[6], 0x01u);
    CHECK_EQ(p[7], 0x0C); CHECK_EQ(p[8], 0x00);
  }

  SECTION("fault: FAULT CONFIRM 帧逐字节匹配契约向量");
  {
    app_fault_build_payload(p, 3000u, PROTO_FAULT_EV_CONFIRM, APP_FAULT_ID_STUCK, 0x01u, 12u);
    n = proto_encode(PROTO_T_FAULT, 0x00u, p, PROTO_LEN_FAULT, frame, sizeof frame);
    CHECK_EQ(n, 16u);
    CHECK(memcmp(frame, V_CONFIRM, 16) == 0);
  }

  SECTION("fault: FAULT RECOVER 帧逐字节匹配契约向量");
  {
    app_fault_build_payload(p, 6000u, PROTO_FAULT_EV_RECOVER, APP_FAULT_ID_STUCK, 0x00u, 120u);
    n = proto_encode(PROTO_T_FAULT, 0x01u, p, PROTO_LEN_FAULT, frame, sizeof frame);
    CHECK_EQ(n, 16u);
    CHECK(memcmp(frame, V_RECOVER, 16) == 0);
  }

  SECTION("fault: SET_DIAG(enable=1) 帧逐字节匹配契约向量");
  {
    uint8_t en = 1u;
    n = proto_encode(PROTO_T_SET_DIAG, 0x02u, &en, PROTO_LEN_SET_DIAG, frame, sizeof frame);
    CHECK_EQ(n, 8u);
    CHECK(memcmp(frame, V_SETDIAG, 8) == 0);
  }

  /* ---------------- Stage 7C-2：快照与查询的契约向量 ---------------- */
  SECTION("fault: 快照帧逐字节匹配契约向量（有故障 / 无故障）");
  {
    static const uint8_t V_SNAP_F[16] = {
      0xAA,0x55,0x86,0x09,0x05, 0x40,0x1F,0x00,0x00, 0x00,0x01,0x01, 0x19,0x00, 0x11,0xDE };
    static const uint8_t V_SNAP_C[16] = {
      0xAA,0x55,0x86,0x09,0x06, 0x28,0x23,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00, 0x00,0x11 };
    app_fault_t g;

    app_fault_init(&g);
    g.active = PROTO_FAULT_MASK_STUCK;    /* 白盒：造出"有故障"的当前状态 */
    g.value  = 25u;
    (void)app_fault_build_snapshot(&g, p, 8000u);
    n = proto_encode(PROTO_T_FAULT, 0x05u, p, PROTO_LEN_FAULT, frame, sizeof frame);
    CHECK_EQ(n, 16u);
    CHECK(memcmp(frame, V_SNAP_F, 16) == 0);

    app_fault_init(&g);                   /* 无故障 */
    (void)app_fault_build_snapshot(&g, p, 9000u);
    n = proto_encode(PROTO_T_FAULT, 0x06u, p, PROTO_LEN_FAULT, frame, sizeof frame);
    CHECK_EQ(n, 16u);
    CHECK(memcmp(frame, V_SNAP_C, 16) == 0);
  }

  SECTION("fault: GET_DIAG 请求帧逐字节匹配契约向量");
  {
    static const uint8_t V_GETDIAG[7] = { 0xAA,0x55,0x16,0x00,0x07, 0xB8,0x4D };
    n = proto_encode(PROTO_T_GET_DIAG, 0x07u, (const uint8_t *)0, PROTO_LEN_GET_DIAG,
                     frame, sizeof frame);
    CHECK_EQ(n, 7u);
    CHECK(memcmp(frame, V_GETDIAG, 7) == 0);
  }

  /* ======================= ① 状态变化 + 队列有空间 ======================= */
  SECTION("fault: 1) active 变化且提交成功 -> reported 跟随");
  app_fault_init(&f);
  cap_reset();
  r = step(&f, 1u, 3000u, 12u, 1u);
  CHECK_EQ(r, 1u);
  CHECK_EQ(em_count, 1);
  CHECK_EQ(em_len, PROTO_LEN_FAULT);
  CHECK_EQ(em_event(), PROTO_FAULT_EV_CONFIRM);
  CHECK_EQ(em_id(), APP_FAULT_ID_STUCK);
  CHECK_EQ(em_active(), 0x01u);
  CHECK_EQ(em_value(), 12u);
  CHECK_EQ(app_fault_active_mask(&f), 0x01u);
  CHECK_EQ(app_fault_reported_mask(&f), 0x01u);   /* 提交成功才拉平 */
  CHECK_EQ(app_fault_commit_count(&f), 1u);

  /* ======================= 重复抑制 ======================= */
  SECTION("fault: 状态不变时反复调用不再发帧");
  {
    int i;
    cap_reset();
    for (i = 0; i < 500; i++)
    {
      r = step(&f, 1u, (uint32_t)(3010u + (uint32_t)i * 10u), 12u, 1u);
      CHECK_EQ(r, 0u);
    }
    CHECK_EQ(em_attempt, 0);      /* 连 emit 都没被调 */
    CHECK_EQ(em_count, 0);
    CHECK_EQ(app_fault_commit_count(&f), 1u);
  }

  /* ======================= ② 队列满 ======================= */
  SECTION("fault: 2) 提交失败 -> reported 不更新");
  cap_reset();
  em_ok = 0u;                          /* 模拟发送环满 */
  r = step(&f, 0u, 6000u, 120u, 1u);   /* 想恢复 */
  CHECK_EQ(r, 0u);
  CHECK_EQ(em_attempt, 1);             /* 试过一次 */
  CHECK_EQ(em_count, 0);               /* 但没成功 */
  CHECK_EQ(app_fault_active_mask(&f), 0x00u);    /* 真实状态已变 */
  CHECK_EQ(app_fault_reported_mask(&f), 0x01u);  /* 已提交状态还停在旧值 */
  CHECK_EQ(app_fault_retry_fail_count(&f), 1u);

  /* ======================= ③ 队列满不消耗序号 ======================= */
  SECTION("fault: 3) tx_has_room=0 时连 emit 都不调（不烧帧序号）");
  {
    int i;
    cap_reset();
    for (i = 0; i < 50; i++)
    {
      /* 队列明确没空间：service 应在构造帧、取序号之前就返回 */
      r = app_fault_service(&f, (uint32_t)(7000u + (uint32_t)i * 10u), 0u, cap_emit, 0);
      CHECK_EQ(r, 0u);
    }
    CHECK_EQ(em_attempt, 0);      /* 关键：一次都没进 emit，也就没有序号被消耗 */
    CHECK_EQ(app_fault_reported_mask(&f), 0x01u);
  }

  /* ======================= ④ 队列恢复后提交最新状态 ======================= */
  SECTION("fault: 4) 队列恢复后提交的是当前状态，不是中间边沿");
  cap_reset();
  r = app_fault_service(&f, 8000u, 1u, cap_emit, 0);
  CHECK_EQ(r, 1u);
  CHECK_EQ(em_count, 1);
  CHECK_EQ(em_event(), PROTO_FAULT_EV_RECOVER);  /* 当前是"无故障" */
  CHECK_EQ(em_active(), 0x00u);
  CHECK_EQ(app_fault_reported_mask(&f), 0x00u);
  CHECK_EQ(app_fault_active_mask(&f), 0x00u);

  /* ======================= ⑤ 100 ms 节流边界 ======================= */
  SECTION("fault: 5) 重试节流：差 1 ms 不试，正好 100 ms 放行");
  {
    app_fault_init(&f);
    cap_reset();
    em_ok = 0u;                            /* 先让它失败一次，留下 last_try */
    r = step(&f, 1u, 1000u, 5u, 1u);
    CHECK_EQ(r, 0u);
    CHECK_EQ(em_attempt, 1);

    em_ok = 1u;
    r = app_fault_service(&f, 1000u + APP_FAULT_RETRY_MIN_MS - 1u, 1u, cap_emit, 0);
    CHECK_EQ(r, 0u);
    CHECK_EQ(em_attempt, 1);               /* 99 ms：还在节流窗口内，不再尝试 */

    r = app_fault_service(&f, 1000u + APP_FAULT_RETRY_MIN_MS, 1u, cap_emit, 0);
    CHECK_EQ(r, 1u);
    CHECK_EQ(em_attempt, 2);               /* 正好 100 ms：放行 */
    CHECK_EQ(app_fault_reported_mask(&f), 0x01u);
  }

  /* ======================= ⑥ 快速三连：允许边沿合并 ======================= */
  SECTION("fault: 6) 确认-恢复-再确认，全程队列满 -> 只补发当前状态一帧");
  {
    app_fault_init(&f);
    cap_reset();
    em_ok = 0u;

    /* 队列满期间的三个边沿：1 -> 0 -> 1 */
    (void)step(&f, 1u, 1000u, 8u, 1u);      /* 确认：失败 */
    (void)step(&f, 0u, 1100u, 90u, 1u);     /* 恢复：失败 */
    (void)step(&f, 1u, 1200u, 7u, 1u);      /* 再确认：失败 */

    CHECK_EQ(app_fault_active_mask(&f), 0x01u);
    CHECK_EQ(app_fault_reported_mask(&f), 0x00u);   /* 一帧都没提交成功 */
    CHECK_EQ(em_count, 0);

    /* 队列恢复：只发一帧，内容必须是当前状态，不能是历史边沿 */
    em_ok = 1u;
    r = app_fault_service(&f, 1300u, 1u, cap_emit, 0);
    CHECK_EQ(r, 1u);
    CHECK_EQ(em_count, 1);
    CHECK_EQ(em_event(), PROTO_FAULT_EV_CONFIRM);
    CHECK_EQ(em_active(), 0x01u);
    CHECK_EQ(app_fault_reported_mask(&f), 0x01u);

    /* 再 service 一次：已经对齐，不应重复 */
    r = app_fault_service(&f, 1500u, 1u, cap_emit, 0);
    CHECK_EQ(r, 0u);
    CHECK_EQ(em_count, 1);
  }

  /* ======================= 恢复事件（正常路径） ======================= */
  SECTION("fault: 正常路径下恢复发一帧 RECOVER");
  {
    app_fault_init(&f);
    cap_reset();
    (void)step(&f, 1u, 1000u, 10u, 1u);     /* 确认 */
    cap_reset();
    r = step(&f, 0u, 2000u, 200u, 1u);
    CHECK_EQ(r, 1u);
    CHECK_EQ(em_event(), PROTO_FAULT_EV_RECOVER);
    CHECK_EQ(em_active(), 0x00u);
    CHECK_EQ(app_fault_reported_mask(&f), 0x00u);
  }

  /* ======================= ⑦ 快照不改 reported ======================= */
  SECTION("fault: 7) build_snapshot 只读，不碰 active / reported");
  {
    app_fault_init(&f);
    cap_reset();                            /* 先清账，否则会把上一节的发送算进来 */
    (void)step(&f, 1u, 1000u, 33u, 1u);     /* active=1, reported=1 */
    {
      uint8_t  before_a = app_fault_active_mask(&f);
      uint8_t  before_r = app_fault_reported_mask(&f);
      uint32_t before_c = app_fault_commit_count(&f);
      int i;
      for (i = 0; i < 100; i++)             /* 反复查 100 次 */
      {
        (void)app_fault_build_snapshot(&f, p, 5000u);
      }
      CHECK_EQ(app_fault_active_mask(&f), before_a);
      CHECK_EQ(app_fault_reported_mask(&f), before_r);
      CHECK_EQ(app_fault_commit_count(&f), before_c);
    }
    CHECK_EQ(em_count, 1);                  /* 快照不产生任何发送 */
  }

  /* ======================= ⑧ 无故障快照 ======================= */
  SECTION("fault: 8) 无故障快照 fault_id=0 / active=0 / value=0");
  {
    app_fault_init(&f);
    r = app_fault_build_snapshot(&f, p, 7777u);
    CHECK_EQ(r, 0u);                        /* 返回写入的 fault_id */
    CHECK_EQ(p[0], 0x61); CHECK_EQ(p[1], 0x1E);   /* 7777 = 0x1E61 小端 */
    CHECK_EQ(p[4], PROTO_FAULT_EV_SNAPSHOT);      /* event=0 */
    CHECK_EQ(p[5], 0u);
    CHECK_EQ(p[6], 0u);
    CHECK_EQ(p[7], 0u); CHECK_EQ(p[8], 0u);
  }

  /* ======================= ⑨ 有故障快照 ======================= */
  SECTION("fault: 9) 有故障快照 fault_id=1 / active=1 / value=span");
  {
    app_fault_init(&f);
    (void)step(&f, 1u, 1000u, 42u, 1u);
    cap_reset();
    r = app_fault_build_snapshot(&f, p, 2000u);
    CHECK_EQ(r, 1u);
    CHECK_EQ(p[4], PROTO_FAULT_EV_SNAPSHOT);
    CHECK_EQ(p[5], APP_FAULT_ID_STUCK);
    CHECK_EQ(p[6], 0x01u);
    CHECK_EQ((uint16_t)(p[7] | (p[8] << 8)), 42u);
    CHECK_EQ(em_count, 0);                  /* 快照本身不发帧，由调用方决定 */
  }

  /* ======================= 多故障位掩码（白盒） ======================= */
  SECTION("fault: 多故障并存时快照只给掩码，不指定单一故障");
  {
    app_fault_init(&f);
    f.active = 0x03u;                       /* 白盒：预置两位（协议层未登记第二种） */
    f.value  = 55u;
    r = app_fault_build_snapshot(&f, p, 1000u);
    CHECK_EQ(r, 0u);                        /* 不指定单一故障 */
    CHECK_EQ(p[6], 0x03u);                  /* 掩码完整 */
    CHECK_EQ((uint16_t)(p[7] | (p[8] << 8)), 0u);

    /* 提交时一次只处理一个位，恢复一个不清另一个 */
    cap_reset();
    r = app_fault_service(&f, 2000u, 1u, cap_emit, 0);
    CHECK_EQ(r, 1u);
    CHECK_EQ(em_active(), 0x03u);
    CHECK_EQ(app_fault_reported_mask(&f), 0x01u);   /* 只拉平了最低位 */
    r = app_fault_service(&f, 2100u, 1u, cap_emit, 0);
    CHECK_EQ(r, 1u);
    CHECK_EQ(app_fault_reported_mask(&f), 0x03u);   /* 第二位也提交了 */
  }

  /* ======================= ⑩ 诊断关闭但状态保留 ======================= */
  SECTION("fault: 10) 不再登记新状态时，快照仍反映保留的 active");
  {
    app_fault_init(&f);
    (void)step(&f, 1u, 1000u, 21u, 1u);     /* 已确认 */
    /* 模拟"诊断被关闭"：后面不再 set_state，active 停在这里 */
    r = app_fault_build_snapshot(&f, p, 9000u);
    CHECK_EQ(r, 1u);
    CHECK_EQ(p[6], 0x01u);
    CHECK_EQ((uint16_t)(p[7] | (p[8] << 8)), 21u);
    CHECK_EQ(app_fault_active_mask(&f), 0x01u);
  }

  /* ======================= 未知故障号 ======================= */
  SECTION("fault: 未知 fault_id 不登记、不发帧");
  {
    app_fault_init(&f);
    /* 白盒预置"已确认且已提交"的状态；否则 active=1/reported=0 本身就该补发一帧，
       会把这条用例的意图搅乱 */
    f.active   = 0x01u;
    f.reported = 0x01u;
    CHECK_EQ(app_fault_mask_of(0x00u), 0u);
    CHECK_EQ(app_fault_mask_of(0x7Fu), 0u);
    cap_reset();
    app_fault_set_state(&f, 0x7Fu, 1u, 1u);   /* 未知编号：应被忽略 */
    r = app_fault_service(&f, 1000u, 1u, cap_emit, 0);
    CHECK_EQ(r, 0u);
    CHECK_EQ(em_count, 0);
    CHECK_EQ(app_fault_active_mask(&f), 0x01u);   /* 原样不动 */
  }

  /* ======================= emit=NULL ======================= */
  SECTION("fault: emit=NULL 时不发帧，但状态照旧欠着");
  {
    app_fault_init(&f);
    app_fault_set_state(&f, APP_FAULT_ID_STUCK, 1u, 9u);
    r = app_fault_service(&f, 1000u, 1u, 0, 0);
    CHECK_EQ(r, 0u);
    CHECK_EQ(app_fault_active_mask(&f), 0x01u);
    CHECK_EQ(app_fault_reported_mask(&f), 0x00u);  /* 没提交，欠着 */
  }

  /* ======================= NULL 安全 ======================= */
  SECTION("fault: NULL 安全");
  {
    app_fault_init(0);
    app_fault_set_state(0, APP_FAULT_ID_STUCK, 1u, 1u);
    CHECK_EQ(app_fault_service(0, 1u, 1u, cap_emit, 0), 0u);
    CHECK_EQ(app_fault_build_snapshot(0, p, 1u), 0u);
    CHECK_EQ(app_fault_active_mask(0), 0u);
    CHECK_EQ(app_fault_reported_mask(0), 0u);
    CHECK_EQ(app_fault_is_active(0, APP_FAULT_ID_STUCK), 0u);
    CHECK_EQ(app_fault_retry_fail_count(0), 0u);
    CHECK_EQ(app_fault_commit_count(0), 0u);
    app_fault_build_payload(0, 1u, 1u, 1u, 1u, 1u);   /* 不崩即可 */
  }
}
