#pragma once

#include <QtWidgets/QWidget>

namespace FluentQt {
class PushSettingCard;
}

namespace fhmi {

class AiService;

class SettingsPage : public QWidget
{
    Q_OBJECT

  public:
    explicit SettingsPage(QWidget *parent = nullptr);

  public slots:
    void setUpdateChecking(bool checking);

  signals:
    void checkUpdateRequested();

  private:
    FluentQt::PushSettingCard *m_checkUpdate = nullptr;
    AiService *m_aiTestService = nullptr;
};

} // namespace fhmi
