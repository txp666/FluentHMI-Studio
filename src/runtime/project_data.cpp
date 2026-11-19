#include "runtime/project_data.h"
#include "core/binding.h"
#include "core/expression.h"
#include "core/project_logic.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

#include <algorithm>
#include <utility>

namespace fhmi {
namespace {
constexpr int kHistoryRows = 2000;

void appendRecent(QJsonArray &records, const QJsonObject &record)
{
    records.append(record);
    if (records.size() > kHistoryRows)
        records.removeFirst();
}

QJsonArray mergeHistory(const QJsonArray &stored, const QJsonArray &recent = {})
{
    struct Entry
    {
        QJsonValue value;
        QDateTime time;
    };
    QList<Entry> entries;
    QSet<QString> ids;
    entries.reserve(stored.size() + recent.size());
    for (const auto *records : {&stored, &recent}) {
        for (const auto &record : *records) {
            const auto fields = record.toObject();
            const QString id = fields.value(QStringLiteral("entryId")).toString();
            if (!id.isEmpty()) {
                if (ids.contains(id))
                    continue;
                ids.insert(id);
            }
            entries.append({record, QDateTime::fromString(fields.value(QStringLiteral("time")).toString(),
                                                          Qt::ISODateWithMs)});
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.time < b.time; });
    QJsonArray merged;
    for (qsizetype i = std::max<qsizetype>(0, entries.size() - kHistoryRows); i < entries.size(); ++i)
        merged.append(entries.at(i).value);
    return merged;
}

QJsonArray readHistory(const QString &path, QString &error)
{
    QFile file(path);
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return {};
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (file.error() != QFileDevice::NoError)
        error = file.errorString();
    else if (!document.isArray())
        error = QCoreApplication::translate("fhmi", "报警历史文件格式无效");
    return document.array();
}
}

// All running projects in this process share the writer. Ordinary stop and project switches only
// queue work, so an in-flight batch survives the ProjectData that produced it.
class AlarmHistoryWriter : public QObject
{
    Q_OBJECT

  public:
    AlarmHistoryWriter() : m_retry(new QTimer(this))
    {
        m_retry->setSingleShot(true);
        connect(m_retry, &QTimer::timeout, this, [this] { flush(); });
    }

    void load(const QString &path)
    {
        QString error;
        const auto records = readHistory(path, error);
        if (error.isEmpty()) {
            emit ready(path, mergeHistory(records, m_pending.value(path)));
        } else {
            emit failed(path, tr("报警历史读取失败：%1").arg(error));
        }
    }

    void append(const QString &path, const QJsonObject &record)
    {
        appendRecent(m_pending[path], record);
        if (!m_retry->isActive())
            m_retry->start(250);
    }

    void flush(int shutdownTimeout = 0)
    {
        m_retry->stop();
        QDeadlineTimer deadline(shutdownTimeout);
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            const QString path = it.key();
            QString error;
            QJsonArray merged;
            if (save(path, it.value(), int(deadline.remainingTime()), merged, error)) {
                it = m_pending.erase(it);
                m_reportedErrors.remove(path);
                emit ready(path, merged);
            } else {
                if (!error.isEmpty() && m_reportedErrors.value(path) != error) {
                    m_reportedErrors.insert(path, error);
                    emit failed(path, tr("报警历史保存失败：%1").arg(error));
                }
                ++it;
            }
        }
        if (!m_pending.isEmpty()) {
            if (shutdownTimeout == 0)
                m_retry->start(500);
            else
                qWarning("Could not finish saving all alarm history before shutdown");
        }
    }

  signals:
    void ready(const QString &path, const QJsonArray &records);
    void failed(const QString &path, const QString &message);

  private:
    static bool save(const QString &path, const QJsonArray &records, int waitMs, QJsonArray &merged, QString &error)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QLockFile lock(path + QStringLiteral(".lock"));
        if (!lock.tryLock(waitMs)) {
            if (lock.error() != QLockFile::LockFailedError)
                error = tr("无法锁定报警历史文件：%1").arg(path);
            return false;
        }
        const auto stored = readHistory(path, error);
        if (!error.isEmpty())
            return false;
        merged = mergeHistory(stored, records);
        const auto bytes = QJsonDocument(merged).toJson(QJsonDocument::Compact);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            error = file.errorString();
            return false;
        }
        return true;
    }

    QTimer *m_retry;
    QHash<QString, QJsonArray> m_pending;
    QHash<QString, QString> m_reportedErrors;
};

