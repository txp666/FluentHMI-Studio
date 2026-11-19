#include "studio/designer_page.h"

#include "core/frame_templates.h"
#include "designer/canvas_item.h"
#include "designer/design_scene.h"
#include "designer/property_editor.h"
#include "designer/project_frame_templates.h"
#include "designer/widget_palette.h"
#include "runtime/hmi_widget.h"
#include "core/s7.h"

#include <FluentQtWidgets/Dialogs/Dialog.h>
#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/CommandBar.h>
#include <FluentQtWidgets/Widgets/Menu.h>
#include <FluentQtWidgets/Widgets/SmoothScrollDelegate.h>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QMimeData>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QClipboard>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QIconEngine>
#include <QtGui/QPainter>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <functional>
#include <optional>

namespace fhmi {

namespace {

using FluentQt::FluentIcon;

// Ctrl/⌘ + wheel zooms; plain wheel and trackpad gestures scroll; the middle button drags the canvas.
class DesignView : public QGraphicsView
{
  public:
    using QGraphicsView::QGraphicsView;

  protected:
    // A middle press is taken here, before the scene sees it, so it never selects, moves, rubber-bands or reaches a
    // widget, whatever is under the pointer. The content follows the mouse 1:1 in screen pixels through the scroll
    // bars (the same ones the wheel moves); a canvas that fits has nothing to scroll and stays put.
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton) {
            // Not in the middle of a left drag: that drag would never see its release.
            if (!m_panning && event->buttons() == Qt::MiddleButton) {
                m_panning = true;
                m_panLast = event->position().toPoint();
                m_hadCursor = viewport()->testAttribute(Qt::WA_SetCursor);
                m_savedCursor = viewport()->cursor();
                viewport()->setCursor(Qt::ClosedHandCursor);
            }
            event->accept();
            return;
        }
        if (m_panning) { // another button while panning: ignore it
            event->accept();
            return;
        }
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_panning) {
            if (!(event->buttons() & Qt::MiddleButton)) { // the release was lost
                endPan();
                return;
            }
            const QPoint pos = event->position().toPoint();
            const QPoint delta = pos - m_panLast;
            m_panLast = pos;
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
            verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
            event->accept();
            return;
        }
        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_panning) {
            if (event->button() == Qt::MiddleButton) {
                endPan();
            }
            event->accept();
            return;
        }
        QGraphicsView::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton || m_panning) {
            event->accept();
            return;
        }
        QGraphicsView::mouseDoubleClickEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (m_panning) {
            event->accept();
            return;
        }
        QGraphicsView::contextMenuEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (event->modifiers() & Qt::ControlModifier) {
            const qreal factor = event->angleDelta().y() > 0 ? 1.1 : 1 / 1.1;
            const qreal next = transform().m11() * factor;
            if (next > 0.2 && next < 4) {
                scale(factor, factor);
            }
            event->accept();
            return;
        }
        QGraphicsView::wheelEvent(event);
    }

  private:
    void endPan()
    {
        m_panning = false;
        if (m_hadCursor) {
            viewport()->setCursor(m_savedCursor);
        } else {
            viewport()->unsetCursor();
        }
    }

    bool m_panning = false;
    bool m_hadCursor = false;
    QPoint m_panLast;
    QCursor m_savedCursor;
};

// 重做 is 撤销's arrow turned left to right, as Fluent has no redo icon. It paints through the Fluent icon, so it
// follows the theme as that does.
class MirroredIconEngine : public QIconEngine
{
  public:
    explicit MirroredIconEngine(const QIcon &icon) : m_icon(icon) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        painter->save();
        painter->translate(rect.left() + rect.right() + 1, 0);
        painter->scale(-1, 1);
        m_icon.paint(painter, rect, Qt::AlignCenter, mode, state);
        painter->restore();
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return QPixmap::fromImage(m_icon.pixmap(size, mode, state).toImage().flipped(Qt::Horizontal));
    }

    QIconEngine *clone() const override { return new MirroredIconEngine(m_icon); }

  private:
    QIcon m_icon;
};

// A new project: its app name is in the interface language (a file without one opens as 上位机).
Project blankProject()
{
    Project project;
    project.appName = DesignerPage::tr("上位机");
    return project;
}

QString projectFilter() { return QCoreApplication::translate("fhmi", "FluentHMI / PyQt UI Designer 工程 (*.uidp)"); }

