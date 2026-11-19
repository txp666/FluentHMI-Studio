#pragma once

#include "comm/comm_hub.h"
#include "comm/s7/s7_client.h"
#include "core/binding.h"
#include "core/command_encoder.h"
#include "core/project.h"
#include "runtime/project_data.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>

#include <functional>
#include <vector>

namespace fhmi {

class AppDialog;
class HmiWidget;
class ModbusPoller;
class PeriodicSender;

namespace NotifyType {
inline const QString Success = QStringLiteral("成功"); // i18n: value
inline const QString Info = QStringLiteral("信息");    // i18n: value
inline const QString Warning = QStringLiteral("警告"); // i18n: value
inline const QString Error = QStringLiteral("错误");   // i18n: value
} // namespace NotifyType

// The live side of a running surface: the hub with its links, the bindings from
// incoming data to widgets, the path from widgets' commands out to devices, and the
// poller that reads what the Modbus bindings show.
class RuntimeContext : public QObject
{
    Q_OBJECT

  public:
    explicit RuntimeContext(QObject *parent = nullptr);

    CommHub *hub() const { return m_hub; }
    S7Client *s7Client() const { return m_s7; }
    PeriodicSender *periodicSender() const { return m_periodic; }
    void setS7CurveNeeds(const QObject *chart, const QList<S7::Need> &needs);
    // Reads the Modbus bindings and curves while the context is active; every running project has one, set
    // up by its link cards.
    ModbusPoller *modbusPoller() const { return m_poller; }

    void registerWidget(const QString &id, HmiWidget *widget);
    QString widgetPreferencesGroup(const HmiWidget &widget) const;
    std::optional<CommandEncoder::InputValue> inputValue(const QString &id) const;
    void addBinding(BindingEntry binding);
    void clear();
    void configureData(const Project &project);
    ProjectData *projectData() const { return m_data; }
    const QJsonObject &frameTemplates() const { return m_frameTemplates; }
    // Resolves an automatic binding against the configured topology. Explicit scopes stay as chosen.
    bool resolveBindingSource(BindingEntry &binding) const;

    void setActive(bool active);
    void setBitProfiles(const QJsonObject &profiles) { m_bitProfiles = profiles; emit bitProfilesChanged(); }
    QJsonObject bitProfiles() const { return m_bitProfiles; }
    bool isActive() const { return m_active; }

    // Sends a widget's command to the selected connections. Modbus writes can also address a local slave table.
    // `sent` collects the frames the links accepted, for a status line.
    bool send(const CommandResult &result, QList<QByteArray> *sent = nullptr);
    // User actions enter here. Confirmation holds the command until the operator accepts.
    void requestSend(HmiWidget *sender, const CommandResult &result, bool confirm, bool showDetails,
                     const QString &mode, std::function<void()> onSent, int intervalMs, CommandReader readInputs);
    std::optional<DataRoute> resolveCommandRoute(const EncodedCommand &command, QString *error) const;
    QString commandTargetText(EncodedCommand::Kind kind, const DataRoute &route) const;

    // A control without an explicitly routed command can write through its Modbus binding to a local slave.
    void writeModbusSlaves(const HmiWidget *widget, double value, bool sendsCommand);

    // What the Modbus poller reads: every Modbus binding, visible while its widget is on screen (not on a
    // tab page in the background), then the curves charts registered.
    QList<ModbusNeed> modbusNeeds() const;
    void modbusReadFailed(const Link &link, const QString &endpoint, const ModbusBlock &block, int fallbackSlave);
    // A chart's Modbus curves; an empty list removes them.
    void setCurveNeeds(const QObject *chart, const QList<ModbusNeed> &needs);

    // Milliseconds on a steady clock, which polling schedules by; tests replace it to step time by hand.
    qint64 now() const { return m_clock ? m_clock() : m_steady.elapsed(); }
    void setClock(std::function<qint64()> clock) { m_clock = std::move(clock); }
    void notify(const QString &title, const QString &content, const QString &type = NotifyType::Info,
                int durationMs = 2000);

  signals:
    void activeChanged(bool active);
    void s7Sample(const fhmi::S7::Sample &sample);
    void bitProfilesChanged();
    void notificationRequested(const QString &title, const QString &content, const QString &type, int durationMs);

  private:
    struct CommandTargets
    {
        QList<SendTarget> links;
        QList<QPointer<ModbusSlave>> slaves;
        quint64 s7Session = 0;

        qsizetype size() const { return links.size() + slaves.size() + (s7Session ? 1 : 0); }
        bool operator==(const CommandTargets &other) const;
    };
    CommandTargets commandTargets(const EncodedCommand &command) const;
    QStringList commandTargetLabels(const EncodedCommand &command, const CommandTargets &targets) const;
    bool sendResolved(const EncodedCommand &command, const CommandTargets &targets, QList<QByteArray> *sent = nullptr);
    void onUnit(const QByteArray &bytes, const Message &message, const DataSource &source);
    void applyMessage(const Message &message, const DataSource &source);
    void applyRaw(const QByteArray &bytes, const DataSource &source, bool evaluateSamples);
    void onCanFrame(const CanFrame &frame, const Message &message, const DataSource &source);
    void push(const BindingEntry &binding, const QVariant &raw, const DataSource &source = {});

    ProjectData *m_data = nullptr;
    QTimer *m_alarmSound = nullptr;
    CommHub *m_hub = nullptr;
    PeriodicSender *m_periodic = nullptr;
    S7Client *m_s7 = nullptr;
    QTimer *m_s7Timer = nullptr;
    QHash<const QObject *, QList<S7::Need>> m_s7Curves;
    ModbusPoller *m_poller = nullptr;
    QPointer<AppDialog> m_sendConfirmation;
    QHash<QString, QPointer<HmiWidget>> m_widgets;
    QString m_projectPreferencesGroup;
    QJsonArray m_connectionCards;
    QString m_projectParser = QStringLiteral("line");
    QHash<QString, quint64> m_widgetStreams;
    std::vector<BindingEntry> m_bindings;
    QHash<quint64, QList<size_t>> m_canBindings; // CanId::key() -> the CAN信号 bindings' indices in m_bindings
    QList<size_t> m_parsedProjectBindings;
    QList<QPair<const QObject *, QList<ModbusNeed>>> m_curveNeeds; // in the order charts registered
    bool m_active = false;
    QJsonObject m_bitProfiles;
    QJsonObject m_frameTemplates;
    QElapsedTimer m_steady;
    std::function<qint64()> m_clock;
};

} // namespace fhmi
