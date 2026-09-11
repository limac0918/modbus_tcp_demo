# Modbus TCP 主站 Demo —— 心跳 / 断线检测 / 自动重连 / 请求队列 + 报警日志 SQLite

一个生产级思路的 Qt Modbus TCP 主站封装 + HMI 报警日志 SQLite 可靠存储，含 UI 测试入口。

---

## 一、架构：通信层 / 存储层 与 UI 完全解耦

```
┌──────────────────────────────────────────────────┐
│ UI 层 (MainWindow，纯测试，可整体删除)            │
│   Modbus 区 + 报警区  ←—— 只通过信号接数据        │
└─────────┬───────────────────────────┬────────────┘
          │ 信号: stateChanged/...     │ 信号: logMessage / alarmsChanged
          │ 槽:   start()/enqueueRead  │ 槽:   writeOne/writeBatch/ack...
┌─────────▼───────────────────┐  ┌─────▼──────────────────────┐
│ 通信层 (ModbusManager)      │  │ 存储层 (AlarmLogger)       │
│  ├─ QModbusTcpClient 状态机 │  │  ├─ SQLite (Qt QSql 模块)  │
│  ├─ 心跳/断线/退避重连      │  │  ├─ WAL + synchronous=FULL │
│  └─ 请求队列(并发/超时/滚动)│  │  ├─ 事务批量写入           │
└────────────────────────────┘  │  └─ 日志滚动(超限删最旧)    │
                                └────────────────────────────┘
```

**关键约束**：`ModbusManager` / `AlarmLogger` 都不 include 任何 UI 头文件、不持有控件指针。
- 业务方：`enqueueRead(...)` 发请求、`writeBatch(...)` 写报警；
- UI/监控：connect 上述信号即可。

`MainWindow` 只是"测试夹具"——删除它，两个核心层可直接放进控制台 / 服务端工程。

---

## 二、构建

```bash
# 依赖：Qt5 (QtSerialBus + QtSql 模块), CMake >= 3.16, 编译器支持 C++17
# 最低 Qt 5.11（用到 QModbusDevice::errorOccurred 信号），建议 5.12 LTS 及以上
# Ubuntu:
#   sudo apt install qtbase5-dev libqt5serialbus5-dev libqt5sql5-sqlite
#   # 若需串口/网络相关额外模块，一并安装:
#   sudo apt install libqt5serialport5-dev
# （本工程只用 TCP，无需串口模块；RTU 场景才需要 QtSerialPort）
# 注意：QSQLITE 驱动是插件，Ubuntu 包名 libqt5sql5-sqlite；
#       aqtinstall 官方构建已内置该插件，无需额外安装。

cd modbus_tcp_demo
cmake -B build
cmake --build build -j
./build/modbus_tcp_demo   # 报警库文件 alarms.db 生成在程序工作目录
```

## 三、测试环境准备

需要一个 Modbus **从站** 监听。任选其一：

| 工具 | 说明 |
|---|---|
| **Modbus Slave**（Windows 商业软件） | 最常用，File → New → 设置 HR 数量，监听 TCP 502 |
| **ModRSsim2**（开源） | 轻量，支持 TCP Server |
| **pymodbus**（本机 Python） | `python -m pymodbus.server --port 502`（3.x） |
| **dianying/虚拟串口** | 网络场景不用 |

> 本机没有从站时，点「连接」会反复失败 → 正好观察**退避重连**。

## 四、功能对照

| 需求 | 实现位置 | 说明 |
|---|---|---|
| 心跳机制 | `enqueueHeartbeat()` / `handleHeartbeatResult()` | 每 2s 读 HR `0x0000`×1；连续 3 次失败 → `heartbeatLost` + 强制重连 |
| 断线检测 | `onClientStateChanged` / `onClientErrorOccurred` / 心跳 | 三重判定：状态机 + errorOccurred + 心跳丢失 |
| 自动重连 | `scheduleReconnect()` / `onReconnectTimeout()` | 指数退避 `base<<attempts`，封顶 30s，连接成功即重置 |
| 并发限制 | `m_maxConcurrent` + `pumpQueue()` | 默认并发 1（最稳），可调 |
| 超时处理 | `m_sweepTimer` (200ms) + `deadlineMs` | 在途超时 → 取消 reply + 回调失败 |
| 丢弃过期 | sweep 清理队首过期 + 发送前检查 | 排队太久的请求直接丢弃 |
| 防内存暴涨 | `m_queueLimit` (200) + 断线 flushQueue + UI 日志 2000 行上限 | 压测塞 500 请求，超限即拒 |
| 模拟故障 | `simulateNetworkDrop` / `simulateBadPort` / `restoreGoodPort` | 闪断 / 改错端口 / 恢复 |
| 通信类解耦 | 全部逻辑在 `ModbusManager`，信号对外 | 不依赖 UI 控件 |

---

## 五、验收方法（对应你的要求）