constexpr int kHistoryLimit = 100;

// Copied widgets retain their source version. Plain snippets without a version are written for this editor.
std::optional<QJsonObject> clipboardDocument(const QByteArray &content, bool plainText)
{
    const QJsonDocument document = QJsonDocument::fromJson(content);
    const QJsonObject object = document.object();
    QJsonArray widgets;
    if (document.isObject() && object.value(QStringLiteral("widgets")).isArray()) {
        widgets = object.value(QStringLiteral("widgets")).toArray();
    } else if (plainText && document.isArray()) {
        widgets = document.array();
    } else if (plainText && document.isObject() && object.contains(QStringLiteral("type"))) {
        widgets = {object};
    } else {
        return std::nullopt;
    }
    QJsonObject selection{{QStringLiteral("widgets"), widgets}};
    selection.insert(QStringLiteral("version"), object.contains(QStringLiteral("version"))
                                                    ? object.value(QStringLiteral("version"))
                                                    : QJsonValue(Project::formatVersion(widgets)));
    if (object.contains(QStringLiteral("frameTemplates")))
        selection.insert(QStringLiteral("frameTemplates"), object.value(QStringLiteral("frameTemplates")));
    return selection;
}

QJsonObject referencedFrameTemplates(const QJsonObject &library, const QJsonArray &widgets)
{
    QJsonObject templates;
    visitWidgets(widgets, [&](const QJsonObject &widget) {
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        auto ids = FrameTemplates::receiveIds(props);
        ids.append(props.value(FrameTemplates::SendId).toString());
        for (const auto &id : ids) {
            if (library.contains(id))
                templates.insert(id, library.value(id));
        }
        return true;
    });
    return templates;
}

QJsonArray copyTemplateReferences(const Project &project, QJsonArray widgets)
{
    for (qsizetype i = 0; i < widgets.size(); ++i) {
        auto widget = widgets.at(i).toObject();
        if (supportsRouteTransport(widget.value(QStringLiteral("type")).toString(), RouteTransport::Bytes)) {
            auto props = widget.value(QStringLiteral("properties")).toObject();
            // A copied card keeps the template it used, including one inherited from the source project.
            if (FrameTemplates::receiveIds(props).isEmpty()) {
                const auto id = project.linkReceiveTemplateId(props);
                if (!id.isEmpty()) {
                    props.insert(FrameTemplates::ReceiveId, id);
                    widget.insert(QStringLiteral("properties"), props);
                }
            }
        }
        if (widget.contains(QStringLiteral("children")))
            widget.insert(QStringLiteral("children"),
                          copyTemplateReferences(project, widget.value(QStringLiteral("children")).toArray()));
        widgets[i] = widget;
    }
    return widgets;
}

QString rebasedPath(const QString &path, const QDir &from, const QDir &to)
{
    return path.isEmpty() || !QDir::isRelativePath(path) ? path : to.relativeFilePath(from.absoluteFilePath(path));
}

QJsonArray rebasedImages(QJsonArray widgets, const QDir &from, const QDir &to)
{
    for (qsizetype i = 0; i < widgets.size(); ++i) {
        QJsonObject widget = widgets.at(i).toObject();
        QJsonObject props = widget.value(QStringLiteral("properties")).toObject();
        if (widget.value(QStringLiteral("type")).toString() == QLatin1String("ImageDisplay") &&
            props.contains(QStringLiteral("imagePath"))) {
            props.insert(QStringLiteral("imagePath"),
                         rebasedPath(props.value(QStringLiteral("imagePath")).toString(), from, to));
            widget.insert(QStringLiteral("properties"), props);
        }
        if (widget.contains(QStringLiteral("children"))) {
            widget.insert(QStringLiteral("children"),
                          rebasedImages(widget.value(QStringLiteral("children")).toArray(), from, to));
        }
        widgets[i] = widget;
    }
    return widgets;
}

} // namespace

DesignerPage::DesignerPage(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("designerPage"));
    buildUi();
    load(blankProject());
}

