"""
协议层：与固件 App/Src/app_proto.c 一一对应。

本模块不依赖 PySide6 / pyserial，可以单独 import 做单元测试，
这是"上位机大部分逻辑不需要硬件就能验证"的前提。

帧格式（小端）：
    +------+------+------+-----+-----+--------------+--------+--------+
    | 0xAA | 0x55 | TYPE | LEN | SEQ | PAYLOAD[LEN] | CRC_LO | CRC_HI |
    +------+------+------+-----+-----+--------------+--------+--------+

CRC16-CCITT（poly=0x1021, init=0xFFFF），覆盖 TYPE..PAYLOAD 最后一个字节。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Iterable, List, Tuple

SOF0 = 0xAA
SOF1 = 0x55
HEADER_LEN = 5
CRC_LEN = 2
MAX_PAYLOAD = 40

# 与固件 app_config.h 对齐的物理常量（由 tests/test_firmware_sync.py 校验，勿单边修改）
ADC_VREF_MV = 3300      # APP_ADC_VREF_MV
THR_HYST_MV = 50        # APP_THR_HYST_MV

# 设备 -> 上位机
T_SAMPLE = 0x81
T_EVENT = 0x82
T_STAT = 0x83
T_ACK = 0x84
T_BOOT = 0x85

# 上位机 -> 设备
T_SET_THR = 0x10
T_SET_LED = 0x11
T_GET_STAT = 0x12
T_RESET = 0x13
T_PING = 0x14

TYPE_NAMES = {
    T_SAMPLE: "SAMPLE",
    T_EVENT: "EVENT",
    T_STAT: "STAT",
    T_ACK: "ACK",
    T_BOOT: "BOOT",
    T_SET_THR: "SET_THR",
    T_SET_LED: "SET_LED",
    T_GET_STAT: "GET_STAT",
    T_RESET: "RESET",
    T_PING: "PING",
}

LEVEL_NAMES = {0: "NORMAL", 1: "WARN", 2: "CRIT"}

ACK_RESULTS = {0: "OK", 1: "参数错误", 2: "未知命令"}


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE，与固件 proto_crc16() 完全一致。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long: %d" % len(payload))
    head = bytes([SOF0, SOF1, msg_type & 0xFF, len(payload) & 0xFF, seq & 0xFF])
    body = head[2:] + payload
    crc = crc16(body)
    return head + payload + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


# ------------------------------ 载荷解析 ------------------------------
@dataclass
class Sample:
    t_ms: int
    raw_mv: int
    filt_mv: int
    level: int
    seq8: int

    @property
    def level_name(self) -> str:
        return LEVEL_NAMES.get(self.level, "?")


@dataclass
class Event:
    t_ms: int
    level_from: int
    level_to: int
    filt_mv: int

    @property
    def text(self) -> str:
        return "%s -> %s" % (
            LEVEL_NAMES.get(self.level_from, "?"),
            LEVEL_NAMES.get(self.level_to, "?"),
        )


@dataclass
class Stat:
    uptime_ms: int
    samples: int
    drops: int
    adc_err: int
    uart_err: int
    rx_err: int
    frame_err: int
    changes: int
    thr_warn: int
    thr_crit: int
    level: int

    @property
    def level_name(self) -> str:
        return LEVEL_NAMES.get(self.level, "?")


@dataclass
class Ack:
    req_type: int
    result: int

    @property
    def result_name(self) -> str:
        return ACK_RESULTS.get(self.result, "未知(%d)" % self.result)


@dataclass
class Boot:
    fw_version: int
    sample_rate: int
    dma_buf: int
    filter_depth: int
    decim: int

    @property
    def version_text(self) -> str:
        return "V%d.%d" % (self.fw_version >> 8, self.fw_version & 0xFF)


def parse_sample(p: bytes) -> Sample:
    if len(p) != 10:
        raise ValueError("SAMPLE len %d != 10" % len(p))
    t_ms, raw, filt, level, seq8 = struct.unpack("<IHHBB", p)
    return Sample(t_ms, raw, filt, level, seq8)


def parse_event(p: bytes) -> Event:
    if len(p) != 8:
        raise ValueError("EVENT len %d != 8" % len(p))
    t_ms, frm, to, filt = struct.unpack("<IBBH", p)
    return Event(t_ms, frm, to, filt)


def parse_stat(p: bytes) -> Stat:
    if len(p) != 37:
        raise ValueError("STAT len %d != 37" % len(p))
    vals = struct.unpack("<IIIIIIIIHHB", p)
    return Stat(*vals)


def parse_ack(p: bytes) -> Ack:
    if len(p) != 2:
        raise ValueError("ACK len %d != 2" % len(p))
    return Ack(p[0], p[1])


def parse_boot(p: bytes) -> Boot:
    if len(p) != 7:
        raise ValueError("BOOT len %d != 7" % len(p))
    ver, rate, buf, depth, decim = struct.unpack("<HHBBB", p)
    return Boot(ver, rate, buf, depth, decim)


# ------------------------------ 命令构造 ------------------------------
def build_set_thr(warn_mv: int, crit_mv: int, seq: int) -> bytes:
    return encode(T_SET_THR, seq, struct.pack("<HH", warn_mv, crit_mv))


def build_set_led(on: bool, seq: int) -> bytes:
    return encode(T_SET_LED, seq, bytes([1 if on else 0]))


def build_get_stat(seq: int) -> bytes:
    return encode(T_GET_STAT, seq, b"")


def build_reset(seq: int) -> bytes:
    return encode(T_RESET, seq, b"")


def build_ping(seq: int) -> bytes:
    return encode(T_PING, seq, b"")


# ------------------------------ 增量解析 ------------------------------
class FrameParser:
    """
    增量解析器：喂多少字节都行，内部自己缓冲。

    除了二进制帧，设备还会在文本通道上输出 ASCII 行（help/stat 的应答）。
    这些行的首字节不是 0xAA，所以会被归到 text_lines，界面可以单独显示。
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self._text = bytearray()
        self.errors = 0
        self.frames = 0
        self.text_lines: List[str] = []

    def reset(self) -> None:
        self._buf.clear()
        self._text.clear()
        self.text_lines.clear()

    def _push_text(self, data: bytes) -> None:
        self._text.extend(data)
        while True:
            nl = -1
            for i, b in enumerate(self._text):
                if b in (0x0A, 0x0D):
                    nl = i
                    break
            if nl < 0:
                break
            line = bytes(self._text[:nl])
            del self._text[: nl + 1]
            if line:
                self.text_lines.append(line.decode("ascii", errors="replace"))

    def feed(self, data: bytes) -> List[Tuple[int, int, bytes]]:
        """返回本次解析出的完整帧列表 [(type, seq, payload)]"""
        self._buf.extend(data)
        frames: List[Tuple[int, int, bytes]] = []

        while True:
            idx = self._buf.find(bytes([SOF0, SOF1]))
            if idx < 0:
                # 没有帧头：剩下的全是文本；只保留末尾可能是半个帧头的 0xAA
                if self._buf and self._buf[-1] == SOF0:
                    self._push_text(bytes(self._buf[:-1]))
                    del self._buf[:-1]
                else:
                    self._push_text(bytes(self._buf))
                    self._buf.clear()
                break

            if idx > 0:
                self._push_text(bytes(self._buf[:idx]))
                del self._buf[:idx]

            if len(self._buf) < HEADER_LEN:
                break

            length = self._buf[3]
            if length > MAX_PAYLOAD:
                self.errors += 1
                del self._buf[0]
                continue

            total = HEADER_LEN + length + CRC_LEN
            if len(self._buf) < total:
                break

            frame = bytes(self._buf[:total])
            crc_recv = frame[-2] | (frame[-1] << 8)
            crc_calc = crc16(frame[2 : HEADER_LEN + length])
            if crc_recv != crc_calc:
                self.errors += 1
                del self._buf[0]      # 从下一个字节重新找帧头
                continue

            frames.append((frame[2], frame[4], frame[5 : HEADER_LEN + length]))
            self.frames += 1
            del self._buf[:total]

        return frames

    def drain_lines(self) -> List[str]:
        lines = self.text_lines
        self.text_lines = []
        return lines


def decode_many(frames: Iterable[Tuple[int, int, bytes]]):
    """把原始帧转成具体消息对象，未知类型跳过。类型不匹配不抛异常，只记录。"""
    out = []
    for msg_type, seq, payload in frames:
        try:
            if msg_type == T_SAMPLE:
                out.append(parse_sample(payload))
            elif msg_type == T_EVENT:
                out.append(parse_event(payload))
            elif msg_type == T_STAT:
                out.append(parse_stat(payload))
            elif msg_type == T_ACK:
                out.append(parse_ack(payload))
            elif msg_type == T_BOOT:
                out.append(parse_boot(payload))
        except ValueError:
            continue
    return out