### 1. 反复开关从站，程序不崩溃、自动恢复、内存不涨
```
1. 启动从站监听 502
2. 程序点「连接」→ 日志出现 CONNECTED，心跳 ok
3. 关掉从站 → 观察：请求全部 dropped(connection lost)，
   心跳 fail 1/3 2/3 3/3 → LOST → force reconnect → 退避 1s/2s/4s...
4. 重新打开从站 → 退避内自动恢复 CONNECTED，心跳恢复 ok
5. 反复操作 3/4 若干次 → 底部 rss 数值不持续增长
```

### 2. 压测队列限流
```
点「压测:塞500请求」→ 日志显示 accepted≈200（队列上限），其余被拒绝；
底部 busy 数量回落，ok/fail 合计回到 0；rss 不涨。
```

### 3. 网络抖动 / PLC 离线模拟
```
「模拟:闪断」→ 断开，退避重连；
「模拟:改错端口」→ 重连一直失败（PLC 离线态），退避递增到 30s 封顶；
「模拟:恢复」→ 1s 内重连成功。
```

---

## 六、周日复盘（第2周）：异常场景梳理

### 异常场景清单

| 场景 | 现象 | 本 Demo 的处理 | 边界 |
|---|---|---|---|
| 网络闪断（物理断开） | 底层 stateChanged → Unconnected | flush 队列 + 退避重连 | 若 TCP 半开，底层可能延迟报错 → 靠心跳兜底 |
| PLC 宕机 | 建连成功但无响应 | 心跳 3 连失败 → 强制断开 → 重连 | 响应超时(1.5s)要小于心跳周期×失败次数 |
| 请求超时 | reply 一直不 finished | sweep 200ms 扫到 deadline 过期 → 取消 + 回调失败 | 超时后 reply 迟到 → `seq==0` 兜底忽略，不误配 |
| 应答报文错误 | reply.error() != NoError | 判为失败，回调 ok=false，不崩溃 | QModbus 抛 ProtocolError 时 reply 仍有效，需判 error |
| 从站断连后遗留请求 | pending+inflight 堆积 | 断线瞬间 flushQueue 全部收尾 | 否则内存/回调泄漏 |
| 高频请求压垮从站 | 请求风暴 | 并发=1 + 队列上限 + 过期丢弃 | 队列预算 500ms，防止刚入队即丢 |

### 记录的关键坑

> **坑 1：QModbus 底层不做"请求收尾"**
> `QModbusReply` 超时/断线后**不会自动 delete**，也不会通知"你的请求失败了"。
> 必须上层自己管：断线时 flush、超时时 sweep 取消，否则 QModbusReply 悬空 → 回调永远不触发 → 内存只增不减。

> **坑 2：超时 reply 的"迟到响应"会污染新请求**
> 场景：请求 A 超时被取消 → 新请求 B 用了相同事务 ID → A 的迟到响应到达。
> 解决：每请求有唯一 `seq`，`m_replySeq` 只认当前在途的 reply；已被取消的 reply 到达时 `seq==0` 直接丢弃。

> **坑 3：心跳不能无限堆积**
> 若每次心跳都"等上一次结果"，断线时会把心跳也堆进队列。
> 解决：`m_heartbeatInFlight` 标志保证同一时刻至多 1 个心跳在途，且心跳插队（prepend）。

> **坑 4：退避算法要防溢出 + 防双调度**
> `base << attempts` 当 attempts 过大时溢出；重连定时器要先 stop 再 start（幂等），否则一次断线会触发多次重连定时器。

> **坑 5：底层 `setNumberOfRetries` 要和上层重试分开**
> 底层重试会打乱我们的超时语义。设 `setNumberOfRetries(0)`，重试逻辑完全交给上层退避。

> **坑 6：UI 日志也会堆内存**
> `QPlainTextEdit::appendPlainText` 无限追加会涨内存。用 `setMaximumBlockCount(2000)` 限流。

### 需要验证的行为保证
- [ ] 反复开关从站 20+ 次，无崩溃、无泄漏（配合 AddressSanitizer 更佳）
- [ ] 断线后 RSS 平稳（当前进程 VmRSS 由 UI 底部实时显示）
- [ ] 请求回调保证"要么 ok=true，要么 ok=false 必有一次"——不存在永不回调
- [ ] 恢复连接后积压请求会补发（Connected 后 pumpQueue）

---

## 七、第3周：HMI 报警日志 SQLite 可靠存储

### 6.1 功能对照

| 需求 | 实现位置 | 说明 |
|---|---|---|
| SQLite 基础 | `AlarmLogger::init` / `ensureSchema` | `QSqlDatabase(QSQLITE)` + `QSqlQuery`，连接名 `alarm_conn` |
| 报警表设计 | `CREATE TABLE alarms` | `id(自增) / ts(时间) / message(内容) / level(等级) / acked(确认状态)` |
| WAL 模式 | `PRAGMA journal_mode=WAL` | 断电安全 + 读写不互斥（HMI 界面不卡） |
| 断电安全 | WAL + `PRAGMA synchronous=FULL` | 每次 commit 落盘；kill -9 后已提交数据不丢 |
| 事务批量写入 | `writeBatch()` | N 条一个 commit（原子性 + 降 Flash 磨损） |
| 日志滚动 | `rollIfNeeded()` | 超过 `maxRows` 自动删最旧，库不无限膨胀 |
| 确认状态 | `ackAlarm` / `ackAll` | 操作员确认报警（HMI 常见需求） |
| 存储层解耦 | 全部逻辑在 `AlarmLogger`，信号对外 | 不依赖 UI 控件 |

