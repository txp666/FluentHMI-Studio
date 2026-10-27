#pragma once

#include "core/project.h"

#include <QtWidgets/QWidget>

namespace FluentQt {
class CaptionLabel;
class PrimaryPushButton;
class PushButton;
} // namespace FluentQt

namespace fhmi {

class RuntimeSurface;

// Runs the current design against real devices, with the same runtime that exported
// applications use.
class PreviewPage : public QWidget
{
    Q_OBJECT

  public:
    explicit PreviewPage(QWidget *parent = nullptr);

    void setProject(const Project &project);
    // The design changed: stop a preview that would otherwise run the old one.
    void invalidate();
    bool isRunning() const { return m_running; }

  signals:
    void exportRequested();

  private:
    void start();
    void stop();
    void showNotification(const QString &title, const QString &content, const QString &type, int durationMs);

    Project m_project;
    bool m_running = false;
    RuntimeSurface *m_surface = nullptr;
    FluentQt::PrimaryPushButton *m_run = nullptr;
    FluentQt::PushButton *m_export = nullptr;
    FluentQt::CaptionLabel *m_status = nullptr;
};

} // namespace fhmi
