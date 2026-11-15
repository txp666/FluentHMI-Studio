#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QVariant>

namespace fhmi::S7 {

struct DataType {
    enum class Kind { Bool, UInt8, Int8, UInt16, Int16, UInt32, Int32, Real32 };
    Kind kind = Kind::Real32;
    int byteCount = 4;
    QString name = QStringLiteral("REAL");

    bool isBool() const { return kind == Kind::Bool; }
    bool isInteger() const { return !isBool() && kind != Kind::Real32; }
};

// The eight Siemens scalar names are accepted, case-insensitively.
bool parseDataType(const QString &text, DataType *type, QString *error = nullptr);
// Strict conversion: finite values, exact integer ranges, BOOL only 0/1 or true/false.
// All non-BOOL multi-byte values use Siemens big-endian wire order. BOOL returns
// one byte containing 0 or 1; the caller applies the bit mask under its area lock.
bool encodeValue(const QVariant &value, const DataType &type, QByteArray *bytes, QString *error = nullptr);
QVariant decodeValue(const char *bytes, int size, const DataType &type, int bit = 0);

} // namespace fhmi::S7
