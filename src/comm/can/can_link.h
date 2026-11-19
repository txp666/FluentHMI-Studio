#pragma once

#include "comm/can/can_backend.h"
#include "comm/can/can_initial_state.h"
#include "comm/link.h"
#include "core/can_signal.h"
#include "core/command_encoder.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QQueue>

#include <functional>
#include <memory>
#include <optional>

class QTimer;

namespace fhmi {

// One CAN channel as a link. Received frames that pass the card's filter are shown as text lines (rawReceived),
// decoded with the card's DBC and inline signals into messages (messageReceived) and counted; bytes are never
// parsed, and send(bytes) does nothing.
//
// Signal writes merge into the frame last accepted by the adapter. Commands for one ID wait for that
// result before another merge. Cyclic sends repeat the confirmed image without building a backlog.
class CanLink : public Link
{
    Q_OBJECT

  public:
    // A command resolved against this link's signals: the frame it sends, and how often.
    struct Prepared
    {
        CanFrame frame;
        int periodMs = 0;
        QByteArray signalMask;
        QByteArray initialData;
        quint64 connectionGeneration = 0;
        bool updateImage = true;
    };
    // Re-prepare later sends in the same frame slot. An invalid input skips the tick.
    using PeriodicFrameReader = std::function<std::optional<Prepared>(QString *error)>;

    explicit CanLink(QObject *parent = nullptr);
    ~CanLink() override;

    // Loads the DBC (an absolute path, or empty), builds the inline signals and the filter, then opens the
    // adapter. False with the reason in *error, which the card shows, and in errorOccurred(); nothing is opened
    // then. The DBC's warnings follow connectedChanged(true) as warningMessage().
    bool open(const CanSettings &settings, const QString &dbcPath, const QString &inlineSignals, const QString &filter,
              QString *error);
    // GUI cards use this asynchronous lifecycle; open() remains the synchronous embedding API.
    bool openAsync(const CanSettings &settings, const QString &dbcPath, const QString &inlineSignals,
                   const QString &filter, QString *error);
    bool isBusy() const { return m_opening || m_closing; }
    bool isClosing() const { return m_closing; }
    void close() override;

    QString description() const override;
    bool isConnected() const override;
    bool isCan() const override { return true; }

    // Resolves a command without sending anything, so a command goes to every CAN link or to none.
    std::optional<Prepared> prepare(const CanCommand &command, QString *error) const;
    bool canCommit(const Prepared &prepared, QString *error) const;
    // Queues a command. The transmit image and period change only after adapter confirmation.
    bool commit(const Prepared &prepared, QString *error);
    // A diagnostic raw frame does not change the signal write baseline.
    bool sendFrame(const CanFrame &frame, QString *error);
    std::optional<Prepared> prepareRawFrame(const CanFrame &frame, QString *error) const;
    bool startPeriodic(const Prepared &prepared, quint64 task, const QString &owner, int intervalMs, QString *error,
                       PeriodicFrameReader readFrame = {});
    void stopPeriodic(quint64 task);
    struct LegacyCycle
    {
        quint64 task;
        CanFrame frame;
        int intervalMs;
        quint64 submitted;
        quint64 skipped;
        bool paused;
    };
    QList<LegacyCycle> legacyCycles() const;
    bool stopLegacyCycles(quint64 task = 0);

    // What the link decodes with while connected; null before.
    std::shared_ptr<const CanSignalSet> signalSet() const { return m_signals; }
    quint64 receivedCount() const { return m_received; }
    quint64 sentCount() const { return m_sent; }
    CanBusState busState() const { return m_state; }
    std::optional<double> busLoadPercent();

  signals:
    // Every frame received (that passes the filter) or sent, in batches: the monitor's feed.
    void canFrames(const QList<fhmi::CanFrame> &frames, bool sent);
    void frameDecoded(const fhmi::CanFrame &frame, const fhmi::Message &message);
    void busStateChanged(fhmi::CanBusState state);
    void lifecycleChanged();
    void periodicResult(quint64 task, fhmi::SendStatus status, const QString &error);
    void legacyCyclesChanged();

  protected:
    qint64 writeBytes(const QByteArray &bytes) override;
    virtual std::unique_ptr<CanBackend> createBackend(const QString &adapter, QString *error);

  private:
    bool openChannel(const CanSettings &settings, const QString &dbcPath, const QString &inlineSignals,
                     const QString &filter, QString *error, bool asynchronous = false);
    void opened();

    struct QueuedCommand
    {
        Prepared command;
        quint64 generation;
    };
    struct Submission
    {
        quint64 id = 0;
        std::optional<int> periodMs; // unset for a repeat of the already confirmed image
        quint64 task = 0;
    };
    struct Periodic
    {
        Prepared command;
        quint64 task;
        QString owner;
        int intervalMs;
        PeriodicFrameReader readFrame;
        bool started = false;
    };
    struct Entry
    {
        std::optional<CanFrame> frame;
        QTimer *timer = nullptr;
        int legacyInterval = 0;
        quint64 legacyTask = 0;
        quint64 legacySubmitted = 0;
        quint64 legacySkipped = 0;
        std::optional<Periodic> periodic;
        Submission submission;
        QQueue<QueuedCommand> queue;
    };

    std::optional<CanFrame> composeFrame(const Prepared &prepared, QString *error) const;
    bool checkSendAvailable(QString *error) const;
    bool queueCommand(Prepared prepared, QString *error);
    bool submitFrame(CanFrame frame, std::optional<int> periodMs, QString *error, quint64 task = 0);
    void finishSubmission(const CanFrame &frame, bool accepted, const QString &error = {});
    void updateCycleTimer(quint64 key, Entry &entry);
    void dispatchNext(quint64 key);
    void recordLoad(const QList<CanFrame> &frames);
    void onReceived(const QList<CanFrame> &frames);
    void onSent(const QList<CanFrame> &frames);
    void onBusState(CanBusState state);
    void onFrameSendFailed(const CanFrame &frame, const QString &message);
    void onSendFailed(const QString &message);
    void resend(quint64 key);
    void dropTable();

    bool m_opening = false;
    bool m_closing = false;
    CanBackend *m_backend = nullptr; // a child, while open
    std::shared_ptr<const CanSignalSet> m_signals;
    CanFilter m_filter;
    CanSettings m_settings;
    QHash<quint64, Entry> m_table; // by CanId::key()
    CanInitialState m_initialState;
    qsizetype m_waiting = 0;
    quint64 m_nextSubmission = 0;
    quint64 m_generation = 0;
    QElapsedTimer m_loadClock;
    double m_loadRate = 0, m_loadDataRate = 0, m_busySeconds = 0;
    std::optional<double> m_loadPercent;
    quint64 m_received = 0;
    quint64 m_sent = 0;
    CanBusState m_state = CanBusState::Normal;
    QString m_lastSendError;
    qint64 m_lastSendErrorAt = 0;
};

// The bus state as the card and the log name it: 正常, 错误警告, 错误被动, 总线关闭.
QString canBusStateText(CanBusState state);

} // namespace fhmi