namespace {
class AlarmHistoryService : public QObject
{
  public:
    AlarmHistoryService() : QObject(QCoreApplication::instance()), writer(new AlarmHistoryWriter)
    {
        writer->moveToThread(&m_thread);
        connect(&m_thread, &QThread::finished, writer, &QObject::deleteLater);
        m_thread.start();
    }

    ~AlarmHistoryService() override
    {
        // Only final process shutdown allows the worker to wait briefly for a competing writer.
        QMetaObject::invokeMethod(writer, [writer = writer] { writer->flush(2000); }, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }

    AlarmHistoryWriter *const writer;

  private:
    QThread m_thread;
};

AlarmHistoryWriter *historyWriter()
{
    static QPointer<AlarmHistoryService> service;
    if (!service)
        service = new AlarmHistoryService;
    return service->writer;
}
} // namespace

QString dataQualityText(DataQuality quality)
{
    const auto tr = [](const char *text) { return QCoreApplication::translate("fhmi", text); };
    switch (quality) {
    case DataQuality::Waiting: return tr("等待数据");
    case DataQuality::Current: return tr("有效");
    case DataQuality::Old: return tr("旧值");
    case DataQuality::Invalid: return tr("数据无效");
    case DataQuality::ConversionFailed: return tr("换算失败");
    case DataQuality::Ambiguous: return tr("来源不唯一");
    case DataQuality::ConfigurationError: return tr("来源配置错误");
    }
    return {};
}

ProjectData::ProjectData(QObject *parent) : QObject(parent)
{
    m_expiry.setInterval(100);
    connect(&m_expiry, &QTimer::timeout, this, &ProjectData::expireSamples);
}

ProjectData::Condition ProjectData::condition(const QString &expression)
{
    const auto refs = Expression::references(expression);
    return {expression, QSet<QString>(refs.cbegin(), refs.cend())};
}

void ProjectData::configure(const QJsonObject &logic, const QString &historyFile)
{
    flushHistory();
    m_historyFile = historyFile;
    m_derived.clear();
    m_alarmConditions.clear();
    m_values = {};
    m_points.clear();
    m_expiry.stop();
    m_states.clear();
    m_history = {};
    m_recent = {};
    m_changed.clear();
    QHash<QString, QString> formulas;
    for (const auto &p : logic.value(QStringLiteral("points")).toArray()) {
        const auto point = p.toObject();
        const QString name = point.value(QStringLiteral("name")).toString();
        Point &state = m_points[name];
        state.staleAfterMs = point.value(QStringLiteral("staleAfterMs")).toInt();
        if (isProjectParameter(point)) {
            state.initial = point.value(QStringLiteral("initialValue")).toVariant();
            state.sample = {state.initial, {}, QDateTime::currentDateTimeUtc(), DataQuality::Current, {}};
        } else if (state.staleAfterMs > 0 && !m_expiry.isActive()) {
            m_expiry.start();
        }
        m_values.insert(name, state.sample.value);
        m_changed.insert(name);
        const QString formula = point.value(QStringLiteral("expression")).toString().trimmed();
        if (!formula.isEmpty())
            formulas.insert(name, formula);
    }
    for (const auto &name : dataPointOrder(logic))
        if (const auto formula = formulas.constFind(name); formula != formulas.cend())
            m_derived.append({name, condition(formula.value())});
    for (const auto &a : logic.value(QStringLiteral("alarms")).toArray()) {
        const auto alarm = a.toObject();
        AlarmState state;
        state.id = alarm.value(QStringLiteral("id")).toString();
        state.text = alarm.value(QStringLiteral("text")).toString();
        if (state.text.isEmpty())
            state.text = state.id;
        state.level = alarm.value(QStringLiteral("level")).toInt(2);
        state.ackRequired = alarm.value(QStringLiteral("ackRequired")).toBool(true);
        m_states.append(state);
        m_alarmConditions.append(condition(alarm.value(QStringLiteral("condition")).toString()));
    }
    if (!historyFile.isEmpty()) {
        auto *writer = historyWriter();
        connect(writer, &AlarmHistoryWriter::ready, this, &ProjectData::updateHistory, Qt::UniqueConnection);
        connect(writer, &AlarmHistoryWriter::failed, this, &ProjectData::reportHistoryError, Qt::UniqueConnection);
        QMetaObject::invokeMethod(writer, [writer, historyFile] { writer->load(historyFile); }, Qt::QueuedConnection);
    }
    m_initialEvaluation = true;
}
DataPointSample ProjectData::sample(const QString &name) const
{
    const auto point = m_points.constFind(name);
    return point == m_points.cend() ? DataPointSample{} : point->sample;
}

QList<quint64> ProjectData::sampleStreams(const QString &name) const
{
    const auto point = m_points.constFind(name);
    return point == m_points.cend() ? QList<quint64>{} : point->sources.keys();
}

void ProjectData::publish(const QString &name, Point &point, DataPointSample sample)
{
    m_values.insert(name, sample.quality == DataQuality::Current ? sample.value : QVariant());
    if (!sample.value.isValid())
        sample.value = point.sample.value;
    if (!sample.timestamp.isValid())
        sample.timestamp = point.sample.timestamp;
    point.sample = std::move(sample);
    m_changed.insert(name);
}

void ProjectData::selectSample(const QString &name, Point &point)
{
    if (point.sources.size() > 1) {
        auto sample = point.sample;
        sample.quality = DataQuality::Ambiguous;
        QStringList sources;
        for (const auto &entry : std::as_const(point.sources)) {
            const auto &source = entry.source;
            const auto label = source.endpoint.isEmpty() ? source.connectionId
                : source.connectionId + QStringLiteral(" / ") + source.endpoint;
            sources.append(label);
        }
        sources.sort();
        sample.detail = tr("收到多个来源的数据 · %1").arg(sources.join(QStringLiteral(", ")));
        publish(name, point, std::move(sample));
    } else if (point.sources.size() == 1) {
        publish(name, point, point.sources.constBegin().value());
    } else {
        setProblem(name, DataQuality::Old);
    }
}

void ProjectData::setSample(const QString &name, const QVariant &value, const DataSource &source, DataQuality quality,
                            const QDateTime &timestamp, const QString &detail)
{
    const auto point = m_points.find(name);
    if (point == m_points.end() || point->initial.isValid())
        return;
    point->sources.insert(source.stream,
                          {value, source, timestamp.isValid() ? timestamp : QDateTime::currentDateTimeUtc(), quality, detail});
    selectSample(name, *point);
}

void ProjectData::setProblem(const QString &name, DataQuality quality, const QString &detail)
{
    const auto point = m_points.find(name);
    if (point == m_points.end())
        return;
    auto sample = point->sample;
    sample.quality = quality;
    sample.detail = detail;
    publish(name, *point, std::move(sample));
}

bool ProjectData::setParameter(const QString &name, const QString &text, bool invalidateOnError)
{
    const auto point = m_points.find(name);
    if (point == m_points.end() || !point->initial.isValid())
        return false;
    QVariant value = text;
    const int type = point->initial.typeId();
    if (type == QMetaType::Bool) {
        const auto parsed = bindingBoolean(text);
        value = parsed ? QVariant(*parsed) : QVariant();
    } else if (type != QMetaType::QString) {
        const auto parsed = bindingFloat(text);
        value = parsed ? QVariant(*parsed) : QVariant();
    }
    if (!value.isValid()) {
        if (invalidateOnError) {
            setProblem(name, DataQuality::Invalid, tr("工程参数的值与类型不匹配"));
            evaluate();
        }
        return false;
    }
    if (point->sample.quality == DataQuality::Current && point->sample.value == value)
        return true;
    publish(name, *point, {value, {}, QDateTime::currentDateTimeUtc(), DataQuality::Current, {}});
    evaluate();
    return true;
}

void ProjectData::markOld(const QString &name, quint64 stream)
{
    const auto point = m_points.find(name);
    if (point == m_points.end())
        return;
    const auto sample = point->sources.find(stream);
    if (sample != point->sources.end()) {
        sample->quality = DataQuality::Old;
        selectSample(name, *point);
    }
}

void ProjectData::expireSamples()
{
    const auto now = QDateTime::currentDateTimeUtc();
    for (auto point = m_points.begin(); point != m_points.end(); ++point) {
        if (point->staleAfterMs <= 0)
            continue;
        bool changed = false;
        for (auto &sample : point->sources) {
            if (sample.quality == DataQuality::Current && sample.timestamp.msecsTo(now) >= point->staleAfterMs) {
                sample.quality = DataQuality::Old;
                changed = true;
            }
        }
        if (changed)
            selectSample(point.key(), *point);
    }
    evaluate();
}

void ProjectData::invalidateStream(quint64 stream)
{
    for (auto point = m_points.begin(); point != m_points.end(); ++point)
        if (point->sources.remove(stream))
            selectSample(point.key(), *point);
    evaluate();
}
void ProjectData::clearValues()
{
    for (auto point = m_points.begin(); point != m_points.end(); ++point) {
        point->sources.clear();
        if (!point->initial.isValid() && point->sample.quality != DataQuality::ConfigurationError)
            setProblem(point.key(), DataQuality::Waiting);
    }
    evaluate();
}
QVariant ProjectData::calculate(const QString &expression) const
{
    // Unknown/disconnected inputs must not silently become a healthy/zero state.
    const auto resolve = [this](const QString &name) -> QVariant {
        const auto value = m_values.value(name);
        const auto number = bindingFloat(value);
        if (!number)
            return {};
        if (value.typeId() == QMetaType::QString) {
            bool integer = false;
            const auto n = value.toString().toLongLong(&integer);
            return integer ? QVariant(n) : QVariant(*number);
        }
        return value;
    };
    return Expression::evaluate(expression, {}, resolve);
}
void ProjectData::evaluate()
{
    const bool initial = std::exchange(m_initialEvaluation, false);
    if (!initial && m_changed.isEmpty())
        return;
    auto touched = std::exchange(m_changed, {});
    for (const auto &derived : m_derived) {
        if (!initial && !touched.contains(derived.name) && !derived.formula.inputs.intersects(touched))
            continue;
        const auto value = calculate(derived.formula.expression);
        const auto validity = value.isValid() ? DataQuality::Current : DataQuality::Invalid;
        publish(derived.name, m_points[derived.name], {value, {}, QDateTime::currentDateTimeUtc(), validity, {}});
        m_changed.remove(derived.name);
        touched.insert(derived.name);
    }
    Message changes;
    for (const auto &name : touched)
        changes.insert(name, m_values.value(name));
    emit valuesChanged(changes);
    bool changed = false;
    for (qsizetype i = 0; i < m_states.size(); ++i) {
        const auto &condition = m_alarmConditions.at(i);
        if (!initial && !condition.inputs.intersects(touched))
            continue;
        auto &state = m_states[i];
        const auto value = bindingBoolean(calculate(condition.expression));
        changed |= state.known != value.has_value();
        state.known = value.has_value();
        if (!value || *value == state.active)
            continue;
        state.active = *value;
        changed = true;
        const auto now = QDateTime::currentDateTimeUtc();
        if (*value) {
            state.acknowledged = !state.ackRequired;
            state.silenced = false;
            state.raisedAt = now;
            state.clearedAt = {};
            record(state, QStringLiteral("raised"), now);
            emit alarmRaised(state);
        } else {
            state.clearedAt = now;
            record(state, QStringLiteral("cleared"), now);
        }
    }
    if (changed)
        emit alarmsChanged();
}
bool ProjectData::audible() const
{
    for (const auto &state : m_states)
        if (state.active && !state.silenced && (!state.ackRequired || !state.acknowledged))
            return true;
    return false;
}
void ProjectData::acknowledge(const QString &id)
{
    bool changed = false;
    for (auto &state : m_states)
        if ((id.isEmpty() || state.id == id) && !state.acknowledged) {
            state.acknowledged = true;
            changed = true;
            record(state, QStringLiteral("acknowledged"), QDateTime::currentDateTimeUtc());
        }
    if (changed)
        emit alarmsChanged();
}
void ProjectData::silence()
{
    bool changed = false;
    for (auto &state : m_states)
        if (state.outstanding() && !state.silenced) {
            state.silenced = true;
            changed = true;
            record(state, QStringLiteral("silenced"), QDateTime::currentDateTimeUtc());
        }
    if (changed)
        emit alarmsChanged();
}
void ProjectData::record(const AlarmState &state, const QString &event, const QDateTime &time)
{
    const QJsonObject entry{{QStringLiteral("entryId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                            {QStringLiteral("time"), time.toString(Qt::ISODateWithMs)},
                            {QStringLiteral("id"), state.id},
                            {QStringLiteral("text"), state.text},
                            {QStringLiteral("level"), state.level},
                            {QStringLiteral("event"), event}};
    appendRecent(m_history, entry);
    if (m_historyFile.isEmpty())
        return;
    appendRecent(m_recent, entry);
    auto *writer = historyWriter();
    QMetaObject::invokeMethod(writer, [writer, path = m_historyFile, entry] { writer->append(path, entry); },
                              Qt::QueuedConnection);
}
void ProjectData::flushHistory()
{
    if (m_historyFile.isEmpty())
        return;
    auto *writer = historyWriter();
    QMetaObject::invokeMethod(writer, [writer] { writer->flush(); }, Qt::QueuedConnection);
}
void ProjectData::updateHistory(const QString &file, const QJsonArray &records)
{
    if (file != m_historyFile)
        return;
    const auto merged = mergeHistory(records, m_recent);
    if (merged != m_history) {
        m_history = merged;
        emit historyChanged();
    }
}
void ProjectData::reportHistoryError(const QString &file, const QString &message)
{
    if (file == m_historyFile)
        emit storageError(message);
}
} // namespace fhmi

#include "project_data.moc"
