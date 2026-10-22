"""
固件与上位机的"协议定义一致性"静态检查。

背景：没有本地 C 编译器，无法在 PC 上直接运行固件的 app_proto.c 做黑盒对比。
退一步做源码级检查——把固件头文件里的常量解析出来，和 protocol.py 的定义逐项比对。
这能抓到最常见也最致命的漂移：有人只改了一边。

注意：这是**静态检查**，不是联调。它不能证明运行期行为一致，
      只能证明两端的"约定"没有写错。

    python -m unittest tests.test_firmware_sync -v
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)

import protocol as P  # noqa: E402

FW_DIR = os.path.normpath(os.path.join(HERE, "..", "App"))
PROTO_H = os.path.join(FW_DIR, "Inc", "app_proto.h")
TASK_C = os.path.join(FW_DIR, "Src", "app_task.c")


def parse_defines(path):
    """把 #define NAME 123u 解析成 {NAME: int}"""
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"\s*#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)[uUlL]*\s*(?:/\*.*?\*/)?\s*$", line)
            if m:
                out[m.group(1)] = int(m.group(2), 0)
    return out


@unittest.skipUnless(os.path.exists(PROTO_H), "找不到固件头文件，跳过（单独拷贝 host 目录时会出现）")
class TestFirmwareSync(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fw = parse_defines(PROTO_H)

    def test_frame_markers(self):
        self.assertEqual(self.fw["PROTO_SOF0"], P.SOF0)
        self.assertEqual(self.fw["PROTO_SOF1"], P.SOF1)
        self.assertEqual(self.fw["PROTO_MAX_PAYLOAD"], P.MAX_PAYLOAD)
        self.assertEqual(self.fw["PROTO_HEADER_LEN"], P.HEADER_LEN)
        self.assertEqual(self.fw["PROTO_CRC_LEN"], P.CRC_LEN)

    def test_message_types(self):
        pairs = {
            "PROTO_T_SAMPLE": P.T_SAMPLE,
            "PROTO_T_EVENT": P.T_EVENT,
            "PROTO_T_STAT": P.T_STAT,
            "PROTO_T_ACK": P.T_ACK,
            "PROTO_T_BOOT": P.T_BOOT,
            "PROTO_T_SET_THR": P.T_SET_THR,
            "PROTO_T_SET_LED": P.T_SET_LED,
            "PROTO_T_GET_STAT": P.T_GET_STAT,
            "PROTO_T_RESET": P.T_RESET,
            "PROTO_T_PING": P.T_PING,
        }
        for name, value in pairs.items():
            self.assertIn(name, self.fw, "固件头文件缺少 %s" % name)
            self.assertEqual(self.fw[name], value, "%s 两端不一致：固件 0x%02X，上位机 0x%02X"
                             % (name, self.fw[name], value))

    def test_payload_lengths(self):
        self.assertEqual(self.fw["PROTO_LEN_SAMPLE"], 10)
        self.assertEqual(self.fw["PROTO_LEN_EVENT"], 8)
        self.assertEqual(self.fw["PROTO_LEN_STAT"], 37)
        self.assertEqual(self.fw["PROTO_LEN_ACK"], 2)
        self.assertEqual(self.fw["PROTO_LEN_BOOT"], 7)


@unittest.skipUnless(os.path.exists(TASK_C), "找不到 app_task.c，跳过")
class TestStatLayout(unittest.TestCase):
    """STAT 载荷是两端最容易写错的地方：8 个 u32 + 2 个 u16 + 1 个 u8，偏移必须严丝合缝。"""

    @classmethod
    def setUpClass(cls):
        with open(TASK_C, encoding="utf-8") as f:
            cls.src = f.read()

    def _offsets(self, func_name):
        start = self.src.index(func_name)
        end = self.src.index("\n}", start)
        body = self.src[start:end]
        u32 = [int(x) for x in re.findall(r"proto_put_u32\(&p\[(\d+)\]", body)]
        u16 = [int(x) for x in re.findall(r"proto_put_u16\(&p\[(\d+)\]", body)]
        single = [int(x) for x in re.findall(r"\bp\[(\d+)\]\s*=", body)]
        return u32, u16, single

    def test_emit_stat_layout(self):
        u32, u16, single = self._offsets("static void emit_stat(void)")
        self.assertEqual(u32, [0, 4, 8, 12, 16, 20, 24, 28], "STAT 的 u32 偏移不对")
        self.assertEqual(u16, [32, 34], "STAT 的 u16 偏移不对")
        self.assertEqual(single, [36], "STAT 的 level 字段偏移不对")
        # 8*4 + 2*2 + 1 = 37
        self.assertEqual(u32[-1] + 4 + 2 * len(u16) + len(single), 37)

    def test_emit_sample_layout(self):
        u32, u16, single = self._offsets("static void emit_sample(")
        self.assertEqual(u32, [0])
        self.assertEqual(u16, [4, 6])
        self.assertEqual(single, [8, 9])
        self.assertEqual(u32[-1] + 4 + 2 * len(u16) + len(single), 10)

    def test_emit_event_layout(self):
        u32, u16, single = self._offsets("static void emit_event(")
        self.assertEqual(u32, [0])
        self.assertEqual(u16, [6])
        self.assertEqual(single, [4, 5])
        # u32(4) + from/to(2 个 u8) + filt(1 个 u16) = 8
        self.assertEqual(u32[-1] + 4 + len(single) + 2 * len(u16), 8)


if __name__ == "__main__":
    unittest.main(verbosity=2)
