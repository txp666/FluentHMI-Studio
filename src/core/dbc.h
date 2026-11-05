#pragma once

#include "core/can_signal.h"

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace fhmi {

// The messages of a DBC file. Only what decoding and sending need is read: BO_, SG_, VAL_, SIG_VALTYPE_ and the
// attributes GenMsgCycleTime, VFrameFormat and GenSigStartValue. Multiplexed signals (m<n>) are skipped with a
// warning; a multiplexor (M) is an ordinary signal.
struct CanDatabase
{
    QList<CanMessageDef> messages; // in file order
    QStringList warnings;          // for the log when the card connects

    const CanMessageDef *find(const CanId &id) const;
    const CanMessageDef *findByName(const QString &name) const;

    // Builds the lookup tables; parseDbc() calls it.
    void index();

  private:
    QHash<quint64, qsizetype> m_byId;
    QHash<QString, qsizetype> m_byName;
};

namespace Dbc {

// Parses a DBC file's bytes: UTF-8 when they are valid UTF-8, else GBK. A malformed BO_ or SG_ line is skipped
// with a warning naming its line; a file without any message is an error.
std::optional<CanDatabase> parse(const QByteArray &bytes, QString *error = nullptr);
std::optional<CanDatabase> load(const QString &path, QString *error = nullptr);

} // namespace Dbc

} // namespace fhmi
