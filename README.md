# Modbus TCP 主站 Demo —— 心跳 / 断线检测 / 自动重连 / 请求队列

一个生产级思路的 Qt Modbus TCP 主站封装，含 UI 测试入口。

---

## 一、架构：通信层与 UI 完全解耦

```
┌──────────────────────────────────────────────────┐
│ UI 层 (MainWindow，纯测试，可整体删除)            │
│   按钮/日志/状态  ←—— 只通过信号接通信层数据       │
└─────────────────────────┬────────────────────────┘
                          │  信号: stateChanged / logMessage /
                          │        errorOccurred / dataReady / heartbeatLost
                          │  槽:   start() / stop() / enqueueRead()
┌─────────────────────────▼────────────────────────┐
│ 通信层 (ModbusManager，零 UI 依赖)                │
│   ├─ QModbusTcpClient 生命周期/状态机             │
│   ├─ 心跳 Heartbeat：定时读 0x0000 判链路存活     │
│   ├─ 断线检测：状态信号 + 错误信号 + 心跳丢失      │
│   ├─ 自动重连：指数退避（1s→2s→…→30s 封顶）       │
│   └─ 请求队列：并发限制 + 超时 + 过期丢弃          │
└──────────────────────────────────────────────────┘
```

**关键约束**：`ModbusManager` 不 include 任何 UI 头文件、不持有任何控件指针。
外部想用：
- 业务方：`enqueueRead(...)` 发请求，`dataReady` 拿数据；
- UI/监控：connect 上述信号即可。

`MainWindow` 只是"测试夹具"——删除它，通信层可直接放进控制台 / 服务端工程。

---

## 二、构建

```bash
# 依赖：Qt5 (QtSerialBus 模块), CMake >= 3.16, 编译器支持 C++17
# 最低 Qt 5.11（用到 QModbusDevice::errorOccurred 信号），建议 5.12 LTS 及以上
# Ubuntu:
#   sudo apt install qtbase5-dev libqt5serialbus5-dev
#   # 若需串口/网络相关额外模块，一并安装:
#   sudo apt install libqt5serialport5-dev
# （本工程只用 TCP，无需串口模块；RTU 场景才需要 QtSerialPort）

cd modbus_tcp_demo
cmake -B build
cmake --build build -j
./build/modbus_tcp_demo
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

## 六、周日复盘：异常场景梳理

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

## 七、文件结构

```
modbus_tcp_demo/
├── CMakeLists.txt
├── README.md
└── src/
    ├── main.cpp               # 入口：组装通信层 + UI
    ├── modbusmanager.h/.cpp   # 核心通信类（解耦、可复用）
    ├── mainwindow.h/.cpp      # 测试 UI（可整体删除）
```
