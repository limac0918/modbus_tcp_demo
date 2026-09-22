#include "modbusmanager.h"

#include <QDebug>
#include <QList>
#include <QtGlobal>

// =====================================================================
// ModbusManager 实现
// =====================================================================

ModbusManager::ModbusManager(QObject *parent)
    : QObject(parent)
{
    m_client = new QModbusTcpClient(this);

    // 底层状态/错误信号 → 我们统一再转发
    connect(m_client, &QModbusDevice::stateChanged,
            this, &ModbusManager::onClientStateChanged);
    connect(m_client, &QModbusDevice::errorOccurred,
            this, &ModbusManager::onClientErrorOccurred);

    // 周期清扫：丢弃过期请求（防堆积）
    m_sweepTimer.setInterval(200);
    connect(&m_sweepTimer, &QTimer::timeout, this, &ModbusManager::onSweepTimeout);
    m_sweepTimer.start();

    // 心跳
    connect(&m_heartbeatTimer, &QTimer::timeout,
            this, &ModbusManager::onHeartbeatTimeout);

    // 重连退避
    connect(&m_reconnectTimer, &QTimer::timeout,
            this, &ModbusManager::onReconnectTimeout);

    // 建连超时守卫
    connect(&m_connectGuard, &QTimer::timeout,
            this, &ModbusManager::onConnectGuardTimeout);

    m_elapsed.start();
}

ModbusManager::~ModbusManager() = default;

// ---------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------
void ModbusManager::setServer(const QString &host, quint16 port)
{
    m_goodHost = host;
    m_goodPort = port;
    m_host = host;
    m_port = port;
}

void ModbusManager::setHeartbeatParams(int intervalMs, int maxFails)
{
    m_heartbeatIntervalMs = qMax(200, intervalMs);
    m_heartbeatMaxFails = qMax(1, maxFails);
    m_heartbeatTimer.setInterval(m_heartbeatIntervalMs);
}

void ModbusManager::setQueueParams(int maxConcurrent, int queueLimit)
{
    m_maxConcurrent = qMax(1, maxConcurrent);
    m_queueLimit = qMax(1, queueLimit);
}

void ModbusManager::setResponseTimeout(int ms)
{
    m_responseTimeoutMs = qMax(200, ms);
}

void ModbusManager::setReconnectParams(int baseMs, int maxMs)
{
    m_reconnectBaseMs = qMax(200, baseMs);
    m_reconnectMaxMs = qMax(baseMs, maxMs);
}

// ---------------------------------------------------------------------
// 控制
// ---------------------------------------------------------------------
void ModbusManager::start()
{
    m_userStopped = false;
    m_reconnectAttempts = 0;
    if (!m_sweepTimer.isActive())
        m_sweepTimer.start(); // stop() 停掉后，start() 需重启清扫
    emit logMessage(QStringLiteral("[mgr] start, target %1:%2").arg(m_host).arg(m_port));
    connectDevice();
}

void ModbusManager::stop()
{
    m_userStopped = true;
    m_reconnectTimer.stop();
    m_connectGuard.stop();
    m_heartbeatTimer.stop();
    m_sweepTimer.stop();
    if (m_client->state() != QModbusDevice::UnconnectedState)
        m_client->disconnectDevice(); // 会触发 stateChanged -> Unconnected（不重连，因为 userStopped）
    flushQueue(QStringLiteral("stopped"));
    emit logMessage(QStringLiteral("[mgr] stopped"));
}

void ModbusManager::reconnectNow()
{
    m_reconnectTimer.stop();
    emit logMessage(QStringLiteral("[mgr] manual reconnect"));
    connectDevice();
}

// ---------------------------------------------------------------------
// 请求入口
// ---------------------------------------------------------------------
quint64 ModbusManager::enqueueRead(QModbusDataUnit::RegisterType type,
                                   int startAddress, int count,
                                   int serverAddress,
                                   std::function<void(bool, const QVector<quint16> &)> cb)
{
    // 入队失败（已停止 / 队列满）：直接返回 0，不调 cb。
    // 调用方根据返回值 == 0 自行计数失败——这样 UI 侧 busy 计数不会因为
    // "没 ++ 却 --" 而无符号下溢。
    if (m_userStopped)
    {
        return 0;
    }
    if (m_pending.size() >= m_queueLimit)
    {
        emit logMessage(QStringLiteral("[queue] full (%1), request rejected").arg(m_queueLimit));
        return 0;
    }

    ModbusRequestItem item;
    item.type = type;
    item.startAddress = startAddress;
    item.count = count;
    item.serverAddress = serverAddress;
    // 队列等待给 500ms 预算，避免刚入队就被 sweep 当"过期"丢掉
    item.deadlineMs = m_elapsed.elapsed() + m_responseTimeoutMs + 500;
    item.seq = m_nextSeq++;
    item.callback = std::move(cb);

    m_pending.enqueue(item);
    pumpQueue();
    return item.seq;
}

