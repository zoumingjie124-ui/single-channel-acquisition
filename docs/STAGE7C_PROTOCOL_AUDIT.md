# Stage 7C 协议约束审计（对照 PROTOCOL.md V5）

> 日期：2026-11-03 20:0x
> 依据：**本仓库 `docs/PROTOCOL.md`（V5）为唯一协议来源**。
> 本轮**没有改协议**，只做核对、补测试、补证据；发现的两处缺口见 §10，**未擅自修改**。

---

## 0. 先说清一件事

`PROTOCOL.md` 里 `0x86 FAULT` / `0x15 SET_DIAG` 的章节是**本次 7C 按规格新增的**，
不是之前就有的。所以"以 V5 示例帧为契约向量"这句话，实际含义是：

> §4.8 里那两串 hex（确认 / 恢复）就是向量，且必须与 `fw_tests/test_fault.c`、
> `host/tests/test_fault_joint.py`、`host/tests/test_protocol_doc_sync.py` 里的字节完全一致。

这条现在是**自动校验**的：`test_protocol_doc_sync.py` 直接从 Markdown 里正则抓 hex，
再和 C 侧数组、Python 构造结果三方比对。文档一改就红。

---

## 1. FAULT 严格 9 字节，偏移/大小端/CRC 与 V5 一致

| 检查 | 证据 | 判定 |
|---|---|---|
| 长度 9 | 固件 `PROTO_LEN_FAULT=9`、`parse_fault` 拒绝其他长度、文档 §3.1 与 §4.8 | ✅ |
| 偏移 0/4/5/6/7 | `app_fault_build_payload()` 用 `proto_put_u32(&p[0])`、`p[4]`、`p[5]`、`p[6]`、`proto_put_u16(&p[7])`；`TestFaultLayout` 解析 C 源码断言偏移序列 | ✅ |
| 字段宽度 u32/u8/u8/u8/u16 | 文档 §4.8 表 + 源码正则断言，合计 4+1+1+1+2=9 | ✅ |
| 小端 | `proto_put_u32/u16` 均按低字节在前；文档声明"所有多字节字段为小端"；测试用 3000=0x0BB8 → `B8 0B` 验证 | ✅ |
| CRC 覆盖范围 | `proto_encode()` 对 `TYPE+LEN+SEQ+PAYLOAD` 算 CRC，与文档一致；测试反向验证"把帧头算进去就得到不同值" | ✅ |

契约向量（三处一致，且能被 Python 独立重构出来）：

```
确认：AA 55 86 09 00 B8 0B 00 00 01 01 01 0C 00 F9 45
恢复：AA 55 86 09 01 70 17 00 00 02 01 00 78 00 54 D2
```

## 2. SET_DIAG 严格 1 字节，只接受 0/1，非法 ACK result=1

| 检查 | 证据 | 判定 |
|---|---|---|
| 长度 1 | `PROTO_LEN_SET_DIAG=1`；文档 §3.2/§4.9 | ✅ |
| 只接受 0/1 | `app_cmd.c` 先判 `len != 1` 再判 `payload[0] > 1`，两者都回 `CMD_RES_PARAM` | ✅ |
| 非法进不到回调 | 板级实测：`enable=2` → ACK 参数错误，`DIAG en` 仍为 1；`LEN=2` → 参数错误 | ✅ |
| ACK 在配置生效后 | `ctx_set_diag()` 先切换，返回 1，`app_cmd` 随后统一 `send_ack()` | ✅ |

## 3. FAULT / ACK / SAMPLE / STAT / EVENT / BOOT 共用全局帧 SEQ

**板级实测**（复位后重烧同一固件，9 秒连续抓帧，`docs/STAGE7C_SEQ_RAW.txt`）：

```
帧总数 913：SAMPLE 900, STAT 10, ACK 2, FAULT 1
按到达顺序检查 912 个间隔：异常 0 个
各类型首次出现位置（同一计数器里交错）：
  SAMPLE 第0帧 seq=64   STAT 第76帧 seq=140
  ACK    第102帧 seq=166  FAULT 第408帧 seq=216
```

- `FAULT` 走 `emit_frame()` → `app_task_next_frame_seq()`，与 SAMPLE/EVENT/STAT 同一函数；
- `ACK` 走 `send_ack()` → 同一函数；
- 抓帧覆盖了 **255→0 回绕**（第 408 帧 seq=216 到第 610 帧 seq=162，中间 202 帧，
  216+202 mod 256 = 162），回绕处仍然连续——说明比较用的是 `(seq+1)&0xFF` 而不是简单加法。

## 4. 多故障位掩码按 V5 处理；只实现 STUCK，不虚构其他故障

