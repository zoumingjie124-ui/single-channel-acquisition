# 阶段四：固件纯逻辑模块的 C 单元测试

> 日期：**2026-11-03 09:27 – 09:30**
> 工具链：**gcc.exe (tdm64-1) 4.9.2**（Dev-Cpp 自带 MinGW64）
> 被测源码：`App/Src/app_proto.c`、`App/Src/app_alarm.c`、`App/Src/app_filter.c`（**仓库里的真实文件，未复制、未重写**）
> 原始输出：`docs/FW_TESTS_RAW.txt`
> 结果：**断言 167 项，失败 0 项 → PASS**

---

## 0. 为什么必须做这一层

在此之前，"28/28 通过"指的是 `host/tests/` 里的 **Python** 测试——它验的是上位机的解析器、流水线和界面，**一行 STM32 固件的 C 代码都没执行过**。

而设备上真正跑的是 `app_proto.c` / `app_alarm.c` / `app_filter.c` 这三份 C 代码。它们的正确性以前只能靠"上板跑起来看起来对"来推断。

这三个模块的共同点：**不依赖 HAL，不碰寄存器，纯函数 + 结构体**，所以可以原样在 PC 上编译。

---

## 1. 工具链与编译命令

| 项 | 值 |
|---|---|
| 编译器 | `C:\Program Files (x86)\Dev-Cpp\MinGW64\bin\gcc.exe` |
| 版本 | `gcc.exe (tdm64-1) 4.9.2` |
| 标准 | `-std=c99` |
| 告警 | `-Wall -Wextra -Werror`（**有问题就不许过**） |
| 优化 | `-O2` |
| 头文件路径 | `-I../App/Inc -I.` |

```bat
gcc -std=c99 -Wall -Wextra -Werror -O2 -I..\App\Inc -I. ^
    main.c test_proto.c test_alarm.c test_filter.c ^
    ..\App\Src\app_proto.c ..\App\Src\app_alarm.c ..\App\Src\app_filter.c ^
    -o fw_tests.exe
```

**编译零告警**（`-Werror` 下通过，说明被测源码本身也没有告警）。

---

## 2. 文件结构

```
fw_tests/
├── test_util.h        断言框架（约 40 行，无第三方依赖）
├── main.c             入口 + 全局计数
├── test_proto.c       app_proto.c 的 8 组用例
├── test_alarm.c       app_alarm.c 的 8 组用例
├── test_filter.c      app_filter.c 的 8 组用例
└── run_tests.bat      自动找 gcc → 编译 → 运行 → 返回退出码
```

`run_tests.bat` 会依次尝试 `%ProgramFiles(x86)%\Dev-Cpp`、`%ProgramFiles%\Dev-Cpp`、`C:\msys64`、`PATH`，找不到就报错退出（退出码 1）。

---

## 3. 用例设计与覆盖点

### 3.1 `app_proto.c`（协议：CRC / 组帧 / 状态机）

| 用例组 | 覆盖点 |
|---|---|
| CRC 已知向量 | `"123456789"` → **0x29B1**（CRC-16/CCITT-FALSE 标准校验值）、空数据 → 0xFFFF、单字节 → 0xC782 |
| 组帧边界 | 正常帧长度 17；载荷 40（上限）允许；41 拒绝；容量差 1 字节拒绝；空载荷 + NULL 允许；有载荷 + NULL 拒绝；输出指针 NULL 拒绝 |
| 往返 | 编码后解析，逐字段比对 type / seq / len / 载荷内容 |
| CRC 错误 | 破坏 CRC 高字节 → `PROTO_R_ERROR`、**回调 0 次**、状态复位；紧接好帧能恢复 |
| LEN 越界 | 41 / 255 → 立即报错并回 SOF0；40 → 不报错，继续等载荷 |
| 帧头异常 | `AA 00` 报错复位；`AA AA 55 ...` 能解出（v5.1.1 的容错）；可打印垃圾前缀被安静跳过 |
| 连续多帧 | 5 帧背靠背，回调 5 次，最后一帧 seq 正确 |
| 半截帧 | **记录现状**：卡在 BODY → 吃掉后 2 帧 → 第 3 帧恢复。这条明确标注"不是期望行为，超时复位在 `app_cmd` 层" |

### 3.2 `app_alarm.c`（告警状态机）

| 用例组 | 覆盖点 |
|---|---|
| 初始化 | 级别 NORMAL、change_count 0、阈值正确 |
| 正常区 | 0 / 2399 各 10 次，不跳变、不计变更 |
| 去抖 | 需**连续 3 次**才从 NORMAL → WARN；第 1、2 次仍 NORMAL |
| 抖动清零 | 2400×2 → 1000×1 → 2400×2 仍不切换（up_cnt 被打断） |
| 相邻跳转 | 3300 连喂 3 次只到 WARN，再 3 次才到 CRIT（**不会从 NORMAL 直接跳 CRIT**） |
| WARN 滞回退出 | 2350（恰好等于门槛）保持 WARN；2399 保持；**2349 连续 3 次才回 NORMAL** |
| CRIT 滞回退出 | 2950 保持 CRIT；2949 连续 3 次 → **只退到 WARN**，不会继续退 |
| 阈值校验 | warn=0 / warn=50 / crit<warn+hyst / crit 超量程 全部拒绝；warn=51+crit=101 恰好通过 |
| **回绕回归** | `warn=65500, crit=100` 必须拒绝（旧实现用 uint16 加法会回绕成 14 而误通过） |
| 阈值生效 | 合法调用后立即生效，且**清空去抖计数**（否则旧计数会污染新阈值） |
| NULL 保护 | init / update / set_thresholds 传 NULL 不崩溃，返回值合理 |
| 名称映射 | NORMAL / WARN / CRIT / 未知值 |

