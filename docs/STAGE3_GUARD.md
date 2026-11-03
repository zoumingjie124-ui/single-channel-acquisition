# 阶段三补充加固：A（parser 超时复位）+ B（RX 重挂守护）

> 日期：**2026-11-03 09:12 – 09:23**
> 参数：`APP_RX_FRAME_TIMEOUT_MS = 50`、`APP_RX_RESTART_INTERVAL_MS = 200`
> 原始记录：`docs/STAGE3_GUARD_RAW.txt`（A/B 共 12 项）、`docs/STAGE3_RAW.txt`（T1–T9 回归）
> 工具：`host/tools/uart_guard.py`（新增）、`host/tools/uart_fault.py`（回归）

| 项目 | 结论 |
|---|---|
| 阶段三主体（T1–T9） | **PASS**（回归重跑，见 §4） |
| A. parser 空闲超时复位 | **PASS**（A1–A6 全通过，见 §1.5） |
| B. RX 重挂守护 | **PASS**（B1–B6 全通过，见 §2.5） |
| 8 小时长跑 / 真实硬件重挂失败 | **NOT TESTED**（见 §5） |

---

## 1. A：解析器空闲超时复位

### 1.1 修改前的问题

`proto_parser_feed()` 是纯状态机，**没有时间概念**。停在 `PROTO_P_BODY` 时，只要没有新字节就永远等下去。

阶段三 T3 实测代价：一个半截帧**确定性吃掉后续 2 帧**，而且期间文本通道也被吞（响应字节被当成载荷）。

### 1.2 阈值依据（50 ms 是怎么定的）

| 约束 | 数值 | 说明 |
|---|---|---|
| 最长合法帧 | 47 B（2 + 3 + 40 + 2） | 帧头 2、TYPE/LEN/SEQ 3、载荷 40、CRC 2 |
| 线上时间 @115200 | **4.08 ms** | 47 × 10 bit ÷ 115200 |
| USB 单包 | 64 B > 47 B | 一帧必然落在同一个 CH340 包内，包内字节连续 |
| 50 ms ÷ 4.08 ms | **12.3 倍余量** | 正常帧不可能被判超时 |
| 50 ms 内可传字节 | 576 B | 反证：真有 50 ms 静默，必然是帧断了 |

### 1.3 改动点

| 文件 | 改动 |
|---|---|
| `App/Inc/app_config.h` | 新增 `APP_RX_FRAME_TIMEOUT_MS 50u` |
| `App/Inc/app_cmd.h` | `app_cmd_poll(void)` → `app_cmd_poll(uint32_t now_ms)`；新增 `app_cmd_parser_timeout_count()` |
| `App/Src/app_cmd.c` | 新增 `s_last_byte_ms` / `s_parser_tmo`；poll 入口做超时判定；每消费一字节刷新时间戳 |
| `App/Src/app_task.c` | 调用点传 `s_ctx.tick_ms`；`ERRSRC` 行加 `pto=` |

关键代码（`app_cmd.c`）：

```c
void app_cmd_poll(uint32_t now_ms)
{
  /* 半截帧超时判定必须放在喂字节**之前**：
     否则新到的一整帧会先被塞进那个卡住的旧状态里，等处理完再判超时就晚了
     （那时 s_last_byte_ms 刚被更新，永远不会触发）。 */
  if ((proto_parser_is_idle(&s_parser) == 0u) &&
      ((uint32_t)(now_ms - s_last_byte_ms) > APP_RX_FRAME_TIMEOUT_MS))
  {
    proto_parser_init(&s_parser);
    s_parser_tmo++;
  }

  while (s_rx_tail != s_rx_head)
  {
    ...
    s_last_byte_ms = now_ms;      /* 有字节在动就不算超时 */
    ...
  }
}
```

> 这个"判定必须放在喂字节之前"是设计要点，不是随手写的顺序。放在后面的话，
> 新帧的字节会先污染旧状态，然后时间戳被刷新 → 超时永远不触发，等于没做。

### 1.4 副作用检查（逐条核对）

| 担心 | 结论 |
|---|---|
| 会误清已完成的帧吗 | 不会。`proto_parser_feed` 在 CRC_HI 里**先** `state = PROTO_P_SOF0` **再**回调 `on_frame`，回调时 parser 已是 idle |
| 会影响已入队的合法帧吗 | 不会。只复位 parser 结构体，不碰 `s_rx_ring` / `head` / `tail` |
| `reset` 命令清零统计会误触发吗 | 不会。`app_cmd_reset_counters()` **故意不清** `s_last_byte_ms`，否则若此刻正处在半帧中间，清零会立刻满足超时条件 |
| 每字节开销 | 一次 32 位减法 + 比较，约 4 个周期。满速 11520 B/s → CPU 占用 < 0.001% |

### 1.5 A 组测试结果（12 项中的 6 项）

