# Stage 7C-2 交付报告：GET_DIAG 状态快照与 TX 失败恢复

> 日期：2026-11-03 20:0x – 23:2x
> 依据：设计文档 `STAGE7C_GET_DIAG_DESIGN.md`（已批准，含 6 条确认决策）
> 范围：新增 `0x16 GET_DIAG`、`event=0` 快照、发送层 `active/reported` 双状态与重试。
>
> **未改**：`app_diag.c`（算法与阈值）、`app_filter` / `app_alarm` / `app_measure`、
> ADC/DMA/定时器、SAMPLE(10B) / STAT(37B) 长度、全局 SEQ 规则、其他故障类型。

---

## 0. 一个必须先纠正的术语

设计评审时你指出：`reported` **不能**叫"上位机已知状态"。
代码与文档已按修正后的定义写：

| 字段 | 准确含义 |
|---|---|
| `active` | 设备当前真实故障状态 |
| `reported` | **已成功提交到 TX 发送队列**的故障状态 |
| 上位机是否收到 | 本模块**不掌握**；`app_tx_write()` 成功 ≠ DMA 发完 ≠ 对端收到 |

正因为 `reported` 只到"入队"为止，才仍然需要 `GET_DIAG` 快照来重新对齐。这条已写进
`app_fault.h` 的文件头注释，避免后来人又把它读成"已送达"。

---

## 1. 修改文件清单及原因

| 文件 | 状态 | 改了什么 | 为什么 |
|---|---|---|---|
| `App/Inc/app_proto.h` | 改 | `PROTO_T_GET_DIAG 0x16`、`PROTO_LEN_GET_DIAG 0`、`PROTO_FAULT_EV_SNAPSHOT 0` | 新增查询命令与快照取值；FAULT 帧结构不变 |
| `App/Inc/app_fault.h` | 改 | 结构体加 `reported / has_tried / last_try_ms / retry_fail / commit_cnt`；`update` 拆成 `set_state` + `service`；emit 回调返回值 `void`→`uint8_t` | 把"设备状态"和"已提交状态"分开；发成功才拉平；不忽略 `app_tx_write` 返回值 |
| `App/Src/app_fault.c` | 改 | 状态差驱动 + 三道闸门 + 按位提交 + `build_snapshot`（只读） | 同上；快照必须无副作用 |
| `App/Inc/app_cmd.h` | 改 | `ops` 加 `get_diag` | 复用现有回调表，不另建命令体系 |
| `App/Src/app_cmd.c` | 改 | `case PROTO_T_GET_DIAG`：`LEN≠0` → PARAM，不进回调 | 非法命令碰不到采样与通信 |
| `App/Src/app_task.c` | 改 | `ctx_get_diag()` 只置标志；主循环 2.5 步提交状态、2.6 步发快照；`fault_emit` 返回入队结果；`process_sample` 改调 `set_state`；`DIAG` 行加 `rep=` | 保证 ACK 在前、快照在入队时刻取状态；发送成败可判定 |
| `App/Inc/app_config.h` | 改 | `APP_FAULT_RETRY_MIN_MS 100` + 编译期下限检查 | 节流参数集中管理 |
| `host/protocol.py` | 改 | `T_GET_DIAG`、`FAULT_EV_SNAPSHOT`、`FAULT_ID_NONE`、`build_get_diag()`、`Fault.is_snapshot` | 解析与构造集中一处 |
| `host/main.py` | 改 | "查询故障状态"按钮；连接/重连成功自动查一次；快照走"状态同步"而不是事件 | 满足重连对齐与"快照不得显示成确认事件" |
| `docs/PROTOCOL.md` | 改 | §3.2 加 `0x16`；§4.8 补 `event=0`；新增 §4.10 | 协议是唯一依据，必须先改文档 |
| `fw_tests/test_fault.c` | 重写 | 适配新 API + 12 条要求全覆盖 + 3 组新契约向量 | 见 §3 |
| `fw_tests/test_cmd.c` | 改 | `ops` 加 `get_diag`；GET_DIAG 合法/非法/回调拒绝/连续请求 | 分发器层 |
| `host/tests/test_fault_joint.py` | 改 | 6 组契约向量跨语言对拍 + 快照解析 | 两端一致性 |
| `host/tests/test_protocol_doc_sync.py` | 改 | `0x16` 长度/类型码、§4.10 快照示例帧三方对拍 | 文档漂移防线 |
| `host/tests/gui_smoke.py` | 改 | 加"连接后自动查询""快照清除显示""查询应答"三项 | 界面联动 |

---

