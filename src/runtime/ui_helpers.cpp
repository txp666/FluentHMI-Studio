#include "runtime/ui_helpers.h"

#include "core/i18n.h"
#include "core/protocol_parser.h"

#include <FluentQtWidgets/Dialogs/ColorDialog.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>

#include <FluentQtWidgets/StyleSheet.h>
#include <FluentQtWidgets/Widgets/ToolTip.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtGui/QImageReader>
#include <QtGui/QPixmap>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QGraphicsProxyWidget>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QScrollBar>

#include <algorithm>
#include <cmath>

namespace fhmi {

namespace {

class SquareFollower : public QObject
{
  public:
    SquareFollower(QAbstractButton *button, QWidget *field) : QObject(button), m_button(button), m_field(field)
    {
        field->installEventFilter(this);
    }

    bool eventFilter(QObject *, QEvent *event) override
    {
        if ((event->type() == QEvent::Resize || event->type() == QEvent::Show) && m_button && m_field) {
            const int side = m_field->height();
            if (side > 0 && m_button->size() != QSize(side, side)) {
                m_button->setFixedSize(side, side);
            }
        }
        return false;
    }

  private:
    QPointer<QAbstractButton> m_button;
    QPointer<QWidget> m_field;
};

class HorizontalWheelFilter : public QObject
{
  public:
    explicit HorizontalWheelFilter(QAbstractScrollArea *area) : QObject(area), m_area(area)
    {
        if (area && area->viewport()) {
            area->viewport()->installEventFilter(this);
        }
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!m_area || watched != m_area->viewport() || event->type() != QEvent::Wheel) {
            return false;
        }
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (!(wheel->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
            return false;
        }
        QScrollBar *hbar = m_area->horizontalScrollBar();
        if (!hbar || hbar->maximum() <= hbar->minimum()) {
            wheel->accept();
            return true;
        }
        const QPoint pixel = wheel->pixelDelta();
        const QPoint angle = wheel->angleDelta();
        int step = 0;
        if (!pixel.isNull()) {
            step = pixel.y() != 0 ? pixel.y() : pixel.x();
        } else {
            const int delta = angle.y() != 0 ? angle.y() : angle.x();
            if (delta != 0) {
                const int unit = std::max(60, hbar->singleStep() * 3);
                step = int(std::lround(double(delta) / 120.0 * double(unit)));
                if (step == 0) {
                    step = delta > 0 ? 1 : -1;
                }
            }
        }
        if (step != 0) {
            hbar->setValue(std::clamp(hbar->value() - step, hbar->minimum(), hbar->maximum()));
        }
        wheel->accept();
        return true;
    }

  private:
    QPointer<QAbstractScrollArea> m_area;
};

} // namespace

void keepSquareBeside(QAbstractButton *button, QWidget *field)
{
    const QString compact =
        QStringLiteral("QToolButton[fqw=\"ToolButton\"] { padding: 0px; min-width: 0px; min-height: 0px; }");
    FluentQt::FluentStyleSheet::setCustomStyleSheet(button, compact, compact);
    new SquareFollower(button, field);
}

void fluentToolTip(QWidget *widget, const QString &text)
{
    if (!text.isEmpty()) {
        widget->setToolTip(text);
    }
    if (!widget->findChild<FluentQt::ToolTipFilter *>(Qt::FindDirectChildrenOnly))
        widget->installEventFilter(new FluentQt::ToolTipFilter(widget));
}

QWidget *visibleWindow(QWidget *widget)
{
    if (!widget) {
        return nullptr;
    }
    QWidget *host = widget->window();
    if (!host) {
        return widget;
    }
    // The standalone player embeds the runtime canvas inside a QGraphicsProxyWidget.
    if (const auto *proxy = host->graphicsProxyWidget(); proxy && proxy->scene()) {
        const auto views = proxy->scene()->views();
        if (!views.isEmpty()) {
            return views.first()->window();
        }
    }
    return host;
}

void enableHorizontalWheelScroll(QAbstractScrollArea *area)
{
    if (area && area->viewport()) {
        new HorizontalWheelFilter(area);
    }
}

QColor chooseColor(const QColor &initial, const QString &title, QWidget *parent)
{
    FluentQt::ColorDialog dialog(initial.isValid() ? initial : QColor(Qt::white), title, visibleWindow(parent));
    dialog.yesButton()->setText(QCoreApplication::translate("fhmi", "确定"));
    dialog.yesButton()->setDefault(true);
    dialog.yesButton()->setFocus();
    dialog.cancelButton()->setText(QCoreApplication::translate("fhmi", "取消"));
    return dialog.exec() == QDialog::Accepted ? dialog.color() : QColor();
}

QIcon iconFromFile(const QString &path)
{
    QIcon icon;
    if (path.isEmpty()) {
        return icon;
    }
    QImageReader reader(path);
    for (int i = 0; i < std::max(1, reader.imageCount()); ++i) {
        if (i > 0 && !reader.jumpToImage(i)) {
            break;
        }
        const QImage image = reader.read();
        if (!image.isNull()) {
            icon.addPixmap(QPixmap::fromImage(image));
        }
    }
    return icon;
}

void addValueItems(FluentQt::ComboBox *combo, const QStringList &values)
{
    for (const QString &value : values) {
        combo->addItem(translated(value), QIcon(), value);
    }
}

void showInheritedParser(FluentQt::ComboBox *combo, const QString &projectType)
{
    const int index = combo->findData(LinkParser::Inherit);
    if (index < 0)
        return;
    QString name = projectType;
    if (projectType == QLatin1String("modbus"))
        name = QStringLiteral("Modbus RTU");
    else if (projectType == QLatin1String("modbus_tcp"))
        name = QStringLiteral("Modbus TCP");
    else if (projectType == QLatin1String("modbus_ascii"))
        name = QStringLiteral("Modbus ASCII");
    else if (projectType == QLatin1String("line"))
        name = QCoreApplication::translate("fhmi", "文本");
    combo->setItemText(index, QCoreApplication::translate("fhmi", "跟随工程（%1）").arg(name));
}

QString selectedValue(const FluentQt::ComboBox *combo)
{
    return combo->currentIndex() < 0 ? QString() : combo->currentData().toString();
}

bool selectValue(FluentQt::ComboBox *combo, const QString &value)
{
    const int index = combo->findData(value);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    }
    return index >= 0;
}

bool ConnectionErrorNotice::shouldShow(const QString &message, int durationMs)
{
    if (m_shownAt.isValid() && m_message == message && m_shownAt.elapsed() < durationMs)
        return false;
    m_message = message;
    m_shownAt.start();
    return true;
}

} // namespace fhmi
