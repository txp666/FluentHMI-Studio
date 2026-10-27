#pragma once

#include "studio/update_release.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>

class QWidget;

namespace fhmi {

class UpdateInstaller;
class UpdateService;

// The studio's side of updating: the check at startup and from the settings page, the
// update dialog, and handing a prepared update to the installer when the studio exits.
class UpdateController : public QObject
{
    Q_OBJECT

  public:
    explicit UpdateController(QWidget *window);

    static bool checkAtStartup();
    static void setCheckAtStartup(bool enabled);

    // Reports an update that did not complete last time, then checks quietly if enabled.
    void startupCheck();
    // Checks and reports the outcome either way.
    void checkNow();
    bool isChecking() const { return m_checking; }

    // For the main window's close: starts the installer when an update is unpacked and
    // waiting. `relaunch` starts the new version afterwards.
    bool finishOnExit(bool relaunch, QString *error);

    // `FluentHMIStudio --update`: installs the latest release without a window.
    static int runHeadless();

  signals:
    void checkingChanged(bool checking);
    // The user chose to restart into a downloaded update now.
    void restartRequested();

  private:
    void check(bool manual);
    void offer(const ReleaseInfo &release, bool manual);

    QWidget *m_window = nullptr;
    UpdateService *m_service = nullptr;
    UpdateInstaller *m_installer = nullptr;
    bool m_checking = false;
    bool m_manual = false;
    QString m_preparedTag;
};

} // namespace fhmi
