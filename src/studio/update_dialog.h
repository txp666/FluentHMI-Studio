#pragma once

#include "studio/update_release.h"

#include <FluentQtWidgets/Dialogs/Dialog.h>

namespace FluentQt {
class CaptionLabel;
class ProgressBar;
class PushButton;
} // namespace FluentQt

namespace fhmi {

class UpdateInstaller;
class UpdateService;

// Offers a newer release and, when this installation can replace itself, downloads,
// verifies and unpacks it while the dialog stays open. Accepted means the new version is
// unpacked and the user chose to restart into it now.
class UpdateDialog : public FluentQt::MessageBoxBase
{
    Q_OBJECT

  public:
    UpdateDialog(const ReleaseInfo &release, UpdateService *service, UpdateInstaller *installer, bool offerSkip,
                 QWidget *parent = nullptr);

  signals:
    void skipRequested();

  private:
    enum class State
    {
        Offer,
        Working,
        Ready,
        Failed
    };
    void proceed();
    void setState(State state, const QString &status = QString());

    ReleaseInfo m_release;
    UpdateService *m_service = nullptr;
    UpdateInstaller *m_installer = nullptr;
    QString m_cannotInstall;
    State m_state = State::Offer;
    FluentQt::ProgressBar *m_progress = nullptr;
    FluentQt::CaptionLabel *m_status = nullptr;
    FluentQt::PushButton *m_skip = nullptr;
};

} // namespace fhmi
