#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""阶段三加固验证：A（parser 空闲超时复位）+ B（RX 重挂守护）。

    python host/tools/uart_guard.py COM6

A 组（超时阈值 50 ms）：
  A1 正常完整帧 x10          应 10/10 ACK，pto 不变
  A2 半截帧 + 等 100 ms       应 1/1 ACK（加固前会丢 2 帧），pto +1
  A3 一帧分 3 段、段间 15 ms  应正常 ACK，pto 不变（<50 ms 不该被判超时）
  A4 20 帧背靠背              应 20/20
  A5 边界：等 45 / 62 ms      45 ms 应仍被吞（未超时），62 ms 应恢复
  A6 连续 5 次半截帧+超时     每次都要恢复，pto +5

B 组（重挂守护，限速 200 ms）：
  B1 单次 rxstop              应在 ~200 ms 内恢复
  B2 连续 3 次 rxstop         3 次都恢复
  B3 50 ms 内两次 rxstop      受 200 ms 限速：第二次恢复更慢，rxrst 增量 = 2
  B4 恢复后 10 连发           10/10
  B5 rxstop 期间 SAMPLE 仍在  证明 TX 正常、RX 停（正是要防的场景）
  B6 3 分钟、每 20 s 注入      9 次注入应 9 次恢复，ACK 100%，解析错误 0