// ---------------------------------------------------------------------
// 业务语义接口：读保持寄存器（第四周：把 Modbus 枚举概念封在通信层内）
// ---------------------------------------------------------------------
quint64 ModbusManager::enqueueReadHolding(int startAddress, int count,
                                          int serverAddress,
                                          std::function<void(bool, const QVector<quint16> &)> cb)
{
    // QModbusDataUnit::HoldingRegisters 只在这里出现 —— UI 层看不到这个概念
    return enqueueRead(QModbusDataUnit::HoldingRegisters,
                       startAddress, count, serverAddress, std::move(cb));
}

// ---------------------------------------------------------------------
// 队列调度核心
// ---------------------------------------------------------------------
void ModbusManager::pumpQueue()
{
    if (m_deviceState != QModbusDevice::ConnectedState)
        return; // 未连接不发送，等重连成功后再调度

    // 只在并发槽有空位时发
    while (m_inflight.size() < m_maxConcurrent && !m_pending.isEmpty())
    {
        ModbusRequestItem item = m_pending.dequeue();
        if (item.deadlineMs <= m_elapsed.elapsed())
        {
            // 排到队头时已过期 → 丢弃（不发送）
            notifyFail(item, QStringLiteral("stale before send"));
            continue;
        }
        if (!sendItem(item))
        {
            // 发送失败（多半是连接刚断），放回队尾稍后再试
            m_pending.enqueue(item);
            break;
        }
    }
}

bool ModbusManager::sendItem(const ModbusRequestItem &item)
{
    QModbusDataUnit unit(item.type, item.startAddress, static_cast<quint16>(item.count));
    QModbusReply *reply = m_client->sendReadRequest(unit, item.serverAddress);
    if (!reply)
    {
        emit logMessage(QStringLiteral("[req #%1] send failed (no reply)").arg(item.seq));
        return false;
    }

    InflightEntry entry;
    entry.item = item;
    entry.reply = reply;
    m_inflight.insert(item.seq, entry);
    m_replySeq.insert(reply, item.seq);

    connect(reply, &QModbusReply::finished, this, [this, reply]()
            { processReply(reply); });
    return true;
}

// ---------------------------------------------------------------------
// 响应处理
// ---------------------------------------------------------------------
void ModbusManager::processReply(QModbusReply *reply)
{
    quint64 seq = m_replySeq.take(reply);
    if (seq == 0)
    {
        // 该 reply 已被超时/断线清理过，忽略
        reply->deleteLater();
        return;
    }

    auto it = m_inflight.find(seq);
    if (it == m_inflight.end())
    {
        reply->deleteLater();
        return;
    }
    ModbusRequestItem item = it->item;
    m_inflight.erase(it);

    const bool ok = (reply->error() == QModbusDevice::NoError);
    QVector<quint16> values;
    if (ok)
    {
        const QModbusDataUnit result = reply->result();
        values.reserve(result.valueCount());
        for (int i = 0; i < result.valueCount(); ++i)
            values.append(static_cast<quint16>(result.value(i)));
    }

    if (item.isHeartbeat)
    {
        handleHeartbeatResult(ok);
        if (ok)
            emit logMessage(QStringLiteral("[heartbeat] ok"));
    }
    else
    {
        emit logMessage(QStringLiteral("[req #%1] done ok=%2").arg(item.seq).arg(ok));
    }

    if (item.callback)
        item.callback(ok, values);

    // 只有成功的业务数据才对外抛（心跳不重复抛）
    if (!item.isHeartbeat && ok)
        emit dataReady(item.startAddress, values);

    reply->deleteLater();
    pumpQueue(); // 腾出并发槽，继续调度
}

// ---------------------------------------------------------------------
// 心跳
// ---------------------------------------------------------------------
void ModbusManager::onHeartbeatTimeout()
{
    if (m_deviceState != QModbusDevice::ConnectedState)
        return;
    enqueueHeartbeat();
}

