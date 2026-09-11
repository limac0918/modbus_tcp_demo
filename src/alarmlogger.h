#ifndef ALARMLOGGER_H
#define ALARMLOGGER_H

// =====================================================================
// AlarmLogger —— 报警日志模块（第3周：SQLite 嵌入式可靠存储）
// ---------------------------------------------------------------------
// 职责（与 UI 完全解耦，只通过信号对外抛事件）：
//   1. SQLite 基础：Qt QSql 模块（QSqlDatabase / QSqlQuery）
//   2. 工业场景重点：
//      - WAL 模式（断电安全 + 读不阻塞写）
//      - 事务批量写入（减少 commit 次数 → 降低 Flash 磨损）
//      - synchronous=FULL（每次提交落盘，断电不丢）
//   3. 报警表设计：ID / 时间 / 内容 / 等级 / 确认状态
//   4. 日志滚动：超过 maxRows 自动删除最旧，防数据库无限膨胀
//   5. 断电模拟：外部 kill -9 杀进程后重启，数据仍在（WAL 保证）
//
// 使用方式（main.cpp）：
//   AlarmLogger alarm;
//   alarm.init("alarms.db", 5000);
// =====================================================================

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QVector>

class QSqlDatabase;

// ---- 报警条目（纯数据，不依赖 Qt SQL）----
struct AlarmItem
{
    qint64 id = 0;      // 数据库自增 ID（写入时由 DB 分配）
    QDateTime ts;       // 报警发生时间
    QString message;    // 报警内容
    int level = 0;      // 等级：0=信息 1=警告 2=严重
    bool acked = false; // 确认状态（操作员是否已确认）
};

class AlarmLogger : public QObject
{
    Q_OBJECT
public:
    explicit AlarmLogger(QObject *parent = nullptr);
    ~AlarmLogger() override;

    // 打开/创建数据库；maxRows 为日志滚动阈值（超过删最旧）
    bool init(const QString &dbPath, int maxRows = 5000);

    bool isOpen() const { return m_open; }
    QString dbPath() const { return m_dbPath; }
    int maxRows() const { return m_maxRows; }
    void setMaxRows(int n); // 动态调整滚动阈值（下次写入时生效）

    // ---- 写入 ----
    bool writeOne(const AlarmItem &item);            // 单条（内部走事务）
    int writeBatch(const QVector<AlarmItem> &items); // 批量（一个事务提交，返回成功条数）

    // ---- 查询 ----
    QVector<AlarmItem> latestAlarms(int limit = 200) const; // 最新 N 条（时间倒序）
    int count() const;                                      // 当前总条数
    qint64 dbSizeBytes() const;                             // 数据库主文件大小（字节）

    // ---- 确认 / 清理 ----
    bool ackAlarm(qint64 id);                 // 确认单条
    bool ackAlarms(const QList<qint64> &ids); // 批量确认
    int ackAll();                             // 全部确认，返回受影响条数
    bool clearAll();                          // 清空（演示用）

signals:
    void logMessage(const QString &msg); // 与 ModbusManager 风格一致的日志通道
    void alarmsChanged();                // 数据变化（写入/删除/确认后发出，UI 刷新用）

private:
    bool ensureSchema(); // 建表（幂等）
    void rollIfNeeded(); // 日志滚动：超过 maxRows 删最旧
    void closeDb();      // 释放句柄 + removeDatabase

    QSqlDatabase *m_db = nullptr; // 指针形式，便于析构时精确控制释放顺序
    QString m_dbPath;
    int m_maxRows = 5000;
    bool m_open = false;
};

#endif // ALARMLOGGER_H
