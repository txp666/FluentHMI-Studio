#include "designer/property_editor.h"

#include "core/binding.h"
#include "designer/canvas_item.h"
#include "designer/design_scene.h"
#include "runtime/hmi_widget.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/CardWidget.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <FluentQtWidgets/Widgets/SwitchButton.h>

#include <QtCore/QSet>
#include <QtGui/QFocusEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

#include <cmath>

namespace fhmi {

namespace {

// Command fields shown for each send mode, as in PyQt UI Designer.
const QHash<QString, QSet<QString>> &modeVisibility()
{
    static const QHash<QString, QSet<QString>> table = {
        {QStringLiteral("文本"),
         {QStringLiteral("command"), QStringLiteral("onCommand"), QStringLiteral("offCommand"),
          QStringLiteral("commandNewline"), QStringLiteral("commandTemplate")}},
        {QStringLiteral("HEX"),
         {QStringLiteral("command"), QStringLiteral("onCommand"), QStringLiteral("offCommand"),
          QStringLiteral("commandTemplate"), QStringLiteral("frameHeader"), QStringLiteral("frameTail"),
          QStringLiteral("dataType"), QStringLiteral("byteOrder")}},
        {QStringLiteral("Modbus"),
         {QStringLiteral("mbSlave"), QStringLiteral("mbFunc"), QStringLiteral("mbAddress"), QStringLiteral("mbValue")}},
    };
    return table;
}

const QSet<QString> &modeFields()
{
    static const QSet<QString> fields = {
        QStringLiteral("command"),     QStringLiteral("onCommand"),  QStringLiteral("offCommand"),
        QStringLiteral("commandNewline"), QStringLiteral("commandTemplate"), QStringLiteral("frameHeader"),
        QStringLiteral("frameTail"),   QStringLiteral("dataType"),   QStringLiteral("byteOrder"),
        QStringLiteral("mbSlave"),     QStringLiteral("mbFunc"),     QStringLiteral("mbAddress"),
        QStringLiteral("mbValue")};
    return fields;
}

const QHash<QString, QSet<QString>> &chartParseVisibility()
{
    static const QHash<QString, QSet<QString>> table = {
        {ParseMode::KeyValue, {QStringLiteral("curveKey")}},
        {ParseMode::JsonPath, {QStringLiteral("curveJsonPath")}},
        {ParseMode::HexBytes,
         {QStringLiteral("curveByteOffset"), QStringLiteral("curveDataType"), QStringLiteral("curveByteOrder"),
          QStringLiteral("curveFrameHeader"), QStringLiteral("curveFrameTail")}},
        {ParseMode::Modbus,
         {QStringLiteral("curveRegIndex"), QStringLiteral("curveMbDataType"), QStringLiteral("curveMbRegOrder"),
          QStringLiteral("curveMbFcFilter"), QStringLiteral("curveMbSlaveFilter")}},
    };
    return table;
}

// Virtual chart property -> per-curve storage suffix.
const QHash<QString, QString> &curveFields()
{
    static const QHash<QString, QString> table = {
        {QStringLiteral("curveName"), QStringLiteral("name")},
        {QStringLiteral("curveParseMode"), QStringLiteral("parseMode")},
        {QStringLiteral("curveKey"), QStringLiteral("key")},
        {QStringLiteral("curveJsonPath"), QStringLiteral("jsonPath")},
        {QStringLiteral("curveByteOffset"), QStringLiteral("byteOffset")},
        {QStringLiteral("curveDataType"), QStringLiteral("dataType")},
        {QStringLiteral("curveByteOrder"), QStringLiteral("byteOrder")},
        {QStringLiteral("curveFrameHeader"), QStringLiteral("frameHeader")},
        {QStringLiteral("curveFrameTail"), QStringLiteral("frameTail")},
        {QStringLiteral("curveRegIndex"), QStringLiteral("regIndex")},
        {QStringLiteral("curveMbDataType"), QStringLiteral("mbDataType")},
        {QStringLiteral("curveMbRegOrder"), QStringLiteral("mbRegOrder")},
        {QStringLiteral("curveMbFcFilter"), QStringLiteral("mbFcFilter")},
        {QStringLiteral("curveMbSlaveFilter"), QStringLiteral("mbSlaveFilter")},
    };
    return table;
}

// The send switch each widget type has; while it is off, no command fields apply.
QString sendToggle(const QString &type)
{
    if (type == QLatin1String("LineEdit")) {
        return QStringLiteral("sendOnEnter");
    }
    if (type == QLatin1String("ComboBox")) {
        return QStringLiteral("sendOnSelect");
    }
    if (type == QLatin1String("Slider") || type == QLatin1String("SpinBox")) {
        return QStringLiteral("sendOnChange");
    }
    return QString();
}

// Commits a complete list of lines when editing ends, not every keystroke: a tab page
// removed mid-edit would move its widgets to the last page for good.
class CommitOnFocusOutEdit : public FluentQt::PlainTextEdit
{
  public:
    using FluentQt::PlainTextEdit::PlainTextEdit;
    std::function<void()> onFinished;