| 编号 | 用例 | 实测 | 判定 |
|---|---|---|---|
| A1 | 正常完整帧 ×10 | ACK **10/10**，`pto +0` | PASS |
| A2 | 半截帧 + 等 100 ms + 1 帧 | **ACK**，`pto +1`（加固前丢 2 帧） | PASS |
| A3 | 一帧分 3 段、段间 15 ms | **ACK**，`pto +0`（未误判） | PASS |
| A4 | 背靠背 20 帧 | ACK **20/20**，`pto +0` | PASS |
| A5 | 边界：等 45 ms / 62 ms | 45 ms → **丢失**（未超时，符合预期）；62 ms → **ACK** | PASS |
| A6 | 连续 5 次「半截帧 + 超时」 | 恢复 **5/5**，`pto +5` | PASS |

A5 是这组最有价值的一条：**45 ms 不触发、62 ms 触发**，说明 50 ms 的边界确实在起作用，不是"设了个很大值看起来永远不误伤"。

### 1.6 A 项判定：**PASS**

---

## 2. B：RX 重挂守护

### 2.1 修改前的问题

`usart_start_rx()` 只在两个中断回调里被调用，失败时仅 `s_rx_fail++`，**没有任何重试**。

后果：TX 照常发 SAMPLE（上位机能画曲线），但命令通道永久死掉——最容易被误判成"上位机卡了"。

### 2.2 设计

| 项 | 选择 | 理由 |
|---|---|---|
| 检测依据 | `huart1.RxState != HAL_UART_STATE_BUSY_RX` | 计数器只能说"曾经失败过"，说明不了"现在在不在收" |
| 检查位置 | `app_task_run()` 第 5.5 步（ADC 恢复之后） | 避免同一轮里两次阻塞操作 |
| 限速 | 首次立即，其后 `APP_RX_RESTART_INTERVAL_MS = 200` | 防止接收已死时每轮都启动 DMA |
| 并发保护 | PRIMASK 临界区内"二次检查 + 启动" | 与两个 ISR 里的重挂互斥，不会重复启动 |
| 返回码 | `NONE / OK / FAIL`，主循环据此打日志 | 与 ADC 恢复对称 |

关键代码（`usart.c`）：

```c
uint8_t usart_rx_guard(uint32_t now_ms)
{
  if (huart1.RxState == HAL_UART_STATE_BUSY_RX)  return USART_RX_GUARD_NONE;

  if ((uint32_t)(now_ms - s_guard_ms) < APP_RX_RESTART_INTERVAL_MS)
    return USART_RX_GUARD_NONE;                  /* 限速 */

  primask = __get_PRIMASK(); __disable_irq();
  if (huart1.RxState != HAL_UART_STATE_BUSY_RX)  /* 二次检查：ISR 可能刚挂好 */
  {
    s_guard_ms = now_ms;
    s_rst_try++;
    if (usart_start_rx() != 0u) { s_rst_ok++;  r = USART_RX_GUARD_OK;   }
    else                        { s_rst_fail++; r = USART_RX_GUARD_FAIL; }
  }
  __set_PRIMASK(primask);
  return r;
}
```

### 2.3 并发与重复启动（重点核对）

| 担心 | 结论 |
|---|---|
| 会不会重复启动 DMA | 临界区内二次检查 `RxState`，ISR 若已挂好会被跳过 |
| 会不会与 ISR 抢 | 启动调用被 PRIMASK 包住，ISR 无法插入 |
| 临界区有多长 | `HAL_UARTEx_ReceiveToIdle_DMA` 约 2–5 µs；只在实际重挂时进入，正常情况每轮只做一次状态比较 |
| 会不会与 ADC 恢复冲突 | 顺序执行（5 步 ADC → 5.5 步 RX），不嵌套 |

### 2.4 注入手段

新增调试命令 `rxstop`（`#if APP_DEBUG_INJECT` 包住）：调用 `HAL_UART_AbortReceive()` 后**不重挂**。

这正是"某次重挂失败之后"的真实状态：`RxState = READY`、DMA 已 abort、接收停止。守护不关心原因，只看状态，所以用它验证等价。

### 2.5 B 组测试结果（12 项中的 6 项）

| 编号 | 用例 | 实测 | 判定 |
|---|---|---|---|
| B1 | 单次 `rxstop` | 恢复 **152 ms**，`rxrst ok +1` | PASS |
| B2 | 连续 3 次 | 恢复 **3/3**，耗时 152 / 152 / 151 ms | PASS |
| B3 | 50 ms 内两次 `rxstop` | 恢复 **322 ms**，`rxrst ok +2`（两次窗口） | PASS |
| B4 | 恢复后 10 连发 | ACK **10/10** | PASS |
| B5 | `rxstop` 后 300 ms 内 SAMPLE | **30 帧**（≈100 Hz）→ 证明 TX 正常、RX 停 | PASS |
| B6 | 3 分钟、每 20 s 注入一次 | 恢复 **9/9**，`rxrst +9 / fail +0`，解析错误 0，结束后 5/5 | PASS |