void DesignerPage::buildUi()
{
    m_scene = new DesignScene(this);
    m_palette = new WidgetPalette(this);
    m_properties = new PropertyEditor(this);
    m_properties->setScene(m_scene);

    m_view = new DesignView(m_scene, this);
    m_view->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    m_view->setDragMode(QGraphicsView::RubberBandDrag);
    m_view->setAlignment(Qt::AlignCenter);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setAcceptDrops(true);
    m_view->setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    // Fluent overlay scroll bars instead of the platform ones; Ctrl+wheel still zooms.
    new FluentQt::SmoothScrollDelegate(m_view);

    // The same buttons in the same order as PyQt UI Designer's; icons only, the tooltip names each and its keys.
    auto *bar = new FluentQt::CommandBar(this);
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->setButtonTight(true);
    const auto action = [this, bar](FluentIcon icon, const QString &text, const QKeySequence &shortcut,
                                    const std::function<void()> &slot) {
        QAction *a = bar->addAction(icon, text);
        a->setToolTip(shortcut.isEmpty() ? text
                                         : QStringLiteral("%1 (%2)").arg(text, shortcut.toString(QKeySequence::NativeText)));
        if (!shortcut.isEmpty()) {
            a->setShortcut(shortcut);
            a->setShortcutContext(Qt::WindowShortcut);
            addAction(a);
        }
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    // The side panels' switches sit at the two ends of the bar; the canvas takes a hidden panel's room.
    const auto panelSwitch = [this, bar](QWidget *panel, const QString &title, const QString &icon) {
        QAction *a = bar->addAction(widgetIcon(icon + QStringLiteral("_on")), tr("隐藏%1").arg(title));
        a->setToolTip(a->text());
        connect(a, &QAction::triggered, this, [a, panel, title, icon]() {
            const bool shown = panel->isHidden();
            panel->setVisible(shown);
            a->setIcon(widgetIcon(shown ? icon + QStringLiteral("_on") : icon));
            a->setText((shown ? DesignerPage::tr("隐藏%1") : DesignerPage::tr("显示%1")).arg(title));
            a->setToolTip(a->text());
        });
    };
    panelSwitch(m_palette, tr("控件库"), QStringLiteral("panel_left"));
    bar->addSeparator();
    action(FluentIcon::Add, tr("新建"), QKeySequence::New, [this]() { newProject(); });
    action(FluentIcon::Folder, tr("打开"), QKeySequence::Open, [this]() { openProject(); });
    action(FluentIcon::Save, tr("保存"), QKeySequence::Save, [this]() { save(); });
    action(FluentIcon::SaveAs, tr("另存为"), QKeySequence::SaveAs, [this]() { saveAs(); });
    auto *logic = new FluentQt::TransparentPushButton(tr("数据与报警"), bar);
    logic->setObjectName(QStringLiteral("projectLogicButton"));
    connect(logic, &QPushButton::clicked, m_properties, &PropertyEditor::editProjectLogic);
    bar->addWidget(logic);
    bar->addSeparator();
    // A focused text field takes these keys first, and keeps its own undo, copy and paste.
    m_undoAction = action(FluentIcon::Cancel, tr("撤销"), QKeySequence::Undo, [this]() { undo(); });
    m_redoAction = action(FluentIcon::Cancel, tr("重做"), QKeySequence::Redo, [this]() { redo(); });
    m_redoAction->setIcon(QIcon(new MirroredIconEngine(m_redoAction->icon())));
    m_redoAction->setShortcuts(QKeySequence::Redo); // Ctrl+Y and Ctrl+Shift+Z on Windows
    bar->addSeparator();
    m_copyAction = action(FluentIcon::Copy, tr("复制"), QKeySequence::Copy, [this]() { copy(); });
    action(FluentIcon::Paste, tr("粘贴"), QKeySequence::Paste, [this]() { paste(); });
    action(FluentIcon::Delete, tr("删除"), QKeySequence(), [this]() { m_scene->deleteSelected(); });
    bar->addSeparator();
    const auto aligner = [this](Alignment alignment) { return [this, alignment]() { m_scene->align(alignment); }; };
    auto *menu = new FluentQt::RoundMenu(this);
    const QList<QPair<QString, Alignment>> alignments = {
        {tr("左对齐"), Alignment::Left},
        {tr("右对齐"), Alignment::Right},
        {tr("顶对齐"), Alignment::Top},
        {tr("底对齐"), Alignment::Bottom},
        {tr("水平居中"), Alignment::HorizontalCenter},
        {tr("垂直居中"), Alignment::VerticalCenter},
        {tr("横向等距分布"), Alignment::DistributeHorizontally},
        {tr("纵向等距分布"), Alignment::DistributeVertically},
    };
    for (const auto &entry : alignments) {
        connect(menu->addAction(entry.first), &QAction::triggered, this, aligner(entry.second));
    }
    action(FluentIcon::Alignment, tr("对齐"), QKeySequence(), [menu]() { menu->exec(QCursor::pos(), true); });
    bar->addSeparator();
    QAction *grid = action(FluentIcon::Tiles, tr("网格"), QKeySequence(), []() {});
    grid->setCheckable(true);
    grid->setChecked(true);
    connect(grid, &QAction::toggled, m_scene, &DesignScene::setGridVisible);
    action(FluentIcon::ZoomIn, tr("放大"), QKeySequence::ZoomIn, [this]() { zoomBy(1.15); });
    action(FluentIcon::ZoomOut, tr("缩小"), QKeySequence::ZoomOut, [this]() { zoomBy(1 / 1.15); });
    action(FluentIcon::FitPage, tr("适应窗口"), QKeySequence(Qt::CTRL | Qt::Key_0), [this]() { fitCanvas(); });
    action(FluentIcon::Zoom, tr("实际大小"), QKeySequence(), [this]() { m_view->resetTransform(); });
    bar->addSeparator();
    panelSwitch(m_properties, tr("属性"), QStringLiteral("panel_right"));

    auto *center = new QWidget(this);
    auto *centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(0, 4, 0, 0);
    centerLayout->setSpacing(4);
    centerLayout->addWidget(bar);
    centerLayout->addWidget(m_view, 1);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_palette);
    splitter->addWidget(center);
    splitter->addWidget(m_properties);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({210, 760, 330});
    splitter->setChildrenCollapsible(false);
    m_palette->setMinimumWidth(170);
    m_properties->setMinimumWidth(280);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 0, 8, 8);
    layout->addWidget(splitter);

    connect(m_palette, &WidgetPalette::widgetActivated, this, [this](const QString &type) {
        const QPointF center = m_view->mapToScene(m_view->viewport()->rect().center());
        const QPointF page(std::clamp<qreal>(center.x(), 0, m_project.canvasWidth - 40),
                           std::clamp<qreal>(center.y(), 0, m_project.canvasHeight - 40));
        m_scene->createItem(type, page);
    });
    connect(m_scene, &QGraphicsScene::selectionChanged, this, [this]() {
        const QList<CanvasItem *> selected = m_scene->selectedCanvasItems();
        m_properties->showItem(selected.size() == 1 ? selected.first() : nullptr);
        updateEditActions();
    });
    connect(m_scene, &DesignScene::itemPropertiesChanged, this, [this](CanvasItem *item) {
        if (m_properties->currentItem() == item) {
            m_properties->refreshGeometry();
        }
    });
    connect(m_scene, &DesignScene::editorPageChanged, this, [this](CanvasItem *item) {
        m_history[m_historyIndex].editorPages.insert(item->widgetId(), item->editorPage());
    });
    connect(m_scene, &DesignScene::connectionRenamed, this, [this](const QString &previous, const QString &current) {
        auto points = m_project.logic.value(QStringLiteral("points")).toArray();
        for (int i = 0; i < points.size(); ++i) {
            auto point = points[i].toObject();
            auto binding = point.value(QStringLiteral("binding")).toObject();
            if (binding.value(QStringLiteral("sourceCardId")).toString() == previous) {
                binding.insert(QStringLiteral("sourceCardId"), current);
                point.insert(QStringLiteral("binding"), binding);
                points[i] = point;
            }
        }
        if (!m_project.logic.isEmpty())
            m_project.logic.insert(QStringLiteral("points"), points);
    });
    connect(m_scene, &DesignScene::modified, this, [this](const QString &key) { recordChange(key); });
    connect(m_properties, &PropertyEditor::modified, this, [this](const QString &key) {
        m_scene->refreshSlaveTables(); // a slave card's areas and initial values show in the register tables
        recordChange(key);
    });
    connect(m_properties, &PropertyEditor::projectSettingsChanged, this, [this](const QString &key) {
        applyProjectSettings();
        recordChange(key.isEmpty() ? key : QStringLiteral("project:") + key);
    });
    connect(m_properties, &PropertyEditor::message, this, &DesignerPage::notify);
}

