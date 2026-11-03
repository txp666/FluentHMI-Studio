#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/StyleSheet.h>

#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtGui/QImageReader>
#include <QtGui/QPixmap>
#include <QtWidgets/QAbstractButton>

#include <algorithm>

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

} // namespace fhmi
