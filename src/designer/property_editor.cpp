#include "designer/property_editor.h"
#include "designer/project_logic_dialog.h"

#include "comm/modbus_slave.h"
#include "comm/can/can_backend.h"
#include "core/binding.h"
#include "core/bit_profile.h"
#include "core/industrial_catalog.h"
#include "core/dbc.h"
#include "core/i18n.h"
#include "core/protocol_parser.h"
#include "designer/canvas_item.h"
#include "designer/design_scene.h"
#include "runtime/hmi_widget.h"
#include "runtime/field_help.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/CardWidget.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/Menu.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <FluentQtWidgets/Widgets/SwitchButton.h>

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QLocale>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringListModel>
#include <QtGui/QPainter>
#include <QtGui/QFocusEvent>
#include <QtGui/QRegularExpressionValidator>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCompleter>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fhmi {

namespace {

class CompactDoubleSpinBox : public FluentQt::DoubleSpinBox
{
  public:
    using FluentQt::DoubleSpinBox::DoubleSpinBox;

  protected:
    QString textFromValue(double value) const override
    {
        QString text = FluentQt::DoubleSpinBox::textFromValue(value);
        const QString decimal = locale().decimalPoint();
        if (text.contains(decimal)) {
            const QString zero = locale().zeroDigit();
            while (text.endsWith(zero))
                text.chop(zero.size());
            if (text.endsWith(decimal))
                text.chop(decimal.size());
        }
        return text;
    }
};

// The same searchable field is used by widget bindings and individual chart curves.
class BindingKeyEdit : public FluentQt::EditableComboBox
{
  public:
    explicit BindingKeyEdit(QWidget *parent) : FluentQt::EditableComboBox(parent), m_names(this)
    {
        auto *suggestions = new QCompleter(&m_names, this);
        suggestions->setCaseSensitivity(Qt::CaseInsensitive);
        suggestions->setFilterMode(Qt::MatchContains);
        suggestions->setMaxVisibleItems(12);
        setCompleter(suggestions);
        setMaxVisibleItems(12);
    }

    void setNames(const QStringList &names, const QString &placeholder)
    {
        setPlaceholderText(placeholder);
        if (names == m_names.stringList())
            return;
        const QSignalBlocker blocker(this);
        const QString current = text();
        clear();
        addItems(names);
        m_names.setStringList(names);
        setCurrentText(current);
    }

  private:
    QStringListModel m_names;
};

QStringList dataPointNames(const Project *project)
{
    QStringList names;
    if (project)
        for (const auto &point : project->logic.value(QStringLiteral("points")).toArray())
            names.append(point.toObject().value(QStringLiteral("name")).toString());
    return names;
}

// Command fields shown for each send mode, as in PyQt UI Designer.
const QHash<QString, QSet<QString>> &modeVisibility()
{
    static const QHash<QString, QSet<QString>> table = {
        {QStringLiteral("文本"), // i18n: value
         {QStringLiteral("command"), QStringLiteral("onCommand"), QStringLiteral("offCommand"),
          QStringLiteral("commandNewline"), QStringLiteral("commandTemplate")}},
        {QStringLiteral("HEX"),
         {QStringLiteral("command"), QStringLiteral("onCommand"), QStringLiteral("offCommand"),
          QStringLiteral("commandTemplate"), QStringLiteral("frameHeader"), QStringLiteral("frameTail"),
          QStringLiteral("dataType"), QStringLiteral("byteOrder")}},
        {QStringLiteral("JSON"),
         {QStringLiteral("command"), QStringLiteral("onCommand"), QStringLiteral("offCommand"),
          QStringLiteral("commandTemplate"), QStringLiteral("commandJsonNewline")}},
        {QStringLiteral("S7"), {QStringLiteral("commandTargetId"), QStringLiteral("s7Command")}},
        {QStringLiteral("Modbus"),
         {QStringLiteral("mbSlave"), QStringLiteral("mbFunc"), QStringLiteral("mbAddress"), QStringLiteral("mbValue"),
          QStringLiteral("mbDataType"), QStringLiteral("mbRegOrder"), QStringLiteral("mbArea")}},
        // A message with signals takes Signal=value assignments in the template, a raw ID HEX mode's bytes.
        {QStringLiteral("CAN"),
         {QStringLiteral("command"), QStringLiteral("onCommand"), QStringLiteral("offCommand"),
          QStringLiteral("commandTemplate"), QStringLiteral("dataType"), QStringLiteral("byteOrder"),
          QStringLiteral("canId"), QStringLiteral("canFd"), QStringLiteral("canCycle")}},
    };
    return table;
}

const QSet<QString> &modeFields()
{
    static const QSet<QString> fields = {
        QStringLiteral("command"),        QStringLiteral("onCommand"),       QStringLiteral("offCommand"),
        QStringLiteral("commandNewline"), QStringLiteral("commandTemplate"), QStringLiteral("commandJsonNewline"),
        QStringLiteral("frameHeader"),    QStringLiteral("frameTail"),       QStringLiteral("dataType"),
        QStringLiteral("byteOrder"),      QStringLiteral("mbSlave"),         QStringLiteral("mbFunc"),
        QStringLiteral("mbAddress"),      QStringLiteral("mbValue"),         QStringLiteral("mbDataType"),
        QStringLiteral("mbRegOrder"),     QStringLiteral("canId"),           QStringLiteral("canFd"),
        QStringLiteral("canCycle"), QStringLiteral("s7Command"), QStringLiteral("mbArea")};
    return fields;
}

// What the project's CAN cards decode with, for completions: the signal keys a binding can use and the message
// names a CAN command can name. Each card's DBC is read again only when the file changed.
struct CanNames
{
    QStringList keys;
    QStringList messages;
    QList<CanSignalSet> sets; // one per CAN card, to resolve a canId as the link does
};

// The widgets as they are now. In the designer that is the scene: the project's list is brought up to date only
// when the project is saved, so a CAN card dropped since would offer no DBC names and no CAN信号 mode.
QJsonArray currentWidgets(const Project *project, const DesignScene *scene)
{
    if (scene) {
        return scene->widgetsJson();
    }
    return project ? project->widgets : QJsonArray();
}

QString endpointProperty(const QString &connection)
{
    if (connection == QLatin1String("commandTargetId"))
        return QStringLiteral("commandEndpoint");
    if (connection == QLatin1String("curveSourceCardId"))
        return QStringLiteral("curveSourceEndpoint");
    if (connection == QLatin1String("sourceCardId"))
        return QStringLiteral("sourceEndpoint");
    return {};
}

class ConnectionSourceEdit : public FluentQt::ComboBox
{
  public:
    using FluentQt::ComboBox::ComboBox;
    enum class Endpoint { None, Address, Topic, Either };

    Endpoint endpointKind() const
    {
        const QString id = selectedValue(this);
        return id.isEmpty() ? m_defaultEndpoint : m_endpoints.value(id, Endpoint::None);
    }

    void setSources(const Project *project, const DesignScene *scene, RouteTransport transport, const QString &current)
    {
        if (m_transport == transport)
            return;
        m_transport = transport;
        const QSignalBlocker blocker(this);
        clear();
        m_endpoints.clear();
        m_defaultEndpoint = Endpoint::None;
        addItem(transport == RouteTransport::S7 ? PropertyEditor::tr("请选择 S7 连接")
                                                : PropertyEditor::tr("全部匹配连接（0.8 之前默认）"),
                QIcon(), QString());
        visitWidgets(currentWidgets(project, scene), [this, transport](const QJsonObject &widget) {
            const QString type = widget.value(QStringLiteral("type")).toString();
            const QString id = widget.value(QStringLiteral("id")).toString();
            if (!id.isEmpty() && supportsRouteTransport(type, transport) &&
                (transport != RouteTransport::ModbusSlave ||
                 widget.value(QStringLiteral("properties")).toObject().value(QStringLiteral("modbusRole")).toString() ==
                     ModbusRole::Slave)) {
                addItem(id + QStringLiteral(" · ") + type, QIcon(), id);
                if (allowsRouteEndpoint(transport) && supportsRouteEndpoint(type)) {
                    const auto kind = type == QLatin1String("MqttConfigCard") ? Endpoint::Topic : Endpoint::Address;
                    m_endpoints.insert(id, kind);
                    if (m_defaultEndpoint == Endpoint::None)
                        m_defaultEndpoint = kind;
                    else if (m_defaultEndpoint != kind)
                        m_defaultEndpoint = Endpoint::Either;
                }
            }
            return true;
        });
        if (!current.isEmpty() && findData(current) < 0) {
            addItem(PropertyEditor::tr("%1（当前模式不可用）").arg(current), QIcon(), current);
            setItemEnabled(count() - 1, false);
        }
        selectValue(this, current); // Keep an unavailable route visible until the user picks its replacement.
    }

