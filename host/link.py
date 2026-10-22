"""
串口连接线程。

为什么要单独一个线程：
    串口 read() 是阻塞的，放主线程会把界面卡住。这里用 QThread 读数据，
    通过信号把解析结果丢回主线程，Qt 的信号槽会保证槽函数在主线程执行。

线程只做两件事：读字节 -> 喂给 FrameParser -> 发信号。
任何界面操作、文件写入都在主线程做。
"""

from __future__ import annotations

import queue
from typing import List, Optional

import serial
from PySide6.QtCore import QThread, Signal

from protocol import FrameParser


class SerialWorker(QThread):
    frames_ready = Signal(list)     # List[Tuple[type, seq, payload]]
    lines_ready = Signal(list)      # List[str]  文本通道
    connected = Signal(str)
    disconnected = Signal(str)
    error = Signal(str)

    def __init__(self, port: str, baud: int, parent=None) -> None:
        super().__init__(parent)
        self.port = port
        self.baud = baud
        self._running = False
        self._outq: "queue.Queue[bytes]" = queue.Queue()
        self._ser: Optional[serial.Serial] = None
        self.parser = FrameParser()

    # ------------------------- 对外接口（主线程调用） -------------------------
    def send(self, data: bytes) -> None:
        if self._running:
            self._outq.put(data)

    def stop(self) -> None:
        self._running = False

    # ----------------------------- 线程主体 -----------------------------
    def run(self) -> None:
        try:
            self._ser = serial.Serial(self.port, self.baud, timeout=0.05)
        except Exception as exc:                      # noqa: BLE001
            self.disconnected.emit("打开 %s 失败：%s" % (self.port, exc))
            return

        self._running = True
        self.connected.emit(self.port)
        self.parser.reset()

        while self._running:
            # 1) 发送
            try:
                while True:
                    payload = self._outq.get_nowait()
                    self._ser.write(payload)
            except queue.Empty:
                pass
            except Exception as exc:                  # noqa: BLE001
                self.error.emit("发送失败：%s" % exc)
                break

            # 2) 接收
            try:
                data = self._ser.read(4096)
            except Exception as exc:                  # noqa: BLE001
                self.disconnected.emit("串口读取失败：%s" % exc)
                break

            if data:
                frames = self.parser.feed(data)
                if frames:
                    self.frames_ready.emit(frames)
                lines = self.parser.drain_lines()
                if lines:
                    self.lines_ready.emit(lines)

        try:
            if self._ser is not None and self._ser.is_open:
                self._ser.close()
        except Exception:                             # noqa: BLE001
            pass
        self._ser = None
        self._running = False
        self.disconnected.emit("已断开 %s" % self.port)


def list_ports() -> List[str]:
    from serial.tools import list_ports as lp
    return [p.device for p in lp.comports()]
