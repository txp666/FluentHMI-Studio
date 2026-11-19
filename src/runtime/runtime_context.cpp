#include "runtime/runtime_context.h"
#include "core/can_frame.h"
#include "core/industrial_catalog.h"
#include "core/project_logic.h"

#include "comm/can/can_link.h"
#include "comm/modbus_slave.h"
#include "runtime/app_dialog.h"
#include "runtime/hmi_widget.h"
#include "runtime/modbus_poller.h"
#include "runtime/periodic_sender.h"
#include "runtime/ui_helpers.h"
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGraphicsProxyWidget>
#include <QtWidgets/QGraphicsView>

namespace fhmi {

namespace {

QString commandDetails(const EncodedCommand &command, const QString &mode, const QStringList &labels,
                       const QList<SendTarget> &targets, bool periodic)
{
    const auto tr = [](const char *source) { return QCoreApplication::translate("fhmi", source); };
    QStringList details;
    if (command.kind != EncodedCommand::Kind::Can) {
        details.append(tr("发送目标\n%1\n\n发送内容\n%2")
                           .arg(labels.join(u'\n'), CommandEncoder::describe(command, mode)));
    } else {
        for (const auto &target : targets) {
            auto *link = static_cast<CanLink *>(target.link.data());
            QString error;
            const auto payload = link->prepare(command.can, &error);
            QString content = error;
            if (payload) {
                content = Can::frameLine(payload->frame);
                if (!payload->signalMask.isEmpty())
                    content += u'\n' + CommandEncoder::describe(command, mode);
                if (!periodic && payload->periodMs > 0)
                    content += u'\n' + tr("循环周期 %1 ms").arg(payload->periodMs);
            }
            details.append(tr("发送目标\n%1\n\n发送内容\n%2").arg(target.label(), content));
        }
    }
    if (!command.inputValues.isEmpty())
        details.append(tr("输入值\n%1").arg(CommandEncoder::inputValuesText(command, true)));
    if (!command.framingNotice.isEmpty())
        details.append(command.framingNotice);
    return details.join(QStringLiteral("\n\n"));
}

QWidget *commandDialogHost(HmiWidget *sender)
{
    auto *host = sender->window();
    // The standalone player embeds the runtime canvas in a graphics view.
    if (const auto *proxy = host->graphicsProxyWidget()) {
        const auto views = proxy->scene()->views();
        if (!views.isEmpty())
            return views.first()->window();
    }
    return host;
}

} // namespace

RuntimeContext::RuntimeContext(QObject *parent) : QObject(parent), m_hub(new CommHub(this))
{
    m_periodic = new PeriodicSender(m_hub, this);
    connect(m_periodic, &PeriodicSender::failed, this,
            [this](const QString &error) { notify(tr("周期发送"), error, NotifyType::Warning); });
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
        for (const auto &binding : m_bindings) {
            if (binding.parseMode != ParseMode::DataPoint || !values.contains(binding.key))
                continue;
            const auto widget = m_widgets.value(binding.widgetId);
            const auto sample = m_data->sample(binding.key);
            if (widget) {
                const bool fault = sample.quality != DataQuality::Current && sample.quality != DataQuality::Waiting;
                widget->setBindingProblem(fault ? dataQualityText(sample.quality) : QString(), sample.detail);
                if (fault)
                    widget->setBindingStatus(HmiWidget::BindingStatus::Stale);
            }
            push(binding, values.value(binding.key));
        }
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
                const DataSource source{sample.sourceCardId, 0, {}};
                if (sample.quality == QLatin1String("good")) {
                    const auto value = binding.evaluateStrict(sample.value);
                    m_data->setSample(binding.projectPoint, value, source,
                                      value.isValid() ? DataQuality::Current : DataQuality::ConversionFailed,
                                      sample.timestamp, value.isValid() ? QString() : binding.expression);
                } else {
                    m_data->setSample(binding.projectPoint, {}, source, DataQuality::Old, sample.timestamp);
                }
            } else if (auto widget = m_widgets.value(binding.widgetId)) {
                widget->setProperty("s7Quality",sample.quality);
                widget->setProperty("s7Timestamp",sample.timestamp);
                if (sample.quality != QLatin1String("good")) {
                    if (widget->bindingStatus() == HmiWidget::BindingStatus::Live)
                        widget->setBindingStatus(HmiWidget::BindingStatus::Stale);
                    widget->setBindingProblem(dataQualityText(DataQuality::Old), sample.quality);
                } else {
                    widget->setToolTip(QString());
                    push(binding, sample.value);
                }
            }
        }
        m_data->evaluate();
        emit s7Sample(sample);
    });
    connect(m_s7, &S7Client::statusChanged, this, [this](const QString &id, bool, const QString &message) {
        emit m_hub->linkInfo(message, DataSource{id, 0, {}});
    });
    connect(m_s7, &S7Client::errorOccurred, this, [this](const QString &id, const QString &message) {
        emit m_hub->linkError(message, DataSource{id, 0, {}});
    });
    connect(m_s7,&S7Client::writeFinished,this,[this](const QString &id,bool success,const QString &error) {
        DataSource source;
        source.connectionId = id;
        if (success)
            emit m_hub->linkInfo(tr("S7 写入已收到设备应答"), source);
        else {
            emit m_hub->linkError(error, source);
            notify(tr("S7 写入"), error, NotifyType::Error);
        }
    });
    m_steady.start();
    connect(m_hub, &CommHub::unitReceived, this, &RuntimeContext::onUnit);
    connect(m_hub, &CommHub::canFrameDecoded, this, &RuntimeContext::onCanFrame);
    connect(m_hub, &CommHub::streamClosed, this, [this](const DataSource &source) {
        for (BindingEntry &binding : m_bindings) {
            binding.resetStream(source.stream);
        }
        for (auto it = m_widgetStreams.begin(); it != m_widgetStreams.end();) {
            if (it.value() == source.stream) {
                if (auto widget = m_widgets.value(it.key())) {
                    widget->setBindingStatus(HmiWidget::BindingStatus::Stale);
                    widget->setBindingProblem(dataQualityText(DataQuality::Old));
                }
                it = m_widgetStreams.erase(it);
            } else {
                ++it;
            }
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

QString RuntimeContext::widgetPreferencesGroup(const HmiWidget &widget) const
{
    const QString id = widget.property("fhmiWidgetId").toString();
    if (m_projectPreferencesGroup.isEmpty() || id.isEmpty())
        return {};
    return m_projectPreferencesGroup + u'/' + widget.typeName() + u'/' + QString::fromLatin1(id.toUtf8().toHex());
}

std::optional<CommandEncoder::InputValue> RuntimeContext::inputValue(const QString &id) const
{
    if (const auto name = pointNameForInput(id); !name.isEmpty()) {
        if (isLegacyPointInput(id)) {
            const auto widget = m_widgets.value(id);
            if (widget && widget->typeName() == QLatin1String("LineEdit"))
                return std::nullopt;
        }
        const auto sample = m_data->sample(name);
        if (sample.quality != DataQuality::Current)
            return std::nullopt;
        return CommandEncoder::InputValue{sample.value, name};
    }
    const auto widget = m_widgets.value(widgetIdForInput(id));
    const auto value = widget ? widget->inputValue() : std::nullopt;
    if (!value)
        return std::nullopt;
    return CommandEncoder::InputValue{*value, widget->properties().value(QStringLiteral("title")).toString().trimmed()};
}

bool RuntimeContext::CommandTargets::operator==(const CommandTargets &other) const
{
    return links == other.links && slaves == other.slaves && s7Session == other.s7Session;
}

void RuntimeContext::addBinding(BindingEntry binding)
{
    if (auto widget = m_widgets.value(binding.widgetId))
        widget->setBindingStatus(HmiWidget::BindingStatus::Waiting);
    if (!binding.projectPoint.isEmpty()) {
        QString error;
        const auto route = resolveProjectRoute(m_connectionCards, m_projectParser, bindingRouteTransport(binding.parseMode),
                                               binding.route(), RoutePurpose::DataPoint, &error);
        if (!route) {
            m_data->setProblem(binding.projectPoint, DataQuality::ConfigurationError, error);
            return;
        }
        binding.sourceCardId = route->connectionId;
    } else if (!resolveBindingSource(binding)) {
        if (auto widget = m_widgets.value(binding.widgetId))
            widget->setBindingProblem(tr("请选择来源连接"));
        return;
    }
    if (binding.parseMode == ParseMode::CanSignal) {
        m_canBindings[binding.canId.key()].append(m_bindings.size());
    }
    if (!binding.projectPoint.isEmpty() && binding.usesParsedData()) {
        m_parsedProjectBindings.append(m_bindings.size());
    }
    m_bindings.push_back(std::move(binding));
}

bool RuntimeContext::resolveBindingSource(BindingEntry &binding) const
{
    if (binding.parseMode == ParseMode::DataPoint || !binding.sourceCardId.isEmpty() || m_connectionCards.isEmpty())
        return true;
    const auto candidates = routeConnections(m_connectionCards, bindingRouteTransport(binding.parseMode),
                                              !binding.sourceEndpoint.isEmpty(), m_projectParser);
    if (candidates.size() != 1)
        return false;
    binding.sourceCardId = candidates.first().first;
    return true;
}

void RuntimeContext::configureData(const Project &project)
{
    const QString path = QFileInfo(project.filePath).canonicalFilePath();
    m_projectPreferencesGroup.clear();
    if (!path.isEmpty()) {
        const auto hash = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
        m_projectPreferencesGroup = QStringLiteral("runtimeProjects/") + QString::fromLatin1(hash);
    }
    m_connectionCards = {};
    m_frameTemplates = project.frameTemplates;
    m_projectParser = project.parserType();
    visitWidgets(project.widgets, [this](const QJsonObject &widget) {
        if (isConnectionCard(widget.value(QStringLiteral("type")).toString()))
            m_connectionCards.append(widget);
        return true;
    });
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
        if (isProjectParameter(point) || !point.value(QStringLiteral("expression")).toString().trimmed().isEmpty())
            continue;
        const QString name = point.value(QStringLiteral("name")).toString();
        if (auto binding =
                BindingEntry::fromWidget({{QStringLiteral("type"), QStringLiteral("NumericDisplay")},
                                          {QStringLiteral("binding"), point.value(QStringLiteral("binding"))}})) {
            binding->projectPoint = name;
            addBinding(*binding);
        }
    }
}

void RuntimeContext::clear()
{
    m_periodic->stopAll();
    m_data->configure({});
    m_alarmSound->stop();
    m_widgets.clear();
    m_projectPreferencesGroup.clear();
    m_connectionCards = {};
    m_frameTemplates = {};
    m_widgetStreams.clear();
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
        const auto need = modbusNeed(binding);
        if (!need || (need->slave > 0 ? need->slave : fallbackSlave) != block.slave ||
            modbusReadFunction(need->table) != block.fc || (binding.mbFcFilter >= 0 && binding.mbFcFilter != block.fc) ||
            need->address < block.start || need->address + need->count > block.start + block.count)
            continue;
        if (!binding.projectPoint.isEmpty()) {
            for (quint64 stream : m_data->sampleStreams(binding.projectPoint))
                if (link.ownsStream(stream, endpoint))
                    m_data->markOld(binding.projectPoint, stream);
        } else if (const quint64 stream = m_widgetStreams.value(binding.widgetId); stream && link.ownsStream(stream, endpoint)) {
            if (auto widget = m_widgets.value(binding.widgetId)) {
                widget->setBindingStatus(HmiWidget::BindingStatus::Stale);
                widget->setBindingProblem(dataQualityText(DataQuality::Old));
            }
        }
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
        m_periodic->stopAll();
        if (m_sendConfirmation) {
            m_sendConfirmation->hide();
            m_sendConfirmation->deleteLater();
            m_sendConfirmation.clear();
        }
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

std::optional<DataRoute> RuntimeContext::resolveCommandRoute(const EncodedCommand &original, QString *error) const
{
    using Kind = EncodedCommand::Kind;
    const auto kind = original.kind;
    const auto ambiguous = [error](const QString &reason) -> std::optional<DataRoute> {
        if (error)
            *error = reason;
        return std::nullopt;
    };
    const auto configured = resolveProjectRoute(m_connectionCards, m_projectParser, CommandEncoder::sendRouteFilter(original), original.route,
                                                RoutePurpose::Send, error);
    if (!configured || configured->isBroadcast())
        return configured;
    auto route = *configured;
    auto command = original;
    command.route = route;
    const auto targets = commandTargets(command);
    if (kind == Kind::Modbus && !route.connectionId.isEmpty() && targets.links.isEmpty() &&
        CommandEncoder::modbusFunctionSpec(command.modbus.func).argument == CommandEncoder::ModbusArgument::ReadCount &&
        !m_hub->modbusSlaves(route).isEmpty())
        return ambiguous(tr("本机 Modbus 从站表仅接受写入命令，可直接绑定寄存器取值"));
    QSet<Link *> links;
    for (const auto &target : targets.links)
        links.insert(target.link.data());
    if (route.connectionId.isEmpty()) {
        if (links.size() + targets.slaves.size() > 1)
            return ambiguous(tr("有多个匹配的发送连接，请选择目标连接或明确启用广播"));
        if (links.size() == 1)
            route.connectionId = (*links.cbegin())->connectionId();
    }
    if (route.endpoint.isEmpty() && links.size() == 1 && targets.links.size() > 1)
        return ambiguous(tr("当前连接有多个子通道，请选择发送子通道"));
    return route;
}

RuntimeContext::CommandTargets RuntimeContext::commandTargets(const EncodedCommand &command) const
{
    CommandTargets targets;
    targets.links = m_hub->sendTargets(command);
    if (command.kind == EncodedCommand::Kind::S7)
        targets.s7Session = m_s7->sendSession(command.route.connectionId);
    if (command.kind == EncodedCommand::Kind::Modbus) {
        for (auto *slave : m_hub->modbusSlaves(command.route))
            if (slave->acceptsCommand(command.modbus))
                targets.slaves.append(slave);
    }
    return targets;
}

QStringList RuntimeContext::commandTargetLabels(const EncodedCommand &command, const CommandTargets &targets) const
{
    QStringList names;
    for (const auto &target : targets.links)
        names.append(target.label());
    for (const auto &slave : targets.slaves) {
        const auto id = m_hub->modbusSlaveSource(slave).connectionId;
        const auto label = tr("本地 Modbus 从站 %1").arg(command.modbus.slave);
        names.append(id.isEmpty() ? label : id + QStringLiteral(" · ") + label);
    }
    if (targets.s7Session)
        names.append(command.route.connectionId);
    return names;
}

QString RuntimeContext::commandTargetText(EncodedCommand::Kind kind, const DataRoute &route) const
{
    EncodedCommand command;
    command.kind = kind;
    command.route = route;
    QString error;
    const auto resolved = resolveCommandRoute(command, &error);
    if (!resolved)
        return error;
    command.route = *resolved;
    const auto names = commandTargetLabels(command, commandTargets(command));
    if (!names.isEmpty())
        return tr("目标连接 %1").arg(names.join(QStringLiteral(", ")));
    if (!resolved->connectionId.isEmpty() && !resolved->isBroadcast())
        return tr("目标 %1 · 未连接").arg(resolved->connectionId +
            (resolved->endpoint.isEmpty() ? QString() : QStringLiteral(" / ") + resolved->endpoint));
    return tr("没有可发送的目标连接");
}

void RuntimeContext::requestSend(HmiWidget *sender, const CommandResult &result, bool confirm, bool showDetails,
                                 const QString &mode, std::function<void()> onSent, int intervalMs,
                                 CommandReader readInputs)
{
    const bool periodic = intervalMs > 0;
    if (m_sendConfirmation || (periodic && (!m_active || sender->isPeriodicSending())))
        return;
    if ((!confirm && !periodic) || !result.command) {
        if (send(result) && onSent)
            onSent();
        return;
    }

    auto command = *result.command;
    QString error;
    const auto target = resolveCommandRoute(command, &error);
    if (!target) {
        notify(tr("提示"), error, NotifyType::Warning);
        return;
    }
    command.route = *target;
    const auto targets = commandTargets(command);
    if (periodic ? targets.links.isEmpty() : targets.size() == 0) {
        notify(tr("提示"), tr("没有可发送的目标连接"), NotifyType::Warning);
        return;
    }

    const bool followsInputs = periodic && bool(readInputs);
    auto dispatch = [this, source = QPointer<HmiWidget>(sender), command, targets, mode, intervalMs,
                     readInputs = std::move(readInputs),
                     onSent = std::move(onSent)] {
        if (!source || source->context() != this)
            return;
        if (intervalMs > 0) {
            if (!m_active)
                return;
            const auto id = source->property("fhmiWidgetId").toString();
            const auto caption = (source->typeName() == QLatin1String("PushButton") ? source->text("text")
                                                                                  : source->text("title")).trimmed();
            QString error;
            if (!m_periodic->start(id, caption.isEmpty() ? id : caption, command, mode, targets.links, intervalMs,
                                   [this, source, onSent] {
                                       if (source && source->context() == this && onSent)
                                           onSent();
                                   }, &error, readInputs) && !error.isEmpty())
                notify(tr("周期发送"), error, NotifyType::Warning);
            return;
        }
        if (!(commandTargets(command) == targets)) {
            notify(tr("提示"), tr("发送目标已变化，请重新下发"), NotifyType::Warning);
            return;
        }
        if (sendResolved(command, targets) && source && source->context() == this && onSent)
            onSent();
    };
    if (!confirm) {
        dispatch();
        return;
    }

    auto *dialog = new AppDialog(tr("确认下发"), commandDialogHost(sender));
    dialog->setObjectName(QStringLiteral("sendConfirmationDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const bool hasInputValues = !command.inputValues.isEmpty();
    const QSize panelSize = showDetails ? QSize(520, 440) : hasInputValues ? QSize(440, 300) : QSize(410, 200);
    dialog->setPreferredSize(panelSize);
    auto *question = new FluentQt::BodyLabel(periodic ? tr("确定每 %1 ms 发送一次吗？").arg(intervalMs)
                                                     : tr("确定发送这条命令吗？"), dialog->widget());
    if (followsInputs)
        question->setText(question->text() + u'\n' + tr("确认后发送首轮，后续每轮读取当时的输入值。"));
    question->setWordWrap(true);
    dialog->viewLayout()->addWidget(question);
    if (showDetails || hasInputValues) {
        auto *details = new FluentQt::PlainTextEdit(dialog->widget());
        details->setObjectName(QStringLiteral("sendConfirmationDetails"));
        details->setAccessibleName(tr("下发详情"));
        details->setReadOnly(true);
        details->setTabChangesFocus(true);
        details->setPlainText(showDetails ? commandDetails(command, mode, commandTargetLabels(command, targets),
                                                        targets.links, periodic)
                                        : CommandEncoder::inputValuesText(command));
        dialog->viewLayout()->addWidget(details, 1);
    }
    dialog->yesButton()->setText(tr("确认发送"));
    dialog->yesButton()->setDefault(false);
    dialog->cancelButton()->setAutoDefault(true);
    dialog->cancelButton()->setDefault(true);

    m_sendConfirmation = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, dispatch = std::move(dispatch)](int choice) {
                if (m_sendConfirmation != dialog)
                    return;
                m_sendConfirmation.clear();
                if (choice == QDialog::Accepted)
                    dispatch();
            });
    dialog->open();
    dialog->cancelButton()->setFocus(Qt::OtherFocusReason);
}

bool RuntimeContext::send(const CommandResult &result, QList<QByteArray> *sent)
{
    if (!result.command) {
        if (!result.error.isEmpty())
            notify(tr("提示"), result.error, NotifyType::Error);
        return false;
    }
    auto command = *result.command;
    QString error;
    const auto target = resolveCommandRoute(command, &error);
    if (!target) {
        notify(tr("提示"), error, NotifyType::Warning);
        return false;
    }
    command.route = *target;
    return sendResolved(command, commandTargets(command), sent);
}

bool RuntimeContext::sendResolved(const EncodedCommand &command, const CommandTargets &targets,
                                  QList<QByteArray> *sent)
{
    QString error;
    if (command.kind == EncodedCommand::Kind::S7) {
        auto write = command.s7;
        write.targetId = command.route.connectionId;
        if (!m_s7->write(write, &error)) {
            notify(tr("S7 写入"), error, NotifyType::Warning);
            return false;
        }
        if (sent)
            sent->append(write.bytes);
        return true;
    }
    int accepted = 0;
    for (const auto &slave : targets.slaves)
        if (slave && slave->execute(command.modbus))
            ++accepted;

    accepted += m_hub->send(command, targets.links, sent, &error);
    if (accepted == 0) {
        const auto message = !error.isEmpty() ? error : m_hub->isConnected() ? tr("没有链路接收该命令")
                                                                         : tr("未连接，命令未发送");
        notify(tr("提示"), message, error.isEmpty() ? NotifyType::Warning : NotifyType::Error);
        return false;
    }
    if (accepted != targets.size()) {
        auto message = tr("仅 %1/%2 个发送目标已提交").arg(accepted).arg(targets.size());
        if (!error.isEmpty())
            message += u'\n' + error;
        notify(tr("提示"), message, NotifyType::Warning);
        return false;
    }
    return true;
}

void RuntimeContext::writeModbusSlaves(const HmiWidget *widget, double value, bool sendsCommand)
{
    if (sendsCommand)
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
        const auto slaves = m_hub->modbusSlaves(route);
        if (slaves.size() != 1)
            continue;
        ModbusSlave *slave = slaves.first();
        if (binding.mbSlaveFilter < 0 || slave->settings().slave == binding.mbSlaveFilter)
            slave->writeValue(table, binding.mbRegIndex, value, binding.mbDataType, binding.mbRegOrder);
    }
}

void RuntimeContext::notify(const QString &title, const QString &content, const QString &type, int durationMs)
{
    emit notificationRequested(title, content, type, durationMs);
}

void RuntimeContext::onUnit(const QByteArray &bytes, const Message &message, const DataSource &source)
{
    if (!m_active)
        return;
    // A frame and its decoded fields are one update, even when points use different binding modes.
    if (!bytes.isEmpty())
        applyRaw(bytes, source, message.isEmpty());
    if (!message.isEmpty()) {
        applyMessage(message, source);
        m_data->evaluate();
    }
}

void RuntimeContext::applyMessage(const Message &message, const DataSource &source)
{
    for (const BindingEntry &binding : m_bindings) {
        if (!binding.usesParsedData() || !binding.route().accepts(source) ||
            (source.canText && !binding.projectPoint.isEmpty())) {
            continue;
        }
        if (!binding.projectPoint.isEmpty() && binding.parseMode == ParseMode::Modbus &&
            binding.mbSlaveFilter < 0 && !source.slaveData) {
            // A point using the default unit must read back from the same unit its poll requests target.
            if (auto *link = m_hub->sourceLink(source);
                link && message.value(QStringLiteral("slave")).toInt() != m_poller->settings(link).fallbackSlave)
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
}

void RuntimeContext::applyRaw(const QByteArray &bytes, const DataSource &source, bool evaluateSamples)
{
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
            if (evaluateSamples)
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
    if (!message.isEmpty())
        applyMessage(message, source);
    m_data->evaluate();
}

void RuntimeContext::push(const BindingEntry &binding, const QVariant &raw, const DataSource &source)
{
    if (!binding.projectPoint.isEmpty()) {
        const bool present = raw.isValid() && !raw.isNull();
        const auto value = present ? binding.evaluateStrict(raw) : QVariant();
        const auto quality = !present ? DataQuality::Invalid
            : value.isValid() ? DataQuality::Current : DataQuality::ConversionFailed;
        m_data->setSample(binding.projectPoint, value, source, quality, {},
                          quality == DataQuality::ConversionFailed ? binding.expression : QString());
        return;
    }
    const QPointer<HmiWidget> widget = m_widgets.value(binding.widgetId);
    if (widget) {
        if (!raw.isValid() || raw.isNull()) {
            if (binding.parseMode == ParseMode::DataPoint) {
                if (widget->bindingStatus() == HmiWidget::BindingStatus::Live)
                    widget->setBindingStatus(HmiWidget::BindingStatus::Stale);
            } else {
                widget->applyBoundValue({}, binding);
            }
            return;
        }
        if (source.stream)
            m_widgetStreams.insert(binding.widgetId, source.stream);
        widget->setBindingStatus(HmiWidget::BindingStatus::Live);
        widget->setBindingProblem({});
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