  protected:
    void focusOutEvent(QFocusEvent *event) override
    {
        FluentQt::PlainTextEdit::focusOutEvent(event);
        if (onFinished) {
            onFinished();
        }
    }
};

QJsonValue numberValue(double value, bool integral)
{
    return integral ? QJsonValue(qint64(std::llround(value))) : QJsonValue(value);
}

} // namespace

PropertyEditor::PropertyEditor(QWidget *parent) : FluentQt::ScrollArea(parent)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rebuild();
}

void PropertyEditor::setProject(Project *project)
{
    m_project = project;
    if (!m_item) {
        rebuild();
    }
}

void PropertyEditor::showItem(CanvasItem *item)
{
    if (m_item == item && item) {
        return;
    }
    if (m_item) {
        disconnect(m_item, nullptr, this, nullptr);
    }
    m_item = item;
    if (m_item) {
        connect(m_item, &QObject::destroyed, this, [this]() { QMetaObject::invokeMethod(this, [this]() { rebuild(); }, Qt::QueuedConnection); });
    }
    rebuild();
}

void PropertyEditor::clearContent()
{
    m_rows.clear();
    m_bindingEditors.clear();
    for (auto &spin : m_geometry) {
        spin = nullptr;
    }
    if (QWidget *old = takeWidget()) {
        old->deleteLater();
    }
    m_content = new QWidget;
    m_content->setAttribute(Qt::WA_TranslucentBackground);
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(4, 8, 8, 8);
    m_layout->setSpacing(8);
    setWidget(m_content);
    enableTransparentBackground();
}

void PropertyEditor::rebuild()
{
    m_loading = true;
    clearContent();
    if (m_item) {
        buildItemForm();
    } else {
        buildProjectForm();
    }
    m_layout->addStretch();
    updateVisibility();
    m_loading = false;
}

QFormLayout *PropertyEditor::addSection(const QString &title)
{
    // SimpleCardWidget comes with its own box layout; the form goes inside it.
    auto *card = new FluentQt::SimpleCardWidget(m_content);
    auto *box = static_cast<QBoxLayout *>(card->layout());
    box->setContentsMargins(12, 10, 12, 10);
    auto *form = new QFormLayout;
    box->addLayout(form);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->addRow(new FluentQt::StrongBodyLabel(title, card));
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_layout->addWidget(card);
    return form;
}

void PropertyEditor::addRow(QFormLayout *form, const QString &key, const QString &label, QWidget *editor)
{
    auto *caption = new FluentQt::BodyLabel(label, m_content);
    form->addRow(caption, editor);
    if (!key.isEmpty()) {
        m_rows.insert(key, {caption, editor});
    }
}

void PropertyEditor::setRowVisible(const QString &key, bool visible)
{
    const auto it = m_rows.constFind(key);
    if (it != m_rows.constEnd()) {
        it->first->setVisible(visible);
        it->second->setVisible(visible);
    }
}