void DesignerPage::zoomBy(qreal factor)
{
    const qreal next = m_view->transform().m11() * factor;
    if (next > 0.2 && next < 4) {
        m_view->scale(factor, factor);
    }
}

void DesignerPage::fitCanvas()
{
    const QRectF page(QPointF(-16, -16), QSizeF(m_project.canvasWidth + 32, m_project.canvasHeight + 32));
    const QSize viewport = m_view->viewport()->size();
    if (viewport.isEmpty()) {
        return;
    }
    const qreal scale = std::min({1.0, viewport.width() / page.width(), viewport.height() / page.height()});
    m_view->setTransform(QTransform::fromScale(scale, scale));
    m_view->centerOn(page.center());
}

void DesignerPage::applyProjectSettings()
{
    m_scene->setCanvasSize(QSize(m_project.canvasWidth, m_project.canvasHeight));
    m_scene->setProjectParser(m_project.parserType());
    m_scene->setBitProfiles(m_project.bitProfiles);
    m_scene->setGridSize(m_project.gridSize);
}

Project DesignerPage::snapshot() const
{
    Project project = m_project;
    project.widgets = m_scene->widgetsJson();
    return project;
}

QString DesignerPage::displayName() const
{
    const QString file = m_project.filePath.isEmpty() ? tr("未命名") : QFileInfo(m_project.filePath).fileName();
    return (m_modified ? QStringLiteral("● ") : QString()) + file;
}

