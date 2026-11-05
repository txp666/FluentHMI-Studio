#pragma once

#include "comm/can/can_backend.h"
#include "comm/link.h"
#include "core/can_signal.h"
#include "core/command_encoder.h"

#include <QtCore/QHash>
#include <QtCore/QPointer>

#include <memory>
#include <optional>

class QTimer;

namespace fhmi {

// One CAN channel as a link. Received frames that pass the card's filter are shown as text lines (rawReceived),
// decoded with the card's DBC and inline signals into messages (messageReceived) and counted; bytes are never
// parsed, and send(bytes) does nothing.
//
// Sending keeps a transmit table: per ID, the frame last sent. A CAN command changes the signals it names (or the
// whole payload of a raw ID) and sends the merged frame at once; a cyclic one (canCycle, else the DBC's
// GenMsgCycleTime) is then sent again every period while the link is connected, with whatever it holds by then.
class CanLink : public Link
{
    Q_OBJECT

  public:
    // A command resolved against this link's signals: the frame it sends, and how often.
    struct Prepared
    {
        CanFrame frame;
        int periodMs = 0;
    };

    explicit CanLink(QObject *parent = nullptr);
    ~CanLink() override;

    // Loads the DBC (an absolute path, or empty), builds the inline signals and the filter, then opens the
    // adapter. False with the reason in *error, which the card shows, and in errorOccurred(); nothing is opened
    // then. The DBC's warnings follow connectedChanged(true) as warningMessage().
    bool open(const CanSettings &settings, const QString &dbcPath, const QString &inlineSignals, const QString &filter,
              QString *error);
    void close() override;

    QString description() const override;
    bool isConnected() const override;
    bool isCan() const override { return true; }

    // Resolves a command without sending anything, so a command goes to every CAN link or to none.
    std::optional<Prepared> prepare(const CanCommand &command, QString *error) const;
    // Stores the frame in the transmit table, sends it and starts its period.
    bool commit(const Prepared &prepared, QString *error);
    // A frame as typed in a DataSendCard: sent once, outside the transmit table.
    bool sendFrame(const CanFrame &frame, QString *error);

    // What the link decodes with while connected; null before.
    std::shared_ptr<const CanSignalSet> signalSet() const { return m_signals; }
    quint64 receivedCount() const { return m_received; }
    quint64 sentCount() const { return m_sent; }
    CanBusState busState() const { return m_state; }

  signals:
    // Every frame received (that passes the filter) or sent, in batches: the monitor's feed.
    void canFrames(const QList<fhmi::CanFrame> &frames, bool sent);
    void busStateChanged(fhmi::CanBusState state);

  protected:
    qint64 writeBytes(const QByteArray &bytes) override;

  private:
    bool openChannel(const CanSettings &settings, const QString &dbcPath, const QString &inlineSignals,
                     const QString &filter, QString *error);

    struct Entry
    {
        CanFrame frame;
        QTimer *timer = nullptr;
    };

    void onReceived(const QList<CanFrame> &frames);
    void onSent(const QList<CanFrame> &frames);
    void onBusState(CanBusState state);
    void onSendFailed(const QString &message);
    void resend(quint64 key);
    void dropTable();

    CanBackend *m_backend = nullptr; // a child, while open
    std::shared_ptr<const CanSignalSet> m_signals;
    CanFilter m_filter;
    CanSettings m_settings;
    QHash<quint64, Entry> m_table; // by CanId::key()
    quint64 m_received = 0;
    quint64 m_sent = 0;
    CanBusState m_state = CanBusState::Normal;
    QString m_lastSendError;
    qint64 m_lastSendErrorAt = 0;
};

// The bus state as the card and the log name it: 正常, 错误警告, 错误被动, 总线关闭.
QString canBusStateText(CanBusState state);

} // namespace fhmi
