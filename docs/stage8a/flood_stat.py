# -*- coding: utf-8 -*-
"""Stage 8A：用"命令放大"把 TX 环灌满，观察 FAULT 事件是否被合并为 1 帧、rep 是否拉平。

单进程 / 单串口会话：本脚本同时负责发灌命令与收帧解析，**不得**再并行打开同一串口。

用法（在仓库根目录）：
    python docs/stage8a/flood_stat.py --port COM5 --flood-sec 4.5 --period-ms 10 \
        --wait-flag docs/stage8a/go.flag --timeout 90

人工握手：脚本做完 reset / diag on 后会打印提示并轮询 go.flag（超时 --timeout 秒）。
收到后：3 声短鸣倒计时 → 长鸣 = 开始灌命令（此时请立刻拧电位器，拧开 >100 mV 并保持 3 s）
→ 再一声长鸣 = 停灌。

不修改固件、不写入设备配置；只发 `stat` / `reset` / `diag on` 三条既有文本命令。
"""
import argparse
import os
import re
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(PROJ, "host"))
import protocol as P  # noqa: E402

RE_STAT = re.compile(rb"STAT t=(\d+)ms n=(\d+) drop=(\d+) adc=(\d+) uart=(\d+) rxf=(\d+) "
                     rb"frm=(\d+) chg=(\d+) cmd=(\d+)/(\d+) txd=(\d+)B")
RE_DIAG = re.compile(rb"DIAG en=(\d+) state=(\d+) conf=(\d+) span=(\d+) blk=(\d+) "
                     rb"unev=(\d+) mask=(\d+) rep=(\d+)")


def wall(ts=None):
    t = time.time() if ts is None else ts
    return time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % (int(t * 1000) % 1000)


def beep(freq=880, ms=180):
    try:
        import winsound
        winsound.Beep(freq, ms)
    except Exception:
        pass


class Rec(object):
    def __init__(self, path):
        self.f = open(path, "w", encoding="utf-8", newline="\n")

    def __call__(self, line=""):
        self.f.write(line + "\n")
        self.f.flush()
        print(line, flush=True)

    def close(self):
        self.f.close()


def read_for(ser, seconds, parser, raw_buf, frames):
    """在 seconds 内持续读，喂解析器；返回收到的原始字节数。"""
    end = time.time() + seconds
    n = 0
    while time.time() < end:
        chunk = ser.read(ser.in_waiting or 1)
        if not chunk:
            continue
        n += len(chunk)
        raw_buf.extend(chunk)
        for mt, ms, payload in parser.feed(chunk):
            frames.append((time.time(), mt, ms, payload))
    return n


