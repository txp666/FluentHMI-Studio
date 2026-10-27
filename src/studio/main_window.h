#pragma once

#include <FluentQtWidgets/Window/FluentWindow.h>

namespace fhmi {

class DesignerPage;
class PreviewPage;
class SettingsPage;
class UpdateController;

class MainWindow : public FluentQt::FluentWindow
{
    Q_OBJECT

  public:
    explicit MainWindow(QWidget *parent = nullptr);

    DesignerPage *designer() const { return m_designer; }
    PreviewPage *preview() const { return m_preview; }
    void openProject(const QString &path);

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    void exportApplication();
    void updateTitle();

    DesignerPage *m_designer = nullptr;
    PreviewPage *m_preview = nullptr;
    SettingsPage *m_settings = nullptr;
    UpdateController *m_updates = nullptr;
    // Set while closing to restart into a downloaded update.
    bool m_restartForUpdate = false;
};

} // namespace fhmi
