#pragma once

#include <QtCore/QString>

namespace fhmi::S7 {

enum class MemoryArea { DataBlock, Marker, Input, Output };

struct Address {
    MemoryArea area = MemoryArea::DataBlock;
    int dbNumber = 0; // Only DataBlock uses this field.
    int byteOffset = 0;
    int bit = -1; // 0..7 for a bit address, -1 for a byte/word/dword.
    int byteCount = 1;
    QString canonical;

    QString areaKey() const;
    int firstBit() const { return byteOffset * 8 + (bit >= 0 ? bit : 0); }
    int bitCount() const { return bit >= 0 ? 1 : byteCount * 8; }
};

// Absolute classic S7 addresses, with no symbolic or optimized DB access.
// On failure, the output is left unchanged.
bool parseAddress(const QString &text, Address *address, QString *error = nullptr);

} // namespace fhmi::S7
