#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""阶段二：ADC 精度与线性验证工具。

三个子命令，都不改固件：

    python host/tools/lin.py COM6 hold  --sec 3 [--ref 1650]
        在当前输入下静置采集，输出 raw_mv / filt_mv 的均值、峰峰、标准差，
        以及相对 --ref（外部表计读数）的误差。

    python host/tools/lin.py COM6 sweep --sec 20 [--csv docs/LIN_SWEEP.csv]
        持续采集同时缓慢拧电位器，落 CSV；结束后分析单调性、卡码、
        各电平区间的噪声，用来判断"中间量程是否有异常台阶"。

    python host/tools/lin.py COM6 step  --sec 15
        等待一次快速阶跃（拧电位器），测量 filt_mv 达到 50%/63%/90% 用了多少
        采样点，与 8 点移动平均的理论群延迟 (N-1)/2 x 10 ms = 35 ms 对照。

注意：所有 mv 都来自固件的 `raw*3300/4095`，属于"比例测量"。
没有外部表计时，只能验证线性/延迟/噪声，**不能**证明绝对电压精度。
"""

import argparse
import os
import statistics
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.dirname(HERE)
ROOT = os.path.dirname(HOST)
sys.path.insert(0, HOST)

import protocol as P  # noqa: E402

TICK_MS = 10.0          # TIM3 100 Hz
FILTER_DEPTH = 8        # APP_FILTER_DEPTH


class Link:
    def __init__(self, port, baud=115200):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.ser.reset_input_buffer()
        self.parser = P.FrameParser()
        self.samples = []

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                n = self.ser.in_waiting
            except Exception:
                return
            if n == 0:
                time.sleep(0.002)
                continue
            try:
                chunk = self.ser.read(n)
            except Exception:
                return
            for mtype, mseq, payload in self.parser.feed(chunk):
                if mtype == P.T_SAMPLE:
                    for o in P.decode_many([(mtype, mseq, payload)]):
                        if isinstance(o, P.Sample):
                            self.samples.append((time.time(), o))


def cmd_hold(link, sec, ref):
    link.samples = []
    link.pump(sec)
    raw = [s.raw_mv for _t, s in link.samples]
    flt = [s.filt_mv for _t, s in link.samples]
    if not raw:
        print("没收到 SAMPLE 帧")
        return 1

    def stat(name, v):
        pp = max(v) - min(v)
        sd = statistics.pstdev(v)
        print("  %-10s 均值 %8.2f  min %5d  max %5d  峰峰 %3d  标准差 %6.2f"
              % (name, sum(v) / len(v), min(v), max(v), pp, sd))

    print("样本数 %d  时长约 %.2f s" % (len(raw), len(raw) * TICK_MS / 1000.0))
    stat("raw_mv", raw)
    stat("filt_mv", flt)

    mean_raw = sum(raw) / len(raw)
    lsb = mean_raw / (3300.0 / 4095.0)
    print("  反算 LSB   %.1f（1 LSB = 0.806 mV）" % lsb)
    if ref is not None:
        err = mean_raw - ref
        print("  外部参考   %d mV" % ref)
        print("  误差       %+.2f mV（%+.3f%% FS）" % (err, err / 3300.0 * 100))
    else:
        print("  未提供 --ref：只做噪声/量化统计，不做绝对误差判断")
    return 0


def cmd_sweep(link, sec, csv_path):
    link.samples = []
    print("开始采集 %.0f 秒 —— 请从一端开始，用约 %.0f 秒**匀速**拧到另一端"
          "（每秒约 %.0f mV），到端点后停住即可" % (sec, sec * 0.7, 2500.0 / (sec * 0.7)))
    for i in range(int(sec), 0, -1):
        print("  剩余 %2d s" % i, end="\r", flush=True)
        link.pump(1.0)
    print(" " * 20, end="\r")

    if len(link.samples) < 100:
        print("样本太少（%d），检查接线" % len(link.samples))
        return 1

    rows = [(round(t, 4), s.t_ms, s.raw_mv, s.filt_mv, s.level, s.seq8)
            for t, s in link.samples]

    if csv_path:
        os.makedirs(os.path.dirname(csv_path), exist_ok=True)
        with open(csv_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("host_time,t_ms,raw_mv,filt_mv,level,seq8\n")
            for r in rows:
                f.write("%.4f,%d,%d,%d,%d,%d\n" % r)
        print("已写 %s（%d 行）" % (csv_path, len(rows)))

    return analyze_series([r[2] for r in rows])


def analyze_series(raw):
    """对一串 raw_mv 做线性/噪声分析（数据可以来自实时采集或 --from-csv）。"""
    span = max(raw) - min(raw)
    print("\n电平范围 %d ~ %d mV（跨度 %d mV，%d 点）"
          % (min(raw), max(raw), span, len(raw)))
    if span < 200:
        print("跨度太小 —— 可能没拧电位器，本次不具备线性分析价值")
        return 1

    # 移动段：从两端向内收缩，去掉停在极值上的部分
    lo, hi = min(raw), max(raw)
    a = 0
    while a < len(raw) and abs(raw[a] - lo) <= 3:
        a += 1
    b = len(raw) - 1
    while b > a and abs(raw[b] - hi) <= 3:
        b -= 1
    moving = raw[a:b + 1]
    print("移动段：样本 %d ~ %d（%d 点，约 %.1f s）"
          % (a, b, len(moving), len(moving) * 10 / 1000.0))
    if len(moving) < 50:
        print("移动段太短，无法分析；请更慢地扫")
        return 1

    # 单调性（忽略 <=3 mV 的抖动与量化）
    up = moving[-1] > moving[0]
    back = 0
    for x, y in zip(moving, moving[1:]):
        if (y - x) < -3 if up else (y - x) > 3:
            back += 1
    print("移动段趋势 %s；明显反向跳变 %d 次 / %d 个相邻对（%.2f%%）"
          % ("上升" if up else "下降", back, len(moving) - 1,
             100.0 * back / max(1, len(moving) - 1)))

    # 扫掠速度与异常跳变
    steps = sorted(abs(y - x) for x, y in zip(moving, moving[1:]))
    med = steps[len(steps) // 2]
    print("相邻步长：中位 %d mV/点，最大 %d mV/点" % (med, steps[-1]))
    limit = max(20, med * 5)
    jumps = sum(1 for s in steps if s > limit)
    print("疑似跳变（>%d mV） %d 处" % (limit, jumps))

    # 噪声：局部线性去趋势后的残差（移动中的信号不能直接算峰峰）
    win = 6
    resid = []
    for i in range(win, len(moving) - win):
        seg = moving[i - win:i + win + 1]
        n = len(seg)
        idx = list(range(-win, win + 1))
        mi = sum(idx) / n
        mv = sum(seg) / n
        den = sum((x - mi) ** 2 for x in idx)
        slope = sum((idx[k] - mi) * (seg[k] - mv) for k in range(n)) / den
        resid.append(moving[i] - mv)

    if resid:
        med_r = sorted(resid)[len(resid) // 2]
        mad = sorted(abs(x - med_r) for x in resid)[len(resid) // 2]
        sigma = 1.4826 * mad
        print("\n去趋势残差（窗口 ±%d 点）：标准差 %.2f mV（MAD 估计，抗离群）"
              % (win, sigma))
        print("残差峰峰 %.1f mV；范围 %.1f ~ %.1f"
              % (max(resid) - min(resid), min(resid), max(resid)))
        print("折算约 ±%.1f LSB（1σ，1 LSB = 0.806 mV）" % (sigma / 0.806))
        print("\n提示：这才是「信号移动时的噪声」。与 hold 模式的标准差接近，"
              "说明扫掠本身没有引入额外干扰。")

    # 电平覆盖：看有没有整段跳过（可能漏采样或 ADC 丢码）
    bins = [0] * 16
    for v in moving:
        bins[min(15, v * 16 // 3301)] += 1
    empty = [i for i, c in enumerate(bins) if c == 0]
    print("\n电平覆盖：%d / 16 段非空" % (16 - len(empty)))
    if empty:
        print("  空段（每段 206 mV）：%s" % [
            "%d-%d" % (i * 3300 // 16, (i + 1) * 3300 // 16) for i in empty])
        print("  若扫掠确实经过这些电平，说明采样有缺口")
    return 0


def cmd_step(link, sec):
    print("等待一次快速阶跃（请在 1 秒内把电位器拧过至少 300 mV）……")
    link.samples = []
    t0 = time.time()
    step_at = None
    link.pump(0.5)

    while time.time() - t0 < sec:
        before = len(link.samples)
        link.pump(0.05)
        if step_at is not None:
            if len(link.samples) - step_at >= 30:
                break
            continue
        s = link.samples
        if len(s) < 6:
            continue
        # 用 3 点均值前后对比，避免单点噪声误判
        a = sum(x[1].raw_mv for x in s[-6:-3]) / 3.0
        b = sum(x[1].raw_mv for x in s[-3:]) / 3.0
        if abs(b - a) >= 300:
            step_at = len(s) - 3
            print("  检测到阶跃：%.0f -> %.0f mV（第 %d 个样本）" % (a, b, step_at))

    if step_at is None:
        print("未检测到阶跃 —— 没拧或幅度不够")
        return 1

    seg = [x[1] for x in link.samples[step_at:]]
    if len(seg) < 15:
        print("阶跃后样本不足（%d）" % len(seg))
        return 1

    v0 = seg[0].raw_mv
    vf = sum(s.raw_mv for s in seg[-5:]) / 5.0
    span = vf - v0
    print("\n阶跃 %.0f -> %.0f mV（跨度 %.0f）" % (v0, vf, span))
    print("\n  目标   raw 首次到达   filt 首次到达   filt 延迟")
    for pct in (50, 63, 90):
        target = v0 + span * pct / 100.0
        i_raw = next((i for i, s in enumerate(seg) if s.raw_mv >= target), None) if span > 0 \
            else next((i for i, s in enumerate(seg) if s.raw_mv <= target), None)
        i_flt = next((i for i, s in enumerate(seg) if s.filt_mv >= target), None) if span > 0 \
            else next((i for i, s in enumerate(seg) if s.filt_mv <= target), None)
        if i_raw is None or i_flt is None:
            print("  %3d%%   —             —" % pct)
            continue
        print("  %3d%%   %d 点 (%d ms)   %d 点 (%d ms)   %+d 点 (%+d ms)"
              % (pct, i_raw, i_raw * 10, i_flt, i_flt * 10,
                 i_flt - i_raw, (i_flt - i_raw) * 10))

    print("\n理论：%d 点移动平均，50%% 处约 %d 点（%d ms），"
          "群延迟 (N-1)/2 = %.1f 点 = %.0f ms"
          % (FILTER_DEPTH, FILTER_DEPTH // 2, FILTER_DEPTH // 2 * 10,
             (FILTER_DEPTH - 1) / 2.0, (FILTER_DEPTH - 1) / 2.0 * 10))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("mode", choices=["hold", "sweep", "step"])
    ap.add_argument("--sec", type=float, default=None)
    ap.add_argument("--ref", type=int, default=None,
                    help="外部表计实测电压（mV），用于误差比对")
    ap.add_argument("--csv", default=None)
    ap.add_argument("--from-csv", default=None,
                    help="只分析已保存的 CSV，不打开串口")
    args = ap.parse_args()

    if args.from_csv:
        import csv as _csv
        raw = []
        with open(args.from_csv, encoding="utf-8") as f:
            r = _csv.DictReader(f)
            for row in r:
                raw.append(int(row["raw_mv"]))
        print("离线分析 %s（%d 点）" % (args.from_csv, len(raw)))
        return analyze_series(raw)

    link = Link(args.port)
    try:
        if args.mode == "hold":
            return cmd_hold(link, args.sec or 3.0, args.ref)
        if args.mode == "sweep":
            csv_path = args.csv or os.path.join(ROOT, "docs", "LIN_SWEEP.csv")
            return cmd_sweep(link, args.sec or 20.0, csv_path)
        return cmd_step(link, args.sec or 15.0)
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