  private:
    std::optional<RouteTransport> m_transport;
    QHash<QString, Endpoint> m_endpoints;
    Endpoint m_defaultEndpoint = Endpoint::None;
};

bool hasCanCard(const QJsonArray &widgets)
{
    return !visitWidgets(widgets, [](const QJsonObject &widget) {
        return !supportsRouteTransport(widget.value(QStringLiteral("type")).toString(), RouteTransport::Can);
    });
}

CanNames canNames(const Project *project, const QJsonArray &widgets, const QString &connectionId)
{
    CanNames names;
    if (!project) {
        return names;
    }
    struct Cached
    {
        QDateTime modified;
        std::shared_ptr<const CanDatabase> dbc;
    };
    static QHash<QString, Cached> cache;
    const DataRoute route{connectionId, {}};
    visitWidgets(widgets, [&](const QJsonObject &widget) {
        if (!supportsRouteTransport(widget.value(QStringLiteral("type")).toString(), RouteTransport::Can) ||
            !route.acceptsConnection(widget.value(QStringLiteral("id")).toString()))
            return true;
        const auto card = widget.value(QStringLiteral("properties")).toObject();
        std::shared_ptr<const CanDatabase> dbc;
        const QString file = card.value(QStringLiteral("dbcFile")).toString().trimmed();
        if (!file.isEmpty()) {
            const QString path = QDir(project->assetBaseDir()).absoluteFilePath(file);
            const QDateTime modified = QFileInfo(path).lastModified();
            Cached &entry = cache[path];
            if (!entry.dbc || entry.modified != modified) {
                const auto loaded = Dbc::load(path);
                entry = {modified, loaded ? std::make_shared<const CanDatabase>(*loaded) : nullptr};
            }
            dbc = entry.dbc;
        }
        if (const auto set = CanSignalSet::build(dbc, card.value(QStringLiteral("canSignals")).toString())) {
            names.keys += set->signalKeys();
            names.messages += set->messageNames();
            names.sets.append(*set);
        }
        return true;
    });
    names.keys.removeDuplicates();
    names.messages.removeDuplicates();
    return names;
}

// What a canId stands for on the project's CAN cards, resolved as CanLink::prepare does. A message with signals
// takes dataType and byteOrder from nowhere (its bytes come from the signals); a DBC message also takes its FD
// from the DBC, while an ID with inline signals still honours the command's canFd.
struct CanTargetKind
{
    bool usesRawEncoding = true;
    bool usesFdSetting = true;
};

CanTargetKind canTargetKind(const CanNames &names, const QString &canId)
{
    CanTargetKind kind;
    bool resolved = false;
    for (const CanSignalSet &set : names.sets) {
        if (const auto target = set.target(canId)) {
            if (!resolved) {
                kind = {false, false};
                resolved = true;
            }
            kind.usesRawEncoding |= !target->hasSignals;
            kind.usesFdSetting |= !target->hasSignals || target->name.isEmpty();
        }
    }
    return kind;
}

void complete(FluentQt::LineEdit *edit, const QStringList &words)
{
    if (auto *completer = edit->completer()) {
        auto *names = static_cast<QStringListModel *>(completer->model());
        if (names->stringList() != words)
            names->setStringList(words);
        return;
    }
    if (words.isEmpty()) {
        return;
    }
    auto *names = new QStringListModel(words, edit);
    auto *completer = new QCompleter(names, edit);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setMaxVisibleItems(12);
    edit->setCompleter(completer);
    // Leaving the field (Tab, a click elsewhere) also closes the list, so it never outlives the edit.
    QObject::connect(edit, &QLineEdit::editingFinished, completer, [completer]() {
        if (QAbstractItemView *popup = completer->popup()) {
            popup->hide();
        }
    });
}

const QHash<QString, QSet<QString>> &chartParseVisibility()
{
    static const QHash<QString, QSet<QString>> table = {
        {ParseMode::KeyValue, {QStringLiteral("curveKey")}},
        {ParseMode::DataPoint, {QStringLiteral("curveKey")}},
        {ParseMode::JsonPath, {QStringLiteral("curveJsonPath")}},
        {ParseMode::HexBytes,
         {QStringLiteral("curveByteOffset"), QStringLiteral("curveDataType"), QStringLiteral("curveByteOrder"),
          QStringLiteral("curveFrameHeader"), QStringLiteral("curveFrameTail")}},
        {ParseMode::S7, {QStringLiteral("curveS7")}},
        {ParseMode::Modbus,
         {QStringLiteral("curveMbArea"), QStringLiteral("curveRegIndex"), QStringLiteral("curveMbDataType"),
          QStringLiteral("curveMbRegOrder"), QStringLiteral("curveMbFcFilter"), QStringLiteral("curveMbSlaveFilter")}},
        {ParseMode::CanSignal,
         {QStringLiteral("curveCanId"), QStringLiteral("curveCanStartBit"), QStringLiteral("curveCanBitLength"),
          QStringLiteral("curveCanByteOrder"), QStringLiteral("curveCanSigned"), QStringLiteral("curveCanFactor"),
          QStringLiteral("curveCanOffset")}},
    };
    return table;
}

// Virtual chart property -> per-curve storage suffix.
const QHash<QString, QString> &curveFields()
{
    static const QHash<QString, QString> table = {
        {QStringLiteral("curveName"), QStringLiteral("name")},
        {QStringLiteral("curveSourceCardId"),QStringLiteral("sourceCardId")},
        {QStringLiteral("curveSourceEndpoint"),QStringLiteral("sourceEndpoint")},
        {QStringLiteral("curveS7"),QStringLiteral("s7")},
        {QStringLiteral("curveExpression"), QStringLiteral("expression")},
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
        {QStringLiteral("curveMbArea"), QStringLiteral("mbArea")},
        {QStringLiteral("curveCanId"), QStringLiteral("canId")},
        {QStringLiteral("curveCanStartBit"), QStringLiteral("canStartBit")},
        {QStringLiteral("curveCanBitLength"), QStringLiteral("canBitLength")},
        {QStringLiteral("curveCanByteOrder"), QStringLiteral("canByteOrder")},
        {QStringLiteral("curveCanSigned"), QStringLiteral("canSigned")},
        {QStringLiteral("curveCanFactor"), QStringLiteral("canFactor")},
        {QStringLiteral("curveCanOffset"), QStringLiteral("canOffset")},
    };
    return table;
}

// What a factor or offset field accepts. Empty is the default, shown as the placeholder.
const QRegularExpression &numberPattern()
{
    static const QRegularExpression number(QStringLiteral("[-+]?(\\d+\\.?\\d*|\\.\\d+)([eE][-+]?\\d+)?"));
    return number;
}

// The red caption that says why a CAN信号 setup reads nothing; its text is set by updateVisibility. It fixes its own
// height to what its wrapped lines need at its width. Left to the form, a note that appears after an edit can be
// laid out at a wider width's height and lose lines: QFormLayout keeps one table of wrapped heights for two widths.
class ProblemLabel : public FluentQt::CaptionLabel
{
  public:
    explicit ProblemLabel(QWidget *parent) : FluentQt::CaptionLabel(parent)
    {
        setWordWrap(true);
        setTextColor(QColor(0xC4, 0x2B, 0x1C), QColor(0xFF, 0x99, 0xA4)); // Fluent's critical colours
    }

    void setProblem(const QString &problem)
    {
        setText(problem);
        fitHeight();
    }

  protected:
    void resizeEvent(QResizeEvent *event) override
    {
        FluentQt::CaptionLabel::resizeEvent(event);
        fitHeight();
    }

  private:
    // Measured from the font, not heightForWidth(), which never goes below the fixed height already set.
    void fitHeight()
    {
        if (text().isEmpty()) {
            return;
        }
        const QMargins margins = contentsMargins();
        const int extra = 2 * margin();
        const int textWidth = std::max(1, width() - margins.left() - margins.right() - extra);
        const QRect lines = fontMetrics().boundingRect(QRect(0, 0, textWidth, QWIDGETSIZE_MAX),
                                                       int(alignment() | Qt::TextWordWrap), text());
        setFixedHeight(lines.height() + margins.top() + margins.bottom() + extra);
    }
};

ProblemLabel *newProblemLabel(QWidget *parent) { return new ProblemLabel(parent); }

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

// The rows a "line" parserConfig shows, then those of a "hex" one.
const char *const kLineFrameKeys[] = {"separator", "terminator", "kvSeparator", "encoding"};
const char *const kHexFrameKeys[] = {"header",        "footer",         "lengthOffset", "lengthSize",
                                     "lengthEndian",  "lengthCovers",   "fixedLength",  "checksum",
                                     "checksumStart", "checksumEndian", "idleTimeout",  "fields"};

// A record terminator as the editor shows it: control characters and the backslash as escapes.
QString escapedTerminator(const QString &text)
{
    QString out;
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '\\':
            out += QStringLiteral("\\\\");
            break;
        case '\n':
            out += QStringLiteral("\\n");
            break;
        case '\r':
            out += QStringLiteral("\\r");
            break;
        case '\t':
            out += QStringLiteral("\\t");
            break;
        default:
            out += c;
        }
    }
    return out;
}

// The terminator typed or picked: \n \r \t and \\ are escapes, any other backslash is itself.
QString unescapedTerminator(const QString &text)
{
    QString out;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar next = i + 1 < text.size() ? text.at(i + 1) : QChar();
        if (text.at(i) == QLatin1Char('\\') && QStringLiteral("nrt\\").contains(next)) {
            out += next == QLatin1Char('n') ? QLatin1Char('\n')
                   : next == QLatin1Char('r') ? QLatin1Char('\r')
                   : next == QLatin1Char('t') ? QLatin1Char('\t')
                                              : QLatin1Char('\\');
            ++i;
        } else {
            out += text.at(i);
        }
    }
    return out;
}

// A HEX field table as the editor shows it: one field per line, 名字,偏移,长度,类型[,字节序].
QString hexFieldsText(const QJsonArray &fields)
{
    QStringList lines;
    for (const QJsonValue &value : fields) {
        QStringList parts;
        for (const QJsonValue &part : value.toArray()) {
            parts << part.toVariant().toString();
        }
        lines << parts.join(QLatin1Char(','));
    }
    return lines.join(QLatin1Char('\n'));
}

// The fields in that text. *badLine is 0 when every line is a field the parser takes, else
// the number (from 1) of the first line that is not, so a project never stores a table it
// could not load. '#' starts a comment and a full-width comma counts, as in the poll table.
QJsonArray hexFieldsFromText(const QString &text, int *badLine)
{
    QJsonArray fields;
    *badLine = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        QStringList parts =
            line.replace(QStringLiteral("，"), QStringLiteral(",")).split(QLatin1Char(',')); // i18n: ignore
        for (QString &part : parts) {
            part = part.trimmed();
        }
        bool offsetOk = false;
        bool lengthOk = false;
        QJsonArray item{parts.value(0), parts.value(1).toInt(&offsetOk), parts.value(2).toInt(&lengthOk),
                        parts.value(3)};
        if (parts.size() == 5) {
            item.append(parts.at(4));
        }
        // Built step by step: QJsonArray{item} would be a copy of item, not an array holding it.
        QJsonArray one;
        one.append(item);
        if ((parts.size() != 4 && parts.size() != 5) || parts.at(0).isEmpty() || !offsetOk || !lengthOk ||
            !createParser(QStringLiteral("hex"), QJsonObject{{QStringLiteral("fields"), one}})) {
            *badLine = i + 1;
            return {};
        }
        fields.append(item);
    }
    return fields;
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
        connect(m_item, &CanvasItem::editorPageChanged, this, [this] {
            QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
        });
    }
    rebuild();
}

void PropertyEditor::clearContent()
{
    m_rows.clear();
    m_helpButtons.clear();
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
        if (m_bindingOnly)
            buildBindingForm();
        else
            buildItemForm();
    } else {
        buildProjectForm();
    }
    m_layout->addStretch();
    updateVisibility();
    m_loading = false;
    // setText leaves the cursor at the end, which scrolls a text longer than its field to its tail ("…ssure pump station")
    for (QLineEdit *edit : m_content->findChildren<QLineEdit *>()) {
        edit->setCursorPosition(0);
    }
}

QFormLayout *PropertyEditor::addSection(const QString &title)
{
    auto *card = new FluentQt::SimpleCardWidget(m_content);
    auto *form = new QFormLayout(card);
    form->setContentsMargins(12, 10, 12, 10);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->addRow(new FluentQt::StrongBodyLabel(title, card));
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_layout->addWidget(card);
    return form;
}

