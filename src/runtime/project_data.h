#pragma once
#include "core/data_source.h"
#include "core/message.h"
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <optional>

namespace fhmi {

enum class DataQuality { Waiting, Current, Old, Invalid, ConversionFailed, Ambiguous, ConfigurationError };
QString dataQualityText(DataQuality quality);

struct DataPointSample
{
    QVariant value; // Last good value remains available for diagnostics.
    DataSource source;
    QDateTime timestamp;
    DataQuality quality = DataQuality::Waiting;
    QString detail;
};

// Scalar consumers keep the first matching address for the run. A new session at that
// address can resume it. A different address can report a conflict but cannot take over.
class RoutedSample
{
  public:
    void update(DataPointSample sample);
    bool close(quint64 stream);
    bool markOld(quint64 stream);
    bool expire(const QDateTime &now, int ageMs);
    DataPointSample sample() const;

  private:
    std::optional<DataPointSample> m_primary;
    QHash<quint64, DataSource> m_otherSources;
    quint64 m_closedEpoch = 0;
    bool m_present = false;
};

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
    void setSample(const QString &name, const QVariant &value, const DataSource &source, DataQuality quality,
                   const QDateTime &timestamp = {}, const QString &detail = {});
    void setProblem(const QString &name, DataQuality quality, const QString &detail = {});
    bool setParameter(const QString &name, const QString &text, bool invalidateOnError = false);
    void markOld(const QString &name, quint64 stream);
    void invalidateStream(quint64 stream);
    void clearValues();
    void evaluate(); // commit one incoming message/frame atomically before evaluating dependencies
    Message values() const { return m_values; }
    DataPointSample sample(const QString &name) const;
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
    struct Point
    {
        DataPointSample sample;
        RoutedSample source;
        QVariant initial;
        int staleAfterMs = 0;
    };
    static Condition condition(const QString &expression);
    void record(const AlarmState &state, const QString &event, const QDateTime &time);
    QVariant calculate(const QString &expression) const;
    void updateHistory(const QString &file, const QJsonArray &records);
    void reportHistoryError(const QString &file, const QString &message);
    void publish(const QString &name, Point &point, DataPointSample sample);
    void expireSamples();
    QList<DerivedPoint> m_derived;
    QList<Condition> m_alarmConditions;
    Message m_values;
    QSet<QString> m_changed;
    QHash<QString, Point> m_points;
    QTimer m_expiry;
    QList<AlarmState> m_states;
    QJsonArray m_history;
    QJsonArray m_recent;
    QString m_historyFile;
    bool m_initialEvaluation = false;
};
} // namespace fhmi
Q_DECLARE_METATYPE(fhmi::AlarmState)