**152 ms 不是"慢"，而是限速的真实体现**：`s_guard_ms` 记的是上一次尝试的时刻，若 `rxstop` 落在窗口的 50 ms 处，就要再等约 150 ms。B3 更直接——50 ms 内两次中止，第二次必须等下一个 200 ms 窗口，总耗时 322 ms。

B5 是这组的关键：**接收停着的时候数据还在往外发**，正是这个缺陷最阴的地方，现在能被守护在 200 ms 内救回来。

### 2.6 B 项判定：**PASS**

---

## 3. 资源占用与实时性

| 项 | 修改前 | 修改后 | 变化 |
|---|---:|---:|---:|
| Release Code | 20876 | **21208** | **+332 B** |
| RO-data | 996 | 996 | 0 |
| RW-data | 124 | **148** | **+24 B** |
| ZI-data | 4916 | 4916 | 0 |
| Debug Code（含 adcinject/oreinject/rxstop） | 21364 | **21792** | +428 B |

实时性影响：

| 项 | 开销 |
|---|---|
| A：每字节一次减法比较 | 约 4 周期；满速 11520 B/s → **< 0.001% CPU** |
| B：每轮主循环一次状态比较 | 约 3 周期，主循环约 1–2 kHz → 可忽略 |
| B：实际重挂时的临界区 | 2–5 µs，只在异常恢复那一刻发生 |

---

## 4. 回归结果

### 4.1 阶段三 9 用例（正式版固件重跑）

| 用例 | 首轮（08:58） | 加固后（09:22） | 变化 |
|---|---|---|---|
| T1 CRC ×20 | `frm +20` | `frm +20` | 一致 |
| T2 长度非法 | `frm +3` | `frm +3` | 一致 |
| **T3 半截帧** | **收帧 1/3** | **收帧 3/3，`frm +0`** | **A 项直接见效** |
| T4 未知命令 | `result=2` | `result=2` | 一致 |
| T5 非法文本 | 1 行 ERR | 1 行 ERR | 一致 |
| T6 超长文本 | 回显 84 B | 回显 84 B | 一致 |
| T7 连续流 TC | 1 行 ERR，`frm +0` | 1 行 ERR，`frm +0` | 一致 |
| T8 队列灌满 | STAT 14 / txd 11333 | STAT 12 / txd 15558 | 队列时序波动，**解析错误均 0** |
| T9 FE 风暴 | `uart +144`，`rxf +0` | `uart +144`，`rxf +0` | 一致 |

**T3 从 1/3 变 3/3 是 A 项最直接的证据**：半截帧之后不再连锁吃帧。

### 4.2 上位机单元测试

```
Ran 28 tests — OK
```

### 4.3 采样频率

正式版烧回后实测：**2 s / 200 帧 = 100.0 Hz**。

---

## 5. 未验证项（诚实清单）

| 项 | 为什么没做 | 状态 |
|---|---|---|
| 8 小时长跑 | 只跑了 3 分钟注入 + 日常运行 | **NOT TESTED** |
| 真实硬件层面的重挂失败 | 本次用 `HAL_UART_AbortReceive` 模拟"未在接收"的状态，不是 HAL 真的返回 BUSY | **NOT TESTED** |
| 其他超时参数（20 / 100 ms）的表现 | 只按确认值 50 ms 验证 | **NOT TESTED** |
| 接收中止瞬间 DMA 缓冲里的半帧去向 | abort 会丢弃在途数据，属预期，未单独量化 | **NOT TESTED** |
| PySide6 界面与本轮改动的长时联调 | GUI 未参与本轮 | **NOT TESTED** |
| 上位机对新增 `ERRSRC` 字段的解析 | 目前只有串口助手/脚本读，GUI 不读该行 | **NOT TESTED** |

---

## 6. 复现

```bat
:: A + B 全部 12 项（需 APP_DEBUG_INJECT=1 的调试固件，约 4 分钟）
python host\tools\uart_guard.py COM6

:: 阶段三 9 项回归（正式固件即可，约 25 秒）
python host\tools\uart_fault.py COM6
```

判读要点：

| 现象 | 含义 |
|---|---|
| A2 收不到 ACK 且 `pto` 不涨 | 超时没生效 |
| A3 `pto` 增长 | 阈值太小，合法分段被误判 |
| A5 的 45 ms 也 ACK | 超时判定实际没按 50 ms 走 |
| B1 恢复 > 450 ms | 限速过大或守护没在跑 |
| B6 `rxrst fail` 增长 | 重挂本身失败，需查 HAL 返回值 |
| B5 的 SAMPLE 掉到 0 | TX 也被拖死（比原缺陷更严重） |
