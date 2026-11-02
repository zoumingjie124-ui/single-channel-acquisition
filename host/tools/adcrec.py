# -*- coding: utf-8 -*-
"""阶段一：ADC 故障恢复链路验证（路径注入，非真实电气故障）。

前提：固件必须用 `APP_DEBUG_INJECT = 1` 编译，否则会回 "ERR unknown command"。
      测完请把开关改回 0 重新编译，Release 固件不包含这条命令。

验证思路
--------
注入只做一件事：调用 app_measure_on_error()，即"把错误标志置起来"。
之后由主循环自己走 DeInit -> Init -> 校准 -> Start_DMA 这条恢复链。
它验证的是**恢复流程能不能跑通并恢复采样**，不是"真实 DMA 硬件故障"。

判据（全部满足才算 PASS）
------------------------
1. 串口收到 `OK inject: adc_err=.. rec_ok=.. rec_fail=..`
2. 随后出现 `[rec] adc restarted (ok=.. fail=..)`
3. adc_err 与 rec_ok 各 +1（文本 STAT 的 adc= 字段）
4. 采样在恢复后继续：30 s 实测频率 99~101 Hz，drops 不变，seq8 无缺口
5. 恢复期间的"真实丢点"用相邻 SAMPLE 的 t_ms 差值衡量
   （seq8 只数已处理样本，恢复期没采到的样本不会产生缺口，不能拿它当丢点依据）

用法::

    python host/tools/adcrec.py COM5            # 单次注入 + 30 s 复核
    python host/tools/adcrec.py COM5 --burst    # 连注 5 次，验证 1 s 限速
"""

import os
import re
import sys
import time

import serial
from serial.tools import list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(PROJECT, "host"))

import protocol as P  # noqa: E402

RE_STAT = re.compile(r"STAT t=(\d+)ms n=(\d+) drop=(\d+) adc=(\d+) uart=(\d+) "
                     r"rxf=(\d+) frm=(\d+) chg=(\d+) cmd=(\d+)/(\d+) txd=(\d+)B")
RE_INJECT = re.compile(r"OK inject: adc_err=(\d+) rec_ok=(\d+) rec_fail=(\d+)")
RE_REC = re.compile(r"\[rec\] adc (restarted|restart failed)")
NORMAL_GAP_MS = 10.0          # 设计采样周期
RATE_SECONDS = 30.0


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    if not ports:
        print("未检测到任何串口，请先插板")
        raise SystemExit(2)
    return ports[0]


class Link:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.1)
        self.ser.reset_input_buffer()
        self.parser = P.FrameParser()

    def read(self, seconds, marks=None):
        """返回 (原始字节, Sample 列表)。

        marks 传入一个列表时，每检测到一次 "[rec] adc restarted" 就往里追加
        到达时刻 —— 用来量恢复间隔。必须在**接收的同时**打时间戳，
        否则串口缓冲会把几条日志合并成一批，时间戳全挤在一起。
        """
        deadline = time.time() + seconds
        raw = b""
        samples = []
        while time.time() < deadline:
            n = self.ser.in_waiting
            if n == 0:
                time.sleep(0.005)
                continue
            chunk = self.ser.read(n)
            raw += chunk
            if marks is not None:
                while b"[rec] adc restarted" in raw:
                    marks.append(time.time())
                    raw = raw.replace(b"[rec] adc restarted", b"", 1)
            for mtype, mseq, payload in self.parser.feed(chunk):
                if mtype == P.T_SAMPLE:
                    for o in P.decode_many([(mtype, mseq, payload)]):
                        if isinstance(o, P.Sample):
                            samples.append(o)
        return raw, samples

    def write(self, s):
        self.ser.write(s.encode() if isinstance(s, str) else s)

    def stat(self):
        self.read(0.2)
        self.write(b"stat\r\n")
        raw, _ = self.read(0.8)
        i = raw.find(b"STAT t=")
        if i < 0:
            return None
        j = raw.find(b"\r\n", i)
        m = RE_STAT.search(raw[i:j if j > 0 else len(raw)].decode("ascii", "replace"))
        if not m:
            return None
        g = m.groups()
        return {"n": int(g[1]), "drop": int(g[2]), "adc": int(g[3]),
                "uart": int(g[4]), "rxf": int(g[5]), "frm": int(g[6])}

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


def gaps_ms(samples):
    return [b.t_ms - a.t_ms for a, b in zip(samples, samples[1:])]


def seq8_gap(samples):
    gap = 0
    for a, b in zip(samples, samples[1:]):
        d = (b.seq8 - a.seq8) & 0xFF
        if d != 1:
            gap += (d - 1) & 0xFF
    return gap