| 检查 | 证据 | 判定 |
|---|---|---|
| 掩码而非单值 | 载荷第 6 字节 `active`，上报后仍处于已确认态的位集合 | ✅ |
| 恢复只清自己那一位 | `app_fault.c` 用 `f->active &= ~bit`；C 用例预置 `0x02` 后恢复 STUCK，断言掩码仍为 `0x02` | ✅ |
| 生产代码只有 STUCK | `grep` 全仓：`PROTO_FAULT_ID_STUCK` 一处定义、`APP_FAULT_ID_STUCK` 一处别名、`FAULT_ID_STUCK`（Python）一处 | ✅ |
| 未登记编号被忽略 | `app_fault_mask_of()` 返回 0 → 不产生任何帧；C 用例覆盖 | ✅ |

> **注意**：`test_fault.c` 里的 `0x02` 是把"另一个故障已在册"这个**状态**直接写进结构体，
> 用来验证"恢复只清自己那一位"。它**没有**在协议层登记第二个故障编号，
> 也没有用未知 id 去调 API。不要把这条用例误读成"实现了两种故障"。

## 5. 关闭诊断 ≠ 故障恢复，不伪造恢复事件

| 检查 | 证据 | 判定 |
|---|---|---|
| 代码路径 | `ctx_set_diag()` 只调 `app_diag_set_enable()`；不动 `app_fault` | ✅ |
| `app_diag_set_enable(0)` 不改状态机 | 只清 `enable`，`state`/`stuck_blocks` 保留 | ✅ |
| 关诊断时仍在喂上报层 | `app_fault_update()` 每样本照调，`is_active` 仍为 1 → 无跃变 → 不发帧 | ✅ |
| 板级实测 | `SET_DIAG(0)` → ACK OK，**期间 0 帧 FAULT**，`DIAG state=2 mask=1` 保留 | ✅ |
| 再使能不重复上报 | 再 `SET_DIAG(1)` 后 3.5 s 内 **0 帧** | ✅ |
| `reset` 命令也不清故障表 | `ctx_reset_stats()` 未调用 `app_fault_init()`；`app_fault_init` 全仓只在开机调用一次 | ✅ |

## 6. TX DMA 环形队列：FAULT 与 ACK 并发时的序号/顺序/队列满

**先纠正一个前提**：本项目里 FAULT 与 ACK **不存在真正的并发**。

| 路径 | 调用链 | 上下文 |
|---|---|---|
| ACK | `app_cmd_poll()`（主循环第 1 步）→ `on_frame()` → `send_ack()` → `app_tx_write()` | 主循环 |
| FAULT | `process_sample()`（主循环第 2 步）→ `app_fault_update()` → `fault_emit()` → `emit_frame()` → `app_tx_write()` | 主循环 |

两者都在主循环里顺序执行，没有 ISR 参与写队列。唯一与 ISR 共享的是 `s_busy`
（主循环写 1、`TxCplt` 写 0），以及 `head/tail`（都只在主循环改）——这是既有设计，7C 没有改变。

| 子项 | 结论 | 依据 |
|---|---|---|
| 序号分配 | 每帧调一次 `app_task_next_frame_seq()`，**一次调用一次自增**，与帧类型无关 | 抓帧 912 间隔 0 异常 |
| 入队顺序 = 序号顺序 | `emit_frame()` 里先取 SEQ、紧接 `proto_encode`、再 `app_tx_write`，中间没有别的入队 | 同上 |
| 一轮内的相对顺序 | 命令先于采样：同一轮里 ACK 一定排在 FAULT 之前；`process_sample()` 内部是 **FAULT → EVENT → SAMPLE** | 代码顺序 |
| 队列满 | `app_tx_write()` **整帧丢弃**并计 `s_drop_bytes`，绝不写半帧；已有 Stage 3 实测（灌满 100 条 stat → 丢 11333 B、解析错误 0、无半帧） | Stage 3 报告 |
| 队列满时 FAULT 的后果 | **见 §10 缺口 1** | — |

## 7. V5 示例帧作为两端契约向量

| 检查 | 证据 |
|---|---|
| 文档示例 = C 侧数组 | `test_protocol_doc_sync.test_doc_examples_equal_contract_vectors` 从 Markdown 正则抓 hex，与 `test_fault.c` 的数组逐字节比对 |
| 文档示例 = Python 构造 | 同一用例用 `P.encode()` 独立重构，断言等于文档里的串 |
| 三组向量 | 确认 / 恢复 / SET_DIAG(on, seq=2)，缺一即失败 |

## 8. 改协议代码后三处同步

新增 `host/tests/test_protocol_doc_sync.py`（14 项），把"文档—固件—上位机"锁在一起：

