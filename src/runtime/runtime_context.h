#pragma once

#include "comm/comm_hub.h"
#include "core/binding.h"
#include "core/command_encoder.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>

#include <vector>

namespace fhmi {

class HmiWidget;

namespace NotifyType {
inline const QString Success = QStringLiteral("成功");
inline const QString Info = QStringLiteral("信息");
inline const QString Warning = QStringLiteral("警告");
inline const QString Error = QStringLiteral("错误");
} // namespace NotifyType

// The live side of a running surface: the hub with its links, the bindings from
// incoming data to widgets, and the path from widgets' commands out to devices.
class RuntimeContext : public QObject
{
    Q_OBJECT

  public:
    explicit RuntimeContext(QObject *parent = nullptr);

    CommHub *hub() const { return m_hub; }

    void registerWidget(const QString &id, HmiWidget *widget);
    void addBinding(BindingEntry binding);
    void clear();

    void setActive(bool active);
    bool isActive() const { return m_active; }

    // Sends a widget's command over every connected link, or explains why not.
    bool send(const CommandResult &result);
    void notify(const QString &title, const QString &content, const QString &type = NotifyType::Info,
                int durationMs = 2000);

  signals:
    void notificationRequested(const QString &title, const QString &content, const QString &type, int durationMs);

  private:
    void onMessage(const Message &message);
    void onRaw(const QByteArray &bytes);
    void push(const BindingEntry &binding, const QVariant &raw);

    CommHub *m_hub = nullptr;
    QHash<QString, QPointer<HmiWidget>> m_widgets;
    std::vector<BindingEntry> m_bindings;
    bool m_active = false;
};

} // namespace fhmi
