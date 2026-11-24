#include "runtime/widgets/s7_config_card.h"
#include "runtime/ui_helpers.h"
#include "runtime/widgets/display_widgets.h"
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QGridLayout>

namespace fhmi {
S7ConfigCardWidget::S7ConfigCardWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    m_layout = new QGridLayout(this);
    m_layout->setContentsMargins(12, 8, 12, 8);
    m_layout->setSpacing(4);
    m_layout->setSizeConstraint(QLayout::SetNoConstraint);
    m_title = new FluentQt::StrongBodyLabel(this);
    m_host = new FluentQt::LineEdit(this);
    m_host->setObjectName(QStringLiteral("s7Host"));
    m_port = new FluentQt::SpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setObjectName(QStringLiteral("s7Port"));
    m_rack = new FluentQt::SpinBox(this);
    m_rack->setRange(0, 7);
    m_slot = new FluentQt::SpinBox(this);
    m_slot->setRange(0, 31);
    m_type = new FluentQt::ComboBox(this);
    m_type->addItems({QStringLiteral("PG"), QStringLiteral("OP"), QStringLiteral("S7 Basic")});
    for (const auto &label : {tr("地址"), tr("端口"), tr("机架"), tr("插槽"), tr("连接类型")})
        m_labels.append(new FluentQt::CaptionLabel(label, this));
    m_status = new FluentQt::CaptionLabel(tr("S7 未连接"), this);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setWordWrap(false);
    m_status->setObjectName(QStringLiteral("s7Status"));
    fluentToolTip(m_status);
    m_button = new FluentQt::PrimaryPushButton(tr("连接"), this);
    m_button->setMinimumWidth(96);
    m_button->setObjectName(QStringLiteral("s7ConnectButton"));
    connect(m_button, &QPushButton::clicked, this, &S7ConfigCardWidget::toggle);
    updateMinimumHeight();
}
S7ConfigCardWidget::~S7ConfigCardWidget() { detach(); }
void S7ConfigCardWidget::apply()
{
    m_title->setText(text("title"));
    m_host->setText(text("host"));
    m_port->setValue(integer("port"));
    m_rack->setValue(integer("rack"));
    m_slot->setValue(integer("slot"));
    m_type->setCurrentIndex(integer("connectionType") - 1);
    refresh();
}
void S7ConfigCardWidget::refresh()
{
    for (auto *field : QList<QWidget *>{m_host, m_port, m_rack, m_slot, m_type})
        field->setEnabled(!m_open);
    m_button->setText(m_open ? tr("断开") : tr("连接"));
    m_button->setEnabled(isLive());
}
void S7ConfigCardWidget::resizeEvent(QResizeEvent *event)
{
    HmiWidget::resizeEvent(event);
    updateMinimumHeight();
}
void S7ConfigCardWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkTheme();
    painter.setPen(dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 18));
    painter.setBrush(dark ? QColor(255, 255, 255, 13) : QColor(255, 255, 255, 200));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}
void S7ConfigCardWidget::arrangeForWidth(int width)
{
    while (auto *item = m_layout->takeAt(0))
        delete item;
    for (int i = 0; i < 6; ++i) {
        m_layout->setColumnStretch(i, 0);
        m_layout->setRowStretch(i, 0);
    }
    const bool wide = width >= 760;
    if (wide) {
        m_layout->addWidget(m_title, 0, 0, 1, 2);
        m_layout->addWidget(m_status, 0, 2, 1, 4);
        const QList<QWidget *> fields{m_host, m_port, m_rack, m_slot, m_type};
        for (int i = 0; i < fields.size(); ++i) {
            m_layout->addWidget(m_labels.at(i), 1, i);
            m_layout->addWidget(fields.at(i), 2, i);
        }
        m_layout->addWidget(m_button, 2, 5);
        m_layout->setColumnStretch(0, 1);
        m_layout->setRowStretch(3, 1);
    } else {
        m_layout->addWidget(m_title, 0, 0, 1, 4);
        m_layout->addWidget(m_labels.at(0), 1, 0);
        m_layout->addWidget(m_host, 1, 1, 1, 3);
        m_layout->addWidget(m_labels.at(1), 2, 0);
        m_layout->addWidget(m_port, 2, 1);
        m_layout->addWidget(m_labels.at(4), 2, 2);
        m_layout->addWidget(m_type, 2, 3);
        m_layout->addWidget(m_labels.at(2), 3, 0);
        m_layout->addWidget(m_rack, 3, 1);
        m_layout->addWidget(m_labels.at(3), 3, 2);
        m_layout->addWidget(m_slot, 3, 3);
        m_layout->addWidget(m_status, 4, 0, 1, 3);
        m_layout->addWidget(m_button, 4, 3);
        m_layout->setColumnStretch(1, 1);
        m_layout->setColumnStretch(3, 1);
        m_layout->setRowStretch(5, 1);
    }
}
void S7ConfigCardWidget::onAttached()
{
    live(connect(context()->s7Client(), &S7Client::statusChanged, this,
                 [this](const QString &source, bool connected, const QString &status) {
                     if (source != id())
                         return;
                     m_status->setText(status);
                     m_status->setToolTip(QString());
                     refresh();
                     if (connected)
                         m_errorNotice.clear();
                 }));
    live(connect(context()->s7Client(), &S7Client::errorOccurred, this,
                 [this](const QString &source, const QString &reason) {
                     if (source == id())
                         showFailure(reason);
                 }));
    QString error;
    if (!S7::config(properties(), &error)) {
        m_open = false;
        showFailure(error);
        refresh();
        return;
    }
    m_open = flag("autoConnect");
    context()->s7Client()->configure(id(), properties());
    refresh();
}
void S7ConfigCardWidget::onDetached()
{
    if (context())
        context()->s7Client()->remove(id());
    m_open = false;
    refresh();
}
void S7ConfigCardWidget::toggle()
{
    if (!context())
        return;
    if (m_open) {
        m_open = false;
        context()->s7Client()->close(id());
    } else {
        QJsonObject props = meta().defaultProperties();
        for (auto it = properties().begin(); it != properties().end(); ++it)
            props.insert(it.key(), it.value());
        props.insert(QStringLiteral("host"), m_host->text());
        props.insert(QStringLiteral("port"), m_port->value());
        props.insert(QStringLiteral("rack"), m_rack->value());
        props.insert(QStringLiteral("slot"), m_slot->value());
        props.insert(QStringLiteral("autoConnect"), false);
        props.insert(QStringLiteral("connectionType"), m_type->currentIndex() + 1);
        QString error;
        if (!S7::config(props, &error)) {
            showFailure(error);
            return;
        }
        context()->s7Client()->configure(id(), props);
        m_open = true;
        context()->s7Client()->open(id());
    }
    refresh();
}
void S7ConfigCardWidget::showFailure(const QString &reason)
{
    if (!flag("autoReconnect")) {
        m_open = false;
        refresh();
    }
    m_status->setText(tr("连接失败"));
    m_status->setToolTip(reason);
    if (context() && m_errorNotice.shouldShow(reason, 4000))
        context()->notify(text("title"), reason, NotifyType::Error, 4000);
}
} // namespace fhmi
