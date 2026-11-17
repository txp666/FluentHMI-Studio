#include "runtime/widgets/container_widgets.h"

#include "runtime/widgets/display_widgets.h"

#include <FluentQtWidgets/Navigation/Pivot.h>
#include <FluentQtWidgets/Navigation/SegmentedWidget.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <FluentQtWidgets/Widgets/TabView.h>

#include <QtCore/QEvent>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtWidgets/QGraphicsEffect>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOption>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

namespace {

class PageTabWidget : public QTabWidget
{
  public:
    using QTabWidget::QTabWidget;

    int pageTop() const
    {
        // QTabWidget delays positioning hidden pages until show(). Ask the same style
        // sub-element it uses, so an unshown designer item has the same content origin.
        QStyleOptionTabWidgetFrame option;
        initStyleOption(&option);
        return style()->subElementRect(QStyle::SE_TabWidgetTabContents, &option, this).top();
    }
};

void repaintOnThemeChange(QWidget *widget)
{
    QObject::connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged, widget,
                     [widget]() { widget->update(); });
}

QColor captionColor()
{
    return isDarkTheme() ? QColor(0xb3, 0xb3, 0xb3) : QColor(0x60, 0x60, 0x60);
}

} // namespace

// ---------------------------------------------------------------------------
// Box containers
// ---------------------------------------------------------------------------

BoxContainerWidget::BoxContainerWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    repaintOnThemeChange(this);
}

void BoxContainerWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkTheme();
    QPen pen(dark ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 36), 1.5, Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(dark ? QColor(255, 255, 255, 8) : QColor(0, 0, 0, 5));
    painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 6, 6);
    const QString title = text("title");
    if (!title.isEmpty()) {
        QFont font = this->font();
        font.setPointSize(std::max(8, font.pointSize() - 3));
        painter.setFont(font);
        painter.setPen(captionColor());
        painter.drawText(QRect(8, 3, width() - 16, ContainerLayout::TitleHeight - 6), Qt::AlignLeft | Qt::AlignVCenter,
                         title);
    }
}

// ---------------------------------------------------------------------------
// GroupBox
// ---------------------------------------------------------------------------

GroupBoxWidget::GroupBoxWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    repaintOnThemeChange(this);
}

void GroupBoxWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkTheme();
    const int top = 10;
    const QString title = text("title");
    QFont font = this->font();
    font.setBold(true);
    painter.setFont(font);
    const QRect label(10, 0, title.isEmpty() ? 0 : painter.fontMetrics().horizontalAdvance(title) + 12, 20);
    // Leave a gap in the border for the caption, whatever is behind the widget.
    painter.setClipRegion(QRegion(rect()).subtracted(QRegion(label)));
    painter.setPen(QPen(dark ? QColor(255, 255, 255, 45) : QColor(0, 0, 0, 40), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(0.5, top + 0.5, width() - 1, height() - top - 1), 6, 6);
    painter.setClipping(false);
    if (!title.isEmpty()) {
        painter.setPen(dark ? QColor(255, 255, 255) : QColor(30, 30, 30));
        painter.drawText(label, Qt::AlignCenter, title);
    }
}

// ---------------------------------------------------------------------------
// CardContainer
// ---------------------------------------------------------------------------

CardContainerWidget::CardContainerWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    repaintOnThemeChange(this);
}

int CardContainerWidget::contentTop() const
{
    return text("title").isEmpty() ? ContainerLayout::Padding : ContainerLayout::GroupTitleHeight;
}

void CardContainerWidget::paintEvent(QPaintEvent *)
{
    paintCardSurface(this, text("cardStyle"));
    QPainter painter(this);
    const bool dark = isDarkTheme();
    const QString title = text("title");
    if (!title.isEmpty()) {
        QFont font = this->font();
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(dark ? QColor(255, 255, 255) : QColor(30, 30, 30));
        painter.drawText(QRect(12, 4, width() - 24, 22), Qt::AlignLeft | Qt::AlignVCenter, title);
    }
}