### 3.3 `app_filter.c`（移动平均）

| 用例组 | 覆盖点 |
|---|---|
| 初始化 | count / sum / index / value 全为 0 |
| 启动阶段 | 1 点 = 它自己；2 点 = 两者均值；3 点 = 三者均值（**按 count 而不是 DEPTH**） |
| 满窗 | 100..800 八点 → 450，sum = 3600 |
| 覆盖滑窗 | 推入 900 → 窗口 200..900 → 550；再推 1000 → 650（验证"先减旧值再加新值"） |
| 极值 | 8×65535 → 65535，sum = 524280（uint32 不溢出）；高低交替 → 32767（截断正确） |
| 阶跃 | 0 → 3300 单点阶跃，第一拍输出 1650（限幅关闭，原值直接进窗口） |
| 规格比对 | **40 点定种子伪随机序列**，逐点与"最近 N 个的算术平均"比对 |
| NULL 保护 | init / push / value / count 传 NULL 不崩溃 |
| 计数饱和 | 24 次推入后 count 仍为 DEPTH |

---

## 4. 结果

```
断言 167 项，失败 0 项
RESULT: PASS
```

各模块断言分布（估）：proto ≈ 70、alarm ≈ 55、filter ≈ 42。

---

## 5. 过程中修正的 4 个"测试自身"错误（必须记下来）

第一版跑出 4 个 FAIL，**全部是测试写错，不是固件问题**：

| # | 现象 | 根因 | 处置 |
|---|---|---|---|
| 1 | 单字节 CRC 期望 0xF4C3，实得 51074（0xC782） | 期望值是凭印象填的 | 手算验证：`0xFFFF^0x3100=0xCEFF`，8 次移位后确为 **0xC782**；实现正确，改测试 |
| 2–3 | LEN 越界用例 `r = 0，期望 2` | `feed()` 返回的是**最后一字节**的结果，我多喂了一字节，把 ERROR 覆盖成 NONE | 只喂到 LEN 字节为止 |
| 4 | `AA 00` 用例同样 `r = 0` | 同上，多喂了第 3 字节 | 只喂 2 字节 |

> 这件事本身说明测试是"真在跑"的：如果断言随便写都能过，就等于没测。
> 但也提醒一点——**测试失败时先怀疑测试**，确认固件真的错了再改固件。

---

## 6. 覆盖范围与未覆盖

### 6.1 已覆盖（本轮）

- 协议：CRC 算法、组帧边界、解析状态机全部 8 个状态、非法长度、错误恢复、重同步、半截帧现状
- 告警：三级判级、相邻跳转、滞回边界（含恰好等于门槛）、去抖计数、阈值校验（含回绕回归）、NULL
- 滤波：启动部分窗、满窗、覆盖滑窗、极值、阶跃、40 点规格比对、NULL、计数饱和

### 6.2 未覆盖（明确 NOT TESTED）

| 模块 / 项 | 原因 |
|---|---|
| `app_measure.c` | 直接依赖 `HAL_ADC_Start_DMA` / `adc.h`，PC 上无法链接 |
| `app_tx.c` | 依赖 `UART_HandleTypeDef` 与 HAL 发送函数 |
| `app_cmd.c` | 依赖 `app_tx.h`（间接 HAL）；其超时逻辑已在阶段三用真实硬件验证 |
| `app_task.c` | 业务总调度，强依赖 HAL 与全局上下文 |
| `app_proto.c` 的 `PROTO_MAX_FRAME` 与上层缓冲一致性 | 属静态检查，已在 `test_firmware_sync.py` 里覆盖 |
| 编译器差异 | 只在 GCC 上跑了；Keil ARMCC5 的编译结果不涉及本层 |

> 要不要把 `app_measure.c` 也搬到 PC 上测？需要给 HAL 打桩（stub），
> 属于"为了测试改架构"，按你的规矩不擅自做。

---

## 7. 与 Python 测试的分工（避免混淆）

| 层 | 位置 | 测什么 | 数量 |
|---|---|---|---|
| 上位机 Python | `host/tests/` | 协议解析、流水线、固件常量一致性、界面冒烟 | 28 项 |
| **固件 C** | **`fw_tests/`** | **设备端真实执行的 C 逻辑** | **167 断言** |

两者互不替代：Python 那 28 项里哪怕全绿，也证明不了 `app_alarm.c` 的滞回减法不会下溢。

---

## 8. 如何复现

```bat
cd fw_tests
run_tests.bat
```

脚本会打印编译器版本、执行编译、运行测试，并返回退出码（0 = 全过）。

---

## 9. 结论

| 项 | 判定 |
|---|---|
| 编译（`-Wall -Wextra -Werror`） | **PASS**（零告警） |
| `app_proto.c` 用例 | **PASS** |
| `app_alarm.c` 用例 | **PASS** |
| `app_filter.c` 用例 | **PASS** |
| 总计 167 断言 | **PASS（0 失败）** |
| 固件代码改动 | **无**（本轮只新增 `fw_tests/`，`git diff HEAD -- App Core` 为空） |

板上的固件完全没动，仍是阶段三加固后的版本（Release `Code=21208`）。
