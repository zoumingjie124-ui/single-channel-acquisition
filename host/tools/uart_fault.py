#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""阶段三：UART/DMA 异常验证（全部可在 PC 侧注入，无需改线）。

    python host/tools/uart_fault.py COM6

覆盖：
  T1 CRC 错误        合法帧结构 + 错误 CRC，验证 parser 复位与 frame_err 计数
  T2 长度非法        LEN = 41 / 255，验证越界拒绝
  T3 半截帧          只发帧头 + 部分载荷后静默，测"后续帧会不会被吃掉"
  T4 未知命令类型    TYPE = 0x99，验证 ACK result=2 与 cmd_err
  T5 非法文本命令    未知字符串，验证不影响二进制通道
  T6 超长文本行      200 字符，验证 64 字节行缓冲截断而不越界
  T7 连续无空闲流    200 字节不停顿，强制走 DMA 缓冲满（TC）路径
  T8 发送队列灌满    连续 100 条 stat，验证整帧原子丢弃（不留半帧）
  T9 帧错误(FE/NE)   用错误波特率发数据，制造 framing error 后验证能否恢复

每一项都记录 STAT 计数器的前后差值，并检查链路是否仍然存活。
原始记录写入 docs/STAGE3_RAW.txt。
"""

import io
import os
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.dirname(HERE)
ROOT = os.path.dirname(HOST)
sys.path.insert(0, HOST)

import protocol as P  # noqa: E402

OUT = os.path.join(ROOT, "docs", "STAGE3_RAW.txt")
SOF0, SOF1 = 0xAA, 0x55


def build_frame(ftype, seq, payload=b"", crc_xor=0):
    """构造一帧；crc_xor != 0 时故意破坏 CRC。"""
    body = bytes([ftype, len(payload), seq]) + payload
    crc = P.crc16(body) if hasattr(P, "crc16") else _crc16(body)
    crc ^= crc_xor & 0xFFFF
    return bytes([SOF0, SOF1]) + body + bytes([crc & 0xFF, crc >> 8])


def _crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class Link:
    def __init__(self, port, baud=115200):
        self.port = port
        self.baud = baud
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.ser.reset_input_buffer()
        self.parser = P.FrameParser()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def reopen(self, baud):
        self.close()
        time.sleep(0.15)
        self.baud = baud
        self.ser = serial.Serial(self.port, baud, timeout=0.05)
        self.ser.reset_input_buffer()
        self.parser = P.FrameParser()

    def write(self, data):
        self.ser.write(data)

    def collect(self, seconds):
        """接收一段时间，返回 (原始字节, 解析出的帧列表)。"""
        end = time.time() + seconds
        raw = b""
        frames = []
        while time.time() < end:
            try:
                n = self.ser.in_waiting
            except Exception:
                break
            if n == 0:
                time.sleep(0.002)
                continue
            try:
                chunk = self.ser.read(n)
            except Exception:
                break
            raw += chunk
            for mtype, mseq, payload in self.parser.feed(chunk):
                frames.append((mtype, mseq, payload))
        return raw, frames

    def stat(self, timeout=1.5):
        """发 stat 文本命令，解析 STAT 行。"""
        self.ser.reset_input_buffer()
        self.write(b"stat\r\n")
        raw, _ = self.collect(0.35)
        for _ in range(int(timeout / 0.35) + 1):
            i = raw.find(b"STAT t=")
            if i >= 0:
                j = raw.find(b"\r\n", i)
                line = raw[i:j if j > 0 else len(raw)].decode("ascii", "replace")
                m = re_stat(line)
                if m:
                    return m
            more, _ = self.collect(0.35)
            if not more:
                break
            raw += more
        return None


def re_stat(line):
    import re
    m = re.search(
        r"STAT t=(\d+)ms n=(\d+) drop=(\d+) adc=(\d+) uart=(\d+) rxf=(\d+) "
        r"frm=(\d+) chg=(\d+) cmd=(\d+)/(\d+) txd=(\d+)B", line)
    if not m:
        return None
    g = [int(x) for x in m.groups()]
    return dict(t=g[0], n=g[1], drop=g[2], adc=g[3], uart=g[4], rxf=g[5],
                frm=g[6], chg=g[7], cmd_ok=g[8], cmd_err=g[9], txd=g[10])


def diff(a, b):
    if not a or not b:
        return {}
    return {k: b[k] - a[k] for k in ("n", "drop", "adc", "uart", "rxf", "frm",
                                    "cmd_ok", "cmd_err", "txd")}


def alive(link):
    """链路是否还能收发：发 ping 看有没有 ACK，再看 stat 有没有回。"""
    ack = False
    link.write(build_frame(P.T_PING, 0x5A))
    _raw, frames = link.collect(0.5)
    for mtype, _seq, _pl in frames:
        if mtype == P.T_ACK:
            ack = True
    st = link.stat()
    return ack, st is not None, st


def main():
    if len(sys.argv) < 2:
        print("用法: python host/tools/uart_fault.py <COMx>")
        return 2
    port = sys.argv[1]
    out = io.StringIO()

    def w(s=""):
        out.write(s + "\n")
        print(s)

    link = Link(port)
    results = []
    try:
        base = link.stat()
        w("=" * 78)
        w("阶段三 UART/DMA 异常验证 — 原始记录")
        w("=" * 78)
        w("时间   : %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
        w("串口   : %s 115200 8N1" % port)
        w("基线   : %s" % base)
        w()

        # ---------------- T1 CRC 错误 ----------------
        before = link.stat()
        n = 20
        for i in range(n):
            link.write(build_frame(P.T_PING, i & 0xFF, b"", crc_xor=0xFFFF))
        link.collect(0.4)
        after = link.stat()
        d = diff(before, after)
        ack_ok, st_ok, _ = alive(link)
        w("[T1] CRC 错误 x%d：frame_err +%s，uart_err +%s，链路存活=%s/%s"
          % (n, d.get("frm"), d.get("uart"), ack_ok, st_ok))
        results.append(("T1 CRC 错误", "frm +%s" % d.get("frm"), "uart +%s" % d.get("uart"),
                        ack_ok and st_ok))

        # ---------------- T2 长度非法 ----------------
        before = link.stat()
        for bad_len in (41, 255, 200):
            link.write(bytes([SOF0, SOF1, P.T_PING, bad_len, 0x00]) + b"\x00" * 4)
            time.sleep(0.05)
        link.collect(0.4)
        after = link.stat()
        d = diff(before, after)
        ack_ok, st_ok, _ = alive(link)
        w("[T2] LEN=41/255/200：frame_err +%s，链路存活=%s/%s"
          % (d.get("frm"), ack_ok, st_ok))
        results.append(("T2 长度非法", "frm +%s" % d.get("frm"), "-",
                        ack_ok and st_ok))

        # ---------------- T3 半截帧 ----------------
        before = link.stat()
        # 发一个"看起来还有 10 字节载荷"的帧，但只给 3 字节
        link.write(bytes([SOF0, SOF1, P.T_PING, 10, 0x01]) + b"\x11\x22\x33")
        time.sleep(0.30)                     # 让 IDLE 触发，parser 停在 BODY
        # 接着发 3 个合法 ping，看能救回几个
        got = 0
        for i in range(3):
            link.write(build_frame(P.T_PING, 0x70 + i))
            _r, fr = link.collect(0.30)
            if any(t == P.T_ACK for t, _s, _p in fr):
                got += 1
        after = link.stat()
        d = diff(before, after)
        w("[T3] 半截帧后连发 3 帧 ping：收到 ACK %d/3，frame_err +%s"
          % (got, d.get("frm")))
        results.append(("T3 半截帧", "收帧 %d/3" % got, "frm +%s" % d.get("frm"),
                        got >= 2))

        # ---------------- T4 未知二进制命令 ----------------
        before = link.stat()
        link.write(build_frame(0x99, 0x02, b"\x01"))
        raw, fr = link.collect(0.4)
        ack_res = None
        for t, _s, pl in fr:
            if t == P.T_ACK and len(pl) >= 2:
                ack_res = pl[1]
        after = link.stat()
        d = diff(before, after)
        w("[T4] 未知 TYPE=0x99：ACK result=%s（期望 2），cmd_err +%s"
          % (ack_res, d.get("cmd_err")))
        results.append(("T4 未知命令", "ACK result=%s" % ack_res,
                        "cmd_err +%s" % d.get("cmd_err"), ack_res == 2))

        # ---------------- T5 非法文本命令 ----------------
        before = link.stat()
        link.write(b"bogus\r\n")
        raw, _fr = link.collect(0.4)
        n_err = raw.count(b"ERR unknown command")
        after = link.stat()
        d = diff(before, after)
        w("[T5] 非法文本命令：ERR 行 %d（期望 1），cmd_err 增量 %s（期望 0）"
          % (n_err, d.get("cmd_err")))
        results.append(("T5 非法文本", "ERR %d" % n_err,
                        "cmd_err +%s" % d.get("cmd_err"), n_err == 1))

        # ---------------- T6 超长文本行 ----------------
        before = link.stat()
        long_line = b"q" * 200 + b"\r\n"
        link.write(long_line)
        raw, _fr = link.collect(0.5)
        i = raw.find(b"ERR unknown command")
        line_len = 0
        if i >= 0:
            j = raw.find(b"\r\n", i)
            line_len = (j - i) if j > 0 else 0
        after = link.stat()
        ack_ok, st_ok, _ = alive(link)
        w("[T6] 200 字符文本行：回显长度 %d（应为 21+63=84），链路存活=%s/%s"
          % (line_len, ack_ok, st_ok))
        results.append(("T6 超长文本", "回显 %d 字节" % line_len, "-",
                        ack_ok and st_ok))

        # ---------------- T7 连续无空闲流（走 TC 路径） ----------------
        before = link.stat()
        burst = b"z" * 190 + b"\r\n"       # 190 > DMA 64，必然触发 TC
        link.write(burst)
        raw, _fr = link.collect(0.6)
        n_err = raw.count(b"ERR unknown command")
        ack_ok, st_ok, _ = alive(link)
        after = link.stat()
        d = diff(before, after)
        w("[T7] 190 字节连续流（跨 DMA 64 缓冲，触发 TC）：ERR 行 %d，"
          "frm +%s，链路存活=%s/%s" % (n_err, d.get("frm"), ack_ok, st_ok))
        results.append(("T7 连续流 TC", "ERR %d" % n_err, "frm +%s" % d.get("frm"),
                        ack_ok and st_ok and n_err == 1))

        # ---------------- T8 发送队列灌满 ----------------
        before = link.stat()
        link.write(b"stat\r\n" * 100)       # 约 600 字节请求，回显远大于 1024 缓冲
        raw, fr = link.collect(2.0)
        n_stat_frames = sum(1 for t, _s, _p in fr if t == P.T_STAT)
        n_sample = sum(1 for t, _s, _p in fr if t == P.T_SAMPLE)
        after = link.stat()
        d = diff(before, after)
        w("[T8] 连发 100 条 stat：收到 STAT 帧 %d，SAMPLE %d，txd +%s，解析错误 %d"
          % (n_stat_frames, n_sample, d.get("txd"), link.parser.errors))
        ack_ok, st_ok, _ = alive(link)
        results.append(("T8 队列灌满", "STAT %d" % n_stat_frames,
                        "txd +%s" % d.get("txd"),
                        st_ok and link.parser.errors == 0))

        # ---------------- T9 帧错误后能否恢复 ----------------
        before = link.stat()
        link.reopen(9600)                   # 波特率不匹配 -> 设备收到 FE/NE
        link.write(b"\x55\xAA" * 16)
        link.collect(0.3)
        link.reopen(115200)
        time.sleep(0.3)
        ack_ok, st_ok, st = alive(link)
        after = st if st else link.stat()
        d = diff(before, after)
        w("[T9] 错误波特率制造 FE/NE 后：uart_err +%s，rxf +%s，恢复=%s/%s"
          % (d.get("uart"), d.get("rxf"), ack_ok, st_ok))
        results.append(("T9 FE 后恢复", "uart +%s" % d.get("uart"),
                        "rxf +%s" % d.get("rxf"), ack_ok and st_ok))

        # ---------------- 汇总 ----------------
        w()
        w("=" * 78)
        w("汇总")
        w("=" * 78)
        w("%-16s %-18s %-18s %s" % ("用例", "主要计数", "其它", "链路存活"))
        for name, a, b, ok in results:
            w("%-16s %-18s %-18s %s" % (name, a, b, "OK" if ok else "FAIL"))
        w()
        w("结束: %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    finally:
        link.close()

    io.open(OUT, "w", encoding="utf-8", newline="\n").write(out.getvalue())
    print("\n已写入 %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
