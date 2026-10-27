#pragma once

#include "runtime/hmi_widget.h"

class QTabWidget;

namespace fhmi {

// Children keep the geometry the designer saved, relative to their container. Layout
// containers (HBox, VBox, Flex, non-free cards and tab pages) are arranged by the
// designer, which writes the resulting positions into the project; at run time every
// container simply places children where the project says.
namespace ContainerLayout {
constexpr int Padding = 8;
constexpr int TitleHeight = 22;
constexpr int GroupTitleHeight = 28;
} // namespace ContainerLayout

// HBoxLayout, VBoxLayout and FlexLayout: a dashed frame with a small caption.
class BoxContainerWidget : public HmiWidget
{
  public:
    BoxContainerWidget(const WidgetMeta &meta, QWidget *parent);
    QWidget *childHost(int) override { return this; }
    int contentTop() const override { return ContainerLayout::TitleHeight; }

  protected:
    void apply() override { update(); }
    void paintEvent(QPaintEvent *event) override;
};

class GroupBoxWidget : public HmiWidget
{
  public:
    GroupBoxWidget(const WidgetMeta &meta, QWidget *parent);
    QWidget *childHost(int) override { return this; }
    int contentTop() const override { return ContainerLayout::GroupTitleHeight; }

  protected:
    void apply() override { update(); }
    void paintEvent(QPaintEvent *event) override;
};

class CardContainerWidget : public HmiWidget
{
  public:
    CardContainerWidget(const WidgetMeta &meta, QWidget *parent);
    QWidget *childHost(int) override { return this; }
    int contentTop() const override;

  protected:
    void apply() override { update(); }
    void paintEvent(QPaintEvent *event) override;
};

class TabContainerWidget : public HmiWidget
{
  public:
    TabContainerWidget(const WidgetMeta &meta, QWidget *parent);
    QWidget *childHost(int tabIndex) override;
    int currentPage() const override;
    int contentTop() const override;
    int pageCount() const;

  protected:
    void apply() override;

  private:
    void restyle();

    QTabWidget *m_tabs = nullptr;
};

class SeparatorWidget : public HmiWidget
{
  public:
    SeparatorWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override { update(); }
    void paintEvent(QPaintEvent *event) override;
};

// Keeps children at their container-relative geometry while a page's own position
// inside the container settles (tab pages are laid out lazily, on first show).
class ChildGeometryKeeper : public QObject
{
    Q_OBJECT

  public:
    explicit ChildGeometryKeeper(QWidget *container);

    void place(QWidget *child, QWidget *host, const QRect &geometry);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void schedule();
    void applyAll();

    QWidget *m_container;
    QList<QPair<QPointer<QWidget>, QRect>> m_children;
    bool m_pending = false;
};

} // namespace fhmi