// ---------------------------------------------------------------------------
// TabWidget
// ---------------------------------------------------------------------------

TabContainerWidget::TabContainerWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    // All navigation styles share these pages and their existing container-relative
    // geometry. The classic strip remains the default for old projects.
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_tabs = new PageTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("hmiTabs"));
    m_tabs->setDocumentMode(false);
    m_tabs->tabBar()->setDrawBase(false);
    m_tabs->tabBar()->setExpanding(false);
    layout->addWidget(m_tabs);
    connect(m_tabs, &QTabWidget::currentChanged, this, &TabContainerWidget::syncNavigation);
    restyle();
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged, this,
            &TabContainerWidget::restyle);
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::accentColorChanged, this,
            &TabContainerWidget::restyle);
}

void TabContainerWidget::restyle()
{
    const bool dark = isDarkTheme();
    const QString foreground = dark ? QStringLiteral("#f3f3f3") : QStringLiteral("#202020");
    const QString muted = dark ? QStringLiteral("#c8c8c8") : QStringLiteral("#5c5c5c");
    const QString border = dark ? QStringLiteral("rgba(255,255,255,0.14)") : QStringLiteral("rgba(0,0,0,0.14)");
    const QString tab = dark ? QStringLiteral("rgba(255,255,255,0.05)") : QStringLiteral("rgba(0,0,0,0.04)");
    const QString selected = dark ? QStringLiteral("#2d2d2d") : QStringLiteral("#ffffff");
    const QString pane = dark ? QStringLiteral("rgba(255,255,255,0.03)") : QStringLiteral("rgba(255,255,255,0.6)");
    m_tabs->setStyleSheet(QStringLiteral(
        "QTabWidget#hmiTabs::pane { border: 1px solid %1; border-radius: 6px; background: %6; top: -1px; }"
        "QTabWidget#hmiTabs > QTabBar::tab { color: %2; background: %3; padding: 6px 14px; margin-right: 2px;"
        " border: 1px solid transparent; border-top-left-radius: 5px; border-top-right-radius: 5px; }"
        "QTabWidget#hmiTabs > QTabBar::tab:selected { color: %4; background: %5; border-color: %1;"
        " border-bottom-color: %5; border-top: 2px solid %7; }"
        "QTabWidget#hmiTabs > QTabBar::tab:hover:!selected { color: %4; }"
        "QTabWidget#hmiTabs > QWidget > QWidget { background: transparent; }")
                              .arg(border, muted, tab, foreground, selected, pane, accentColor().name()));
}

void TabContainerWidget::apply()
{
    const QStringList names = tabNames(text("tabNames"));
    while (m_tabs->count() > names.size()) {
        QWidget *page = m_tabs->widget(m_tabs->count() - 1);
        m_tabs->removeTab(m_tabs->count() - 1);
        // Children on a removed page move to the last remaining one.
        if (QWidget *last = m_tabs->widget(m_tabs->count() - 1)) {
            for (QWidget *child : page->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
                child->setParent(last);
                child->show();
            }
        }
        page->deleteLater();
    }
    for (int i = 0; i < names.size(); ++i) {
        if (i < m_tabs->count()) {
            m_tabs->setTabText(i, names.at(i));
        } else {
            auto *page = new QWidget;
            page->setObjectName(QStringLiteral("tabPage%1").arg(i));
            page->setAttribute(Qt::WA_TranslucentBackground);
            m_tabs->addTab(page, names.at(i));
        }
    }
    m_tabs->setCurrentIndex(clampTabIndex(integer("currentTab"), m_tabs->count()));
    updateNavigation(names);
    // The designer reads this immediately after changing navigationStyle to keep each
    // free-layout child's distance from its page's content edge unchanged.
    contentTop();
}

