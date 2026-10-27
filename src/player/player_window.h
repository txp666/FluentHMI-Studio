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
    explicit PlayerWindow(const Project &project, QWidget *parent = nullptr);

    RuntimeSurface *surface() const { return m_surface; }

  private:
    RuntimeSurface *m_surface = nullptr;
};

} // namespace fhmi
