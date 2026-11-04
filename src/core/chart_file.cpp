#include "core/chart_file.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QIODevice>
#include <QtCore/QLocale>
#include <QtCore/QSet>
#include <QtCore/QStringDecoder>
#include <QtCore/QTemporaryFile>
#include <QtCore/QtEndian>

#include <cmath>
#include <cstring>
#include <limits>

namespace fhmi {

namespace {

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

// Decimal text already known to be well formed, rounded as Python rounds it. Qt refuses what overflows or
// underflows a double, where Python gives an infinity or a zero of the text's sign.
double decimalToDouble(const QByteArray &ascii)
{
    bool ok = false;
    const double value = QLocale::c().toDouble(QString::fromLatin1(ascii), &ok);
    if (ok || std::isinf(value)) {
        return value;
    }
    return ascii.startsWith('-') ? -0.0 : 0.0;
}

bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// float(text) as Python reads it: surrounding whitespace, any script's decimal digits and single underscores
// between digits are allowed, and inf / infinity / nan (any case) give those values.
bool parsePythonFloat(const QString &text, double *value)
{
    QByteArray ascii;
    for (const char32_t c : text.toUcs4()) {
        if (c < 127) {
            ascii += char(c);
        } else if (QChar::isSpace(c)) {
            ascii += ' ';
        } else if (QChar::category(c) == QChar::Number_DecimalDigit) {
            ascii += char('0' + QChar::digitValue(c));
        } else {
            return false;
        }
    }
    ascii = ascii.trimmed();
    if (ascii.contains('_')) {
        QByteArray plain;
        char previous = 0;
        for (const char c : std::as_const(ascii)) {
            if (c == '_' ? !isAsciiDigit(previous) : previous == '_' && !isAsciiDigit(c)) {
                return false;
            }
            if (c != '_') {
                plain += c;
            }
            previous = c;
        }
        if (previous == '_') {
            return false;
        }
        ascii = plain;
    }
    const bool negative = ascii.startsWith('-');
    const QByteArray body = (negative || ascii.startsWith('+') ? ascii.mid(1) : ascii).toLower();
    if (body == "inf" || body == "infinity") {
        *value = negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        return true;
    }
    if (body == "nan") {
        *value = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    qsizetype i = 0;
    const auto digits = [&]() {
        const qsizetype start = i;
        while (i < body.size() && isAsciiDigit(body.at(i))) {
            ++i;
        }
        return i - start;
    };
    qsizetype mantissa = digits();
    if (i < body.size() && body.at(i) == '.') {
        ++i;
        mantissa += digits();
    }
    if (mantissa == 0) {
        return false;
    }
    if (i < body.size() && body.at(i) == 'e') {
        ++i;
        if (i < body.size() && (body.at(i) == '+' || body.at(i) == '-')) {
            ++i;
        }
        if (digits() == 0) {
            return false;
        }
    }
    if (i != body.size()) {
        return false;
    }
    *value = decimalToDouble(ascii);
    return true;
}

bool pyIsSpace(char32_t c)
{
    return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f) || (c >= 128 && QChar::isSpace(c));
}

// What str.strip() leaves of a cell is empty.
bool isBlank(const QString &text)
{
    for (const char32_t c : text.toUcs4()) {
        if (!pyIsSpace(c)) {
            return false;
        }
    }
    return true;
}

QString finiteError() { return QCoreApplication::translate("fhmi", "曲线样本必须是有限数"); }

bool sampleFromText(const QString &text, double *value, QString *error)
{
    if (!parsePythonFloat(text, value)) {
        return fail(error, QCoreApplication::translate("fhmi", "不是数字: '%1'").arg(text));
    }
    return std::isfinite(*value) || fail(error, finiteError());
}

// A JSON sample as float() takes it: a number, a boolean or numeric text.
bool sampleFromJson(const PyJsonValue &item, double *value, QString *error)
{
    switch (item.kind) {
    case PyJsonValue::Bool:
        *value = item.flag ? 1 : 0;
        break;
    case PyJsonValue::Int:
        if (!std::isfinite(item.number)) {
            return fail(error, QCoreApplication::translate("fhmi", "整数太大，无法转换为浮点数"));
        }
        *value = item.number;
        break;
    case PyJsonValue::Float:
        *value = item.number;
        break;
    case PyJsonValue::String:
        return sampleFromText(item.text, value, error);
    default:
        return fail(error, QCoreApplication::translate("fhmi", "曲线样本必须是数字"));
    }
    return std::isfinite(*value) || fail(error, finiteError());
}

// Python's csv.reader (the excel dialect, not strict) over text whose line ends are already "\n".
QList<QStringList> csvRows(const QString &text)
{
    enum State
    {
        StartRecord,
        StartField,
        InField,
        InQuotedField,
        QuoteInQuotedField,
        EatLineEnd,
    };
    QList<QStringList> rows;
    QStringList fields;
    QString field;
    State state = StartRecord;
    const auto save = [&]() {
        fields.append(field);
        field.clear();
    };
    // A null QChar stands for the end of a line, which the csv module handles apart from "\n".
    const auto process = [&](QChar c, bool endOfLine) {
        const bool newline = !endOfLine && (c == QLatin1Char('\n') || c == QLatin1Char('\r'));
        switch (state) {
        case StartRecord:
            if (endOfLine) {
                return;
            }
            if (newline) {
                state = EatLineEnd;
                return;
            }
            state = StartField;
            Q_FALLTHROUGH();
        case StartField:
            if (newline || endOfLine) {
                save();
                state = endOfLine ? StartRecord : EatLineEnd;
            } else if (c == QLatin1Char('"')) {
                state = InQuotedField;
            } else if (c == QLatin1Char(',')) {
                save();
            } else {
                field += c;
                state = InField;
            }
            return;
        case InField:
            if (newline || endOfLine) {
                save();
                state = endOfLine ? StartRecord : EatLineEnd;
            } else if (c == QLatin1Char(',')) {
                save();
                state = StartField;
            } else {
                field += c;
            }
            return;
        case InQuotedField:
            if (endOfLine) {
                return;
            }
            if (c == QLatin1Char('"')) {
                state = QuoteInQuotedField;
            } else {
                field += c;
            }
            return;
        case QuoteInQuotedField:
            if (!endOfLine && c == QLatin1Char('"')) {
                field += c;
                state = InQuotedField;
            } else if (!endOfLine && c == QLatin1Char(',')) {
                save();
                state = StartField;
            } else if (newline || endOfLine) {
                save();
                state = endOfLine ? StartRecord : EatLineEnd;
            } else {
                field += c;
                state = InField;
            }
            return;
        case EatLineEnd:
            if (endOfLine) {
                state = StartRecord;
            }
            return;
        }
    };
    qsizetype start = 0;
    while (start < text.size()) {
        qsizetype end = text.indexOf(QLatin1Char('\n'), start);
        end = end < 0 ? text.size() : end + 1;
        for (qsizetype i = start; i < end; ++i) {
            process(text.at(i), false);
        }
        process(QChar(), true);
        if (state == StartRecord) {
            rows.append(fields);
            fields.clear();
        }
        start = end;
    }
    // A quoted field still open at the end of the text ends there.
    if (!field.isEmpty() || state == InQuotedField) {
        save();
        rows.append(fields);
    }
    return rows;
}

bool readCsv(const QString &content, QStringList *names, QList<QList<double>> *values, QString *error)
{
    // Python opens the file in text mode: "\r\n" and a lone "\r" both end a line.
    QString text = content;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QList<QStringList> rows = csvRows(text);
    const QStringList header = rows.value(0);
    QList<QList<double>> columns(header.size());
    for (qsizetype r = 1; r < rows.size(); ++r) {
        const QStringList &row = rows.at(r);
        for (qsizetype c = 0; c < header.size() && c < row.size(); ++c) {
            if (isBlank(row.at(c))) {
                continue;
            }
            double value = 0;
            if (!sampleFromText(row.at(c), &value, error)) {
                return false;
            }
            columns[c].append(value);
        }
    }
    *names = header;
    *values = columns;
    return true;
}

bool readJson(const QString &content, QStringList *names, QList<QList<double>> *values, QString *error)
{
    PyJsonValue document;
    if (!parsePyJson(content, &document, error)) {
        return false;
    }
    QList<const PyJsonValue *> channelNames;
    QList<const PyJsonValue *> channelValues;
    std::vector<PyJsonValue> keys;
    if (document.kind == PyJsonValue::Object) {
        keys.resize(document.keys.size());
        for (size_t i = 0; i < document.keys.size(); ++i) {
            keys[i].kind = PyJsonValue::String;
            keys[i].text = document.keys[i];
            channelNames.append(&keys[i]);
            channelValues.append(&document.items[i]);
        }
    } else if (document.kind == PyJsonValue::Array) {
        const QString shape = QCoreApplication::translate("fhmi", "曲线 JSON 通道必须包含 name 和 values");
        for (const PyJsonValue &entry : document.items) {
            if (entry.kind != PyJsonValue::Object) {
                return fail(error, shape);
            }
            // As dict(entry): a repeated member counts once, with its last value.
            const PyJsonValue *name = nullptr;
            const PyJsonValue *samples = nullptr;
            for (size_t i = 0; i < entry.keys.size(); ++i) {
                if (entry.keys[i] == QLatin1String("name")) {
                    name = &entry.items[i];
                } else if (entry.keys[i] == QLatin1String("values")) {
                    samples = &entry.items[i];
                } else {
                    return fail(error, shape);
                }
            }
            if (!name || !samples) {
                return fail(error, shape);
            }
            channelNames.append(name);
            channelValues.append(samples);
        }
    } else {
        return fail(error, QCoreApplication::translate("fhmi", "曲线 JSON 必须是对象或通道数组"));
    }
    for (qsizetype i = 0; i < channelNames.size(); ++i) {
        if (channelNames.at(i)->kind != PyJsonValue::String || channelValues.at(i)->kind != PyJsonValue::Array) {
            return fail(error, QCoreApplication::translate("fhmi", "曲线 JSON 通道名称必须是文本，样本必须是数组"));
        }
    }
    QStringList outNames;
    QList<QList<double>> outValues;
    for (qsizetype i = 0; i < channelNames.size(); ++i) {
        outNames.append(channelNames.at(i)->text);
        QList<double> samples;
        for (const PyJsonValue &item : channelValues.at(i)->items) {
            if (item.kind == PyJsonValue::Null) {
                continue;
            }
            double value = 0;
            if (!sampleFromJson(item, &value, error)) {
                return false;
            }
            samples.append(value);
        }
        outValues.append(samples);
    }
    *names = outNames;
    *values = outValues;
    return true;
}

// Python's json.loads: strict strings, NaN / Infinity / -Infinity accepted, nothing after the value.
class PyJsonReader
{
  public:
    explicit PyJsonReader(const QString &text) : m_text(text) {}