void TabContainerWidget::updateNavigation(const QStringList &names)
{
    QString style = text("navigationStyle");
    if (style != QStringLiteral("下划线导航") && style != QStringLiteral("分段导航") && // i18n: value
        style != QStringLiteral("文档页签")) // i18n: value
        style = QStringLiteral("经典选项卡"); // i18n: value
    if (style == m_navigationStyle && names == m_pageNames) {
        syncNavigation(m_tabs->currentIndex());
        return;
    }

    delete m_navigation;
    m_navigation = nullptr;
    m_pivot = nullptr;
    m_navigationScroll = nullptr;
    m_documentTabs = nullptr;
    m_navigationStyle = style;
    m_pageNames = names;
    const bool classic = style == QStringLiteral("经典选项卡"); // i18n: value
    m_tabs->tabBar()->setVisible(classic);
    if (classic)
        return;

    if (style == QStringLiteral("文档页签")) { // i18n: value
        m_documentTabs = new FluentQt::TabBar(this);
        m_documentTabs->setObjectName(QStringLiteral("hmiDocumentTabs"));
        m_documentTabs->setTabsClosable(false);
        m_documentTabs->setAddButtonVisible(false);
        m_documentTabs->setMovable(false);
        m_documentTabs->setTabShadowEnabled(false);
        // Natural short names fit together, while long names have a bounded width and
        // use the component's own horizontal scrolling instead of widening the canvas.
        int tabWidth = 96;
        for (const QString &name : names)
            tabWidth = std::max(tabWidth, m_documentTabs->fontMetrics().horizontalAdvance(name) + 48);
        m_documentTabs->setTabMaximumWidth(std::min(tabWidth, 200));
        m_documentTabs->setScrollable(true);
        for (int i = 0; i < names.size(); ++i) {
            m_documentTabs->addTab(QString::number(i), names.at(i));
            // HMI also embeds these widgets in a scaled QGraphicsProxyWidget.
            // An active QWidget graphics effect makes the tab disappear there,
            // even when the component has made its shadow fully transparent.
            if (auto *effect = m_documentTabs->tabItem(i)->graphicsEffect())
                effect->setEnabled(false);
        }
        connect(m_documentTabs, &FluentQt::TabBar::currentChanged, m_tabs, &QTabWidget::setCurrentIndex);
        m_navigation = m_documentTabs;
    } else {
        m_navigationScroll = new FluentQt::SingleDirectionScrollArea(Qt::Horizontal, this);
        m_navigationScroll->setObjectName(QStringLiteral("hmiNavigationScroll"));
        m_navigationScroll->enableTransparentBackground();
        m_pivot = style == QStringLiteral("分段导航") // i18n: value
                      ? new FluentQt::SegmentedWidget(m_navigationScroll)
                      : new FluentQt::Pivot(m_navigationScroll);
        m_pivot->setObjectName(QStringLiteral("hmiPageNavigation"));
        for (int i = 0; i < names.size(); ++i)
            m_pivot->addItem(QString::number(i), names.at(i));
        m_pivot->ensurePolished();
        m_navigationScroll->setWidget(m_pivot);
        m_navigationScroll->setFixedHeight(m_pivot->sizeHint().height());
        connect(m_pivot, &FluentQt::Pivot::currentItemChanged, this, [this](const QString &key) {
            m_tabs->setCurrentIndex(key.toInt());
        });
        m_navigation = m_navigationScroll;
    }
    m_navigation->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    static_cast<QVBoxLayout *>(layout())->insertWidget(0, m_navigation);
    m_navigation->show();
    syncNavigation(m_tabs->currentIndex());
}

void TabContainerWidget::syncNavigation(int index)
{
    if (index < 0 || index >= m_pageNames.size())
        return;
    if (m_documentTabs) {
        const QSignalBlocker blocker(m_documentTabs);
        m_documentTabs->setCurrentIndex(index);
        // The component's tab strip settles after resize/show; keep the selected
        // page reachable even when the saved current page starts outside the viewport.
        QTimer::singleShot(0, m_documentTabs, [tabs = m_documentTabs]() {
            if (auto *scroll = tabs->findChild<QScrollArea *>())
                scroll->ensureWidgetVisible(tabs->currentTab(), 8, 0);
        });
    } else if (m_pivot) {
        const QSignalBlocker blocker(m_pivot);
        m_pivot->setCurrentItem(QString::number(index));
        m_navigationScroll->ensureWidgetVisible(m_pivot->item(QString::number(index)), 8, 0);
    }
}

