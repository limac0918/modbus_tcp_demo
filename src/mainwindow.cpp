#include "mainwindow.h"
#include "modbusmanager.h"
#include "alarmlogger.h"

#include <QWidget>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QFile>
#include <QVector>
#include <QScrollBar>
#include <QRegularExpression>
#include <QDateTime>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QColor>
#include <QRandomGenerator>

MainWindow::MainWindow(ModbusManager *mgr, AlarmLogger *alarm, QWidget *parent)
    : QWidget(parent), m_mgr(mgr), m_alarm(alarm)
{
    setWindowTitle(QStringLiteral("Modbus TCP Manager Demo (心跳/重连/队列/模拟故障) + 报警日志(SQLite)"));
    resize(920, 760);

    // ---- 连接参数 ----
    m_hostEdit = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    m_portEdit = new QLineEdit(QStringLiteral("502"), this);

    // ---- 控制按钮 ----
    m_connectBtn = new QPushButton(QStringLiteral("连接"), this);
    m_disconnectBtn = new QPushButton(QStringLiteral("断开"), this);
    m_dropBtn = new QPushButton(QStringLiteral("模拟:闪断"), this);
    m_badPortBtn = new QPushButton(QStringLiteral("模拟:改错端口(PLC离线)"), this);
    m_restoreBtn = new QPushButton(QStringLiteral("模拟:恢复"), this);
    m_stressBtn = new QPushButton(QStringLiteral("压测:塞500请求"), this);
    m_disconnectBtn->setEnabled(false);

    // ---- 日志与状态 ----
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000); // 防 UI 端内存暴涨

    m_stateLabel = new QLabel(QStringLiteral("state: unconnected"), this);
    m_metaLabel = new QLabel(QStringLiteral("rss: - | queue: -"), this);

    // ---- 布局：Modbus 区 ----
    auto *connBox = new QGroupBox(QStringLiteral("目标从站 (需先用 Modbus Slave 工具监听)"), this);
    auto *connLayout = new QHBoxLayout(connBox);
    connLayout->addWidget(new QLabel(QStringLiteral("IP:"), connBox));
    connLayout->addWidget(m_hostEdit);
    connLayout->addWidget(new QLabel(QStringLiteral("Port:"), connBox));
    connLayout->addWidget(m_portEdit);
    connLayout->addStretch();

    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(m_connectBtn);
    btnRow->addWidget(m_disconnectBtn);
    btnRow->addSpacing(12);
    btnRow->addWidget(m_dropBtn);
    btnRow->addWidget(m_badPortBtn);
    btnRow->addWidget(m_restoreBtn);
    btnRow->addSpacing(12);
    btnRow->addWidget(m_stressBtn);
    btnRow->addStretch();

    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(m_stateLabel);
    statusRow->addWidget(m_metaLabel);
    statusRow->addStretch();

    // ---- 布局：报警区（第3周）----
    auto *alarmBox = new QGroupBox(QStringLiteral("报警日志 (SQLite 可靠存储: WAL / 批量事务 / 日志滚动)"), this);
    auto *alarmLayout = new QVBoxLayout(alarmBox);

    auto *alarmBtnRow = new QHBoxLayout;
    auto *genBtn = new QPushButton(QStringLiteral("模拟:报1条警"), alarmBox);
    auto *genBBtn = new QPushButton(QStringLiteral("模拟:批量50条(单事务)"), alarmBox);
    auto *ackBtn = new QPushButton(QStringLiteral("确认选中"), alarmBox);
    auto *ackAllBtn = new QPushButton(QStringLiteral("全部确认"), alarmBox);
    auto *clearBtn = new QPushButton(QStringLiteral("清空报警"), alarmBox);
    auto *rollBtn = new QPushButton(QStringLiteral("滚动演示:塞6000条"), alarmBox);
    alarmBtnRow->addWidget(genBtn);
    alarmBtnRow->addWidget(genBBtn);
    alarmBtnRow->addSpacing(8);
    alarmBtnRow->addWidget(ackBtn);
    alarmBtnRow->addWidget(ackAllBtn);
    alarmBtnRow->addSpacing(8);
    alarmBtnRow->addWidget(clearBtn);
    alarmBtnRow->addWidget(rollBtn);
    alarmBtnRow->addStretch();

    m_alarmInfoLabel = new QLabel(QStringLiteral("rows: - | db: -"), alarmBox);

    m_alarmTable = new QTableWidget(0, 5, alarmBox);
    m_alarmTable->setHorizontalHeaderLabels(
        {QStringLiteral("ID"), QStringLiteral("时间"), QStringLiteral("内容"),
         QStringLiteral("等级"), QStringLiteral("状态")});
    m_alarmTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_alarmTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_alarmTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_alarmTable->verticalHeader()->setVisible(false);

    alarmLayout->addLayout(alarmBtnRow);
    alarmLayout->addWidget(m_alarmInfoLabel);
    alarmLayout->addWidget(m_alarmTable, 1);

    // ---- 总布局 ----
    auto *root = new QVBoxLayout(this);
    root->addWidget(connBox);
    root->addLayout(btnRow);
    root->addWidget(m_log, 1);
    root->addLayout(statusRow);
    root->addWidget(alarmBox, 1);

    // ---- 信号：通信层 → UI ----
    connect(m_mgr, &ModbusManager::logMessage, this, &MainWindow::appendLog);
    connect(m_mgr, &ModbusManager::stateChanged, this, [this](const QString &s)
            {
        m_stateLabel->setText(QStringLiteral("state: %1").arg(s));
        m_disconnectBtn->setEnabled(s == QLatin1String("connected")); });
    connect(m_mgr, &ModbusManager::errorOccurred, this, [this](const QString &e)
            { appendLog(QStringLiteral("[UI] error: %1").arg(e)); });
    connect(m_mgr, &ModbusManager::heartbeatLost, this, [this]()
            { appendLog(QStringLiteral("[UI] heartbeat lost! 链路判定已死")); });

    // ---- 信号：存储层 → UI ----
    if (m_alarm)
    {
        connect(m_alarm, &AlarmLogger::logMessage, this, &MainWindow::appendLog);
        connect(m_alarm, &AlarmLogger::alarmsChanged, this, &MainWindow::refreshAlarms);
        refreshAlarms(); // 启动时载入历史报警
    }

    // ---- 按钮 → 通信层 ----
    connect(m_connectBtn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(m_disconnectBtn, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);
    connect(m_dropBtn, &QPushButton::clicked, this, &MainWindow::onDropClicked);
    connect(m_badPortBtn, &QPushButton::clicked, this, &MainWindow::onBadPortClicked);
    connect(m_restoreBtn, &QPushButton::clicked, this, &MainWindow::onRestoreClicked);
    connect(m_stressBtn, &QPushButton::clicked, this, &MainWindow::onStressClicked);

    // ---- 按钮 → 报警层 ----
    connect(genBtn, &QPushButton::clicked, this, &MainWindow::onGenAlarmClicked);
    connect(genBBtn, &QPushButton::clicked, this, &MainWindow::onGenBatchClicked);
    connect(ackBtn, &QPushButton::clicked, this, &MainWindow::onAckClicked);
    connect(ackAllBtn, &QPushButton::clicked, this, &MainWindow::onAckAllClicked);
    connect(clearBtn, &QPushButton::clicked, this, &MainWindow::onClearAlarmsClicked);
    connect(rollBtn, &QPushButton::clicked, this, &MainWindow::onRollDemoClicked);

    // ---- 周期刷新元信息 ----
    connect(&m_tickTimer, &QTimer::timeout, this, &MainWindow::onTimerTick);
    m_tickTimer.start(500);

    appendLog(QStringLiteral("[UI] demo started. 先启动一个 Modbus Slave 监听 %1:%2 再点「连接」。")
                  .arg(m_hostEdit->text(), m_portEdit->text()));
    if (!m_alarm)
        appendLog(QStringLiteral("[UI] AlarmLogger 未初始化，报警区不可用。"));
}

