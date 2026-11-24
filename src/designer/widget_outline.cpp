#include "designer/widget_outline.h"

#include "core/i18n.h"
#include "core/widget_meta.h"
#include "designer/canvas_item.h"
#include "designer/design_scene.h"
#include "runtime/hmi_widget.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

namespace {

constexpr int ItemPtrRole = Qt::UserRole + 1;

QString nodeLabel(const CanvasItem *item)
{
    QString prefix;
    if (const CanvasItem *parent = item->containerParent();
        parent && parent->typeName() == QLatin1String("TabWidget")) {
        const QStringList pages = parent->tabPageNames();
        const int idx = item->tabIndex();
        const QString pageTitle = idx >= 0 && idx < pages.size() ? pages.at(idx) : QString::number(idx + 1);
        prefix = QStringLiteral("[%1] ").arg(pageTitle);
    }
    const QString title = item->propertyValue(QStringLiteral("title")).toString().trimmed();
    const QString text = title.isEmpty() ? item->propertyValue(QStringLiteral("text")).toString().trimmed() : title;
    const QString kind = translated(item->meta().displayName);
    if (!text.isEmpty() && text != item->widgetId()) {
        const QString brief = text.size() > 14 ? text.left(13) + QChar(0x2026) : text;
        return QStringLiteral("%1%2 · %3").arg(prefix, item->widgetId(), brief);
    }
    return QStringLiteral("%1%2 · %3").arg(prefix, item->widgetId(), kind);
}

} // namespace

WidgetOutline::WidgetOutline(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("widgetOutline"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 4, 8);
    layout->setSpacing(6);

    m_search = new FluentQt::SearchLineEdit(this);
    m_search->setObjectName(QStringLiteral("outlineSearch"));
    m_search->setPlaceholderText(tr("搜索控件 ID / 名称"));

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(4);
    const auto makeBtn = [this](FluentQt::FluentIcon icon, const QString &tip, const char *name) {
        auto *btn = new FluentQt::TransparentToolButton(FluentQt::icon(icon), this);
        btn->setObjectName(QLatin1String(name));
        btn->setFixedSize(28, 28);
        fluentToolTip(btn, tip);
        return btn;
    };
    m_up = makeBtn(FluentQt::FluentIcon::Up, tr("在容器内上移"), "outlineMoveUp");
    m_down = makeBtn(FluentQt::FluentIcon::Down, tr("在容器内下移"), "outlineMoveDown");
    m_pullOut = makeBtn(FluentQt::FluentIcon::Return, tr("移出容器"), "outlinePullOut");
    actions->addWidget(m_up);
    actions->addWidget(m_down);
    actions->addWidget(m_pullOut);
    actions->addStretch(1);

    m_tree = new FluentQt::TreeWidget(this);
    m_tree->setObjectName(QStringLiteral("outlineTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setIndentation(14);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);

    layout->addWidget(m_search);
    layout->addLayout(actions);
    layout->addWidget(m_tree, 1);

    connect(m_search, &QLineEdit::textChanged, this, &WidgetOutline::filter);
    connect(m_up, &QToolButton::clicked, this, [this]() { moveSelectedBy(-1); });
    connect(m_down, &QToolButton::clicked, this, [this]() { moveSelectedBy(1); });
    connect(m_pullOut, &QToolButton::clicked, this, &WidgetOutline::moveSelectedOut);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this]() {
        if (m_syncing || !m_scene) {
            return;
        }
        m_syncing = true;
        m_scene->clearSelection();
        for (QTreeWidgetItem *node : m_tree->selectedItems()) {
            auto *item = reinterpret_cast<CanvasItem *>(node->data(0, ItemPtrRole).value<quintptr>());
            if (item && item->scene() == m_scene) {
                revealItem(item);
                item->setSelected(true);
            }
        }
        m_syncing = false;
        updateActionButtons();
    });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *node = m_tree->itemAt(pos);
        CanvasItem *item =
            node ? reinterpret_cast<CanvasItem *>(node->data(0, ItemPtrRole).value<quintptr>()) : nullptr;
        if (item && m_scene && item->scene() == m_scene) {
            if (!item->isSelected()) {
                m_tree->clearSelection();
                node->setSelected(true);
            }
            emit itemContextMenuRequested(item, m_tree->viewport()->mapToGlobal(pos));
        }
    });
    updateActionButtons();
}

void WidgetOutline::setScene(DesignScene *scene)
{
    m_scene = scene;
    rebuild();
}

void WidgetOutline::rebuild()
{
    m_syncing = true;
    m_tree->clear();
    m_nodes.clear();
    if (m_scene) {
        for (CanvasItem *item : m_scene->topLevelItems()) {
            addNode(item, nullptr);
        }
    }
    m_tree->expandAll();
    if (!m_search->text().trimmed().isEmpty()) {
        filter(m_search->text());
    }
    m_syncing = false;
    syncSelection();
}

