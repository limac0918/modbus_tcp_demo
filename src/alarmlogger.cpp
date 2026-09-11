#include "alarmlogger.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QFileInfo>

// =====================================================================
// AlarmLogger 实现 —— 第3周：SQLite 嵌入式可靠存储
// =====================================================================

namespace
{
    const QLatin1String kConnName("alarm_conn");
}

AlarmLogger::AlarmLogger(QObject *parent)
    : QObject(parent)
{
}

AlarmLogger::~AlarmLogger()
{
    closeDb();
}

// ---------------------------------------------------------------------
// 初始化：打开 DB + WAL + 建表
// ---------------------------------------------------------------------
bool AlarmLogger::init(const QString &dbPath, int maxRows)
{
    m_dbPath = dbPath;
    m_maxRows = qMax(100, maxRows);

    if (QSqlDatabase::contains(kConnName))
    {
        // 已存在同名连接：复用（避免重复 addDatabase 告警）
        m_db = new QSqlDatabase(QSqlDatabase::database(kConnName));
    }
    else
    {
        m_db = new QSqlDatabase(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kConnName));
    }
    m_db->setDatabaseName(m_dbPath);

    if (!m_db->open())
    {
        emit logMessage(QStringLiteral("[alarm] open DB failed: %1").arg(m_db->lastError().text()));
        return false;
    }

    // ---- 工业重点 ①：WAL 模式 ----
    // 作用：
    //   a) 断电安全：已提交但未 checkpoint 的事务记录在 -wal 文件中，
    //      崩溃恢复时自动重放，不丢已提交数据（journal 模式下损坏风险更高）；
    //   b) 读不阻塞写、写不阻塞读（journal 模式写时读被阻塞，HMI 界面会卡）。
    // PRAGMA journal_mode=WAL 是持久的：写一次，之后每次打开都保持 WAL。
    {
        QSqlQuery q(*m_db);
        if (!q.exec(QStringLiteral("PRAGMA journal_mode=WAL;")))
        {
            emit logMessage(QStringLiteral("[alarm] set WAL failed: %1").arg(q.lastError().text()));
        }
    }

    // ---- 工业重点 ②：同步级别 FULL ----
    // 每次 commit 都等待数据真正落盘（fsync），断电/掉电不丢已提交事务。
    // 代价是写稍慢 —— 报警场景频率低，可靠优先。
    {
        QSqlQuery q(*m_db);
        q.exec(QStringLiteral("PRAGMA synchronous=FULL;"));
    }

    if (!ensureSchema())
    {
        m_db->close();
        return false;
    }

    m_open = true;
    emit logMessage(QStringLiteral("[alarm] DB ready: %1 (WAL on, synchronous=FULL, maxRows=%2)")
                        .arg(m_dbPath)
                        .arg(m_maxRows));
    return true;
}

bool AlarmLogger::ensureSchema()
{
    // ---- 报警表设计：ID / 时间 / 内容 / 等级 / 确认状态 ----
    QSqlQuery q(*m_db);
    const bool ok = q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS alarms ("
        " id      INTEGER PRIMARY KEY AUTOINCREMENT,"
        " ts      TEXT    NOT NULL,"
        " message TEXT    NOT NULL,"
        " level   INTEGER NOT NULL DEFAULT 0,"
        " acked   INTEGER NOT NULL DEFAULT 0"
        ")"));
    if (!ok)
        emit logMessage(QStringLiteral("[alarm] create table failed: %1").arg(q.lastError().text()));
    return ok;
}

// ---------------------------------------------------------------------
// 写入：单条 / 批量（事务）
// ---------------------------------------------------------------------
bool AlarmLogger::writeOne(const AlarmItem &item)
{
    QVector<AlarmItem> v;
    v.append(item);
    return writeBatch(v) == 1;
}

int AlarmLogger::writeBatch(const QVector<AlarmItem> &items)
{
    if (items.isEmpty() || !m_open)
        return 0;

    rollIfNeeded(); // 写入前先滚动（超限删最旧）

    // ---- 工业重点 ③：事务批量写入 ----
    // 为什么批量：Flash(eMMC/NAND) 擦写次数有限，每次 commit 都是一次写入周期。
    // 50 条报警用 1 个事务 = 1 次 commit，而不是 50 次 —— 显著降低 Flash 磨损，
    // 且保证"要么全部成功、要么全部不落"（原子性）。
    if (!m_db->transaction())
    {
        emit logMessage(QStringLiteral("[alarm] begin transaction failed: %1")
                            .arg(m_db->lastError().text()));
        return 0;
    }

    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO alarms (ts, message, level, acked)"
        " VALUES (:ts, :msg, :lvl, :ack)"));

    int okCount = 0;
    for (const AlarmItem &it : items)
    {
        q.bindValue(QStringLiteral(":ts"), it.ts.toString(Qt::ISODate));
        q.bindValue(QStringLiteral(":msg"), it.message);
        q.bindValue(QStringLiteral(":lvl"), it.level);
        q.bindValue(QStringLiteral(":ack"), it.acked ? 1 : 0);
        if (q.exec())
            ++okCount;
    }

    if (!m_db->commit())
    {
        m_db->rollback(); // 提交失败 → 整批回滚（原子性）
        emit logMessage(QStringLiteral("[alarm] commit failed, batch rolled back: %1")
                            .arg(m_db->lastError().text()));
        return 0;
    }

    if (okCount > 0)
        emit alarmsChanged();
    emit logMessage(QStringLiteral("[alarm] batch committed: %1/%2 rows")
                        .arg(okCount)
                        .arg(items.size()));
    return okCount;
}