    bool read(PyJsonValue *out, QString *error)
    {
        skipSpace();
        if (!value(out, 0)) {
            return fail(error, m_error);
        }
        skipSpace();
        if (m_pos != m_text.size()) {
            return fail(error, problem(QCoreApplication::translate("fhmi", "多余的数据")));
        }
        return true;
    }

  private:
    QString problem(const QString &what) const
    {
        return QCoreApplication::translate("fhmi", "JSON 格式错误: %1（第 %2 个字符）").arg(what).arg(m_pos + 1);
    }

    bool error(const QString &what)
    {
        m_error = problem(what);
        return false;
    }

    QChar peek(qsizetype ahead = 0) const { return m_pos + ahead < m_text.size() ? m_text.at(m_pos + ahead) : QChar(); }

    static int hexValue(QChar c)
    {
        const char16_t u = c.unicode();
        if (u >= '0' && u <= '9') {
            return u - '0';
        }
        if ((u | 0x20) >= 'a' && (u | 0x20) <= 'f') {
            return (u | 0x20) - 'a' + 10;
        }
        return -1;
    }

    bool startsWith(QLatin1StringView word) const { return QStringView(m_text).mid(m_pos).startsWith(word); }

    void skipSpace()
    {
        while (m_pos < m_text.size()) {
            const QChar c = m_text.at(m_pos);
            if (c != QLatin1Char(' ') && c != QLatin1Char('\t') && c != QLatin1Char('\n') && c != QLatin1Char('\r')) {
                return;
            }
            ++m_pos;
        }
    }

