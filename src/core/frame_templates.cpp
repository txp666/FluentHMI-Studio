#include "core/frame_templates.h"
#include "core/command_encoder.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QSet>

namespace fhmi::FrameTemplates {

QJsonObject Entry::toJson() const
{
    QJsonObject object{{QStringLiteral("name"), name}, {QStringLiteral("parser"), format.type},
                       {QStringLiteral("parserConfig"), format.settings}};
    if (!sample.isEmpty())
        object.insert(QStringLiteral("sample"), sample);
    return object;
}

std::optional<Entry> find(const QJsonObject &library, const QString &id)
{
    const auto value = library.value(id);
    if (!value.isObject())
        return std::nullopt;
    const auto object = value.toObject();
    return Entry{object.value(QStringLiteral("name")).toString(),
                 {object.value(QStringLiteral("parser")).toString(),
                  object.value(QStringLiteral("parserConfig")).toObject()},
                 object.value(QStringLiteral("sample")).toString()};
}

QString problem(const QJsonObject &library)
{
    const auto tr = [](const char *s) { return QCoreApplication::translate("fhmi", s); };
    QSet<QString> names;
    for (auto it = library.constBegin(); it != library.constEnd(); ++it) {
        const auto entry = find(library, it.key());
        if (!entry || it.key().trimmed().isEmpty() || entry->name.trimmed().isEmpty() || names.contains(entry->name))
            return tr("帧格式模板的名称不能为空或重复");
        names.insert(entry->name);
        if (!LinkParser::hasEditableFraming(entry->format.type) ||
            !it.value().toObject().value(QStringLiteral("parserConfig")).isObject())
            return tr("帧格式模板 %1 的解析模式或配置无效").arg(entry->name);
        QString error;
        if (!createParser(entry->format.type, entry->format.settings, &error))
            return tr("帧格式模板 %1 · %2").arg(entry->name, error);
        error = sendFieldRulesProblem(entry->format);
        if (!error.isEmpty())
            return tr("帧格式模板 %1 · %2").arg(entry->name, error);
    }
    return {};
}

QString defaultValuesProblem(const LinkParser::Format &format)
{
    const auto defaults = format.settings.value(SendDefaults);
    if (defaults.isUndefined())
        return {};
    if (format.type != QLatin1String("hex") || !defaults.isObject())
        return QCoreApplication::translate("fhmi", "HEX 模板的发送默认值必须是按字段名设置的对象");
    return defaults.toObject().isEmpty() ? QString() : CommandEncoder::frameFromTemplateDefaults(format.settings).error;
}

QStringList requiredFields(const QJsonObject &settings)
{
    QStringList fields;
    for (const auto &field : settings.value(SendRequired).toArray())
        fields.append(field.toString());
    return fields;
}

QString sendFieldRulesProblem(const LinkParser::Format &format)
{
    const auto tr = [](const char *s) { return QCoreApplication::translate("fhmi", s); };
    const auto required = format.settings.value(SendRequired);
    if (!required.isUndefined()) {
        if (format.type != QLatin1String("hex") || !required.isArray())
            return tr("发送必填项必须是 HEX 模板的字段列表");
        QString error;
        const auto config = hexFrameConfig(format.settings, &error);
        if (!config)
            return error;
        QSet<QString> available;
        for (const auto &field : writableFields(*config))
            available.insert(field.name);
        for (const auto &name : requiredFields(format.settings)) {
            if (!available.remove(name))
                return tr("必填字段 %1 不存在、不可写或重复").arg(name);
        }
    }
    return defaultValuesProblem(format);
}

std::optional<QByteArray> buildSendFrame(const QByteArray &body, const QJsonObject &settings,
                                        QString *error, HexFrameLayout *layout)
{
    const auto config = hexFrameConfig(settings, error);
    if (!config)
        return std::nullopt;
    const auto required = requiredFields(settings);
    for (const auto &field : config->fields) {
        if (!required.contains(field.name))
            continue;
        // Raw payloads supply the data bytes. The assembler inserts the length slot separately.
        const auto end = config->header.size() + field.offset + field.length;
        const auto lengthBytes = config->lengthOffset >= 0 && end > config->lengthOffset ? config->lengthSize : 0;
        if (end - config->header.size() - lengthBytes > body.size()) {
            if (error)
                *error = QCoreApplication::translate("fhmi", "必填字段 %1 缺少发送值").arg(field.name);
            return std::nullopt;
        }
    }
    return buildHexFrame(body, config->header, config->footer, settings, error, layout);
}

void fillUnassignedMatchBytes(QByteArray &body, QByteArrayView written, const HexFrameConfig &config)
{
    const auto headerSize = config.header.size();
    for (qsizetype i = 0; i < config.matchBytes.size(); ++i) {
        const auto offset = config.matchOffset + i;
        const auto index = offset - headerSize;
        const bool computed = config.lengthOffset >= 0 && offset >= config.lengthOffset &&
                              offset < config.lengthOffset + config.lengthSize;
        if (index >= 0 && index < body.size() && !computed && (written.isEmpty() || written.at(index) == 0))
            body[index] = config.matchBytes.at(i);
    }
}

QString sendMatchNotice(const QByteArray &frame, const QJsonObject &settings)
{
    if (settings.value(QStringLiteral("matchBytes")).toString().isEmpty())
        return {};
    const auto config = hexFrameConfig(settings);
    if (!config || matchHexFrame(frame, *config) == HexFrameMatch::Yes)
        return {};
    return QCoreApplication::translate("fhmi", "发送内容与接收模板的命令字不同（从字节 %1 开始，接收匹配 %2）。")
        .arg(config->matchOffset).arg(QString::fromLatin1(config->matchBytes.toHex(' ').toUpper()));
}

QString sendParser(QStringView value)
{
    using CommandEncoder::Mode;
    const auto mode = CommandEncoder::commandMode(value);
    if (mode == Mode::Hex || mode == Mode::Can)
        return QStringLiteral("hex");
    if (mode == Mode::Text || mode == Mode::Json)
        return QStringLiteral("line");
    return {};
}

bool isBinarySend(QStringView mode)
{
    return sendParser(mode) == QLatin1String("hex");
}

bool writableField(const HexFrameConfig &format, const HexField &field)
{
    const auto layout = format.layout();
    const auto first = layout.headerSize + field.offset;
    const auto end = first + field.length;
    const auto trailer = layout.checksumSize + layout.footerSize;
    const auto maximum = format.fixedLength > 0 ? format.fixedLength : format.maxFrameLength;
    if (field.name.isEmpty() || end + trailer > maximum)
        return false;
    return layout.lengthOffset < 0 || (layout.lengthOffset >= layout.headerSize &&
           (end <= layout.lengthOffset || first >= layout.lengthOffset + layout.lengthSize));
}

QList<HexField> writableFields(const HexFrameConfig &format)
{
    QHash<QString, int> names;
    for (const auto &field : format.fields)
        ++names[field.name];
    QList<HexField> fields;
    for (const auto &field : format.fields)
        if (names.value(field.name) == 1 && writableField(format, field))
            fields.append(field);
    return fields;
}

QList<HexField> sendFields(const QJsonObject &library, const QJsonObject &properties)
{
    if (!isBinarySend(properties.value(QStringLiteral("commandMode")).toString()))
        return {};
    const auto settings = sendSettings(library, properties);
    const auto format = settings ? hexFrameConfig(*settings) : std::nullopt;
    return format ? writableFields(*format) : QList<HexField>{};
}

std::optional<QJsonObject> sendSettings(const QJsonObject &library, const QJsonObject &properties)
{
    const auto entry = find(library, properties.value(SendId).toString());
    if (entry && entry->format.type == sendParser(properties.value(QStringLiteral("commandMode")).toString()))
        return entry->format.settings;
    return std::nullopt;
}

QStringList receiveIds(const QJsonObject &properties)
{
    QStringList ids;
    for (const auto &value : properties.value(ReceiveIds).toArray())
        ids.append(value.toString());
    if (ids.isEmpty()) {
        const auto id = properties.value(ReceiveId).toString();
        if (!id.isEmpty())
            ids.append(id);
    }
    return ids;
}

LinkParser::Format linkFormat(const QJsonObject &library, const QStringList &ids, const QString &selection,
                              const QJsonObject &local, const LinkParser::Format &defaults)
{
    if (ids.size() > 1) {
        QJsonArray templates;
        for (const auto &id : ids) {
            auto entry = library.value(id).toObject();
            entry.insert(QStringLiteral("id"), id);
            templates.append(entry);
        }
        return {LinkParser::HexGroup, {{LinkParser::GroupTemplates, templates}}};
    }
    if (!ids.isEmpty())
        if (const auto entry = find(library, ids.first()))
            return entry->format;
    return LinkParser::effectiveFormat(selection, local, defaults);
}

QString receiveGroupProblem(const QJsonObject &library, const QJsonValue &selection)
{
    if (selection.isUndefined() || (selection.isArray() && selection.toArray().isEmpty()))
        return {};
    const auto tr = [](const char *s) { return QCoreApplication::translate("fhmi", s); };
    if (!selection.isArray() || selection.toArray().size() < 2)
        return tr("请选择至少两个 HEX 接收模板");
    QStringList ids;
    for (const auto &value : selection.toArray()) {
        const auto id = value.toString();
        const auto entry = find(library, id);
        if (!entry || entry->format.type != QLatin1String("hex"))
            return tr("接收模板组仅接受已定义的 HEX 模板 · %1").arg(id);
        ids.append(id);
    }
    const auto format = linkFormat(library, ids, {}, {}, {});
    QString error;
    hexFrameVariants(format.settings, &error);
    return error;
}

} // namespace fhmi::FrameTemplates
