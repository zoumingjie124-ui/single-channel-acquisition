# -*- coding: utf-8 -*-
"""用真实串口跑一次 PySide6 上位机，并保存界面截图。

与 gui_smoke.py 的区别：
  gui_smoke 用 --demo 的内置信号源，**不碰串口**；
  本脚本走真正的 SerialWorker，连真板子，用来给"界面接真实串口"留证据。

用法（在工程根目录下）::

    python host/tools/gui_snapshot.py COM5
    python host/tools/gui_snapshot.py            # 自动选第一个串口

无显示器时用离屏渲染：

    set QT_QPA_PLATFORM=offscreen
    python host/tools/gui_snapshot.py COM5

截图输出到 docs/gui_real_serial.png。
退出码 0 = 收到数据且无解析错误。
"""

import os
import sys
import time

from serial.tools import list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(PROJECT, "host"))

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication  # noqa: E402
from PySide6.QtCore import QTimer  # noqa: E402

import main as M  # noqa: E402

DURATION_MS = 16000
OUT = os.path.join(PROJECT, "docs", "gui_real_serial.png")


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    if not ports:
        print("未检测到任何串口")
        raise SystemExit(2)
    return ports[0]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else pick_port()

    app = QApplication(sys.argv[:1])
    M.apply_cjk_font(app)
    win = M.MainWindow(demo=False)
    win.show()

    win._refresh_ports()
    win.cb_port.setCurrentText(port)
    time.sleep(0.3)
    win._connect()

    snap = {}

    def on_done():
        # 必须在事件循环**内**截图并取值：app.quit() 之后窗口会走 closeEvent
        # -> _disconnect()，状态标签被改成"未连接"，截出来就是错的。
        snap["conn"] = win.lbl_conn.text()
        snap["samples"] = win.samples
        snap["points"] = len(win.t_buf)
        snap["rows"] = win.table.rowCount()
        snap["gaps"] = win.gap_count
        snap["frame_gaps"] = getattr(win, "frame_gap_count", -1)
        snap["level"] = win.lbl_level.text()
        snap["stats_samples"] = win.lbl_samples.text() if hasattr(win, "lbl_samples") else "-"
        win.grab().save(OUT)
        app.quit()

    QTimer.singleShot(DURATION_MS, on_done)
    app.exec()

    samples = snap.get("samples", 0)
    points = snap.get("points", 0)
    rows = snap.get("rows", 0)
    gaps = snap.get("gaps", -1)
    frame_gaps = snap.get("frame_gaps", -1)
    conn = snap.get("conn", "?")
    level = snap.get("level", "?")
    win._disconnect()

    print("串口状态      : %s" % conn)
    print("采样点        : %d" % samples)
    print("曲线缓冲点数  : %d" % points)
    print("告警事件行数  : %d" % rows)
    print("序号缺口      : %d" % gaps)
    print("整帧丢失      : %d" % frame_gaps)
    print("当前级别      : %s" % level)
    print("统计面板采样数: %s" % snap.get("stats_samples", "-"))
    print("截图已保存    : %s" % OUT)

    ok = samples > 100 and points > 100 and gaps == 0 and frame_gaps == 0
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
