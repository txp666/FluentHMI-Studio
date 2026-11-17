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

class HmiWidget;
class ModbusPoller;

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
    void setS7CurveNeeds(const QObject *chart, const QList<S7::Need> &needs);
    // Reads the Modbus bindings and curves while the context is active; every running project has one, set
    // up by its link cards.
    ModbusPoller *modbusPoller() const { return m_poller; }

    void registerWidget(const QString &id, HmiWidget *widget);
    void addBinding(BindingEntry binding);
    void clear();
    void configureData(const Project &project);
    ProjectData *projectData() const { return m_data; }

    void setActive(bool active);
    void setBitProfiles(const QJsonObject &profiles) { m_bitProfiles = profiles; emit bitProfilesChanged(); }
    QJsonObject bitProfiles() const { return m_bitProfiles; }
    bool isActive() const { return m_active; }

    // Sends a widget's command to the selected connections. Modbus writes can also address a local slave table.
    // `sent` collects the frames the links accepted, for a status line.
    bool send(const CommandResult &result, QList<QByteArray> *sent = nullptr);

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
    void onMessage(const Message &message, const DataSource &source);
    void onRaw(const QByteArray &bytes, const DataSource &source);
    void onCanFrame(const CanFrame &frame, const Message &message, const DataSource &source);
    void push(const BindingEntry &binding, const QVariant &raw, const DataSource &source = {});

    ProjectData *m_data = nullptr;
    QTimer *m_alarmSound = nullptr;
    CommHub *m_hub = nullptr;
    S7Client *m_s7 = nullptr;
    QTimer *m_s7Timer = nullptr;
    QHash<const QObject *, QList<S7::Need>> m_s7Curves;
    ModbusPoller *m_poller = nullptr;
    QHash<QString, QPointer<HmiWidget>> m_widgets;
    std::vector<BindingEntry> m_bindings;
    QHash<quint64, QList<size_t>> m_canBindings; // CanId::key() -> the CAN信号 bindings' indices in m_bindings
    QList<size_t> m_parsedProjectBindings;
    QList<QPair<const QObject *, QList<ModbusNeed>>> m_curveNeeds; // in the order charts registered
    bool m_active = false;
    QJsonObject m_bitProfiles;
    QElapsedTimer m_steady;
    std::function<qint64()> m_clock;
};

} // namespace fhmi
