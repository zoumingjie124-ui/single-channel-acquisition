"""
协议三方一致性测试：`docs/PROTOCOL.md` ↔ 固件头文件 ↔ `host/protocol.py`。

为什么单独一条：
    常量一致（test_firmware_sync）和字节一致（test_fault_joint）都验了，
    但**文档**的一致性没人守。文档一旦和代码漂移，下一个人会照着错的文档写第三份实现。
    这里把文档里的"帧长度、字段偏移、示例字节"直接解析出来，和两边对拍。

覆盖 Stage 7C 的协议约束：
    1. FAULT 严格 9 字节，偏移/大小端/CRC 与 V5 一致
    2. SET_DIAG 严格 1 字节，只接受 0/1
    4. 掩码规则，且不得虚构 STUCK 以外的故障
    7. 文档里的 FAULT 示例帧 = 两端共用的契约向量

    python -m unittest tests.test_protocol_doc_sync -v
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)

import protocol as P  # noqa: E402

DOC = os.path.normpath(os.path.join(HERE, "..", "docs", "PROTOCOL.md"))
PROTO_H = os.path.normpath(os.path.join(HERE, "..", "App", "Inc", "app_proto.h"))
FAULT_C = os.path.normpath(os.path.join(HERE, "..", "fw_tests", "test_fault.c"))


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def parse_defines(path):
    out = {}
    for line in read_text(path).splitlines():
        m = re.match(r"\s*#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)[uUlL]*\s*(?:/\*.*?\*/)?\s*$", line)
        if m:
            out[m.group(1)] = int(m.group(2), 0)
    return out


def doc_type_rows(src):
    """解析 §3 的两张类型表：返回 {type_code: (name, len_str)}"""
    rows = {}
    for m in re.finditer(r"^\|\s*`(0x[0-9A-F]{2})`\s*\|\s*(\w+)\s*\|\s*(\d+)\s*\|", src, re.M):
        rows[int(m.group(1), 16)] = (m.group(2), int(m.group(3)))
    return rows


@unittest.skipUnless(os.path.exists(DOC) and os.path.exists(PROTO_H), "缺文档或固件头文件，跳过")
class TestProtocolDocSync(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = read_text(DOC)
        cls.fw = parse_defines(PROTO_H)
        cls.rows = doc_type_rows(cls.doc)

    # ---------------- 第 1 条：FAULT 9 字节 ----------------
    def test_fault_len_is_9_everywhere(self):
        self.assertEqual(self.fw["PROTO_LEN_FAULT"], 9, "固件里 FAULT 长度不是 9")
        self.assertEqual(P.parse_fault(bytes(9)).value, 0)   # 9 字节能解析
        self.assertIn(P.T_FAULT, self.rows, "文档 §3.1 没有 FAULT 行")
        self.assertEqual(self.rows[P.T_FAULT][1], 9, "文档里 FAULT 的 LEN 列不是 9")

    def test_set_diag_len_is_1_everywhere(self):
        self.assertEqual(self.fw["PROTO_LEN_SET_DIAG"], 1)
        self.assertEqual(self.rows[P.T_SET_DIAG][1], 1, "文档里 SET_DIAG 的 LEN 列不是 1")

    def test_fault_field_offsets_in_doc(self):
        """文档 §4.8 的偏移列必须是 0/4/5/6/7 —— 与 app_fault_build_payload 一致"""
        seg = self.doc[self.doc.index("### 4.8 FAULT"):self.doc.index("### 4.9 SET_DIAG")]
        offsets = [int(m) for m in re.findall(r"^\|\s*(\d+)\s*\|\s*`\w+`", seg, re.M)]
        self.assertEqual(offsets, [0, 4, 5, 6, 7],
                         "文档 §4.8 的字段偏移应为 0(t_ms) 4(event) 5(fault_id) 6(active) 7(value)")

    def test_fault_field_widths_in_doc(self):
        seg = self.doc[self.doc.index("### 4.8 FAULT"):self.doc.index("### 4.9 SET_DIAG")]
        widths = re.findall(r"^\|\s*\d+\s*\|\s*`\w+`\s*\|\s*(\w+)", seg, re.M)
        self.assertEqual(widths, ["u32", "u8", "u8", "u8", "u16"],
                         "字段宽度顺序应为 u32/u8/u8/u8/u16")
        # 4 + 1 + 1 + 1 + 2 = 9
        total = {"u8": 1, "u16": 2, "u32": 4}
        self.assertEqual(sum(total[w] for w in widths), 9)

    def test_crc_covers_type_to_payload(self):
        """CRC 覆盖 TYPE..PAYLOAD，不含帧头与 CRC 自身 —— 文档与 Python 都要这么说"""
        self.assertIn("TYPE + LEN + SEQ + PAYLOAD", self.doc.replace("**", ""))
        frame = P.encode(P.T_FAULT, 0x00, bytes(range(9)))
        crc_calc = P.crc16(frame[2:5 + 9])
        crc_recv = frame[-2] | (frame[-1] << 8)
        self.assertEqual(crc_calc, crc_recv)
        self.assertNotEqual(P.crc16(frame[0:5 + 9]), crc_recv, "不能把帧头算进 CRC")

    def test_little_endian_declared(self):
        self.assertIn("小端", self.doc)
        # 3000 = 0x0BB8，小端应为 B8 0B
        p = P.encode(P.T_FAULT, 0, bytes([0xB8, 0x0B, 0, 0, 1, 1, 1, 12, 0]))
        self.assertEqual(p[5:7], b"\xB8\x0B", "t_ms 必须小端")

    # ---------------- 第 2 条：SET_DIAG 只接受 0/1 ----------------
    def test_set_diag_accepts_only_0_and_1(self):
        seg = self.doc[self.doc.index("### 4.9 SET_DIAG"):self.doc.index("## 5. 文本调试通道")]
        self.assertIn("`0` = 关闭诊断，`1` = 使能", seg.replace("`", "`"))
        self.assertIn("result=1", seg, "文档必须写明非法参数回 ACK result=1")
        self.assertEqual(P.build_set_diag(True, 0)[5], 1)
        self.assertEqual(P.build_set_diag(False, 0)[5], 0)

    # ---------------- 第 4 条：只登记 STUCK，不许虚构 ----------------
    def test_only_stuck_is_registered(self):
        ids = {}
        seg = self.doc[self.doc.index("### 4.8 FAULT"):self.doc.index("### 4.9 SET_DIAG")]
        for m in re.finditer(r"^\|\s*`(0x[0-9A-F]{2})`\s*\|\s*`(\w+)`", seg, re.M):
            ids[m.group(1)] = m.group(2)
        self.assertEqual(ids, {"0x01": "STUCK"},
                         "文档里只允许登记 STUCK 一种故障，实际：%r" % ids)

        fw_ids = [k for k in self.fw if k.startswith("PROTO_FAULT_ID_")]
        self.assertEqual(fw_ids, ["PROTO_FAULT_ID_STUCK"],
                         "固件头文件里出现了 STUCK 以外的故障编号：%r" % fw_ids)

        py_ids = sorted(k for k in dir(P)
                        if k.startswith("FAULT_ID_") and k != "FAULT_ID_NAMES")
        self.assertEqual(py_ids, ["FAULT_ID_STUCK"],
                         "protocol.py 里出现了 STUCK 以外的故障编号：%r" % py_ids)
        self.assertEqual(set(P.FAULT_ID_NAMES), {P.FAULT_ID_STUCK},
                         "FAULT_ID_NAMES 表里登记了别的东西")

    def test_mask_matches_doc(self):
        self.assertEqual(self.fw["PROTO_FAULT_MASK_STUCK"], P.FAULT_MASK_STUCK)
        self.assertEqual(P.FAULT_MASK_STUCK, 0x01)
        self.assertIn("0x01", self.doc)

    # ---------------- 第 7 条：文档示例帧 = 契约向量 ----------------
    def test_doc_examples_equal_contract_vectors(self):
        docs = {}
        for m in re.finditer(r"(确认|恢复)：((?:[0-9A-F]{2} )+[0-9A-F]{2})", self.doc):
            docs[m.group(1)] = bytes.fromhex(m.group(2).replace(" ", ""))
        self.assertEqual(sorted(docs), ["恢复", "确认"], "文档里应有两组 FAULT 示例帧")

        c_vectors = []
        for m in re.finditer(r"0xAA\s*,\s*0x55(?:[\s\S]*?)\}\s*;", read_text(FAULT_C)):
            vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(0))]
            c_vectors.append(bytes(vals))
        self.assertEqual(len(c_vectors), 3, "C 侧应有 3 组向量（确认/恢复/SET_DIAG）")

        self.assertEqual(docs["确认"], c_vectors[0],
                         "文档里的确认示例帧与 C 侧契约向量不一致")
        self.assertEqual(docs["恢复"], c_vectors[1],
                         "文档里的恢复示例帧与 C 侧契约向量不一致")

        # 再用 Python 独立构造一遍，确认"文档示例 = 两端共用向量"
        self.assertEqual(P.encode(P.T_FAULT, 0x00,
                                  bytes([0xB8, 0x0B, 0x00, 0x00, 0x01, 0x01, 0x01, 0x0C, 0x00])),
                         docs["确认"])
        self.assertEqual(P.encode(P.T_FAULT, 0x01,
                                  bytes([0x70, 0x17, 0x00, 0x00, 0x02, 0x01, 0x00, 0x78, 0x00])),
                         docs["恢复"])

    def test_doc_set_diag_example(self):
        """§4.9 没写示例帧，但 §4.8 下面那组 SET_DIAG 向量必须能由 build_set_diag 重现"""
        c_vectors = []
        for m in re.finditer(r"0xAA\s*,\s*0x55(?:[\s\S]*?)\}\s*;", read_text(FAULT_C)):
            vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(0))]
            c_vectors.append(bytes(vals))
        self.assertEqual(P.build_set_diag(True, 2), c_vectors[2])

    # ---------------- 第 8 条：三处同步 ----------------
    def test_type_codes_three_way(self):
        self.assertEqual(self.fw["PROTO_T_FAULT"], 0x86)
        self.assertEqual(P.T_FAULT, 0x86)
        self.assertEqual(self.rows[P.T_FAULT][0], "FAULT")

        self.assertEqual(self.fw["PROTO_T_SET_DIAG"], 0x15)
        self.assertEqual(P.T_SET_DIAG, 0x15)
        self.assertEqual(self.rows[P.T_SET_DIAG][0], "SET_DIAG")

    def test_event_codes_three_way(self):
        self.assertEqual(self.fw["PROTO_FAULT_EV_CONFIRM"], 1)
        self.assertEqual(self.fw["PROTO_FAULT_EV_RECOVER"], 2)
        self.assertEqual(P.FAULT_EV_CONFIRM, 1)
        self.assertEqual(P.FAULT_EV_RECOVER, 2)
        self.assertIn("1 = 确认，2 = 恢复", self.doc.replace("**", ""))

    def test_old_frames_not_renumbered(self):
        """7C 不许动老帧的类型码与长度"""
        pairs = {
            "PROTO_T_SAMPLE": (P.T_SAMPLE, 0x81),
            "PROTO_T_EVENT": (P.T_EVENT, 0x82),
            "PROTO_T_STAT": (P.T_STAT, 0x83),
            "PROTO_T_ACK": (P.T_ACK, 0x84),
            "PROTO_T_BOOT": (P.T_BOOT, 0x85),
        }
        for name, (py_val, expect) in pairs.items():
            self.assertEqual(self.fw[name], expect, "%s 固件值被改了" % name)
            self.assertEqual(py_val, expect, "%s 上位机值被改了" % name)
            self.assertEqual(self.rows[expect][1],
                             {"0x81": 10, "0x82": 8, "0x83": 37, "0x84": 2, "0x85": 7}["0x%02X" % expect],
                             "文档里 %s 的 LEN 被改了" % name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