    bool value(PyJsonValue *out, int depth)
    {
        if (depth > 512) {
            return error(QCoreApplication::translate("fhmi", "嵌套太深"));
        }
        const QChar c = peek();
        if (c == QLatin1Char('{')) {
            return object(out, depth);
        }
        if (c == QLatin1Char('[')) {
            return array(out, depth);
        }
        if (c == QLatin1Char('"')) {
            out->kind = PyJsonValue::String;
            return string(&out->text);
        }
        const struct
        {
            QLatin1StringView word;
            PyJsonValue::Kind kind;
            double number;
            bool flag;
        } constants[] = {
            {QLatin1StringView("null"), PyJsonValue::Null, 0, false},
            {QLatin1StringView("true"), PyJsonValue::Bool, 0, true},
            {QLatin1StringView("false"), PyJsonValue::Bool, 0, false},
            {QLatin1StringView("NaN"), PyJsonValue::Float, std::numeric_limits<double>::quiet_NaN(), false},
            {QLatin1StringView("Infinity"), PyJsonValue::Float, std::numeric_limits<double>::infinity(), false},
            {QLatin1StringView("-Infinity"), PyJsonValue::Float, -std::numeric_limits<double>::infinity(), false},
        };
        for (const auto &constant : constants) {
            if (startsWith(constant.word)) {
                m_pos += constant.word.size();
                out->kind = constant.kind;
                out->number = constant.number;
                out->flag = constant.flag;
                return true;
            }
        }
        return number(out);
    }

