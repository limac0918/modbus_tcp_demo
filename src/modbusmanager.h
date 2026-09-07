#ifndef MODBUSMANAGER_H
#define MODBUSMANAGER_H

// =====================================================================
// ModbusManager —— 独立的 Modbus TCP 主站通信层（与 UI 完全解耦）
// ---------------------------------------------------------------------
// 职责：
//  1. 封装 QModbusTcpClient 的生命周期（连接/断开/状态机）
//  2. 心跳机制：周期读测试寄存器判链路存活
//  3. 断线检测：状态信号 + 错误信号 + 心跳丢失三重判定
//  4. 自动重连：指数退避，避免高频重连
//  5. 请求队列：限制并发 + 超时 + 丢弃过期，防止压垮 PLC / 内存暴涨
//  6. 模拟故障入口：供 UI/测试触发离线、改端口等场景
//
// 对外只暴露信号和槽，不依赖任何 UI 控件：
//   入：start() / stop() / enqueueRead()
//   出：stateChanged / logMessage / errorOccurred / dataReady / heartbeatLost
//
// 【坑】QModbus 底层不会替你管理"请求超时清队列 / 断线收尾 / 重连调度"，
//       这些必须在通信层自己做 —— 这正是本类存在的意义。
// =====================================================================

#include <QObject>
#include <QPointer>
#include <QElapsedTimer>
#include <QModbusTcpClient>
#include <QModbusReply>
#include <QModbusDataUnit>
#include <QQueue>
#include <QHash>
#include <QTimer>
#include <QVector>
#include <functional>

// ---------------------------------------------------------------------
// 一个排队请求的封装
// ---------------------------------------------------------------------
struct ModbusRequestItem {
    QModbusDataUnit::RegisterType type = QModbusDataUnit::HoldingRegisters;
    int startAddress = 0;                 // 起始偏移
    int count = 1;                        // 寄存器个数
    int serverAddress = 1;                // 单元 ID / 从站地址
    qint64 deadlineMs = 0;                // 绝对过期时刻（相对 m_elapsed）
    quint64 seq = 0;                      // 全局递增序号（防错配/调试）
    bool isHeartbeat = false;             // 是否心跳请求
    std::function<void(bool ok, const QVector<quint16> &values)> callback;
};

class ModbusManager : public QObject
{
    Q_OBJECT
public:
    explicit ModbusManager(QObject *parent = nullptr);
    ~ModbusManager() override;

    // ---- 配置（必须在 start 之前设置）----
    void setServer(const QString &host, quint16 port);
    void setHeartbeatParams(int intervalMs, int maxFails);
    void setQueueParams(int maxConcurrent, int queueLimit);
    void setResponseTimeout(int ms);      // 单次请求响应超时（给底层）
    void setReconnectParams(int baseMs, int maxMs);

    // ---- 控制 ----
    void start();                          // 启动连接（内部自动开始心跳/重连/清理）
    void stop();                           // 停止（用户主动，不触发重连）
    void reconnectNow();                   // 立即重连一次（手动）

    // ---- 通用读请求（业务方唯一入口）----
    // 返回 seq；失败（队列满）返回 0。cb 在完成/失败/超时/被丢弃时都会以 ok=false 触发。
    quint64 enqueueRead(QModbusDataUnit::RegisterType type,
                        int startAddress, int count, int serverAddress = 1,
                        std::function<void(bool, const QVector<quint16> &)> cb = nullptr);

    // ---- 模拟故障（供 UI/测试触发，验收"反复开关从站不崩"）----
    void simulateNetworkDrop();            // 模拟网络闪断：直接断开
    void simulateBadPort();                // 模拟 PLC 离线：切到错误端口（重连必失败）
    void restoreGoodPort();                // 恢复正确端口

    // 状态查询
    bool isConnected() const { return m_deviceState == QModbusDevice::ConnectedState; }
    int  pendingCount() const { return m_pending.size() + m_inflight.size(); }

signals:
    void stateChanged(const QString &state);                       // connected / connecting / unconnected
    void logMessage(const QString &msg);                           // 全部日志，UI 直接接
    void errorOccurred(const QString &err);                        // 底层错误
    void dataReady(int startAddress, const QVector<quint16> &values); // 成功读到的业务数据
    void heartbeatLost();                                          // 心跳连续失败（链路已死）

private slots:
    void onClientStateChanged(QModbusDevice::State state);
    void onClientErrorOccurred(QModbusDevice::Error error);
    void onSweepTimeout();                 // 周期清理：超时请求 / 过期队列
    void onHeartbeatTimeout();             // 周期发心跳
    void onReconnectTimeout();             // 退避到期，尝试重连
    void onConnectGuardTimeout();          // 连接建立超时守卫

private:
    struct InflightEntry {
        ModbusRequestItem item;
        QPointer<QModbusReply> reply;
    };

    void connectDevice();
    void scheduleReconnect();
    void pumpQueue();                      // 队列调度核心（并发限制）
    bool sendItem(const ModbusRequestItem &item);
    void processReply(QModbusReply *reply);
    void flushQueue(const QString &reason);// 断线/停止时收尾所有请求
    void notifyFail(const ModbusRequestItem &item, const QString &reason);
    void enqueueHeartbeat();
    void handleHeartbeatResult(bool ok);

    // --- 底层 client ---
    QModbusTcpClient *m_client = nullptr;
    QString m_host;
    quint16 m_port = 502;
    quint16 m_goodPort = 502;
    QString m_goodHost;
    bool m_badPortSim = false;

    // --- 队列 ---
    QQueue<ModbusRequestItem> m_pending;                 // 待发
    QHash<quint64, InflightEntry> m_inflight;            // 在途（按 seq）
    QHash<QModbusReply *, quint64> m_replySeq;           // reply -> seq
    int m_maxConcurrent = 1;                             // 并发上限（默认 1，最稳）
    int m_queueLimit = 200;                              // 队列上限，防内存暴涨
    quint64 m_nextSeq = 1;

    // --- 心跳 ---
    QTimer m_heartbeatTimer;
    int m_heartbeatIntervalMs = 2000;
    int m_heartbeatMaxFails = 3;
    int m_heartbeatFailCount = 0;
    bool m_heartbeatInFlight = false;

    // --- 重连（指数退避）---
    QTimer m_reconnectTimer;
    int m_reconnectAttempts = 0;
    int m_reconnectBaseMs = 1000;
    int m_reconnectMaxMs = 30000;

    // --- 超时 ---
    QTimer m_sweepTimer;                   // 200ms 清扫
    int m_responseTimeoutMs = 1500;        // 单请求响应超时
    QTimer m_connectGuard;
    int m_connectGuardMs = 3000;           // 建连超时守卫

    // --- 状态 ---
    QModbusDevice::State m_deviceState = QModbusDevice::UnconnectedState;
    bool m_userStopped = false;

    QElapsedTimer m_elapsed;               // 全局时间基准（deadline 用它计算）
};

#endif // MODBUSMANAGER_H