void ModbusManager::enqueueHeartbeat()
{
    if (m_heartbeatInFlight)
        return; // 上一个心跳还没结果，不重复发（避免堆积）

    if (m_pending.size() >= m_queueLimit)
        return;

    m_heartbeatInFlight = true;
    ModbusRequestItem item;
    item.type = QModbusDataUnit::HoldingRegisters;
    item.startAddress = 0;
    item.count = 1;
    item.serverAddress = 1;
    item.deadlineMs = m_elapsed.elapsed() + m_responseTimeoutMs + 1000;
    item.seq = m_nextSeq++;
    item.isHeartbeat = true;
    m_pending.prepend(item); // 心跳优先（插队）
    pumpQueue();
}

void ModbusManager::handleHeartbeatResult(bool ok)
{
    m_heartbeatInFlight = false;
    if (ok)
    {
        m_heartbeatFailCount = 0;
        return;
    }
    m_heartbeatFailCount++;
    emit logMessage(QStringLiteral("[heartbeat] fail %1/%2")
                        .arg(m_heartbeatFailCount)
                        .arg(m_heartbeatMaxFails));
    if (m_heartbeatFailCount >= m_heartbeatMaxFails)
    {
        m_heartbeatFailCount = 0;
        emit heartbeatLost();
        emit logMessage(QStringLiteral("[heartbeat] LOST -> force reconnect"));
        // 强制断开：会走 stateChanged(Unconnected) -> scheduleReconnect
        if (m_deviceState == QModbusDevice::ConnectedState)
            m_client->disconnectDevice();
    }
}

// ---------------------------------------------------------------------
// 周期清扫：超时 / 过期
// ---------------------------------------------------------------------
void ModbusManager::onSweepTimeout()
{
    if (m_userStopped)
        return;

    const qint64 now = m_elapsed.elapsed();

    // 1) 在途请求超时 → 取消（reply 后续到达会被 processReply 判 seq==0 忽略）
    QList<quint64> stale;
    for (auto it = m_inflight.cbegin(); it != m_inflight.cend(); ++it)
    {
        if (it->item.deadlineMs <= now)
            stale.append(it.key());
    }
    for (quint64 seq : stale)
    {
        auto entry = m_inflight.take(seq);
        if (entry.reply)
        {
            m_replySeq.remove(entry.reply);
            entry.reply->deleteLater();
        }
        notifyFail(entry.item, QStringLiteral("timeout"));
    }

    // 2) 队首过期 → 丢弃
    while (!m_pending.isEmpty() && m_pending.head().deadlineMs <= now)
    {
        ModbusRequestItem item = m_pending.dequeue();
        notifyFail(item, QStringLiteral("stale"));
    }

    pumpQueue();
}

// ---------------------------------------------------------------------
// 失败统一出口
// ---------------------------------------------------------------------
void ModbusManager::notifyFail(const ModbusRequestItem &item, const QString &reason)
{
    if (item.isHeartbeat)
    {
        handleHeartbeatResult(false); // 心跳失败累计 + 可能触发强制重连
    }
    else
    {
        emit logMessage(QStringLiteral("[req #%1] dropped: %2").arg(item.seq).arg(reason));
    }
    if (item.callback)
        item.callback(false, {});
}

// ---------------------------------------------------------------------
// 断线 / 停止时收尾所有在途与排队请求
// ---------------------------------------------------------------------
void ModbusManager::flushQueue(const QString &reason)
{
    while (!m_pending.isEmpty())
    {
        ModbusRequestItem item = m_pending.dequeue();
        notifyFail(item, reason);
    }
    QList<quint64> keys = m_inflight.keys();
    for (quint64 seq : keys)
    {
        InflightEntry entry = m_inflight.take(seq);
        if (entry.reply)
        {
            m_replySeq.remove(entry.reply);
            entry.reply->deleteLater();
        }
        notifyFail(entry.item, reason);
    }
}

// ---------------------------------------------------------------------
// 连接 / 状态机 / 重连
// ---------------------------------------------------------------------
void ModbusManager::connectDevice()
{
    if (m_userStopped)
        return;

    m_client->setConnectionParameter(QModbusDevice::NetworkAddressParameter, m_host);
    m_client->setConnectionParameter(QModbusDevice::NetworkPortParameter, m_port);
    m_client->setTimeout(m_responseTimeoutMs);
    // 重试交给我们自己管理（指数退避），底层不再重试
    m_client->setNumberOfRetries(0);

    if (m_client->state() != QModbusDevice::UnconnectedState)
        m_client->disconnectDevice();

    m_client->connectDevice();
    m_connectGuard.start(m_connectGuardMs); // 建连超时兜底
    emit logMessage(QStringLiteral("[mgr] connecting %1:%2 ...").arg(m_host).arg(m_port));
}

