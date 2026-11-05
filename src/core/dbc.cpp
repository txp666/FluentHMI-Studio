#include "core/dbc.h"

#include "core/protocol_parser.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStringDecoder>

namespace fhmi {

namespace {

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

// DBC IDs are decimal with bit 31 marking an extended frame.
constexpr quint32 kExtendedBit = 0x80000000U;
// Vector's pseudo message that holds signals belonging to no message.
constexpr quint32 kIndependentSignals = 0xC0000000U;

struct Token
{
    QString text;
    bool quoted = false;
};

// Words, quoted strings (with \" escapes) and the punctuation : , as separate tokens.
QList<Token> tokenize(const QString &statement)
{
    QList<Token> out;
    qsizetype i = 0;
    const qsizetype n = statement.size();
    while (i < n) {
        const QChar c = statement.at(i);
        if (c.isSpace()) {
            ++i;
        } else if (c == u'"') {
            QString text;
            ++i;
            while (i < n && statement.at(i) != u'"') {
                if (statement.at(i) == u'\\' && i + 1 < n) {
                    ++i;
                }
                text += statement.at(i++);
            }
            ++i; // the closing quote
            out.append({text, true});
        } else if (c == u':' || c == u',') {
            out.append({QString(c), false});
            ++i;
        } else {
            const qsizetype start = i;
            while (i < n && !statement.at(i).isSpace() && statement.at(i) != u'"' && statement.at(i) != u':' &&
                   statement.at(i) != u',') {
                ++i;
            }
            out.append({statement.mid(start, i - start), false});
        }
    }
    return out;
}

// Walks the text statement by statement, counting lines for the warnings.
class Scanner
{
  public:
    explicit Scanner(const QString &text) : m_text(text) {}

    bool atEnd() const { return m_pos >= m_text.size(); }
    int line() const { return m_line; }

    void skipSpace()
    {
        while (!atEnd() && m_text.at(m_pos).isSpace()) {
            if (m_text.at(m_pos) == u'\n') {
                ++m_line;
            }
            ++m_pos;
        }
    }

    QString word()
    {
        const qsizetype start = m_pos;
        while (!atEnd() && (m_text.at(m_pos).isLetterOrNumber() || m_text.at(m_pos) == u'_')) {
            ++m_pos;
        }
        return m_text.mid(start, m_pos - start);
    }

    // The rest of the current line, which is consumed with its line break.
    QString restOfLine()
    {
        const qsizetype start = m_pos;
        while (!atEnd() && m_text.at(m_pos) != u'\n') {
            ++m_pos;
        }
        const QString line = m_text.mid(start, m_pos - start);
        if (!atEnd()) {
            ++m_pos;
            ++m_line;
        }
        return line;
    }

    // The next line without consuming it.
    QString peekLine() const
    {
        const qsizetype end = m_text.indexOf(u'\n', m_pos);
        return m_text.mid(m_pos, end < 0 ? -1 : end - m_pos);
    }

    // Up to the next ';' outside quotes, which is consumed; statements like CM_ span lines and quote ';'.
    QString untilSemicolon()
    {
        const qsizetype start = m_pos;
        bool quoted = false;
        while (!atEnd()) {
            const QChar c = m_text.at(m_pos);
            if (c == u'\n') {
                ++m_line;
            }
            if (quoted && c == u'\\') {
                m_pos += 2;
                continue;
            }
            if (c == u'"') {
                quoted = !quoted;
            } else if (c == u';' && !quoted) {
                const QString statement = m_text.mid(start, m_pos - start);
                ++m_pos;
                return statement;
            }
            ++m_pos;
        }
        return m_text.mid(start);
    }

