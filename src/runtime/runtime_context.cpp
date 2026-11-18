#include "runtime/runtime_context.h"
#include "core/can_frame.h"
#include "core/industrial_catalog.h"

#include "comm/modbus_slave.h"
#include "runtime/hmi_widget.h"
#include "runtime/modbus_poller.h"
#include "runtime/ui_helpers.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>

namespace fhmi {

RuntimeContext::RuntimeContext(QObject *parent) : QObject(parent), m_hub(new CommHub(this))
{
    m_data = new ProjectData(this);
    m_alarmSound = new QTimer(this);
    m_alarmSound->setInterval(5000);
    connect(m_alarmSound, &QTimer::timeout, this, [this] {
        if (m_active && m_data->audible())
            QApplication::beep();
    });
    connect(m_data, &ProjectData::storageError, this,
            [this](const QString &error) { notify(tr("报警历史"), error, NotifyType::Error); });
    connect(m_data, &ProjectData::valuesChanged, this, [this](const Message &values) {
        for (const auto &binding : m_bindings)
            if (binding.parseMode == ParseMode::DataPoint && values.contains(binding.key))
                push(binding, values.value(binding.key));
    });
    connect(m_data, &ProjectData::alarmRaised, this, [this](const AlarmState &alarm) {
        notify(tr("%1 级报警 · %2").arg(alarm.level).arg(alarm.id), alarm.text,
               alarm.level >= 3 ? NotifyType::Error : NotifyType::Warning, 5000);
        if (m_alarmSound->property("enabled").toBool() && m_data->audible())
            QApplication::beep();
    });
    m_poller = new ModbusPoller(this);
    m_s7 = new S7Client(this);
    m_s7Timer = new QTimer(this);
    m_s7Timer->setInterval(100);
    connect(m_s7Timer,&QTimer::timeout,this,[this] {
        QList<S7::Need> needs;
        for(const auto &binding:m_bindings) {
            const auto widget=m_widgets.value(binding.widgetId);
            if (binding.s7 && (!binding.projectPoint.isEmpty() || (widget && widget->isVisible())))
                needs.append({binding.sourceCardId, *binding.s7});
        }
        for(const auto &curves:m_s7Curves) needs.append(curves);
        m_s7->setNeeds(needs);
    });
    connect(m_s7,&S7Client::sampleReceived,this,[this](const S7::Sample &sample) {
        if(!m_active)return;
        for(const auto &binding:m_bindings) {
            if(!binding.s7 || binding.sourceCardId!=sample.sourceCardId || binding.s7->address.canonical!=sample.address
                || binding.s7->type.name!=sample.dataType)continue;
            if (!binding.projectPoint.isEmpty()) {
                m_data->setValue(binding.projectPoint,
                                 sample.quality == QLatin1String("good") ? binding.evaluate(sample.value) : QVariant());
            } else if (auto widget = m_widgets.value(binding.widgetId)) {
                widget->setProperty("s7Quality",sample.quality);
                widget->setProperty("s7Timestamp",sample.timestamp);
                if (sample.quality != QLatin1String("good")) {
                    fluentToolTip(widget, tr("S7 数据不可用：%1").arg(sample.quality));
                } else {
                    widget->setToolTip(QString());
                    push(binding, sample.value);
                }
            }
        }
        m_data->evaluate();
        emit s7Sample(sample);
    });
    connect(m_s7,&S7Client::logMessage,m_hub,&CommHub::linkInfo);
    connect(m_s7,&S7Client::writeFinished,this,[this](const QString &,bool success,const QString &error) {
        if(!success)notify(tr("S7 写入"),error,NotifyType::Error);
    });
    m_steady.start();
    connect(m_hub, &CommHub::messageReceived, this, &RuntimeContext::onMessage);
    connect(m_hub, &CommHub::rawReceived, this, &RuntimeContext::onRaw);
    connect(m_hub, &CommHub::canFrameDecoded, this, &RuntimeContext::onCanFrame);
    connect(m_hub, &CommHub::streamClosed, this, [this](const DataSource &source) {
        for (BindingEntry &binding : m_bindings) {
            binding.resetStream(source.stream);
        }
        m_data->invalidateStream(source.stream);
    });
}

void RuntimeContext::registerWidget(const QString &id, HmiWidget *widget)
{
    if (!id.isEmpty()) {
        widget->setProperty("fhmiWidgetId",id);
        m_widgets.insert(id, widget);
    }
}

void RuntimeContext::addBinding(BindingEntry binding)
{
    if (binding.parseMode == ParseMode::CanSignal) {
        m_canBindings[binding.canId.key()].append(m_bindings.size());
    }
    if (!binding.projectPoint.isEmpty() && binding.usesParsedData()) {
        m_parsedProjectBindings.append(m_bindings.size());
    }
    m_bindings.push_back(std::move(binding));
}

void RuntimeContext::configureData(const Project &project)
{
    const QString key = project.logic.value(QStringLiteral("id"))
                            .toString(project.filePath.isEmpty() ? project.appName : project.filePath);
    const QString history =
        project.logic.value(QStringLiteral("alarms")).toArray().isEmpty()
            ? QString()
            : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/alarms/") +
                  QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex()) +
                  QStringLiteral(".json");
    m_data->configure(project.logic, history);
    m_alarmSound->setProperty("enabled", project.logic.value(QStringLiteral("soundEnabled")).toBool());
    for (const auto &p : project.logic.value(QStringLiteral("points")).toArray()) {
        const auto point = p.toObject();
        if (!point.value(QStringLiteral("expression")).toString().trimmed().isEmpty())
            continue;
        const QString name = point.value(QStringLiteral("name")).toString();
        const QString id = QStringLiteral("@data:") + name;
        if (auto binding =
                BindingEntry::fromWidget({{QStringLiteral("id"), id},
                                          {QStringLiteral("type"), QStringLiteral("NumericDisplay")},
                                          {QStringLiteral("binding"), point.value(QStringLiteral("binding"))}})) {
            binding->projectPoint = name;
            addBinding(*binding);
        }
    }
}

