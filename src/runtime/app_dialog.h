#pragma once

#include <FluentQtWidgets/Dialogs/Dialog.h>

#include <QtCore/QSize>

#include <functional>

namespace fhmi {

// Application forms share the same Fluent surface and actions as the update dialog.
class AppDialog : public FluentQt::MessageBoxBase
{
    Q_OBJECT

  public:
    explicit AppDialog(const QString &title, QWidget *parent = nullptr);

    void setPreferredSize(const QSize &size);
    void setValidator(std::function<bool()> validator);

  public slots:
    void open() override;

  protected:
    bool validate() const override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    void updatePanelSize();

    QSize m_preferredSize{640, 480};
    std::function<bool()> m_validator;
};

} // namespace fhmi
