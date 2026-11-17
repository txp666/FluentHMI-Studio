#pragma once

#include "core/can_signal.h"

namespace fhmi {

// Explicit per-channel feedback -> first transmit mapping. Message rules match equal names; signal rules
// can rename fields. Encodings must agree. Unmapped bytes keep the command's own initial values.
class CanInitialState
{
  public:
    static std::optional<CanInitialState> build(const CanSignalSet &signalSet, const QString &mapping, QString *error);
    bool acceptsFilter(const CanFilter &filter, QString *error) const;
    void observe(const CanFrame &frame, qint64 nowMs);
    void clearReceived();
    // No rule: leaves the DBC initial state intact. A configured rule with no fresh complete feedback fails.
    bool initialize(const CanId &target, QByteArray &data, qint64 nowMs, int maxAgeMs, QString *error) const;

  private:
    struct Binding
    {
        CanSignalDef target;
        CanSignalDef source;
        quint64 sourceKey = 0;
    };
    struct Rule
    {
        QString targetName;
        QList<Binding> bindings;
        QByteArray usedBits;
    };
    struct Source
    {
        CanId id;
        QString name;
        int length = 0;
        bool fd = false;
        bool received = false;
        QByteArray data;
        qint64 receivedAt = 0;
    };
    QHash<quint64, Rule> m_rules;
    QHash<quint64, Source> m_sources;
};

} // namespace fhmi