void DesignerPage::setModified(bool modified)
{
    m_modified = modified;
    emit titleChanged(displayName());
    if (modified) {
        emit projectChanged();
    }
}

void DesignerPage::load(const Project &project)
{
    QString error;
    m_scene->setAssetBaseDir(project.assetBaseDir());
    if (!m_scene->loadWidgets(project.widgets, &error)) {
        emit notify(tr("无法打开工程"), error, true);
        return;
    }
    m_project = project;
    m_properties->showItem(nullptr);
    m_properties->setProject(&m_project);
    applyProjectSettings();
    // Once the view has its size: a page larger than the view opens fitted.
    QTimer::singleShot(0, this, [this]() { fitCanvas(); });
    resetHistory();
    setModified(false);
    emit projectChanged();
}

// ---------------------------------------------------------------------------
// Undo and redo
// ---------------------------------------------------------------------------

void DesignerPage::resetHistory()
{
    m_history = {HistoryStep{snapshot().toJson(), editorPages()}};
    m_historyIndex = 0;
    m_savedIndex = 0;
    m_lastMergeKey.clear();
    m_pasteContent.clear();
    m_pasteCount = 0;
    updateEditActions();
}

void DesignerPage::recordChange(const QString &mergeKey)
{
    const QJsonObject state = snapshot().toJson();
    if (state == m_history.at(m_historyIndex).project) {
        return; // a click that moved nothing
    }
    // A burst of edits is one step, but never folded into the saved design, which undo must still reach.
    const bool merge = !mergeKey.isEmpty() && mergeKey == m_lastMergeKey && m_historyIndex > 0 &&
                       m_historyIndex + 1 == m_history.size() && m_historyIndex != m_savedIndex;
    if (merge) {
        m_history[m_historyIndex] = {state, editorPages()};
    } else {
        m_history.erase(m_history.begin() + m_historyIndex + 1, m_history.end());
        if (m_savedIndex > m_historyIndex) {
            m_savedIndex = -1;
        }
        m_history.append(HistoryStep{state, editorPages()});
        ++m_historyIndex;
        if (m_history.size() > kHistoryLimit) {
            m_history.removeFirst();
            --m_historyIndex;
            m_savedIndex = std::max(-1, m_savedIndex - 1);
        }
    }
    m_lastMergeKey = mergeKey;
    setModified(m_historyIndex != m_savedIndex);
    updateEditActions();
}

void DesignerPage::undo()
{
    if (canUndo()) {
        restore(m_historyIndex - 1);
    }
}

void DesignerPage::redo()
{
    if (canRedo()) {
        restore(m_historyIndex + 1);
    }
}

QHash<QString, int> DesignerPage::editorPages() const
{
    QHash<QString, int> pages;
    for (const CanvasItem *item : m_scene->allItems()) {
        if (item->typeName() == QLatin1String("TabWidget"))
            pages.insert(item->widgetId(), item->editorPage());
    }
    return pages;
}