void MainWindow::appendLog(const QString &msg)
{
    const QString line = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")) + QStringLiteral("  ") + msg;
    m_log->appendPlainText(line);
    // 自动滚动到底部
    m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}

quint64 MainWindow::selfRssKb()
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    // 关键：/proc 是 procfs 伪文件，stat 大小恒为 0。
    // QFile::atEnd() 的实现是 pos>=size()，在 size()==0 时会误判"已到末尾"，
    // 导致 while(!f.atEnd()) 循环一次都不执行、读不到任何内容（rss 恒 0）。
    // 必须用 f.read() 直接读底层字节（不经过 atEnd()/size() 逻辑）。
    const QByteArray data = f.read(65536);
    static const QRegularExpression re(QStringLiteral("^VmRSS:\\s*(\\d+)\\s*kB"));
    const QList<QByteArray> lines = data.split('\n');
    for (const QByteArray &line : lines)
    {
        const QRegularExpressionMatch m = re.match(QString::fromLatin1(line));
        if (m.hasMatch())
            return m.captured(1).toULongLong();
    }
    return 0;
}

// =====================================================================
// Modbus 槽
// =====================================================================
void MainWindow::onConnectClicked()
{
    m_mgr->setServer(m_hostEdit->text(), static_cast<quint16>(m_portEdit->text().toUShort()));
    m_mgr->start();
}