void PropertyEditor::addRow(QFormLayout *form, const QString &key, const QString &label, QWidget *editor,
                           const QString &helpKey)
{
    auto *caption = new FluentQt::BodyLabel(label, m_content);
    // A long label (English "Transform expression") wraps instead of widening the panel past its dock
    caption->setWordWrap(true);
    QWidget *labelWidget = caption;
    if (const auto topic = fieldHelpTopic(helpKey)) {
        labelWidget = new QWidget(m_content);
        auto *layout = new QHBoxLayout(labelWidget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        layout->addWidget(caption, 1);
        auto *help = createFieldHelpButton(*topic, labelWidget);
        help->setObjectName(QStringLiteral("propertyHelp_%1").arg(key));
        layout->addWidget(help, 0, Qt::AlignVCenter);
        m_helpButtons.insert(key, help);
    }
    form->addRow(labelWidget, editor);
    if (!key.isEmpty()) {
        m_rows.insert(key, {labelWidget, editor});
    }
    if (key.endsWith(QLatin1String("Endpoint"))) {
        addRow(form, key + QStringLiteral("Problem"), QString(), newProblemLabel(m_content));
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

void PropertyEditor::updateRouteEndpoint(const QString &connection, const QString &endpoint, bool active)
{
    auto *edit = qobject_cast<FluentQt::LineEdit *>(m_rows.value(endpoint).second);
    if (!edit)
        return;
    const auto *select = static_cast<ConnectionSourceEdit *>(m_rows.value(connection).second);
    const auto kind = select->endpointKind();
    const bool available = kind != ConnectionSourceEdit::Endpoint::None;
    const bool problem = active && !available && !edit->text().isEmpty();
    const QString problemKey = endpoint + QStringLiteral("Problem");
    auto *message = static_cast<ProblemLabel *>(m_rows.value(problemKey).second);
    message->setProblem(problem ? tr("当前连接或模式不支持子通道。清空此项，或重新选择连接。") : QString());
    edit->setError(problem);
    if (kind == ConnectionSourceEdit::Endpoint::Topic)
        edit->setPlaceholderText(tr("如 device/data"));
    else if (kind == ConnectionSourceEdit::Endpoint::Address)
        edit->setPlaceholderText(tr("如 192.168.1.20:502"));
    else
        edit->setPlaceholderText(tr("地址:端口或 MQTT Topic"));
    setRowVisible(endpoint, active && (available || problem));
    setRowVisible(problemKey, problem);
}

QWidget *PropertyEditor::createEditor(const PropDef &def, const QJsonValue &value, const Commit &commit)
{
    if (def.name == QLatin1String("curveKey")) {
        auto *edit = new BindingKeyEdit(m_content);
        edit->setCurrentText(value.toString());
        connect(edit, &FluentQt::EditableComboBox::currentTextChanged, this,
                [commit](const QString &text) { commit(text); });
        return edit;
    }
    if (def.name == QLatin1String("commandTargetId") || def.name == QLatin1String("curveSourceCardId") ||
        def.name == QLatin1String("sourceCardId")) {
        const QString type = m_item->typeName();
        RouteTransport transport = widgetReceiveTransport(type);
        if (def.name == QLatin1String("commandTargetId")) {
            transport = commandRouteTransport(m_item->propertyValue(QStringLiteral("commandMode")).toString(), type);
        } else if (def.name == QLatin1String("curveSourceCardId")) {
            transport = bindingRouteTransport(m_item->properties().value(storageKey(QStringLiteral("curveParseMode"))).toString());
        }
        auto *box = new ConnectionSourceEdit(m_content);
        box->setSources(m_project, m_scene, transport, value.toString());
        connect(box, &FluentQt::ComboBox::currentIndexChanged, this, [box, commit] { commit(selectedValue(box)); });
        return box;
    }

    if(def.name==QLatin1String("pulsesPerTurn")) {
        auto *edit=new FluentQt::LineEdit(m_content);edit->setText(QString::number(value.toDouble(1024),'f',0));
        edit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]+")),edit));
        connect(edit,&QLineEdit::textChanged,this,[commit](const QString &text){bool ok=false;const double value=text.toDouble(&ok);if(ok&&value>0&&value<=9007199254740991.0)commit(value);});
        return edit;
    }
    switch (def.type) {
    case PropType::Int:
    case PropType::Float: {
        if (def.name == QLatin1String("curveCanFactor") || def.name == QLatin1String("curveCanOffset")) {
            // A spin box rounds to its decimals, and a DBC's factor can be 0.0001 or finer: a text field keeps
            // what is typed. Empty is the default, which is not stored.
            auto *edit = new FluentQt::LineEdit(m_content);
            edit->setText(QString::number(value.toDouble(), 'g', QLocale::FloatingPointShortest));
            edit->setPlaceholderText(QString::number(def.defaultValue.toDouble()));
            edit->setValidator(new QRegularExpressionValidator(numberPattern(), edit));
            connect(edit, &QLineEdit::textChanged, this, [commit](const QString &text) {
                bool ok = false;
                const double number = text.toDouble(&ok);
                commit(ok ? QJsonValue(number) : QJsonValue(QJsonValue::Undefined));
            });
            return edit;
        }
        const bool spinParameter = m_item && m_item->meta().typeName == QLatin1String("SpinBox") &&
                                   (def.name == QLatin1String("minimum") || def.name == QLatin1String("maximum") ||
                                    def.name == QLatin1String("value") || def.name == QLatin1String("singleStep"));
        const bool decimalInput = spinParameter &&
                                  m_item->propertyValue(QStringLiteral("numberMode")).toString() == QStringLiteral("小数"); // i18n: value
        const bool integral = def.type == PropType::Int || (spinParameter && !decimalInput);
        QAbstractSpinBox *box = nullptr;
        if (integral) {
            auto *spin = new FluentQt::SpinBox(m_content);
            spin->setRange(int(std::ceil(def.minimum.value_or(-999999))), int(def.maximum.value_or(999999)));
            spin->setValue(value.isString() ? value.toString().toInt() : value.toInt());
            connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
                    [commit](int v) { commit(QJsonValue(v)); });
            box = spin;
        } else {
            auto *spin = new CompactDoubleSpinBox(m_content);
            spin->setDecimals(decimalInput ? m_item->propertyValue(QStringLiteral("decimals")).toInt(2)
                                          : m_item && isIndustrialCategory(m_item->meta().category) ? 6 : 3);
            spin->setRange(def.minimum.value_or(-999999), def.maximum.value_or(999999));
            if (decimalInput) {
                const double unit = std::pow(10., -spin->decimals());
                spin->setSingleStep(unit);
                if (def.name == QLatin1String("singleStep"))
                    spin->setMinimum(unit);
            }
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
        QStringList options = def.options;
        if (def.name == QLatin1String("curveParseMode") && !hasCanCard(currentWidgets(m_project, m_scene)) &&
            value.toVariant().toString() != ParseMode::CanSignal) {
            options.removeAll(ParseMode::CanSignal); // offered where there is a CAN card, as for a binding
        }
        addValueItems(combo, options);
        if (def.name == QLatin1String("defaultParser") && m_project)
            showInheritedParser(combo, m_project->parserType());
        if (!selectValue(combo, value.toVariant().toString())) {
            combo->setCurrentIndex(0);
        }
        connect(combo, &FluentQt::ComboBox::currentIndexChanged, this,
                [combo, commit]() { commit(selectedValue(combo)); });
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
    case PropType::Color: {
        auto *row = new QWidget(m_content);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        auto *edit = new FluentQt::LineEdit(row);
        edit->setText(value.toString());
        auto *pick = new FluentQt::ToolButton(row);
        pick->setAccessibleName(tr("选择颜色"));
        fluentToolTip(pick, tr("选择颜色"));
        keepSquareBeside(pick, edit);
        layout->addWidget(edit, 1);
        layout->addWidget(pick);
        const auto swatch = [pick](const QString &text) {
            QPixmap image(24, 24);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            const QColor color(text);
            painter.setPen(QColor("#808080"));
            painter.setBrush(color.isValid() ? color : Qt::transparent);
            painter.drawRoundedRect(QRectF(2, 2, 19, 19), 3, 3);
            painter.end();
            pick->setIcon(QIcon(image));
        };
        swatch(edit->text());
        connect(edit, &QLineEdit::textChanged, this, [commit, swatch](const QString &text) {
            swatch(text);
            commit(text);
        });
        connect(pick, &QToolButton::clicked, this, [this, edit] {
            const QColor color = chooseColor(QColor(edit->text()), tr("选择颜色"), this);
            if (color.isValid())
                edit->setText(color.name());
        });
        return row;
    }
    case PropType::String:
    case PropType::Command:
    case PropType::Object: // not reached: an object has a section of its own
        break;
    }

    if (def.name == QLatin1String("bitProfile")) {
        auto *combo = new FluentQt::ComboBox(m_content);
        QStringList names = m_project ? m_project->bitProfiles.keys() : QStringList();
        const QString current = value.toString(QStringLiteral("default"));
        if (!names.contains(QStringLiteral("default"))) names.prepend(QStringLiteral("default"));
        if (!names.contains(current)) names.append(current);
        combo->addItems(names);combo->setCurrentText(current);
        connect(combo,&FluentQt::ComboBox::currentTextChanged,this,[commit](const QString &name){commit(name);});
        return combo;
    }
    auto *edit = new FluentQt::LineEdit(m_content);
    edit->setText(value.toVariant().toString());
    connect(edit, &QLineEdit::textChanged, this, [commit](const QString &t) { commit(t); });
    const bool dbc = def.name == QLatin1String("dbcFile");
    if (def.name != QLatin1String("imagePath") && !dbc) {
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
    connect(browse, &QToolButton::clicked, this, [this, edit, dbc]() {
        const QString path =
            dbc ? QFileDialog::getOpenFileName(this, tr("选择 DBC 文件"), QString(), tr("DBC 文件 (*.dbc)"))
                : QFileDialog::getOpenFileName(this, tr("选择图片"), QString(),
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

void PropertyEditor::buildBitProfilesForm()
{
    if (!m_project)
        return;
    if (m_item)
        m_bitProfileName = m_item->propertyValue(QStringLiteral("bitProfile")).toString(QStringLiteral("default"));
    auto *form = addSection(tr("共享位配置"));
    auto *selector = new FluentQt::ComboBox(m_content);
    QStringList names = m_project->bitProfiles.keys();
    if (!names.contains(QStringLiteral("default")))
        names.prepend(QStringLiteral("default"));
    if (!names.contains(m_bitProfileName))
        names.append(m_bitProfileName);
    selector->addItems(names);
    selector->setCurrentText(m_bitProfileName);
    addRow(form, QStringLiteral("profile.select"), tr("配置名"), selector);
    connect(selector, &FluentQt::ComboBox::currentTextChanged, this, [this](const QString &name) {
        m_bitProfileName = name;
        if (m_item)
            commitProperty(QStringLiteral("bitProfile"), name);
        QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
    });
    const auto publish = [this](const QString &key) {
        if (m_scene)
            m_scene->setBitProfiles(m_project->bitProfiles);
        emit projectSettingsChanged(QStringLiteral("bitProfiles:") + m_bitProfileName + QLatin1Char(':') + key);
    };
    auto *name = new FluentQt::LineEdit(m_content);
    name->setText(m_bitProfileName);
    addRow(form, QStringLiteral("profile.name"), tr("新配置名"), name);
    auto *actions = new QWidget(m_content);
    auto *layout = new QHBoxLayout(actions);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *create = new FluentQt::PushButton(tr("新建配置"), actions);
    auto *rename = new FluentQt::PushButton(tr("重命名配置"), actions);
    layout->addWidget(create);
    layout->addWidget(rename);
    form->addRow(actions);
    connect(create, &QPushButton::clicked, this, [this, name, publish] {
        const QString target = name->text().trimmed();
        if (target.isEmpty() || m_project->bitProfiles.contains(target))
            return;
        m_project->bitProfiles.insert(target, effectiveBitProfile({}, QStringLiteral("default")));
        m_bitProfileName = target;
        if (m_item) {
            const QSignalBlocker blocker(m_scene);
            m_item->setPropertyValue(QStringLiteral("bitProfile"), target);
        }
        publish(QStringLiteral("create"));
        QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
    });
    connect(rename, &QPushButton::clicked, this, [this, name, publish] {
        const QString target = name->text().trimmed(), old = m_bitProfileName;
        if (target.isEmpty() || target == old || m_project->bitProfiles.contains(target))
            return;
        auto profile = m_project->bitProfiles.value(old).toObject();
        if (profile.isEmpty())
            profile = effectiveBitProfile({}, QStringLiteral("default"));
        m_project->bitProfiles.remove(old);
        m_project->bitProfiles.insert(target, profile);
        std::function<QJsonArray(QJsonArray)> rewrite = [&](QJsonArray widgets) {
            for (int i = 0; i < widgets.size(); ++i) {
                auto data = widgets.at(i).toObject();
                const QString type = data.value(QStringLiteral("type")).toString();
                auto props = data.value(QStringLiteral("properties")).toObject();
                if ((type == QLatin1String("StatusWordPanel") || type == QLatin1String("DigitalTimingChart")) &&
                    props.value(QStringLiteral("bitProfile")).toString(QStringLiteral("default")) == old) {
                    props.insert(QStringLiteral("bitProfile"), target);
                    data.insert(QStringLiteral("properties"), props);
                }
                if (data.contains(QStringLiteral("children")))
                    data.insert(QStringLiteral("children"), rewrite(data.value(QStringLiteral("children")).toArray()));
                widgets[i] = data;
            }
            return widgets;
        };
        m_project->widgets = rewrite(m_project->widgets);
        if (m_scene) {
            const QSignalBlocker blocker(m_scene);
            for (auto *item : m_scene->allItems())
                if (item->meta().prop(QStringLiteral("bitProfile")) &&
                    item->propertyValue(QStringLiteral("bitProfile")).toString() == old)
                    item->setPropertyValue(QStringLiteral("bitProfile"), target);
        }
        m_bitProfileName = target;
        publish(QStringLiteral("rename"));
        QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
    });
    const auto profile = effectiveBitProfile(m_project->bitProfiles, m_bitProfileName);
    auto *width = new FluentQt::ComboBox(m_content);
    width->addItems({QStringLiteral("8"), QStringLiteral("16")});
    width->setCurrentText(QString::number(profile.value(QStringLiteral("width")).toInt()));
    addRow(form, QStringLiteral("profile.width"), tr("位宽"), width);
    connect(width, &FluentQt::ComboBox::currentTextChanged, this, [this, publish](const QString &value) {
        auto data = m_project->bitProfiles.value(m_bitProfileName).toObject();
        data.insert(QStringLiteral("width"), value.toInt());
        m_project->bitProfiles.insert(m_bitProfileName, data);
        publish(QStringLiteral("width"));
        QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
    });
    const auto bits = profile.value(QStringLiteral("bits")).toArray();
    for (int i = 0; i < bits.size(); ++i) {
        auto *row = new QWidget(m_content);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *label = new FluentQt::LineEdit(row);
        auto *color = new FluentQt::LineEdit(row);
        auto *pick = new FluentQt::ToolButton(row);
        pick->setIcon(FluentQt::icon(FluentQt::FluentIcon::Palette));
        label->setText(bits.at(i).toObject().value(QStringLiteral("name")).toString());
        color->setText(bits.at(i).toObject().value(QStringLiteral("color")).toString());
        color->setMaximumWidth(92);
        h->addWidget(label, 1);
        h->addWidget(color);
        h->addWidget(pick);
        addRow(form, QStringLiteral("profile.bit%1").arg(i), QStringLiteral("B%1").arg(i), row);
        const auto commit = [this, publish, i, label, color] {
            auto data = m_project->bitProfiles.value(m_bitProfileName).toObject();
            auto values = data.value(QStringLiteral("bits")).toArray();
            const auto defaults = effectiveBitProfile(m_project->bitProfiles, m_bitProfileName);
            const auto defaultBits = defaults.value(QStringLiteral("bits")).toArray();
            while (values.size() < defaultBits.size())
                values.append(defaultBits.at(values.size()));
            auto bit = values.at(i).toObject();
            bit.insert(QStringLiteral("name"), label->text());
            bit.insert(QStringLiteral("color"), color->text());
            values[i] = bit;
            data.insert(QStringLiteral("width"), defaults.value(QStringLiteral("width")));
            data.insert(QStringLiteral("bits"), values);
            m_project->bitProfiles.insert(m_bitProfileName, data);
            publish(QStringLiteral("bit%1").arg(i));
        };
        connect(label, &QLineEdit::textChanged, this, [commit] { commit(); });
        connect(color, &QLineEdit::textChanged, this, [commit] { commit(); });
        connect(pick, &QToolButton::clicked, this, [this, color] {
            const auto picked = chooseColor(QColor(color->text()), tr("点亮颜色"), this);
            if (picked.isValid())
                color->setText(picked.name());
        });
    }
}
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
        emit projectSettingsChanged(QStringLiteral("appName"));
    });
    addRow(general, QString(), tr("应用名称"), createIconButton(name));

    const auto spin = [this](const char *key, int lo, int hi, int value, const std::function<void(int)> &apply) {
        auto *box = new FluentQt::SpinBox(m_content);
        box->setRange(lo, hi);
        box->setValue(value);
        box->setKeyboardTracking(false);
        connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this, apply, key](int v) {
            apply(v);
            emit projectSettingsChanged(QLatin1String(key));
        });
        return box;
    };
    addRow(general, QString(), tr("画布宽度"),
           spin("canvasWidth", 100, 9999, m_project->canvasWidth, [this](int v) { m_project->canvasWidth = v; }));
    addRow(general, QString(), tr("画布高度"),
           spin("canvasHeight", 100, 9999, m_project->canvasHeight, [this](int v) { m_project->canvasHeight = v; }));
    addRow(general, QString(), tr("网格大小"),
           spin("gridSize", 1, 100, m_project->gridSize, [this](int v) { m_project->gridSize = v; }));

    auto *logic = new FluentQt::PushButton(tr("工程数据与报警…"), m_content);
    addRow(general, QStringLiteral("logic"), tr("数据与报警"), logic, QStringLiteral("project.logic"));
    connect(logic, &QPushButton::clicked, this, &PropertyEditor::editProjectLogic);
    buildBitProfilesForm();
    QFormLayout *frame = addSection(tr("默认帧格式"));
    auto *hint = new FluentQt::CaptionLabel(tr("链路卡片选择“项目设置”时使用"), m_content);
    hint->setWordWrap(true);
    frame->addRow(hint);
    auto *parser = new FluentQt::ComboBox(m_content);
    parser->addItems({QStringLiteral("line"), QStringLiteral("hex"), QStringLiteral("modbus"),
                      QStringLiteral("modbus_tcp"), QStringLiteral("modbus_ascii")});
    parser->setCurrentIndex(std::max(0, parser->findText(m_project->parserType())));
    addRow(frame, QStringLiteral("parser"), tr("解析模式"), parser, QStringLiteral("receive.framing"));

    connect(parser, &FluentQt::ComboBox::currentTextChanged, this, [this](const QString &type) {
        m_project->serial.insert(QStringLiteral("parser"), type);
        updateVisibility();
        emit projectSettingsChanged(QStringLiteral("parser"));
    });
    addFrameRows(
        frame, QString(), [this]() { return m_project->parserConfig(); },
        [this](const QJsonObject &config, const QString &key) {
            m_project->serial.insert(QStringLiteral("parserConfig"), config);
            emit projectSettingsChanged(QStringLiteral("parserConfig.") + key);
        });

    auto *tip = new FluentQt::CaptionLabel(tr("从左侧拖拽控件到画布；选中控件后在这里编辑属性和数据绑定。"), m_content);
    tip->setWordWrap(true);
    m_layout->addWidget(tip);
}

void PropertyEditor::editProjectLogic()
{
    if (!m_project)
        return;
    Project draft = *m_project;
    draft.widgets = currentWidgets(m_project, m_scene);
    if (fhmi::editProjectLogic(&draft, this)) {
        m_project->logic = draft.logic;
        m_project->widgets = draft.widgets;
        if (m_scene) {
            // Apply all renamed bindings before recording the single project undo step.
            const QSignalBlocker blocker(m_scene);
            visitWidgets(draft.widgets, [this](const QJsonObject &widget) {
                auto *item = m_scene->findItem(widget.value(QStringLiteral("id")).toString());
                item->setBinding(widget.value(QStringLiteral("binding")).toObject());
                const auto props = widget.value(QStringLiteral("properties")).toObject();
                for (auto it = props.begin(); it != props.end(); ++it)
                    if (item->propertyValue(it.key()) != it.value())
                        item->setPropertyValue(it.key(), it.value());
                return true;
            });
        }
        emit projectSettingsChanged(QString()); // each accepted dialog is one independent undo step
        QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
    }
}

QWidget *PropertyEditor::createIconButton(QWidget *nameField)
{
    auto *row = new QWidget(m_content);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *button = new FluentQt::ToolButton(row);
    fluentToolTip(button, tr("应用图标"));
    button->setIconSize(QSize(20, 20));
    // The studio's own logo stands for the default icon, which is what an exported application gets.
    const QIcon icon = iconFromFile(m_project->iconPath());
    button->setIcon(icon.isNull() ? QIcon(QStringLiteral(":/fluenthmi/logo.png")) : icon);
    keepSquareBeside(button, nameField);
    nameField->setParent(row);
    layout->addWidget(button);
    layout->addWidget(nameField, 1);

    const auto setIcon = [this](const QString &icon) {
        m_project->icon = icon;
        emit projectSettingsChanged(QString());
        QMetaObject::invokeMethod(this, [this]() { rebuild(); }, Qt::QueuedConnection);
    };
    const auto choose = [this, setIcon]() {
        const QString file =
            QFileDialog::getOpenFileName(this, tr("选择应用图标"), QString(), tr("图片 (*.png *.jpg *.jpeg *.ico)"));
        if (file.isEmpty() || !m_project) {
            return;
        }
        // Stored like an image path: relative to a saved project when inside its folder.
        QString stored = file;
        if (!m_project->filePath.isEmpty()) {
            const QString relative = QDir(m_project->assetBaseDir()).relativeFilePath(file);
            if (!relative.startsWith(QLatin1String(".."))) {
                stored = relative;
            }
        }
        setIcon(stored);
    };
    connect(button, &QToolButton::clicked, this, [this, button, setIcon, choose]() {
        auto *menu = new FluentQt::RoundMenu(this);
        connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
        // The dialog runs an event loop of its own, which may delete the menu: open it once the menu is done.
        connect(menu->addAction(tr("选择图标…")), &QAction::triggered, this,
                [this, choose]() { QMetaObject::invokeMethod(this, choose, Qt::QueuedConnection); });
        QAction *reset = menu->addAction(tr("使用默认图标"));
        reset->setEnabled(!m_project->icon.isEmpty());
        connect(reset, &QAction::triggered, this, [setIcon]() { setIcon(QString()); });
        menu->exec(button->mapToGlobal(QPoint(0, button->height())), true);
    });
    return row;
}

void PropertyEditor::addFrameRows(QFormLayout *form, const QString &prefix, const ConfigRead &read,
                                  const ConfigWrite &write)
{
    const QJsonObject config = read();
    const auto store = [read, write](const char *key, const QJsonValue &value) {
        QJsonObject edited = read();
        edited.insert(QLatin1String(key), value);
        write(edited, QLatin1String(key));
    };
    const auto row = [&](const char *key, const QString &label, QWidget *editor, const QString &helpKey = QString()) {
        addRow(form, prefix + QLatin1String(key), label, editor, helpKey);
    };
    const auto text = [&](const char *key, const QString &fallback) {
        auto *edit = new FluentQt::LineEdit(m_content);
        edit->setText(config.value(QLatin1String(key)).toString(fallback));
        connect(edit, &QLineEdit::textChanged, this, [store, key](const QString &t) { store(key, t); });
        return edit;
    };
    const auto number = [&](const char *key, int lo, int hi, int fallback) {
        auto *box = new FluentQt::SpinBox(m_content);
        box->setRange(lo, hi);
        box->setValue(config.value(QLatin1String(key)).toInt(fallback));
        box->setKeyboardTracking(false);
        connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [store, key](int v) { store(key, v); });
        return box;
    };
    const auto choice = [&](const char *key, const QStringList &options) {
        auto *combo = new FluentQt::ComboBox(m_content);
        addValueItems(combo, options);
        // Case aside: an encoding is compared in lower case, so "GBK" in a file shows as gbk.
        const QString stored = config.value(QLatin1String(key)).toString();
        for (int i = 0; i < options.size(); ++i) {
            if (options.at(i).compare(stored, Qt::CaseInsensitive) == 0) {
                combo->setCurrentIndex(i);
            }
        }
        connect(combo, &FluentQt::ComboBox::currentIndexChanged, this,
                [combo, store, key]() { store(key, selectedValue(combo)); });
        return combo;
    };

    row("separator", tr("分隔符"), text("separator", QStringLiteral(",")));
    // Picked or typed as escapes, stored as the characters themselves. An empty box is not
    // stored: the parser refuses an empty terminator, and the project could not be saved.
    auto *terminator = new FluentQt::EditableComboBox(m_content);
    terminator->addItems(
        {QStringLiteral("\\n"), QStringLiteral("\\r\\n"), QStringLiteral("\\r"), QStringLiteral(";")});
    terminator->setCurrentText(
        escapedTerminator(config.value(QStringLiteral("terminator")).toString(QStringLiteral("\n"))));
    connect(terminator, &FluentQt::EditableComboBox::currentTextChanged, this, [store](const QString &t) {
        const QString value = unescapedTerminator(t);
        if (!value.isEmpty()) {
            store("terminator", value);
        }
    });
    row("terminator", tr("行结束符"), terminator);
    row("kvSeparator", tr("键值分隔符"), choice("kvSeparator", LineOptions::kvSeparators()));
    row("encoding", tr("编码"), choice("encoding", LineOptions::encodings()));
    row("header", tr("帧头(HEX)"), text("header", QStringLiteral("AA55")));
    row("footer", tr("帧尾(HEX)"), text("footer", QStringLiteral("55AA")));
    row("lengthOffset", tr("长度字段偏移"), number("lengthOffset", -1, 65535, -1));
    auto *lengthSize = new FluentQt::ComboBox(m_content);
    lengthSize->addItems({QStringLiteral("1"), QStringLiteral("2")});
    lengthSize->setCurrentIndex(config.value(QStringLiteral("lengthSize")).toInt(1) == 2 ? 1 : 0);
    connect(lengthSize, &FluentQt::ComboBox::currentTextChanged, this,
            [store](const QString &t) { store("lengthSize", t.toInt()); });
    row("lengthSize", tr("长度字节数"), lengthSize);
    row("lengthEndian", tr("长度字节序"), choice("lengthEndian", HexOptions::byteOrders()));
    row("lengthCovers", tr("长度含义"), choice("lengthCovers", HexOptions::lengthCovers()));
    row("fixedLength", tr("固定帧长"), number("fixedLength", 0, 65536, 0));
    row("checksum", tr("校验"), choice("checksum", HexOptions::checksums()));
    row("checksumStart", tr("校验起点"), number("checksumStart", 0, 65535, 0));
    row("checksumEndian", tr("校验字节序"), choice("checksumEndian", HexOptions::byteOrders()));
    row("idleTimeout", tr("空闲超时(ms)"), number("idleTimeout", 0, 60000, 0));

    // The table is text; a line that is not a field shows its number and leaves the stored table alone.
    auto *fieldsBox = new QWidget(m_content);
    auto *fieldsLayout = new QVBoxLayout(fieldsBox);
    fieldsLayout->setContentsMargins(0, 0, 0, 0);
    fieldsLayout->setSpacing(2);
    auto *fields = new FluentQt::PlainTextEdit(fieldsBox);
    fields->setPlainText(hexFieldsText(config.value(QStringLiteral("fields")).toArray()));
    fields->setPlaceholderText(tr("每行一个：名字,偏移,长度,类型[,字节序]"));
    fields->setFixedHeight(96);
    auto *problem = new FluentQt::CaptionLabel(fieldsBox);
    problem->setTextColor(QColor(0xC4, 0x2B, 0x1C), QColor(0xFF, 0x99, 0xA4));
    problem->hide();
    fieldsLayout->addWidget(fields);
    fieldsLayout->addWidget(problem);
    connect(fields, &QPlainTextEdit::textChanged, this, [fields, problem, store]() {
        int bad = 0;
        const QJsonArray parsed = hexFieldsFromText(fields->toPlainText(), &bad);
        problem->setText(bad ? tr("第 %1 行格式错误").arg(bad) : QString());
        problem->setVisible(bad > 0);
        if (!bad) {
            store("fields", parsed);
        }
    });
    row("fields", tr("字段表"), fieldsBox, QStringLiteral("receive.framing"));
}

void PropertyEditor::setFrameRowsVisible(const QString &prefix, const QString &mode)
{
    for (const char *key : kLineFrameKeys) {
        setRowVisible(prefix + QLatin1String(key), mode == QLatin1String("line"));
    }
    for (const char *key : kHexFrameKeys) {
        setRowVisible(prefix + QLatin1String(key), mode == QLatin1String("hex"));
    }
}

QJsonObject PropertyEditor::cardFrameConfig() const
{
    const QJsonObject own = m_item->propertyValue(QStringLiteral("parserConfig")).toObject();
    if (!own.isEmpty() || !m_project ||
        m_item->propertyValue(QStringLiteral("defaultParser")).toString() != m_project->parserType()) {
        return own;
    }
    return m_project->parserConfig();
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
        return QStringLiteral("_tabLayout_%1").arg(m_item->editorPage());
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
    if (def.name == QLatin1String("curveName")) { // as the chart names a curve without one
        const int curve = m_item->propertyValue(QStringLiteral("currentCurve")).toInt(0);
        return QStringLiteral("曲线%1").arg(curve + 1); // i18n: value
    }
    return def.defaultValue;
}

void PropertyEditor::commitProperty(const QString &name, const QJsonValue &value, const QString &detail)
{
    if (m_loading || !m_item) {
        return;
    }
    if ((name==QLatin1String("commandMode") || name==QLatin1String("curveParseMode")) && value.toString()==QLatin1String("S7")) {
        const QString objectName = name==QLatin1String("commandMode") ? QStringLiteral("s7Command") : QStringLiteral("curveS7");
        const auto *def=m_item->meta().prop(objectName);
        if(def && !m_item->properties().contains(storageKey(objectName)))m_item->setPropertyValue(storageKey(objectName),def->defaultValue);
    }
    const QString key = storageKey(name);
    const int curves = m_item->propertyValue(QStringLiteral("curveCount")).toInt(1);
    const QString endpoint = endpointProperty(name);
    // A new card starts with its own default destination. A mode change keeps the
    // existing scope until the operator explicitly replaces or clears it.
    if (!endpoint.isEmpty() && m_item->propertyValue(key).toString() != value.toString() &&
        m_rows.contains(endpoint)) {
        m_item->setPropertyValue(storageKey(endpoint), QJsonValue(QJsonValue::Undefined));
        auto *edit = static_cast<QLineEdit *>(m_rows.value(endpoint).second);
        const QSignalBlocker blocker(edit);
        edit->clear();
    }
    m_item->setPropertyValue(key, value);
    emit modified(QStringLiteral("property:%1:%2:%3").arg(m_item->widgetId(), key, detail));
    // These change which per-curve / per-page values, or which fields, the form shows;
    // defaultParser decides whether a link card has a frame format section, and what it starts from.
    static const QSet<QString> structural = {QStringLiteral("bitProfile"), QStringLiteral("currentCurve"), QStringLiteral("curveCount"),
                                             QStringLiteral("tabNames"),
                                             QStringLiteral("defaultParser")};
    if (structural.contains(name) || (m_item->meta().typeName == QLatin1String("SpinBox") &&
                                     (name == QLatin1String("numberMode") || name == QLatin1String("decimals")))) {
        if (name == QLatin1String("curveCount")) {
            const int count = m_item->propertyValue(QStringLiteral("curveCount")).toInt(1);
            // Added curves are named in the interface language, as a new chart's first one is.
            for (int i = curves; i < count; ++i) {
                const QString curveName = QStringLiteral("_curve_%1_name").arg(i);
                if (m_item->properties().value(curveName).isUndefined()) {
                    m_item->setPropertyValue(curveName, tr("曲线%1").arg(i + 1));
                }
            }
            if (m_item->propertyValue(QStringLiteral("currentCurve")).toInt(0) >= count) {
                m_item->setPropertyValue(QStringLiteral("currentCurve"), count - 1);
            }
        }
        QMetaObject::invokeMethod(this, [this]() { rebuild(); }, Qt::QueuedConnection);
        return;
    }
    if (m_geometry[3] && m_item->isLinkCard()) { // its rows (displayMode) set how short it may be
        m_loading = true;
        m_geometry[3]->setMinimum(int(std::ceil(m_item->minimumHeight())));
        m_loading = false;
        refreshGeometry();
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

void PropertyEditor::addPageRows(QFormLayout *form)
{
    const QStringList names = tabNames(m_item->propertyValue(QStringLiteral("tabNames")).toString());
    const int current = m_item->editorPage();
    auto *pages = new FluentQt::ComboBox(m_content);
    pages->setObjectName(QStringLiteral("tabPageSelector"));
    pages->addItems(names);
    pages->setCurrentIndex(current);
    connect(pages, &FluentQt::ComboBox::currentIndexChanged, m_item, &CanvasItem::setEditorPage);
    addRow(form, QStringLiteral("editorPage"), tr("当前编辑页"), pages);
    auto *startup = new FluentQt::ComboBox(m_content);
    startup->setObjectName(QStringLiteral("tabStartupPage"));
    startup->addItems(names);
    startup->setCurrentIndex(clampTabIndex(m_item->propertyValue(QStringLiteral("currentTab")).toInt(0),
                                          int(names.size())));
    connect(startup, &FluentQt::ComboBox::currentIndexChanged, this,
            [this](int index) { commitProperty(QStringLiteral("currentTab"), index); });
    addRow(form, QStringLiteral("currentTab"), tr("运行时启动页"), startup);

    const auto finish = [this]() {
        emit modified(QString());
        QMetaObject::invokeMethod(this, [this]() { rebuild(); }, Qt::QueuedConnection);
    };
    auto *name = new FluentQt::LineEdit(m_content);
    name->setObjectName(QStringLiteral("tabPageName"));
    name->setText(names.at(current));
    connect(name, &QLineEdit::editingFinished, this, [this, name, current, finish]() {
        if (m_item && m_item->renamePage(current, name->text())) {
            finish();
        } else if (m_item) {
            name->setText(tabNames(m_item->propertyValue(QStringLiteral("tabNames")).toString()).value(current));
        }
    });
    addRow(form, QStringLiteral("tabNames"), tr("页面名称"), name);

    auto *actions = new QWidget(m_content);
    auto *layout = new QHBoxLayout(actions);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    const auto button = [actions, layout](const QString &objectName, FluentQt::FluentIcon icon, const QString &label) {
        auto *control = new FluentQt::ToolButton(actions);
        control->setObjectName(objectName);
        control->setIcon(FluentQt::icon(icon));
        control->setAccessibleName(label);
        fluentToolTip(control, label);
        layout->addWidget(control);
        return control;
    };
    auto *add = button(QStringLiteral("addTabPage"), FluentQt::FluentIcon::Add, tr("添加页面"));
    connect(add, &QToolButton::clicked, this, [this, finish]() {
        if (m_item && m_item->addPage() >= 0)
            finish();
    });
    auto *remove = button(QStringLiteral("removeTabPage"), FluentQt::FluentIcon::Remove, tr("删除当前页面"));
    remove->setEnabled(names.size() > 1);
    connect(remove, &QToolButton::clicked, this, [this, current, finish]() {
        if (m_item && m_item->removePage(current))
            finish();
    });
    auto *previous = button(QStringLiteral("moveTabPagePrevious"), FluentQt::FluentIcon::LeftArrow, tr("页面前移"));
    previous->setEnabled(current > 0);
    connect(previous, &QToolButton::clicked, this, [this, current, finish]() {
        if (m_item && m_item->movePage(current, current - 1))
            finish();
    });
    auto *next = button(QStringLiteral("moveTabPageNext"), FluentQt::FluentIcon::RightArrow, tr("页面后移"));
    next->setEnabled(current + 1 < names.size());
    connect(next, &QToolButton::clicked, this, [this, current, finish]() {
        if (m_item && m_item->movePage(current, current + 1))
            finish();
    });
    layout->addStretch();
    addRow(form, QStringLiteral("tabPageActions"), tr("页面管理"), actions);
}

void PropertyEditor::buildItemForm()
{
    const WidgetMeta &meta = m_item->meta();

    auto *header = new FluentQt::SimpleCardWidget(m_content);
    auto *headerLayout = new QFormLayout(header);
    headerLayout->setContentsMargins(12, 10, 12, 10);
    headerLayout->setSpacing(8);
    headerLayout->addRow(new FluentQt::SubtitleLabel(translated(meta.displayName), header));
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
        spin->setRange(i == 2 ? meta.minWidth : i == 3 ? int(std::ceil(m_item->minimumHeight())) : -9999, 9999);
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
            emit modified(QStringLiteral("geometry:%1:%2").arg(m_item->widgetId()).arg(i));
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
            if (def.type == PropType::Object) {
                continue; // a section of its own below
            }
            const QString name = def.name;
            if (meta.typeName == QLatin1String("TabWidget")) {
                if (name == QLatin1String("tabNames")) {
                    addPageRows(props);
                    continue;
                }
                if (name == QLatin1String("currentTab"))
                    continue;
            }
            QWidget *editor =
                createEditor(def, itemValue(def), [this, name](const QJsonValue &v) { commitProperty(name, v); });
            addRow(props, name, translated(def.label), editor, def.helpKey);
            if (name == QLatin1String("defaultParser") && m_project && m_scene &&
                LinkParser::inheritsProject(m_item->propertyValue(name).toString())) {
                auto *settings = new FluentQt::TransparentPushButton(tr("工程解析设置"), m_content);
                addRow(props, {}, {}, settings);
                connect(settings, &QPushButton::clicked, this, [this] {
                    m_scene->clearSelection();
                    if (m_item)
                        showItem(nullptr);
                    ensureWidgetVisible(m_rows.value(QStringLiteral("parser")).second);
                });
            }
            if (name == QLatin1String("curveCanOffset")) {
                addRow(props, QStringLiteral("curveCanProblem"), QString(), newProblemLabel(m_content));
            }
        }
        for(const QString &root: {QStringLiteral("s7Command"),QStringLiteral("curveS7")}) {
            const auto *objectDef=meta.prop(root);if(!objectDef)continue;
            const QJsonObject object=itemValue(*objectDef).toObject();
            QList<PropDef> fields{
                {QStringLiteral("address"),tr("S7 地址"),PropType::String,QStringLiteral("DB1.DBD4"),{},std::nullopt,std::nullopt,false,{},QStringLiteral("s7.addressing")},
                {QStringLiteral("dataType"),tr("S7 数据类型"),PropType::Enum,QStringLiteral("REAL"),S7::dataTypes(),std::nullopt,std::nullopt,false,{}}};
            if(root==QLatin1String("curveS7")) fields.append({QStringLiteral("period"),tr("采集周期(ms)"),PropType::Int,0,{},0,60000,false,{}});
            else {
                fields.append({QStringLiteral("value"),tr("写入值"),PropType::String,QStringLiteral("1"),{},std::nullopt,std::nullopt,false,{}});
                fields.append({QStringLiteral("onValue"),tr("开启写入值"),PropType::String,QStringLiteral("1"),{},std::nullopt,std::nullopt,false,{}});
                fields.append({QStringLiteral("offValue"),tr("关闭写入值"),PropType::String,QStringLiteral("0"),{},std::nullopt,std::nullopt,false,{}});
            }
            for(const auto &field:fields) {
                auto *editor=createEditor(field,object.value(field.name).isUndefined()?field.defaultValue:object.value(field.name),[this,root,name=field.name,defaults=objectDef->defaultValue](const QJsonValue &value) {
                    auto object=m_item->properties().value(storageKey(root)).toObject();if(object.isEmpty())object=defaults.toObject();
                    object.insert(name,value);commitProperty(root,object,name);
                });
                addRow(props,root+QLatin1Char('.')+field.name,field.label,editor,field.helpKey);
            }
        }
    }
    if (meta.prop(QStringLiteral("bitProfile"))) buildBitProfilesForm();
    // A link card with a mode of its own frames with its own settings, edited like the project's.
    const PropDef *frame = meta.prop(QStringLiteral("parserConfig"));
    const QString parser = m_item->propertyValue(QStringLiteral("defaultParser")).toString();
    if (frame && (parser == QLatin1String("line") || parser == QLatin1String("hex"))) {
        addFrameRows(
            addSection(tr("帧格式")), QStringLiteral("parserConfig."), [this]() { return cardFrameConfig(); },
            [this](const QJsonObject &config, const QString &key) {
                commitProperty(QStringLiteral("parserConfig"), config, key);
            });
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
    // Each editor commits under its own key, so typing in one field is one undo step.
    const auto add = [&](const char *key, const QString &label, QWidget *editor, const QString &helpKey = QString()) {
        m_bindingEditors.insert(QLatin1String(key), editor);
        addRow(form, QStringLiteral("binding.") + QLatin1String(key), label, editor, helpKey);
        const QString field = QLatin1String(key);
        if (auto *box = qobject_cast<FluentQt::ComboBox *>(editor)) {
            connect(box, &FluentQt::ComboBox::currentIndexChanged, this, [this, field]() { commitBinding(field); });
        } else if (auto *box = qobject_cast<FluentQt::EditableComboBox *>(editor)) {
            connect(box, &FluentQt::EditableComboBox::currentTextChanged, this,
                    [this, field]() { commitBinding(field); });
        } else if (auto *edit = qobject_cast<QLineEdit *>(editor)) {
            connect(edit, &QLineEdit::textChanged, this, [this, field]() { commitBinding(field); });
        } else if (auto *number = qobject_cast<QSpinBox *>(editor)) {
            connect(number, qOverload<int>(&QSpinBox::valueChanged), this, [this, field]() { commitBinding(field); });
        }
    };
    const auto combo = [this](const QStringList &values, const QString &current) {
        auto *box = new FluentQt::ComboBox(m_content);
        addValueItems(box, values);
        if (!selectValue(box, current)) {
            box->setCurrentIndex(0);
        }
        return box;
    };
    const auto line = [this](const QString &text, const QString &placeholder) {
        auto *edit = new FluentQt::LineEdit(m_content);
        edit->setText(text);
        edit->setPlaceholderText(placeholder);
        return edit;
    };
    const auto spin = [this](int lo, int hi, int current) {
        auto *box = new FluentQt::SpinBox(m_content);
        box->setRange(lo, hi);
        box->setValue(current);
        return box;
    };

    const QString mode = value("parseMode", ParseMode::KeyValue).toString();
    QStringList modes{ParseMode::KeyValue, ParseMode::DataPoint, ParseMode::JsonPath,
                      ParseMode::HexBytes, ParseMode::Modbus,    ParseMode::S7};
    // CAN信号 reads the frames of a CAN card's links, so it is offered only where there is one, and kept where a
    // binding already uses it.
    if (mode == ParseMode::CanSignal || hasCanCard(currentWidgets(m_project, m_scene))) {
        modes.append(ParseMode::CanSignal);
    }
    if (m_bindingOnly)
        modes.removeAll(ParseMode::DataPoint);
    add("parseMode", tr("解析模式"), combo(modes, mode), QStringLiteral("data.binding"));
    const auto s7=binding.value(QStringLiteral("s7")).toObject();
    auto *source = new ConnectionSourceEdit(m_content);
    source->setSources(m_project, m_scene, bindingRouteTransport(mode), value("sourceCardId", QString()).toString());
    add("sourceCardId", tr("来源连接"), source, QStringLiteral("data.routing"));
    add("sourceEndpoint", tr("来源子通道"), line(value("sourceEndpoint", QString()).toString(), {}));
    add("s7Address",tr("S7 地址"),line(s7.value(QStringLiteral("address")).toString(QStringLiteral("DB1.DBD4")),QStringLiteral("DB1.DBD4")),QStringLiteral("s7.addressing"));
    add("s7DataType",tr("S7 数据类型"),combo(S7::dataTypes(),s7.value(QStringLiteral("dataType")).toString(QStringLiteral("REAL"))));
    add("s7Period",tr("S7 采集周期(ms)"),spin(0,60000,s7.value(QStringLiteral("period")).toInt(0)));

    auto *key = new BindingKeyEdit(m_content);
    key->setCurrentText(value("key", QString()).toString());
    add("key", tr("绑定字段"), key);
    add("jsonPath", tr("JSON路径"), line(value("jsonPath", QString()).toString(), tr("路径 (如 data.sensors.0.temp)")));
    add("byteOffset", tr("字节偏移"), spin(0, 9999, value("byteOffset", 0).toInt()));
    add("dataType", tr("数据类型"),
        combo({QStringLiteral("uint8"), QStringLiteral("int8"), QStringLiteral("uint16"), QStringLiteral("int16"),
               QStringLiteral("uint32"), QStringLiteral("int32"), QStringLiteral("float32"), QStringLiteral("ASCII")},
              value("dataType", QStringLiteral("uint16")).toString()));
    add("byteOrder", tr("字节序"),
        combo({QStringLiteral("大端(BE)"), QStringLiteral("小端(LE)")},    // i18n: value
              value("byteOrder", QStringLiteral("大端(BE)")).toString())); // i18n: value
    add("frameHeader", tr("帧头(HEX)"), line(value("frameHeader", QString()).toString(), tr("如 AA 55")));
    add("frameTail", tr("帧尾(HEX)"), line(value("frameTail", QString()).toString(), tr("如 0D 0A")));
    add("mbArea", tr("寄存器区"),
        combo(ModbusArea::options(), value("mbArea", ModbusArea::HoldingRegisters).toString()));
    add("mbRegIndex", tr("寄存器地址"), spin(0, 65535, value("mbRegIndex", 0).toInt()), QStringLiteral("modbus.addressing"));
    add("mbDataType", tr("数据类型"),
        combo({QStringLiteral("uint16"), QStringLiteral("int16"), QStringLiteral("uint32"), QStringLiteral("int32"),
               QStringLiteral("float32")},
              value("mbDataType", QStringLiteral("uint16")).toString()));
    add("mbRegOrder", tr("寄存器序"),
        combo({QStringLiteral("大端(AB CD)"), QStringLiteral("小端(CD AB)")},  // i18n: value
              value("mbRegOrder", QStringLiteral("大端(AB CD)")).toString())); // i18n: value
    add("mbFcFilter", tr("功能码过滤"), spin(-1, 255, value("mbFcFilter", -1).toInt()));
    add("mbSlaveFilter", tr("从站过滤"), spin(-1, 247, value("mbSlaveFilter", -1).toInt()));
    auto *period = spin(0, 60000, value("mbPeriod", 0).toInt());
    period->setSpecialValueText(tr("默认周期"));
    add("mbPeriod", tr("采集周期(ms)"), period);
    add("canId", tr("CAN ID"), line(value("canId", QString()).toString(), tr("如 18FF50E5")));
    add("canStartBit", tr("起始位"), spin(0, 511, value("canStartBit", 0).toInt()));
    add("canBitLength", tr("长度"), spin(1, 64, value("canBitLength", 8).toInt()));
    add("canByteOrder", tr("字节序"),
        combo(CanByteOrder::options(), value("canByteOrder", CanByteOrder::Intel).toString()));
    auto *sign = new FluentQt::ComboBox(m_content);
    sign->addItem(tr("无符号"), QIcon(), false);
    sign->addItem(tr("有符号"), QIcon(), true);
    sign->setCurrentIndex(value("canSigned", false).toBool() ? 1 : 0);
    add("canSigned", tr("有无符号"), sign);
    // Empty is the default, shown as the placeholder; only numbers can be typed, so none is lost on commit.
    const auto numberLine = [&](const char *key, const QString &fallback) {
        const QJsonValue current = value(key, QJsonValue());
        auto *edit = line(current.isDouble() ? QString::number(current.toDouble(), 'g', QLocale::FloatingPointShortest)
                                             : QString(),
                          fallback);
        edit->setValidator(new QRegularExpressionValidator(numberPattern(), edit));
        return edit;
    };
    add("canFactor", tr("因子"), numberLine("canFactor", QStringLiteral("1")));
    add("canOffset", tr("偏移"), numberLine("canOffset", QStringLiteral("0")));
    // Why the signal as set up reads nothing (updateVisibility fills it): the binding is kept while it is typed.
    add("canProblem", QString(), newProblemLabel(m_content));
    add("expression", tr("转换表达式"), line(value("expression", QString()).toString(), tr("如 value * 0.1")),
         QStringLiteral("data.expression"));
}

void PropertyEditor::commitBinding(const QString &field)
{
    if (m_loading || !m_item) {
        return;
    }
    if (field == QLatin1String("sourceCardId")) {
        auto *endpoint = static_cast<QLineEdit *>(m_bindingEditors.value(QStringLiteral("sourceEndpoint")));
        const QSignalBlocker blocker(endpoint);
        endpoint->clear();
    }
    const auto textOf = [this](const char *key) {
        QWidget *editor = m_bindingEditors.value(QLatin1String(key));
        if (auto *combo = qobject_cast<FluentQt::ComboBox *>(editor)) {
            return selectedValue(combo);
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
    // A number field's value, or Undefined for an empty one, which QJsonObject::insert leaves out: the default.
    const auto numberOf = [&textOf](const char *key) {
        bool ok = false;
        const double number = textOf(key).toDouble(&ok);
        return ok ? QJsonValue(number) : QJsonValue(QJsonValue::Undefined);
    };
    const QString mode = textOf("parseMode");
    // A CAN信号 binding is kept once it has an ID, so what was typed survives while the signal is still being set
    // up; the runtime uses it once the ID parses and the signal is valid.
    const bool configured =
        ((mode == ParseMode::KeyValue || mode == ParseMode::DataPoint) && !textOf("key").isEmpty()) ||
        (mode == ParseMode::JsonPath && !textOf("jsonPath").isEmpty()) || mode == ParseMode::HexBytes ||
        mode == ParseMode::Modbus || mode == ParseMode::S7 ||
        (mode == ParseMode::CanSignal && !textOf("canId").isEmpty());
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
            {QStringLiteral("mbArea"), textOf("mbArea")},
            {QStringLiteral("mbPeriod"), intOf("mbPeriod")},
        };
        if (mode == ParseMode::CanSignal) {
            const auto *sign = qobject_cast<FluentQt::ComboBox *>(m_bindingEditors.value(QStringLiteral("canSigned")));
            binding.insert(QStringLiteral("canId"), textOf("canId"));
            binding.insert(QStringLiteral("canStartBit"), intOf("canStartBit"));
            binding.insert(QStringLiteral("canBitLength"), intOf("canBitLength"));
            binding.insert(QStringLiteral("canByteOrder"), textOf("canByteOrder"));
            binding.insert(QStringLiteral("canSigned"), sign && sign->currentData().toBool());
            binding.insert(QStringLiteral("canFactor"), numberOf("canFactor"));
            binding.insert(QStringLiteral("canOffset"), numberOf("canOffset"));
        }
        binding.insert(QStringLiteral("sourceCardId"),textOf("sourceCardId"));
        const QString endpoint = textOf("sourceEndpoint");
        if (!endpoint.isEmpty())
            binding.insert(QStringLiteral("sourceEndpoint"), endpoint);
        binding.insert(QStringLiteral("s7"),QJsonObject{{QStringLiteral("address"),textOf("s7Address")},{QStringLiteral("dataType"),textOf("s7DataType")},{QStringLiteral("period"),intOf("s7Period")}});
        const QString expression = textOf("expression");
        binding.insert(QStringLiteral("expression"), expression.isEmpty() ? QJsonValue() : QJsonValue(expression));
        m_item->setBinding(binding);
    }
    updateVisibility();
    emit modified(QStringLiteral("binding:%1:%2").arg(m_item->widgetId(), field));
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

void PropertyEditor::updateVisibility()
{
    if (!m_item) {
        if (m_project) {
            setFrameRowsVisible(QString(), m_project->parserType());
        }
        return;
    }
    const WidgetMeta &meta = m_item->meta();
    if (meta.prop(QStringLiteral("textLayout"))) {
        const bool title = m_item->propertyValue(QStringLiteral("showTitle")).toBool();
        const bool value = m_item->propertyValue(QStringLiteral("showValue")).toBool();
        const bool offset =
            m_item->propertyValue(QStringLiteral("textLayout")).toString() == QStringLiteral("中心偏移"); // i18n: value
        setRowVisible(QStringLiteral("title"), title);
        setRowVisible(QStringLiteral("textLayout"), title || value);
        const bool locked = m_item->propertyValue(QStringLiteral("textPositionLocked")).toBool();
        for (const char *key : {"textLayout", "titleOffsetX", "titleOffsetY", "valueOffsetX", "valueOffsetY"}) {
            if (auto *editor = m_rows.value(QLatin1String(key)).second)
                editor->setEnabled(!locked);
        }
        for (const char *field : {"titleOffsetX", "titleOffsetY"}) {
            setRowVisible(QLatin1String(field), offset && title);
        }
        for (const char *field : {"valueOffsetX", "valueOffsetY"}) {
            setRowVisible(QLatin1String(field), offset && value);
        }
    }
    if (meta.typeName == QLatin1String("NumericDisplay")) {
        const bool enumeration =
            m_item->propertyValue(QStringLiteral("displayFormat")).toString() == QStringLiteral("枚举"); // i18n: value
        for (const char *key : {"textList", "unknownText", "unknownColor"})
            setRowVisible(QLatin1String(key), enumeration);
        setRowVisible(QStringLiteral("decimals"), !enumeration);
        setRowVisible(QStringLiteral("unit"), !enumeration);
        setRowVisible(QStringLiteral("fontSize"),
                      m_item->propertyValue(QStringLiteral("displayStyle")).toString() != QStringLiteral("状态徽标")); // i18n: value
    }
    if (meta.typeName == QLatin1String("SpinBox")) {
        setRowVisible(QStringLiteral("decimals"),
                      m_item->propertyValue(QStringLiteral("numberMode")).toString() == QStringLiteral("小数")); // i18n: value
    }
    if (meta.typeName == QLatin1String("TractionBattery")) {
        for (const char *key :
             {"chargeLow", "chargeWarning", "chargeLowColor", "chargeWarningColor", "chargeNormalColor"})
            setRowVisible(QLatin1String(key), m_item->propertyValue(QStringLiteral("chargeColors")).toBool());
    }
    if (auto *help = m_helpButtons.value(QStringLiteral("commandTemplate"))) {
        help->setVisible(m_item->propertyValue(QStringLiteral("commandMode")).toString() == QLatin1String("CAN"));
    }
    if (meta.typeName == QLatin1String("ToggleSwitch")) {
        setRowVisible(QStringLiteral("onValue"),
                      m_item->propertyValue(QStringLiteral("onCondition")).toString() ==
                          QStringLiteral("等于指定值")); // i18n: value
    }
    if (meta.prop(QStringLiteral("parserConfig"))) {
        const QString parser = m_item->propertyValue(QStringLiteral("defaultParser")).toString();
        setFrameRowsVisible(QStringLiteral("parserConfig."), parser);
        const QString projectParser = m_project ? m_project->parserType() : QStringLiteral("line");
        const bool modbus = isModbusParser(LinkParser::effectiveType(parser, projectParser));
        const bool slave = m_item->propertyValue(QStringLiteral("modbusRole")).toString() == ModbusRole::Slave;
        for (const char *field : {"modbusRole", "modbusSlave"}) {
            setRowVisible(QLatin1String(field), modbus);
        }
        for (const char *field : {"modbusPeriod", "modbusTimeout"}) {
            setRowVisible(QLatin1String(field), modbus && !slave);
        }
        for (const char *field :
             {"modbusDelay", "modbusExceptions", "slaveCoStart", "slaveCoCount", "slaveDiStart", "slaveDiCount",
              "slaveHrStart", "slaveHrCount", "slaveIrStart", "slaveIrCount", "slaveInit"}) {
            setRowVisible(QLatin1String(field), modbus && slave);
        }
    }
    if (meta.prop(QStringLiteral("commandMode"))) {
        const QString mode = m_item->propertyValue(QStringLiteral("commandMode")).toString();
        const QString target = m_item->propertyValue(QStringLiteral("commandTargetId")).toString();
        if (auto *box = static_cast<ConnectionSourceEdit *>(m_rows.value(QStringLiteral("commandTargetId")).second))
            box->setSources(m_project, m_scene, commandRouteTransport(mode, meta.typeName), target);
        const QSet<QString> shown = modeVisibility().value(mode);
        for (const QString &field : modeFields()) {
            setRowVisible(field, shown.contains(field));
        }
        for(const QString &field:{QStringLiteral("address"),QStringLiteral("dataType"),QStringLiteral("value"),QStringLiteral("onValue"),QStringLiteral("offValue")}) {
            const bool toggle=meta.typeName==QLatin1String("ToggleSwitch");
            const bool shown=mode==QLatin1String("S7") && (field==QLatin1String("address") || field==QLatin1String("dataType")
                || (toggle ? field==QLatin1String("onValue")||field==QLatin1String("offValue") : field==QLatin1String("value")&&meta.typeName==QLatin1String("PushButton")));
            setRowVisible(QStringLiteral("s7Command.")+field,shown);
        }
        if (mode == QLatin1String("CAN")) {
            // The link ignores these for a message with signals; show them only where they still matter.
            const auto names = canNames(m_project, currentWidgets(m_project, m_scene), target);
            if (auto *edit = qobject_cast<FluentQt::LineEdit *>(m_rows.value(QStringLiteral("canId")).second))
                complete(edit, names.messages);
            const CanTargetKind kind = canTargetKind(names,
                                                     m_item->propertyValue(QStringLiteral("canId")).toString());
            setRowVisible(QStringLiteral("dataType"), kind.usesRawEncoding);
            setRowVisible(QStringLiteral("byteOrder"), kind.usesRawEncoding);
            setRowVisible(QStringLiteral("canFd"), kind.usesFdSetting);
        }
        const QString toggle = sendToggle(meta.typeName);
        // A Modbus-mode control sends whether or not its toggle is on, so its settings stay visible.
        const bool sends = toggle.isEmpty() || m_item->propertyValue(toggle).toBool() ||
                           mode == QLatin1String("Modbus") || mode == QLatin1String("S7");
        setRowVisible(QStringLiteral("commandTargetId"), sends);
        updateRouteEndpoint(QStringLiteral("commandTargetId"), QStringLiteral("commandEndpoint"), sends);
        if (!sends) {
            for (const QString &field : modeFields()) {
                setRowVisible(field, false);
            }
            setRowVisible(QStringLiteral("commandMode"), false);
            setRowVisible(QStringLiteral("commands"), false);
        } else {
            setRowVisible(QStringLiteral("commandMode"), true);
            setRowVisible(QStringLiteral("commands"), true);
        }
    } else {
        updateRouteEndpoint(QStringLiteral("commandTargetId"), QStringLiteral("commandEndpoint"));
    }
    updateRouteEndpoint(QStringLiteral("sourceCardId"), QStringLiteral("sourceEndpoint"));
    if (meta.typeName == QLatin1String("CanMonitor")) {
        setRowVisible(QStringLiteral("autoScroll"),
                      m_item->propertyValue(QStringLiteral("displayMode")).toString() == QLatin1String("滚动日志")); // i18n: value
        setRowVisible(QStringLiteral("autoSaveRows"), m_item->propertyValue(QStringLiteral("autoSave")).toBool());
        setRowVisible(QStringLiteral("autoSaveDirectory"), m_item->propertyValue(QStringLiteral("autoSave")).toBool());
    }
    if (meta.typeName == QLatin1String("CanConfigCard")) {
        setRowVisible(QStringLiteral("canTxInitialMaxAge"),
                      !m_item->propertyValue(QStringLiteral("canTxInitialState")).toString().trimmed().isEmpty());
        // The ZLG fields for a ZLG adapter, the data phase for CAN FD.
        const QString adapter = m_item->propertyValue(QStringLiteral("defaultAdapter")).toString();
        const bool zlg = adapter == CanAdapter::Zlg;
        for (const char *field : {"zlgModel", "defaultDeviceIndex", "defaultChannel"}) {
            setRowVisible(QLatin1String(field), zlg);
        }
        const auto caps = CanAdapter::caps(adapter, m_item->propertyValue(QStringLiteral("zlgModel")).toString());
        const QString custom = m_item->propertyValue(zlg ? QStringLiteral("zlgCustomTiming")
                                                        : QStringLiteral("customTiming")).toString();
        const auto timing = CanAdapter::timingOverrides(adapter, caps.customTiming ? custom : QString());
        const bool fd = caps.fdEnabled(m_item->propertyValue(QStringLiteral("canFd")).toBool());
        const bool systemTiming = adapter == CanAdapter::SocketCan; // presets fill the ip link command
        setRowVisible(QStringLiteral("defaultBitrate"), !timing.nominal);
        setRowVisible(QStringLiteral("defaultSamplePoint"), !zlg && (caps.samplePoint || systemTiming) && !timing.nominal);
        setRowVisible(QStringLiteral("zlgSamplePoint"), zlg && caps.samplePoint && !timing.nominal);
        setRowVisible(QStringLiteral("customTiming"), !zlg && caps.customTiming);
        setRowVisible(QStringLiteral("zlgCustomTiming"), zlg && caps.customTiming);
        setRowVisible(QStringLiteral("canFd"), caps.fd);
        setRowVisible(QStringLiteral("defaultDataBitrate"), fd && !timing.data);
        setRowVisible(QStringLiteral("defaultDataSamplePoint"), fd && (caps.dataSamplePoint || systemTiming) && !timing.data);
        setRowVisible(QStringLiteral("canBrs"), fd);
        setRowVisible(QStringLiteral("listenOnly"), caps.listenOnly);
        setRowVisible(QStringLiteral("autoRecover"), caps.autoRecover);
        setRowVisible(QStringLiteral("termination"), caps.termination);
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
        auto *key = static_cast<BindingKeyEdit *>(m_rows.value(QStringLiteral("curveKey")).second);
        const QString source = m_item->properties().value(storageKey(QStringLiteral("curveSourceCardId"))).toString();
        if (auto *box = static_cast<ConnectionSourceEdit *>(m_rows.value(QStringLiteral("curveSourceCardId")).second))
            box->setSources(m_project, m_scene, bindingRouteTransport(mode), source);
        key->setNames(mode == ParseMode::DataPoint ? dataPointNames(m_project)
                                                 : canNames(m_project, currentWidgets(m_project, m_scene), source).keys,
                      mode == ParseMode::DataPoint ? tr("选择或搜索工程数据点") : tr("字段名 (如 temperature)"));
        updateRouteEndpoint(QStringLiteral("curveSourceCardId"), QStringLiteral("curveSourceEndpoint"),
                            mode != ParseMode::DataPoint);
        setRowVisible(QStringLiteral("curveSourceCardId"), mode != ParseMode::DataPoint);
        const QSet<QString> shown = chartParseVisibility().value(mode);
        for (const QSet<QString> &fields : chartParseVisibility()) {
            for (const QString &field : fields) {
                setRowVisible(field, shown.contains(field));
            }
        }
        for(const QString &field:{QStringLiteral("address"),QStringLiteral("dataType"),QStringLiteral("period")})
            setRowVisible(QStringLiteral("curveS7.")+field,mode==ParseMode::S7);
        // Only once the curve has an ID to judge: an empty one is a field still to fill, not a mistake.
        QString problem;
        if (mode == ParseMode::CanSignal &&
            !m_item->properties().value(storageKey(QStringLiteral("curveCanId"))).toString().isEmpty()) {
            problem = canSignalProblem(chartCurveBinding(
                m_item->properties(), m_item->propertyValue(QStringLiteral("currentCurve")).toInt(0)));
        }
        if (const auto row = m_rows.constFind(QStringLiteral("curveCanProblem")); row != m_rows.constEnd()) {
            if (auto *label = dynamic_cast<ProblemLabel *>(row->second)) {
                label->setProblem(problem);
            }
        }
        setRowVisible(QStringLiteral("curveCanProblem"), !problem.isEmpty());
    }
    if (!m_bindingEditors.isEmpty()) {
        const auto *combo = qobject_cast<FluentQt::ComboBox *>(m_bindingEditors.value(QStringLiteral("parseMode")));
        const QString mode = combo ? selectedValue(combo) : ParseMode::KeyValue;
        auto *source = static_cast<ConnectionSourceEdit *>(m_bindingEditors.value(QStringLiteral("sourceCardId")));
        const QString sourceId = selectedValue(source);
        source->setSources(m_project, m_scene, bindingRouteTransport(mode), sourceId);
        auto *key = static_cast<BindingKeyEdit *>(m_bindingEditors.value(QStringLiteral("key")));
        key->setNames(mode == ParseMode::DataPoint ? dataPointNames(m_project)
                                                 : canNames(m_project, currentWidgets(m_project, m_scene), sourceId).keys,
                      mode == ParseMode::DataPoint ? tr("选择或搜索工程数据点") : tr("字段名 (如 temperature)"));
        updateRouteEndpoint(QStringLiteral("binding.sourceCardId"), QStringLiteral("binding.sourceEndpoint"),
                            mode != ParseMode::DataPoint);
        for(const char *field:{"s7Address","s7DataType","s7Period"})
            setRowVisible(QStringLiteral("binding.")+QLatin1String(field),mode==ParseMode::S7);
        setRowVisible(QStringLiteral("binding.key"), mode == ParseMode::KeyValue || mode == ParseMode::DataPoint);
        setRowVisible(QStringLiteral("binding.sourceCardId"), mode != ParseMode::DataPoint);
        setRowVisible(QStringLiteral("binding.jsonPath"), mode == ParseMode::JsonPath);
        for (const char *field : {"byteOffset", "dataType", "byteOrder", "frameHeader", "frameTail"}) {
            setRowVisible(QStringLiteral("binding.") + QLatin1String(field), mode == ParseMode::HexBytes);
        }
        for (const char *field :
             {"mbArea", "mbRegIndex", "mbDataType", "mbRegOrder", "mbFcFilter", "mbSlaveFilter", "mbPeriod"}) {
            setRowVisible(QStringLiteral("binding.") + QLatin1String(field), mode == ParseMode::Modbus);
        }
        for (const char *field :
             {"canId", "canStartBit", "canBitLength", "canByteOrder", "canSigned", "canFactor", "canOffset"}) {
            setRowVisible(QStringLiteral("binding.") + QLatin1String(field), mode == ParseMode::CanSignal);
        }
        // Only once there is a binding to judge: an empty ID is a field still to fill, not a mistake.
        const QJsonObject binding = m_item ? m_item->binding() : QJsonObject();
        const QString problem =
            mode == ParseMode::CanSignal && !binding.isEmpty() ? canSignalProblem(binding) : QString();
        if (auto *label = dynamic_cast<ProblemLabel *>(m_bindingEditors.value(QStringLiteral("canProblem")))) {
            label->setProblem(problem);
        }
        setRowVisible(QStringLiteral("binding.canProblem"), !problem.isEmpty());
    }
}

} // namespace fhmi
