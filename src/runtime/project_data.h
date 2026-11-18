#pragma once
#include "core/message.h"
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QSet>

namespace fhmi {
struct AlarmState
{
    QString id, text;
    int level = 2;
    bool active = false, known = false, acknowledged = true, silenced = false, ackRequired = true;
    QDateTime raisedAt, clearedAt;
    bool outstanding() const { return active || !acknowledged; }
};
// One per project runtime: protocol-independent values, derived values and alarm transitions.
class ProjectData : public QObject
{
    Q_OBJECT
  public:
    explicit ProjectData(QObject *parent = nullptr);
    void configure(const QJsonObject &logic, const QString &historyFile = {});
    void setValue(const QString &name, const QVariant &value, quint64 stream = 0);
    void invalidateStream(quint64 stream);
    void clearValues();
    void evaluate(); // commit one incoming message/frame atomically before evaluating dependencies
    Message values() const { return m_values; }
    quint64 valueStream(const QString &name) const { return m_streams.value(name); }
    QList<AlarmState> alarms() const { return m_states; }
    QJsonArray history() const { return m_history; }
    bool audible() const;
    void acknowledge(const QString &id = {});
    void silence();
    void flushHistory();
  signals:
    void valuesChanged(const fhmi::Message &values);
    void alarmsChanged();
    void historyChanged();
    void alarmRaised(const fhmi::AlarmState &alarm);
    void storageError(const QString &message);

  private:
    struct Condition
    {
        QString expression;
        QSet<QString> inputs;
    };
    struct DerivedPoint
    {
        QString name;
        Condition formula;
    };
    static Condition condition(const QString &expression);
    void record(const AlarmState &state, const QString &event, const QDateTime &time);
    QVariant calculate(const QString &expression) const;
    void updateHistory(const QString &file, const QJsonArray &records);
    void reportHistoryError(const QString &file, const QString &message);
    QList<DerivedPoint> m_derived;
    QList<Condition> m_alarmConditions;
    Message m_values;
    QSet<QString> m_changed;
    QHash<QString, quint64> m_streams;
    QList<AlarmState> m_states;
    QJsonArray m_history;
    QJsonArray m_recent;
    QString m_historyFile;
    bool m_initialEvaluation = false;
};
} // namespace fhmi
Q_DECLARE_METATYPE(fhmi::AlarmState)
