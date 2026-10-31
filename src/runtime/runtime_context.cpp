#include "runtime/runtime_context.h"

#include "runtime/hmi_widget.h"

namespace fhmi {

RuntimeContext::RuntimeContext(QObject *parent) : QObject(parent), m_hub(new CommHub(this))
{
    connect(m_hub, &CommHub::messageReceived, this, &RuntimeContext::onMessage);
    connect(m_hub, &CommHub::rawReceived, this, &RuntimeContext::onRaw);
    connect(m_hub, &CommHub::linkDisconnected, this, [this]() {
        for (BindingEntry &binding : m_bindings) {
            binding.resetStream();
        }
    });
}

void RuntimeContext::registerWidget(const QString &id, HmiWidget *widget)
{
    if (!id.isEmpty()) {
        m_widgets.insert(id, widget);
    }
}

void RuntimeContext::addBinding(BindingEntry binding)
{
    m_bindings.push_back(std::move(binding));
}

void RuntimeContext::clear()
{
    m_widgets.clear();
    m_bindings.clear();
}

void RuntimeContext::setActive(bool active)
{
    m_active = active;
    if (!active) {
        for (BindingEntry &binding : m_bindings) {
            binding.resetStream();
        }
    }
}

bool RuntimeContext::send(const CommandResult &result, QList<QByteArray> *sent)
{
    if (!m_hub->isConnected()) {
        notify(tr("提示"), tr("未连接，命令未发送"), NotifyType::Warning);
        return false;
    }
    if (!result.command) {
        notify(tr("提示"), result.error, NotifyType::Error);
        return false;
    }
    if (m_hub->send(*result.command, sent) == 0) {
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
        if (value.isValid() && !value.isNull()) {
            push(binding, value);
        }
    }
}

void RuntimeContext::onRaw(const QByteArray &bytes)
{
    if (!m_active) {
        return;
    }
    for (BindingEntry &binding : m_bindings) {
        if (binding.usesParsedData()) {
            continue;
        }
        for (const QVariant &value : binding.extractAllFromRaw(bytes)) {
            push(binding, value);
        }
    }
}

void RuntimeContext::push(const BindingEntry &binding, const QVariant &raw)
{
    const QPointer<HmiWidget> widget = m_widgets.value(binding.widgetId);
    if (widget) {
        widget->applyBoundValue(binding.formatValue(binding.evaluate(raw)), binding);
    }
}

} // namespace fhmi
