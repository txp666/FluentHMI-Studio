#pragma once

#include "core/project.h"

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

namespace fhmi {

class HmiWidget;
class RuntimeContext;

// A project running for real: widgets at their designed positions, attached to a
// context whose hub talks to devices. The studio's preview page and every exported
// application show one of these.
class RuntimeSurface : public QWidget
{
    Q_OBJECT

  public:
    explicit RuntimeSurface(QWidget *parent = nullptr);
    ~RuntimeSurface() override;

    // Replaces whatever ran before. Widgets that fail to build are skipped.
    void build(const Project &project);
    void clear();

    RuntimeContext *context() const { return m_context; }
    QList<HmiWidget *> widgets() const;

  signals:
    void notificationRequested(const QString &title, const QString &content, const QString &type, int durationMs);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    void buildWidget(const QJsonObject &data, HmiWidget *container);

    RuntimeContext *m_context = nullptr;
    QList<QPointer<HmiWidget>> m_widgets;
    QString m_assetBaseDir;
};

} // namespace fhmi