    bool object(PyJsonValue *out, int depth)
    {
        out->kind = PyJsonValue::Object;
        ++m_pos;
        skipSpace();
        if (peek() == QLatin1Char('}')) {
            ++m_pos;
            return true;
        }
        for (;;) {
            if (peek() != QLatin1Char('"')) {
                return error(QCoreApplication::translate("fhmi", "缺少用双引号括起的名称"));
            }
            QString key;
            if (!string(&key)) {
                return false;
            }
            skipSpace();
            if (peek() != QLatin1Char(':')) {
                return error(QCoreApplication::translate("fhmi", "缺少 ':'"));
            }
            ++m_pos;
            skipSpace();
            PyJsonValue item;
            if (!value(&item, depth + 1)) {
                return false;
            }
            out->keys.push_back(key);
            out->items.push_back(std::move(item));
            skipSpace();
            if (peek() == QLatin1Char('}')) {
                ++m_pos;
                return true;
            }
            if (peek() != QLatin1Char(',')) {
                return error(QCoreApplication::translate("fhmi", "缺少 ',' 或 '}'"));
            }
            ++m_pos;
            skipSpace();
        }
    }

    bool array(PyJsonValue *out, int depth)
    {
        out->kind = PyJsonValue::Array;
        ++m_pos;
        skipSpace();
        if (peek() == QLatin1Char(']')) {
            ++m_pos;
            return true;
        }
        for (;;) {
            PyJsonValue item;
            if (!value(&item, depth + 1)) {
                return false;
            }
            out->items.push_back(std::move(item));
            skipSpace();
            if (peek() == QLatin1Char(']')) {
                ++m_pos;
                return true;
            }
            if (peek() != QLatin1Char(',')) {
                return error(QCoreApplication::translate("fhmi", "缺少 ',' 或 ']'"));
            }
            ++m_pos;
            skipSpace();
        }
    }