### 6.2 本周 Demo 用法

```
启动程序 → 报警区出现（DB 自动创建 alarms.db）：
「模拟:报1条警」→ 写 1 条（内部事务）
「模拟:批量50条(单事务)」→ 50 条一个 commit（看日志 batch committed: 50/50）
「确认选中 / 全部确认」→ 更新 acked 状态，列表刷新
「滚动演示:塞6000条」→ maxRows=5000，写后自动删最旧 1000+ 条，rows 停在 5000
```

### 6.3 验收方法

**① 断电模拟（重点验收）**
```
1. 启动 demo → 点「模拟:批量50条」几次，写入报警
2. 终端: kill -9 <pid>（暴力杀进程，不等任何清理）
3. 重启 demo → 报警列表恢复，数据一条不丢、无损坏
4. 用 DB Browser for SQLite 打开 alarms.db 校验
   （DB Browser: File → Open Database → alarms.db → Browse Data）
```
> WAL 保证：已提交但未 checkpoint 的事务记录在 `alarms.db-wal` 文件中，
> 崩溃后下次打开自动重放（recovery），这是"断电不丢"的原理。

**② 日志滚动（防无限膨胀）**
```
点「滚动演示:塞6000条」→ 日志出现 [alarm] roll: removed 1000+ oldest
报警信息标签 rows 停在 5000（= maxRows），db 大小不再无限增长
```

**③ DB Browser 校验**
```
1. alarms.db 与 alarms.db-wal / -shm 三个文件并存（WAL 模式正常特征）
2. Browse Data 看到表结构与数据；等级/确认字段与界面一致
```

### 6.4 周日复盘：背诵要点

> **WAL 作用（journal_mode=WAL）**
> 1. **断电安全**：已提交事务先写进 `-wal` 文件，崩溃恢复时自动重放，不丢已提交数据（传统 journal 模式崩溃时损坏风险更高）；
> 2. **读不阻塞写、写不阻塞读**：HMI 一边查报警列表一边写报警不互相卡；
> 3. 减少 fsync 次数，写性能更好。
> 代价：同库多进程同时写需共享内存（-shm），本 demo 单进程无需担心。

> **为什么工业场景要批量事务**
> 1. **原子性**：50 条报警要么全部落库、要么全部不落，不会"半批"；
> 2. **降 Flash 磨损**：每次 commit 都是一次完整写入周期（含 fsync），
>    50 条一个事务 = 1 次 commit，而非 50 次；
> 3. 性能：批量 commit 远快于逐条 commit。

> **Flash 频繁写的风险**
> 1. eMMC/NAND 的擦写次数有限（几千~几万次），频繁小写入会加速磨损、缩短寿命；
> 2. 磨损严重 → 坏块 → 存储故障 → 工业现场数据丢失；
> 3. 对策：批量事务（减少 commit 次数）+ 日志滚动（防库无限膨胀 = 防无限写）+
>    尽量减少不必要写入（如 WAL 下避免高频 checkpoint）。

### 6.5 记录的新坑

> **坑 7：procfs 伪文件的 `size()==0` 会骗过 QFile::atEnd()**
> `/proc/self/status` 的 stat 大小恒为 0，而 `QFile::atEnd()` 实现是 `pos>=size()`，
> 导致 `while(!f.atEnd())` 一次都不执行、读 0 行（rss 恒 0.0 MB）。
> 解决：用 `f.read(65536)` 直接读底层字节，绕过 `atEnd()/size()` 逻辑。

> **坑 8：QSqlDatabase 的 removeDatabase 时序**
> 必须在所有 `QSqlDatabase` 对象销毁后才能 `removeDatabase()`，否则 Qt 打印
> "connection ... is still in use" 告警。本 demo 用指针成员 + 析构里先 delete 再 remove。

> **坑 9：WAL 模式下库文件是"三个"**
> `alarms.db`（主库）+ `alarms.db-wal`（未 checkpoint 的写日志）+ `alarms.db-shm`（共享内存索引）。
> 别只拷贝主库文件做备份（会缺 -wal 里的已提交事务）；断电模拟后 `-wal` 会被重放合并。

---

## 八、文件结构

```
modbus_tcp_demo/
├── CMakeLists.txt
├── README.md
└── src/
    ├── main.cpp               # 入口：组装通信层 + 存储层 + UI
    ├── modbusmanager.h/.cpp   # 核心通信类（解耦、可复用）
    ├── alarmlogger.h/.cpp     # 报警日志存储类（SQLite/WAL/事务/滚动）
    └── mainwindow.h/.cpp      # 测试 UI（可整体删除）
```
