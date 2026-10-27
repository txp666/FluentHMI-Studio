#pragma once

#include <QtWidgets/QWidget>

class QTreeWidget;

namespace FluentQt {
class SearchLineEdit;
} // namespace FluentQt

namespace fhmi {

// Every registered widget by category. Drag one onto the canvas, or double-click
// to drop it at the canvas centre.
class WidgetPalette : public QWidget
{
    Q_OBJECT

  public:
    explicit WidgetPalette(QWidget *parent = nullptr);

  signals:
    void widgetActivated(const QString &typeName);

  private:
    void filter(const QString &text);

    FluentQt::SearchLineEdit *m_search = nullptr;
    QTreeWidget *m_tree = nullptr;
};

} // namespace fhmi