    bool string(QString *out)
    {
        ++m_pos; // the opening quote
        out->clear();
        for (;;) {
            if (m_pos >= m_text.size()) {
                return error(QCoreApplication::translate("fhmi", "字符串没有结束"));
            }
            const QChar c = m_text.at(m_pos);
            if (c == QLatin1Char('"')) {
                ++m_pos;
                return true;
            }
            if (c.unicode() < 0x20) {
                return error(QCoreApplication::translate("fhmi", "字符串里有控制字符"));
            }
            if (c != QLatin1Char('\\')) {
                *out += c;
                ++m_pos;
                continue;
            }
            const QChar escape = peek(1);
            static const QString simple = QStringLiteral("\"\\/bfnrt");
            static const QString meaning = QStringLiteral("\"\\/\b\f\n\r\t");
            if (const qsizetype at = simple.indexOf(escape); at >= 0 && !escape.isNull()) {
                *out += meaning.at(at);
                m_pos += 2;
            } else if (escape == QLatin1Char('u')) {
                char16_t unit = 0;
                for (int k = 2; k < 6; ++k) {
                    const int hex = hexValue(peek(k));
                    if (hex < 0) {
                        return error(QCoreApplication::translate("fhmi", "无效的 \\u 转义"));
                    }
                    unit = char16_t(unit * 16 + hex);
                }
                // Surrogates go in as UTF-16 units: a pair joins as Python joins it, a lone one stays lone.
                *out += QChar(unit);
                m_pos += 6;
            } else {
                return error(QCoreApplication::translate("fhmi", "无效的转义"));
            }
        }
    }

    bool number(PyJsonValue *out)
    {
        const qsizetype start = m_pos;
        const auto digit = [this](qsizetype ahead = 0) {
            const QChar c = peek(ahead);
            return c >= QLatin1Char('0') && c <= QLatin1Char('9');
        };
        if (peek() == QLatin1Char('-')) {
            ++m_pos;
        }
        if (!digit()) {
            m_pos = start;
            return error(QCoreApplication::translate("fhmi", "缺少值"));
        }
        if (peek() == QLatin1Char('0')) {
            ++m_pos;
        } else {
            while (digit()) {
                ++m_pos;
            }
        }
        bool integral = true;
        if (peek() == QLatin1Char('.') && digit(1)) {
            integral = false;
            ++m_pos;
            while (digit()) {
                ++m_pos;
            }
        }
        if ((peek() == QLatin1Char('e') || peek() == QLatin1Char('E')) &&
            (digit(1) || ((peek(1) == QLatin1Char('+') || peek(1) == QLatin1Char('-')) && digit(2)))) {
            integral = false;
            m_pos += 2;
            while (digit()) {
                ++m_pos;
            }
        }
        const QByteArray text = m_text.mid(start, m_pos - start).toLatin1();
        out->kind = integral ? PyJsonValue::Int : PyJsonValue::Float;
        out->number = decimalToDouble(text);
        if (integral && out->number == 0) {
            out->number = 0; // float(-0) is 0.0
        }
        return true;
    }

    const QString &m_text;
    qsizetype m_pos = 0;
    QString m_error;
};

// json.dumps(text, ensure_ascii=False)
QString pyJsonString(const QString &text)
{
    QString out = QStringLiteral("\"");
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '"':
            out += QStringLiteral("\\\"");
            break;
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
        case '\b':
            out += QStringLiteral("\\b");
            break;
        case '\f':
            out += QStringLiteral("\\f");
            break;
        default:
            if (c.unicode() < 0x20) {
                out += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
            } else {
                out += c;
            }
        }
    }
    return out + QLatin1Char('"');
}

// One field as csv.writer writes it (QUOTE_MINIMAL); a record of one empty field is written as "".
QString csvField(const QString &field, bool alone)
{
    if (alone && field.isEmpty()) {
        return QStringLiteral("\"\"");
    }
    for (const QChar c : field) {
        if (c == QLatin1Char(',') || c == QLatin1Char('"') || c == QLatin1Char('\r') || c == QLatin1Char('\n')) {
            return QLatin1Char('"') + QString(field).replace(QLatin1Char('"'), QStringLiteral("\"\"")) +
                   QLatin1Char('"');
        }
    }
    return field;
}

// Text written in pieces, so a long recording never sits in memory as a whole.
class ChunkedWriter
{
  public:
    explicit ChunkedWriter(QIODevice *out) : m_out(out) {}

