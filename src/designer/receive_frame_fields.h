#pragma once

#include "core/protocol_parser.h"

#include <functional>

class QWidget;

namespace FluentQt {
class ComboBox;
}

namespace fhmi {

using ReadFrameConfig = std::function<QJsonObject()>;
using WriteFrameConfig = std::function<void(const QJsonObject &, const QString &key)>;
using AddFrameField = std::function<void(const QString &key, const QString &label, QWidget *editor, const QString &help)>;
using ShowFrameField = std::function<void(const QString &key, bool visible)>;

void addReceiveFrameFields(QWidget *owner, const ReadFrameConfig &read, const WriteFrameConfig &write,
                           const AddFrameField &add, bool includeFieldText = false, QStringView onlyMode = {});
void updateReceiveFrameFields(const LinkParser::Format &format, const ShowFrameField &show);
QWidget *createChecksumRuleHint(FluentQt::ComboBox *algorithm, QWidget *parent);

} // namespace fhmi