| 用例 | 守的是什么 |
|---|---|
| `test_fault_len_is_9_everywhere` | 三处的 FAULT 长度 |
| `test_fault_field_offsets_in_doc` / `_widths_in_doc` | 文档 §4.8 表与源码偏移 |
| `test_crc_covers_type_to_payload` | CRC 覆盖范围 |
| `test_set_diag_accepts_only_0_and_1` | §4.9 的取值约束与 result=1 |
| `test_only_stuck_is_registered` | 三处都只有一个故障编号 |
| `test_doc_examples_equal_contract_vectors` | 示例帧 = 契约向量 |
| `test_type_codes_three_way` / `test_event_codes_three_way` | 类型码与 event 取值 |
| `test_old_frames_not_renumbered` | 老帧类型码与长度没被顺手改掉 |

现状：**上位机 66 项测试全过**（原 52 + 本次 14），其中 14 项是跨文档的。

---

## 9. 本轮实际执行的动作

| 动作 | 结果 |
|---|---|
| 新增 `test_protocol_doc_sync.py` | 14 项，全过；首次运行抓到自己写错一处断言（把 `FAULT_ID_NAMES` 当成了故障编号），已修正 |
| 复位设备（重烧同一固件，不改代码） | 清掉上一轮遗留的 `state=2 mask=1` |
| 板级抓 9 秒 | 913 帧，912 间隔 0 异常，含 1 帧 FAULT 与 2 帧 ACK |
| 全量回归 | 固件 C 892 项（未改固件，仍全过）；上位机 66 项 |

原始记录：`docs/STAGE7C_SEQ_RAW.txt`。

---

## 10. 发现的两处缺口（**未修改，等你决定**）

### 缺口 1：被丢弃的 FAULT 不会重发（与"无查询命令"是同一个洞）

**链路**：
1. `app_fault_update()` 先把 `f->active |= bit` 置位，**然后**才调 `emit()`；
2. `emit_frame()` 里 `app_tx_write()` 的返回值被 `(void)` 丢弃；
3. 若此刻发送环满（1024 B 满，约需 DMA 停摆 640 ms 以上），整帧被丢、只计 `s_drop_bytes`；
4. 但掩码已经置位 → 后续每样本调进来都"已经报过了" → **永久不再重发**。

**后果**：上位机永远不知道这次故障；设备自己认为"已上报"。只有人工看 `stat` 的
`DIAG ... mask=` 才能发现两端不一致。

**触发条件**：只在发送环真正满时发生（Stage 3 用 100 条 stat 灌满过，正常 100 Hz 负载下不会）。
但"链路拔掉几分钟再插回"这类场景会命中——期间故障发生、恢复、再确认，全部丢失。

**同一个洞的另一半**：没有"查询当前故障"的命令。上位机重连后无法对齐设备既有状态
（`stat` 文本行能看到，二进制侧没有）。

**可选方案（都不动现有帧长度）**：

| 方案 | 做法 | 代价 |
|---|---|---|
| A（推荐） | 新增 `0x16 GET_DIAG`（LEN=0），设备回一帧 `0x86` 状态帧（`event=0` 表示"当前快照"） | 新增命令 + 复用 FAULT 帧格式；约 +60 B 代码 |
| B | 新增 `0x87 DIAG_STAT`，随 STAT 周期 1 Hz 发掩码 | 每秒多 9 B；新增一种帧 |
| C | 只改文档，写明"链路中断期间的故障事件会丢，重连后请对照 `stat` 文本" | 零代码，但把洞留在文档里 |

**我的意见**：A 最干净——复用已有的 FAULT 载荷，只增加一条查询命令，
既解决重连对齐，也给"事件被丢"提供了兜底（重连后主动查一次）。

### 缺口 2：文档没写"一轮内的帧序"

`process_sample()` 里的顺序是 **FAULT → EVENT → SAMPLE**，且 ACK 在它们之前（命令先处理）。
上位机目前不依赖这个顺序，所以不是缺陷；但文档里没写，将来有人做严格顺序断言会踩坑。

**建议**：在 `PROTOCOL.md` §2 或 §7 加一句"同一轮主循环内，ACK 先于采样类帧；
采样类帧内部顺序为 FAULT → EVENT → SAMPLE"。**要改文档需要你点头**（协议文档我不擅自动）。

---

## 11. 结论

协议实现与 V5 **没有冲突**：8 条约束逐条对上，其中第 3、5、6 条有板级证据，
第 1、2、4、7、8 条有代码 + 自动化测试证据。两处缺口都是**能力缺失**（无法重发/无法查询），
不是**协议不一致**，所以没有触发"先报告冲突"的红线。
