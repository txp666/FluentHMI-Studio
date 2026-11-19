#include "designer/receive_frame_fields.h"

#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {
namespace {
QString tr(const char *text) { return QCoreApplication::translate("fhmi", text); }

// The rows a "line" parserConfig shows, then those of a "hex" one.
const char *const kLineFrameKeys[] = {"separator", "terminator", "kvSeparator", "encoding"};
const char *const kHexFrameKeys[] = {"header",        "footer",         "matchBytes",   "matchOffset",
                                     "lengthOffset", "lengthSize",
                                     "lengthEndian",  "lengthCovers",   "fixedLength",  "checksum",
                                     "checksumStart", "checksumEndian", "idleTimeout",  "maxFrameLength",
                                     "fields", "lengthMode"};

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

void addReceiveFrameFields(QWidget *owner, const ReadFrameConfig &read, const WriteFrameConfig &write,
                           const AddFrameField &add, bool includeFieldText, QStringView onlyMode)
{
    const QJsonObject config = read();
    const auto store = [read, write](const char *key, const QJsonValue &value) {
        QJsonObject edited = read();
        edited.insert(QLatin1String(key), value);
        write(edited, QLatin1String(key));
    };
    const auto row = [&](const char *key, const QString &label, QWidget *editor, const QString &helpKey = {}) {
        add(QLatin1String(key), label, editor, helpKey);
    };
    const auto text = [&](const char *key, const QString &fallback) {
        auto *edit = new FluentQt::LineEdit(owner);
        edit->setText(config.value(QLatin1String(key)).toString(fallback));
        QObject::connect(edit, &QLineEdit::textChanged, owner, [store, key](const QString &t) { store(key, t); });
        return edit;
    };
    const auto number = [&](const char *key, int lo, int hi, int fallback) {
        auto *box = new FluentQt::SpinBox(owner);
        box->setRange(lo, hi);
        box->setValue(config.value(QLatin1String(key)).toInt(fallback));
        box->setKeyboardTracking(false);
        QObject::connect(box, qOverload<int>(&QSpinBox::valueChanged), owner, [store, key](int v) { store(key, v); });
        return box;
    };
    const auto choice = [&](const char *key, const QStringList &options) {
        auto *combo = new FluentQt::ComboBox(owner);
        addValueItems(combo, options);
        // Case aside: an encoding is compared in lower case, so "GBK" in a file shows as gbk.
        const QString stored = config.value(QLatin1String(key)).toString();
        for (int i = 0; i < options.size(); ++i) {
            if (options.at(i).compare(stored, Qt::CaseInsensitive) == 0) {
                combo->setCurrentIndex(i);
            }
        }
        QObject::connect(combo, &FluentQt::ComboBox::currentIndexChanged, owner,
                [combo, store, key]() { store(key, selectedValue(combo)); });
        return combo;
    };

    if (onlyMode.isEmpty() || onlyMode == QLatin1String("line")) {
        row("separator", tr("分隔符"), text("separator", QStringLiteral(",")));
        auto *terminator = new FluentQt::EditableComboBox(owner);
        terminator->addItems(
            {QStringLiteral("\\n"), QStringLiteral("\\r\\n"), QStringLiteral("\\r"), QStringLiteral(";")});
        terminator->setCurrentText(
            escapedTerminator(config.value(QStringLiteral("terminator")).toString(QStringLiteral("\n"))));
        QObject::connect(terminator, &FluentQt::EditableComboBox::currentTextChanged, owner, [store](const QString &t) {
            const QString value = unescapedTerminator(t);
            if (!value.isEmpty())
                store("terminator", value);
        });
        row("terminator", tr("行结束符"), terminator);
        row("kvSeparator", tr("键值分隔符"), choice("kvSeparator", LineOptions::kvSeparators()));
        row("encoding", tr("编码"), choice("encoding", LineOptions::encodings()));
    }
    if (onlyMode == QLatin1String("line"))
        return;
    row("header", tr("帧头(HEX)"), text("header", QStringLiteral("AA55")));
    row("footer", tr("帧尾(HEX)"), text("footer", QStringLiteral("55AA")));
    auto *match = text("matchBytes", {});
    match->setPlaceholderText(tr("可选，如 10 或 A5 01"));
    row("matchBytes", tr("匹配命令字(HEX)"), match);
    row("matchOffset", tr("命令字整帧偏移"), number("matchOffset", 0, 65535, 0));
    auto *lengthMode = new FluentQt::ComboBox(owner);
    lengthMode->addItems({tr("按边界或空闲间隔"), tr("固定长度"), tr("读取长度字段")});
    lengthMode->setCurrentIndex(config.value(QStringLiteral("fixedLength")).toInt() > 0 ? 1
                                : config.value(QStringLiteral("lengthOffset")).toInt(-1) >= 0 ? 2 : 0);
    row("lengthMode", tr("帧长方式"), lengthMode);
    auto *offset = number("lengthOffset", 0, 65535, 0);
    row("lengthOffset", tr("长度字段偏移"), offset);
    auto *lengthSize = new FluentQt::ComboBox(owner);
    lengthSize->addItems({QStringLiteral("1"), QStringLiteral("2")});
    lengthSize->setCurrentIndex(config.value(QStringLiteral("lengthSize")).toInt(1) == 2 ? 1 : 0);
    QObject::connect(lengthSize, &FluentQt::ComboBox::currentTextChanged, owner,
            [store](const QString &t) { store("lengthSize", t.toInt()); });
    row("lengthSize", tr("长度字节数"), lengthSize);
    row("lengthEndian", tr("长度字节序"), choice("lengthEndian", HexOptions::byteOrders()));
    row("lengthCovers", tr("长度含义"), choice("lengthCovers", HexOptions::lengthCovers()));
    auto *fixedLength = number("fixedLength", 1, 65536, 8);
    row("fixedLength", tr("固定帧长"), fixedLength);
    QObject::connect(lengthMode, &FluentQt::ComboBox::currentIndexChanged, owner,
            [read, write, offset, fixedLength](int selected) {
        auto edited = read();
        edited.insert(QStringLiteral("fixedLength"), selected == 1 ? fixedLength->value() : 0);
        edited.insert(QStringLiteral("lengthOffset"), selected == 2 ? offset->value() : -1);
        write(edited, QStringLiteral("lengthMode"));
    });
    row("maxFrameLength", tr("最大帧长(字节)"), number("maxFrameLength", 1, 65536, 65536));
    auto *timeout = number("idleTimeout", 0, 60000, 0);
    const auto showTimeoutDefault = [timeout](int mode) {
        timeout->setSpecialValueText(mode == 2 ? tr("自动（%1 ms）").arg(HexOptions::LengthRecoveryMs) : tr("关闭超时"));
    };
    QObject::connect(lengthMode, &FluentQt::ComboBox::currentIndexChanged, timeout, showTimeoutDefault);
    showTimeoutDefault(lengthMode->currentIndex());
    row("idleTimeout", tr("组帧超时(ms)"), timeout);
    row("checksum", tr("校验算法"), choice("checksum", HexOptions::checksums()));
    row("checksumStart", tr("校验起始字节"), number("checksumStart", 0, 65535, 0));
    row("checksumEndian", tr("校验值字节序"), choice("checksumEndian", HexOptions::byteOrders()));

    if (!includeFieldText)
        return;
    // The table is text; a line that is not a field shows its number and leaves the stored table alone.
    auto *fieldsBox = new QWidget(owner);
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
    QObject::connect(fields, &QPlainTextEdit::textChanged, owner, [fields, problem, store]() {
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


void updateReceiveFrameFields(const LinkParser::Format &format, const ShowFrameField &show)
{
    const bool hex = format.type == QLatin1String("hex");
    const auto &settings = format.settings;
    for (const char *key : kLineFrameKeys)
        show(QLatin1String(key), format.type == QLatin1String("line"));
    for (const char *key : kHexFrameKeys)
        show(QLatin1String(key), hex);
    if (!hex)
        return;
    show(QStringLiteral("matchOffset"), !settings.value(QStringLiteral("matchBytes")).toString().trimmed().isEmpty());
    const bool fixed = settings.value(QStringLiteral("fixedLength")).toInt() > 0;
    const bool length = !fixed && settings.value(QStringLiteral("lengthOffset")).toInt(-1) >= 0;
    for (const char *key : {"lengthOffset", "lengthSize", "lengthCovers"})
        show(QLatin1String(key), length);
    show(QStringLiteral("lengthEndian"), length && settings.value(QStringLiteral("lengthSize")).toInt(1) == 2);
    show(QStringLiteral("fixedLength"), fixed);
    show(QStringLiteral("maxFrameLength"), !fixed);
    const int checksum = HexOptions::checksums().indexOf(settings.value(QStringLiteral("checksum")).toString());
    show(QStringLiteral("checksumStart"), checksum > 0);
    show(QStringLiteral("checksumEndian"), checksum >= int(HexChecksum::Crc16Modbus));
}

QWidget *createChecksumRuleHint(FluentQt::ComboBox *algorithm, QWidget *parent)
{
    auto *hint = new FluentQt::CaptionLabel(parent);
    hint->setWordWrap(true);
    const auto update = [algorithm, hint] {
        QString rule;
        switch (HexChecksum(algorithm->currentIndex())) {
        case HexChecksum::None: break;
        case HexChecksum::Sum: rule = tr("逐字节求和，保留低 8 位。"); break;
        case HexChecksum::Xor: rule = tr("逐字节异或，结果占 1 字节。"); break;
        case HexChecksum::Crc8: rule = tr("多项式 0x07，初值 0x00，高位先算。"); break;
        case HexChecksum::Crc16Modbus: rule = tr("反射多项式 0xA001，初值 0xFFFF，低位先算。"); break;
        case HexChecksum::Crc16Ccitt: rule = tr("CCITT-FALSE，多项式 0x1021，初值 0xFFFF，高位先算。"); break;
        }
        hint->setVisible(!rule.isEmpty());
        hint->setText(rule + u' ' + tr("整帧字节从 0 编号。校验覆盖起始字节到校验字段前一字节。"));
    };
    QObject::connect(algorithm, &FluentQt::ComboBox::currentIndexChanged, hint, update);
    update();
    return hint;
}

} // namespace fhmi
