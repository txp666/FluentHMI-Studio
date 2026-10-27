#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/StyleSheet.h>

#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtWidgets/QAbstractButton>

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

} // namespace

void keepSquareBeside(QAbstractButton *button, QWidget *field)
{
    const QString compact =
        QStringLiteral("QToolButton[fqw=\"ToolButton\"] { padding: 0px; min-width: 0px; min-height: 0px; }");
    FluentQt::FluentStyleSheet::setCustomStyleSheet(button, compact, compact);
    new SquareFollower(button, field);
}

} // namespace fhmi