QWidget *PropertyEditor::createEditor(const PropDef &def, const QJsonValue &value, const Commit &commit)
{
    switch (def.type) {
    case PropType::Int:
    case PropType::Float: {
        const bool integral = def.type == PropType::Int;
        QAbstractSpinBox *box = nullptr;
        if (integral) {
            auto *spin = new FluentQt::SpinBox(m_content);
            spin->setRange(int(def.minimum.value_or(-999999)), int(def.maximum.value_or(999999)));
            spin->setValue(value.isString() ? value.toString().toInt() : value.toInt());
            connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
                    [commit](int v) { commit(QJsonValue(v)); });
            box = spin;
        } else {
            auto *spin = new FluentQt::DoubleSpinBox(m_content);
            spin->setRange(def.minimum.value_or(-999999), def.maximum.value_or(999999));
            spin->setDecimals(3);
            spin->setValue(value.toDouble());
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [commit](double v) { commit(numberValue(v, false)); });
            box = spin;
        }
        box->setKeyboardTracking(false);
        return box;
    }
    case PropType::Bool: {
        auto *toggle = new FluentQt::SwitchButton(m_content);
        toggle->setOnText(tr("开"));
        toggle->setOffText(tr("关"));
        toggle->setChecked(value.isBool() ? value.toBool() : value.toDouble() != 0);
        connect(toggle, &FluentQt::SwitchButton::checkedChanged, this, [commit](bool on) { commit(on); });
        return toggle;
    }
    case PropType::Enum: {
        auto *combo = new FluentQt::ComboBox(m_content);
        combo->addItems(def.options);
        const QString current = value.toVariant().toString();
        const int index = combo->findText(current);
        combo->setCurrentIndex(index >= 0 ? index : 0);
        connect(combo, &FluentQt::ComboBox::currentTextChanged, this, [commit](const QString &t) { commit(t); });
        return combo;
    }
    case PropType::Text: {
        auto *edit = new CommitOnFocusOutEdit(m_content);
        edit->setPlainText(value.toString());
        edit->setFixedHeight(84);
        if (def.name == QLatin1String("tabNames")) {
            edit->onFinished = [edit, commit]() { commit(edit->toPlainText()); };
        } else {
            connect(edit, &QPlainTextEdit::textChanged, this, [edit, commit]() { commit(edit->toPlainText()); });
        }
        return edit;
    }
    case PropType::String:
    case PropType::Color:
    case PropType::Command:
        break;
    }

    auto *edit = new FluentQt::LineEdit(m_content);
    edit->setText(value.toVariant().toString());
    connect(edit, &QLineEdit::textChanged, this, [commit](const QString &t) { commit(t); });
    if (def.name != QLatin1String("imagePath")) {
        return edit;
    }
    auto *row = new QWidget(m_content);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *browse = new FluentQt::ToolButton(row);
    browse->setIcon(FluentQt::icon(FluentQt::FluentIcon::Folder));
    keepSquareBeside(browse, edit);
    layout->addWidget(edit, 1);
    layout->addWidget(browse);
    connect(browse, &QToolButton::clicked, this, [this, edit]() {
        const QString path = QFileDialog::getOpenFileName(this, tr("选择图片"), QString(),
                                                          tr("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.svg)"));
        if (path.isEmpty()) {
            return;
        }
        // Relative to a saved project, so the project folder can move as a whole.
        QString stored = path;
        if (m_project && !m_project->filePath.isEmpty()) {
            const QString relative = QDir(m_project->assetBaseDir()).relativeFilePath(path);
            if (!relative.startsWith(QLatin1String(".."))) {
                stored = relative;
            }
        }
        edit->setText(stored);
    });
    return row;
}

// ---------------------------------------------------------------------------
// Project settings
// ---------------------------------------------------------------------------

