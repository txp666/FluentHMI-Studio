#pragma once
#include "core/can_frame.h"
#include "core/can_signal.h"
#include "core/data_source.h"
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <atomic>
#include <memory>

namespace fhmi {

// One continuous capture, independent of the monitor's retained rows and display filter.
// A worker writes bounded batches and rotates files; stopping flushes the last partial file.
class CanRecorder : public QObject
{
    Q_OBJECT
  public:
    using QObject::QObject;
    ~CanRecorder() override;
    void start(const QString &directory, int rowsPerFile);
    void append(const QList<CanFrame> &frames, bool sent, const DataSource &source, const QString &channel,
                std::shared_ptr<const CanSignalSet> signalSet);
    void stop();
    bool isRecording() const { return m_recording; }
  signals:
    void failed(const QString &reason, quint64 unsavedFrames, bool currentCapture);
    void finished();

  private:
    QPointer<QObject> m_worker;
    std::shared_ptr<std::atomic<quint64>> m_pending;
    bool m_recording = false;
};

QString canCsvTime(qint64 timestamp);
QString canCsvType(const CanFrame &frame);
QByteArray canCsvLine(QStringList fields);

} // namespace fhmi
