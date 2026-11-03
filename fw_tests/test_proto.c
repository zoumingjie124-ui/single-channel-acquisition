/**
  ******************************************************************************
  * @file    test_proto.c
  * @brief   app_proto.c 的单元测试（编译真实固件源码，不重写逻辑）
  *
  * 覆盖：CRC 已知向量、组帧边界、解析状态机、非法输入、重同步、逐字节喂入。
  ******************************************************************************
  */
#include "test_util.h"
#include "app_proto.h"

/* ------------------------- 采集回调 ------------------------- */
static int     cb_count;
static uint8_t cb_type;
static uint8_t cb_seq;
static uint8_t cb_len;
static uint8_t cb_buf[PROTO_MAX_PAYLOAD];

static void on_frame(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len)
{
  uint8_t i;
  cb_count++;
  cb_type = type;
  cb_seq  = seq;
  cb_len  = len;
  for (i = 0u; (i < len) && (i < PROTO_MAX_PAYLOAD); i++)
  {
    cb_buf[i] = payload[i];
  }
}

static void cb_reset(void)
{
  cb_count = 0;
  cb_type = 0u;
  cb_seq = 0u;
  cb_len = 0u;
}

/* 逐字节喂入，返回最后一次的解析结果 */
static uint8_t feed(proto_parser_t *ps, const uint8_t *data, int len)
{
  uint8_t r = PROTO_R_NONE;
  int i;
  for (i = 0; i < len; i++)
  {
    r = proto_parser_feed(ps, data[i], on_frame);
  }
  return r;
}

