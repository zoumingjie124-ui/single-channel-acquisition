# -*- coding: utf-8 -*-
"""长跑收尾：等日志出现「合计」段 → 生成记录表 → 关机（可取消）。

用法::

    python host/tools/soak_finish.py --log docs/SOAK_60MIN.txt --minutes 60 --grace 300
    python host/tools/soak_finish.py --log docs/SOAK_60MIN.txt --dry-run    # 只出报告不关机

为什么要有这个脚本：
  1 小时 / 8 小时长跑没法一直盯着。跑完那一下是最有价值的时刻（数据齐、设备状态还在），
  但人往往已经走了。脚本把"判定 + 落盘 + 关机"三件事做完，证据先保住再断电。

安全设计（都很直白，别改）：
  - 先写报告、后关机：报告落盘失败就不关机；
  - 关机用 `shutdown /s /t <grace>`，缓冲期内 `shutdown /a` 可随时取消；
  - 日志没写完 / 中途死掉 → 照关（按用户要求），但报告里必须写清"日志不完整"，
    不许把半截数据写成 PASS。
"""

import argparse
import os
import re
import subprocess
import sys
import time

PROJECT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# 每分钟打点行
RE_SNAP = re.compile(
    r"\[\s*(\d+)s\]\s+SAMPLE\s+(\d+)\(\+(\d+)\s*\)\s+STAT\s+(\d+)\(\+(\d+)\s*\)\s*\|"
    r"\s*drops=(\S+)\s+frame_err=(\S+)\s+adc_err=(\S+)\s+uart_err=(\S+)\s+rx_err=(\S+)\s*\|"
    r"\s*seq8缺口=(\d+)\s+整帧丢失=(\d+)\s+解析错=(\d+)"
)
RE_TOTAL = re.compile(r"=====\s*合计（([\d.]+)\s*秒）=====")
RE_KV = re.compile(r"^\s*([^\s:][^:]*?)\s*:\s*(\d+)")

# 判定门槛（与 docs/SOAK_PLAN.md §2 完全一致）
TH = {
    "sample_hz": (100.00, 0.10),
    "stat_hz": (1.00, 0.05),
}


def read_log(path):
    """读日志。先按 UTF-8，失败再按 GBK —— 早期版本的采集脚本写的是本地编码，
    这里必须兼容，否则中文键名（drops 以外的那些）会全部解析不出来。"""
    with open(path, "rb") as f:
        raw = f.read()
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", errors="replace")


def parse(text):
    """返回 (header, snaps, total) ；total 为 None 表示日志没写完。"""
    header = text.split("\n", 1)[0].strip()

    snaps = []
    for m in RE_SNAP.finditer(text):
        g = m.groups()
        snaps.append({
            "elapsed": int(g[0]),
            "sample": int(g[1]),
            "sample_delta": int(g[2]),
            "stat": int(g[3]),
            "stat_delta": int(g[4]),
            # 设备侧计数可能是 "-"（那一刻还没收到 STAT）
            "drops": _num(g[5]), "frame_err": _num(g[6]), "adc_err": _num(g[7]),
            "uart_err": _num(g[8]), "rx_err": _num(g[9]),
            "seq8_gap": int(g[10]), "frame_gap": int(g[11]), "parse_err": int(g[12]),
        })

    total = None
    mt = RE_TOTAL.search(text)
    if mt:
        total = {"duration_s": float(mt.group(1))}
        tail = text[mt.end():]
        for line in tail.split("\n"):
            mk = RE_KV.match(line)
            if mk:
                total[mk.group(1).strip()] = int(mk.group(2))
        # 频率直接从合计段里的括号值取，避免二次计算口径不一致。
        # 兼容两种写法："(100.02 Hz)" 与旧版的 "(实测 100.01 Hz)"
        m = re.search(r"SAMPLE 帧\s*:\s*\d+\s*\([^()]*?([\d.]+)\s*Hz", tail)
        if m:
            total["sample_hz"] = float(m.group(1))
        m = re.search(r"STAT 帧\s*:\s*\d+\s*\([^()]*?([\d.]+)\s*Hz", tail)
        if m:
            total["stat_hz"] = float(m.group(1))
    return header, snaps, total