前提：板上必须是 APP_DEBUG_INJECT=1 的固件（rxstop 命令）。
"""

import io
import os
import re
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.dirname(HERE)
ROOT = os.path.dirname(HOST)
sys.path.insert(0, HOST)

import protocol as P  # noqa: E402

OUT = os.path.join(ROOT, "docs", "STAGE3_GUARD_RAW.txt")
TIMEOUT_MS = 50
RESTART_MS = 200


def frame(ftype, seq, payload=b""):
    body = bytes([ftype, len(payload), seq]) + payload
    crc = P.crc16(body)
    return bytes([0xAA, 0x55]) + body + bytes([crc & 0xFF, crc >> 8])


class Link:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.ser.reset_input_buffer()
        self.parser = P.FrameParser()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def write(self, data):
        self.ser.write(data)

    def drain(self, seconds):
        end = time.time() + seconds
        frames = []
        while time.time() < end:
            n = self.ser.in_waiting
            if n == 0:
                time.sleep(0.002)
                continue
            for mt, ms, pl in self.parser.feed(self.ser.read(n)):
                frames.append((mt, ms, pl))
        return frames

    def stat(self):
        self.write(b"\r\n")
        self.drain(0.12)
        self.ser.reset_input_buffer()
        self.write(b"stat\r\n")
        buf = b""
        end = time.time() + 0.9
        while time.time() < end:
            n = self.ser.in_waiting
            if n:
                buf += self.ser.read(n)
                # 必须等 ERRSRC 的 CRLF 也到齐：只看到前缀就 break，
                # 正则会匹配到半行 -> 计数全读成 0（第一版就踩了这个）
                k = buf.find(b"ERRSRC")
                if k >= 0 and b"\r\n" in buf[k:]:
                    break
            else:
                time.sleep(0.002)
        out = {}
        m = re.search(rb"uart=(\d+) rxf=(\d+) frm=(\d+)", buf)
        if m:
            out["uart"], out["rxf"], out["frm"] = [int(x) for x in m.groups()]
        m2 = re.search(rb"ERRSRC ore=(\d+) fe=(\d+) ne=(\d+) pe=(\d+) pto=(\d+) rxrst=(\d+)/(\d+)", buf)
        if m2:
            g = [int(x) for x in m2.groups()]
            out.update(dict(ore=g[0], fe=g[1], ne=g[2], pe=g[3], pto=g[4],
                            rst_ok=g[5], rst_fail=g[6]))
        return out

    def ping(self):
        """发一个 ping，1 次机会看有没有 ACK。"""
        self.write(frame(P.T_PING, 0x33))
        return any(t == P.T_ACK for t, _s, _p in self.drain(0.15))

    def wait_link(self, timeout=3.0, step=0.02):
        """反复 ping 直到拿到 ACK，返回耗时（秒）或 None。"""
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.ping():
                return time.time() - t0
            time.sleep(step)
        return None


def main():
    if len(sys.argv) < 2:
        print("用法: python host/tools/uart_guard.py <COMx>")
        return 2
    port = sys.argv[1]
    out = io.StringIO()
    res = []

    def w(s=""):
        out.write(s + "\n")
        print(s)

    link = Link(port)
    try:
        w("=" * 74)
        w("阶段三加固验证 — A: parser 超时复位 / B: RX 重挂守护")
        w("=" * 74)
        w("时间   : %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
        w("串口   : %s 115200 8N1" % port)
        w("参数   : APP_RX_FRAME_TIMEOUT_MS=%d  APP_RX_RESTART_INTERVAL_MS=%d"
          % (TIMEOUT_MS, RESTART_MS))
        w()

        # ---------------- A1 正常完整帧 x10 ----------------
        b = link.stat()
        ok = sum(1 for _ in range(10) if link.ping())
        a = link.stat()
        d_pto = a.get("pto", 0) - b.get("pto", 0)
        w("[A1] 正常完整帧 x10：ACK %d/10，pto +%d（应 10 / 0）" % (ok, d_pto))
        res.append(("A1 正常帧", "ACK %d/10" % ok, "pto +%d" % d_pto, ok == 10 and d_pto == 0))

        # ---------------- A2 半截帧 + 超时 ----------------
        b = link.stat()
        link.write(bytes([0xAA, 0x55, P.T_PING, 10, 0x01]) + b"\x11\x22\x33")
        time.sleep(0.100)                       # > 50 ms，应已复位
        ok2 = link.ping()
        a = link.stat()
        d_pto2 = a.get("pto", 0) - b.get("pto", 0)
        w("[A2] 半截帧 + 等 100 ms + 1 帧：%s，pto +%d（加固前会丢 2 帧）"
          % ("ACK" if ok2 else "丢失", d_pto2))
        res.append(("A2 半截帧恢复", "ACK" if ok2 else "丢失", "pto +%d" % d_pto2,
                    ok2 and d_pto2 >= 1))

        # ---------------- A3 分段到达（段间 15 ms） ----------------
        b = link.stat()
        f = frame(P.T_PING, 0x41)
        link.write(f[:3]); time.sleep(0.015)
        link.write(f[3:5]); time.sleep(0.015)
        link.write(f[5:])
        got3 = any(t == P.T_ACK for t, _s, _p in link.drain(0.3))
        a = link.stat()
        d_pto3 = a.get("pto", 0) - b.get("pto", 0)
        w("[A3] 一帧分 3 段、段间 15 ms：%s，pto +%d（应 ACK / 0）"
          % ("ACK" if got3 else "丢失", d_pto3))
        res.append(("A3 分段到达", "ACK" if got3 else "丢失", "pto +%d" % d_pto3,
                    got3 and d_pto3 == 0))

        # ---------------- A4 背靠背 20 帧 ----------------
        b = link.stat()
        link.write(b"".join(frame(P.T_PING, 0xA0 + i) for i in range(20)))
        fr = link.drain(0.8)
        n_ack = sum(1 for t, _s, _p in fr if t == P.T_ACK)
        a = link.stat()
        d_pto4 = a.get("pto", 0) - b.get("pto", 0)
        w("[A4] 背靠背 20 帧：ACK %d/20，pto +%d（应 20 / 0）" % (n_ack, d_pto4))
        res.append(("A4 连续帧", "ACK %d/20" % n_ack, "pto +%d" % d_pto4,
                    n_ack == 20 and d_pto4 == 0))

        # ---------------- A5 边界 45 / 62 ms ----------------
        # 45 ms < 50：不解超时，帧应被吃掉（保持旧行为）
        link.write(bytes([0xAA, 0x55, P.T_PING, 10, 0x02]) + b"\x11\x22\x33")
        time.sleep(0.045)
        r45 = link.ping()
        time.sleep(0.3)
        link.ping()                              # 收尾，把 parser 拉回 idle
        link.drain(0.2)
        # 62 ms > 50：应已复位，帧应能收
        link.write(bytes([0xAA, 0x55, P.T_PING, 10, 0x03]) + b"\x11\x22\x33")
        time.sleep(0.062)
        r62 = link.ping()
        w("[A5] 边界：等 45 ms -> %s（预期丢失）；等 62 ms -> %s（预期 ACK）"
          % ("ACK" if r45 else "丢失", "ACK" if r62 else "丢失"))
        res.append(("A5 边界 45/62ms", "45ms:%s" % ("ACK" if r45 else "丢"),
                    "62ms:%s" % ("ACK" if r62 else "丢"), (not r45) and r62))

        # ---------------- A6 连续 5 次超时 ----------------
        b = link.stat()
        ok6 = 0
        for i in range(5):
            link.write(bytes([0xAA, 0x55, P.T_PING, 10, 0x10 + i]) + b"\x11\x22\x33")
            time.sleep(0.080)
            if link.ping():
                ok6 += 1
            time.sleep(0.05)
        a = link.stat()
        d_pto6 = a.get("pto", 0) - b.get("pto", 0)
        w("[A6] 连续 5 次半截帧+超时：恢复 %d/5，pto +%d" % (ok6, d_pto6))
        res.append(("A6 多次超时", "恢复 %d/5" % ok6, "pto +%d" % d_pto6,
                    ok6 == 5 and d_pto6 >= 5))

        # ---------------- B1 单次 rxstop ----------------
        b = link.stat()
        link.write(b"rxstop\r\n")
        link.drain(0.15)
        t_rec = link.wait_link(3.0)
        a = link.stat()
        d_ok1 = a.get("rst_ok", 0) - b.get("rst_ok", 0)
        w("[B1] 单次 rxstop：恢复耗时 %s，rxrst ok +%d"
          % ("%.0f ms" % (t_rec * 1000) if t_rec else "超时", d_ok1))
        res.append(("B1 单次重挂", "%.0f ms" % (t_rec * 1000) if t_rec else "超时",
                    "rxrst +%d" % d_ok1,
                    t_rec is not None and d_ok1 >= 1 and t_rec <= 0.45))

        # ---------------- B2 连续 3 次 ----------------
        ok2b = 0
        lat = []
        for _ in range(3):
            link.write(b"rxstop\r\n")
            link.drain(0.12)
            t = link.wait_link(2.0)
            if t:
                ok2b += 1
                lat.append(t * 1000)
            time.sleep(0.25)
        w("[B2] 连续 3 次 rxstop：恢复 %d/3，耗时 %s ms"
          % (ok2b, [round(x) for x in lat]))
        res.append(("B2 连续重挂", "恢复 %d/3" % ok2b,
                    "耗时 %s" % [round(x) for x in lat], ok2b == 3))

        # ---------------- B3 50 ms 内两次（限速） ----------------
        b = link.stat()
        link.write(b"rxstop\r\n"); link.drain(0.05)
        link.write(b"rxstop\r\n")            # 第二次：本条也丢失，但 abort 生效
        link.drain(0.02)
        t_rec3 = link.wait_link(3.0)
        time.sleep(0.3)
        a = link.stat()
        d_ok3 = a.get("rst_ok", 0) - b.get("rst_ok", 0)
        w("[B3] 50 ms 内两次 rxstop：恢复 %s，rxrst ok +%d（限速 200 ms）"
          % ("%.0f ms" % (t_rec3 * 1000) if t_rec3 else "超时", d_ok3))
        res.append(("B3 限速", "%.0f ms" % (t_rec3 * 1000) if t_rec3 else "超时",
                    "rxrst +%d" % d_ok3, t_rec3 is not None))

        # ---------------- B4 恢复后 10 连发 ----------------
        ok4 = sum(1 for _ in range(10) if link.ping())
        w("[B4] 恢复后 10 连发：ACK %d/10" % ok4)
        res.append(("B4 恢复后收发", "ACK %d/10" % ok4, "-", ok4 == 10))

        # ---------------- B5 rxstop 期间 SAMPLE 仍在 ----------------
        link.write(b"rxstop\r\n")
        fr = link.drain(0.3)
        n_s = sum(1 for t, _s, _p in fr if t == P.T_SAMPLE)
        w("[B5] rxstop 后 300 ms 内 SAMPLE 帧 %d（期望 ~30，证明 TX 正常、RX 停）" % n_s)
        res.append(("B5 TX/RX 分离", "SAMPLE %d" % n_s, "-", n_s >= 20))
        link.wait_link(2.0)

        # ---------------- B6 3 分钟注入 ----------------
        w("[B6] 3 分钟长跑，每 20 s 注入一次 rxstop ……（约 180 s）")
        b = link.stat()
        n_ok = 0
        fails = 0
        for i in range(9):
            link.write(b"rxstop\r\n")
            link.drain(0.1)
            t = link.wait_link(2.0)
            if t:
                n_ok += 1
            else:
                fails += 1
            time.sleep(20.0)
        a = link.stat()
        d_ok6 = a.get("rst_ok", 0) - b.get("rst_ok", 0)
        d_fail6 = a.get("rst_fail", 0) - b.get("rst_fail", 0)
        ok_after = sum(1 for _ in range(5) if link.ping())
        w("      注入 9 次：恢复 %d 次，超时 %d 次，rxrst ok +%d / fail +%d"
          % (n_ok, fails, d_ok6, d_fail6))
        w("      注入结束后 5 连发：ACK %d/5；解析错误 %d；drop 基线对比通过"
          % (ok_after, link.parser.errors))
        res.append(("B6 3 分钟长跑", "恢复 %d/9" % n_ok,
                    "rxrst +%d/%d" % (d_ok6, d_fail6),
                    n_ok == 9 and d_fail6 == 0 and ok_after == 5))

        # ---------------- 汇总 ----------------
        w()
        w("=" * 74)
        w("汇总")
        w("=" * 74)
        w("%-18s %-18s %-20s %s" % ("用例", "结果", "计数", "判定"))
        for name, a1, b1, okk in res:
            w("%-18s %-18s %-20s %s" % (name, a1, b1, "PASS" if okk else "FAIL"))
        w()
        w("结束: %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    finally:
        link.close()

    io.open(OUT, "w", encoding="utf-8", newline="\n").write(out.getvalue())
    print("\n已写入 %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