void DesignerPage::restore(int index)
{
    QStringList selected;
    for (const CanvasItem *item : m_scene->selectedCanvasItems()) {
        selected.append(item->widgetId());
    }
    const auto &step = m_history.at(index);
    Project project = Project::fromTrustedJson(step.project);
    project.filePath = m_project.filePath;
    m_properties->showItem(nullptr);
    QString error;
    if (!m_scene->loadWidgets(project.widgets, &error)) {
        emit notify(tr("无法撤销"), error, true);
        return;
    }
    m_historyIndex = index;
    m_lastMergeKey.clear();
    m_project = project;
    m_properties->setProject(&m_project);
    applyProjectSettings();
    {
        const QSignalBlocker blocker(m_scene);
        for (CanvasItem *item : m_scene->allItems()) {
            const auto it = step.editorPages.constFind(item->widgetId());
            if (it != step.editorPages.constEnd())
                item->setEditorPage(it.value());
        }
    }
    m_scene->selectIds(selected);
    const bool modified = m_historyIndex != m_savedIndex;
    setModified(modified);
    if (!modified) {
        emit projectChanged(); // back at the saved design, but still not the design the preview has
    }
    updateEditActions();
}

void DesignerPage::updateEditActions()
{
    if (!m_undoAction) {
        return;
    }
    m_undoAction->setEnabled(canUndo());
    m_redoAction->setEnabled(canRedo());
    m_copyAction->setEnabled(!m_scene->selectedCanvasItems().isEmpty());
}

// ---------------------------------------------------------------------------
// Copy and paste
// ---------------------------------------------------------------------------

void DesignerPage::copy()
{
    const auto widgets = copyTemplateReferences(m_project, m_scene->selectionJson());
    if (widgets.isEmpty()) {
        return;
    }
    QJsonObject document{{QStringLiteral("version"), Project::formatVersion(widgets)}, {QStringLiteral("widgets"), widgets}};
    const auto templates = referencedFrameTemplates(m_project.frameTemplates, widgets);
    if (!templates.isEmpty())
        document.insert(QStringLiteral("frameTemplates"), templates);
    const QByteArray json = QJsonDocument(document).toJson(QJsonDocument::Compact);
    auto *mime = new QMimeData;
    mime->setData(QLatin1String(WidgetsClipboardType), json);
    mime->setText(QString::fromUtf8(json));
    QGuiApplication::clipboard()->setMimeData(mime);
}

void DesignerPage::paste()
{
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    QByteArray content;
    std::optional<QJsonObject> document;
    if (mime && mime->hasFormat(QLatin1String(WidgetsClipboardType))) {
        content = mime->data(QLatin1String(WidgetsClipboardType));
        document = clipboardDocument(content, false);
    }
    if (!document && mime && mime->hasText()) {
        content = mime->text().toUtf8();
        document = clipboardDocument(content, true);
    }
    // Checked as opening a project checks them; a link card's frame format against this project's mode.
    if (document) {
        document->insert(QStringLiteral("serial"), QJsonObject{{QStringLiteral("parser"), m_project.parserType()},
                                                               {QStringLiteral("parserConfig"), m_project.parserConfig()}});
        const auto library = document->value(QStringLiteral("frameTemplates"));
        if (library.isUndefined() || library.isObject()) {
            auto definitions = library.toObject();
            auto missing = referencedFrameTemplates(m_project.frameTemplates,
                                                      document->value(QStringLiteral("widgets")).toArray());
            for (const auto &id : definitions.keys())
                missing.remove(id);
            mergeFrameTemplateLibrary(definitions, missing);
            if (!definitions.isEmpty())
                document->insert(QStringLiteral("frameTemplates"), definitions);
        }
    }
    Project clipboard;
    QString error;
    if (!document || !Project::fromJson(*document, &clipboard, &error, true) || clipboard.widgets.isEmpty()) {
        emit warning(tr("剪贴板里没有可粘贴的控件"), error);
        return;
    }
    const QList<CanvasItem *> selected = m_scene->selectedCanvasItems();
    CanvasItem *container = selected.size() == 1 && selected.first()->isContainer() ? selected.first() : nullptr;
    const QString target = container ? container->widgetId() : QString();
    m_pasteCount = content == m_pasteContent && target == m_pasteTarget ? m_pasteCount + 1 : 1;
    m_pasteContent = content;
    m_pasteTarget = target;
    const auto previous = m_project.frameTemplates;
    m_project.frameTemplates = importClipboardFrameTemplates(clipboard, previous);
    if (m_scene->paste(clipboard.widgets, container, m_pasteCount).isEmpty())
        m_project.frameTemplates = previous;
}