    void add(const QString &text)
    {
        m_buffer += text.toUtf8();
        if (m_buffer.size() >= 1 << 16) {
            flush();
        }
    }

    bool flush()
    {
        if (m_ok && !m_buffer.isEmpty()) {
            m_ok = m_out->write(m_buffer) == m_buffer.size();
        }
        m_buffer.clear();
        return m_ok;
    }

  private:
    QIODevice *m_out;
    QByteArray m_buffer;
    bool m_ok = true;
};

} // namespace

QString pythonFloatRepr(double value)
{
    const QString sign = std::signbit(value) ? QStringLiteral("-") : QString();
    if (value == 0) {
        return sign + QStringLiteral("0.0");
    }
    // The shortest digits that read back as this double, d.ddd e±x; Python writes them out in full while the
    // decimal point falls within 16 places of them, and as d.ddde±xx otherwise.
    const QString shortest = QString::number(std::fabs(value), 'e', QLocale::FloatingPointShortest);
    const qsizetype e = shortest.indexOf(QLatin1Char('e'));
    const QString digits = shortest.left(e).remove(QLatin1Char('.'));
    const int exponent = shortest.mid(e + 1).toInt();
    const int point = exponent + 1; // digits before the decimal point
    if (point <= -4 || point > 16) {
        const QString mantissa = digits.size() > 1 ? digits.left(1) + QLatin1Char('.') + digits.mid(1) : digits;
        return sign + mantissa + (exponent < 0 ? QStringLiteral("e-") : QStringLiteral("e+")) +
               QStringLiteral("%1").arg(std::abs(exponent), 2, 10, QLatin1Char('0'));
    }
    if (point <= 0) {
        return sign + QStringLiteral("0.") + QString(-point, QLatin1Char('0')) + digits;
    }
    if (point >= digits.size()) {
        return sign + digits + QString(point - digits.size(), QLatin1Char('0')) + QStringLiteral(".0");
    }
    return sign + digits.left(point) + QLatin1Char('.') + digits.mid(point);
}

ChartFileFormat chartFileFormat(const QString &path)
{
    return path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive) ? ChartFileFormat::Json : ChartFileFormat::Csv;
}

bool writeChartFile(QIODevice *out, ChartFileFormat format, const QStringList &names,
                    const QList<ChartSamples> &samples, QString *error)
{
    ChunkedWriter writer(out);
    double value = 0;
    if (format == ChartFileFormat::Json) {
        // Python writes an object while the names are unique; an object cannot hold one name twice.
        const bool repeated = QSet<QString>(names.begin(), names.end()).size() != names.size();
        writer.add(repeated ? QStringLiteral("[") : QStringLiteral("{"));
        for (qsizetype i = 0; i < names.size(); ++i) {
            if (i) {
                writer.add(QStringLiteral(","));
            }
            const QString name = pyJsonString(names.at(i));
            writer.add(repeated ? QStringLiteral("{\"name\":%1,\"values\":[").arg(name) : name + QStringLiteral(":["));
            for (bool first = true; i < samples.size() && samples.at(i)(&value); first = false) {
                writer.add(first ? pythonFloatRepr(value) : QLatin1Char(',') + pythonFloatRepr(value));
            }
            writer.add(repeated ? QStringLiteral("]}") : QStringLiteral("]"));
        }
        writer.add(repeated ? QStringLiteral("]") : QStringLiteral("}"));
    } else {
        const auto row = [&writer](const QStringList &fields) {
            QStringList quoted;
            for (const QString &field : fields) {
                quoted.append(csvField(field, fields.size() == 1));
            }
            writer.add(quoted.join(QLatin1Char(',')) + QStringLiteral("\r\n"));
        };
        row(names);
        // zip_longest: a row per sample number until every curve has run out, blank cells for those that have.
        QList<bool> done(names.size(), false);
        for (;;) {
            QStringList cells;
            bool any = false;
            for (qsizetype i = 0; i < names.size(); ++i) {
                done[i] = done.at(i) || i >= samples.size() || !samples.at(i)(&value);
                cells.append(done.at(i) ? QString() : pythonFloatRepr(value));
                any = any || !done.at(i);
            }
            if (!any) {
                break;
            }
            row(cells);
        }
    }
    return writer.flush() || fail(error, out->errorString());
}

