#include "runtime/widgets/container_widgets.h"

#include "runtime/widgets/display_widgets.h"

#include <FluentQtWidgets/Theme.h>

#include <QtCore/QEvent>
#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

namespace {

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
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkTheme();
    painter.setPen(dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 18));
    painter.setBrush(dark ? QColor(255, 255, 255, 13) : QColor(255, 255, 255, 200));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
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
    // Compact flat tabs over a bordered page, as in PyQt UI Designer. Children keep the
    // coordinates saved there, so the page has to start where it started there too; the
    // browser-style Fluent tab strip is a good deal taller and would clip them.
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("hmiTabs"));
    m_tabs->setDocumentMode(false);
    m_tabs->tabBar()->setDrawBase(false);
    m_tabs->tabBar()->setExpanding(false);
    layout->addWidget(m_tabs);
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

int TabContainerWidget::contentTop() const
{
    const QWidget *page = m_tabs->currentWidget();
    const int top = page ? page->mapTo(this, QPoint()).y() : 0;
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
