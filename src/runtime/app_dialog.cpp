#include "runtime/app_dialog.h"

#include <FluentQtWidgets/Widgets/Label.h>

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>

#include <utility>

namespace fhmi {

AppDialog::AppDialog(const QString &title, QWidget *parent) : FluentQt::MessageBoxBase(parent)
{
    setWindowTitle(title);
    auto *heading = new FluentQt::SubtitleLabel(title, widget());
    heading->setWordWrap(true);
    viewLayout()->addWidget(heading);

    yesButton()->setObjectName(QStringLiteral("appDialogAccept"));
    yesButton()->setText(QCoreApplication::translate("fhmi", "确定"));
    yesButton()->setDefault(true);

    // Keep the library's cancelButton name: its dialog stylesheet uses that selector.
    cancelButton()->setText(QCoreApplication::translate("fhmi", "取消"));

    hBoxLayout()->setContentsMargins(24, 24, 24, 24);
    // A form's minimum-size hint must not force the overlay beyond its host window.
    hBoxLayout()->setSizeConstraint(QLayout::SetNoConstraint);
}

void AppDialog::setPreferredSize(const QSize &size)
{
    m_preferredSize = size;
    updatePanelSize();
}

void AppDialog::setValidator(std::function<bool()> validator) { m_validator = std::move(validator); }

bool AppDialog::validate() const { return !m_validator || m_validator(); }

void AppDialog::open()
{
    // QDialog::open() turns child dialogs into native sheets on macOS. Keep the Fluent
    // overlay in its host while retaining asynchronous accepted/rejected signals.
    setResult(QDialog::Rejected);
    show();
}

void AppDialog::showEvent(QShowEvent *event)
{
    if (!parentWidget()) {
        const QSize available = QGuiApplication::primaryScreen()->availableGeometry().size();
        resize((m_preferredSize + QSize(48, 48)).boundedTo(available));
    }
    FluentQt::MessageBoxBase::showEvent(event);
    updatePanelSize();
}

void AppDialog::resizeEvent(QResizeEvent *event)
{
    FluentQt::MessageBoxBase::resizeEvent(event);
    updatePanelSize();
}

void AppDialog::updatePanelSize()
{
    const QMargins margins = hBoxLayout()->contentsMargins();
    const QSize available = size() - QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    widget()->setFixedSize(m_preferredSize.boundedTo(available.expandedTo(QSize(1, 1))));
}

} // namespace fhmi