bool readChartFile(const QByteArray &content, ChartFileFormat format, QStringList *names, QList<QList<double>> *values,
                   QString *error)
{
    // Python's "utf-8" codec: a byte order mark stays part of the text, and bytes that are not UTF-8 fail.
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::ConvertInitialBom);
    const QString text = decoder(content);
    if (decoder.hasError()) {
        return fail(error, QCoreApplication::translate("fhmi", "文件不是 UTF-8 文本"));
    }
    return format == ChartFileFormat::Json ? readJson(text, names, values, error) : readCsv(text, names, values, error);
}

bool parsePyJson(const QString &text, PyJsonValue *out, QString *error)
{
    PyJsonValue value;
    if (!PyJsonReader(text).read(&value, error)) {
        return false;
    }
    *out = std::move(value);
    return true;
}

// ---------------------------------------------------------------------------
// ChartHistory
// ---------------------------------------------------------------------------

ChartHistory::ChartHistory(int capacity) : m_capacity(std::max(1, capacity)) {}

ChartHistory::~ChartHistory() = default;

bool ChartHistory::append(double value, QString *error)
{
    if (!std::isfinite(value)) {
        return false;
    }
    if (!m_file) {
        auto file = std::make_unique<QTemporaryFile>();
        if (!file->open()) {
            return fail(error, file->errorString());
        }
        m_file = std::move(file);
    }
    if (m_readSinceWrite) {
        m_file->seek(m_count * qint64(sizeof(double)));
        m_readSinceWrite = false;
    }
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    char bytes[sizeof bits];
    qToLittleEndian(bits, bytes);
    if (m_file->write(bytes, sizeof bytes) != qint64(sizeof bytes)) {
        return fail(error, m_file->errorString());
    }
    m_cache.push_back(value);
    if (qsizetype(m_cache.size()) > m_capacity) {
        m_cache.pop_front();
    }
    ++m_count;
    return true;
}

QList<double> ChartHistory::tail(int n) const
{
    const auto take = std::min<qsizetype>(std::max(0, n), qsizetype(m_cache.size()));
    return QList<double>(m_cache.end() - take, m_cache.end());
}

std::optional<double> ChartHistory::last() const
{
    return m_cache.empty() ? std::nullopt : std::optional<double>(m_cache.back());
}

ChartSamples ChartHistory::samples(bool *failed) const
{
    if (!m_file) {
        return [](double *) { return false; };
    }
    m_file->flush();
    m_readSinceWrite = true;
    struct Cursor
    {
        qint64 offset = 0;
        QByteArray chunk;
        qsizetype at = 0;
    };
    auto cursor = std::make_shared<Cursor>();
    QTemporaryFile *file = m_file.get();
    const qint64 total = m_count * qint64(sizeof(double));
    return [file, total, cursor, failed](double *value) {
        if (cursor->at + qsizetype(sizeof(double)) > cursor->chunk.size()) {
            if (cursor->offset >= total) {
                return false;
            }
            cursor->chunk = file->seek(cursor->offset) ? file->read(std::min<qint64>(1 << 16, total - cursor->offset))
                                                       : QByteArray();
            if (cursor->chunk.isEmpty() || cursor->chunk.size() % qsizetype(sizeof(double))) {
                if (failed) {
                    *failed = true;
                }
                return false;
            }
            cursor->offset += cursor->chunk.size();
            cursor->at = 0;
        }
        const quint64 bits = qFromLittleEndian<quint64>(cursor->chunk.constData() + cursor->at);
        std::memcpy(value, &bits, sizeof bits);
        cursor->at += qsizetype(sizeof(double));
        return true;
    };
}

void ChartHistory::clear()
{
    m_file.reset();
    m_cache.clear();
    m_count = 0;
    m_readSinceWrite = false;
}

} // namespace fhmi
