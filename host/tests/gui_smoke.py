"""
界面冒烟测试（离屏运行，不需要显示器，也不需要硬件）。

分两段：
  1. 真实跑 3 秒演示数据流，断言：采样点收到、曲线有数据、事件表非空、
     **整帧丢失恒为 0**、没有解析错误；
  2. 人为注入一次序号跳变 + 一次 BOOT 重启，反向验证"整帧丢失检测"
     确实在工作——否则"恒为 0"可能只是功能没实现。

    python tests/gui_smoke.py

退出码 0 = 通过。没有装 PySide6 时会直接跳过并返回 0（打印 SKIP）。
"""

import os
import struct
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

try:
    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication
except ImportError as exc:            # pragma: no cover
    print("SKIP: 未安装 PySide6（%s），界面冒烟测试跳过" % exc)
    sys.exit(0)

import main as M  # noqa: E402
from protocol import T_BOOT, T_SAMPLE, encode  # noqa: E402,F401

DURATION_MS = 3000


def _sample_frame(seq, t_ms=1000):
    return (T_SAMPLE, seq, struct.pack("<IHHBB", t_ms, 1000, 1000, 0, seq & 0xFF))


def check_frame_gap_detection(win):
    """
    反向验证整帧丢失检测确实生效。

    注入 seq 10 -> 13（中间少 2 帧），计数应 +2；
    再注入一帧 BOOT（设备重启、序号归零重来），计数不应增加，且期望值重新对齐。
    """
    win.demo_src.stop()

    base = win.frame_gap_count
    win.expected_frame_seq = 10
    win._on_frames([_sample_frame(10)])          # 对齐到 10
    win._on_frames([_sample_frame(13)])          # 跳过 11、12
    counted = win.frame_gap_count - base
    ok_gap = (counted == 2)

    base2 = win.frame_gap_count
    win._on_frames([(T_BOOT, 200, struct.pack("<HHBBB", 0x0500, 100, 2, 8, 1))])
    ok_boot = (win.frame_gap_count == base2) and (win.expected_frame_seq == 201)

    return ok_gap, ok_boot, counted


def run():
    app = QApplication(sys.argv)
    print("界面字体      : %s" % M.apply_cjk_font(app))
    win = M.MainWindow(demo=True)
    win.show()

    QTimer.singleShot(DURATION_MS, app.quit)
    app.exec()

    # 顺手存一张界面截图，方便没有环境时查看最终形态
    shot = os.path.join(os.path.dirname(os.path.dirname(HERE)), "docs", "gui_demo.png")
    os.makedirs(os.path.dirname(shot), exist_ok=True)
    win.grab().save(shot)
    print("截图已保存    : %s" % shot)

    samples = win.samples
    points = len(win.t_buf)
    rows = win.table.rowCount()
    gaps = win.gap_count
    frame_gaps = win.frame_gap_count
    errors = win.parser.errors

    print("采样点        : %d" % samples)
    print("曲线缓冲点数  : %d" % points)
    print("告警事件行数  : %d" % rows)
    print("序号缺口      : %d" % gaps)
    print("整帧丢失      : %d" % frame_gaps)
    print("解析错误      : %d" % errors)
    print("当前级别      : %s" % win.lbl_level.text())
    print("统计面板采样数: %s" % win.lbl_samples.text())

    ok = True
    # 3 秒 100Hz 理论 300 点，留出启动开销余量
    if samples < 200:
        print("FAIL: 采样点太少，数据通路没跑起来")
        ok = False
    if points < 200:
        print("FAIL: 曲线缓冲没数据")
        ok = False
    if rows < 1:
        print("FAIL: 告警事件表为空（演示数据应至少触发一次状态变化）")
        ok = False
    if gaps != 0:
        print("FAIL: 演示流不应有 seq8 缺口，却统计到 %d" % gaps)
        ok = False
    if frame_gaps != 0:
        print("FAIL: 演示流不应有整帧丢失，却统计到 %d" % frame_gaps)
        ok = False
    if errors != 0:
        print("FAIL: 解析错误 %d" % errors)
        ok = False

    # ---- 第二段：反向验证检测逻辑本身 ----
    ok_gap, ok_boot, counted = check_frame_gap_detection(win)
    print("注入缺口检测  : %s（期望 +2，实得 +%d）" % ("OK" if ok_gap else "FAIL", counted))
    print("BOOT 重新对齐 : %s" % ("OK" if ok_boot else "FAIL"))
    if not ok_gap:
        ok = False
    if not ok_boot:
        ok = False

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(run())
