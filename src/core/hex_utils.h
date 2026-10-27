#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <optional>

namespace fhmi {

// Parse hex text such as "AA 55 0d0a". Whitespace is ignored and an odd number of
// digits gets a leading zero, matching the Python designer. Returns nullopt when
// any character is not a hex digit.
std::optional<QByteArray> parseHex(const QString &text);

// Like parseHex, but an invalid field yields an empty array (binding frame delimiters
// are optional, and a malformed one simply disables framing).
QByteArray parseHexOrEmpty(const QString &text);

QString toHexString(const QByteArray &bytes, bool spaced = true);

} // namespace fhmi