QWidget *TabContainerWidget::childHost(int tabIndex)
{
    return m_tabs->widget(clampTabIndex(tabIndex, m_tabs->count()));
}

int TabContainerWidget::currentPage() const
{
    return m_tabs->currentIndex();
}

int TabContainerWidget::pageCount() const
{
    return m_tabs->count();
}

int TabContainerWidget::navigationPageAt(const QPoint &position) const
{
    if (!m_navigation) {
        auto *bar = m_tabs->tabBar();
        return bar->tabAt(bar->mapFrom(this, position));
    }
    if (!m_navigation->rect().contains(m_navigation->mapFrom(this, position)))
        return -1;
    for (int i = 0; i < m_pageNames.size(); ++i) {
        QWidget *tab = m_documentTabs ? static_cast<QWidget *>(m_documentTabs->tabItem(i))
                                      : static_cast<QWidget *>(m_pivot->item(QString::number(i)));
        if (tab && tab->visibleRegion().contains(tab->mapFrom(this, position)))
            return i;
    }
    return -1;
}

int TabContainerWidget::contentTop() const
{
    // Settle the outer navigation layout synchronously; pageTop() also works before
    // Qt has positioned an unshown page. Runtime geometry follows the same style rect.
    if (layout())
        layout()->activate();
    const int top = m_tabs->y() + static_cast<PageTabWidget *>(m_tabs)->pageTop();
    return top > 0 ? top : 33; // where the page starts in PyQt UI Designer
}

// ---------------------------------------------------------------------------
// Separator
// ---------------------------------------------------------------------------

SeparatorWidget::SeparatorWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent) {}

void SeparatorWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setPen(QPen(QColor(128, 128, 128, 90), 1));
    if (text("orientation") == QLatin1String("vertical")) {
        painter.drawLine(width() / 2, 0, width() / 2, height());
    } else {
        painter.drawLine(0, height() / 2, width(), height() / 2);
    }
}

// ---------------------------------------------------------------------------
// ChildGeometryKeeper
// ---------------------------------------------------------------------------

ChildGeometryKeeper::ChildGeometryKeeper(QWidget *container) : QObject(container), m_container(container)
{
    container->installEventFilter(this);
}

void ChildGeometryKeeper::place(QWidget *child, QWidget *host, const QRect &geometry)
{
    child->setParent(host);
    m_children.append({child, geometry});
    if (host != m_container) {
        host->installEventFilter(this);
    }
    applyAll();
    schedule();
}

bool ChildGeometryKeeper::eventFilter(QObject *, QEvent *event)
{
    switch (event->type()) {
    case QEvent::Resize:
    case QEvent::Move:
    case QEvent::Show:
    case QEvent::LayoutRequest:
    case QEvent::StyleChange:
    case QEvent::FontChange:
        schedule();
        break;
    default:
        break;
    }
    return false;
}

void ChildGeometryKeeper::schedule()
{
    if (!m_pending) {
        m_pending = true;
        QTimer::singleShot(0, this, [this]() { applyAll(); });
    }
}

void ChildGeometryKeeper::applyAll()
{
    m_pending = false;
    for (auto &entry : m_children) {
        QWidget *child = entry.first;
        if (!child || !child->parentWidget()) {
            continue;
        }
        const QPoint origin = child->parentWidget() == m_container ? QPoint()
                                                                   : child->parentWidget()->mapTo(m_container, QPoint());
        child->setGeometry(entry.second.translated(-origin));
    }
}

} // namespace fhmi
