#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <type_traits>
#include <utility>

namespace fhmi::CanDiagnostics {

#if defined(FHMI_CAN_DIAGNOSTICS) && FHMI_CAN_DIAGNOSTICS
inline constexpr bool BuildEnabled = true;
#else
inline constexpr bool BuildEnabled = false;
#endif

bool enabled();
// Ordinary builds retain status polling. Diagnostic builds require an explicit value of "1".
// Sample once when starting a worker: changing the environment never changes an active connection.
bool statusQueriesEnabled();
qint64 monotonicUs();
QString logFilePath();
// Enqueues only. A dedicated writer persists records even while a vendor call is blocked.
void record(const char *event, QJsonObject fields = {});
// Waits for records already accepted by the queue, and reports a file open/write failure.
bool flush();

class Call
{
  public:
    Call(const char *name, QJsonObject fields);
    ~Call();
    void finish(const QJsonValue &result);

  private:
    QString m_name;
    qint64 m_id = 0;
    qint64 m_startedUs = 0;
    bool m_finished = false;
};

template <class Function>
auto invoke(const char *name, Function &&function, QJsonObject fields = {}) -> decltype(function())
{
    if constexpr (BuildEnabled) {
        Call call(name, std::move(fields));
        auto result = function();
        if constexpr (std::is_pointer_v<decltype(result)>) {
            call.finish(QStringLiteral("0x%1").arg(quintptr(result), 0, 16));
        } else {
            call.finish(QJsonValue(qint64(result)));
        }
        return result;
    } else {
        Q_UNUSED(name);
        Q_UNUSED(fields);
        return function();
    }
}

} // namespace fhmi::CanDiagnostics