void PropertyEditor::buildProjectForm()
{
    if (!m_project) {
        return;
    }
    QFormLayout *general = addSection(tr("项目"));
    auto *name = new FluentQt::LineEdit(m_content);
    name->setText(m_project->appName);
    connect(name, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_project->appName = text;
        emit projectSettingsChanged();
    });
    addRow(general, QString(), tr("应用名称"), name);

    const auto spin = [this](int lo, int hi, int value, const std::function<void(int)> &apply) {
        auto *box = new FluentQt::SpinBox(m_content);
        box->setRange(lo, hi);
        box->setValue(value);
        box->setKeyboardTracking(false);
        connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this, apply](int v) {
            apply(v);
            emit projectSettingsChanged();
        });
        return box;
    };
    addRow(general, QString(), tr("画布宽度"),
           spin(100, 9999, m_project->canvasWidth, [this](int v) { m_project->canvasWidth = v; }));
    addRow(general, QString(), tr("画布高度"),
           spin(100, 9999, m_project->canvasHeight, [this](int v) { m_project->canvasHeight = v; }));
    addRow(general, QString(), tr("网格大小"),
           spin(1, 100, m_project->gridSize, [this](int v) { m_project->gridSize = v; }));

    QFormLayout *frame = addSection(tr("默认帧格式"));
    auto *hint = new FluentQt::CaptionLabel(tr("链路卡片选择“项目设置”时使用"), m_content);
    hint->setWordWrap(true);
    frame->addRow(hint);
    auto *parser = new FluentQt::ComboBox(m_content);
    parser->addItems({QStringLiteral("line"), QStringLiteral("hex"), QStringLiteral("modbus"),
                      QStringLiteral("modbus_tcp")});
    parser->setCurrentIndex(std::max(0, parser->findText(m_project->parserType())));
    addRow(frame, QStringLiteral("parser"), tr("解析模式"), parser);

    const auto configEdit = [this](const char *key, const QString &fallback) {
        auto *edit = new FluentQt::LineEdit(m_content);
        edit->setText(m_project->parserConfig().value(QLatin1String(key)).toString(fallback));
        connect(edit, &QLineEdit::textChanged, this, [this, key](const QString &text) {
            QJsonObject config = m_project->parserConfig();
            config.insert(QLatin1String(key), text);
            m_project->serial.insert(QStringLiteral("parserConfig"), config);
            emit projectSettingsChanged();
        });
        return edit;
    };
    addRow(frame, QStringLiteral("separator"), tr("分隔符"), configEdit("separator", QStringLiteral(",")));
    addRow(frame, QStringLiteral("header"), tr("帧头(HEX)"), configEdit("header", QStringLiteral("AA55")));
    addRow(frame, QStringLiteral("footer"), tr("帧尾(HEX)"), configEdit("footer", QStringLiteral("55AA")));
    connect(parser, &FluentQt::ComboBox::currentTextChanged, this, [this](const QString &type) {
        m_project->serial.insert(QStringLiteral("parser"), type);
        updateVisibility();
        emit projectSettingsChanged();
    });

    auto *tip = new FluentQt::CaptionLabel(tr("从左侧拖拽控件到画布；选中控件后在这里编辑属性和数据绑定。"), m_content);
    tip->setWordWrap(true);
    m_layout->addWidget(tip);
}

// ---------------------------------------------------------------------------
// Item properties
// ---------------------------------------------------------------------------

QString PropertyEditor::storageKey(const QString &name) const
{
    if (!m_item) {
        return name;
    }
    if (m_item->typeName() == QLatin1String("RealtimeChart") && curveFields().contains(name)) {
        const int curve = m_item->propertyValue(QStringLiteral("currentCurve")).toInt(0);
        return QStringLiteral("_curve_%1_%2").arg(curve).arg(curveFields().value(name));
    }
    if (m_item->typeName() == QLatin1String("TabWidget") && name == QLatin1String("tabLayout")) {
        return QStringLiteral("_tabLayout_%1").arg(m_item->propertyValue(QStringLiteral("currentTab")).toInt(0));
    }
    return name;
}

QJsonValue PropertyEditor::itemValue(const PropDef &def) const
{
    const QString key = storageKey(def.name);
    const QJsonValue stored = m_item->properties().value(key);
    if (!stored.isUndefined() && !stored.isNull()) {
        return stored;
    }
    if (def.name == QLatin1String("curveName")) {
        return tr("曲线%1").arg(m_item->propertyValue(QStringLiteral("currentCurve")).toInt(0) + 1);
    }
    return def.defaultValue;
}

void PropertyEditor::commitProperty(const QString &name, const QJsonValue &value)
{
    if (m_loading || !m_item) {
        return;
    }
    m_item->setPropertyValue(storageKey(name), value);
    emit modified();
    // These change which per-curve / per-page values, or which fields, the form shows.
    static const QSet<QString> structural = {QStringLiteral("currentCurve"), QStringLiteral("curveCount"),
                                             QStringLiteral("currentTab"), QStringLiteral("tabNames")};
    if (structural.contains(name)) {
        if (name == QLatin1String("curveCount")) {
            const int count = m_item->propertyValue(QStringLiteral("curveCount")).toInt(1);
            if (m_item->propertyValue(QStringLiteral("currentCurve")).toInt(0) >= count) {
                m_item->setPropertyValue(QStringLiteral("currentCurve"), count - 1);
            }
        }
        QMetaObject::invokeMethod(this, [this]() { rebuild(); }, Qt::QueuedConnection);
        return;
    }
    updateVisibility();
}

