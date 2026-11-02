# -*- coding: utf-8 -*-
"""HT（半传输）修复验证：长文本命令是否被重复交付 / 是否误加 rx_err。

背景
----
`HAL_UARTEx_ReceiveToIdle_DMA` 在 DMA 缓冲填到一半（64 字节的一半 = 32 字节）时
会触发 HT 事件。修前的代码在 HT 分支里同样调用了 `app_cmd_rx_isr()` 并 `rearm`：

  1. 重复交付：同一批字节被当两次数据交给命令层 -> 一条命令被执行两次；
  2. rearm 时 `RxState == BUSY_RX`，`HAL_UARTEx_ReceiveToIdle_DMA()` 返回 BUSY
     -> `s_rx_fail++`，而这个值会出现在 STAT 的 `rxf=` 里（上位机显示为 rx_err）。

修复后 HT 分支直接 return，数据只在 IDLE/TC 上交。

判定依据（不需要示波器）
------------------------
一条未知文本命令必然回一行 `ERR unknown command: ...`：

  * 发 N 条 -> 应收到 N 行 ERR。收到 2N 行 = 重复交付（HT 缺陷还在）
  * 发 N 条 -> `rxf` 增量应为 0。有增量 = rearm 撞 BUSY（HT 缺陷还在）

同时对比"短命令"（低于 32 字节，不触发 HT）与"长命令"（48 字节，必然触发 HT），
两者行为应完全一致。

用法::

    python host/tools/htcheck.py            # 自动选第一个串口
    python host/tools/htcheck.py COM5
"""

import os
import re
import sys
import time

import serial
from serial.tools import list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(PROJECT, "host"))

HT_BOUNDARY = 32          # APP_RX_DMA_BUF_LEN(64) / 2
SHORT_LEN = 8             # 明显低于半传输边界
LONG_LEN = 48             # 明显高于半传输边界
REPEAT = 10
GAP = 0.06                # 两条命令之间留出 IDLE 触发时间

STAT_RE = re.compile(r"STAT t=(\d+)ms n=(\d+) drop=(\d+) adc=(\d+) uart=(\d+) "
                     r"rxf=(\d+) frm=(\d+) chg=(\d+) cmd=(\d+)/(\d+) txd=(\d+)B")


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    if not ports:
        print("未检测到任何串口")
        raise SystemExit(2)
    return ports[0]


class Link:
    """串口封套：按"整段原始字节"统计，避免二进制帧的可见字节打断文本行。

    二进制 SAMPLE/STAT 帧和文本响应共用一条串口，逐行切分会把帧里的
    可见字节（0x20~0x7E）混进文本。这里不做切分：
      * 数 ERR：直接在原始字节里数子串 b"ERR unknown command"
      * 读 STAT：在原始字节里找 b"STAT t=" 到 b"\r\n" 这一段
    文本行由发送队列整段发出，在线上是连续的，所以子串匹配可靠。
    """

    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.1)
        self.ser.reset_input_buffer()

    def write(self, data):
        self.ser.write(data)

    def read(self, seconds):
        deadline = time.time() + seconds
        buf = b""
        while time.time() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk
        return buf

    def stat(self):
        self.read(0.2)                      # 丢掉旧残留
        self.write(b"stat\r\n")
        raw = self.read(0.9)
        i = raw.find(b"STAT t=")
        if i < 0:
            return None
        j = raw.find(b"\r\n", i)
        if j < 0:
            j = len(raw)
        line = raw[i:j].decode("ascii", "replace")
        m = STAT_RE.search(line)
        if not m:
            return None
        g = m.groups()
        return {
            "uptime": int(g[0]), "n": int(g[1]), "drop": int(g[2]),
            "adc": int(g[3]), "uart": int(g[4]), "rxf": int(g[5]),
            "frm": int(g[6]), "chg": int(g[7]),
            "cmd_ok": int(g[8]), "cmd_err": int(g[9]), "txd": int(g[10]),
        }


def phase(link, length):
    """发 REPEAT 条指定长度的未知命令，返回原始字节（供调用方数 ERR）。"""
    body = "q" * (length - 1)
    raw = b""
    for i in range(REPEAT):
        link.write(("%s%d\r\n" % (body, i % 10)).encode())
        time.sleep(GAP)
        raw += link.read(GAP)
    raw += link.read(0.5)                   # 收尾，取回最后一条的响应
    return raw


