#include <QApplication>
#include <QStandardPaths>
#include <QDir>
#include "modbusmanager.h"
#include "alarmlogger.h"
#include "mainwindow.h"

// =====================================================================
// Modbus TCP 主站 Demo —— 心跳 / 断线检测 / 自动重连 / 请求队列
//           + 第3周：HMI 报警日志 SQLite 可靠存储
// （Qt5 + QtSerialBus + QtSql 模块）
// ---------------------------------------------------------------------
// 验收目标（第2周，Modbus）：
//   1. 正常连接，心跳每 2s 读 0x0000；
//   2. 关掉从站 → 心跳 3 连失败 → 强制重连（指数退避 1s→2s→4s…→30s封顶）；
//   3. 重新打开从站 → 自动恢复连接，队列积压请求全部发出；
//   4. 压测塞 500 请求 → 队列上限 200，超出的被拒绝，内存(RSS)不涨；
//   5. 反复「模拟闪断 / 改错端口 / 恢复」→ 不崩溃、不泄漏。
// 验收目标（第3周，报警日志）：
//   1. 模拟报警/批量报警 → 写入 SQLite，界面展示，DB Browser 校验文件；
//   2. 进程暴力杀死(kill -9) → 重启验证报警数据不损坏（WAL 保证）；
//   3. 滚动演示塞 6000 条 → 日志自动滚动保留 5000 条，库不无限膨胀。
// 验收目标（第4周，解耦整合）：
//   1. 网络闪断不崩溃、自动重连（沿用第2周，回归验证）；
//   2. 暴力杀进程报警不丢（沿用第3周 WAL，回归验证）；
//   3. 通信与 UI 完全解耦：UI 只收信号/调业务语义接口，
//      grep mainwindow.* 无 QModbus 字样；换 UI 不重写通信层。
// =====================================================================

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // 显式设置应用名（保证 AppDataLocation 路径稳定，不依赖 argv[0]）
    QCoreApplication::setApplicationName(QStringLiteral("modbus_tcp_demo"));

    // ---- 通信层：纯 Qt 类，不依赖 UI ----
    ModbusManager mgr;
    mgr.setServer(QStringLiteral("127.0.0.1"), 502);
    mgr.setHeartbeatParams(2000, 3);     // 心跳 2s，连续 3 次失败判死
    mgr.setQueueParams(1, 200);          // 并发 1，队列上限 200
    mgr.setResponseTimeout(1500);        // 单请求响应超时 1.5s
    mgr.setReconnectParams(1000, 30000); // 退避 1s 起，30s 封顶

    // ---- 存储层：报警日志（SQLite），纯 Qt 类，不依赖 UI ----
    // alarms.db 生成在程序工作目录；WAL 模式会产生 alarms.db-wal / -shm 伴随文件。
    AlarmLogger alarm;

    // ★ 统一数据库位置：~/.local/share/modbus_tcp_demo/alarms.db
    QString dbDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dbDir); // 目录不存在则创建
    QString dbPath = dbDir + QStringLiteral("/alarms.db");

    alarm.init(dbPath, 5000); // 超过 5000 条自动滚动

    // ---- UI：纯测试用途（可整体删除，通信/存储层仍可工作）----
    MainWindow w(&mgr, &alarm);
    w.show();

    return app.exec();
}