void PropertyEditor::refreshGeometry()
{
    if (!m_item || !m_geometry[0]) {
        return;
    }
    m_loading = true;
    const int values[4] = {qRound(m_item->pos().x()), qRound(m_item->pos().y()), qRound(m_item->size().width()),
                           qRound(m_item->size().height())};
    for (int i = 0; i < 4; ++i) {
        m_geometry[i]->setValue(values[i]);
    }
    m_loading = false;
}

void PropertyEditor::buildItemForm()
{
    const WidgetMeta &meta = m_item->meta();

    auto *header = new FluentQt::SimpleCardWidget(m_content);
    static_cast<QBoxLayout *>(header->layout())->setContentsMargins(12, 10, 12, 10);
    auto *headerLayout = new QFormLayout;
    static_cast<QBoxLayout *>(header->layout())->addLayout(headerLayout);
    headerLayout->addRow(new FluentQt::SubtitleLabel(meta.displayName, header));
    auto *id = new FluentQt::LineEdit(header);
    id->setText(m_item->widgetId());
    connect(id, &QLineEdit::editingFinished, this, [this, id]() {
        if (!m_item || id->text() == m_item->widgetId()) {
            return;
        }
        QString error;
        if (!m_scene || !m_scene->renameItem(m_item, id->text(), &error)) {
            emit message(tr("无法重命名"), error, true);
            id->setText(m_item->widgetId());
        }
    });
    headerLayout->addRow(new FluentQt::BodyLabel(tr("控件 ID"), header), id);
    m_layout->addWidget(header);

    QFormLayout *geometry = addSection(tr("位置与尺寸"));
    const QString labels[4] = {QStringLiteral("X"), QStringLiteral("Y"), tr("宽度"), tr("高度")};
    for (int i = 0; i < 4; ++i) {
        auto *spin = new FluentQt::SpinBox(m_content);
        spin->setRange(i == 2 ? meta.minWidth : i == 3 ? meta.minHeight : -9999, 9999);
        spin->setKeyboardTracking(false);
        m_geometry[i] = spin;
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this, i](int value) {
            if (m_loading || !m_item) {
                return;
            }
            if (i < 2) {
                QPointF p = m_item->pos();
                (i == 0 ? p.rx() : p.ry()) = value;
                m_item->setPos(p);
            } else {
                QSizeF s = m_item->size();
                (i == 2 ? s.rwidth() : s.rheight()) = value;
                m_item->setSize(s);
            }
            if (CanvasItem *parent = m_item->containerParent()) {
                parent->relayoutChildren();
            }
            emit modified();
        });
        geometry->addRow(new FluentQt::BodyLabel(labels[i], m_content), spin);
    }
    refreshGeometry();
    m_loading = true;
    if (!(m_item->flags() & QGraphicsItem::ItemIsMovable)) {
        // A layout container places this child; only its size is the user's.
        m_geometry[0]->setEnabled(false);
        m_geometry[1]->setEnabled(false);
    }

    if (!meta.properties.isEmpty()) {
        QFormLayout *props = addSection(tr("属性"));
        for (const PropDef &def : meta.properties) {
            const QString name = def.name;
            QWidget *editor = createEditor(def, itemValue(def), [this, name](const QJsonValue &v) {
                commitProperty(name, v);
            });
            addRow(props, name, def.label, editor);
        }
    }
    if (!meta.bindProperty().isEmpty()) {
        buildBindingForm();
    }
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void PropertyEditor::buildBindingForm()
{
    QFormLayout *form = addSection(tr("数据绑定"));
    const QJsonObject binding = m_item->binding();
    const auto value = [&binding](const char *key, const QJsonValue &fallback) {
        const QJsonValue v = binding.value(QLatin1String(key));
        return v.isUndefined() || v.isNull() ? fallback : v;
    };
    const auto add = [&](const char *key, const QString &label, QWidget *editor) {
        m_bindingEditors.insert(QLatin1String(key), editor);
        addRow(form, QStringLiteral("binding.") + QLatin1String(key), label, editor);
    };
    const auto combo = [this](const QStringList &items, const QString &current) {
        auto *box = new FluentQt::ComboBox(m_content);
        box->addItems(items);
        box->setCurrentIndex(std::max(0, box->findText(current)));
        connect(box, &FluentQt::ComboBox::currentTextChanged, this, [this]() { commitBinding(); });
        return box;
    };
    const auto line = [this](const QString &text, const QString &placeholder) {
        auto *edit = new FluentQt::LineEdit(m_content);
        edit->setText(text);
        edit->setPlaceholderText(placeholder);
        connect(edit, &QLineEdit::textChanged, this, [this]() { commitBinding(); });
        return edit;
    };
    const auto spin = [this](int lo, int hi, int current) {
        auto *box = new FluentQt::SpinBox(m_content);
        box->setRange(lo, hi);
        box->setValue(current);
        connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this]() { commitBinding(); });
        return box;
    };

    add("parseMode", tr("解析模式"),
        combo({ParseMode::KeyValue, ParseMode::JsonPath, ParseMode::HexBytes, ParseMode::Modbus},
              value("parseMode", ParseMode::KeyValue).toString()));
    add("key", tr("绑定字段"), line(value("key", QString()).toString(), tr("字段名 (如 temperature)")));
    add("jsonPath", tr("JSON路径"), line(value("jsonPath", QString()).toString(), tr("路径 (如 data.sensors.0.temp)")));
    add("byteOffset", tr("字节偏移"), spin(0, 9999, value("byteOffset", 0).toInt()));
    add("dataType", tr("数据类型"),
        combo({QStringLiteral("uint8"), QStringLiteral("int8"), QStringLiteral("uint16"), QStringLiteral("int16"),
               QStringLiteral("uint32"), QStringLiteral("int32"), QStringLiteral("float32"), QStringLiteral("ASCII")},
              value("dataType", QStringLiteral("uint16")).toString()));
    add("byteOrder", tr("字节序"),
        combo({QStringLiteral("大端(BE)"), QStringLiteral("小端(LE)")},
              value("byteOrder", QStringLiteral("大端(BE)")).toString()));
    add("frameHeader", tr("帧头(HEX)"), line(value("frameHeader", QString()).toString(), tr("如 AA 55")));
    add("frameTail", tr("帧尾(HEX)"), line(value("frameTail", QString()).toString(), tr("如 0D 0A")));
    add("mbRegIndex", tr("寄存器索引"), spin(0, 255, value("mbRegIndex", 0).toInt()));
    add("mbDataType", tr("数据类型"),
        combo({QStringLiteral("uint16"), QStringLiteral("int16"), QStringLiteral("uint32"), QStringLiteral("int32"),
               QStringLiteral("float32")},
              value("mbDataType", QStringLiteral("uint16")).toString()));
    add("mbRegOrder", tr("寄存器序"),
        combo({QStringLiteral("大端(AB CD)"), QStringLiteral("小端(CD AB)")},
              value("mbRegOrder", QStringLiteral("大端(AB CD)")).toString()));
    add("mbFcFilter", tr("功能码过滤"), spin(-1, 255, value("mbFcFilter", -1).toInt()));
    add("mbSlaveFilter", tr("从站过滤"), spin(-1, 247, value("mbSlaveFilter", -1).toInt()));
    add("expression", tr("转换表达式"), line(value("expression", QString()).toString(), tr("如 value * 0.1")));
}

