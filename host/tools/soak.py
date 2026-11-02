# -*- coding: utf-8 -*-
"""连续采集压力测试：统计丢点 / 丢帧 / 各类帧的实际频率。

用法（在工程根目录下）::

    python host/tools/soak.py            # 默认 10 分钟，自动选第一个串口
    python host/tools/soak.py 60 COM5    # 60 秒，指定串口

关注的四个指标：
  drops      设备端采样环溢出数（STAT 帧载荷）——0 才代表一个点都没丢
  frame_err  设备端 CRC / 格式错误累计
  seq8 缺口   样本级：SAMPLE 载荷第 9 字节的 seq8 是否连续
  整帧丢失    帧级：帧头 SEQ 是否连续（用帧头而不是载荷，才能发现整帧未到）

另外统计 SAMPLE / STAT / EVENT 的实收帧数与频率——
STAT 设计为 1 Hz（APP_STAT_PERIOD_MS=1000），若实测远低于 1 Hz，
说明 STAT 帧被丢或发送队列被饿死，属于要查的问题。

日志写到 docs/SOAK_<分钟数>MIN.txt，可直接作为测试记录归档。
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


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    if not ports:
        print("未检测到任何串口")
        raise SystemExit(2)
    return ports[0]


def main():
    minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    port = sys.argv[2] if len(sys.argv) > 2 else pick_port()
    total = minutes * 60.0

    log_path = os.path.join(PROJECT, "docs", "SOAK_%gMIN.txt" % minutes)
    ser = serial.Serial(port, 115200, timeout=0.2)
    ser.reset_input_buffer()
    parser = P.FrameParser()

    nbytes = 0
    n_sample = 0
    n_stat = 0
    n_event = 0
    n_boot = 0
    reconnects = 0
    stat = None
    exp_seq8 = None
    seq8_gap = 0
    exp_frame = None
    frame_gap = 0
    t0 = time.time()
    next_mark = 60.0
    last_sample = 0
    last_stat = 0

    out = open(log_path, "w", encoding="utf-8")
    out.write("port=%s  duration=%g min  开始 %s\n"
              % (port, minutes, time.strftime("%Y-%m-%d %H:%M:%S")))
    out.flush()

    while True:
        elapsed = time.time() - t0
        if elapsed >= total:
            break

        try:
            # 按 in_waiting 取：本机（Windows + CH340 + pyserial 3.5）实测，
            # read(n) 会累积到凑满 n 字节才返回，数据持续流入时不在 timeout 处停下；
            # 1.7 KB/s 下凑满 8192 B 要 4.7 s —— 这是 SOAK_10MIN 里每分钟采样数
            # 在 6107 / 5635 之间跳动的观测到的原因（打点被推迟一个读周期）。
            # 具体机制尚未查证，改用 in_waiting 后波动消失。
            n_avail = ser.in_waiting
            if n_avail == 0:
                time.sleep(0.005)
                chunk = b""
            else:
                chunk = ser.read(n_avail)
        except (serial.SerialException, OSError) as exc:
            # 端口被抢 / 设备重枚举：记录后重连，不中断整轮测试
            reconnects += 1
            out.write("[%5.0fs] !! 链路中断: %s\n" % (elapsed, str(exc)[:60]))
            out.flush()
            try:
                ser.close()
            except Exception:
                pass
            deadline = time.time() + 30.0
            while time.time() < deadline:
                try:
                    ser = serial.Serial(port, 115200, timeout=0.2)
                    ser.reset_input_buffer()
                    parser = P.FrameParser()
                    exp_seq8 = None
                    exp_frame = None
                    out.write("[%5.0fs] !! 已重连（序号重新对齐）\n" % (time.time() - t0))
                    out.flush()
                    break
                except Exception:
                    time.sleep(1.0)
            else:
                out.write("重连失败，提前结束\n")
                break
            continue

        if chunk:
            nbytes += len(chunk)
            for mtype, mseq, payload in parser.feed(chunk):
                # ---- 帧级：整帧丢失（用帧头 SEQ，不用载荷 seq8）----
                if mtype == P.T_BOOT:
                    exp_frame = (mseq + 1) & 0xFF
                    n_boot += 1
                else:
                    if exp_frame is not None and mseq != exp_frame:
                        frame_gap += (mseq - exp_frame) & 0xFF
                    exp_frame = (mseq + 1) & 0xFF

                if mtype == P.T_SAMPLE:
                    n_sample += 1
                    if len(payload) >= 10:
                        s8 = payload[9]
                        if exp_seq8 is not None and s8 != exp_seq8:
                            seq8_gap += (s8 - exp_seq8) & 0xFF
                        exp_seq8 = (s8 + 1) & 0xFF
                elif mtype == P.T_STAT:
                    n_stat += 1
                    for o in P.decode_many([(mtype, mseq, payload)]):
                        if isinstance(o, P.Stat):
                            stat = o
                elif mtype == P.T_EVENT:
                    n_event += 1

        if elapsed >= next_mark:
            out.write("[%5.0fs] SAMPLE %-5d(+%-5d) STAT %-3d(+%-3d) | "
                      "drops=%-5s frame_err=%-4s adc_err=%-3s uart_err=%-3s rx_err=%-3s | "
                      "seq8缺口=%-4d 整帧丢失=%-3d 解析错=%d\n"
                      % (elapsed, n_sample, n_sample - last_sample, n_stat, n_stat - last_stat,
                         stat.drops if stat else "-", stat.frame_err if stat else "-",
                         stat.adc_err if stat else "-", stat.uart_err if stat else "-",
                         stat.rx_err if stat else "-",
                         seq8_gap, frame_gap, parser.errors))
            out.flush()
            last_sample, last_stat = n_sample, n_stat
            next_mark += 60.0

    try:
        ser.close()
    except Exception:
        pass

    dur = time.time() - t0
    out.write("\n===== 合计（%.1f 秒）=====\n" % dur)
    out.write("收到字节      : %d  (%.1f B/s)\n" % (nbytes, nbytes / dur))
    out.write("SAMPLE 帧     : %d  (%.2f Hz)\n" % (n_sample, n_sample / dur))
    out.write("STAT 帧       : %d  (%.2f Hz，设计 1.00 Hz)\n" % (n_stat, n_stat / dur))
    out.write("EVENT 帧      : %d\n" % n_event)
    out.write("drops         : %s   (设备端采样环溢出，0 才算不丢点)\n"
              % (stat.drops if stat else "无 STAT 帧"))
    out.write("frame_err     : %s   (CRC/格式错误)\n" % (stat.frame_err if stat else "-"))
    out.write("adc_err       : %s\n" % (stat.adc_err if stat else "-"))
    out.write("uart_err      : %s\n" % (stat.uart_err if stat else "-"))
    out.write("rx_err        : %s\n" % (stat.rx_err if stat else "-"))
    out.write("状态切换      : %s\n" % (stat.changes if stat else "-"))
    out.write("设备运行时间  : %s ms\n" % (stat.uptime_ms if stat else "-"))
    out.write("seq8 缺口     : %d   (样本级)\n" % seq8_gap)
    out.write("整帧丢失      : %d   (帧级)\n" % frame_gap)
    out.write("解析错误      : %d\n" % parser.errors)
    out.write("BOOT 帧       : %d   (0 表示中途没有复位)\n" % n_boot)
    out.write("链路中断次数  : %d\n" % reconnects)
    out.close()

    # 控制台只打关键结论
    print("时长 %.1f s | SAMPLE %d (%.2f Hz) | STAT %d (%.2f Hz) | "
          "drops=%s frame_err=%s 序号缺口=%d 整帧丢失=%d"
          % (dur, n_sample, n_sample / dur, n_stat, n_stat / dur,
             stat.drops if stat else "-", stat.frame_err if stat else "-",
             seq8_gap, frame_gap))
    print("日志:", log_path)


if __name__ == "__main__":
    main()