def _num(s):
    s = s.strip()
    return int(s) if s.lstrip("-").isdigit() else None


def judge(total, snaps):
    """按 H1-H12 逐项判定。返回 [(编号, 指标, 实测, 门槛, 判定)]。"""
    rows = []
    ok_all = True

    def add(hid, name, val, gate, cond):
        nonlocal ok_all
        if cond is None:          # 字段缺失 → 不许判 PASS，也不判 FAIL，写"无法判定"
            ok_all = False
            rows.append((hid, name, val, gate, "无法判定"))
            return
        ok = bool(cond)
        ok_all = ok_all and ok
        rows.append((hid, name, val, gate, "PASS" if ok else "FAIL"))

    if total is None:
        # 日志不完整：所有项都没法判
        for hid, name in [("H1", "SAMPLE 频率"), ("H2", "STAT 频率"), ("H3", "drops"),
                          ("H4", "frame_err"), ("H5", "adc_err"), ("H6", "uart_err"),
                          ("H7", "seq8 缺口"), ("H8", "整帧丢失"), ("H9", "BOOT 帧"),
                          ("H10", "链路中断"), ("H11", "时基偏差"), ("H12", "解析错误")]:
            rows.append((hid, name, "无「合计」段", "—", "无法判定"))
        return rows, False

    hz = total.get("sample_hz")
    if hz is None:
        add("H1", "SAMPLE 频率", "—（日志无此字段）", "100.00 ± 0.10", None)
    else:
        add("H1", "SAMPLE 频率", "%.3f Hz" % hz, "%.2f ± %.2f" % TH["sample_hz"],
            abs(hz - TH["sample_hz"][0]) <= TH["sample_hz"][1])

    shz = total.get("stat_hz")
    if shz is None:
        add("H2", "STAT 频率", "—（日志无此字段）", "1.00 ± 0.05", None)
    else:
        add("H2", "STAT 频率", "%.3f Hz" % shz, "%.2f ± %.2f" % TH["stat_hz"],
            abs(shz - TH["stat_hz"][0]) <= TH["stat_hz"][1])

    for hid, key, name in [("H3", "drops", "drops"), ("H4", "frame_err", "frame_err"),
                           ("H5", "adc_err", "adc_err"), ("H6", "uart_err", "uart_err"),
                           ("H7", "seq8 缺口", "seq8 缺口"), ("H8", "整帧丢失", "整帧丢失"),
                           ("H9", "BOOT 帧", "BOOT 帧"), ("H10", "链路中断次数", "链路中断"),
                           ("H12", "解析错误", "解析错误")]:
        v = total.get(key)
        add(hid, name, "—（日志无此字段）" if v is None else str(v), "= 0", None if v is None else (v == 0))

    # H11：设备时基 vs PC 墙钟。帧由 TIM3(HSE) 产生，按 PC 墙钟计时，
    # 所以帧率偏差就是时基偏差（不是 uptime 差——uptime 起点没记进日志）。
    if hz is None:
        add("H11", "时基偏差（帧率反推）", "—（依赖 H1）", "< 0.1 %", None)
    else:
        drift = abs(hz - 100.0) / 100.0 * 100.0
        add("H11", "时基偏差（帧率反推）", "%.4f %%" % drift, "< 0.1 %", drift < 0.1)

    return rows, ok_all


