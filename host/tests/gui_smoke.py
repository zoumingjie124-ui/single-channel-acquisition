"""
界面冒烟测试（离屏运行，不需要显示器，也不需要硬件）。

它真的会创建 QApplication、实例化主窗口、跑演示数据源 3 秒，
然后断言：采样点收到、曲线缓冲有数据、告警事件表被填充、没有解析错误。

    python tests/gui_smoke.py

退出码 0 = 通过。没有装 PySide6 时会直接跳过并返回 0（打印 SKIP）。
"""

import os
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

DURATION_MS = 3000


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
    errors = win.parser.errors

    print("采样点        : %d" % samples)
    print("曲线缓冲点数  : %d" % points)
    print("告警事件行数  : %d" % rows)
    print("序号缺口      : %d" % gaps)
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
        print("FAIL: 演示流不应有缺口，却统计到 %d" % gaps)
        ok = False
    if errors != 0:
        print("FAIL: 解析错误 %d" % errors)
        ok = False

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(run())