void ModbusManager::onClientStateChanged(QModbusDevice::State state)
{
    m_deviceState = state;
    switch (state)
    {
    case QModbusDevice::ConnectedState:
        m_connectGuard.stop();
        m_reconnectAttempts = 0; // 退避重置
        emit stateChanged(QStringLiteral("connected"));
        emit logMessage(QStringLiteral("[mgr] CONNECTED"));
        m_heartbeatTimer.start(m_heartbeatIntervalMs);
        pumpQueue(); // 把积压请求发出去
        break;
    case QModbusDevice::UnconnectedState:
        m_connectGuard.stop();
        m_heartbeatTimer.stop();
        emit stateChanged(QStringLiteral("unconnected"));
        if (m_userStopped)
        {
            flushQueue(QStringLiteral("stopped"));
        }
        else
        {
            emit logMessage(QStringLiteral("[mgr] connection lost"));
            flushQueue(QStringLiteral("connection lost"));
            scheduleReconnect();
        }
        break;
    default:
        emit stateChanged(QStringLiteral("connecting"));
        break;
    }
}

void ModbusManager::onClientErrorOccurred(QModbusDevice::Error error)
{
    QString s;
    switch (error)
    {
    case QModbusDevice::NoError:
        s = QStringLiteral("NoError");
        break;
    case QModbusDevice::ReadError:
        s = QStringLiteral("ReadError");
        break;
    case QModbusDevice::WriteError:
        s = QStringLiteral("WriteError");
        break;
    case QModbusDevice::ConnectionError:
        s = QStringLiteral("ConnectionError");
        break;
    case QModbusDevice::ConfigurationError:
        s = QStringLiteral("ConfigurationError");
        break;
    case QModbusDevice::TimeoutError:
        s = QStringLiteral("TimeoutError");
        break;
    case QModbusDevice::ProtocolError:
        s = QStringLiteral("ProtocolError");
        break;
    default:
        s = QStringLiteral("UnknownError");
        break;
    }
    emit errorOccurred(s);
    emit logMessage(QStringLiteral("[mgr] client error: %1").arg(s));
}

void ModbusManager::scheduleReconnect()
{
    if (m_userStopped)
        return;

    m_reconnectTimer.stop(); // 幂等：避免双调度
    m_reconnectAttempts++;
    // 指数退避：base << (attempts-1)，封顶 max；attempts 太大时按 12 封顶位移
    const int shift = qMin(m_reconnectAttempts - 1, 12);
    const qint64 delay = qMin<qint64>(qint64(m_reconnectBaseMs) << shift, m_reconnectMaxMs);
    emit logMessage(QStringLiteral("[mgr] schedule reconnect in %1 ms (attempt %2)")
                        .arg(delay)
                        .arg(m_reconnectAttempts));
    m_reconnectTimer.start(int(delay));
}

void ModbusManager::onReconnectTimeout()
{
    if (m_userStopped)
        return;
    if (m_deviceState == QModbusDevice::ConnectedState)
        return;
    connectDevice();
}

void ModbusManager::onConnectGuardTimeout()
{
    if (m_userStopped)
        return;
    if (m_deviceState == QModbusDevice::ConnectedState)
        return;
    emit logMessage(QStringLiteral("[mgr] connect timeout, abort & retry"));
    m_client->disconnectDevice(); // 触发 Unconnected -> scheduleReconnect（指数退避）
}

// ---------------------------------------------------------------------
// 模拟故障
// ---------------------------------------------------------------------
void ModbusManager::simulateNetworkDrop()
{
    emit logMessage(QStringLiteral("[sim] network drop (disconnect)"));
    if (m_client->state() != QModbusDevice::UnconnectedState)
        m_client->disconnectDevice();
}

void ModbusManager::simulateBadPort()
{
    if (m_badPortSim)
        return;
    m_badPortSim = true;
    emit logMessage(QStringLiteral("[sim] switch to BAD port -> simulate PLC offline"));
    m_port = static_cast<quint16>(m_goodPort + 1); // 指向错误端口
    m_reconnectTimer.stop();
    m_heartbeatTimer.stop();
    if (m_client->state() != QModbusDevice::UnconnectedState)
        m_client->disconnectDevice();
    scheduleReconnect(); // 重连会一直失败 → 观察退避
}

void ModbusManager::restoreGoodPort()
{
    if (!m_badPortSim)
        return;
    m_badPortSim = false;
    emit logMessage(QStringLiteral("[sim] restore GOOD port"));
    m_port = m_goodPort;
    m_reconnectTimer.stop();
    scheduleReconnect(); // 重连应恢复成功
}
