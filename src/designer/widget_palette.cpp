#include "designer/widget_palette.h"

#include "core/widget_meta.h"
#include "designer/design_scene.h"
#include "runtime/hmi_widget.h"

#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>

#include <QtCore/QMimeData>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

namespace {

constexpr int TypeRole = Qt::UserRole + 1;

class PaletteTree : public FluentQt::TreeWidget
{
  public:
    using FluentQt::TreeWidget::TreeWidget;

  protected:
    QStringList mimeTypes() const override { return {QLatin1String(WidgetMimeType)}; }

    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override
    {
        if (items.isEmpty() || items.first()->data(0, TypeRole).toString().isEmpty()) {
            return nullptr;
        }
        auto *mime = new QMimeData;
        mime->setData(QLatin1String(WidgetMimeType), items.first()->data(0, TypeRole).toString().toUtf8());
        return mime;
    }
};

} // namespace

WidgetPalette::WidgetPalette(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 4, 8);
    layout->setSpacing(8);
    auto *title = new FluentQt::StrongBodyLabel(tr("控件"), this);
    m_search = new FluentQt::SearchLineEdit(this);
    m_search->setPlaceholderText(tr("搜索控件"));
    m_tree = new PaletteTree(this);
    m_tree->setHeaderHidden(true);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setIndentation(12);
    m_tree->setIconSize(QSize(16, 16));

    const WidgetRegistry &registry = WidgetRegistry::instance();
    for (const WidgetCategory category : registry.categories()) {
        auto *group = new QTreeWidgetItem(m_tree, {categoryTitle(category)});
        group->setFlags(Qt::ItemIsEnabled);
        for (const WidgetMeta *meta : registry.byCategory(category)) {
            auto *item = new QTreeWidgetItem(group, {meta->displayName});
            item->setIcon(0, widgetIcon(meta->icon));
            item->setData(0, TypeRole, meta->typeName);
            item->setToolTip(0, meta->typeName);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        }
        group->setExpanded(true);
    }

    layout->addWidget(title);
    layout->addWidget(m_search);
    layout->addWidget(m_tree, 1);

    connect(m_search, &QLineEdit::textChanged, this, &WidgetPalette::filter);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        const QString type = item->data(0, TypeRole).toString();
        if (!type.isEmpty()) {
            emit widgetActivated(type);
        }
    });
}

void WidgetPalette::filter(const QString &text)
{
    const QString needle = text.trimmed();
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *group = m_tree->topLevelItem(g);
        int visible = 0;
        for (int i = 0; i < group->childCount(); ++i) {
            QTreeWidgetItem *item = group->child(i);
            const bool match = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive) ||
                               item->data(0, TypeRole).toString().contains(needle, Qt::CaseInsensitive);
            item->setHidden(!match);
            visible += match ? 1 : 0;
        }
        group->setHidden(visible == 0);
    }
}

} // namespace fhmi
