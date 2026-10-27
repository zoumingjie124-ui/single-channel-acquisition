"""
协议层单元测试。不依赖 PySide6 / pyserial / 硬件，用标准库即可运行：

    python -m unittest discover -s tests -v

覆盖点：
    1. CRC 与公开标准向量一致（CRC-16/CCITT-FALSE，b"123456789" -> 0x29B1）
    2. 组帧后可原样解析回来（逐字段核对）
    3. 分包到达（一次一个字节）也能拼出完整帧
    4. 垃圾字节、损坏 CRC、超长 LEN 都能重新同步而不是卡死
    5. 文本行（设备 help/stat 应答）不会被误当成帧
    6. 长度与固件的 struct 定义严格一致
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import protocol as P  # noqa: E402


class TestCRC(unittest.TestCase):
    def test_standard_vector(self):
        # CRC-16/CCITT-FALSE 的标准校验值
        self.assertEqual(P.crc16(b"123456789"), 0x29B1)

    def test_empty(self):
        self.assertEqual(P.crc16(b""), 0xFFFF)


class TestEncodeParse(unittest.TestCase):
    def test_sample_roundtrip(self):
        payload = struct.pack("<IHHBB", 123456, 2480, 2470, 1, 42)
        frame = P.encode(P.T_SAMPLE, 0x5A, payload)

        self.assertEqual(frame[0], 0xAA)
        self.assertEqual(frame[1], 0x55)
        self.assertEqual(frame[2], P.T_SAMPLE)
        self.assertEqual(frame[3], 10)
        self.assertEqual(len(frame), P.HEADER_LEN + 10 + P.CRC_LEN)

        parser = P.FrameParser()
        frames = parser.feed(frame)
        self.assertEqual(len(frames), 1)
        _t, seq, body = frames[0]
        self.assertEqual(seq, 0x5A)

        s = P.parse_sample(body)
        self.assertEqual(s.t_ms, 123456)
        self.assertEqual(s.raw_mv, 2480)
        self.assertEqual(s.filt_mv, 2470)
        self.assertEqual(s.level, 1)
        self.assertEqual(s.level_name, "WARN")
        self.assertEqual(s.seq8, 42)

    def test_stat_roundtrip(self):
        payload = struct.pack(
            "<IIIIIIIIHHB",
            60000, 6000, 7, 1, 2, 3, 4, 9, 2400, 3000, 2,
        )
        self.assertEqual(len(payload), 37)
        parser = P.FrameParser()
        frames = parser.feed(P.encode(P.T_STAT, 1, payload))
        st = P.parse_stat(frames[0][2])
        self.assertEqual(st.uptime_ms, 60000)
        self.assertEqual(st.samples, 6000)
        self.assertEqual(st.drops, 7)
        self.assertEqual(st.changes, 9)
        self.assertEqual(st.thr_warn, 2400)
        self.assertEqual(st.thr_crit, 3000)
        self.assertEqual(st.level_name, "CRIT")

    def test_event_and_boot(self):
        parser = P.FrameParser()
        ev = P.parse_event(
            P.encode(P.T_EVENT, 2, struct.pack("<IBBH", 1000, 0, 1, 2410))[5:-2]
        )
        self.assertEqual(ev.text, "NORMAL -> WARN")
        self.assertEqual(ev.filt_mv, 2410)

        bt = P.parse_boot(
            P.encode(P.T_BOOT, 3, struct.pack("<HHBBB", 0x0500, 100, 2, 8, 1))[5:-2]
        )
        self.assertEqual(bt.version_text, "V5.0")
        self.assertEqual(bt.sample_rate, 100)
        self.assertEqual(bt.filter_depth, 8)
        del parser  # 仅为对称，不参与断言

    def test_all_command_builders_are_valid_frames(self):
        seq = 1
        for frame in (
            P.build_set_thr(2400, 3000, seq),
            P.build_set_led(True, seq),
            P.build_get_stat(seq),
            P.build_reset(seq),
            P.build_ping(seq),
        ):
            parser = P.FrameParser()
            frames = parser.feed(frame)
            self.assertEqual(len(frames), 1, "构造的命令帧无法被解析回来")
            self.assertEqual(parser.errors, 0)


class TestParserRobustness(unittest.TestCase):
    def _sample_frame(self, seq8=0):
        return P.encode(P.T_SAMPLE, 1, struct.pack("<IHHBB", 10, 100, 100, 0, seq8))

    def test_byte_by_byte(self):
        frame = self._sample_frame()
        parser = P.FrameParser()
        got = []
        for b in frame:
            got.extend(parser.feed(bytes([b])))
        self.assertEqual(len(got), 1)
        self.assertEqual(parser.errors, 0)

    def test_garbage_prefix_resync(self):
        frame = self._sample_frame()
        parser = P.FrameParser()
        frames = parser.feed(b"\x01\x02\xAA\x13garbage" + frame)
        self.assertEqual(len(frames), 1, "带垃圾前缀后没有重新对上帧")

    def test_double_sof0_prefix(self):
        """连续 0xAA 帧头：AA AA 55 ... 必须能解出，不能整帧丢失。

        固件侧的 PROTO_P_SOF1 在收到 0xAA 时保持本状态（把新的 0xAA 当作帧头首字节），
        Python 侧用 find(b"\xAA\x55") 天然支持。两边的行为必须一致。
        """
        frame = self._sample_frame(seq8=9)

        parser = P.FrameParser()
        frames = parser.feed(b"\xAA\xAA" + frame)
        self.assertEqual(len(frames), 1, "连续 0xAA 之后的一帧被丢掉了")
        self.assertEqual(frames[0][1], 1)

        parser2 = P.FrameParser()
        frames2 = parser2.feed(b"\xAA\xAA\xAA" + frame)
        self.assertEqual(len(frames2), 1, "连续三个 0xAA 之后的一帧被丢掉了")

        parser3 = P.FrameParser()
        frames3 = parser3.feed(b"\xAA" + frame)
        self.assertEqual(len(frames3), 1, "单个 0xAA 前缀不应影响解析")

    def test_bad_crc_is_rejected_then_recover(self):
        good = self._sample_frame()
        bad = bytearray(good)
        bad[-1] ^= 0xFF                    # 破坏 CRC 高字节

        parser = P.FrameParser()
        frames = parser.feed(bytes(bad))
        self.assertEqual(len(frames), 0, "CRC 错误的帧不该被接受")
        self.assertGreaterEqual(parser.errors, 1)

        # 紧接着来一个正常帧，必须还能解析
        frames = parser.feed(good)
        self.assertEqual(len(frames), 1, "坏帧之后没有恢复")

    def test_oversize_length_resync(self):
        parser = P.FrameParser()
        bogus = bytes([0xAA, 0x55, 0x81, 0xFF, 0x00])   # LEN=255 > MAX_PAYLOAD
        self.assertEqual(parser.feed(bogus), [])
        self.assertGreaterEqual(parser.errors, 1)

        frames = parser.feed(self._sample_frame())
        self.assertEqual(len(frames), 1)

    def test_truncated_frame_waits(self):
        frame = self._sample_frame()
        parser = P.FrameParser()
        self.assertEqual(parser.feed(frame[:-3]), [], "半截帧不应被吐出来")
        frames = parser.feed(frame[-3:])
        self.assertEqual(len(frames), 1, "补齐后应解析成功")

    def test_text_lines_are_separated(self):
        parser = P.FrameParser()
        data = b"OK thr=2400/3000\r\n" + self._sample_frame() + b"ERR unknown command\r\n"
        frames = parser.feed(data)
        lines = parser.drain_lines()
        self.assertEqual(len(frames), 1)
        self.assertIn("OK thr=2400/3000", lines)
        self.assertIn("ERR unknown command", lines)

    def test_text_line_split_across_reads(self):
        parser = P.FrameParser()
        parser.feed(b"OK thr=24")
        self.assertEqual(parser.drain_lines(), [])
        parser.feed(b"00/3000\r\n")
        self.assertEqual(parser.drain_lines(), ["OK thr=2400/3000"])


class TestLengths(unittest.TestCase):
    """长度必须与固件 app_proto.h 的定义严格一致，否则上位机会整体错位。"""

    def test_defined_lengths(self):
        self.assertEqual(len(struct.pack("<IHHBB", 0, 0, 0, 0, 0)), 10)   # SAMPLE
        self.assertEqual(len(struct.pack("<IBBH", 0, 0, 0, 0)), 8)        # EVENT
        self.assertEqual(len(struct.pack("<IIIIIIIIHHB", *([0] * 8), 0, 0, 0)), 37)  # STAT
        self.assertEqual(len(struct.pack("<HHBBB", 0, 0, 0, 0, 0)), 7)    # BOOT
        self.assertEqual(P.MAX_PAYLOAD, 40)


if __name__ == "__main__":
    unittest.main(verbosity=2)
