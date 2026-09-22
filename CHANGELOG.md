# Changelog

> 使用说明：每周发版时，把最上面的 `[Unreleased]` 小节改名为 `[vX.Y.Z] - 日期`，
> 并在其上方新建一个空的 `[Unreleased]` 小节。各版本按时间倒序排列，最新在最上面。

版本号遵循**语义化版本**：`主.次.补丁`
（破坏性改动 → 主版本 +1；新增功能 → 次版本 +1；问题修复 → 补丁 +1）。
发布节奏：**每周迭代一版**，发版同时执行 `./release.sh` 打 tag 并双推 GitHub / Gitee。

## [Unreleased]

## [v1.4.0] - 2026-09-22

### 新增
- 通信层新增业务语义接口 enqueueReadHolding(startAddress, count, serverAddress, cb)
- main 注释补第四周验收目标（解耦三条）

### 修复
- UI层`onStressClicked` 里 `seq==0` 时手动 `++m_failCount`
- 通信层 入队失败（userStopped / 队列满）时**不再同步调 cb**，直接返回 0

### 变更
- UI层`onStressClicked` 压测改为 `m_mgr->enqueueReadHolding(addr, 2, 1, cb)`，UI 不再碰寄存器类型
- 通信层实现：内部转发 `enqueueRead(QModbusDataUnit::HoldingRegisters, ...)`——**Modbus 枚举只在通信层出现一次**
- 通信层更新注释："返回 0 = 入队失败，不调 cb；返回非 0 = 回调一定会被调"

## [v1.3.0] - 2026-09-11

### 新增
- 报警日志模块：新增 AlarmLogger，实现报警记录的采集
- 报警日志持久化：SQLite 存储，程序重启后记录不丢失
- 报警日志集成：主界面接入报警日志展示

### 变更
- 构建配置：CMake 接入 SQLite 依赖

## [v1.0.0] - 2026-09-08

### 新增
- 首个可运行版本：Modbus TCP 客户端读写保持寄存器（功能码 03 / 06 / 16）

### 修复
- （首次发布，无修复项）

### 变更
- （首次发布，无变更项）
