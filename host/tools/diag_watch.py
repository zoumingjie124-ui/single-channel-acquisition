# -*- coding: utf-8 -*-
"""盯 FAULT 帧：板级验证故障确认/恢复时用。

用法（在工程根目录）::

    python host/tools/diag_watch.py 30 COM6        # 盯 30 秒
    python host/tools/diag_watch.py 30 COM6 --stat # 顺便每 5 s 打一次 DIAG 文本行

为什么不用 GUI：
    GUI 会把串口独占，脚本更适合"一边拧电位器一边记录"这种手工配合的场景。
    输出可以直接 tee 到 docs/ 下当原始记录。

它只做三件事：解析 FAULT 帧、按秒打印、结束时报总数。
不判断对错——判定留给报告，工具只负责记录。
"""

import os
import sys
import time

import serial
from serial.tools import list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(PROJECT, "host"))

import protocol as P  # noqa: E402


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    if len(sys.argv) > 2 and not sys.argv[2].startswith("-"):
        port = sys.argv[2]
    else:
        ports = [p.device for p in list_ports.comports()]
        if not ports:
            print("未检测到串口")
            return 2
        port = ports[0]
    want_stat = "--stat" in sys.argv

    ser = serial.Serial(port, 115200, timeout=0.2)
    ser.reset_input_buffer()
    parser = P.FrameParser()
    t0 = time.time()
    next_stat = 5.0
    faults = []
    samples = 0

    print("port=%s  duration=%gs  开始 %s"
          % (port, secs, time.strftime("%H:%M:%S")))

    while True:
        elapsed = time.time() - t0
        if elapsed >= secs:
            break

        nav = ser.in_waiting
        if nav:
            for mt, _ms, pl in parser.feed(ser.read(nav)):
                if mt == P.T_FAULT:
                    f = P.decode_many([(mt, _ms, pl)])[0]
                    faults.append(f)
                    print("[%6.2fs] FAULT t=%dms %s %s mask=0x%02X span=%dmV"
                          % (elapsed, f.t_ms, f.fault_name, f.event_name, f.active, f.value))
                elif mt == P.T_SAMPLE:
                    samples += 1
        else:
            time.sleep(0.005)

        if want_stat and elapsed >= next_stat:
            next_stat += 5.0
            ser.write(b"stat\r\n")
            ser.flush()
            # 收一小段，把文本行捞出来
            t_end = time.time() + 0.15
            while time.time() < t_end:
                nav = ser.in_waiting
                if nav:
                    parser.feed(ser.read(nav))
                else:
                    time.sleep(0.005)
            for line in parser.drain_lines():
                if line.startswith("DIAG") or line.startswith("STAT"):
                    print("[%6.2fs]   TXT %s" % (elapsed, line))

    ser.close()
    print("---- 合计 ----")
    print("时长 %.1fs  SAMPLE %d  解析错误 %d" % (time.time() - t0, samples, parser.errors))
    print("FAULT 帧 %d 个" % len(faults))
    for f in faults:
        print("  %dms %-10s %s mask=0x%02X value=%d"
              % (f.t_ms, f.fault_name, f.event_name, f.active, f.value))
    return 0


if __name__ == "__main__":
    sys.exit(main())
