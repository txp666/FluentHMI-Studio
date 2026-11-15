#include "runtime/runtime_context.h"
#include "core/industrial_catalog.h"

#include "comm/modbus_slave.h"
#include "runtime/hmi_widget.h"
#include "runtime/modbus_poller.h"
#include <QtCore/QTimer>

namespace fhmi {

RuntimeContext::RuntimeContext(QObject *parent) : QObject(parent), m_hub(new CommHub(this))
{
    m_poller = new ModbusPoller(this);
    m_s7 = new S7Client(this);
    m_s7Timer = new QTimer(this);
    m_s7Timer->setInterval(100);
    connect(m_s7Timer,&QTimer::timeout,this,[this] {
        QList<S7::Need> needs;
        for(const auto &binding:m_bindings) {
            const auto widget=m_widgets.value(binding.widgetId);
            if(binding.s7 && widget && widget->isVisible()) needs.append({binding.sourceCardId,*binding.s7});
        }
        for(const auto &curves:m_s7Curves) needs.append(curves);
        m_s7->setNeeds(needs);
    });
    connect(m_s7,&S7Client::sampleReceived,this,[this](const S7::Sample &sample) {
        if(!m_active)return;
        for(const auto &binding:m_bindings) {
            if(!binding.s7 || binding.sourceCardId!=sample.sourceCardId || binding.s7->address.canonical!=sample.address
                || binding.s7->type.name!=sample.dataType)continue;
            if(auto widget=m_widgets.value(binding.widgetId)) {
                widget->setProperty("s7Quality",sample.quality);
                widget->setProperty("s7Timestamp",sample.timestamp);
                if(sample.quality!=QLatin1String("good")) widget->setToolTip(tr("S7 数据不可用：%1").arg(sample.quality));
                else { widget->setToolTip(QString());push(binding,sample.value); }
            }
        }
        emit s7Sample(sample);
    });
    connect(m_s7,&S7Client::logMessage,m_hub,&CommHub::linkInfo);
    connect(m_s7,&S7Client::writeFinished,this,[this](const QString &,bool success,const QString &error) {
        if(!success)notify(tr("S7 写入"),error,NotifyType::Error);
    });
    m_steady.start();
    connect(m_hub, &CommHub::messageReceived, this, &RuntimeContext::onMessage);
    connect(m_hub, &CommHub::rawReceived, this, &RuntimeContext::onRaw);
    connect(m_hub, &CommHub::canFrames, this,
            [this](CanLink *, const QList<CanFrame> &frames, bool sent) { onCanFrames(frames, sent); });
    connect(m_hub, &CommHub::linkDisconnected, this, [this]() {
        for (BindingEntry &binding : m_bindings) {
            binding.resetStream();
        }
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
    m_bindings.push_back(std::move(binding));
}

void RuntimeContext::clear()
{
    m_widgets.clear();
    m_bindings.clear();
    m_canBindings.clear();
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
            need->visible = widget && widget->isVisible();
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
        for (ModbusSlave *slave : m_hub->modbusSlaves()) {
            wroteSlaves = slave->execute(result.command->modbus) || wroteSlaves;
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

void RuntimeContext::writeModbusSlaves(const HmiWidget *widget, double value)
{
    const QList<ModbusSlave *> slaves = m_hub->modbusSlaves();
    if (slaves.isEmpty()) {
        return;
    }
    for (auto it = m_widgets.constBegin(); it != m_widgets.constEnd(); ++it) {
        if (it.value() != widget) {
            continue;
        }
        for (const BindingEntry &binding : m_bindings) {
            const QString table = ModbusArea::prefix(binding.mbArea);
            if (binding.widgetId != it.key() || binding.parseMode != ParseMode::Modbus || table.isEmpty() ||
                !binding.expression.trimmed().isEmpty()) {
                continue; // an expression cannot be worked back
            }
            for (ModbusSlave *slave : slaves) {
                slave->writeValue(table, binding.mbRegIndex, value, binding.mbDataType, binding.mbRegOrder);
            }
        }
    }
}

bool RuntimeContext::sendModbus(const ModbusRequest &request, QList<QByteArray> *sent, const ModbusSend &how)
{
    if (!m_hub->isConnected()) {
        notify(tr("提示"), tr("未连接，命令未发送"), NotifyType::Warning);
        return false;
    }
    if (m_hub->sendModbus(request, sent, how) == 0) {
        notify(tr("提示"), tr("没有链路接收该命令"), NotifyType::Warning);
        return false;
    }
    return true;
}

void RuntimeContext::notify(const QString &title, const QString &content, const QString &type, int durationMs)
{
    emit notificationRequested(title, content, type, durationMs);
}

void RuntimeContext::onMessage(const Message &message)
{
    if (!m_active) {
        return;
    }
    for (const BindingEntry &binding : m_bindings) {
        if (!binding.usesParsedData()) {
            continue;
        }
        const QVariant value = binding.extractFromParsed(message);
        const bool special = binding.typeName==QLatin1String("DigitalTimingChart")||binding.typeName==QLatin1String("RotaryEncoder")||binding.typeName==QLatin1String("StatusWordPanel")||binding.typeName==QLatin1String("BmsCellMatrix")||binding.typeName==QLatin1String("DcPowerSupply");
        bool explicitlyPresent = special && binding.parseMode==ParseMode::KeyValue && message.contains(binding.key);
        if(special && (!value.isValid()||value.isNull()) && binding.parseMode==ParseMode::JsonPath) {
            const auto dot=binding.jsonPath.indexOf(QLatin1Char('.'));const auto first=dot<0?binding.jsonPath:binding.jsonPath.left(dot);
            if(message.contains(first)){if(dot<0)explicitlyPresent=true;else BindingEntry::resolvePath(message.value(first),binding.jsonPath.mid(dot+1),&explicitlyPresent);}
        }
        if ((value.isValid() && !value.isNull()) || explicitlyPresent) {
            push(binding, value);
        }
    }
}

void RuntimeContext::onRaw(const QByteArray &bytes)
{
    // A CAN link's frame line is for showing; its frames reach bindings decoded, as messages.
    if (!m_active || m_hub->carriesCanText()) {
        return;
    }
    for (BindingEntry &binding : m_bindings) {
        if (!binding.usesRawData()) {
            continue;
        }
        for (const QVariant &value : binding.extractAllFromRaw(bytes)) {
            push(binding, value);
        }
    }
}

void RuntimeContext::onCanFrames(const QList<CanFrame> &frames, bool sent)
{
    // Only what the bus brought in shows: a frame this program sent is its own command, not the device's state.
    if (!m_active || sent || m_canBindings.isEmpty()) {
        return;
    }
    for (const CanFrame &frame : frames) {
        const auto it = m_canBindings.constFind(CanId{frame.id, frame.extended}.key());
        if (it == m_canBindings.constEnd()) {
            continue;
        }
        for (const size_t index : *it) {
            const BindingEntry &binding = m_bindings[index];
            const QVariant value = binding.extractFromCan(frame);
            if (value.isValid()) {
                push(binding, value);
            }
        }
    }
}

void RuntimeContext::push(const BindingEntry &binding, const QVariant &raw)
{
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
