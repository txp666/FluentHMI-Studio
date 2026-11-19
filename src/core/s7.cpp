#include "core/s7.h"

#include "core/project.h"
#include "core/widget_meta.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <cmath>

namespace fhmi::S7 {
namespace {
QString tr(const char *source) { return QCoreApplication::translate("fhmi", source); }
bool integer(const QJsonObject &object, const char *key, int fallback, int lo, int hi, int *out)
{
    const auto value = object.value(QLatin1String(key));
    if (value.isUndefined()) { *out = fallback; return true; }
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() != std::floor(value.toDouble())
        || value.toDouble() < lo || value.toDouble() > hi) return false;
    *out = value.toInt(); return true;
}
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }

bool bindsS7(const QJsonObject &widget, const WidgetMeta *meta)
{
    return meta && !meta->bindProperty().isEmpty() &&
           widget.value(QStringLiteral("binding")).toObject().value(QStringLiteral("parseMode")).toString() ==
               QLatin1String("S7");
}

bool sendsS7(const QJsonObject &props, const WidgetMeta *meta)
{
    return meta && meta->prop(QStringLiteral("commandMode")) && widgetSendsCommands(meta->typeName, props) &&
           props.value(QStringLiteral("commandMode")).toString() == QLatin1String("S7");
}
}

QStringList dataTypes() { return {QStringLiteral("BOOL"), QStringLiteral("BYTE"), QStringLiteral("SINT"), QStringLiteral("WORD"), QStringLiteral("INT"), QStringLiteral("DWORD"), QStringLiteral("DINT"), QStringLiteral("REAL")}; }

std::optional<Config> config(const QJsonObject &props, QString *error)
{
    Config out;
    const auto bad = [&]() -> std::optional<Config> { fail(error, tr("S7 连接参数无效，请检查 IPv4 地址、机架、插槽、端口与周期")); return {}; };
    const auto host = props.value(QStringLiteral("host"));
    if (!host.isUndefined() && !host.isString()) return bad();
    out.host = host.toString(out.host).trimmed();
    const auto octets = out.host.split(QLatin1Char('.'));
    if (octets.size() != 4) return bad();
    for (const auto &octet : octets) {
        static const QRegularExpression digits(QStringLiteral("^(0|[1-9][0-9]{0,2})$"));
        if (!digits.match(octet).hasMatch() || octet.toInt() > 255) return bad();
    }
    if (!integer(props,"port",102,1,65535,&out.port) || !integer(props,"rack",0,0,7,&out.rack)
        || !integer(props,"slot",1,0,31,&out.slot) || !integer(props,"connectionType",1,1,3,&out.connectionType)
        || !integer(props,"period",500,1,60000,&out.period) || !integer(props,"timeout",1000,1,60000,&out.timeout)
        || !integer(props,"reconnectInterval",1000,1,60000,&out.reconnectInterval)) return bad();
    for (const char *key : {"autoConnect", "autoReconnect"})
        if (props.contains(QLatin1String(key)) && !props.value(QLatin1String(key)).isBool()) return bad();
    out.autoConnect = props.value(QStringLiteral("autoConnect")).toBool(false);
    out.autoReconnect = props.value(QStringLiteral("autoReconnect")).toBool(true);
    return out;
}

std::optional<Point> point(const QJsonObject &object, QString *error)
{
    Point out;
    if (!object.value(QStringLiteral("address")).isString() || !object.value(QStringLiteral("dataType")).isString()
        || !parseAddress(object.value(QStringLiteral("address")).toString(), &out.address, error)
        || !parseDataType(object.value(QStringLiteral("dataType")).toString(), &out.type, error)) {
        if (error && error->isEmpty()) *error = tr("请填写 S7 地址和数据类型");
        return {};
    }
    if ((out.address.bit >= 0) != out.type.isBool() || out.address.byteCount != out.type.byteCount) {
        fail(error, tr("S7 地址宽度与数据类型不匹配")); return {};
    }
    if (!integer(object,"period",0,0,60000,&out.period)) { fail(error,tr("S7 采集周期必须为 0–60000 毫秒")); return {}; }
    return out;
}
QJsonObject pointJson(const Point &p) { return {{QStringLiteral("address"),p.address.canonical},{QStringLiteral("dataType"),p.type.name},{QStringLiteral("period"),p.period}}; }