QTreeWidgetItem *WidgetOutline::addNode(CanvasItem *item, QTreeWidgetItem *parent)
{
    auto *node = parent ? new QTreeWidgetItem(parent, {nodeLabel(item)}) : new QTreeWidgetItem(m_tree, {nodeLabel(item)});
    node->setIcon(0, widgetIcon(item->meta().icon));
    node->setData(0, ItemPtrRole, quintptr(item));
    node->setToolTip(0, QStringLiteral("%1 (%2)").arg(item->widgetId(), translated(item->meta().displayName)));
    m_nodes.insert(item, node);
    for (CanvasItem *child : item->containerChildren()) {
        addNode(child, node);
    }
    return node;
}

void WidgetOutline::syncSelection()
{
    if (m_syncing || !m_scene) {
        return;
    }
    m_syncing = true;
    {
        const QSignalBlocker blocker(m_tree);
        m_tree->clearSelection();
        QTreeWidgetItem *first = nullptr;
        for (CanvasItem *item : m_scene->selectedCanvasItems()) {
            if (QTreeWidgetItem *node = m_nodes.value(item)) {
                node->setSelected(true);
                if (!first) {
                    first = node;
                }
            }
        }
        if (first) {
            m_tree->scrollToItem(first);
        }
    }
    m_syncing = false;
    updateActionButtons();
}

void WidgetOutline::filter(const QString &text)
{
    const QString needle = text.trimmed();
    const std::function<bool(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *node) -> bool {
        bool anyChild = false;
        for (int i = 0; i < node->childCount(); ++i) {
            if (visit(node->child(i))) {
                anyChild = true;
            }
        }
        const bool match = needle.isEmpty() || node->text(0).contains(needle, Qt::CaseInsensitive) ||
                           node->toolTip(0).contains(needle, Qt::CaseInsensitive);
        node->setHidden(!match && !anyChild);
        if (anyChild && !needle.isEmpty()) {
            node->setExpanded(true);
        }
        return match || anyChild;
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        visit(m_tree->topLevelItem(i));
    }
}

CanvasItem *WidgetOutline::selectedOutlineItem() const
{
    if (!m_scene) {
        return nullptr;
    }
    const QList<CanvasItem *> selected = m_scene->selectedCanvasItems();
    return selected.size() == 1 ? selected.first() : nullptr;
}

void WidgetOutline::updateActionButtons()
{
    CanvasItem *item = selectedOutlineItem();
    CanvasItem *parent = item ? item->containerParent() : nullptr;
    bool canUp = false;
    bool canDown = false;
    if (parent) {
        const bool tabs = parent->typeName() == QLatin1String("TabWidget");
        QList<CanvasItem *> siblings;
        for (CanvasItem *c : parent->containerChildren()) {
            if (!tabs || c->tabIndex() == item->tabIndex()) {
                siblings.append(c);
            }
        }
        const int idx = int(siblings.indexOf(item));
        canUp = idx > 0;
        canDown = idx >= 0 && idx + 1 < siblings.size();
    }
    m_up->setEnabled(canUp);
    m_down->setEnabled(canDown);
    m_pullOut->setEnabled(parent != nullptr);
}

void WidgetOutline::revealItem(CanvasItem *item)
{
    // Switch every enclosing TabWidget to the page that holds `item` so it becomes visible on canvas.
    QList<QPair<CanvasItem *, int>> switches;
    for (CanvasItem *cur = item; cur && cur->containerParent(); cur = cur->containerParent()) {
        CanvasItem *parent = cur->containerParent();
        if (parent->typeName() == QLatin1String("TabWidget")) {
            switches.prepend({parent, cur->tabIndex()});
        }
    }
    for (const auto &entry : switches) {
        entry.first->setEditorPage(entry.second);
    }
}

void WidgetOutline::moveSelectedBy(int delta)
{
    CanvasItem *item = selectedOutlineItem();
    CanvasItem *parent = item ? item->containerParent() : nullptr;
    if (!parent || !parent->moveChildBy(item, delta)) {
        return;
    }
    emit m_scene->modified();
    rebuild();
}

void WidgetOutline::moveSelectedOut()
{
    CanvasItem *item = selectedOutlineItem();
    CanvasItem *parent = item ? item->containerParent() : nullptr;
    if (!parent) {
        return;
    }
    parent->removeChild(item);
    item->snapToGrid();
    emit m_scene->itemPropertiesChanged(item);
    emit m_scene->modified();
    rebuild();
}

} // namespace fhmi
