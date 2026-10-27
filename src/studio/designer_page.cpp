#include "studio/designer_page.h"

#include "designer/canvas_item.h"
#include "designer/design_scene.h"
#include "designer/property_editor.h"
#include "designer/widget_palette.h"

#include <FluentQtWidgets/Dialogs/Dialog.h>
#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/CommandBar.h>
#include <FluentQtWidgets/Widgets/Menu.h>
#include <FluentQtWidgets/Widgets/SmoothScrollDelegate.h>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QCursor>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <functional>

namespace fhmi {

namespace {

using FluentQt::FluentIcon;

// Ctrl/⌘ + wheel zooms; plain wheel and trackpad gestures scroll.
class DesignView : public QGraphicsView
{
  public:
    using QGraphicsView::QGraphicsView;

  protected:
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
};

QString projectFilter()
{
    return QObject::tr("FluentHMI / PyQt UI Designer 工程 (*.uidp)");
}

} // namespace

DesignerPage::DesignerPage(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("designerPage"));
    buildUi();
    load(Project());
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

    auto *bar = new FluentQt::CommandBar(this);
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    const auto action = [this, bar](FluentIcon icon, const QString &text, const QKeySequence &shortcut,
                                    const std::function<void()> &slot) {
        QAction *a = bar->addAction(icon, text);
        if (!shortcut.isEmpty()) {
            a->setShortcut(shortcut);
            a->setShortcutContext(Qt::WindowShortcut);
            addAction(a);
        }
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    action(FluentIcon::Add, tr("新建"), QKeySequence::New, [this]() { newProject(); });
    action(FluentIcon::Folder, tr("打开"), QKeySequence::Open, [this]() { openProject(); });
    action(FluentIcon::Save, tr("保存"), QKeySequence::Save, [this]() { save(); });
    action(FluentIcon::SaveAs, tr("另存为"), QKeySequence::SaveAs, [this]() { saveAs(); });
    bar->addSeparator();
    action(FluentIcon::Delete, tr("删除"), QKeySequence(), [this]() { m_scene->deleteSelected(); });
    bar->addSeparator();
    const auto aligner = [this](Alignment alignment) { return [this, alignment]() { m_scene->align(alignment); }; };
    QAction *alignAction = bar->addAction(FluentIcon::Alignment, tr("对齐"));
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
    connect(alignAction, &QAction::triggered, this, [menu]() { menu->exec(QCursor::pos(), true); });
    QAction *grid = action(FluentIcon::Tiles, tr("网格"), QKeySequence(), []() {});
    grid->setCheckable(true);
    grid->setChecked(true);
    connect(grid, &QAction::toggled, m_scene, &DesignScene::setGridVisible);
    bar->addSeparator();
    action(FluentIcon::ZoomIn, tr("放大"), QKeySequence::ZoomIn, [this]() { zoomBy(1.15); });
    action(FluentIcon::ZoomOut, tr("缩小"), QKeySequence::ZoomOut, [this]() { zoomBy(1 / 1.15); });
    action(FluentIcon::FitPage, tr("适应窗口"), QKeySequence(Qt::CTRL | Qt::Key_0), [this]() { fitCanvas(); });
    action(FluentIcon::Zoom, tr("实际大小"), QKeySequence(), [this]() { m_view->resetTransform(); });

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
    });
    connect(m_scene, &DesignScene::itemPropertiesChanged, this, [this](CanvasItem *item) {
        if (m_properties->currentItem() == item) {
            m_properties->refreshGeometry();
        }
    });
    connect(m_scene, &DesignScene::modified, this, [this]() { setModified(true); });
    connect(m_properties, &PropertyEditor::modified, this, [this]() { setModified(true); });
    connect(m_properties, &PropertyEditor::projectSettingsChanged, this, [this]() {
        applyProjectSettings();
        setModified(true);
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
    setModified(false);
    emit projectChanged();
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
    box.buttonLayout()->insertWidget(1, discard);
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
    load(Project());
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
    // Relative image paths follow the project to its new folder.
    const QDir oldBase(m_project.assetBaseDir());
    const QDir newBase(QFileInfo(path).absolutePath());
    if (oldBase.absolutePath() != newBase.absolutePath()) {
        for (CanvasItem *item : m_scene->allItems()) {
            const QString image = item->propertyValue(QStringLiteral("imagePath")).toString();
            if (item->typeName() == QLatin1String("ImageDisplay") && !image.isEmpty() && QDir::isRelativePath(image)) {
                item->setPropertyValue(QStringLiteral("imagePath"),
                                       newBase.relativeFilePath(oldBase.absoluteFilePath(image)));
            }
        }
    }
    Project project = snapshot();
    QString error;
    if (!project.save(path, &error)) {
        emit notify(tr("保存失败"), error, true);
        return false;
    }
    m_project.filePath = QFileInfo(path).absoluteFilePath();
    m_scene->setAssetBaseDir(m_project.assetBaseDir());
    setModified(false);
    emit notify(tr("已保存"), QFileInfo(path).fileName(), false);
    return true;
}

} // namespace fhmi