// ---------------------------------------------------------------------
// 日志滚动：超过 maxRows 删最旧，防数据库无限膨胀
// ---------------------------------------------------------------------
void AlarmLogger::rollIfNeeded()
{
    if (count() <= m_maxRows)
        return;

    // 语义：保留最新的 maxRows 条，删除其余（最旧的）。
    // LIMIT -1 = 不限制返回条数；OFFSET :keep = 跳过最新的 keep 条。
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM alarms WHERE id IN ("
        "  SELECT id FROM alarms ORDER BY id DESC LIMIT -1 OFFSET :keep"
        ")"));
    q.bindValue(QStringLiteral(":keep"), m_maxRows);
    if (!q.exec())
    {
        emit logMessage(QStringLiteral("[alarm] roll failed: %1").arg(q.lastError().text()));
        return;
    }
    const int removed = q.numRowsAffected();
    if (removed > 0)
    {
        emit alarmsChanged();
        emit logMessage(QStringLiteral("[alarm] roll: removed %1 oldest, keep %2")
                            .arg(removed)
                            .arg(m_maxRows));
    }
}

// ---------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------
QVector<AlarmItem> AlarmLogger::latestAlarms(int limit) const
{
    QVector<AlarmItem> out;
    if (!m_open)
        return out;

    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "SELECT id, ts, message, level, acked FROM alarms"
        " ORDER BY id DESC LIMIT :lim"));
    q.bindValue(QStringLiteral(":lim"), limit);
    if (!q.exec())
        return out;

    while (q.next())
    {
        AlarmItem it;
        it.id = q.value(0).toLongLong();
        it.ts = QDateTime::fromString(q.value(1).toString(), Qt::ISODate);
        it.message = q.value(2).toString();
        it.level = q.value(3).toInt();
        it.acked = (q.value(4).toInt() != 0);
        out.append(it);
    }
    return out;
}

int AlarmLogger::count() const
{
    if (!m_open)
        return 0;
    QSqlQuery q(*m_db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM alarms")))
        return 0;
    if (q.next())
        return q.value(0).toInt();
    return 0;
}

qint64 AlarmLogger::dbSizeBytes() const
{
    const QFileInfo fi(m_dbPath);
    return fi.exists() ? fi.size() : 0;
}

// ---------------------------------------------------------------------
// 确认 / 清理
// ---------------------------------------------------------------------
bool AlarmLogger::ackAlarm(qint64 id)
{
    if (!m_open)
        return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("UPDATE alarms SET acked=1 WHERE id=:id"));
    q.bindValue(QStringLiteral(":id"), id);
    if (q.exec())
    {
        emit alarmsChanged();
        return true;
    }
    emit logMessage(QStringLiteral("[alarm] ack failed: %1").arg(q.lastError().text()));
    return false;
}

bool AlarmLogger::ackAlarms(const QList<qint64> &ids)
{
    if (!m_open) // 同单条 48-49
        return false;
    if (ids.isEmpty()) // 空列表 = 无事可做（幂等）
        return true;

    if (!m_db->transaction())
    { // 开启事务失败
        emit logMessage(QStringLiteral("[alarm] ack batch: begin failed: %1")
                            .arg(m_db->lastError().text()));
        return false;
    }

    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("UPDATE alarms SET acked=1 WHERE id=:id"));
    for (qint64 id : ids)
    {
        q.bindValue(QStringLiteral(":id"), id);
        if (!q.exec())
        { // 任一条失败 → 整批回滚
            m_db->rollback();
            emit logMessage(QStringLiteral("[alarm] ack batch failed at id=%1: %2")
                                .arg(id)
                                .arg(q.lastError().text()));
            return false;
        }
    }

    if (!m_db->commit())
    { // commit 失败
        emit logMessage(QStringLiteral("[alarm] ack batch commit failed: %1")
                            .arg(m_db->lastError().text()));
        return false;
    }

    emit alarmsChanged(); // 成功 → 同单条 55
    return true;          // 同单条 56
}

int AlarmLogger::ackAll()
{
    if (!m_open)
        return 0;
    QSqlQuery q(*m_db);
    if (!q.exec(QStringLiteral("UPDATE alarms SET acked=1")))
        return 0;
    const int n = q.numRowsAffected();
    if (n > 0)
        emit alarmsChanged();
    return n;
}

bool AlarmLogger::clearAll()
{
    if (!m_open)
        return false;
    QSqlQuery q(*m_db);
    const bool ok = q.exec(QStringLiteral("DELETE FROM alarms"));
    if (ok)
        emit alarmsChanged();
    return ok;
}

// ---------------------------------------------------------------------
// 析构：释放句柄后 removeDatabase（避免 Qt 退出时的 connection-in-use 告警）
// ---------------------------------------------------------------------
void AlarmLogger::closeDb()
{
    if (m_db)
    {
        if (m_db->isOpen())
            m_db->close();
        delete m_db; // 先销毁 QSqlDatabase 对象
        m_db = nullptr;
        if (QSqlDatabase::contains(kConnName))
            QSqlDatabase::removeDatabase(kConnName); // 再移除连接（此时已无句柄）
    }
    m_open = false;
}

void AlarmLogger::setMaxRows(int n)
{
    m_maxRows = qMax(100, n);
    emit logMessage(QStringLiteral("[alarm] maxRows set to %1").arg(m_maxRows));
}
