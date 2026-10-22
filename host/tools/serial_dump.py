"""
命令行抓帧工具：不启动图形界面，只把收到的帧按行打印出来。

用途：没有装 PySide6、或者只想快速确认板子在发什么的时候用。
依赖只有 pyserial。

    python tools/serial_dump.py COM5 115200
    python tools/serial_dump.py COM5            # 默认 115200
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import serial  # noqa: E402

import protocol as P  # noqa: E402


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

    print("打开 %s @ %d ..." % (port, baud))
    ser = serial.Serial(port, baud, timeout=0.05)
    parser = P.FrameParser()
    t0 = time.time()
    n = 0

    print("Ctrl+C 退出\n")
    try:
        while True:
            data = ser.read(4096)
            if not data:
                continue

            for line in parser.drain_lines():
                print("[text] %s" % line)

            for obj in P.decode_many(parser.feed(data)):
                n += 1
                if isinstance(obj, P.Sample):
                    if n % 20 == 0:      # 100Hz 下每 0.2s 打一行，避免刷屏
                        print("[SAMPLE] t=%7dms raw=%4dmV filt=%4dmV level=%-6s seq8=%d"
                              % (obj.t_ms, obj.raw_mv, obj.filt_mv,
                                 obj.level_name, obj.seq8))
                elif isinstance(obj, P.Event):
                    print("[EVENT ] t=%7dms %s (filt=%dmV)"
                          % (obj.t_ms, obj.text, obj.filt_mv))
                elif isinstance(obj, P.Stat):
                    print("[STAT  ] up=%dms n=%d drop=%d adc=%d uart=%d rx=%d frm=%d chg=%d thr=%d/%d"
                          % (obj.uptime_ms, obj.samples, obj.drops, obj.adc_err,
                             obj.uart_err, obj.rx_err, obj.frame_err, obj.changes,
                             obj.thr_warn, obj.thr_crit))
                elif isinstance(obj, P.Ack):
                    print("[ACK   ] type=0x%02X -> %s" % (obj.req_type, obj.result_name))
                elif isinstance(obj, P.Boot):
                    print("[BOOT  ] fw=%s rate=%dHz dma_buf=%d filter=%d decim=%d"
                          % (obj.version_text, obj.sample_rate,
                             obj.dma_buf, obj.filter_depth, obj.decim))
    except KeyboardInterrupt:
        elapsed = time.time() - t0
        print("\n退出：%.1fs 内共收 %d 帧，解析错误 %d 次，解析器缓冲残留 %d 字节"
              % (elapsed, parser.frames, parser.errors, len(parser._buf)))
    finally:
        ser.close()


if __name__ == "__main__":
    main()
