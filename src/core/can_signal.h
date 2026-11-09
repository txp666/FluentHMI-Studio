#pragma once

#include "core/can_frame.h"
#include "core/message.h"

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

#include <memory>
#include <optional>

namespace fhmi {

// One signal of a CAN message, from a DBC SG_ line or a line of the card's canSignals.
struct CanSignalDef
{
    enum class ValueType
    {
        Integer,
        Float32, // SIG_VALTYPE_ 1: the 32 raw bits are an IEEE float
        Double,  // SIG_VALTYPE_ 2: the 64 raw bits are an IEEE double
    };

    QString name;
    int start = 0;         // Intel: the LSB's bit; Motorola: the MSB's bit, in DBC "sawtooth" numbering
    int length = 1;        // 1-64 bits
    bool motorola = false; // @0
    bool isSigned = false; // two's complement
    ValueType valueType = ValueType::Integer;
    double factor = 1;
    double offset = 0;
    double minimum = 0;
    double maximum = 0;
    QString unit;
    QMap<qint64, QString> valueDescriptions; // VAL_: raw value -> text
    std::optional<double> startValue;        // GenSigStartValue, a raw value

    // Whether every bit lies inside `dataLength` bytes.
    bool fits(int dataLength) const;
    // The data length that holds every bit.
    int bytesNeeded() const;
    // The raw bits, LSB first in the result; the signal must fit.
    quint64 rawBits(const QByteArray &data) const;
    void putRawBits(QByteArray &data, quint64 bits) const;

    // The physical value raw × factor + offset: a qlonglong for an integer signal whose factor and offset are
    // whole numbers and whose result fits, else a double. Invalid when the signal does not fit the data.
    QVariant decode(const QByteArray &data) const;
    // The value description of the raw value in the data, or an empty string.
    QString valueText(const QByteArray &data) const;
    // The raw bits for a value written as text: a decimal physical value, one of the value descriptions, or a
    // 0x… hex number, which is the raw value itself (no factor, offset or physical limits; for a float signal the
    // IEEE bits, for a signed one the two's complement bits). A value outside the signal's bit range, or not a
    // number, is an error naming the signal; nothing wraps.
    std::optional<quint64> encode(const QString &value, QString *error = nullptr) const;
    std::optional<quint64> encode(double physical, QString *error = nullptr) const;
    // The raw bits GenSigStartValue stands for, or 0.
    quint64 initialBits() const;
    // Checks start, length and the value type; the message names it in the error.
    bool validate(QString *error) const;
};

// The DBC start (the MSB's bit) of a Motorola signal whose start bit is given at its LSB instead, as some tools
// number it, in the same sawtooth numbering: from the LSB each step towards the MSB goes up one bit within the
// byte, or from bit 7 to bit 0 of the byte before. nullopt when that walks off the front of the frame (before
// byte 0), or the length is not 1-64.
std::optional<int> motorolaMsbStart(int lsbStart, int length);

// A message of a DBC file, or the messages the card's canSignals describe.
struct CanMessageDef
{
    QString name; // empty for an ID that only has inline signals
    CanId id;
    bool fd = false;
    int length = 8;
    int cycleTime = 0; // GenMsgCycleTime, ms; 0 when not cyclic
    QList<CanSignalDef> signalDefs;
};

struct CanDatabase;

// A line of the card's canSignals: <ID> [SG_] <Name> : <start>|<length>@<0|1><+|-> [(<factor>,<offset>)]
// [[<min>|<max>]] ["<unit>"]. The ID is hexadecimal (Can::parseId). Blank lines and lines starting with # or //
// are skipped; a bad line is an error naming its number.
struct InlineSignal
{
    CanId id;
    CanSignalDef signal;
};
std::optional<QList<InlineSignal>> parseInlineSignals(const QString &text, QString *error = nullptr);

// The body of an SG_ line after its keyword (DBC files and canSignals alike): <Name> [M|m<n>] : <start>|<length>@
// <0|1><+|-> [(<factor>,<offset>)] [[<min>|<max>]] ["<unit>"] [receivers]. `mux` gets the multiplexer marker,
// `rest` what follows the unit.
bool parseSignalBody(const QString &body, CanSignalDef *signal, QString *mux, QString *rest, QString *error);

// A CAN command's signal assignments: Signal=<value> separated by commas, semicolons or newlines.
std::optional<QList<QPair<QString, QString>>> parseAssignments(const QString &text, QString *error = nullptr);

// The signals a CAN card decodes and sends with: its DBC file and its inline signals, built once when it connects.
class CanSignalSet
{
  public:
    // A message a command names, with every signal it can set.
    struct Target
    {
        CanId id;
        QString name;           // the DBC message's, or empty
        bool hasSignals = false; // false: a raw ID, sent as the command's bytes
        bool fd = false;
        int length = 8;
        int cycleTime = 0;
        QList<const CanSignalDef *> signalDefs; // DBC signals in file order, then inline ones
    };

    // Inline signals are checked against each other and against the DBC (a name twice is an error).
    static std::optional<CanSignalSet> build(std::shared_ptr<const CanDatabase> dbc, const QString &inlineText,
                                             QString *error = nullptr);

    bool isEmpty() const;
    // The fields of a received frame (see the spec): <Message>.<Signal> and <Signal>, with .text keys for value
    // descriptions, for DBC signals in file order, then <Signal> for inline ones. Empty when nothing matches.
    Message decode(const CanFrame &frame) const;
    // "Sig=val unit, …" for the monitor.
    QString signalsText(const CanFrame &frame) const;
    // The DBC message's name for the ID, or empty.
    QString messageName(const CanId &id) const;
    // What a command's canId names: a DBC message by name, else an ID per Can::parseId, which has signals when
    // the DBC or the inline signals define some for it.
    std::optional<Target> target(const QString &canId, QString *error = nullptr) const;
    // Every message name and every signal key a binding can use, for the designer's completions.
    QStringList messageNames() const;
    QStringList signalKeys() const;
    QStringList warnings() const;

  private:
    std::shared_ptr<const CanDatabase> m_dbc;
    QHash<quint64, QList<CanSignalDef>> m_inline; // by CanId::key()
    QList<CanId> m_inlineOrder;                   // IDs in the order their first line came
};

} // namespace fhmi
