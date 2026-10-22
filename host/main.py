"""
STM32 数据采集与故障监测终端 —— PC 上位机

运行：
    python main.py                # 真实串口
    python main.py --demo         # 无硬件演示模式（内置信号源，走同一套解析路径）

依赖见 requirements.txt。
"""

from __future__ import annotations

import csv
import math
import os
import random
import sys
from collections import deque
from datetime import datetime

import pyqtgraph as pg
from PySide6.QtCore import QObject, Qt, QTimer
from PySide6.QtGui import QColor, QFont, QFontDatabase
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from protocol import (
    Ack,
    Boot,
    Event,
    FrameParser,
    LEVEL_NAMES,
    Sample,
    Stat,
    T_ACK,
    T_BOOT,
    T_EVENT,
    T_SAMPLE,
    T_STAT,
    TYPE_NAMES,
    build_get_stat,
    build_reset,
    build_set_led,
    build_set_thr,
    decode_many,
    encode,
)

PLOT_POINTS = 1500          # 曲线保留的点数（100Hz 下约 15 秒）
PLOT_REFRESH_MS = 50        # 界面刷新周期（20fps，远低于采样率，避免 UI 拖慢解析）

LEVEL_COLORS = {
    0: "#2E7D32",   # NORMAL 绿
    1: "#B26A00",   # WARN   琥珀
    2: "#C62828",   # CRIT   红
}