void PropertyEditor::commitBinding()
{
    if (m_loading || !m_item) {
        return;
    }
    const auto textOf = [this](const char *key) {
        QWidget *editor = m_bindingEditors.value(QLatin1String(key));
        if (auto *combo = qobject_cast<FluentQt::ComboBox *>(editor)) {
            return combo->currentText();
        }
        if (auto *edit = qobject_cast<QLineEdit *>(editor)) {
            return edit->text().trimmed();
        }
        return QString();
    };
    const auto intOf = [this](const char *key) {
        auto *spin = qobject_cast<QSpinBox *>(m_bindingEditors.value(QLatin1String(key)));
        return spin ? spin->value() : 0;
    };
    const QString mode = textOf("parseMode");
    const bool configured = (mode == ParseMode::KeyValue && !textOf("key").isEmpty()) ||
                            (mode == ParseMode::JsonPath && !textOf("jsonPath").isEmpty()) ||
                            mode == ParseMode::HexBytes || mode == ParseMode::Modbus;
    if (!configured) {
        m_item->setBinding({});
    } else {
        QJsonObject binding{
            {QStringLiteral("parseMode"), mode},
            {QStringLiteral("key"), textOf("key")},
            {QStringLiteral("jsonPath"), textOf("jsonPath")},
            {QStringLiteral("byteOffset"), intOf("byteOffset")},
            {QStringLiteral("dataType"), textOf("dataType")},
            {QStringLiteral("byteOrder"), textOf("byteOrder")},
            {QStringLiteral("frameHeader"), textOf("frameHeader")},
            {QStringLiteral("frameTail"), textOf("frameTail")},
            {QStringLiteral("mbRegIndex"), intOf("mbRegIndex")},
            {QStringLiteral("mbDataType"), textOf("mbDataType")},
            {QStringLiteral("mbRegOrder"), textOf("mbRegOrder")},
            {QStringLiteral("mbFcFilter"), intOf("mbFcFilter")},
            {QStringLiteral("mbSlaveFilter"), intOf("mbSlaveFilter")},
        };
        const QString expression = textOf("expression");
        binding.insert(QStringLiteral("expression"), expression.isEmpty() ? QJsonValue() : QJsonValue(expression));
        m_item->setBinding(binding);
    }
    updateVisibility();
    emit modified();
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

void PropertyEditor::updateVisibility()
{
    if (!m_item) {
        if (m_project) {
            const QString parser = m_project->parserType();
            setRowVisible(QStringLiteral("separator"), parser == QLatin1String("line"));
            setRowVisible(QStringLiteral("header"), parser == QLatin1String("hex"));
            setRowVisible(QStringLiteral("footer"), parser == QLatin1String("hex"));
        }
        return;
    }
    const WidgetMeta &meta = m_item->meta();
    if (meta.prop(QStringLiteral("commandMode"))) {
        const QString mode = m_item->propertyValue(QStringLiteral("commandMode")).toString();
        const QSet<QString> shown = modeVisibility().value(mode);
        for (const QString &field : modeFields()) {
            setRowVisible(field, shown.contains(field));
        }
        const QString toggle = sendToggle(meta.typeName);
        if (!toggle.isEmpty() && !m_item->propertyValue(toggle).toBool()) {
            for (const QString &field : modeFields()) {
                setRowVisible(field, false);
            }
            setRowVisible(QStringLiteral("commandMode"), false);
            setRowVisible(QStringLiteral("commands"), false);
        } else {
            setRowVisible(QStringLiteral("commandMode"), true);
            setRowVisible(QStringLiteral("commands"), true);
        }
    }
    if (meta.prop(QStringLiteral("notify"))) {
        const bool notify = m_item->propertyValue(QStringLiteral("notify")).toBool();
        for (const char *field : {"notifyTitle", "notifyContent", "notifyType", "notifyDuration"}) {
            setRowVisible(QLatin1String(field), notify);
        }
    }
    if (meta.typeName == QLatin1String("RealtimeChart")) {
        const QString mode =
            m_item->properties().value(storageKey(QStringLiteral("curveParseMode"))).toString(ParseMode::KeyValue);
        const QSet<QString> shown = chartParseVisibility().value(mode);
        for (const QSet<QString> &fields : chartParseVisibility()) {
            for (const QString &field : fields) {
                setRowVisible(field, shown.contains(field));
            }
        }
    }
    if (!m_bindingEditors.isEmpty()) {
        const auto *combo = qobject_cast<FluentQt::ComboBox *>(m_bindingEditors.value(QStringLiteral("parseMode")));
        const QString mode = combo ? combo->currentText() : ParseMode::KeyValue;
        setRowVisible(QStringLiteral("binding.key"), mode == ParseMode::KeyValue);
        setRowVisible(QStringLiteral("binding.jsonPath"), mode == ParseMode::JsonPath);
        for (const char *field : {"byteOffset", "dataType", "byteOrder", "frameHeader", "frameTail"}) {
            setRowVisible(QStringLiteral("binding.") + QLatin1String(field), mode == ParseMode::HexBytes);
        }
        for (const char *field : {"mbRegIndex", "mbDataType", "mbRegOrder", "mbFcFilter", "mbSlaveFilter"}) {
            setRowVisible(QStringLiteral("binding.") + QLatin1String(field), mode == ParseMode::Modbus);
        }
    }
}

} // namespace fhmi