## 2. 协议文档更新

### 2.1 `0x16 GET_DIAG`

| 项 | 值 |
|---|---|
| TYPE / LEN | `0x16` / **0** |
| 非法 | `LEN≠0` → ACK `result=1`，不进回调 |
| 应答顺序 | **先 ACK，后快照**（两帧共用设备全局帧序号） |

### 2.2 `event=0` 快照（复用 `0x86`，帧结构不变）

| 情况 | `fault_id` | `active` | `value` |
|---|---|---|---|
| 无故障 | `0` | `0` | `0` |
| 恰好一个故障 | 该编号（`1`） | 掩码 | 观测量 |
| 多故障并存 | `0` | 完整掩码 | `0` |

五条硬规则：不补发历史事件 / 不是事件（上位机不得显示成确认或恢复）/ `active` 是权威值 /
只读副作用为零 / 在入队时刻取状态。

### 2.3 新增契约向量（三方共用：文档 §4.10、C 测试、Python 测试）

```
快照(有故障)：AA 55 86 09 05 40 1F 00 00 00 01 01 19 00 11 DE
快照(无故障)：AA 55 86 09 06 28 23 00 00 00 00 00 00 00 00 11
请求 GET_DIAG：AA 55 16 00 07 B8 4D
```

`test_protocol_doc_sync.py` 会从 Markdown 里正则抓这两串 hex，与 C 数组、Python 构造结果比对；
改一处而忘另一处，测试立刻红。

---

## 3. C 单元测试：**1041 项断言，0 失败**

`gcc -std=c99 -Wall -Wextra -Werror -O2`，编译的是仓库里的真实源码（`app_fault.c`、`app_cmd.c` 等）。

从 892 → 1041（**+149**）。逐条对应你列的 12 条要求：

| 你的要求 | 对应用例 | 结果 |
|---|---|---|
| 1. active 变化 + TX 成功 → reported 更新 | `1) active 变化且提交成功` | ✅ |
| 2. TX 队列满 → reported 不更新 | `2) 提交失败 -> reported 不更新` | ✅ |
| 3. 队列满期间不消耗 SEQ | `3) tx_has_room=0 时连 emit 都不调`（断言 emit 调用次数为 0） | ✅ |
| 4. 队列恢复后提交最新 active | `4) 队列恢复后提交的是当前状态` | ✅ |
| 5. 100 ms 节流边界 | `5) 差 1 ms 不试，正好 100 ms 放行` | ✅ |
| 6. 确认→恢复→再确认，允许边沿合并 | `6) 全程队列满 -> 只补发当前状态一帧`（3 个边沿→1 帧） | ✅ |
| 7. 快照不修改 reported | `7) build_snapshot 只读`（查 100 次后 active/reported/commit_cnt 全不变） | ✅ |
| 8. 无故障快照字段 | `8) fault_id=0 / active=0 / value=0` | ✅ |
| 9. 有故障快照字段 | `9) fault_id=1 / active=1 / value=span` | ✅ |
| 10. 诊断关闭但 active 保留 | `10) 不再登记新状态时快照仍反映保留的 active` | ✅ |
| 11. 多次 GET_DIAG 不丢标志不积压 | `cmd: 连续 3 次 GET_DIAG -> 3 个 ACK`（标志在 app_task，本层验 ACK 一一对应） | ✅ |
| 12. ACK 先于快照入队 | `cmd: GET_DIAG len=0 合法` 断言 app_cmd 只发 1 帧 ACK、**绝不发 FAULT**（快照由主循环发） | ✅ |

另外保留：3 组旧契约向量 + 多故障白盒掩码 + NULL 安全 + 未知 fault_id + `emit=NULL`。

**过程中修掉的 2 个测试自身缺陷**（不是固件问题）：
① 第 7 节忘了 `cap_reset()`，把上一节的发送计入了断言；
② 未知 fault_id 用例白盒设了 `active=1` 却没设 `reported`，于是"欠发送"本身产生了 1 帧，
断言与意图冲突——补 `reported=1` 后语义正确。

---

## 4. 上位机测试：**72 项全过**（+6）

| 新增用例 | 验什么 |
|---|---|
| `test_parse_snapshot_with_fault` | `event=0` 被识别为快照；字段值正确 |
| `test_parse_snapshot_no_fault` | `fault_id=0 / active=0 / value=0`；名称显示"无故障" |
| `test_build_get_diag_bytes` | 请求帧 7 字节、LEN=0、字节匹配契约向量 |
| `test_python_can_rebuild_snapshot_vectors` | 快照两串能由 Python 独立重构 |
| `test_get_diag_len_is_0_everywhere` | 固件/上位机/文档三处的 LEN |
| `test_doc_snapshot_vectors` | 文档 §4.10 示例 = C 侧数组 |

