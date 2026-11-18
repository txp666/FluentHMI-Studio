#include "comm/can/can_discovery.h"
#include "comm/can/zlg_api.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QRegularExpression>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <setupapi.h>
#endif
namespace fhmi::CanAdapter {
QStringList zlgModelsFromDescriptions(const QStringList &descriptions)
{
    QStringList found;
    for (const auto &description : descriptions) {
        QString match;
        for (const auto &model : zlg::kModels) {
            const QString name = QString::fromLatin1(model.name);
            const QRegularExpression pattern(
                QStringLiteral("(?<![A-Za-z0-9])%1(?![A-Za-z0-9+_-])").arg(QRegularExpression::escape(name)),
                QRegularExpression::CaseInsensitiveOption);
            if (name.size() > match.size() && pattern.match(description).hasMatch())
                match = name;
        }
        if (!match.isEmpty() && !found.contains(match))
            found.append(match);
    }
    return found;
}
QStringList detectedZlgModels(QString *error)
{
#ifdef Q_OS_WIN
    const HDEVINFO devices = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE) {
        if (error)
            *error = QCoreApplication::translate("fhmi", "无法读取已连接的 USB 设备");
        return {};
    }
    QStringList descriptions;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof device;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devices, i, &device); ++i) {
        for (DWORD property : {DWORD(SPDRP_FRIENDLYNAME), DWORD(SPDRP_DEVICEDESC)}) {
            DWORD bytes = 0;
            SetupDiGetDeviceRegistryPropertyW(devices, &device, property, nullptr, nullptr, 0, &bytes);
            if (!bytes)
                continue;
            QList<wchar_t> text(bytes / sizeof(wchar_t) + 1, wchar_t(0));
            if (SetupDiGetDeviceRegistryPropertyW(devices, &device, property, nullptr,
                                                  reinterpret_cast<PBYTE>(text.data()), bytes, nullptr))
                descriptions.append(QString::fromWCharArray(text.constData()));
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return zlgModelsFromDescriptions(descriptions);
#else
    if (error)
        *error = QCoreApplication::translate("fhmi", "ZLG 自动识别需要 Windows 和已安装的设备驱动");
    return {};
#endif
}
} // namespace fhmi::CanAdapter
