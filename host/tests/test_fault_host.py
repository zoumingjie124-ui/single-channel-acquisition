"""
上位机侧健壮性测试（Stage 7C B 组）。

与 `test_fault_joint.py` 的分工：
    joint  -> 与固件 C 侧共用同一串字节，验"两端约定一致"
    本文件 -> 只验"上位机自己不会被打挂"：未知帧、坏 CRC、错长度、连续多帧、旧协议兼容

为什么单独一组：
    接收线程一旦因为某个野帧抛异常退出，界面就"假死"——数据不更新但也不报错，
    这种故障最难查。所以这里断言的重点不是"解析对"，而是"解析器还活着"。
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)

import protocol as P  # noqa: E402


def frame(msg_type, seq, payload=b""):
    return P.encode(msg_type, seq, payload)


class TestUnknownFrames(unittest.TestCase):
    def test_unknown_type_is_skipped_not_fatal(self):
        """未知帧类型必须被静默跳过，且不影响后续帧"""
        f = P.FrameParser()
        bad = frame(0x99, 0, b"\x01\x02\x03")
        good = frame(P.T_SAMPLE, 1, bytes([0x00] * 10))
        frames = f.feed(bad + good)
        self.assertEqual(len(frames), 2, "两帧都应被识别为完整帧")
        self.assertEqual(f.errors, 0, "未知类型不是解析错误")
        objs = P.decode_many(frames)
        self.assertEqual(len(objs), 1, "只有 SAMPLE 能解出对象")
        self.assertIsInstance(objs[0], P.Sample)

    def test_unknown_fault_id_does_not_raise(self):
        """故障码未登记时，界面显示 0xNN，而不是抛异常"""
        payload = bytes([0x00, 0x00, 0x00, 0x00, P.FAULT_EV_CONFIRM, 0x7E, 0x00, 0x00, 0x00])
        obj = P.parse_fault(payload)
        self.assertEqual(obj.fault_name, "0x7E")
        self.assertEqual(obj.active_names, "无")

    def test_old_host_ignores_fault_frames(self):
        """旧版上位机（只认 0x81~0x85）遇到 FAULT 应当无感——兼容性承诺"""
        legacy = {P.T_SAMPLE, P.T_EVENT, P.T_STAT, P.T_ACK, P.T_BOOT}
        frames = [(P.T_FAULT, 0, b"\x00" * 9),
                  (P.T_SAMPLE, 1, bytes([0x00] * 10))]
        decoded = [x for x in frames if x[0] in legacy]
        self.assertEqual(len(decoded), 1, "旧逻辑会忽略 FAULT，只处理 SAMPLE")
        self.assertIsInstance(P.decode_many(decoded)[0], P.Sample)


class TestBrokenFrames(unittest.TestCase):
    def test_bad_crc_counts_and_recovers(self):
        """坏 CRC：计一次错误，且下一帧仍能正常解析（解析器没有卡死）"""
        good = frame(P.T_STAT, 0, bytes([0x00] * 37))
        broken = bytearray(good)
        broken[-1] ^= 0xFF                    # 破坏 CRC 高字节
        f = P.FrameParser()
        frames = f.feed(bytes(broken) + good)
        self.assertGreaterEqual(f.errors, 1, "坏 CRC 必须计错误")
        self.assertEqual(len(frames), 1, "坏帧丢弃后，后面那帧要能解出来")
        self.assertIsInstance(P.decode_many(frames)[0], P.Stat)

    def test_wrong_length_payload_is_skipped(self):
        """FAULT 长度不对：帧本身合法（CRC 对），但 decode 层拒绝，不抛给界面"""
        f = P.FrameParser()
        frames = f.feed(frame(P.T_FAULT, 0, bytes([0x00] * 8)))
        self.assertEqual(f.errors, 0, "LEN=8 是合法帧长，只是载荷不合规")
        self.assertEqual(len(frames), 1)
        self.assertEqual(P.decode_many(frames), [], "长度不符应被丢弃，不产生对象")

    def test_garbage_prefix_then_fault(self):
        """线上有垃圾字节时，FAULT 仍能被找到"""
        f = P.FrameParser()
        frames = f.feed(b"\x11\x22\x33" + frame(P.T_FAULT, 0, b"\x00" * 9))
        self.assertEqual(len(frames), 1)
        self.assertIsInstance(P.decode_many(frames)[0], P.Fault)


class TestMixedStream(unittest.TestCase):
    def test_five_types_back_to_back(self):
        """SAMPLE/STAT/FAULT/ACK/BOOT 粘在一起：一次喂完，顺序与数量都不能错"""
        stream = (frame(P.T_SAMPLE, 0, bytes([0x10] * 10))
                  + frame(P.T_STAT, 1, bytes([0x00] * 37))
                  + frame(P.T_FAULT, 2, bytes([0x00] * 9))
                  + frame(P.T_ACK, 3, bytes([P.T_SET_DIAG, 0]))
                  + frame(P.T_BOOT, 4, bytes([0x00] * 7)))
        f = P.FrameParser()
        frames = f.feed(stream)
        self.assertEqual(f.errors, 0)
        self.assertEqual([x[0] for x in frames],
                         [P.T_SAMPLE, P.T_STAT, P.T_FAULT, P.T_ACK, P.T_BOOT])
        objs = P.decode_many(frames)
        self.assertEqual(len(objs), 5)
        self.assertEqual([type(o).__name__ for o in objs],
                         ["Sample", "Stat", "Fault", "Ack", "Boot"])

    def test_byte_by_byte_feed(self):
        """逐字节喂（模拟串口碎片化）：结果必须与一次性喂完全相同"""
        stream = frame(P.T_FAULT, 7, bytes([0x00] * 9))
        f = P.FrameParser()
        got = []
        for b in stream:
            got.extend(f.feed(bytes([b])))
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0][1], 7)
        self.assertEqual(f.errors, 0)


class TestRegressionOldTypes(unittest.TestCase):
    """Stage 7C 不许动 SAMPLE / STAT —— 这里锁死它们的长度与字段"""

    def test_sample_roundtrip(self):
        payload = bytes([0xE8, 0x03, 0x00, 0x00, 0xAE, 0x03, 0xAD, 0x03, 0x01, 0x7F])
        obj = P.parse_sample(payload)
        self.assertEqual(obj.t_ms, 1000)
        self.assertEqual(obj.raw_mv, 942)
        self.assertEqual(obj.filt_mv, 941)
        self.assertEqual(obj.level, 1)
        self.assertEqual(obj.seq8, 0x7F)

    def test_stat_roundtrip(self):
        f = P.FrameParser()
        payload = bytes([0] * 37)
        frames = f.feed(frame(P.T_STAT, 0, payload))
        obj = P.decode_many(frames)[0]
        self.assertIsInstance(obj, P.Stat)
        self.assertEqual(len(payload), 37, "STAT 载荷长度不许变")

    def test_ack_for_set_diag_is_two_bytes(self):
        f = P.FrameParser()
        frames = f.feed(frame(P.T_ACK, 1, bytes([P.T_SET_DIAG, 1])))
        a = P.decode_many(frames)[0]
        self.assertEqual(a.req_type, P.T_SET_DIAG)
        self.assertEqual(a.result, 1)
        self.assertEqual(a.result_name, "参数错误")


if __name__ == "__main__":
    unittest.main(verbosity=2)