  private:
    const QString &m_text;
    qsizetype m_pos = 0;
    int m_line = 1;
};

struct AttributeDef
{
    QStringList enumValues;
    std::optional<Token> defaultValue;
};

QString decodeText(const QByteArray &bytes)
{
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString text = utf8(bytes);
    if (!utf8.hasError()) {
        return text;
    }
    return LineParser::decode(bytes, QStringLiteral("gbk"));
}

std::optional<qint64> integerToken(const Token &token)
{
    bool ok = false;
    const qlonglong value = token.text.toLongLong(&ok);
    if (ok) {
        return value;
    }
    const qulonglong unsignedValue = token.text.toULongLong(&ok); // a raw value above 2^63 keeps its bits
    return ok ? std::optional<qint64>(qint64(unsignedValue)) : std::nullopt;
}

} // namespace

const CanMessageDef *CanDatabase::find(const CanId &id) const
{
    const auto it = m_byId.constFind(id.key());
    return it == m_byId.constEnd() ? nullptr : &messages.at(*it);
}

const CanMessageDef *CanDatabase::findByName(const QString &name) const
{
    const auto it = m_byName.constFind(name);
    return it == m_byName.constEnd() ? nullptr : &messages.at(*it);
}

void CanDatabase::index()
{
    m_byId.clear();
    m_byName.clear();
    for (qsizetype i = 0; i < messages.size(); ++i) {
        m_byId.insert(messages.at(i).id.key(), i);
        m_byName.insert(messages.at(i).name, i);
    }
}

namespace Dbc {

std::optional<CanDatabase> parse(const QByteArray &bytes, QString *error)
{
    const QString text = decodeText(bytes);
    CanDatabase db;
    const auto warn = [&db](int line, const QString &why) {
        db.warnings << QCoreApplication::translate("fhmi", "DBC 第 %1 行: %2，已跳过").arg(line).arg(why);
    };

    QHash<quint32, qsizetype> byRawId; // the DBC's own ID, with bit 31
    qsizetype current = -1;            // the message SG_ lines belong to; -1 after a skipped one
    QSet<qsizetype> multiplexWarned;
    QHash<QString, AttributeDef> attributes;
    QHash<quint32, QHash<QString, Token>> messageAttributes;
    QHash<QPair<quint32, QString>, QHash<QString, Token>> signalAttributes;
    QHash<QPair<quint32, QString>, QMap<qint64, QString>> descriptions;
    QHash<QPair<quint32, QString>, int> valueTypes;

    static const QRegularExpression messagePattern(
        QStringLiteral("^\\s*(\\d+)\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*:\\s*(\\d+)\\s*(\\S*)"));
    static const QRegularExpression keywordLine(QStringLiteral("^\\s*[A-Za-z_][A-Za-z0-9_]*\\s*$"));
    static const QSet<QString> statementKeywords = {
        QStringLiteral("CM_"),         QStringLiteral("BA_DEF_"),        QStringLiteral("BA_DEF_DEF_"),
        QStringLiteral("BA_"),         QStringLiteral("VAL_"),           QStringLiteral("VAL_TABLE_"),
        QStringLiteral("SIG_VALTYPE_"), QStringLiteral("BO_TX_BU_"),     QStringLiteral("EV_"),
        QStringLiteral("ENVVAR_DATA_"), QStringLiteral("SGTYPE_"),       QStringLiteral("SGTYPE_VAL_"),
        QStringLiteral("SIG_GROUP_"),  QStringLiteral("SIG_TYPE_REF_"),  QStringLiteral("BA_DEF_SGTYPE_"),
        QStringLiteral("BA_SGTYPE_"),  QStringLiteral("BA_DEF_REL_"),    QStringLiteral("BA_REL_"),
        QStringLiteral("BA_DEF_DEF_REL_"), QStringLiteral("BU_SG_REL_"), QStringLiteral("BU_EV_REL_"),
        QStringLiteral("BU_BO_REL_"),  QStringLiteral("SG_MUL_VAL_"),    QStringLiteral("CAT_DEF_"),
        QStringLiteral("CAT_"),        QStringLiteral("FILTER"),         QStringLiteral("EV_DATA_")};

    Scanner scanner(text);
    while (true) {
        scanner.skipSpace();
        if (scanner.atEnd()) {
            break;
        }
        const int line = scanner.line();
        const QString keyword = scanner.word();
        if (keyword == QLatin1String("BO_")) {
            const QString rest = scanner.restOfLine();
            current = -1;
            const QRegularExpressionMatch match = messagePattern.match(rest);
            bool idOk = false;
            const quint32 rawId = match.hasMatch() ? match.captured(1).toUInt(&idOk) : 0;
            const int length = match.captured(3).toInt();
            if (!match.hasMatch() || !idOk || length > 64) {
                warn(line, QCoreApplication::translate("fhmi", "报文格式错误"));
                continue;
            }
            if (rawId == kIndependentSignals) {
                continue;
            }
            CanMessageDef message;
            message.name = match.captured(2);
            message.id = {rawId & Can::MaxExtendedId, (rawId & kExtendedBit) != 0};
            message.length = length;
            if ((!message.id.extended && message.id.id > Can::MaxStandardId) || byRawId.contains(rawId)) {
                warn(line, QCoreApplication::translate("fhmi", "报文 %1 的 ID 无效或重复").arg(message.name));
                continue;
            }
            byRawId.insert(rawId, db.messages.size());
            current = db.messages.size();
            db.messages.append(message);
        } else if (keyword == QLatin1String("SG_")) {
            const QString rest = scanner.restOfLine();
            if (current < 0) {
                continue; // the message was skipped, and said so
            }
            CanMessageDef &message = db.messages[current];
            CanSignalDef signal;
            QString mux;
            QString receivers;
            QString why;
            if (!parseSignalBody(rest, &signal, &mux, &receivers, &why)) {
                warn(line, why);
                continue;
            }
            if (mux.startsWith(QLatin1Char('m'))) {
                if (!multiplexWarned.contains(current)) {
                    multiplexWarned.insert(current);
                    warn(line, QCoreApplication::translate("fhmi", "报文 %1 的多路复用信号不支持").arg(message.name));
                }
                continue;
            }
            bool twice = false;
            for (const CanSignalDef &existing : message.signalDefs) {
                twice = twice || existing.name == signal.name;
            }
            if (twice) {
                warn(line, QCoreApplication::translate("fhmi", "报文 %1 的信号 %2 重复").arg(message.name, signal.name));
                continue;
            }
            message.signalDefs.append(signal);
        } else if (keyword == QLatin1String("NS_")) {
            scanner.restOfLine();
            // The list of new symbols: one keyword per line, which must not read as statements.
            while (!scanner.atEnd()) {
                const QString next = scanner.peekLine();
                if (!next.trimmed().isEmpty() && !keywordLine.match(next).hasMatch()) {
                    break;
                }
                const QString word = next.trimmed();
                if (word == QLatin1String("BS_") || word == QLatin1String("BU_") || word == QLatin1String("BO_")) {
                    break;
                }
                scanner.restOfLine();
            }
        } else if (statementKeywords.contains(keyword)) {
            const QList<Token> tokens = tokenize(scanner.untilSemicolon());
            const auto rawIdAt = [&tokens](int index) -> std::optional<quint32> {
                bool ok = false;
                const quint32 value = index < tokens.size() ? tokens.at(index).text.toUInt(&ok) : 0;
                return ok ? std::optional<quint32>(value) : std::nullopt;
            };
            if (keyword == QLatin1String("BA_DEF_")) {
                // [object] "name" type values...
                const int nameAt = !tokens.isEmpty() && !tokens.first().quoted ? 1 : 0;
                if (nameAt < tokens.size() && tokens.at(nameAt).quoted) {
                    AttributeDef &def = attributes[tokens.at(nameAt).text];
                    if (nameAt + 1 < tokens.size() && tokens.at(nameAt + 1).text == QLatin1String("ENUM")) {
                        for (int i = nameAt + 2; i < tokens.size(); ++i) {
                            if (tokens.at(i).quoted) {
                                def.enumValues << tokens.at(i).text;
                            }
                        }
                    }
                }
            } else if (keyword == QLatin1String("BA_DEF_DEF_")) {
                if (tokens.size() >= 2 && tokens.first().quoted) {
                    attributes[tokens.first().text].defaultValue = tokens.at(1);
                }
            } else if (keyword == QLatin1String("BA_")) {
                // "name" BO_ <id> value | "name" SG_ <id> <signal> value
                if (tokens.size() >= 4 && tokens.first().quoted && tokens.at(1).text == QLatin1String("BO_")) {
                    if (const auto id = rawIdAt(2)) {
                        messageAttributes[*id].insert(tokens.first().text, tokens.at(3));
                    }
                } else if (tokens.size() >= 5 && tokens.first().quoted &&
                           tokens.at(1).text == QLatin1String("SG_")) {
                    if (const auto id = rawIdAt(2)) {
                        signalAttributes[{*id, tokens.at(3).text}].insert(tokens.first().text, tokens.at(4));
                    }
                }
            } else if (keyword == QLatin1String("VAL_")) {
                // <id> <signal> (<value> "<text>")* ; environment variables have no ID and are left out.
                if (const auto id = rawIdAt(0); id && tokens.size() >= 2) {
                    QMap<qint64, QString> &table = descriptions[{*id, tokens.at(1).text}];
                    for (int i = 2; i + 1 < tokens.size(); i += 2) {
                        const auto value = integerToken(tokens.at(i));
                        if (value && tokens.at(i + 1).quoted) {
                            table.insert(*value, tokens.at(i + 1).text);
                        }
                    }
                }
            } else if (keyword == QLatin1String("SIG_VALTYPE_")) {
                // <id> <signal> : <type>
                if (const auto id = rawIdAt(0); id && tokens.size() >= 4) {
                    valueTypes.insert({*id, tokens.at(1).text}, tokens.last().text.toInt());
                }
            }
        } else {
            // VERSION, BS_, BU_ and anything unknown take one line.
            scanner.restOfLine();
        }
    }

    if (db.messages.isEmpty()) {
        fail(error, QCoreApplication::translate("fhmi", "DBC 文件中没有报文"));
        return std::nullopt;
    }

    // Attributes: a message's own value, else the definition's default.
    const auto attribute = [&attributes](const QHash<QString, Token> &own, const QString &name) {
        const auto it = own.constFind(name);
        if (it != own.constEnd()) {
            return std::optional<Token>(*it);
        }
        return attributes.value(name).defaultValue;
    };
    const QString cycleName = QStringLiteral("GenMsgCycleTime");
    const QString formatName = QStringLiteral("VFrameFormat");
    const QString startName = QStringLiteral("GenSigStartValue");
    const QStringList formats = attributes.value(formatName).enumValues;
    for (auto it = byRawId.constBegin(); it != byRawId.constEnd(); ++it) {
        CanMessageDef &message = db.messages[it.value()];
        const QHash<QString, Token> own = messageAttributes.value(it.key());
        if (const auto cycle = attribute(own, cycleName)) {
            message.cycleTime = std::max(0, int(cycle->text.toDouble()));
        }
        if (const auto format = attribute(own, formatName)) {
            // An enum value is its index; a default may be written as the name.
            bool isIndex = false;
            const int index = format->text.toInt(&isIndex);
            const QString name = format->quoted || !isIndex ? format->text : formats.value(index);
            message.fd = name.endsWith(QLatin1String("_FD"));
        }
        message.fd = message.fd || message.length > 8;
        for (CanSignalDef &signal : message.signalDefs) {
            const QPair<quint32, QString> key{it.key(), signal.name};
            signal.valueDescriptions = descriptions.value(key);
            if (const auto start = attribute(signalAttributes.value(key), startName)) {
                bool ok = false;
                const double value = start->text.toDouble(&ok);
                if (ok) {
                    signal.startValue = value;
                }
            }
            const int type = valueTypes.value(key, 0);
            if (type == 1 || type == 2) {
                CanSignalDef typed = signal;
                typed.valueType = type == 1 ? CanSignalDef::ValueType::Float32 : CanSignalDef::ValueType::Double;
                QString why;
                if (typed.validate(&why)) {
                    signal = typed;
                } else {
                    db.warnings << why;
                }
            }
        }
    }
    db.index();
    return db;
}

std::optional<CanDatabase> load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, QCoreApplication::translate("fhmi", "无法读取 DBC 文件: %1").arg(path));
        return std::nullopt;
    }
    return parse(file.readAll(), error);
}

} // namespace Dbc

} // namespace fhmi