void test_proto(void)
{
  proto_parser_t ps;
  uint8_t buf[PROTO_MAX_FRAME + 8];
  uint8_t payload[PROTO_MAX_PAYLOAD];
  uint16_t total;
  uint8_t i;
  uint8_t r;

  /* ---------------- CRC 已知向量 ---------------- */
  SECTION("proto: CRC16 已知向量");
  {
    static const uint8_t vec[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    /* CRC-16/CCITT-FALSE：init=0xFFFF, poly=0x1021, refin=false, refout=false
       标准校验值 0x29B1。这条能钉住实现的算法，不依赖任何本地数据 */
    CHECK_EQ(proto_crc16(vec, 9u), 0x29B1);
    CHECK_EQ(proto_crc16(vec, 0u), 0xFFFF);      /* 空数据 = 初值 */
    /* 单字节 '1'：手算 0xFFFF^0x3100=0xCEFF，逐位 8 次移位后为 0xC782。
       第一版这里写成 0xF4C3 是凭印象填的，实测不符——以手算与 9 字节标准向量为准 */
    CHECK_EQ(proto_crc16(vec, 1u), 0xC782);
  }

  /* ---------------- 组帧：正常与边界 ---------------- */
  SECTION("proto: 组帧长度与边界");
  {
    for (i = 0u; i < PROTO_MAX_PAYLOAD; i++) { payload[i] = i; }

    total = proto_encode(PROTO_T_SAMPLE, 0x21u, payload, PROTO_LEN_SAMPLE, buf, sizeof buf);
    CHECK_EQ(total, PROTO_HEADER_LEN + PROTO_LEN_SAMPLE + PROTO_CRC_LEN);   /* 17 */
    CHECK_EQ(buf[0], PROTO_SOF0);
    CHECK_EQ(buf[1], PROTO_SOF1);
    CHECK_EQ(buf[2], PROTO_T_SAMPLE);
    CHECK_EQ(buf[3], PROTO_LEN_SAMPLE);
    CHECK_EQ(buf[4], 0x21u);

    /* 最大载荷 40 字节应当允许 */
    total = proto_encode(PROTO_T_STAT, 0x00u, payload, PROTO_MAX_PAYLOAD, buf, sizeof buf);
    CHECK_EQ(total, PROTO_MAX_FRAME);

    /* 超过 40 字节必须拒绝 */
    CHECK_EQ(proto_encode(PROTO_T_STAT, 0x00u, payload, PROTO_MAX_PAYLOAD + 1u, buf, sizeof buf), 0u);

    /* 容量不足 1 字节也必须拒绝（不是"截断"） */
    CHECK_EQ(proto_encode(PROTO_T_PING, 0x00u, (const uint8_t *)0, 0u, buf, PROTO_HEADER_LEN + PROTO_CRC_LEN - 1u), 0u);

    /* 空载荷 + 空指针：合法 */
    CHECK_EQ(proto_encode(PROTO_T_PING, 0x07u, (const uint8_t *)0, 0u, buf, sizeof buf), PROTO_HEADER_LEN + PROTO_CRC_LEN);

    /* 有载荷但指针为空：拒绝 */
    CHECK_EQ(proto_encode(PROTO_T_SET_LED, 0x00u, (const uint8_t *)0, 1u, buf, sizeof buf), 0u);

    /* 输出缓冲为空：拒绝 */
    CHECK_EQ(proto_encode(PROTO_T_PING, 0x00u, (const uint8_t *)0, 0u, (uint8_t *)0, 64u), 0u);
  }

  /* ---------------- 往返：编码 -> 解析 ---------------- */
  SECTION("proto: 编码后解析回来");
  {
    proto_parser_init(&ps);
    cb_reset();
    for (i = 0u; i < PROTO_LEN_SAMPLE; i++) { payload[i] = (uint8_t)(0xA0u + i); }
    total = proto_encode(PROTO_T_SAMPLE, 0x5Au, payload, PROTO_LEN_SAMPLE, buf, sizeof buf);
    r = feed(&ps, buf, (int)total);

    CHECK_EQ(r, PROTO_R_FRAME);
    CHECK_EQ(cb_count, 1);
    CHECK_EQ(cb_type, PROTO_T_SAMPLE);
    CHECK_EQ(cb_seq, 0x5Au);
    CHECK_EQ(cb_len, PROTO_LEN_SAMPLE);
    CHECK_EQ(cb_buf[0], 0xA0u);
    CHECK_EQ(cb_buf[PROTO_LEN_SAMPLE - 1u], 0xA0u + PROTO_LEN_SAMPLE - 1u);
    CHECK_EQ(proto_parser_is_idle(&ps), 1u);
  }

  /* ---------------- CRC 错误 ---------------- */
  SECTION("proto: CRC 错误");
  {
    proto_parser_init(&ps);
    cb_reset();
    total = proto_encode(PROTO_T_PING, 0x11u, (const uint8_t *)0, 0u, buf, sizeof buf);
    buf[total - 1u] ^= 0xFFu;                    /* 破坏 CRC 高字节 */
    r = feed(&ps, buf, (int)total);
    CHECK_EQ(r, PROTO_R_ERROR);
    CHECK_EQ(cb_count, 0);                       /* 绝不能回调 */
    CHECK_EQ(proto_parser_is_idle(&ps), 1u);

    /* 坏帧之后紧接一个好帧，必须能恢复 */
    cb_reset();
    total = proto_encode(PROTO_T_PING, 0x12u, (const uint8_t *)0, 0u, buf, sizeof buf);
    r = feed(&ps, buf, (int)total);
    CHECK_EQ(r, PROTO_R_FRAME);
    CHECK_EQ(cb_count, 1);
    CHECK_EQ(cb_seq, 0x12u);
  }

  /* ---------------- 长度非法 ---------------- */
  SECTION("proto: LEN 越界");
  {
    static const uint8_t len41[5]  = {0xAA, 0x55, 0x81, 41, 0x00};
    static const uint8_t len255[5] = {0xAA, 0x55, 0x81, 255, 0x00};
    static const uint8_t len40[5]  = {0xAA, 0x55, 0x81, 40, 0x00};

    proto_parser_init(&ps);
    cb_reset();
    r = feed(&ps, len41, 4);   /* 只喂到 LEN，多喂一字节会把 ERROR 覆盖成 NONE */
    CHECK_EQ(r, PROTO_R_ERROR);
    CHECK_EQ(proto_parser_is_idle(&ps), 1u);

    proto_parser_init(&ps);
    r = feed(&ps, len255, 4);
    CHECK_EQ(r, PROTO_R_ERROR);

    /* 40 是允许的最大值：不报错，只是继续等载荷 */
    proto_parser_init(&ps);
    r = feed(&ps, len40, 4);
    CHECK_EQ(r, PROTO_R_NONE);
    CHECK_EQ(proto_parser_is_idle(&ps), 0u);
  }

  /* ---------------- 帧头错误与重同步 ---------------- */
  SECTION("proto: 帧头错误 / 连续 0xAA / 垃圾前缀");
  {
    /* AA 后面不是 55：报错并回到找帧头 */
    proto_parser_init(&ps);
    cb_reset();
    {
      static const uint8_t bad[3] = {0xAA, 0x00, 0x00};
      r = feed(&ps, bad, 2);   /* 同理：第 3 个字节会覆盖结果 */
      CHECK_EQ(r, PROTO_R_ERROR);
      CHECK_EQ(proto_parser_is_idle(&ps), 1u);
    }

    /* 连续 0xAA：AA AA 55 ... 必须能解出来（v5.1.1 的容错） */
    proto_parser_init(&ps);
    cb_reset();
    total = proto_encode(PROTO_T_PING, 0x31u, (const uint8_t *)0, 0u, buf, sizeof buf);
    {
      uint8_t stream[3 + PROTO_MAX_FRAME];
      stream[0] = 0xAA; stream[1] = 0xAA; stream[2] = 0x55;
      for (i = 0u; i < (uint8_t)total - 2u; i++) { stream[3 + i] = buf[2 + i]; }
      r = feed(&ps, stream, 3 + (int)total - 2);
      CHECK_EQ(r, PROTO_R_FRAME);
      CHECK_EQ(cb_count, 1);
      CHECK_EQ(cb_seq, 0x31u);
    }

    /* 垃圾前缀（可打印字符）：解析器应安静跳过，不报错、不误解帧 */
    proto_parser_init(&ps);
    cb_reset();
    {
      static const uint8_t junk[3] = {'a', 'b', 'c'};
      r = feed(&ps, junk, 3);
      CHECK_EQ(r, PROTO_R_NONE);
      total = proto_encode(PROTO_T_PING, 0x32u, (const uint8_t *)0, 0u, buf, sizeof buf);
      r = feed(&ps, buf, (int)total);
      CHECK_EQ(r, PROTO_R_FRAME);
      CHECK_EQ(cb_count, 1);
    }
  }

  /* ---------------- 背靠背多帧 ---------------- */
  SECTION("proto: 连续多帧");
  {
    proto_parser_init(&ps);
    cb_reset();
    total = proto_encode(PROTO_T_PING, 0x40u, (const uint8_t *)0, 0u, buf, sizeof buf);
    for (i = 0u; i < 5u; i++)
    {
      buf[4] = (uint8_t)(0x40u + i);             /* 改 SEQ，CRC 会失效，故重算 */
      total = proto_encode(PROTO_T_PING, (uint8_t)(0x40u + i), (const uint8_t *)0, 0u, buf, sizeof buf);
      feed(&ps, buf, (int)total);
    }
    CHECK_EQ(cb_count, 5);
    CHECK_EQ(cb_seq, 0x44u);                     /* 最后一帧的 SEQ */
    CHECK_EQ(proto_parser_is_idle(&ps), 1u);
  }

  /* ---------------- 半截帧的连锁代价（记录现状，不是期望行为） ---------------- */
  SECTION("proto: 半截帧（记录现状，超时复位在 app_cmd 层）");
  {
    proto_parser_init(&ps);
    cb_reset();
    {
      /* 声称 10 字节载荷，实际只给 3 字节 */
      uint8_t half[8];
      half[0] = 0xAA; half[1] = 0x55; half[2] = PROTO_T_PING;
      half[3] = 10;   half[4] = 0x01;
      half[5] = 0x11; half[6] = 0x22; half[7] = 0x33;
      feed(&ps, half, 8);
      CHECK_EQ(proto_parser_is_idle(&ps), 0u);   /* 卡在 BODY */

      /* 紧接一个合法帧：会被当作载荷吃掉，收不到 */
      total = proto_encode(PROTO_T_PING, 0x50u, (const uint8_t *)0, 0u, buf, sizeof buf);
      r = feed(&ps, buf, (int)total);
      CHECK_EQ(cb_count, 0);
      (void)r;

      /* 再来一帧：CRC 阶段被吃掉 2 字节后报错，此时已复位 */
      total = proto_encode(PROTO_T_PING, 0x51u, (const uint8_t *)0, 0u, buf, sizeof buf);
      r = feed(&ps, buf, (int)total);
      CHECK_EQ(proto_parser_is_idle(&ps), 1u);

      /* 第三帧开始恢复正常 —— 这就是"半截帧吃掉 2 帧"的来源 */
      cb_reset();
      total = proto_encode(PROTO_T_PING, 0x52u, (const uint8_t *)0, 0u, buf, sizeof buf);
      r = feed(&ps, buf, (int)total);
      CHECK_EQ(r, PROTO_R_FRAME);
      CHECK_EQ(cb_count, 1);
    }
  }
}