**界面冒烟**（offscreen，软件模拟）新增 3 项，全 PASS：
连接后自动查询、快照清除故障显示、查询应答 `GET_DIAG -> OK`。

---

## 5. 编译结果与资源变化

```
Program Size: Code=22468  RO-data=1076  RW-data=148  ZI-data=4972
"DataAcq\DataAcq.axf" - 0 Error(s), 0 Warning(s).
```

| 项 | 7C | 7C-2 | 变化 |
|---|---|---|---|
| Code | 22204 | **22468** | **+264** |
| RO-data | 1000 | 1076 | +76（新格式化字符串） |
| RW-data | 148 | 148 | 0 |
| ZI-data | 4956 | **4972** | **+16**（`app_fault_t` 扩容） |
| Flash | 35.4% | **35.9%** | +0.5% |
| RAM | 24.9% | **25.0%** | +0.1% |

---

## 6. 板级测试（真实板卡 + 真实电气）

原始日志：`docs/STAGE7C2_BOARD_RAW.txt`。固件 `Code=22468`，烧录后端口重枚举为 COM5。

| # | 动作 | 结果 |
|---|---|---|
| 1 | 复位后立刻 `GET_DIAG` | **ACK seq=46 → 快照 seq=47**，`event=快照 id=无故障 mask=0x00 value=0` |
| 2 | `SET_DIAG(1)` | ACK seq=199；**3.02 s** 后确认帧 seq=250，`value=8` |
| 3 | 有故障时 `GET_DIAG` | ACK seq=39 → 快照 seq=40，`id=STUCK mask=0x01 value=12` |
| 4 | 关闭串口（模拟断线），设备继续跑 | — |
| 5 | 断线期间拧电位器 | 重连后 `conf` 由 1 变 2 → **期间确实发生过"恢复→再确认"** |
| 6 | 重连后查 | 快照如实返回 `mask=0x01`（那一刻设备确实是活动态） |
| 7 | 再次断线，连续缓慢拧动，**每 350 ms 轮询** | **57 帧快照全为 `mask=0x00`**；停手后设备又自行重判卡死（`conf=4`） |
| 8 | 全程计数 | `drop=0 adc=0 uart=0 frm=0`，解析错误 0 |
| 9 | 首段 915 帧 SEQ 连续性 | **异常 0 个**（ACK 与快照在同一计数器里紧邻） |

**结论**：ACK→快照的先后顺序、快照字段（有无故障两种）、断线后重新对齐、`active=0` 快照
都在真实硬件上成立。第 7 项尤其关键：它证明"事件丢了以后，快照能给出当前真相"。

> 说明：第 5/6 项第一次尝试时窗口已过，快照返回 `mask=1`；我没有把它改写成"成功"，
> 而是改成第 7 项的**连续轮询**才拿到 `active=0`。两次结果都记在原始日志里。

---

## 7. 未验证事项（明确标注）

| # | 项 | 为什么没验证 |
|---|---|---|
| 1 | **TX 队列满时的重试路径（硬件）** | 需要让发送环真满（灌 100 条 stat 那种），本轮没做。该路径只有 **C 单测**（模拟 `emit` 返回 0）覆盖 |
| 2 | **100 ms 节流在硬件上的行为** | 同上，节流只在队列持续满时才有意义 |
| 3 | 队列检查与写入之间是否有并发生产者 | 已确认没有：ACK（`app_cmd_poll`）与 FAULT（`process_sample`）**都在主循环**，无 ISR 参与入队；`s_busy` 与 DMA 回调的竞争是既有设计，本轮未改 |
| 4 | 多故障并存时的快照（`fault_id=0, active=多bit`） | 协议层只登记 STUCK；该分支只有**白盒单测**，没有真实第二种故障 |
| 5 | 真实传感器失效 | 仍只有电位器 |
| 6 | 诊断长时间开启（1h/8h） | 本轮板级累计约 6 分钟 |
| 7 | GUI 点按钮连真板 | 界面路径由**软件模拟**验证（gui_smoke）；真板路径走的是同一套 `protocol.py` 的脚本 |

---

## 8. Git

| 项 | 值 |
|---|---|
| 提交 | 见本轮 commit（`feat(diag): Stage 7C-2 ...`） |
| 父提交 | `6b3632a`（协议约束审计） |
| 分支 | `main` |
