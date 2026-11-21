#pragma once

#include "core/protocol_parser.h"

#include <functional>

namespace fhmi::FrameTemplates {

inline const QString ReceiveId = QStringLiteral("receiveFrameTemplateId");
inline const QString ReceiveIds = QStringLiteral("receiveFrameTemplateIds");
inline const QString SendId = QStringLiteral("sendFrameTemplateId");
inline const QString SendFieldsEnabled = QStringLiteral("sendUseTemplateFields");
inline const QString SendBindings = QStringLiteral("sendFrameFieldBindings");
inline const QString SendSequence = QStringLiteral("sendFrameSequence");
inline const QString SendValueField = QStringLiteral("sendFrameField");
inline const QString SendDefaults = QStringLiteral("sendDefaults"); // name -> typed text in a HEX template's parserConfig
inline const QString SendRequired = QStringLiteral("sendRequiredFields"); // writable field names in a HEX template

struct Entry
{
    QString name;
    LinkParser::Format format;
    QString sample;

    QJsonObject toJson() const;
};

std::optional<Entry> find(const QJsonObject &library, const QString &id);
QString problem(const QJsonObject &library);
QString defaultValuesProblem(const LinkParser::Format &format);
QStringList requiredFields(const QJsonObject &settings);
QString sendFieldRulesProblem(const LinkParser::Format &format);
std::optional<QByteArray> buildSendFrame(const QByteArray &body, const QJsonObject &settings,
                                        QString *error, HexFrameLayout *layout = nullptr);
void fillUnassignedMatchBytes(QByteArray &body, QByteArrayView written, const HexFrameConfig &config);
QString sendMatchNotice(const QByteArray &frame, const QJsonObject &settings);
QString sendParser(QStringView mode);
std::optional<QJsonObject> sendSettings(const QJsonObject &library, const QJsonObject &properties);
bool isBinarySend(QStringView mode);
bool writableField(const HexFrameConfig &format, const HexField &field);
QList<HexField> writableFields(const HexFrameConfig &format);
QList<HexField> sendFields(const QJsonObject &library, const QJsonObject &properties);
QStringList receiveIds(const QJsonObject &properties);
QStringList sendIds(const QJsonObject &properties);
void mapSendFrames(QJsonObject &properties, const std::function<void(QJsonObject &)> &edit);
QString receiveGroupProblem(const QJsonObject &library, const QJsonValue &selection);
LinkParser::Format linkFormat(const QJsonObject &library, const QStringList &ids, const QString &selection,
                              const QJsonObject &local, const LinkParser::Format &defaults);

} // namespace fhmi::FrameTemplates
