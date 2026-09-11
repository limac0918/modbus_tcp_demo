#ifndef MAINWINDOW_H
#define MAINWINDOW_H

// =====================================================================
// MainWindow —— 纯测试用 UI（可选）
// ---------------------------------------------------------------------
// 只做一件事：把 ModbusManager / AlarmLogger 的信号接到控件上 + 提供
// 模拟故障/报警按钮。它不包含任何 Modbus 或 SQL 逻辑 ——
// 通信层与存储层完全解耦：删掉这个文件，两个核心类依然可独立工作。
// =====================================================================

#include <QWidget>
#include <QTimer>

class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QTableWidget;
class ModbusManager;
class AlarmLogger;

class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(ModbusManager *mgr, AlarmLogger *alarm, QWidget *parent = nullptr);

private slots:
    // ---- Modbus 控制 ----
    void onConnectClicked();
    void onDisconnectClicked();
    void onDropClicked();
    void onBadPortClicked();
    void onRestoreClicked();
    void onStressClicked();      // 压测：一次性塞入 N 个请求，验证队列限流
    void onTimerTick();          // 周期更新：状态 + 内存 + 队列长度

    // ---- 报警日志（第3周）----
    void onGenAlarmClicked();    // 模拟 1 条报警
    void onGenBatchClicked();    // 模拟 50 条报警（一个事务批量写）
    void onAckClicked();         // 确认选中的报警
    void onAckAllClicked();      // 全部确认
    void onClearAlarmsClicked(); // 清空全部报警（演示用）
    void onRollDemoClicked();    // 塞 6000 条 → 触发日志滚动（保留 maxRows 条）
    void refreshAlarms();        // 刷新报警表格

private:
    void appendLog(const QString &msg);
    quint64 selfRssKb();

    ModbusManager *m_mgr = nullptr;
    AlarmLogger   *m_alarm = nullptr;

    // ---- Modbus 区 ----
    QLineEdit      *m_hostEdit = nullptr;
    QLineEdit      *m_portEdit = nullptr;
    QPushButton    *m_connectBtn = nullptr;
    QPushButton    *m_disconnectBtn = nullptr;
    QPushButton    *m_dropBtn = nullptr;
    QPushButton    *m_badPortBtn = nullptr;
    QPushButton    *m_restoreBtn = nullptr;
    QPushButton    *m_stressBtn = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QLabel         *m_stateLabel = nullptr;
    QLabel         *m_metaLabel = nullptr;

    // ---- 报警区（第3周）----
    QTableWidget   *m_alarmTable = nullptr;
    QLabel         *m_alarmInfoLabel = nullptr;

    QTimer m_tickTimer;
    quint64 m_busyReqCount = 0;
    quint64 m_okCount = 0;
    quint64 m_failCount = 0;
};

#endif // MAINWINDOW_H