void RuntimeContext::clear()
{
    m_data->configure({});
    m_alarmSound->stop();
    m_widgets.clear();
    m_bindings.clear();
    m_canBindings.clear();
    m_parsedProjectBindings.clear();
    m_curveNeeds.clear();
    m_s7Curves.clear();
    m_s7->clear();
    m_poller->reset();
}

QList<ModbusNeed> RuntimeContext::modbusNeeds() const
{
    QList<ModbusNeed> needs;
    for (const BindingEntry &binding : m_bindings) {
        if (auto need = modbusNeed(binding)) {
            const QPointer<HmiWidget> widget = m_widgets.value(binding.widgetId);
            need->visible = !binding.projectPoint.isEmpty() || (widget && widget->isVisible());
            needs.append(*need);
        }
    }
    for (const auto &[chart, curves] : m_curveNeeds) {
        QString id;
        for (auto it = m_widgets.constBegin(); it != m_widgets.constEnd(); ++it) {
            if (it.value().data() == chart) {
                id = it.key();
            }
        }
        for (ModbusNeed need : curves) {
            need.widgetId = id;
            need.visible = true; // a curve keeps recording while it is out of sight
            needs.append(need);
        }
    }
    return needs;
}

void RuntimeContext::modbusReadFailed(const Link &link, const QString &endpoint, const ModbusBlock &block,
                                      int fallbackSlave)
{
    for (const auto &binding : m_bindings) {
        if (binding.projectPoint.isEmpty() || !link.ownsStream(m_data->valueStream(binding.projectPoint), endpoint))
            continue;
        const auto need = modbusNeed(binding);
        if (!need || (need->slave > 0 ? need->slave : fallbackSlave) != block.slave ||
            modbusReadFunction(need->table) != block.fc || (binding.mbFcFilter >= 0 && binding.mbFcFilter != block.fc) ||
            need->address < block.start || need->address + need->count > block.start + block.count)
            continue;
        m_data->setValue(binding.projectPoint, {});
    }
    m_data->evaluate();
}

void RuntimeContext::setCurveNeeds(const QObject *chart, const QList<ModbusNeed> &needs)
{
    for (qsizetype i = 0; i < m_curveNeeds.size(); ++i) {
        if (m_curveNeeds.at(i).first == chart) {
            m_curveNeeds.removeAt(i);
            break;
        }
    }
    if (!needs.isEmpty()) {
        m_curveNeeds.append({chart, needs});
    }
}

void RuntimeContext::setS7CurveNeeds(const QObject *chart, const QList<S7::Need> &needs)
{
    if(needs.isEmpty())m_s7Curves.remove(chart);else m_s7Curves.insert(chart,needs);
}