def main():
    port = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else pick_port()
    burst = "--burst" in sys.argv

    link = Link(port)
    print("串口: %s @115200  模式: %s" % (port, "连注 5 次" if burst else "单次注入"))

    before = link.stat()
    if before is None:
        print("拿不到文本 STAT —— 检查板子是否在跑、串口是否选对")
        link.close()
        return 1
    print("基线: adc_err=%d drop=%d n=%d" % (before["adc"], before["drop"], before["n"]))

    # ---- 注入 ----
    raw_inj = b""
    rec_marks = []
    if burst:
        # 1 秒内连注 5 次：期望"首次立即恢复 + 之后 1 秒一次"，共 1~2 次恢复
        for _ in range(5):
            link.write(b"adcinject\r\n")
            r, _ = link.read(0.2, rec_marks)
            raw_inj += r
        r, _ = link.read(2.0, rec_marks)
        raw_inj += r
    else:
        link.write(b"adcinject\r\n")
        raw_inj, _ = link.read(2.5, rec_marks)

    if b"ERR unknown command" in raw_inj and b"OK inject" not in raw_inj:
        print("\n固件里没有 adcinject —— 请把 APP_DEBUG_INJECT 改成 1 重新编译烧录")
        link.close()
        return 1

    n_inj = raw_inj.count(b"OK inject")
    last_inj = RE_INJECT.findall(raw_inj.decode("ascii", "replace"))
    rec_lines = RE_REC.findall(raw_inj.decode("ascii", "replace"))
    n_ok = sum(1 for x in rec_lines if x == "restarted")
    n_fail = sum(1 for x in rec_lines if x == "restart failed")

    # ---- 恢复后的采样与真丢点 ----
    raw_after, samples = link.read(RATE_SECONDS)
    after = link.stat()
    link.close()

    g = gaps_ms(samples)
    normal = [x for x in g if 5 <= x <= 20]
    big = max(g) if g else 0
    lost = int(round((big - NORMAL_GAP_MS) / NORMAL_GAP_MS)) if big > NORMAL_GAP_MS * 1.5 else 0
    rate = len(samples) / RATE_SECONDS
    s8 = seq8_gap(samples)

    print("\n===== 结果 =====")
    print("注入命令响应  : %d 次 OK inject" % n_inj)
    if last_inj:
        print("固件回显      : 最后一次 adc_err=%s rec_ok=%s rec_fail=%s" % last_inj[-1])
    print("恢复日志      : restarted=%d  failed=%d" % (n_ok, n_fail))
    print("adc_err 增量  : %+d" % (after["adc"] - before["adc"]))
    print("drops   增量  : %+d" % (after["drop"] - before["drop"]))
    print("采样率(30s)   : %.2f Hz  (%d 帧)" % (rate, len(samples)))
    print("seq8 缺口     : %d" % s8)
    print("最大 t_ms 间隔: %.0f ms  -> 恢复期间约丢 %d 个采样周期" % (big, lost))
    if normal:
        print("常规间隔      : %.0f~%.0f ms（%d 个样本）" % (min(normal), max(normal), len(normal)))

    ok = True
    expect_ok = 1 if not burst else 1          # 连注时限速会让部分请求合并
    if burst and rec_marks:
        gaps = [(b - a) * 1000 for a, b in zip(rec_marks, rec_marks[1:])]
        if gaps:
            print("恢复间隔      : %s ms（阈值 ≥950）" % [round(x) for x in gaps])
    if n_inj != (1 if not burst else 5):
        print("FAIL: 注入响应条数不符（期望 %d，实得 %d）" % (1 if not burst else 5, n_inj))
        ok = False
    if n_ok < expect_ok:
        print("FAIL: 没有看到恢复成功日志")
        ok = False
    if after["adc"] - before["adc"] < 1:
        print("FAIL: adc_err 没有增加")
        ok = False
    if burst:
        if n_ok > 2:
            print("FAIL: 1 秒内连注 5 次却恢复了 %d 次 —— 1 s 限速未生效" % n_ok)
            ok = False
        if len(rec_marks) >= 2:
            gaps = [(b - a) * 1000 for a, b in zip(rec_marks, rec_marks[1:])]
            if min(gaps) < 950:
                print("FAIL: 两次恢复间隔仅 %.0f ms，小于 1 s 限速" % min(gaps))
                ok = False
        if n_ok == 0:
            print("FAIL: 连注后没有任何恢复")
            ok = False
    if not (99.0 <= rate <= 101.0):
        print("FAIL: 恢复后采样率 %.2f Hz，偏离 100 Hz" % rate)
        ok = False
    if s8 != 0:
        print("FAIL: seq8 出现缺口 %d" % s8)
        ok = False
    if after["drop"] != before["drop"]:
        print("FAIL: drops 增加 %d，恢复过程丢了环形缓冲" % (after["drop"] - before["drop"]))
        ok = False

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