def build_record(log_path, header, snaps, total, rows, ok_all):
    out = []
    name = os.path.basename(log_path)

    out.append("# 连续运行记录（自动生成）\n")
    out.append("> 由 `host/tools/soak_finish.py` 在长跑结束后自动生成，数字直接取自原始日志。\n")
    out.append("> **本文件不是手填的**：每个值都能在 `%s` 里找到对应行。\n" % name)

    out.append("\n## 0. 环境\n")
    out.append("```\n%s\n```\n" % header)
    if total:
        out.append("\n实际时长：**%.1f 秒**（%.2f 分钟）\n" % (total["duration_s"], total["duration_s"] / 60.0))
    if snaps:
        out.append("打点行数：**%d**（每分钟一行，说明采集侧没有长时间卡顿）\n" % len(snaps))

    out.append("\n## 1. 表 A —— 首末总账\n")
    out.append("| 字段 | 开跑 | 结束 | 判定 |")
    out.append("|---|---|---|---|")
    out.append("| 设备计数器（drops/frame_err/…） | 全部 0（开跑前已发 `reset`） | 见下表 | — |")
    if total:
        out.append("| SAMPLE 帧 | 0 | %s | H1 |" % total.get("SAMPLE 帧", "—"))
        out.append("| STAT 帧 | 0 | %s | H2 |" % total.get("STAT 帧", "—"))
        out.append("| drops | 0 | %s | H3 |" % total.get("drops", "—"))
        out.append("| frame_err | 0 | %s | H4 |" % total.get("frame_err", "—"))
        out.append("| adc_err | 0 | %s | H5 |" % total.get("adc_err", "—"))
        out.append("| uart_err | 0 | %s | H6 |" % total.get("uart_err", "—"))
        out.append("| seq8 缺口 | 0 | %s | H7 |" % total.get("seq8 缺口", "—"))
        out.append("| 整帧丢失 | 0 | %s | H8 |" % total.get("整帧丢失", "—"))
        out.append("| BOOT 帧 | 0 | %s | H9 |" % total.get("BOOT 帧", "—"))
        out.append("| 链路中断 | 0 | %s | H10 |" % total.get("链路中断次数", "—"))
        out.append("| changes（状态切换） | 0 | %s | O1 |" % total.get("状态切换", "—"))
        out.append("| 解析错误 | 0 | %s | H12 |" % total.get("解析错误", "—"))
        out.append("| 设备 uptime | — | %s ms | — |" % total.get("设备运行时间", "—"))
        out.append("| 收到字节 | 0 | %s B | — |" % total.get("收到字节", "—"))

    out.append("\n## 2. 表 B —— 每 5 分钟打点\n")
    if not snaps:
        out.append("> 该日志没有逐分钟打点行（早期脚本格式），本节跳过；以 §1 合计段为准。\n")
    out.append("| # | 时刻 | SAMPLE 累计 | 本段帧数 | STAT 累计 | drops | frame_err | adc_err | uart_err | seq8 缺口 | 整帧丢失 | 解析错 |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    step = max(1, len(snaps) // 12) if snaps else 5
    for s in snaps[step - 1::step]:
        out.append("| %d | %ds | %d | %d | %d | %s | %s | %s | %s | %d | %d | %d |" % (
            s["elapsed"] // 60, s["elapsed"], s["sample"], s["sample_delta"], s["stat"],
            s["drops"], s["frame_err"], s["adc_err"], s["uart_err"],
            s["seq8_gap"], s["frame_gap"], s["parse_err"]))

    out.append("\n## 3. 判定（门槛见 docs/SOAK_PLAN.md §2）\n")
    out.append("| 编号 | 指标 | 实测 | 门槛 | 判定 |")
    out.append("|---|---|---|---|---|")
    for hid, n, v, g, r in rows:
        out.append("| %s | %s | %s | %s | **%s** |" % (hid, n, v, g, r))

    out.append("\n## 4. 结论\n")
    if total is None:
        out.append("**日志不完整**：没有找到「合计」段，说明长跑中途被打断（采集脚本异常退出 / 串口断开无法重连 / 被手动停止）。\n")
        out.append("这种情况**不能**当作一次有效长跑，H1–H12 全部记「无法判定」。\n")
    else:
        out.append("**%s**（%d 项门槛中 %d 项通过）\n" % (
            "PASS" if ok_all else "FAIL", len(rows),
            sum(1 for r in rows if r[4] == "PASS")))
        if not ok_all:
            bad = [r[0] for r in rows if r[4] != "PASS"]
            out.append("\n未通过：%s —— 具体数值见上表，先怀疑线路/供电/PC 侧，再怀疑固件。\n"
                       % "、".join(bad))

    out.append("\n---\n原始日志：`docs/%s`\n" % name)
    out.append("记录表模板：`docs/SOAK_PLAN.md`\n")
    return "\n".join(out) + "\n"


def wait_for_total(path, poll_s=5.0, stale_min=20.0):
    """等日志出现「合计」段并写稳。返回 (header, snaps, total)。
    若日志超过 stale_min 分钟没有增长且仍无「合计」，判定为中途死掉。

    为什么要"写稳"：soak.py 是先写「===== 合计 =====」再逐行写各项，
    一看到这几行就立刻解析，后面的 drops/frame_err 可能还没落盘，
    会被误读成"日志无此字段"。所以命中后再等两次大小不变。"""
    last_size = -1
    last_change = time.time()
    stable = 0

    while True:
        try:
            text = read_log(path)
        except OSError:
            text = ""
        header, snaps, total = parse(text)
        size = len(text)

        if total is not None:
            if size == last_size:
                stable += 1
            else:
                stable = 0
            # 连续两次大小不变（约 2 个 poll 周期）才算写稳
            if stable >= 2:
                return header, snaps, total
        else:
            if size != last_size:
                last_change = time.time()
            elif time.time() - last_change > stale_min * 60.0:
                return header, snaps, None

        last_size = size
        time.sleep(poll_s)


def do_shutdown(grace, reason):
    cmd = ["shutdown", "/s", "/t", str(grace), "/c", reason]
    print("执行关机：%s" % " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("关机命令失败（returncode=%d）" % r.returncode)
        print("stdout:", r.stdout.strip())
        print("stderr:", r.stderr.strip())
        print("手动关机：shutdown /s /t 60")
        return False
    print("已预约 %d 秒后关机。取消：shutdown /a" % grace)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default="docs/SOAK_60MIN.txt")
    ap.add_argument("--grace", type=int, default=300, help="关机缓冲秒数")
    ap.add_argument("--dry-run", action="store_true", help="只生成报告，不关机")
    ap.add_argument("--poll", type=float, default=5.0)
    ap.add_argument("--stale-min", type=float, default=20.0)
    args = ap.parse_args()

    log_path = args.log if os.path.isabs(args.log) else os.path.join(PROJECT, args.log)
    if not os.path.exists(log_path):
        print("日志不存在：%s" % log_path)
        return 2

    print("等待长跑结束：%s" % log_path)
    header, snaps, total = wait_for_total(log_path, args.poll, args.stale_min)
    if total is None:
        print("!! 没有等到「合计」段，按不完整处理")

    rows, ok_all = judge(total, snaps)
    record = build_record(log_path, header, snaps, total, rows, ok_all)
    rec_path = log_path.replace(".txt", "_RECORD.md")
    try:
        with open(rec_path, "w", encoding="utf-8") as f:
            f.write(record)
        print("记录已写入：%s" % rec_path)
    except OSError as exc:
        print("!! 记录写入失败，放弃关机：%s" % exc)
        return 3

    # 控制台摘要
    print("\n===== 结果 =====")
    if total:
        print("时长 %.1fs | SAMPLE %s 帧 (%s Hz) | STAT %s 帧 (%s Hz)"
              % (total["duration_s"], total.get("SAMPLE 帧"),
                 total.get("sample_hz", "—"), total.get("STAT 帧"),
                 total.get("stat_hz", "—")))
        print("drops=%s frame_err=%s adc_err=%s uart_err=%s 缺口=%s 整帧丢失=%s BOOT=%s"
              % (total.get("drops"), total.get("frame_err"), total.get("adc_err"),
                 total.get("uart_err"), total.get("seq8 缺口"), total.get("整帧丢失"),
                 total.get("BOOT 帧")))
    for hid, n, v, g, r in rows:
        print("  %-4s %-18s %-14s %-10s %s" % (hid, n, v, g, r))
    print("总判定：%s" % ("PASS" if ok_all else ("FAIL" if total else "无法判定")))

    if args.dry_run:
        print("\n--dry-run：不关机")
        return 0

    verdict = "PASS" if ok_all else "FAIL"
    do_shutdown(args.grace, "STM32 soak finished: %s (report saved)" % verdict)
    return 0


if __name__ == "__main__":
    sys.exit(main())
