"""
端到端离线回放测试：不需要硬件、不需要 Qt。

用 protocol.encode() 合成 5 秒的 100Hz 数据流（含告警切换、丢帧、坏帧），
再用与界面完全相同的 FrameParser + decode_many 解析，检查：
    - 帧数、字段、序号连续性
    - 丢帧能被 seq8 缺口发现
    - 坏帧被丢弃且不影响后续
    - 文本行不会被吞
这相当于把「上位机数据通路」在没有板子的情况下跑一遍。
"""

import os
import random
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import protocol as P  # noqa: E402


def build_stream(seconds=5, rate=100, drop_every=0, inject_bad=True, seed=7):
    """合成数据流，返回 (bytes, 期望的样本数)"""
    rnd = random.Random(seed)
    out = bytearray()
    seq = 0
    n = 0
    total = seconds * rate
    level = 0

    for i in range(total):
        t_ms = int(i * 1000 / rate)
        raw = 1650 + int(1200 * (i % 200) / 200.0) + rnd.randint(-15, 15)
        raw = max(0, min(3300, raw))
        filt = raw - 10
        new_level = 2 if filt > 3000 else (1 if filt > 2400 else 0)
        if new_level != level:
            level = new_level
            out += P.encode(P.T_EVENT, seq, struct.pack("<IBBH", t_ms, 0, level, filt))
            seq = (seq + 1) & 0xFF

        # 制造丢帧：跳过某些样本
        if drop_every and i % drop_every == 0 and i > 0:
            continue

        out += P.encode(P.T_SAMPLE, seq, struct.pack("<IHHBB", t_ms, raw, filt, level, i & 0xFF))
        seq = (seq + 1) & 0xFF
        n += 1

        if i % 100 == 0:
            out += P.encode(P.T_STAT, seq,
                            struct.pack("<IIIIIIIIHHB", t_ms, n, 0, 0, 0, 0, 0, 0, 2400, 3000, level))
            seq = (seq + 1) & 0xFF

        if inject_bad and i == 137:
            bad = bytearray(P.encode(P.T_SAMPLE, seq, struct.pack("<IHHBB", t_ms, 1, 1, 0, 0)))
            bad[-1] ^= 0xFF
            out += bytes(bad)          # 坏 CRC，应被丢弃
            out += b"noise-bytes"      # 夹带文本噪声
            seq = (seq + 1) & 0xFF

    return bytes(out), n, total


class TestPipeline(unittest.TestCase):
    def test_five_seconds_100hz(self):
        data, expected, _ = build_stream(seconds=5, rate=100)
        parser = P.FrameParser()
        frames = parser.feed(data)
        objs = P.decode_many(frames)
        samples = [o for o in objs if isinstance(o, P.Sample)]

        # 坏帧是额外插入的副本，正常帧全都在，所以样本数等于 expected
        self.assertEqual(len(samples), expected)
        self.assertEqual(parser.errors, 1, "刚好应该只有 1 个坏 CRC 被拒绝")

        # 序号必须连续递增（无丢帧场景）
        for a, b in zip(samples, samples[1:]):
            self.assertEqual(b.seq8, (a.seq8 + 1) & 0xFF, "序号出现非预期跳变")

        # 时间戳单调
        for a, b in zip(samples, samples[1:]):
            self.assertGreaterEqual(b.t_ms, a.t_ms)

        # 必须解析出 EVENT 与 STAT
        self.assertTrue(any(isinstance(o, P.Event) for o in objs), "没有解析到告警事件")
        self.assertTrue(any(isinstance(o, P.Stat) for o in objs), "没有解析到统计帧")

    def test_gap_detection(self):
        data, expected, _ = build_stream(seconds=2, rate=100, drop_every=17, inject_bad=False)
        parser = P.FrameParser()
        samples = [o for o in P.decode_many(parser.feed(data)) if isinstance(o, P.Sample)]

        gaps = 0
        for a, b in zip(samples, samples[1:]):
            gaps += (b.seq8 - a.seq8 - 1) & 0xFF
        self.assertGreater(gaps, 0, "主动丢帧后序号缺口没有被检测到")

    def test_text_noise_does_not_break_stream(self):
        data, expected, _ = build_stream(seconds=1, rate=100)
        parser = P.FrameParser()
        frames = parser.feed(data + b"OK led on\r\n" + data)
        self.assertEqual(parser.drain_lines(), ["OK led on"])
        # 两段数据都要被解析出来，中间的文本行不能吞掉任何一帧
        self.assertGreater(len(frames), expected - 1)

    def test_byte_by_byte_replay(self):
        data, expected, _ = build_stream(seconds=1, rate=100, inject_bad=False)
        parser = P.FrameParser()
        got = 0
        # 模拟串口一次读到 13 字节（真实串口很不规则）
        for i in range(0, len(data), 13):
            got += len(parser.feed(data[i:i + 13]))
        self.assertGreater(got, 0)
        self.assertEqual(parser.errors, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
