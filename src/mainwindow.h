#ifndef MAINWINDOW_H
#define MAINWINDOW_H

// =====================================================================
// MainWindow —— 纯测试用 UI（可选）
// ---------------------------------------------------------------------
// 只做一件事：把 ModbusManager 的信号接到控件上 + 提供模拟故障按钮。
// 它不包含任何 Modbus 逻辑 —— 通信层与 UI 完全解耦：
//   删掉这个文件，ModbusManager 依然可以在无界面(控制台)工程里独立工作。
// =====================================================================

#include <QWidget>
#include <QTimer>

class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class ModbusManager;

class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(ModbusManager *mgr, QWidget *parent = nullptr);

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onDropClicked();
    void onBadPortClicked();
    void onRestoreClicked();
    void onStressClicked();      // 压测：一次性塞入 N 个请求，验证队列限流
    void onTimerTick();          // 周期更新：状态 + 内存 + 队列长度

private:
    void appendLog(const QString &msg);
    quint64 selfRssKb();

    ModbusManager *m_mgr = nullptr;

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

    QTimer m_tickTimer;
    quint64 m_busyReqCount = 0;
    quint64 m_okCount = 0;
    quint64 m_failCount = 0;
};

#endif // MAINWINDOW_H
