#include "core/s7_codec.h"
#include <QtCore/QCoreApplication>

#include <QtCore/QMetaType>
#include <QtCore/QRegularExpression>
#include <QtCore/QtEndian>

#include <cmath>
#include <cstring>
#include <limits>

namespace fhmi::S7 {
namespace {
QString tr(const char *source) { return QCoreApplication::translate("fhmi", source); }
bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

bool numericVariant(const QVariant &value)
{
    switch (value.metaType().id()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Short:
    case QMetaType::UShort:
    case QMetaType::Long:
    case QMetaType::ULong:
    case QMetaType::Char:
    case QMetaType::SChar:
    case QMetaType::UChar: return true;
    default: return false;
    }
}

bool asDouble(const QVariant &value, double *out)
{
    if (!numericVariant(value) && value.metaType().id() != QMetaType::QString) return false;
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok || !std::isfinite(number)) return false;
    *out = number;
    return true;
}
} // namespace

bool parseDataType(const QString &text, DataType *type, QString *error)
{
    const QString name = text.trimmed().toUpper();
    DataType out;
    if (name == QLatin1String("BOOL")) out = {DataType::Kind::Bool, 1, QStringLiteral("BOOL")};
    else if (name == QLatin1String("BYTE"))
        out = {DataType::Kind::UInt8, 1, QStringLiteral("BYTE")};
    else if (name == QLatin1String("SINT"))
        out = {DataType::Kind::Int8, 1, QStringLiteral("SINT")};
    else if (name == QLatin1String("WORD"))
        out = {DataType::Kind::UInt16, 2, QStringLiteral("WORD")};
    else if (name == QLatin1String("INT"))
        out = {DataType::Kind::Int16, 2, QStringLiteral("INT")};
    else if (name == QLatin1String("DWORD"))
        out = {DataType::Kind::UInt32, 4, QStringLiteral("DWORD")};
    else if (name == QLatin1String("DINT"))
        out = {DataType::Kind::Int32, 4, QStringLiteral("DINT")};
    else if (name == QLatin1String("REAL"))
        out = {DataType::Kind::Real32, 4, QStringLiteral("REAL")};
    else return fail(error, tr("不支持的数据类型：%1").arg(text));
    if (type) *type = out;
    if (error) error->clear();
    return true;
}

bool encodeValue(const QVariant &value, const DataType &type, QByteArray *bytes, QString *error)
{
    const auto invalid = [&]() { return fail(error, tr("值“%1”无效或超出 %2 范围").arg(value.toString(), type.name)); };
    QByteArray out(type.byteCount, '\0');
    if (type.isBool()) {
        bool bit = false;
        if (value.metaType().id() == QMetaType::Bool) bit = value.toBool();
        else if (value.metaType().id() == QMetaType::QString) {
            const QString text = value.toString().trimmed().toLower();
            if (text == QLatin1String("true") || text == QLatin1String("1")) bit = true;
            else if (text != QLatin1String("false") && text != QLatin1String("0")) return invalid();
        } else {
            double number = 0;
            if (!asDouble(value, &number) || (number != 0 && number != 1)) return invalid();
            bit = number == 1;
        }
        out[0] = bit ? 1 : 0;
    } else if (type.kind == DataType::Kind::Real32) {
        double number = 0;
        if (!asDouble(value, &number) || std::abs(number) > double(std::numeric_limits<float>::max())) return invalid();
        const float real = float(number);
        quint32 bits = 0;
        std::memcpy(&bits, &real, sizeof bits);
        qToBigEndian<quint32>(bits, out.data());
    } else {
        // For integer text, do not silently accept decimal fractions/exponents.
        if (value.metaType().id() == QMetaType::QString) {
            static const QRegularExpression integer(QStringLiteral("^[+-]?[0-9]+$"));
            if (!integer.match(value.toString().trimmed()).hasMatch()) return invalid();
        }
        double number = 0;
        if (!asDouble(value, &number) || number != std::trunc(number)) return invalid();
        double minimum = 0;
        double maximum = 0;
        switch (type.kind) {
        case DataType::Kind::UInt8: maximum = 255; break;
        case DataType::Kind::Int8: minimum = -128; maximum = 127; break;
        case DataType::Kind::UInt16: maximum = 65535; break;
        case DataType::Kind::Int16: minimum = -32768; maximum = 32767; break;
        case DataType::Kind::UInt32: maximum = 4294967295.0; break;
        case DataType::Kind::Int32: minimum = -2147483648.0; maximum = 2147483647.0; break;
        default: return invalid();
        }
        if (number < minimum || number > maximum) return invalid();
        const quint32 bits = quint32(qint64(number));
        if (type.byteCount == 1) out[0] = char(bits & 0xFFU);
        else if (type.byteCount == 2) qToBigEndian<quint16>(quint16(bits), out.data());
        else qToBigEndian<quint32>(bits, out.data());
    }
    if (bytes) *bytes = out;
    if (error) error->clear();
    return true;
}

QVariant decodeValue(const char *bytes, int size, const DataType &type, int bit)
{
    if (!bytes || size < type.byteCount || (type.isBool() && (bit < 0 || bit > 7))) return {};
    const quint8 first = quint8(static_cast<unsigned char>(bytes[0]));
    switch (type.kind) {
    case DataType::Kind::Bool: return bool((first >> bit) & 1U);
    case DataType::Kind::UInt8: return uint(first);
    case DataType::Kind::Int8: return int(first <= 127 ? first : int(first) - 256);
    case DataType::Kind::UInt16: return uint(qFromBigEndian<quint16>(bytes));
    case DataType::Kind::Int16: {
        const quint16 bits = qFromBigEndian<quint16>(bytes);
        return int(bits <= 32767 ? bits : int(bits) - 65536);
    }
    case DataType::Kind::UInt32: return QVariant::fromValue(qFromBigEndian<quint32>(bytes));
    case DataType::Kind::Int32: {
        const quint32 bits = qFromBigEndian<quint32>(bytes);
        return QVariant::fromValue(qint32(bits <= 2147483647U ? qint64(bits) : qint64(bits) - 4294967296LL));
    }
    case DataType::Kind::Real32: {
        const quint32 bits = qFromBigEndian<quint32>(bytes);
        float number = 0;
        std::memcpy(&number, &bits, sizeof number);
        return double(number);
    }
    }
    return {};
}
} // namespace fhmi::S7
