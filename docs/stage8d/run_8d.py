# -*- coding: utf-8 -*-
"""8D 自动执行：用 QtTest 合成鼠标事件驱动真实 GUI 窗口 + 真实串口，
每一步截图并记录界面/日志状态。物理动作（拧电位器）用 flag 文件做人工握手。

不是人手点击：走的是完整控件事件链（press/release → toggled/clicked → 同一槽函数），
证据等级按"GUI 控件自动化 + 人手拧电位器 + 真机"记录，不写成"人工点击"。

【归档版说明】本文件为 2026-11-05 实际运行脚本的脱敏副本：
  - 仅把文件头部的路径解析块由硬编码绝对路径改为「相对本文件 + 环境变量」；
  - 其余逻辑、时间轴、延时、点击顺序、日志文本与时序完全未改（逐行比对见 STAGE8D_EVIDENCE.md §6.2）；
  - 原件（未脱敏）保留在仓库外，SHA-256 见 §6.2。
运行方式（需先把仓库根目录设为工程目录）：
  set STAGE8D_PROJ=<项目目录>
  set STAGE8D_OUT=<输出目录，默认本文件同级的 _run>
  python docs\\stage8d\\run_8d.py
"""
import os
import sys
import time

from PySide6.QtCore import Qt, QTimer
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.environ.get("STAGE8D_PROJ", os.path.abspath(os.path.join(HERE, "..", "..")))
HOST = os.path.join(PROJ, "host")
sys.path.insert(0, HOST)
import main as gui  # noqa: E402

OUT = os.environ.get("STAGE8D_OUT", os.path.join(HERE, "_run"))
SHOTS = os.path.join(OUT, "shots")
os.makedirs(SHOTS, exist_ok=True)
REPORT = os.path.join(OUT, "report.txt")
GO1 = os.path.join(OUT, "go1.flag")
GO2 = os.path.join(OUT, "go2.flag")
for f in (GO1, GO2, REPORT):
    if os.path.exists(f):
        os.remove(f)

app = QApplication(sys.argv)
gui.apply_cjk_font(app)
win = gui.MainWindow(demo=False)
win.show()
win.raise_()
win.activateWindow()


def log(text):
    with open(REPORT, "a", encoding="utf-8") as f:
        f.write(text + "\n")
    print(text, flush=True)


def shot(tag):
    from PIL import ImageGrab
    g = win.frameGeometry()
    img = ImageGrab.grab(bbox=(g.x(), g.y(), g.x() + g.width(), g.y() + g.height()))
    p = os.path.join(SHOTS, tag + ".png")
    img.save(p)
    return p


def snap(tag):
    log("\n===== %s   %s =====" % (tag, time.strftime("%H:%M:%S")))
    log("[pix]        %s" % shot(tag))
    log("[conn]       %s" % win.lbl_conn.text())
    log("[level]      %s" % win.lbl_level.text())
    log("[fault]      %s   last=%s" % (win.lbl_fault.text(), win.lbl_fault_last.text()))
    log("[diag_chk]   %s" % win.chk_diag.isChecked())
    log("[table_rows] %d" % win.table.rowCount())
    log("[samples]    %s   drops=%s   errs=%s   gaps=%s   fgaps=%s" % (
        win.lbl_samples.text(), win.lbl_drops.text(), win.lbl_errs.text(),
        win.lbl_gaps.text(), win.lbl_frame_gaps.text()))
    tail = win.log.toPlainText().splitlines()[-10:]
    log("[log_tail]")
    for line in tail:
        log("    " + line)


def pick_port():
    for i in range(win.cb_port.count()):
        if "COM5" in win.cb_port.itemText(i):
            win.cb_port.setCurrentIndex(i)
            log("[port] 选择 %s" % win.cb_port.itemText(i))
            return True
    log("[port] 未找到 COM5！")
    return False


def click(widget, name):
    QTest.mouseClick(widget, Qt.LeftButton)
    log(">> 点击 %s" % name)


def wait_flag(path, hint, then):
    log("\n>>> 等待人工动作：%s" % hint)
    log(">>> 完成后请让执行代理创建 %s" % path)

    def poll():
        if os.path.exists(path):
            log(">>> 收到人工完成信号，继续")
            QTimer.singleShot(1200, then)
        else:
            QTimer.singleShot(500, poll)

    QTimer.singleShot(500, poll)


def pause1():
    wait_flag(GO1, "把电位器拧动使得故障恢复（界面转“无”），再停手",
              after_pot_recover)


def after_pot_recover():
    QTimer.singleShot(3000, lambda: snap("06_D9_recover"))
    QTimer.singleShot(9000, lambda: snap("07_D4_confirm_after_idle"))
    QTimer.singleShot(10500, do_disconnect)


def do_disconnect():
    click(win.btn_connect, "断开")
    QTimer.singleShot(2500, lambda: snap("08_D10_disconnected"))
    QTimer.singleShot(3500, pause2)


def pause2():
    wait_flag(GO2, "断线期间把电位器拧到另一个位置（制造变化）", after_pot_disconnected)


def after_pot_disconnected():
    click(win.btn_connect, "连接（重连）")
    QTimer.singleShot(5000, lambda: snap("09_D12_reconnected"))
    QTimer.singleShot(7000, finish)


def finish():
    log("\n===== 全程日志（GUI 文本通道）=====")
    for line in win.log.toPlainText().splitlines():
        log("    " + line)
    log("\n===== 结束 =====")
    QTimer.singleShot(500, app.quit)


# ---------------- 时间轴 ----------------
def start():
    if not pick_port():
        app.quit()
        return
    click(win.btn_connect, "连接")
    QTimer.singleShot(4000, lambda: snap("01_D1_connected"))
    QTimer.singleShot(4500, lambda: click(win.chk_diag, "使能诊断（勾选）"))
    QTimer.singleShot(7000, lambda: snap("02_D3_enabled"))
    QTimer.singleShot(7500, lambda: click(win.chk_diag, "取消使能（取消勾选）"))
    QTimer.singleShot(10000, lambda: snap("03_D6_disabled"))
    QTimer.singleShot(10500, lambda: click(win.btn_get_diag, "查询故障状态"))
    QTimer.singleShot(13000, lambda: snap("04_D7_query_while_disabled"))
    QTimer.singleShot(13500, lambda: click(win.chk_diag, "重新使能（勾选）"))
    QTimer.singleShot(16500, lambda: snap("05_D8_reenabled"))
    QTimer.singleShot(17500, pause1)


QTimer.singleShot(800, start)
app.exec()
