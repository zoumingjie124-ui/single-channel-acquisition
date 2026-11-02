# -*- coding: utf-8 -*-
"""零点 / 满量程读数：把 PA1 接到 GND 或 3V3，各测一次。

用法（在工程根目录下）::

    python host/tools/calib.py zero          # PA1 接 GND
    python host/tools/calib.py full          # PA1 接 3V3
    python host/tools/calib.py watch         # 只观察，不判定（改线时用）

接线安全（务必先断电再改线）：
  * 只允许 GND 或 3V3，**绝不允许 5V**，也不要接负压
  * 测完记得把 PA1 接回电位器抽头

判定标准（12 位 ADC + 3.3V 参考）：
  zero : raw_mv 应为 0~2（对应 raw <= 2 LSB）
  full : raw_mv 应 >= 3295（对应 raw >= 4090 LSB）

注意：线上传输的已是换算后的 mV（SAMPLE 载荷里是 raw_mv / filt_mv），
不是 ADC 原始码。所以下面给出的 LSBy 是"由 mV 反算"的估计值：
    lsb_est = mv * 4095 / 3300
分辨率约 0.806 mV/LSB，反算误差 ±1 LSB 以内。
超出即说明存在零点偏移 / 增益误差 / 参考电压偏差，需要在文档里如实记录。
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

SECONDS = 3.0


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    if not ports:
        print("未检测到任何串口")
        raise SystemExit(2)
    return ports[0]


def main():
    mode = sys.argv[1].lower() if len(sys.argv) > 1 else "watch"
    if mode not in ("zero", "full", "watch"):
        print("用法: python host/tools/calib.py [zero|full|watch] [COM口]")
        return 2
    port = sys.argv[2] if len(sys.argv) > 2 else pick_port()

    ser = serial.Serial(port, 115200, timeout=0.2)
    ser.reset_input_buffer()
    parser = P.FrameParser()

    mvs, filts = [], []
    t0 = time.time()
    print("串口 %s，目标采集 %.1f 秒……" % (port, SECONDS))
    while time.time() - t0 < SECONDS:
        # 按 in_waiting 取，不用 read(8192)。
        # 本机（Windows + CH340 + pyserial 3.5）实测：read(n) 会累积到凑满 n 字节
        # 才返回，数据持续流入时不会在 timeout 处停下；1.7 KB/s 下凑满 8192 B 要 4.7 s，
        # 于是"请求 3 秒"跑成 4.69 秒、"请求 5 秒"跑成 9.41 秒（多一个读周期）。
        # 具体机制/是否与驱动版本有关尚未查证，此处只记录观测到的现象。
        n = ser.in_waiting
        if n == 0:
            time.sleep(0.005)
            continue
        chunk = ser.read(n)
        for mtype, mseq, payload in parser.feed(chunk):
            if mtype != P.T_SAMPLE or len(payload) < 4:
                continue
            for o in P.decode_many([(mtype, mseq, payload)]):
                if isinstance(o, P.Sample):
                    mvs.append(o.raw_mv)
                    filts.append(o.filt_mv)
    elapsed = time.time() - t0
    ser.close()

    if not mvs:
        print("没有收到 SAMPLE 帧 —— 检查板子是否在运行、串口是否选对")
        return 1

    n = len(mvs)
    mmin, mmax, mmean = min(mvs), max(mvs), sum(mvs) / n
    lsb = mmean * 4095.0 / 3300.0

    print("\n真实时长            : %.2f s" % elapsed)
    print("样本数              : %d  (%.2f Hz)" % (n, n / elapsed))
    print("raw_mv min/mean/max : %d / %.2f / %d  mV" % (mmin, mmean, mmax))
    print("反算 LSB (均值)     : %.1f  (0.806 mV/LSB)" % lsb)
    print("mV 峰峰值           : %d mV" % (mmax - mmin))

    if mode == "watch":
        print("\n（观察模式，不做判定）")
        return 0

    ok = True
    print()
    if mode == "zero":
        if mmean <= 3.0:
            print("零点判定: PASS   实测均值 %.2f mV（约 %.1f LSB），最大 %d mV"
                  % (mmean, lsb, mmax))
        else:
            print("零点判定: FAIL   期望均值 ≤3 mV，实测 %.2f mV（偏移约 %.0f LSB）"
                  % (mmean, lsb))
            ok = False
    else:
        if mmean >= 3295 and mmin >= 3290:
            print("满量程判定: PASS  实测均值 %.2f mV（约 %.1f LSB），最小 %d mV"
                  % (mmean, lsb, mmin))
        else:
            print("满量程判定: FAIL  期望均值 ≥3295 mV，实测 %.2f mV"
                  "（差 %.2f mV，约 %.0f LSB；可能是参考电压或增益误差）"
                  % (mmean, 3300 - mmean, 4095 - lsb))
            ok = False
    if mmax != mmin:
        # 峰峰值衡量的是噪声，不是零点偏移，单独列出不参与 PASS/FAIL
        print("        噪声：静止输入下读数在 %d~%d mV 间变动（峰峰 %d mV ≈ %.0f LSB）"
              % (mmin, mmax, mmax - mmin, (mmax - mmin) * 4095.0 / 3300.0))

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
