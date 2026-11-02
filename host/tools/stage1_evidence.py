#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""阶段一证据采集：ADC 恢复链路的可追溯原始记录。

与 adcrec.py 的区别：本脚本**只采集、只落盘**，不做 PASS/FAIL 判定，
目的是让报告里的每个数字都能在日志里找到对应行。

    python host/tools/stage1_evidence.py COM5

输出 docs/STAGE1_RAW.txt，包含：
  [0] 环境与固件信息
  [1] 基线计数器
  [2] 单次注入 x5：每次的注入时刻 / 回执时刻 / 恢复完成时刻 / 耗时
  [3] 限速验证：1 秒内连注 5 次，逐条记录注入与恢复时刻
  [4] 丢样核算：10 次注入前后 STAT 的 n 与 t，给出计算方法
  [5] 恢复后 30 秒采样率与 seq8 连续性
  [6] 结束计数器

前提：板上必须是 APP_DEBUG_INJECT=1 的固件（否则 adcinject 命令不存在）。
"""

import io
import os
import platform
import re
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.dirname(HERE)
ROOT = os.path.dirname(HOST)
sys.path.insert(0, HOST)

import protocol as P  # noqa: E402

OUT = os.path.join(ROOT, "docs", "STAGE1_RAW.txt")

MARK_ACK = b"OK inject"
MARK_REC = b"[rec] adc restarted"
MARK_RECFAIL = b"[rec] adc restart failed"
MARKS = ((MARK_REC, "rec"), (MARK_RECFAIL, "recfail"), (MARK_ACK, "ack"))

RE_STAT = re.compile(
    r"STAT t=(\d+)ms n=(\d+) drop=(\d+) adc=(\d+) uart=(\d+) rxf=(\d+) "
    r"frm=(\d+) chg=(\d+) cmd=(\d+)/(\d+) txd=(\d+)B"
)


class Link:
    """串口封套：接收时为文本标记打时间戳，并按帧类型解析。"""

    def __init__(self, port, baud=115200):
        self.port = port
        self.baud = baud
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.ser.reset_input_buffer()
        self.parser = P.FrameParser()
        self.buf = b""
        self.events = []       # [(kind, wall_time)]
        self.samples = []
        self.boots = 0
        self.reopens = 0
        self.notes = []

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _reopen(self):
        """串口句柄失效（PermissionError 13 / 设备重枚举）后尝试重连。"""
        try:
            self.ser.close()
        except Exception:
            pass
        for _ in range(40):                      # 最多等 20 s
            try:
                self.ser = serial.Serial(self.port, self.baud, timeout=0.05)
                self.ser.reset_input_buffer()
                self.parser = P.FrameParser()
                self.buf = b""
                self.reopens += 1
                self.notes.append((time.strftime("%H:%M:%S"), "重连成功"))
                return True
            except Exception:
                time.sleep(0.5)
        self.notes.append((time.strftime("%H:%M:%S"), "重连失败"))
        return False

    def reset_marks(self):
        self.buf = b""
        self.events = []

    def _scan_marks(self, now):
        """找文本标记 -> 记 (kind, 时刻) -> 消费掉。

        尾部保留 64 字节：标记被切在两个 chunk 之间时仍能匹配。
        """
        while True:
            hit = None
            for mark, kind in MARKS:
                i = self.buf.find(mark)
                if i >= 0 and (hit is None or i < hit[0]):
                    hit = (i, mark, kind)
            if hit is None:
                if len(self.buf) > 64:
                    self.buf = self.buf[-64:]
                return
            i, mark, kind = hit
            self.events.append((kind, now))
            self.buf = self.buf[i + len(mark):]

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                n = self.ser.in_waiting
            except Exception:
                # 句柄失效：重连，样本计数会被打断，日志里记一次
                if not self._reopen():
                    return
                n = 0
            if n == 0:
                time.sleep(0.002)
                continue
            try:
                chunk = self.ser.read(n)
            except Exception:
                if not self._reopen():
                    return
                continue
            now = time.time()
            self.buf += chunk
            self._scan_marks(now)
            for mtype, mseq, payload in self.parser.feed(chunk):
                if mtype == P.T_SAMPLE:
                    for o in P.decode_many([(mtype, mseq, payload)]):
                        if isinstance(o, P.Sample):
                            self.samples.append(o)
                elif mtype == P.T_BOOT:
                    self.boots += 1

    def wait_kind(self, kind, timeout):
        """等到某个标记出现，返回其时间戳；超时返回 None。"""
        end = time.time() + timeout
        while time.time() < end:
            for k, t in self.events:
                if k == kind:
                    return t
            self.pump(0.01)
        for k, t in self.events:
            if k == kind:
                return t
        return None

    def stat(self, timeout=1.2):
        self.buf = b""
        self.pump(0.15)
        self.ser.write(b"stat\r\n")
        end = time.time() + timeout
        acc = b""
        while time.time() < end:
            n = self.ser.in_waiting
            if n:
                acc += self.ser.read(n)
                m = RE_STAT.search(acc.decode("ascii", "replace"))
                if m:
                    g = [int(x) for x in m.groups()]
                    return dict(t=g[0], n=g[1], drop=g[2], adc=g[3], uart=g[4],
                                rxf=g[5], frm=g[6], chg=g[7], cmd_ok=g[8],
                                cmd_err=g[9], txd=g[10])
            else:
                time.sleep(0.002)
        return None


def fw_info():
    cfg = os.path.join(ROOT, "App", "Inc", "app_config.h")
    txt = io.open(cfg, encoding="utf-8").read()
    ver = re.search(r"#define\s+APP_FW_VERSION\s+(0x[0-9A-Fa-f]+)u?", txt)
    inj = re.search(r"#define\s+APP_DEBUG_INJECT\s+(\d+)u?", txt)
    return (ver.group(1) if ver else "?"), (inj.group(1) if inj else "?")


def main():
    if len(sys.argv) < 2:
        print("用法: python host/tools/stage1_evidence.py <COMx>")
        return 2
    port = sys.argv[1]

    out = io.StringIO()

    def w(line=""):
        out.write(line + "\n")
        print(line)

    ver, inj = fw_info()

    w("=" * 74)
    w("阶段一 ADC 恢复验证 — 原始采集记录")
    w("=" * 74)
    w("[0] 环境与固件")
    w("  采集开始       : %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    w("  主机           : %s" % platform.platform())
    w("  Python         : %s" % sys.version.split()[0])
    w("  pyserial       : %s" % getattr(serial, "__version__", "?"))
    w("  串口           : %s  115200 8N1  timeout=0.05" % port)
    w("  固件版本宏     : APP_FW_VERSION = %s" % ver)
    w("  注入开关       : APP_DEBUG_INJECT = %s（1 = 注入版）" % inj)
    w("  工程目录       : %s" % ROOT)
    w()
    if inj != "1":
        w("!! 板上固件不是注入版（开关 = %s），adcinject 命令不存在。" % inj)
        w("!! 请先用 APP_DEBUG_INJECT=1 编译烧录再跑本脚本。")
        io.open(OUT, "w", encoding="utf-8", newline="\n").write(out.getvalue())
        return 1

    link = Link(port)
    try:
        # ---------------- [1] 基线 ----------------
        w("[1] 基线计数器")
        base = link.stat()
        if base is None:
            w("  !! 读不到 STAT，检查板子是否在运行")
            return 1
        w("  t=%d ms  n=%d  drop=%d  adc_err=%d  uart_err=%d  rxf=%d  frm=%d"
          % (base["t"], base["n"], base["drop"], base["adc"],
             base["uart"], base["rxf"], base["frm"]))
        w()

        # ---------------- [2] 单次注入 x5 ----------------
        w("[2] 单次注入 x5（间隔 1.5 s，避开 1 s 限速窗口）")
        w("    时刻定义：t_send = 写 adcinject；t_ack = 收到 OK inject；")
        w("              t_rec = 收到 [rec] adc restarted")
        w("    恢复耗时 = t_rec - t_ack（含主循环剩余步骤 + DeInit/Init/校准/Start_DMA）")
        w()
        w("    #   注入(s)   回执(ms)  恢复(ms)  恢复耗时(ms)")
        recs = []
        t_phase = time.time()
        for i in range(5):
            link.reset_marks()
            link.pump(0.25)
            t_send = time.time()
            link.ser.write(b"adcinject\r\n")
            t_ack = link.wait_kind("ack", 1.0)
            t_rec = link.wait_kind("rec", 1.0)
            dt_ack = (t_ack - t_send) * 1000 if t_ack else float("nan")
            dt_rec = (t_rec - t_ack) * 1000 if (t_ack and t_rec) else float("nan")
            w("    %d  %8.3f  %8.1f  %8.1f  %12.1f"
              % (i + 1, t_send - t_phase, dt_ack, dt_rec, dt_rec))
            if t_ack and t_rec:
                recs.append(dt_rec)
            time.sleep(1.5)

        w()
        if recs:
            w("    恢复耗时: min %.1f / max %.1f / 平均 %.1f ms（n=%d）"
              % (min(recs), max(recs), sum(recs) / len(recs), len(recs)))
        else:
            w("    !! 未捕获到完整的一对「回执 + 恢复完成」")
        w()

        # ---------------- [3] 限速 ----------------
        w("[3] 限速验证：1 秒内连注 5 次（间隔 200 ms）")
        w("    期望：首次立即恢复，其后相邻两次恢复至少相隔 APP_ADC_RECOVER_INTERVAL_MS=1000")
        w("    中间几次注入只把标志置 1，若恢复已被限速则并入下一次")
        w()
        link.reset_marks()
        inj_ms = []
        t0 = time.time()
        for _ in range(5):
            link.ser.write(b"adcinject\r\n")
            inj_ms.append(round((time.time() - t0) * 1000))
            link.pump(0.2)
        link.pump(2.2)

        rec_ms = [round((t - t0) * 1000) for k, t in link.events if k == "rec"]
        fail_ms = [round((t - t0) * 1000) for k, t in link.events if k == "recfail"]
        w("    注入时刻(ms) : %s" % inj_ms)
        w("    恢复完成(ms) : %s" % rec_ms)
        w("    恢复失败(ms) : %s" % (fail_ms if fail_ms else "无"))
        w("    恢复次数     : %d（注入 5 次）" % len(rec_ms))
        if len(rec_ms) >= 2:
            gaps = [b - a for a, b in zip(rec_ms, rec_ms[1:])]
            w("    相邻恢复间隔 : %s ms（阈值 >= 950）" % gaps)
        w()

        # ---------------- [4] 丢样核算 ----------------
        w("[4] 丢样核算：10 次注入（间隔 2 s），比对累计样本数与设备运行时间")
        w("    方法：理论样本数 = dt_ms / 10（TIM3 100 Hz），与实际 n 增量相减")
        w("    不用 SAMPLE.t_ms、不用 seq8 —— 两者都看不到恢复空档（见报告 4.1）")
        w()
        a = link.stat()
        t_a = time.time()
        inj10 = []
        rec_before = sum(1 for k, _t in link.events if k == "rec")
        for _ in range(10):
            inj10.append(round((time.time() - t_a) * 1000))
            link.ser.write(b"adcinject\r\n")
            link.pump(2.0)
        t_b = time.time()
        b = link.stat()
        rec10 = sum(1 for k, _t in link.events if k == "rec") - rec_before
        if not a or not b:
            w("  !! STAT 读取失败")
        else:
            dt_ms = b["t"] - a["t"]
            dn = b["n"] - a["n"]
            exp = dt_ms / 10.0
            w("    之前 : t=%d ms  n=%d  drop=%d  adc_err=%d"
              % (a["t"], a["n"], a["drop"], a["adc"]))
            w("    之后 : t=%d ms  n=%d  drop=%d  adc_err=%d"
              % (b["t"], b["n"], b["drop"], b["adc"]))
            w("    设备时间增量 : %d ms" % dt_ms)
            w("    样本增量     : %d" % dn)
            w("    理论样本数   : %.1f" % exp)
            w("    差值         : %+d" % (dn - round(exp)))
            w("    drops 增量   : %d" % (b["drop"] - a["drop"]))
            w("    adc_err 增量 : %d（应等于注入次数 10）" % (b["adc"] - a["adc"]))
            w("    逐条注入时刻 : %s ms（相对第一次注入）" % inj10)
            w("    恢复完成次数 : %d（10 次注入应全部恢复，无失败）" % rec10)
            w("    墙钟耗时     : %.2f s（设备 %d ms，差值 = STAT 往返 + 2 s 收尾）"
              % (t_b - t_a, dt_ms))
        w()

        # ---------------- [5] 30 秒采样率 ----------------
        w("[5] 恢复后连续 30 s：采样率与 seq8 连续性")
        link.samples = []
        link.boots = 0
        t0 = time.time()
        link.pump(30.0)
        dt = time.time() - t0
        n = len(link.samples)
        gaps = 0
        for x, y in zip(link.samples, link.samples[1:]):
            if (x.seq8 + 1) & 0xFF != y.seq8:
                gaps += 1
        w("    墙钟时长     : %.3f s" % dt)
        w("    SAMPLE 帧数  : %d" % n)
        w("    实测采样率   : %.3f Hz（设计 100.000）" % (n / dt))
        w("    seq8 缺口    : %d" % gaps)
        w("    BOOT 帧      : %d（>0 说明中途复位过）" % link.boots)
        if link.samples:
            mv = [s.raw_mv for s in link.samples]
            w("    raw_mv       : min %d / mean %.1f / max %d"
              % (min(mv), sum(mv) / len(mv), max(mv)))
        w()

        # ---------------- [6] 结束计数器 ----------------
        w("[6] 结束计数器")
        end = link.stat()
        if end:
            w("  t=%d ms  n=%d  drop=%d  adc_err=%d  uart_err=%d  rxf=%d  frm=%d"
              % (end["t"], end["n"], end["drop"], end["adc"],
                 end["uart"], end["rxf"], end["frm"]))
            w("  全程增量：adc_err +%d  drop +%d  uart_err +%d  rxf +%d  frm +%d"
              % (end["adc"] - base["adc"], end["drop"] - base["drop"],
                 end["uart"] - base["uart"], end["rxf"] - base["rxf"],
                 end["frm"] - base["frm"]))
        w()
        w("采集结束: %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    except Exception as exc:                     # noqa: BLE001
        w()
        w("!! 采集中断: %s: %s" % (type(exc).__name__, exc))
        w("!! 以上为中断前已采集的数据；标 !! 的段落不完整，写报告时须注明")
    finally:
        w("串口重连次数: %d%s"
          % (link.reopens,
             ("  " + str(link.notes)) if link.notes else ""))
        link.close()
        io.open(OUT, "w", encoding="utf-8", newline="\n").write(out.getvalue())
        print("\n已写入 %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
