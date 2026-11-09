#pragma once

#include "comm/comm_hub.h"
#include "core/binding.h"
#include "core/command_encoder.h"

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
    // Reads the Modbus bindings and curves while the context is active; every running project has one, set
    // up by its link cards.
    ModbusPoller *modbusPoller() const { return m_poller; }

    void registerWidget(const QString &id, HmiWidget *widget);
    void addBinding(BindingEntry binding);
    void clear();

    void setActive(bool active);
    bool isActive() const { return m_active; }

    // Sends a widget's command over every connected link, or explains why not.
    // `sent` collects the frames the links accepted, for a status line. A Modbus write (FC 05, 06, 16) is also
    // made in the table of every Modbus slave (CommHub::modbusSlaves()) whose area holds its addresses; one
    // that some slave took needs no link.
    bool send(const CommandResult &result, QList<QByteArray> *sent = nullptr);
    // A Modbus request queued as `how` says (CommHub::sendModbus), explained the same way when no link takes it.
    bool sendModbus(const ModbusRequest &request, QList<QByteArray> *sent, const ModbusSend &how);

    // A control's own change to `value` (a ToggleSwitch, Slider or SpinBox the user moved): written into every
    // Modbus slave's table at the address of the control's Modbus binding, unless the binding has an expression.
    void writeModbusSlaves(const HmiWidget *widget, double value);

    // What the Modbus poller reads: every Modbus binding, visible while its widget is on screen (not on a
    // tab page in the background), then the curves charts registered.
    QList<ModbusNeed> modbusNeeds() const;
    // A chart's Modbus curves; an empty list removes them.
    void setCurveNeeds(const QObject *chart, const QList<ModbusNeed> &needs);

    // Milliseconds on a steady clock, which polling schedules by; tests replace it to step time by hand.
    qint64 now() const { return m_clock ? m_clock() : m_steady.elapsed(); }
    void setClock(std::function<qint64()> clock) { m_clock = std::move(clock); }
    void notify(const QString &title, const QString &content, const QString &type = NotifyType::Info,
                int durationMs = 2000);

  signals:
    void notificationRequested(const QString &title, const QString &content, const QString &type, int durationMs);

  private:
    void onMessage(const Message &message);
    void onRaw(const QByteArray &bytes);
    void onCanFrames(const QList<CanFrame> &frames, bool sent);
    void push(const BindingEntry &binding, const QVariant &raw);

    CommHub *m_hub = nullptr;
    ModbusPoller *m_poller = nullptr;
    QHash<QString, QPointer<HmiWidget>> m_widgets;
    std::vector<BindingEntry> m_bindings;
    QHash<quint64, QList<size_t>> m_canBindings; // CanId::key() -> the CAN信号 bindings' indices in m_bindings
    QList<QPair<const QObject *, QList<ModbusNeed>>> m_curveNeeds; // in the order charts registered
    bool m_active = false;
    QElapsedTimer m_steady;
    std::function<qint64()> m_clock;
};

} // namespace fhmi