void RuntimeContext::setActive(bool active)
{
    m_active = active;
    if (active && m_alarmSound->property("enabled").toBool())
        m_alarmSound->start();
    else
        m_alarmSound->stop();
    if (!active) {
        m_data->clearValues();
        m_data->flushHistory();
    } else
        m_data->evaluate();
    emit activeChanged(active);
    if (!active) {
        for (BindingEntry &binding : m_bindings) {
            binding.resetStream();
        }
    }
    m_poller->setActive(active);
    if(active)m_s7Timer->start();else {m_s7Timer->stop();m_s7->setNeeds({});}
    if (active) {
        m_hub->deliverSlaveStart(); // the bound controls start from the slaves' tables
    }
}

bool RuntimeContext::send(const CommandResult &result, QList<QByteArray> *sent)
{
    if(result.command && result.command->kind==EncodedCommand::Kind::S7) {
        QString error;
        if(!m_s7->write(result.command->s7,&error)) {notify(tr("S7 写入"),error,NotifyType::Warning);return false;}
        if(sent)sent->append(result.command->s7.bytes);
        return true;
    }
    bool wroteSlaves = false;
    if (result.command && result.command->kind == EncodedCommand::Kind::Modbus) {
        const auto &command = *result.command;
        for (ModbusSlave *slave : m_hub->modbusSlaves(command.route)) {
            if (command.route.isExplicit() && slave->settings().slave != command.modbus.slave)
                continue;
            wroteSlaves = slave->execute(command.modbus) || wroteSlaves;
        }
    }
    if (!m_hub->isConnected()) {
        if (wroteSlaves) {
            return true;
        }
        notify(tr("提示"), tr("未连接，命令未发送"), NotifyType::Warning);
        return false;
    }
    if (!result.command) {
        notify(tr("提示"), result.error, NotifyType::Error);
        return false;
    }
    QString error;
    if (m_hub->send(*result.command, sent, &error) == 0 && !wroteSlaves) {
        // A CAN command that a link could not resolve says why; otherwise no link of its kind is connected.
        if (!error.isEmpty()) {
            notify(tr("提示"), error, NotifyType::Error);
        } else {
            notify(tr("提示"), tr("没有链路接收该命令"), NotifyType::Warning);
        }
        return false;
    }
    return true;
}

void RuntimeContext::writeModbusSlaves(const HmiWidget *widget, double value, bool sendsCommand)
{
    // S7 already required an explicit target before general routing existed.
    const QString mode = widget->properties().value(QStringLiteral("commandMode")).toString();
    const auto target = widget->commandRoute();
    if (sendsCommand && (!target.endpoint.isEmpty() || (target.isExplicit() && mode != QLatin1String("S7"))))
        return;
    const QString id = widget->property("fhmiWidgetId").toString();
    if (id.isEmpty())
        return;
    for (const BindingEntry &binding : m_bindings) {
        if (binding.widgetId != id || binding.parseMode != ParseMode::Modbus ||
            !binding.expression.trimmed().isEmpty())
            continue;
        const QString table = ModbusArea::prefix(binding.mbArea);
        if (table.isEmpty())
            continue;
        const auto route = binding.route();
        if (sendsCommand && mode == QLatin1String("S7") && route.isExplicit())
            continue;
        for (ModbusSlave *slave : m_hub->modbusSlaves(route)) {
            if (route.isExplicit() && binding.mbSlaveFilter >= 0 && slave->settings().slave != binding.mbSlaveFilter)
                continue;
            slave->writeValue(table, binding.mbRegIndex, value, binding.mbDataType, binding.mbRegOrder);
        }
    }
}

void RuntimeContext::notify(const QString &title, const QString &content, const QString &type, int durationMs)
{
    emit notificationRequested(title, content, type, durationMs);
}