def read_until_stat(ser, parser, raw_buf, frames, timeout=1.5):
    """读到出现 STAT 文本行为止（最多 timeout 秒）。"""
    end = time.time() + timeout
    got = False
    while time.time() < end:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            raw_buf.extend(chunk)
            for mt, ms, payload in parser.feed(chunk):
                frames.append((time.time(), mt, ms, payload))
            if RE_STAT.search(bytes(raw_buf)):
                got = True
                break
    return got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--flood-sec", type=float, default=4.5)
    ap.add_argument("--period-ms", type=float, default=10.0)
    ap.add_argument("--wait-flag", default=os.path.join(HERE, "go.flag"))
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--countdown", type=int, default=3,
                    help="收到 go.flag 后的倒计时秒数（留人工就位时间）")
    ap.add_argument("--out", default=os.path.join(PROJ, "docs", "STAGE8A_TX_FULL_RAW.txt"))
    a = ap.parse_args()

    log = Rec(a.out)
    log("=== Stage 8A：TX 环满载（命令放大）原始记录 ===")
    log("开始时间 : %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    log("端口     : %s @ %d 8N1" % (a.port, a.baud))
    log("灌包参数 : 每条 stat\\r，间隔 %.0f ms，持续 %.2f s（约 %d 条）"
        % (a.period_ms, a.flood_sec, int(a.flood_sec * 1000 / a.period_ms)))
    log("握手     : 轮询 %s，超时 %.0f s；收到后倒计时 %d s" 
        % (os.path.basename(a.wait_flag), a.timeout, a.countdown))
    log("复现命令 : python docs/stage8a/flood_stat.py --port %s --flood-sec %.1f "
        "--period-ms %.0f --wait-flag docs/stage8a/go.flag --timeout %.0f --countdown %d"
        % (a.port, a.flood_sec, a.period_ms, a.timeout, a.countdown))
    log("说明     : 仅发 stat / reset / diag on 三条既有文本命令；不改固件、不改配置")
    log("")

    ser = serial.Serial(a.port, a.baud, timeout=0.05)
    parser = P.FrameParser()
    raw = bytearray()
    frames = []
    try:
        # ---------- 步 0：reset + 判据基线 ----------
        log("[步0] reset @ %s" % wall())
        ser.reset_input_buffer()
        ser.write(b"reset\r")
        time.sleep(1.2)
        read_for(ser, 0.3, parser, raw, frames)
        raw.clear()
        log("[步0] 基线（reset 后）:")
        ser.write(b"stat\r")
        read_until_stat(ser, parser, raw, frames)
        txt = bytes(raw)
        m = RE_STAT.search(txt)
        d = RE_DIAG.search(txt)
        base = {}
        if m:
            g = [int(x) for x in m.groups()]
            base = dict(t=g[0], n=g[1], drop=g[2], adc=g[3], uart=g[4], rxf=g[5],
                        frm=g[6], chg=g[7], cmd_ok=g[8], cmd_err=g[9], txd=g[10])
            log("   STAT   " + m.group(0).decode("ascii", "replace"))
        else:
            log("   STAT   <未收到>")
        for mm in re.finditer(rb"ERRSRC[^\r\n]*", txt):
            log("   ERRSRC " + mm.group(0).decode("ascii", "replace"))
        if d:
            gg = [int(x) for x in d.groups()]
            base.update(dict(en=gg[0], state=gg[1], conf=gg[2], span=gg[3], blk=gg[4],
                             unev=gg[5], mask=gg[6], rep=gg[7]))
            log("   DIAG   " + d.group(0).decode("ascii", "replace"))
        else:
            log("   DIAG   <未收到>")
        log("")

        # ---------- 步 1：diag on，等确认 ----------
        if base.get("en", 0) != 1:
            log("[步1] diag on @ %s" % wall())
            raw.clear()
            ser.write(b"diag on\r")
            time.sleep(1.0)
            read_for(ser, 0.4, parser, raw, frames)
            for mm in re.finditer(rb"(OK|ERR)[^\r\n]*", bytes(raw)):
                log("   " + mm.group(0).decode("ascii", "replace"))
        else:
            log("[步1] diag 已为 en=1，不重复下发")
        log("[步1] 等待卡死确认（最多 6 s）…")
        t_end = time.time() + 6.0
        while time.time() < t_end:
            raw.clear()
            ser.write(b"stat\r")
            time.sleep(0.9)
            read_for(ser, 0.2, parser, raw, frames)
            d = RE_DIAG.search(bytes(raw))
            if d and int(d.group(7)) == 1 and int(d.group(8)) == 1:
                log("   " + d.group(0).decode("ascii", "replace"))
                break
        fault_n = sum(1 for f in frames if f[1] == P.T_FAULT)
        log("[步1] 此时已收 FAULT 帧数 = %d（期望 ≥1：请保持电位器卡死不动）" % fault_n)
        log("")

        # ---------- 人工握手 ----------
        if os.path.exists(a.wait_flag):
            os.remove(a.wait_flag)
            log("[握手] 发现旧的 go.flag，已删除（避免误触发）")
        log("[握手] >>> 请把电位器拧到卡死位置并按住 <<<")
        log("[握手] >>> 准备好后创建文件：%s <<<" % a.wait_flag)
        log("[握手] 等待中（超时 %.0f s）…" % a.timeout)
        t_wait = time.time()
        while not os.path.exists(a.wait_flag):
            if time.time() - t_wait > a.timeout:
                log("[握手] >>> ABORT：%.0f s 内未收到 go.flag，本轮不产生判定 <<<" % a.timeout)
                return 2
            time.sleep(0.2)
        log("[握手] 收到 go.flag @ %s" % wall())
        log("")

        # ---------- 倒计时（同时排空积压，避免把倒计时数据算进窗口） ----------
        raw_pre = bytearray()
        frames_pre = []
        for k in range(a.countdown, 0, -1):
            log("[倒计时] %d" % k)
            beep(660, 100)
            chunk = ser.read(ser.in_waiting or 0)
            if chunk:
                raw_pre.extend(chunk)
                for mt, msx, payload in parser.feed(chunk):
                    frames_pre.append((time.time(), mt, msx, payload))
            time.sleep(0.85)
        if frames_pre:
            pre_by = {}
            for _, mt, _, _ in frames_pre:
                pre_by[mt] = pre_by.get(mt, 0) + 1
            log("[倒计时] 倒计时期间排空帧（不计入窗口）: " + ", ".join(
                "%s=%d" % (P.TYPE_NAMES.get(k, hex(k)), v) for k, v in sorted(pre_by.items())))

        # ---------- 锚点 1 ----------
        raw.clear()
        u1 = w1 = None
        for attempt in range(3):
            ser.write(b"stat\r")
            read_until_stat(ser, parser, raw, frames)
            m = RE_STAT.search(bytes(raw))
            if m:
                u1, w1 = int(m.group(1)), time.time()
                break
            log("[锚点1] 第 %d 次未拿到 STAT，重试…" % (attempt + 1))
        log("[锚点1] STAT t=%s ms @ %s" % (u1, wall(w1)))

        # ---------- 步 2/3：灌命令（三声上行短鸣 = 立刻拧） ----------
        beep(880, 120)
        time.sleep(0.12)
        beep(1100, 120)
        time.sleep(0.12)
        beep(1500, 300)
        log("[步2] >>> 开始灌命令，请立刻拧开电位器（>100 mV）并保持 3 s <<<")
        t_start = time.time()
        n_sent = 0
        raw_win = bytearray()
        frames_win = []
        while time.time() - t_start < a.flood_sec:
            ser.write(b"stat\r")
            n_sent += 1
            chunk = ser.read(ser.in_waiting or 0)
            if chunk:
                raw_win.extend(chunk)
                raw.extend(chunk)
                for mt, msx, payload in parser.feed(chunk):
                    frames_win.append((time.time(), mt, msx, payload))
            time.sleep(a.period_ms / 1000.0)
        t_stop = time.time()
        beep(500, 700)
        log("[步2] 停灌 @ %s（发送 %d 条，窗口 %.2f s）—— 一声低音长鸣 = 停灌" % (wall(t_stop), n_sent, t_stop - t_start))

        by_type_win = {}
        for _, mt, _, _ in frames_win:
            by_type_win[mt] = by_type_win.get(mt, 0) + 1
        log("[步2] 窗口内收到二进制帧: " + ", ".join(
            "%s=%d" % (P.TYPE_NAMES.get(k, hex(k)), v) for k, v in sorted(by_type_win.items())))
        fault_win = [f for f in frames_win if f[1] == P.T_FAULT]
        log("[步2] 窗口内 FAULT 帧数 = %d（A-1 期望 0）" % len(fault_win))
        leaked = [ln for ln in raw_win.split(b"\r\n")
                  if ln.startswith(b"STAT ") or ln.startswith(b"DIAG ") or ln.startswith(b"ERRSRC ")]
        log("[步2] 窗口内漏出的文本行 %d 条：" % len(leaked))
        for ln in leaked:
            log("      " + ln.decode("ascii", "replace")[:120])
        if not leaked:
            log("      <无>")
        log("")

        # ---------- 步 4：停灌后密集采样，定位 mask→0 的时刻 ----------
        log("[步4] 停灌后密集读取 8 s（每 0.4 s 一次 stat），用于界定 mask 何时变 0 …")
        series = []
        t4 = time.time()
        while time.time() - t4 < 8.0:
            ser.write(b"stat\r")
            buf = bytearray()
            endp = time.time() + 0.40
            while time.time() < endp:
                c = ser.read(ser.in_waiting or 1)
                if c:
                    buf.extend(c)
                    for mt, msx, payload in parser.feed(c):
                        frames.append((time.time(), mt, msx, payload))
            md = RE_DIAG.search(bytes(buf))
            mst = RE_STAT.search(bytes(buf))
            if md and mst:
                series.append((time.time(), int(mst.group(1)), int(md.group(2)),
                               int(md.group(7)), int(md.group(8)), int(mst.group(11))))
        log("[步4] 收到 %d 组 DIAG/STAT 采样" % len(series))
        prev = None
        for (tw, tdev, st, mask, rep, txd) in series:
            if prev is None or mask != prev:
                log("      %s  t=%dms state=%d mask=%d rep=%d txd=%dB %s"
                    % (wall(tw), tdev, st, mask, rep, txd,
                       "(mask 变化)" if prev is not None else "(首个采样)"))
                prev = mask
        if series:
            log("      %s  t=%dms state=%d mask=%d rep=%d txd=%dB (末次采样)"
                % (wall(series[-1][0]), series[-1][1], series[-1][2],
                   series[-1][3], series[-1][4], series[-1][5]))

        # ---------- 步 6：请他拧回卡死位置，等重新确认 ----------
        log("")
        log("[步6] 请把电位器拧回卡死位置并保持 —— 两声低音 = 现在开始统计 6 s")
        beep(500, 350)
        time.sleep(0.25)
        beep(500, 350)
        read_for(ser, 6.0, parser, raw, frames)
        raw.clear()
        ser.write(b"stat\r")
        read_until_stat(ser, parser, raw, frames)
        d3 = RE_DIAG.search(bytes(raw))
        if d3:
            log("[步6] DIAG   " + d3.group(0).decode("ascii", "replace"))

        # ---------- 汇总 ----------
        log("")
        log("=== 汇总 ===")
        # 设备时间映射：以两个锚点线性插值，把 t_stop 换算成设备 ms
        t_stop_dev = None
        if u1 and w1:
            # 用锚点1 + 停灌后首个采样点建立映射（密度更高，误差更小）
            if series:
                tw0, tdev0 = series[0][0], series[0][1]
                rate = (tdev0 - u1) / (tw0 - w1) if tw0 > w1 else 1.0
                t_stop_dev = u1 + (t_stop - w1) * rate
                log("设备时间映射(锚点1→停灌后首采样): rate=%.4f ms/ms，t_stop(设备) ≈ %.0f ms"
                    % (rate, t_stop_dev))
        # mask 首次为 0 的时刻 → 状态迁移上界 → 边沿上界
        first0 = next((x for x in series if x[3] == 0), None)
        if first0 and t_stop_dev:
            log("mask 首次为 0 @ %s（设备 t=%d ms），距停灌 %+.0f ms"
                % (wall(first0[0]), first0[1], first0[1] - t_stop_dev))
            log("ACTIVE→CLEAR 需 2 个连续干净 1 s 块 ⇒ 边沿 ≤ mask0 - 1000 ms = %+.0f ms（相对停灌）"
                % (first0[1] - 1000 - t_stop_dev))
        elif t_stop_dev:
            log("**未在 8 s 内采到 mask=0**：无法界定边沿时刻 → A-1/A-2 记 INCONCLUSIVE")
        faults_all = [f for f in frames if f[1] == P.T_FAULT]
        log("全程收到的 FAULT 帧: %d 条" % len(faults_all))
        for ts, _, _, payload in faults_all:
            try:
                fo = P.parse_fault(payload)
            except Exception as ex:
                log("   <解析失败 %s>" % ex)
                continue
            delay = (fo.t_ms - t_stop_dev) if t_stop_dev else None
            log("   FAULT t_ms=%d event=%d active=%d value=%d  收到@%s%s"
                % (fo.t_ms, fo.event, fo.active, fo.value, wall(ts),
                   "" if delay is None else "  与停灌时刻差=%+.0f ms" % delay))
        log("")
        log("=== 结束 %s ===" % time.strftime("%Y-%m-%d %H:%M:%S"))
        return 0
    finally:
        try:
            ser.close()
        except Exception:
            pass
        log.close()


if __name__ == "__main__":
    sys.exit(main())
