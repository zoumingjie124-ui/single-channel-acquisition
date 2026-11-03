/**
  ******************************************************************************
  * @file    test_fault.c
  * @brief   app_fault.c 的单元测试（编译真实固件源码，不重写逻辑）。
  *
  * 覆盖：FAULT 帧字节向量（与上位机 Python 测试逐字节对拍）、确认事件、
  *       恢复事件、重复抑制、多故障并存（恢复一个不清另一个）、未知故障号、
  *       build_payload 布局。
  *
  * 字节向量说明：下面三组 hex 是"契约向量"，host/tests/test_fault_joint.py
  *   里用 Python 独立算出同样的字节并断言相等。两端都写死同一串，
  *   谁单边改了协议，对拍立刻失败。
  ******************************************************************************
  */
#include <string.h>
#include "test_util.h"
#include "app_fault.h"
#include "app_proto.h"

/* ------------------------- 发帧捕获 ------------------------- */
static int     em_count;
static uint8_t em_len;
static uint8_t em_buf[PROTO_LEN_FAULT];

static void cap_emit(const uint8_t *payload, uint8_t len, void *user)
{
  (void)user;
  em_count++;
  em_len = len;
  memcpy(em_buf, payload, (len < PROTO_LEN_FAULT) ? len : PROTO_LEN_FAULT);
}

static void cap_reset(void)
{
  em_count = 0;
  em_len   = 0u;
  memset(em_buf, 0, sizeof em_buf);
}

/* 从捕获的载荷里取字段，验证内容而不只是"发了一帧" */
static uint8_t  em_event(void)    { return em_buf[4]; }
static uint8_t  em_id(void)       { return em_buf[5]; }
static uint8_t  em_active(void)   { return em_buf[6]; }
static uint16_t em_value(void)    { return (uint16_t)(em_buf[7] | (em_buf[8] << 8)); }

