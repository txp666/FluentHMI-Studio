#pragma once

#include "core/project.h"

#include <FluentQtWidgets/Window/FluentWindow.h>

namespace fhmi {

class RuntimeSurface;

// The window of an exported application: the project's surface under a Fluent title bar.
class PlayerWindow : public FluentQt::FluentWidget
{
    Q_OBJECT

  public:
    explicit PlayerWindow(const Project &project, QWidget *parent = nullptr, bool startRuntime = true);

    RuntimeSurface *surface() const { return m_surface; }

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    RuntimeSurface *m_surface = nullptr;
};

} // namespace fhmi