void RuntimeContext::onMessage(const Message &message, const DataSource &source)
{
    if (!m_active) {
        return;
    }
    for (const BindingEntry &binding : m_bindings) {
        if (!binding.usesParsedData() || !binding.route().accepts(source) ||
            (source.canText && !binding.projectPoint.isEmpty())) {
            continue;
        }
        const QVariant value = binding.extractFromParsed(message);
        const bool special = binding.typeName==QLatin1String("DigitalTimingChart")||binding.typeName==QLatin1String("RotaryEncoder")||binding.typeName==QLatin1String("StatusWordPanel")||binding.typeName==QLatin1String("BmsCellMatrix")||binding.typeName==QLatin1String("DcPowerSupply");
        bool explicitlyPresent = (special || !binding.projectPoint.isEmpty()) &&
                                 binding.parseMode == ParseMode::KeyValue && message.contains(binding.key);
        if ((special || !binding.projectPoint.isEmpty()) && (!value.isValid() || value.isNull()) &&
            binding.parseMode == ParseMode::JsonPath) {
            const auto dot=binding.jsonPath.indexOf(QLatin1Char('.'));const auto first=dot<0?binding.jsonPath:binding.jsonPath.left(dot);
            if(message.contains(first)){if(dot<0)explicitlyPresent=true;else BindingEntry::resolvePath(message.value(first),binding.jsonPath.mid(dot+1),&explicitlyPresent);}
        }
        if ((value.isValid() && !value.isNull()) || explicitlyPresent) {
            push(binding, value, source);
        }
    }
    m_data->evaluate();
}

void RuntimeContext::onRaw(const QByteArray &bytes, const DataSource &source)
{
    // A CAN link's frame line is for showing; its frames reach bindings decoded, as messages.
    if (!m_active || source.canText) {
        return;
    }
    struct Samples
    {
        BindingEntry *binding;
        QList<QVariant> values;
    };
    QMap<QPair<QByteArray, QByteArray>, QList<Samples>> points;
    for (BindingEntry &binding : m_bindings) {
        if (!binding.usesRawData() || !binding.route().accepts(source))
            continue;
        if (!binding.projectPoint.isEmpty()) {
            points[{binding.frameHeader, binding.frameTail}].append(
                {&binding, binding.extractAllFromRaw(bytes, source.stream, true)});
        } else {
            for (const auto &value : binding.extractAllFromRaw(bytes, source.stream))
                push(binding, value, source);
        }
    }
    // Preserve transient alarms when several framed samples arrive in one transport read.
    for (const auto &group : points) {
        qsizetype count = 0;
        for (const auto &samples : group)
            count = std::max(count, samples.values.size());
        for (qsizetype i = 0; i < count; ++i) {
            for (const auto &samples : group)
                if (i < samples.values.size())
                    push(*samples.binding, samples.values[i], source);
            m_data->evaluate();
        }
    }
}

void RuntimeContext::onCanFrame(const CanFrame &frame, const Message &message, const DataSource &source)
{
    if (!m_active)
        return;
    // Confirm DBC and bit bindings together so calculations see a complete received frame.
    for (const auto index : m_parsedProjectBindings) {
        const auto &binding = m_bindings[index];
        if (!binding.route().accepts(source))
            continue;
        const auto value = binding.extractFromParsed(message);
        if (value.isValid())
            push(binding, value, source);
    }
    const auto it = m_canBindings.constFind(CanId{frame.id, frame.extended}.key());
    if (it != m_canBindings.constEnd())
        for (const size_t index : *it) {
            const auto &binding = m_bindings[index];
            if (!binding.route().accepts(source))
                continue;
            const auto value = binding.extractFromCan(frame);
            if (value.isValid() || (!binding.projectPoint.isEmpty() && !frame.rtr && !frame.error))
                push(binding, value, source);
        }
    m_data->evaluate();
}

void RuntimeContext::push(const BindingEntry &binding, const QVariant &raw, const DataSource &source)
{
    if (!binding.projectPoint.isEmpty()) {
        m_data->setValue(binding.projectPoint, raw.isValid() && !raw.isNull() ? binding.evaluate(raw) : QVariant(),
                         source.stream);
        return;
    }
    const QPointer<HmiWidget> widget = m_widgets.value(binding.widgetId);
    if (widget) {
        const QString type=widget->typeName();
        if(type==QLatin1String("BmsCellMatrix")||type==QLatin1String("DcPowerSupply")||type==QLatin1String("StatusWordPanel")||type==QLatin1String("DigitalTimingChart")||type==QLatin1String("RotaryEncoder")) {
            widget->applyBoundValue(binding.evaluate(raw),binding);
        } else if (isIndustrialCategory(widget->meta().category)) {
            const auto value = binding.chartSample(raw);
            widget->applyBoundValue(value ? QVariant(*value) : QVariant(), binding);
        } else {
            widget->applyBoundValue(binding.formatValue(binding.evaluate(raw)), binding);
        }
    }
}

} // namespace fhmi
