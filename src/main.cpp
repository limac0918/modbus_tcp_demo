#include <QApplication>
#include "modbusmanager.h"
#include "mainwindow.h"

// =====================================================================
// Modbus TCP 主站 Demo —— 心跳 / 断线检测 / 自动重连 / 请求队列
// （Qt5 + QtSerialBus 模块）
// ---------------------------------------------------------------------
// 验收目标（反复开关 Modbus-Slave，程序不崩溃、自动恢复、不堆积内存）：
//   1. 正常连接，心跳每 2s 读 0x0000；
//   2. 关掉从站 → 心跳 3 连失败 → 强制重连（指数退避 1s→2s→4s…→30s封顶）；
//   3. 重新打开从站 → 自动恢复连接，队列积压请求全部发出；
//   4. 压测塞 500 请求 → 队列上限 200，超出的被拒绝，内存(RSS)不涨；
//   5. 反复「模拟闪断 / 改错端口 / 恢复」→ 不崩溃、不泄漏。
// =====================================================================

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // ---- 通信层：纯 Qt 类，不依赖 UI ----
    ModbusManager mgr;
    mgr.setServer(QStringLiteral("127.0.0.1"), 502);
    mgr.setHeartbeatParams(2000, 3);          // 心跳 2s，连续 3 次失败判死
    mgr.setQueueParams(1, 200);               // 并发 1，队列上限 200
    mgr.setResponseTimeout(1500);             // 单请求响应超时 1.5s
    mgr.setReconnectParams(1000, 30000);      // 退避 1s 起，30s 封顶

    // ---- UI：纯测试用途（可整体删除，通信层仍可工作）----
    MainWindow w(&mgr);
    w.show();

    return app.exec();
}