# ============================== 演示数据源 ==============================
class DemoSource(QObject):
    """无硬件模式：按真实帧格式生成数据，走与串口完全相同的解析路径。"""

    def __init__(self, on_bytes):
        super().__init__()
        self._on_bytes = on_bytes
        self._t = 0
        self._seq = 0
        self._phase = 0.0
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self.level = 0
        self.warn, self.crit = 2400, 3000
        self._stat_acc = 0

    def start(self):
        self._timer.start(10)      # 100Hz

    def stop(self):
        self._timer.stop()

    def _tick(self):
        self._t += 10
        self._phase += 0.012

        # 慢速正弦 + 噪声，偶尔把幅度拉高触发告警
        amp = 1700
        if (self._t // 8000) % 3 == 2:
            amp = 2900
        base = 1650 + amp * 0.6 * math.sin(self._phase)
        raw = int(max(0, min(3300, base + random.randint(-25, 25))))
        filt = int(0.85 * getattr(self, "_last_filt", raw) + 0.15 * raw)
        self._last_filt = filt

        lvl = 0
        if filt >= self.crit:
            lvl = 2
        elif filt >= self.warn:
            lvl = 1
        if lvl != self.level:
            ev = encode(0x82, self._seq, self._pack_event(self._t, self.level, lvl, filt))
            self._seq = (self._seq + 1) & 0xFF
            self.level = lvl
            self._changes = getattr(self, "_changes", 0) + 1
            self._on_bytes(ev)

        payload = self._pack_sample(self._t, raw, filt, lvl, self._t // 10)
        self._on_bytes(encode(0x81, self._seq, payload))
        self._seq = (self._seq + 1) & 0xFF

        self._stat_acc += 1
        if self._stat_acc >= 100:
            self._stat_acc = 0
            self._on_bytes(encode(0x83, self._seq, self._pack_stat()))

    @staticmethod
    def _pack_sample(t, raw, filt, lvl, seq8):
        import struct
        return struct.pack("<IHHBB", t, raw, filt, lvl, seq8 & 0xFF)

    @staticmethod
    def _pack_event(t, frm, to, filt):
        import struct
        return struct.pack("<IBBH", t, frm, to, filt)

    def _pack_stat(self):
        import struct
        return struct.pack(
            "<IIIIIIIIHHB",
            self._t, self._t // 10, 0, 0, 0, 0, 0,
            getattr(self, "_changes", 0),
            self.warn, self.crit, self.level,
        )


# ================================ 主窗口 ================================
class MainWindow(QMainWindow):
    def __init__(self, demo: bool = False):
        super().__init__()
        self.demo = demo
        self.worker = None
        self.parser = FrameParser()
        self.cmd_parser = FrameParser()
        self.demo_src = None

        self.samples = 0
        self.expected_seq = None
        self.gap_count = 0
        self.offset = 0
        self.last_t = 0

        self.t_buf = deque(maxlen=PLOT_POINTS)
        self.raw_buf = deque(maxlen=PLOT_POINTS)
        self.filt_buf = deque(maxlen=PLOT_POINTS)

        self.csv_path = None
        self.csv_file = None
        self.csv_writer = None

        self._build_ui()
        self._refresh_ports()

        self.plot_timer = QTimer(self)
        self.plot_timer.timeout.connect(self._redraw)
        self.plot_timer.start(PLOT_REFRESH_MS)

        if demo:
            self._start_demo()

    # ------------------------------ 界面 ------------------------------
    def _build_ui(self):
        self.setWindowTitle("STM32 数据采集与故障监测终端 — 上位机 V1")
        self.resize(1180, 760)

        root = QWidget()
        self.setCentralWidget(root)
        outer = QVBoxLayout(root)

        # ---- 连接栏 ----
        bar = QHBoxLayout()
        bar.addWidget(QLabel("串口"))
        self.cb_port = QComboBox()
        self.cb_port.setMinimumWidth(120)
        bar.addWidget(self.cb_port)
        self.btn_refresh = QPushButton("刷新")
        self.btn_refresh.clicked.connect(self._refresh_ports)
        bar.addWidget(self.btn_refresh)

        bar.addWidget(QLabel("波特率"))
        self.cb_baud = QComboBox()
        self.cb_baud.addItems(["115200", "230400", "460800", "921600"])
        bar.addWidget(self.cb_baud)

        self.btn_connect = QPushButton("连接")
        self.btn_connect.clicked.connect(self._toggle_connect)
        bar.addWidget(self.btn_connect)

        self.lbl_conn = QLabel("未连接")
        self.lbl_conn.setStyleSheet("color:#C62828;font-weight:500;")
        bar.addWidget(self.lbl_conn)
        bar.addStretch(1)
        outer.addLayout(bar)

        # ---- 中部：曲线 + 控制面板 ----
        split = QSplitter(Qt.Horizontal)

        plot_box = QGroupBox("实时曲线")
        pv = QVBoxLayout(plot_box)
        pg.setConfigOptions(antialias=True)
        self.plot = pg.PlotWidget()
        self.plot.setBackground("#FFFFFF")
        self.plot.showGrid(x=True, y=True, alpha=0.25)
        self.plot.setLabel("bottom", "时间", units="s")
        self.plot.setLabel("left", "电压", units="mV")
        self.plot.setYRange(0, 3300)
        self.plot.addLegend(offset=(10, 10))
        self.curve_raw = self.plot.plot(pen=pg.mkPen("#90A4AE", width=1), name="原始值 raw")
        self.curve_filt = self.plot.plot(pen=pg.mkPen("#1565C0", width=2), name="滤波值 filt")
        self.line_warn = pg.InfiniteLine(angle=0, pen=pg.mkPen("#B26A00", width=1, style=Qt.DashLine))
        self.line_crit = pg.InfiniteLine(angle=0, pen=pg.mkPen("#C62828", width=1, style=Qt.DashLine))
        self.plot.addItem(self.line_warn)
        self.plot.addItem(self.line_crit)
        self.line_warn.setValue(2400)
        self.line_crit.setValue(3000)
        pv.addWidget(self.plot)
        split.addWidget(plot_box)

        split.addWidget(self._build_panel())
        split.setStretchFactor(0, 4)
        split.setStretchFactor(1, 1)
        outer.addWidget(split, stretch=1)

        # ---- 底部：事件 + 日志 ----
        tabs = QTabWidget()
        self.table = QTableWidget(0, 4)
        self.table.setHorizontalHeaderLabels(["时间(ms)", "变化", "滤波值(mV)", "记录时刻"])
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.setColumnWidth(0, 100)
        self.table.setColumnWidth(1, 190)
        self.table.setColumnWidth(2, 110)
        tabs.addTab(self.table, "告警事件")

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(2000)
        self.log.setFont(QFont("Consolas", 9))
        tabs.addTab(self.log, "日志 / 文本通道")
        tabs.setMaximumHeight(190)
        outer.addWidget(tabs)

    def _build_panel(self) -> QWidget:
        box = QGroupBox("状态与控制")
        lay = QVBoxLayout(box)

        self.lbl_level = QLabel("NORMAL")
        self.lbl_level.setAlignment(Qt.AlignCenter)
        self.lbl_level.setFixedHeight(44)
        self.lbl_level.setStyleSheet(
            "background:#2E7D32;color:#FFFFFF;font-size:18px;font-weight:500;border-radius:6px;"
        )
        lay.addWidget(self.lbl_level)

        form = QFormLayout()
        self.lbl_uptime = QLabel("-")
        self.lbl_samples = QLabel("-")
        self.lbl_drops = QLabel("-")
        self.lbl_errs = QLabel("-")
        self.lbl_changes = QLabel("-")
        self.lbl_gaps = QLabel("-")
        form.addRow("运行时间", self.lbl_uptime)
        form.addRow("采样数", self.lbl_samples)
        form.addRow("丢点", self.lbl_drops)
        form.addRow("异常(uart/rx/帧)", self.lbl_errs)
        form.addRow("状态切换", self.lbl_changes)
        form.addRow("序号缺口", self.lbl_gaps)
        lay.addLayout(form)

        thr = QGroupBox("阈值 (mV)")
        tf = QFormLayout(thr)
        self.sp_warn = QSpinBox()
        self.sp_warn.setRange(0, 3300)
        self.sp_warn.setValue(2400)
        self.sp_crit = QSpinBox()
        self.sp_crit.setRange(0, 3300)
        self.sp_crit.setValue(3000)
        tf.addRow("WARN", self.sp_warn)
        tf.addRow("CRIT", self.sp_crit)
        self.btn_apply = QPushButton("下发阈值")
        self.btn_apply.clicked.connect(self._apply_thr)
        tf.addRow(self.btn_apply)
        lay.addWidget(thr)

        row = QHBoxLayout()
        self.btn_stat = QPushButton("读取统计")
        self.btn_stat.clicked.connect(lambda: self._send(build_get_stat(self._next_seq())))
        self.btn_reset = QPushButton("清零")
        self.btn_reset.clicked.connect(lambda: self._send(build_reset(self._next_seq())))
        row.addWidget(self.btn_stat)
        row.addWidget(self.btn_reset)
        lay.addLayout(row)

        row2 = QHBoxLayout()
        self.btn_led_on = QPushButton("LED 亮")
        self.btn_led_on.clicked.connect(lambda: self._send(build_set_led(True, self._next_seq())))
        self.btn_led_off = QPushButton("LED 灭")
        self.btn_led_off.clicked.connect(lambda: self._send(build_set_led(False, self._next_seq())))
        row2.addWidget(self.btn_led_on)
        row2.addWidget(self.btn_led_off)
        lay.addLayout(row2)

        self.btn_csv = QPushButton("开始记录 CSV")
        self.btn_csv.setCheckable(True)
        self.btn_csv.toggled.connect(self._toggle_csv)
        lay.addWidget(self.btn_csv)

        lay.addStretch(1)
        return box

    # ---------------------------- 串口管理 ----------------------------
    def _refresh_ports(self):
        from link import list_ports
        ports = list_ports()
        cur = self.cb_port.currentText()
        self.cb_port.clear()
        self.cb_port.addItems(ports if ports else ["<未发现串口>"])
        if cur in ports:
            self.cb_port.setCurrentText(cur)

    def _toggle_connect(self):
        if self.worker is not None:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self.cb_port.currentText()
        if not port or port.startswith("<"):
            QMessageBox.warning(self, "提示", "没有可用串口，请先插入 USB-TTL 并点刷新。")
            return
        from link import SerialWorker
        self.worker = SerialWorker(port, int(self.cb_baud.currentText()))
        self.worker.frames_ready.connect(self._on_frames)
        self.worker.lines_ready.connect(self._on_lines)
        self.worker.connected.connect(self._on_connected)
        self.worker.disconnected.connect(self._on_disconnected)
        self.worker.error.connect(self._on_error)
        self.worker.start()
        self.btn_connect.setText("断开")

    def _disconnect(self):
        if self.worker is not None:
            self.worker.stop()
            self.worker.wait(800)
            self.worker = None
        self.btn_connect.setText("连接")
        self.lbl_conn.setText("未连接")
        self.lbl_conn.setStyleSheet("color:#C62828;font-weight:500;")

    def _on_connected(self, port):
        self.lbl_conn.setText("已连接 %s" % port)
        self.lbl_conn.setStyleSheet("color:#2E7D32;font-weight:500;")
        self._log("已连接 %s" % port)

    def _on_disconnected(self, msg):
        self.lbl_conn.setText("已断开")
        self.lbl_conn.setStyleSheet("color:#C62828;font-weight:500;")
        self._log("断开：%s（界面数据保留，重连后可继续）" % msg)
        self.worker = None
        self.btn_connect.setText("连接")

    def _on_error(self, msg):
        self._log("错误：%s" % msg)

    # ---------------------------- 演示模式 ----------------------------
    def _start_demo(self):
        self.demo_src = DemoSource(self._feed_demo)
        self.demo_src.start()
        self.lbl_conn.setText("演示模式（无硬件）")
        self.lbl_conn.setStyleSheet("color:#B26A00;font-weight:500;")
        self._log("演示模式：内置 100Hz 信号源，走的是一样的解析路径")

    def _feed_demo(self, data: bytes):
        self._on_frames(self.parser.feed(data))

    # ---------------------------- 数据接收 ----------------------------
    def _next_seq(self) -> int:
        return self.parser.frames & 0xFF

    def _send(self, data: bytes):
        if self.worker is not None:
            self.worker.send(data)
        elif self.demo_src is not None:
            self._handle_demo_cmd(data)
        else:
            self._log("未连接，命令未发送")

    def _handle_demo_cmd(self, data: bytes):
        # 演示模式下模拟设备应答，让按钮也有反馈
        frames = self.cmd_parser.feed(data)
        for msg_type, seq, payload in frames:
            if msg_type == 0x10 and len(payload) == 4:
                import struct
                w, c = struct.unpack("<HH", payload)
                self.demo_src.warn, self.demo_src.crit = w, c
                self.line_warn.setValue(w)
                self.line_crit.setValue(c)
                self._on_frames(self.parser.feed(encode(T_ACK, seq, bytes([0x10, 0]))))

    def _on_frames(self, frames):
        for obj in decode_many(frames):
            if isinstance(obj, Sample):
                self._on_sample(obj)
            elif isinstance(obj, Event):
                self._on_event(obj)
            elif isinstance(obj, Stat):
                self._on_stat(obj)
            elif isinstance(obj, Ack):
                self._on_ack(obj)
            elif isinstance(obj, Boot):
                self._log("设备启动：固件 %s，采样率 %dHz，滤波 %d 点"
                          % (obj.version_text, obj.sample_rate, obj.filter_depth))

        # 原始帧里若有解不出的类型，单独记一笔
        for msg_type, _seq, _payload in frames:
            if msg_type in (T_SAMPLE, T_EVENT, T_STAT, T_ACK, T_BOOT):
                continue
            self._log("未知帧类型 0x%02X (%s)" % (msg_type, TYPE_NAMES.get(msg_type, "?")))

        if self.parser.errors:
            self._log("解析错误累计 %d 次" % self.parser.errors)
            self.parser.errors = 0

    def _on_sample(self, s):
        self.samples += 1
        if self.last_t and s.t_ms < self.last_t - 1000:
            self.offset += 1 << 32
        self.last_t = s.t_ms
        x = (s.t_ms + self.offset) / 1000.0

        self.t_buf.append(x)
        self.raw_buf.append(s.raw_mv)
        self.filt_buf.append(s.filt_mv)

        # 序号缺口检测：payload 里的 seq8 应该逐个递增
        if self.expected_seq is not None:
            if s.seq8 != self.expected_seq:
                self.gap_count += (s.seq8 - self.expected_seq) & 0xFF
                self.lbl_gaps.setText(str(self.gap_count))
        self.expected_seq = (s.seq8 + 1) & 0xFF

        self._set_level(s.level)
        self.lbl_samples.setText(str(self.samples))

        if self.csv_writer is not None:
            self.csv_writer.writerow([
                datetime.now().isoformat(timespec="milliseconds"),
                s.t_ms, s.raw_mv, s.filt_mv, s.level_name,
            ])

    def _on_event(self, e):
        row = self.table.rowCount()
        self.table.insertRow(row)
        self.table.setItem(row, 0, QTableWidgetItem(str(e.t_ms)))
        self.table.setItem(row, 1, QTableWidgetItem(e.text))
        self.table.setItem(row, 2, QTableWidgetItem(str(e.filt_mv)))
        self.table.setItem(row, 3, QTableWidgetItem(datetime.now().strftime("%H:%M:%S")))
        self.table.scrollToBottom()
        self._log("告警变化：%s（filt=%dmV）" % (e.text, e.filt_mv))

    def _on_stat(self, st):
        self.lbl_uptime.setText("%.1f s" % (st.uptime_ms / 1000.0))
        self.lbl_samples.setText(str(st.samples))
        self.lbl_drops.setText(str(st.drops))
        self.lbl_errs.setText("%d / %d / %d" % (st.uart_err, st.rx_err, st.frame_err))
        self.lbl_changes.setText(str(st.changes))
        self.sp_warn.setValue(st.thr_warn)
        self.sp_crit.setValue(st.thr_crit)
        self.line_warn.setValue(st.thr_warn)
        self.line_crit.setValue(st.thr_crit)
        self._set_level(st.level)

    def _on_ack(self, a):
        name = TYPE_NAMES.get(a.req_type, "0x%02X" % a.req_type)
        self._log("应答 %s -> %s" % (name, a.result_name))

    def _on_lines(self, lines):
        for line in lines:
            self._log(line)

    # ------------------------------ 交互 ------------------------------
    def _apply_thr(self):
        w = self.sp_warn.value()
        c = self.sp_crit.value()
        if w <= 50 or c <= w or c > 3300:
            QMessageBox.warning(self, "参数不合法",
                                "需满足 50 < WARN < CRIT <= 3300（与固件校验一致）")
            return
        self._send(build_set_thr(w, c, self._next_seq()))

    def _set_level(self, level):
        name = LEVEL_NAMES.get(level, "?")
        self.lbl_level.setText(name)
        color = LEVEL_COLORS.get(level, "#455A64")
        self.lbl_level.setStyleSheet(
            "background:%s;color:#FFFFFF;font-size:18px;font-weight:500;border-radius:6px;" % color
        )

    def _toggle_csv(self, checked):
        if checked:
            path, _ = QFileDialog.getSaveFileName(
                self, "保存 CSV", "capture_%s.csv" % datetime.now().strftime("%Y%m%d_%H%M%S"),
                "CSV 文件 (*.csv)")
            if not path:
                self.btn_csv.setChecked(False)
                return
            self.csv_file = open(path, "w", newline="", encoding="utf-8-sig")
            self.csv_writer = csv.writer(self.csv_file)
            self.csv_writer.writerow(["pc_time", "t_ms", "raw_mv", "filt_mv", "level"])
            self.btn_csv.setText("停止记录")
            self._log("开始记录：%s" % path)
        else:
            if self.csv_file is not None:
                self.csv_file.close()
                self.csv_file = None
                self.csv_writer = None
                self.btn_csv.setText("开始记录 CSV")
                self._log("CSV 已保存")

    def _log(self, text):
        self.log.appendPlainText(text)

    def _redraw(self):
        if not self.t_buf:
            return
        self.curve_raw.setData(list(self.t_buf), list(self.raw_buf))
        self.curve_filt.setData(list(self.t_buf), list(self.filt_buf))
        self.plot.setXRange(max(0.0, self.t_buf[-1] - 15.0), self.t_buf[-1] + 0.2, padding=0)

    def closeEvent(self, event):        # noqa: N802
        if self.demo_src is not None:
            self.demo_src.stop()
        self._disconnect()
        if self.csv_file is not None:
            self.csv_file.close()
        super().closeEvent(event)


def apply_cjk_font(app: QApplication) -> str:
    """
    挑一个带中文字形的字体。

    分两步走：
      1. 直接注册系统字体文件。离屏渲染、远程桌面、精简系统里
         QFontDatabase.families() 可能为空，但字体文件本身还在，
         用 addApplicationFont() 注册后就能用。
      2. 实在找不到就退回系统默认，此时中文可能显示为方框（会有日志提示）。
    """
    for path in (r"C://Windows//Fonts//msyh.ttc",      # 微软雅黑
                 r"C://Windows//Fonts//msyhl.ttc",
                 r"C://Windows//Fonts//simhei.ttf",    # 黑体
                 r"C://Windows//Fonts//simsun.ttc"):   # 宋体
        if os.path.exists(path):
            fid = QFontDatabase.addApplicationFont(path)
            if fid >= 0:
                fams = QFontDatabase.applicationFontFamilies(fid)
                if fams:
                    app.setFont(QFont(fams[0], 9))
                    return fams[0]

    families = set(QFontDatabase.families())
    for name in ("Microsoft YaHei UI", "Microsoft YaHei", "SimHei",
                 "Noto Sans CJK SC", "WenQuanYi Micro Hei"):
        if name in families:
            app.setFont(QFont(name, 9))
            return name

    app.setFont(QFont(app.font().family(), 9))
    return app.font().family() + "（未找到中文字体，中文可能显示为方框）"


def main():
    demo = "--demo" in sys.argv
    app = QApplication(sys.argv)
    used = apply_cjk_font(app)
    print("界面字体：%s" % used)
    win = MainWindow(demo=demo)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
