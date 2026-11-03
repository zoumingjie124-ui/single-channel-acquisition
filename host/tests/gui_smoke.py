"""
界面冒烟测试（离屏运行，不需要显示器，也不需要硬件）。

分两段：
  1. 真实跑 3 秒演示数据流，断言：采样点收到、曲线有数据、事件表非空、
     **整帧丢失恒为 0**、没有解析错误；
  2. 人为注入一次序号跳变 + 一次 BOOT 重启，反向验证"整帧丢失检测"
     确实在工作——否则"恒为 0"可能只是功能没实现；并验证"清零"按钮
     能将界面与演示源计数同步归零且不破坏 seq8 连续性。

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
from protocol import T_BOOT, T_FAULT, T_SAMPLE, encode  # noqa: E402,F401

DURATION_MS = 4050   # 4 s + 50 ms：STAT 是 1 Hz，多等半拍让 t=4000 ms 那条 STAT
                     # 被处理掉，否则截图里"运行时间 3.0 s"会和"采样 400+"看着对不上
                     # （其实只是 STAT 天然滞后一拍，不是数据丢了）


def _sample_frame(seq, t_ms=1000):
    return (T_SAMPLE, seq, struct.pack("<IHHBB", t_ms, 1000, 1000, 0, seq & 0xFF))


def check_frame_gap_detection(win):
    """
    反向验证整帧丢失检测与清零交互确实生效。

    1. 触发一次清零：采样计数应归零，随后演示源再走 1 个 tick，采样数应为 1 且无 seq8 缺口；
    2. 注入 seq 10 -> 13（中间少 2 帧），计数应 +2；
    3. 再注入一帧 BOOT（设备重启、序号归零重来），计数不应增加，且期望值重新对齐。
    """
    win.demo_src.stop()

    win._reset_stats()
    ok_reset_zero = (win.samples == 0 and win.lbl_samples.text() == "0")
    win.demo_src._tick()
    ok_reset = ok_reset_zero and (win.samples == 1) and (win.gap_count == 0)

    base = win.frame_gap_count
    win.expected_frame_seq = 10
    win._on_frames([_sample_frame(10)])          # 对齐到 10
    win._on_frames([_sample_frame(13)])          # 跳过 11、12
    counted = win.frame_gap_count - base
    ok_gap = (counted == 2)

    base2 = win.frame_gap_count
    win._on_frames([(T_BOOT, 200, struct.pack("<HHBBB", 0x0500, 100, 2, 8, 1))])
    ok_boot = (win.frame_gap_count == base2) and (win.expected_frame_seq == 201)

    return ok_reset, ok_gap, ok_boot, counted


def check_fault_ui(win):
    """
    Stage 7C：验证界面能跟着 FAULT 帧变。

    注意这是**软件模拟**（直接往界面喂帧），不是板级实测：
    它证明的是"收到确认帧 -> 标签变红显示 STUCK""收到恢复帧 -> 标签回到无"，
    证明不了设备真的会发这两帧。

    同时验证：插一个未知帧类型不会打挂解析器。
    """
    win.demo_src.stop()
    win.expected_frame_seq = None
    win.expected_seq = None

    # 1) 勾选使能：应下发 SET_DIAG 并收到 ACK（演示源会回）
    win.chk_diag.setChecked(True)
    ok_enable = win.diag_enabled and ("SET_DIAG" in win.log.toPlainText())

    # 2) 故障确认帧
    seq = 0x60
    payload = struct.pack("<IBBBH", 3000, 1, 1, 1, 12)      # t=3000 确认 STUCK mask=1 span=12
    win._on_frames(win.parser.feed(encode(T_FAULT, seq, payload)))
    ok_confirm = ("STUCK" in win.lbl_fault.text()) and ("确认" in win.lbl_fault_last.text())

    # 3) 故障恢复帧
    payload = struct.pack("<IBBBH", 6000, 2, 1, 0, 120)     # t=6000 恢复 mask=0 span=120
    win._on_frames(win.parser.feed(encode(T_FAULT, (seq + 1) & 0xFF, payload)))
    ok_recover = (win.lbl_fault.text() == "无") and ("恢复" in win.lbl_fault_last.text())

    # 4) 未知帧类型 + 紧接着一帧正常 SAMPLE：解析器不能失效
    before = win.samples
    junk = encode(0x99, (seq + 2) & 0xFF, b"\x01\x02\x03")
    win._on_frames(win.parser.feed(junk))
    win._on_frames(win.parser.feed(
        encode(T_SAMPLE, (seq + 3) & 0xFF, struct.pack("<IHHBB", 7000, 1000, 1000, 0, 0))))
    ok_unknown = (win.samples == before + 1) and (win.parser.errors == 0)

    return ok_enable, ok_confirm, ok_recover, ok_unknown


def run():
    app = QApplication(sys.argv)
    print("界面字体      : %s" % M.apply_cjk_font(app))
    win = M.MainWindow(demo=True)
    win.show()

    # 截图与取值都必须在事件循环**内**完成。
    # 否则 app.quit() 之后窗口会走 closeEvent -> _disconnect()，顶部标签变成
    # 红色的"未连接"（演示模式应是琥珀色的"演示模式（无硬件）"），
    # 而且 STAT 只到第 3 秒、采样数却已跑到第 4 秒，两个数字对不上。
    if "--shot" in sys.argv:
        shot = os.path.join(os.path.dirname(os.path.dirname(HERE)), "docs", "gui_demo.png")
    else:
        import tempfile
        shot = os.path.join(tempfile.gettempdir(), "gui_demo_smoke.png")
    os.makedirs(os.path.dirname(shot), exist_ok=True)

    snap = {}

    def on_done():
        snap["samples"] = win.samples
        snap["points"] = len(win.t_buf)
        snap["rows"] = win.table.rowCount()
        snap["gaps"] = win.gap_count
        snap["frame_gaps"] = win.frame_gap_count
        snap["errors"] = win.parser.errors
        snap["level"] = win.lbl_level.text()
        snap["conn"] = win.lbl_conn.text()
        snap["stats"] = win.lbl_samples.text()
        win.grab().save(shot)
        app.quit()

    QTimer.singleShot(DURATION_MS, on_done)
    app.exec()

    print("截图已保存    : %s" % shot)
    print("顶部状态      : %s" % snap["conn"])

    samples = snap["samples"]
    points = snap["points"]
    rows = snap["rows"]
    gaps = snap["gaps"]
    frame_gaps = snap["frame_gaps"]
    errors = snap["errors"]

    print("采样点        : %d" % samples)
    print("曲线缓冲点数  : %d" % points)
    print("告警事件行数  : %d" % rows)
    print("序号缺口      : %d" % gaps)
    print("整帧丢失      : %d" % frame_gaps)
    print("解析错误      : %d" % errors)
    print("当前级别      : %s" % snap["level"])
    print("统计面板采样数: %s" % snap["stats"])

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
    if "演示模式" not in snap["conn"]:
        print("FAIL: 顶部状态应为「演示模式（无硬件）」，实际是 %r" % snap["conn"])
        ok = False

    # ---- 第二段：反向验证检测逻辑与清零闭环 ----
    ok_reset, ok_gap, ok_boot, counted = check_frame_gap_detection(win)
    print("清零同步验证  : %s" % ("OK" if ok_reset else "FAIL"))
    print("注入缺口检测  : %s（期望 +2，实得 +%d）" % ("OK" if ok_gap else "FAIL", counted))
    print("BOOT 重新对齐 : %s" % ("OK" if ok_boot else "FAIL"))
    if not ok_reset:
        ok = False
    if not ok_gap:
        ok = False
    if not ok_boot:
        ok = False

    # ---- 第三段：Stage 7C 故障帧与界面联动（软件模拟）----
    ok_en, ok_cf, ok_rc, ok_uk = check_fault_ui(win)
    print("诊断开关下发  : %s" % ("OK" if ok_en else "FAIL"))
    print("故障确认显示  : %s" % ("OK" if ok_cf else "FAIL"))
    print("故障恢复显示  : %s" % ("OK" if ok_rc else "FAIL"))
    print("未知帧不致命  : %s" % ("OK" if ok_uk else "FAIL"))
    if not (ok_en and ok_cf and ok_rc and ok_uk):
        ok = False

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(run())