void MainWindow::onDisconnectClicked()
{
    m_mgr->stop();
}

void MainWindow::onDropClicked()
{
    m_mgr->simulateNetworkDrop();
}

void MainWindow::onBadPortClicked()
{
    m_mgr->simulateBadPort();
}

void MainWindow::onRestoreClicked()
{
    m_mgr->restoreGoodPort();
}

void MainWindow::onStressClicked()
{
    appendLog(QStringLiteral("[UI] stress: push 500 read requests (queue limited to %1, concurrent %2)")
                  .arg(200)
                  .arg(1));
    for (int i = 0; i < 500; ++i)
    {
        const int addr = (i % 100) * 2;
        const quint64 seq = m_mgr->enqueueReadHolding(
            addr, 2, 1,
            [this](bool ok, const QVector<quint16> &)
            {
                if (ok)
                    ++m_okCount;
                else
                    ++m_failCount;
                --m_busyReqCount; // 只有真入队成功的请求才会走到这里（seq!=0 才 ++）
            });
        if (seq != 0)
            ++m_busyReqCount;
        else
            ++m_failCount; // 队列满 / 已停止：入队失败，手动计数（Manager 不再同步调 cb）
    }
    appendLog(QStringLiteral("[UI] stress done, accepted=%1 (overflow auto-rejected)").arg(m_busyReqCount));
}

void MainWindow::onTimerTick()
{
    const quint64 rss = selfRssKb();
    m_metaLabel->setText(QStringLiteral("rss=%1 MB | queue=%2 | ok=%3 fail=%4 busy=%5")
                             .arg(rss / 1024.0, 0, 'f', 1)
                             .arg(m_mgr->pendingCount())
                             .arg(m_okCount)
                             .arg(m_failCount)
                             .arg(m_busyReqCount));

    // 报警信息标签也周期性刷新（条数/库大小，供滚动演示观察）
    if (m_alarm && m_alarm->isOpen())
    {
        m_alarmInfoLabel->setText(QStringLiteral("rows=%1 | maxRows=%2 | db=%3 KB")
                                      .arg(m_alarm->count())
                                      .arg(m_alarm->maxRows())
                                      .arg(m_alarm->dbSizeBytes() / 1024.0, 0, 'f', 1));
    }
}

// =====================================================================
// 报警日志槽（第3周）
// =====================================================================
void MainWindow::onGenAlarmClicked()
{
    if (!m_alarm || !m_alarm->isOpen())
    {
        appendLog(QStringLiteral("[UI] AlarmLogger 未就绪"));
        return;
    }
    AlarmItem it;
    it.ts = QDateTime::currentDateTime();
    it.level = static_cast<int>(QRandomGenerator::global()->bounded(3)); // 0/1/2
    it.message = QStringLiteral("HMI 模拟报警 #%1 (level %2)")
                     .arg(m_alarm->count() + 1)
                     .arg(it.level);
    m_alarm->writeOne(it); // 单条（内部事务）
}

void MainWindow::onGenBatchClicked()
{
    if (!m_alarm || !m_alarm->isOpen())
    {
        appendLog(QStringLiteral("[UI] AlarmLogger 未就绪"));
        return;
    }
    QVector<AlarmItem> batch;
    batch.reserve(50);
    for (int i = 0; i < 50; ++i)
    {
        AlarmItem it;
        it.ts = QDateTime::currentDateTime();
        it.level = static_cast<int>(QRandomGenerator::global()->bounded(3));
        it.message = QStringLiteral("批量报警 #%1 (batch, level %2)")
                         .arg(m_alarm->count() + i + 1)
                         .arg(it.level);
        batch.append(it);
    }
    m_alarm->writeBatch(batch); // 50 条一个事务提交
}