bool usesS7(const QJsonArray &widgets)
{
    return !visitWidgets(widgets, [](const QJsonObject &widget) {
        const QString type = widget.value(QStringLiteral("type")).toString();
        const auto *meta = WidgetRegistry::instance().find(type);
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        if (type == QLatin1String("S7ConfigCard") || bindsS7(widget, meta) || sendsS7(props, meta))
            return false;
        if (type == QLatin1String("RealtimeChart")) {
            const int count = chartCurveCount(props);
            for (int i = 0; i < count; ++i) {
                if (props.value(QStringLiteral("_curve_%1_parseMode").arg(i)).toString() == QLatin1String("S7"))
                    return false;
            }
        }
        return true;
    });
}

bool validateWidgets(const QJsonArray &widgets, QString *error, bool allowExternalReferences)
{
    QSet<QString> cards;
    if (!visitWidgets(widgets, [&](const QJsonObject &widget) {
        if (widget.value(QStringLiteral("type")).toString() != QLatin1String("S7ConfigCard")) return true;
        const QString id = widget.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || cards.contains(id)) return fail(error,tr("S7 连接卡片需要唯一的控件 ID"));
        cards.insert(id);
        return config(widget.value(QStringLiteral("properties")).toObject(),error).has_value();
    })) return false;
    const auto checkPoint = [&](const QString &id, const QJsonObject &settings) -> std::optional<Point> {
        if (id.isEmpty() || (!allowExternalReferences && !cards.contains(id))) {
            fail(error, tr("S7 连接引用不存在或不是 S7 卡片：%1").arg(id));
            return std::nullopt;
        }
        QString problem;
        const auto parsed = point(settings, &problem);
        if (!parsed)
            fail(error, problem);
        return parsed;
    };
    return visitWidgets(widgets, [&](const QJsonObject &widget) {
        const QString type = widget.value(QStringLiteral("type")).toString();
        const auto *meta = WidgetRegistry::instance().find(type);
        const auto binding = widget.value(QStringLiteral("binding")).toObject();
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        if (bindsS7(widget, meta) && !checkPoint(binding.value(QStringLiteral("sourceCardId")).toString(),
                                                binding.value(QStringLiteral("s7")).toObject()))
            return false;
        if (sendsS7(props, meta)) {
            const auto command = props.value(QStringLiteral("s7Command")).toObject();
            const auto p = checkPoint(props.value(QStringLiteral("commandTargetId")).toString(), command);
            if (!p)
                return false;
            if (p->address.area == MemoryArea::Input)
                return fail(error, tr("S7 输入区 I 只读，请选择 DB、M 或 Q 写入"));
            const auto checkValue = [&](const char *key) {
                const auto value = command.value(QLatin1String(key));
                QByteArray bytes;
                return value.isUndefined() || encodeValue(value.toVariant(), p->type, &bytes, error);
            };
            if (type == QLatin1String("PushButton") && !checkValue("value"))
                return false;
            if (type == QLatin1String("ToggleSwitch") && (!checkValue("onValue") || !checkValue("offValue")))
                return false;
        }
        if (type == QLatin1String("RealtimeChart")) {
            const int count = chartCurveCount(props);
            for (int i = 0; i < count; ++i) {
                const QString prefix = QStringLiteral("_curve_%1_").arg(i);
                if (props.value(prefix + QStringLiteral("parseMode")).toString() == QLatin1String("S7") &&
                    !checkPoint(props.value(prefix + QStringLiteral("sourceCardId")).toString(),
                                props.value(prefix + QStringLiteral("s7")).toObject()))
                    return false;
            }
        }
        return true;
    });
}
} // namespace fhmi::S7
