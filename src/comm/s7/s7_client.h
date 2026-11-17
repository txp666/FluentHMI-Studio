#pragma once
#include "core/s7.h"
#include <QtCore/QObject>
#include <memory>

namespace fhmi {
// Structured, addressed PLC sessions. Never registered as byte links in CommHub.
class S7Client : public QObject {
    Q_OBJECT
public:
    explicit S7Client(QObject *parent = nullptr);
    ~S7Client() override;
    void configure(const QString &id, const QJsonObject &properties);
    void open(const QString &id);
    void close(const QString &id);
    void remove(const QString &id);
    void clear();
    bool connected(const QString &id) const;
    void setNeeds(const QList<S7::Need> &needs);
    bool write(const S7::Command &command, QString *error = nullptr);
signals:
    void sampleReceived(const fhmi::S7::Sample &sample);
    void statusChanged(const QString &id, bool connected, const QString &status);
    void errorOccurred(const QString &id, const QString &message);
    void logMessage(const QString &message);
    void writeFinished(const QString &id, bool success, const QString &error);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace fhmi