void MainWindow::onAckClicked()
{
    if (!m_alarm || !m_alarm->isOpen())
        return;

    const auto rows = m_alarmTable->selectionModel()->selectedRows(); // 所有选中整行
    if (rows.isEmpty())
    {
        appendLog(QStringLiteral("[UI] 请先在表格中选中一行再确认"));
        return;
    }

    QList<qint64> ids;
    ids.reserve(rows.size());
    for (const auto &idx : rows)
    {
        QTableWidgetItem *it = m_alarmTable->item(idx.row(), 0);
        if (it)
            ids.append(it->text().toLongLong()); // 判空更稳
    }
    if (ids.isEmpty())
        return;

    if (ids.size() == 1)
    {
        m_alarm->ackAlarm(ids.first()); // 单条：保持原逻辑
    }
    else
    {
        m_alarm->ackAlarms(ids); // 多条：走批量接口（见下）
    }
}

void MainWindow::onAckAllClicked()
{
    if (!m_alarm || !m_alarm->isOpen())
        return;
    const int n = m_alarm->ackAll();
    appendLog(QStringLiteral("[UI] ack all: %1 rows").arg(n));
}

void MainWindow::onClearAlarmsClicked()
{
    if (!m_alarm || !m_alarm->isOpen())
        return;
    m_alarm->clearAll();
    appendLog(QStringLiteral("[UI] alarms cleared"));
}

void MainWindow::onRollDemoClicked()
{
    if (!m_alarm || !m_alarm->isOpen())
    {
        appendLog(QStringLiteral("[UI] AlarmLogger 未就绪"));
        return;
    }
    // 塞 6000 条（maxRows 默认 5000）→ 写入时触发 rollIfNeeded，
    // 数据库最终保留 5000 条，最旧 1000+ 条被自动删除 → 库不会无限膨胀。
    appendLog(QStringLiteral("[UI] roll demo: push 6000 alarms (maxRows=%1)...")
                  .arg(m_alarm->maxRows()));
    QVector<AlarmItem> batch;
    batch.reserve(6000);
    for (int i = 0; i < 6000; ++i)
    {
        AlarmItem it;
        it.ts = QDateTime::currentDateTime();
        it.level = static_cast<int>(QRandomGenerator::global()->bounded(3));
        it.message = QStringLiteral("滚动测试报警 #%1").arg(i + 1);
        batch.append(it);
    }
    m_alarm->writeBatch(batch); // 单事务写 6000 条（同时演示批量事务性能）
}

void MainWindow::refreshAlarms()
{
    if (!m_alarm || !m_alarm->isOpen())
        return;
    const QVector<AlarmItem> rows = m_alarm->latestAlarms(200);
    m_alarmTable->setRowCount(rows.size());
    for (int r = 0; r < rows.size(); ++r)
    {
        const AlarmItem &it = rows.at(r);
        m_alarmTable->setItem(r, 0, new QTableWidgetItem(QString::number(it.id)));
        m_alarmTable->setItem(r, 1, new QTableWidgetItem(it.ts.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        m_alarmTable->setItem(r, 2, new QTableWidgetItem(it.message));

        QString lvl;
        QColor color;
        switch (it.level)
        {
        case 2:
            lvl = QStringLiteral("严重");
            color = QColor(0xC0, 0x28, 0x28);
            break;
        case 1:
            lvl = QStringLiteral("警告");
            color = QColor(0xC8, 0x7A, 0x00);
            break;
        default:
            lvl = QStringLiteral("信息");
            color = QColor(0x40, 0x40, 0x40);
            break;
        }
        auto *lvlItem = new QTableWidgetItem(lvl);
        lvlItem->setForeground(color);
        m_alarmTable->setItem(r, 3, lvlItem);

        auto *ackItem = new QTableWidgetItem(it.acked ? QStringLiteral("已确认")
                                                      : QStringLiteral("未确认"));
        if (!it.acked)
            ackItem->setForeground(QColor(0xC0, 0x28, 0x28));
        m_alarmTable->setItem(r, 4, ackItem);
    }
}