void test_fault(void)
{
  app_fault_t f;
  uint8_t frame[PROTO_MAX_FRAME];
  uint16_t n;
  uint8_t r;

  /* 契约向量（与 host/tests/test_fault_joint.py 完全一致） */
  static const uint8_t V_CONFIRM[16] = {
    0xAA,0x55,0x86,0x09,0x00, 0xB8,0x0B,0x00,0x00, 0x01,0x01,0x01, 0x0C,0x00, 0xF9,0x45 };
  static const uint8_t V_RECOVER[16] = {
    0xAA,0x55,0x86,0x09,0x01, 0x70,0x17,0x00,0x00, 0x02,0x01,0x00, 0x78,0x00, 0x54,0xD2 };
  static const uint8_t V_SETDIAG[8] = {
    0xAA,0x55,0x15,0x01,0x02, 0x01, 0x51,0x62 };

  /* ---------------- 载荷布局 ---------------- */
  SECTION("fault: build_payload 9 字节布局");
  {
    uint8_t p[PROTO_LEN_FAULT];
    app_fault_build_payload(p, 3000u, PROTO_FAULT_EV_CONFIRM,
                            APP_FAULT_ID_STUCK, 0x01u, 12u);
    CHECK_EQ(p[0], 0xB8); CHECK_EQ(p[1], 0x0B);   /* t_ms=3000 小端 */
    CHECK_EQ(p[2], 0x00); CHECK_EQ(p[3], 0x00);
    CHECK_EQ(p[4], 1u);                            /* event=CONFIRM */
    CHECK_EQ(p[5], 1u);                            /* id=STUCK */
    CHECK_EQ(p[6], 0x01u);                         /* active mask */
    CHECK_EQ(p[7], 0x0C); CHECK_EQ(p[8], 0x00);    /* value=12 小端 */
  }

  /* ---------------- FAULT 帧字节向量（CONFIRM） ---------------- */
  SECTION("fault: FAULT CONFIRM 帧逐字节匹配契约向量");
  {
    uint8_t p[PROTO_LEN_FAULT];
    app_fault_build_payload(p, 3000u, PROTO_FAULT_EV_CONFIRM,
                            APP_FAULT_ID_STUCK, 0x01u, 12u);
    n = proto_encode(PROTO_T_FAULT, 0x00u, p, PROTO_LEN_FAULT, frame, sizeof frame);
    CHECK_EQ(n, 16u);
    CHECK(memcmp(frame, V_CONFIRM, 16) == 0);
  }

  /* ---------------- FAULT 帧字节向量（RECOVER） ---------------- */
  SECTION("fault: FAULT RECOVER 帧逐字节匹配契约向量");
  {
    uint8_t p[PROTO_LEN_FAULT];
    app_fault_build_payload(p, 6000u, PROTO_FAULT_EV_RECOVER,
                            APP_FAULT_ID_STUCK, 0x00u, 120u);
    n = proto_encode(PROTO_T_FAULT, 0x01u, p, PROTO_LEN_FAULT, frame, sizeof frame);
    CHECK_EQ(n, 16u);
    CHECK(memcmp(frame, V_RECOVER, 16) == 0);
  }

  /* ---------------- SET_DIAG 帧字节向量 ---------------- */
  SECTION("fault: SET_DIAG(enable=1) 帧逐字节匹配契约向量");
  {
    uint8_t en = 1u;
    n = proto_encode(PROTO_T_SET_DIAG, 0x02u, &en, PROTO_LEN_SET_DIAG, frame, sizeof frame);
    CHECK_EQ(n, 8u);
    CHECK(memcmp(frame, V_SETDIAG, 8) == 0);
  }

  /* ---------------- 确认事件 ---------------- */
  SECTION("fault: 首次确认发一帧 CONFIRM");
  app_fault_init(&f);
  cap_reset();
  r = app_fault_update(&f, APP_FAULT_ID_STUCK, 1u, 3000u, 12u, cap_emit, 0);
  CHECK_EQ(r, 1u);
  CHECK_EQ(em_count, 1);
  CHECK_EQ(em_len, PROTO_LEN_FAULT);
  CHECK_EQ(em_event(), PROTO_FAULT_EV_CONFIRM);
  CHECK_EQ(em_id(), APP_FAULT_ID_STUCK);
  CHECK_EQ(em_active(), 0x01u);          /* 确认后掩码置位 */
  CHECK_EQ(em_value(), 12u);
  CHECK_EQ(app_fault_active_mask(&f), 0x01u);

  /* ---------------- 重复抑制 ---------------- */
  SECTION("fault: ACTIVE 期间反复调用不重复上报");
  {
    int i;
    cap_reset();
    for (i = 0; i < 500; i++)
    {
      /* 模拟每样本都喂 is_active=1：一帧都不该再发 */
      r = app_fault_update(&f, APP_FAULT_ID_STUCK, 1u, (uint32_t)(3000 + i * 10), 12u, cap_emit, 0);
      CHECK_EQ(r, 0u);
    }
    CHECK_EQ(em_count, 0);
    CHECK_EQ(app_fault_active_mask(&f), 0x01u);
  }

  /* ---------------- 恢复事件 ---------------- */
  SECTION("fault: 恢复发一帧 RECOVER 并清位");
  cap_reset();
  r = app_fault_update(&f, APP_FAULT_ID_STUCK, 0u, 6000u, 120u, cap_emit, 0);
  CHECK_EQ(r, 1u);
  CHECK_EQ(em_count, 1);
  CHECK_EQ(em_event(), PROTO_FAULT_EV_RECOVER);
  CHECK_EQ(em_active(), 0x00u);          /* 恢复后掩码清零 */
  CHECK_EQ(em_value(), 120u);
  CHECK_EQ(app_fault_active_mask(&f), 0x00u);

  /* ---------------- 无确认时的恢复 ---------------- */
  SECTION("fault: 未确认就恢复 -> 不发帧");
  cap_reset();
  r = app_fault_update(&f, APP_FAULT_ID_STUCK, 0u, 7000u, 200u, cap_emit, 0);
  CHECK_EQ(r, 0u);
  CHECK_EQ(em_count, 0);

  /* ---------------- 恢复后再次确认 ---------------- */
  SECTION("fault: 恢复后再次卡死 -> 再次 CONFIRM");
  cap_reset();
  r = app_fault_update(&f, APP_FAULT_ID_STUCK, 1u, 8000u, 8u, cap_emit, 0);
  CHECK_EQ(r, 1u);
  CHECK_EQ(em_event(), PROTO_FAULT_EV_CONFIRM);
  CHECK_EQ(app_fault_active_mask(&f), 0x01u);

  /* ---------------- 多故障并存：恢复一个不清另一个 ---------------- */
  SECTION("fault: 多故障并存，恢复 STUCK 不动其它位");
  {
    app_fault_init(&f);
    /* 预置一个"未来扩展故障"的位（0x02），模拟另一种故障已确认在册。
       直接写 active 是白盒：目的是验证 update() 的按位清除不会误伤别的位。 */
    f.active = 0x02u;

    cap_reset();
    r = app_fault_update(&f, APP_FAULT_ID_STUCK, 1u, 9000u, 5u, cap_emit, 0);
    CHECK_EQ(r, 1u);
    CHECK_EQ(em_active(), 0x03u);        /* STUCK 位叠加，另一位保留 */
    CHECK_EQ(app_fault_active_mask(&f), 0x03u);

    cap_reset();
    r = app_fault_update(&f, APP_FAULT_ID_STUCK, 0u, 10000u, 90u, cap_emit, 0);
    CHECK_EQ(r, 1u);
    CHECK_EQ(em_event(), PROTO_FAULT_EV_RECOVER);
    CHECK_EQ(em_active(), 0x02u);        /* 关键：只清 STUCK 位，0x02 还在 */
    CHECK_EQ(app_fault_active_mask(&f), 0x02u);
  }

  /* ---------------- 未知故障号 ---------------- */
  SECTION("fault: 未知 fault_id 不发帧、不改掩码");
  {
    app_fault_init(&f);
    f.active = 0x01u;
    CHECK_EQ(app_fault_mask_of(0x00u), 0u);
    CHECK_EQ(app_fault_mask_of(0x7Fu), 0u);
    cap_reset();
    r = app_fault_update(&f, 0x7Fu, 1u, 1u, 1u, cap_emit, 0);
    CHECK_EQ(r, 0u);
    CHECK_EQ(em_count, 0);
    CHECK_EQ(app_fault_active_mask(&f), 0x01u);   /* 原样不动 */
  }

  /* ---------------- emit 为 0 时只改状态 ---------------- */
  SECTION("fault: emit=NULL 只更新状态不发帧");
  {
    app_fault_init(&f);
    r = app_fault_update(&f, APP_FAULT_ID_STUCK, 1u, 1u, 1u, 0, 0);
    CHECK_EQ(r, 0u);
    CHECK_EQ(app_fault_active_mask(&f), 0x01u);   /* 状态仍推进 */
  }
}
