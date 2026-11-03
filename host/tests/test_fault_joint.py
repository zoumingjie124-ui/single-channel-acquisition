"""
联合契约测试：同一串字节在**固件 C 侧**和**上位机 Python 侧**同时被断言。

背景：`fw_tests/test_fault.c` 里把三组字节向量写成 C 数组并逐字节比对；
`host/protocol.py` 是另一份独立实现。两边各自测自己，是证明不了"两端一致"的——
必须有人拿同一串字节在两边都验一遍。

本文件做两件事：
  1. 直接从 `fw_tests/test_fault.c` 里把 C 侧那三组向量读出来（不是抄一遍），
     与 Python 侧重新构造的帧逐字节比对——任一侧改了格式，这里立刻红；
  2. 用这三组字节喂 Python 解析器，断言字段值。

所以这条测试是**跨语言的**：它读的是 C 源文件，验的是 Python 解码器。

    python -m unittest tests.test_fault_joint -v
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)

import protocol as P  # noqa: E402

FAULT_C = os.path.normpath(os.path.join(HERE, "..", "fw_tests", "test_fault.c"))

# 契约向量：与 fw_tests/test_fault.c 第 53~59 行完全一致（那里也标了本文件名）
VEC_CONFIRM_HEX = "AA 55 86 09 00 B8 0B 00 00 01 01 01 0C 00 F9 45"
VEC_RECOVER_HEX = "AA 55 86 09 01 70 17 00 00 02 01 00 78 00 54 D2"
VEC_SET_DIAG_HEX = "AA 55 15 01 02 01 51 62"


def hex_to_bytes(s: str) -> bytes:
    return bytes.fromhex(s.replace(" ", ""))


def read_c_vectors(path: str):
    """从 C 源里抽出形如 { 0xAA,0x55,... }; 的字节数组。找不到返回空列表。"""
    with open(path, encoding="utf-8") as f:
        src = f.read()
    out = []
    for m in re.finditer(r"0xAA\s*,\s*0x55(?:[\s\S]*?)\}\s*;", src):
        vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(0))]
        out.append(bytes(vals))
    return out


@unittest.skipUnless(os.path.exists(FAULT_C), "找不到 fw_tests/test_fault.c，跳过")
class TestFaultContractVectors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.c_vecs = read_c_vectors(FAULT_C)

    def test_c_file_has_three_vectors(self):
        self.assertEqual(len(self.c_vecs), 3,
                         "期望 C 侧存在 3 组契约向量，实际 %d 组" % len(self.c_vecs))

    def test_c_vectors_equal_python_vectors(self):
        """C 侧数组与 Python 侧 hex 必须逐字节相同（这条是跨语言的那一条）"""
        expect = [hex_to_bytes(VEC_CONFIRM_HEX),
                  hex_to_bytes(VEC_RECOVER_HEX),
                  hex_to_bytes(VEC_SET_DIAG_HEX)]
        for i, (got, want) in enumerate(zip(self.c_vecs, expect)):
            self.assertEqual(got.hex(" ").upper(), want.hex(" ").upper(),
                             "第 %d 组契约向量两端不一致" % (i + 1))

    def test_python_can_rebuild_c_vectors(self):
        """反过来：用 Python 从字段值重新构造，必须还原出同一串字节"""
        p_confirm = bytes([
            0xB8, 0x0B, 0x00, 0x00,     # t_ms = 3000
            0x01,                       # event = 确认
            0x01,                       # fault_id = STUCK
            0x01,                       # active = STUCK
            0x0C, 0x00,                 # value = 12
        ])
        self.assertEqual(P.encode(P.T_FAULT, 0x00, p_confirm).hex(" ").upper(),
                         VEC_CONFIRM_HEX)

        p_recover = bytes([0x70, 0x17, 0x00, 0x00, 0x02, 0x01, 0x00, 0x78, 0x00])
        self.assertEqual(P.encode(P.T_FAULT, 0x01, p_recover).hex(" ").upper(),
                         VEC_RECOVER_HEX)

        self.assertEqual(P.build_set_diag(True, 2).hex(" ").upper(), VEC_SET_DIAG_HEX)

    def test_parse_confirm(self):
        f = P.FrameParser()
        frames = f.feed(hex_to_bytes(VEC_CONFIRM_HEX))
        self.assertEqual(f.errors, 0)
        self.assertEqual(len(frames), 1)
        obj = P.decode_many(frames)[0]
        self.assertIsInstance(obj, P.Fault)
        self.assertEqual(obj.t_ms, 3000)
        self.assertEqual(obj.event, P.FAULT_EV_CONFIRM)
        self.assertEqual(obj.fault_id, P.FAULT_ID_STUCK)
        self.assertEqual(obj.active, P.FAULT_MASK_STUCK)
        self.assertEqual(obj.value, 12)
        self.assertEqual(obj.text, "STUCK 卡死 确认")
        self.assertEqual(obj.active_names, "STUCK")

    def test_parse_recover(self):
        f = P.FrameParser()
        frames = f.feed(hex_to_bytes(VEC_RECOVER_HEX))
        self.assertEqual(f.errors, 0)
        obj = P.decode_many(frames)[0]
        self.assertEqual(obj.t_ms, 6000)
        self.assertEqual(obj.event, P.FAULT_EV_RECOVER)
        self.assertEqual(obj.active, 0, "恢复帧的掩码应为 0")
        self.assertEqual(obj.value, 120)
        self.assertEqual(obj.text, "STUCK 卡死 恢复")
        self.assertEqual(obj.active_names, "无")

    def test_parse_set_diag(self):
        f = P.FrameParser()
        frames = f.feed(hex_to_bytes(VEC_SET_DIAG_HEX))
        self.assertEqual(f.errors, 0)
        self.assertEqual(len(frames), 1)
        msg_type, seq, payload = frames[0]
        self.assertEqual(msg_type, P.T_SET_DIAG)
        self.assertEqual(seq, 2)
        self.assertEqual(payload, b"\x01", "SET_DIAG 载荷就是 1 字节 enable")

    def test_two_frames_back_to_back(self):
        """连续两帧粘在一起也必须各自解出来（帧定界的回归）"""
        f = P.FrameParser()
        frames = f.feed(hex_to_bytes(VEC_CONFIRM_HEX) + hex_to_bytes(VEC_RECOVER_HEX))
        self.assertEqual(f.errors, 0)
        objs = P.decode_many(frames)
        self.assertEqual(len(objs), 2)
        self.assertEqual(objs[0].event, P.FAULT_EV_CONFIRM)
        self.assertEqual(objs[1].event, P.FAULT_EV_RECOVER)


class TestFaultLocalRules(unittest.TestCase):
    """不依赖 C 文件的那部分：帧长、字段越界、未知 id 的容错"""

    def test_fault_len_must_be_9(self):
        for bad in (b"", b"\x00" * 8, b"\x00" * 10):
            with self.assertRaises(ValueError):
                P.parse_fault(bad)

    def test_unknown_fault_id_keeps_hex_name(self):
        obj = P.parse_fault(bytes([0x00, 0x00, 0x00, 0x00, 0x01, 0x7F, 0x00, 0x00, 0x00]))
        self.assertEqual(obj.fault_name, "0x7F")
        self.assertEqual(obj.event_name, "确认")

    def test_unknown_event_is_labelled(self):
        obj = P.parse_fault(bytes([0x00, 0x00, 0x00, 0x00, 0x09, 0x01, 0x01, 0x00, 0x00]))
        self.assertEqual(obj.event_name, "未知(9)")

    def test_set_diag_off_builds_zero(self):
        self.assertEqual(P.build_set_diag(False, 0)[5], 0x00)
        self.assertEqual(P.build_set_diag(True, 0)[5], 0x01)


if __name__ == "__main__":
    unittest.main(verbosity=2)