def count_err(raw):
    return raw.count(b"ERR unknown command")


def first_err_line(raw):
    """取第一条 ERR 的整行（不含 CRLF），用于检查回显是否被重复拼接。"""
    i = raw.find(b"ERR unknown command")
    if i < 0:
        return b""
    j = raw.find(b"\r\n", i)
    return raw[i:j if j > 0 else len(raw)]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else pick_port()
    print("串口: %s @115200" % port)

    link = Link(port)
    time.sleep(0.3)
    link.read(0.3)

    base = link.stat()
    if base is None:
        print("拿不到 STAT 文本行，检查串口是否被占用 / 板子是否在发数据")
        raise SystemExit(1)

    print("\n基线: uart=%d rxf=%d frm=%d cmd=%d/%d"
          % (base["uart"], base["rxf"], base["frm"],
             base["cmd_ok"], base["cmd_err"]))

    print("\n阶段 A：%d 条短命令（%d 字符，低于半传输边界 %d）"
          % (REPEAT, SHORT_LEN, HT_BOUNDARY))
    raw_a = phase(link, SHORT_LEN)
    errs_a = count_err(raw_a)
    echo_a = first_err_line(raw_a)
    mid = link.stat()

    print("阶段 B：%d 条长命令（%d 字符，必然触发 HT）" % (REPEAT, LONG_LEN))
    raw_b = phase(link, LONG_LEN)
    errs_b = count_err(raw_b)
    echo_b = first_err_line(raw_b)
    end = link.stat()

    if mid is None or end is None:
        print("STAGE FAIL: 中途拿不到 STAT，链路可能已停摆")
        raise SystemExit(1)

    d_uart = end["uart"] - base["uart"]
    d_rxf = end["rxf"] - base["rxf"]
    d_frm = end["frm"] - base["frm"]

    print("\n===== 结果 =====")
    # 回显长度 = len("ERR unknown command: ") + 命令长度
    #   A: 21 + 8  = 29
    #   B: 21 + 48 = 69
    # 若 HT 重复交付，同一行会被拼两遍并被 64 字节行缓冲截断 -> B 变成 84 字节
    exp_a = 21 + SHORT_LEN
    exp_b = 21 + LONG_LEN

    print("短命令 ERR 行 : %d / 应为 %d   (重复交付则为 %d)"
          % (errs_a, REPEAT, REPEAT * 2))
    print("长命令 ERR 行 : %d / 应为 %d   (重复交付则为 %d)"
          % (errs_b, REPEAT, REPEAT * 2))
    print("短命令回显长度: %d / 应为 %d" % (len(echo_a), exp_a))
    print("长命令回显长度: %d / 应为 %d   (重复交付会被拼成 84 并截断)" % (len(echo_b), exp_b))
    print("uart_err 增量 : %+d" % d_uart)
    print("rxf     增量 : %+d   (每出现一次 rearm BUSY 会 +1)" % d_rxf)
    print("frm     增量 : %+d" % d_frm)
    print("阶段 A 后 rxf : %d    阶段 B 后 rxf : %d" % (mid["rxf"], end["rxf"]))
    print("阶段 A 后 uart: %d    阶段 B 后 uart: %d" % (mid["uart"], end["uart"]))

    ok = True
    if errs_a != REPEAT:
        print("FAIL: 短命令回 ERR 次数不符（重复交付或丢命令）")
        ok = False
    if errs_b != REPEAT:
        print("FAIL: 长命令回 ERR 次数不符（重复交付或丢命令）")
        ok = False
    if d_rxf != 0:
        print("FAIL: rxf 增长 %d，说明 rearm 撞了 BUSY —— HT 修复未生效" % d_rxf)
        ok = False
    if d_uart != 0:
        print("FAIL: uart_err 增长 %d（ORE/FE/NE 被触发）" % d_uart)
        ok = False
    if (mid["uart"], mid["rxf"]) != (base["uart"], base["rxf"]):
        print("FAIL: 阶段 A（短命令）就已经污染计数")
        ok = False
    if len(echo_a) != exp_a:
        print("FAIL: 短命令回显长度不符（%d，应为 %d）" % (len(echo_a), exp_a))
        ok = False
    if len(echo_b) != exp_b:
        print("FAIL: 长命令回显长度不符（%d，应为 %d）——命令被重复拼接了"
              % (len(echo_b), exp_b))
        ok = False

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