bool DesignerPage::maybeSave()
{
    if (!m_modified) {
        return true;
    }
    FluentQt::MessageBox box(tr("保存更改？"), tr("“%1”有未保存的更改。").arg(displayName().remove(QStringLiteral("● "))),
                             window());
    box.yesButton()->setText(tr("保存"));
    box.cancelButton()->setText(tr("取消"));
    auto *discard = new FluentQt::PushButton(&box);
    discard->setText(tr("不保存"));
    // Stretch 1, like the box's own buttons, so the three share the row evenly.
    box.buttonLayout()->insertWidget(1, discard, 1, Qt::AlignVCenter);
    bool discarded = false;
    connect(discard, &QPushButton::clicked, &box, [&box, &discarded]() {
        discarded = true;
        box.reject();
    });
    if (box.exec() == QDialog::Accepted) {
        return save();
    }
    return discarded;
}

bool DesignerPage::newProject()
{
    if (!maybeSave()) {
        return false;
    }
    load(blankProject());
    return true;
}

bool DesignerPage::openProject(const QString &path)
{
    if (!maybeSave()) {
        return false;
    }
    QString file = path;
    if (file.isEmpty()) {
        file = QFileDialog::getOpenFileName(this, tr("打开工程"), QString(), projectFilter());
        if (file.isEmpty()) {
            return false;
        }
    }
    Project project;
    QString error;
    if (!Project::load(file, &project, &error)) {
        emit notify(tr("无法打开工程"), error, true);
        return false;
    }
    load(project);
    emit notify(tr("已打开"), QFileInfo(file).fileName(), false);
    return true;
}

bool DesignerPage::save()
{
    return m_project.filePath.isEmpty() ? saveAs() : saveTo(m_project.filePath);
}

bool DesignerPage::saveAs()
{
    const QString suggested = m_project.filePath.isEmpty() ? m_project.appName + QStringLiteral(".uidp")
                                                           : m_project.filePath;
    QString file = QFileDialog::getSaveFileName(this, tr("保存工程"), suggested, projectFilter());
    if (file.isEmpty()) {
        return false;
    }
    if (QFileInfo(file).suffix().isEmpty()) {
        file += QStringLiteral(".uidp");
    }
    return saveTo(file);
}

bool DesignerPage::saveTo(const QString &path)
{
    // Relative image and icon paths follow the project to its new folder.
    const QDir oldBase(m_project.assetBaseDir());
    const QDir newBase(QFileInfo(path).absolutePath());
    const bool moved = oldBase.absolutePath() != newBase.absolutePath();
    if (moved) {
        for (CanvasItem *item : m_scene->allItems()) {
            const QString image = item->propertyValue(QStringLiteral("imagePath")).toString();
            if (item->typeName() == QLatin1String("ImageDisplay") && !image.isEmpty() && QDir::isRelativePath(image)) {
                item->setPropertyValue(QStringLiteral("imagePath"), rebasedPath(image, oldBase, newBase));
            }
        }
        m_project.icon = rebasedPath(m_project.icon, oldBase, newBase);
    }
    Project project = snapshot();
    QString error;
    if (!project.save(path, &error)) {
        emit notify(tr("保存失败"), error, true);
        return false;
    }
    m_project.filePath = QFileInfo(path).absoluteFilePath();
    m_scene->setAssetBaseDir(m_project.assetBaseDir());
    // The undo steps follow too, so undoing past the save still finds the images.
    if (moved) {
        for (HistoryStep &step : m_history) {
            QJsonObject &state = step.project;
            state.insert(QStringLiteral("widgets"),
                         rebasedImages(state.value(QStringLiteral("widgets")).toArray(), oldBase, newBase));
            const QString icon = rebasedPath(state.value(QStringLiteral("icon")).toString(), oldBase, newBase);
            if (!icon.isEmpty()) {
                state.insert(QStringLiteral("icon"), icon);
            }
        }
    }
    m_history[m_historyIndex].project = project.toJson();
    m_savedIndex = m_historyIndex;
    m_lastMergeKey.clear();
    setModified(false);
    emit notify(tr("已保存"), QFileInfo(path).fileName(), false);
    return true;
}

} // namespace fhmi
