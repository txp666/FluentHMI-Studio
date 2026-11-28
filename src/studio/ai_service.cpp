#include "studio/ai_service.h"

#include "core/binding.h"
#include "core/frame_templates.h"
#include "core/i18n.h"
#include "core/widget_meta.h"

#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtGui/QImage>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>

#ifndef FHMI_VERSION
#define FHMI_VERSION "0.9.3"
#endif

namespace fhmi {

namespace {

constexpr int ChatTimeoutMs = 90000;
constexpr int TestTimeoutMs = 15000;

const QString ProviderKey = QStringLiteral("ai/provider");
const QString BaseUrlKey = QStringLiteral("ai/baseUrl");
const QString ModelKey = QStringLiteral("ai/model");
const QString LegacyApiKey = QStringLiteral("ai/apiKey");

QString providerKeyPath(const QString &provider, const QString &field)
{
    return QStringLiteral("ai/%1/%2").arg(provider, field);
}

void collectWidgetIds(const QJsonArray &widgets, QSet<QString> *ids)
{
    for (const QJsonValue &val : widgets) {
        const QJsonObject obj = val.toObject();
        const QString id = obj.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            ids->insert(id);
        }
        collectWidgetIds(obj.value(QStringLiteral("children")).toArray(), ids);
    }
}

QString generateUniqueWidgetId(const QString &typeName, const QSet<QString> &used)
{
    QString base = typeName.isEmpty() ? QStringLiteral("widget") : typeName;
    base[0] = base.at(0).toLower();
    for (int n = 1;; ++n) {
        const QString candidate = QStringLiteral("%1%2").arg(base).arg(n);
        if (!used.contains(candidate)) {
            return candidate;
        }
    }
}

QJsonObject mergeJsonObjects(QJsonObject target, const QJsonObject &patch)
{
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.value().isNull()) {
            target.remove(it.key());
        } else {
            target.insert(it.key(), it.value());
        }
    }
    return target;
}

QString normalizeParseMode(const QString &rawMode)
{
    const QString mode = rawMode.trimmed();
    if (mode.isEmpty() || mode == QStringLiteral("键值") || mode == QLatin1String("keyValue") || // i18n: ignore
        mode == QLatin1String("key_value") || mode == QLatin1String("kv")) {
        return ParseMode::KeyValue;
    }
    if (mode == QStringLiteral("HEX字节") || mode == QLatin1String("HEX") || mode == QLatin1String("hex") || // i18n: ignore
        mode == QLatin1String("hexBytes")) {
        return ParseMode::HexBytes;
    }
    if (mode == QStringLiteral("数据点") || mode == QLatin1String("dataPoint") || // i18n: ignore
        mode == QLatin1String("point")) {
        return ParseMode::DataPoint;
    }
    if (mode == QLatin1String("json") || mode == QLatin1String("jsonPath")) {
        return ParseMode::JsonPath;
    }
    if (mode.compare(QLatin1String("modbus"), Qt::CaseInsensitive) == 0) {
        return ParseMode::Modbus;
    }
    if (mode.compare(QLatin1String("s7"), Qt::CaseInsensitive) == 0) {
        return ParseMode::S7;
    }
    if (mode == QLatin1String("can") || mode == QLatin1String("canSignal")) {
        return ParseMode::CanSignal;
    }
    if (mode == QStringLiteral("CAN信号名称") || mode == QLatin1String("canField")) { // i18n: ignore
        return ParseMode::CanField;
    }
    return mode;
}

QString normalizeSingleBindingExpression(const QString &rawExpr)
{
    QString expr = rawExpr.trimmed();
    if (expr == QLatin1String("x") || expr == QLatin1String("value")) {
        return {};
    }
    static const QRegularExpression standaloneX(QStringLiteral("\\bx\\b"));
    expr.replace(standaloneX, QStringLiteral("value"));
    return expr;
}

QJsonObject normalizeBindingObject(QJsonObject binding)
{
    if (binding.isEmpty()) {
        return binding;
    }
    QString mode = normalizeParseMode(binding.value(QStringLiteral("parseMode")).toString());
    if (binding.contains(QStringLiteral("jsonPath")) && !binding.contains(QStringLiteral("parseMode"))) {
        mode = ParseMode::JsonPath;
    } else if (binding.contains(QStringLiteral("byteOffset")) && !binding.contains(QStringLiteral("parseMode"))) {
        mode = ParseMode::HexBytes;
    } else if (binding.contains(QStringLiteral("regIndex")) && !binding.contains(QStringLiteral("parseMode"))) {
        mode = ParseMode::Modbus;
    } else if (binding.contains(QStringLiteral("s7")) && !binding.contains(QStringLiteral("parseMode"))) {
        mode = ParseMode::S7;
    }
    binding.insert(QStringLiteral("parseMode"), mode);

    if (binding.contains(QStringLiteral("expression"))) {
        const QString normExpr = normalizeSingleBindingExpression(binding.value(QStringLiteral("expression")).toString());
        if (normExpr.isEmpty()) {
            binding.remove(QStringLiteral("expression"));
        } else {
            binding.insert(QStringLiteral("expression"), normExpr);
        }
    }
    if (binding.contains(QStringLiteral("canByteOrder"))) {
        const QString bo = binding.value(QStringLiteral("canByteOrder")).toString();
        if (bo == QLatin1String("little_endian") || bo == QStringLiteral("小端(LE)")) { // i18n: ignore
            binding.insert(QStringLiteral("canByteOrder"), CanByteOrder::Intel);
        } else if (bo == QLatin1String("big_endian") || bo == QStringLiteral("大端(BE)")) { // i18n: ignore
            binding.insert(QStringLiteral("canByteOrder"), CanByteOrder::MotorolaMsb);
        }
    }
    return binding;
}

QJsonObject normalizeCurveProperties(const QString &type, QJsonObject props)
{
    if (type != QLatin1String("RealtimeChart") && type != QLatin1String("DigitalTimingChart")) {
        return props;
    }
    for (int i = 0; i < 32; ++i) {
        const QString prefix = QStringLiteral("_curve_%1_").arg(i);
        const QString modeKey = prefix + QStringLiteral("parseMode");
        if (props.contains(modeKey)) {
            props.insert(modeKey, normalizeParseMode(props.value(modeKey).toString()));
        }
        const QString exprKey = prefix + QStringLiteral("expression");
        if (props.contains(exprKey)) {
            const QString normExpr = normalizeSingleBindingExpression(props.value(exprKey).toString());
            if (normExpr.isEmpty()) {
                props.remove(exprKey);
            } else {
                props.insert(exprKey, normExpr);
            }
        }
    }
    return props;
}

QJsonObject normalizePointObject(QJsonObject pt)
{
    const QString name = pt.value(QStringLiteral("name")).toString().trimmed();
    if (pt.contains(QStringLiteral("initialValue")) || !pt.value(QStringLiteral("expression")).toString().trimmed().isEmpty()) {
        return pt;
    }
    QJsonObject binding = pt.value(QStringLiteral("binding")).toObject();
    for (const QString &field : {QStringLiteral("parseMode"), QStringLiteral("sourceCardId"), QStringLiteral("sourceEndpoint"),
                                 QStringLiteral("key"), QStringLiteral("jsonPath"), QStringLiteral("byteOffset"),
                                 QStringLiteral("dataType"), QStringLiteral("byteOrder"), QStringLiteral("regIndex"),
                                 QStringLiteral("mbArea"), QStringLiteral("s7")}) {
        if (pt.contains(field)) {
            if (!binding.contains(field)) {
                binding.insert(field, pt.value(field));
            }
            pt.remove(field);
        }
    }
    binding = normalizeBindingObject(binding);
    if (binding.value(QStringLiteral("parseMode")).toString() == ParseMode::KeyValue &&
        binding.value(QStringLiteral("key")).toString().trimmed().isEmpty() && !name.isEmpty()) {
        binding.insert(QStringLiteral("key"), name);
    }
    pt.insert(QStringLiteral("binding"), binding);
    return pt;
}

QJsonObject normalizeAlarmObject(QJsonObject al, const QSet<QString> &pointNames)
{
    if (al.value(QStringLiteral("id")).toString().trimmed().isEmpty() && al.contains(QStringLiteral("name"))) {
        al.insert(QStringLiteral("id"), al.value(QStringLiteral("name")).toString().trimmed());
    }
    al.remove(QStringLiteral("name"));
    if (!al.contains(QStringLiteral("text")) && al.contains(QStringLiteral("message"))) {
        al.insert(QStringLiteral("text"), al.value(QStringLiteral("message")).toString());
    }
    al.remove(QStringLiteral("message"));

    if (al.value(QStringLiteral("level")).isString()) {
        const QString lvl = al.value(QStringLiteral("level")).toString();
        const int mapped = (lvl.contains(QStringLiteral("紧急")) || lvl.contains(QStringLiteral("高")) || // i18n: ignore
                            lvl.compare(QLatin1String("critical"), Qt::CaseInsensitive) == 0 ||
                            lvl.compare(QLatin1String("high"), Qt::CaseInsensitive) == 0)
                               ? 3
                           : (lvl.contains(QStringLiteral("一般")) || lvl.contains(QStringLiteral("低")) || // i18n: ignore
                              lvl.compare(QLatin1String("low"), Qt::CaseInsensitive) == 0 ||
                              lvl.compare(QLatin1String("info"), Qt::CaseInsensitive) == 0)
                               ? 1
                               : 2;
        al.insert(QStringLiteral("level"), mapped);
    } else {
        al.insert(QStringLiteral("level"), std::clamp(al.value(QStringLiteral("level")).toInt(2), 1, 3));
    }

    QString cond = al.value(QStringLiteral("condition")).toString().trimmed();
    if (cond.isEmpty() && al.contains(QStringLiteral("point"))) {
        const QString pt = al.value(QStringLiteral("point")).toString().trimmed();
        const QString op = al.value(QStringLiteral("operator")).toString(QStringLiteral(">")).trimmed();
        const QJsonValue thresh = al.value(QStringLiteral("threshold"));
        const QString threshStr = thresh.isDouble() ? QString::number(thresh.toDouble(), 'g', 12) : thresh.toString(QStringLiteral("0"));
        cond = QStringLiteral("T[\"%1\"] %2 %3").arg(pt, op, threshStr);
    }
    al.remove(QStringLiteral("point"));
    al.remove(QStringLiteral("operator"));
    al.remove(QStringLiteral("threshold"));

    if (!cond.isEmpty() && !cond.contains(QLatin1String("T["))) {
        for (const QString &ptName : pointNames) {
            if (ptName.isEmpty()) {
                continue;
            }
            const QRegularExpression bareRef(
                QStringLiteral("(?<![\"\\w])%1(?![\"\\w])").arg(QRegularExpression::escape(ptName)));
            cond.replace(bareRef, QStringLiteral("T[\"%1\"]").arg(ptName));
        }
    }
    static const QRegularExpression singleQuoteT(QStringLiteral("T\\['([^']+)'\\]"));
    cond.replace(singleQuoteT, QStringLiteral("T[\"\\1\"]"));
    al.insert(QStringLiteral("condition"), cond);
    return al;
}

bool updateExistingWidgetInTree(QJsonArray &widgets, const QString &id, const QJsonObject &patchWidget,
                                QString *matchedType)
{
    for (qsizetype i = 0; i < widgets.size(); ++i) {
        QJsonObject existing = widgets.at(i).toObject();
        if (existing.value(QStringLiteral("id")).toString() == id) {
            const QString type = patchWidget.value(QStringLiteral("type")).toString(
                existing.value(QStringLiteral("type")).toString());
            existing.insert(QStringLiteral("type"), type);
            if (matchedType) {
                *matchedType = type;
            }
            if (patchWidget.contains(QStringLiteral("geometry")) && patchWidget.value(QStringLiteral("geometry")).isObject()) {
                const QJsonObject mergedGeo = mergeJsonObjects(existing.value(QStringLiteral("geometry")).toObject(),
                                                               patchWidget.value(QStringLiteral("geometry")).toObject());
                existing.insert(QStringLiteral("geometry"), mergedGeo);
            }
            if (patchWidget.contains(QStringLiteral("properties")) && patchWidget.value(QStringLiteral("properties")).isObject()) {
                QJsonObject mergedProps = mergeJsonObjects(existing.value(QStringLiteral("properties")).toObject(),
                                                           patchWidget.value(QStringLiteral("properties")).toObject());
                mergedProps = normalizeCurveProperties(type, mergedProps);
                mergedProps = normalizedWidgetProperties(type, mergedProps);
                existing.insert(QStringLiteral("properties"), mergedProps);
            }
            if (patchWidget.contains(QStringLiteral("binding"))) {
                if (patchWidget.value(QStringLiteral("binding")).isNull()) {
                    existing.remove(QStringLiteral("binding"));
                } else if (patchWidget.value(QStringLiteral("binding")).isObject()) {
                    const QJsonObject mergedBind = normalizeBindingObject(
                        mergeJsonObjects(existing.value(QStringLiteral("binding")).toObject(),
                                         patchWidget.value(QStringLiteral("binding")).toObject()));
                    existing.insert(QStringLiteral("binding"), mergedBind);
                }
            }
            if (patchWidget.contains(QStringLiteral("tabIndex"))) {
                existing.insert(QStringLiteral("tabIndex"), patchWidget.value(QStringLiteral("tabIndex")));
            }
            if (patchWidget.contains(QStringLiteral("children")) && patchWidget.value(QStringLiteral("children")).isArray()) {
                existing.insert(QStringLiteral("children"), patchWidget.value(QStringLiteral("children")).toArray());
            }
            widgets[i] = existing;
            return true;
        }
        QJsonArray children = existing.value(QStringLiteral("children")).toArray();
        if (!children.isEmpty() && updateExistingWidgetInTree(children, id, patchWidget, matchedType)) {
            existing.insert(QStringLiteral("children"), children);
            widgets[i] = existing;
            return true;
        }
    }
    return false;
}

bool removeWidgetFromTree(QJsonArray &widgets, const QString &id)
{
    for (qsizetype i = 0; i < widgets.size(); ++i) {
        QJsonObject existing = widgets.at(i).toObject();
        if (existing.value(QStringLiteral("id")).toString() == id) {
            widgets.removeAt(i);
            return true;
        }
        QJsonArray children = existing.value(QStringLiteral("children")).toArray();
        if (!children.isEmpty() && removeWidgetFromTree(children, id)) {
            existing.insert(QStringLiteral("children"), children);
            widgets[i] = existing;
            return true;
        }
    }
    return false;
}

bool appendChildToContainer(QJsonArray &widgets, const QString &parentId, const QJsonObject &childWidget)
{
    for (qsizetype i = 0; i < widgets.size(); ++i) {
        QJsonObject existing = widgets.at(i).toObject();
        if (existing.value(QStringLiteral("id")).toString() == parentId) {
            QJsonArray children = existing.value(QStringLiteral("children")).toArray();
            children.append(childWidget);
            existing.insert(QStringLiteral("children"), children);
            widgets[i] = existing;
            return true;
        }
        QJsonArray children = existing.value(QStringLiteral("children")).toArray();
        if (!children.isEmpty() && appendChildToContainer(children, parentId, childWidget)) {
            existing.insert(QStringLiteral("children"), children);
            widgets[i] = existing;
            return true;
        }
    }
    return false;
}

QJsonObject normalizeNewWidgetTree(const QJsonObject &raw, QSet<QString> *usedIds, int indexHint, QString *error)
{
    const QString type = raw.value(QStringLiteral("type")).toString();
    const WidgetMeta *meta = WidgetRegistry::instance().find(type);
    if (!meta) {
        if (error) {
            *error = QCoreApplication::translate("fhmi", "未知控件类型: %1").arg(type);
        }
        return {};
    }
    QJsonObject built;
    QString id = raw.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty() || usedIds->contains(id)) {
        id = generateUniqueWidgetId(type, *usedIds);
    }
    usedIds->insert(id);
    built.insert(QStringLiteral("id"), id);
    built.insert(QStringLiteral("type"), type);

    const QJsonObject rawGeo = raw.value(QStringLiteral("geometry")).toObject();
    const int defaultX = 30 + (indexHint % 4) * 220;
    const int defaultY = 30 + (indexHint / 4) * 160;
    QJsonObject geo{
        {QStringLiteral("x"), rawGeo.value(QStringLiteral("x")).toDouble(defaultX)},
        {QStringLiteral("y"), rawGeo.value(QStringLiteral("y")).toDouble(defaultY)},
        {QStringLiteral("w"), std::max(double(meta->minWidth), rawGeo.value(QStringLiteral("w")).toDouble(meta->defaultWidth))},
        {QStringLiteral("h"), std::max(double(meta->minHeight), rawGeo.value(QStringLiteral("h")).toDouble(meta->defaultHeight))},
    };
    built.insert(QStringLiteral("geometry"), geo);

    QJsonObject props = mergeJsonObjects(meta->newWidgetProperties(), raw.value(QStringLiteral("properties")).toObject());
    props = normalizeCurveProperties(type, props);
    props = normalizedWidgetProperties(type, props);
    built.insert(QStringLiteral("properties"), props);

    if (raw.value(QStringLiteral("binding")).isObject()) {
        built.insert(QStringLiteral("binding"), normalizeBindingObject(raw.value(QStringLiteral("binding")).toObject()));
    }
    if (raw.contains(QStringLiteral("tabIndex"))) {
        built.insert(QStringLiteral("tabIndex"), raw.value(QStringLiteral("tabIndex")).toInt(0));
    }
    if (raw.value(QStringLiteral("children")).isArray()) {
        QJsonArray builtChildren;
        const QJsonArray rawChildren = raw.value(QStringLiteral("children")).toArray();
        for (int i = 0; i < rawChildren.size(); ++i) {
            const QJsonObject child = normalizeNewWidgetTree(rawChildren.at(i).toObject(), usedIds, i, error);
            if (child.isEmpty()) {
                return {};
            }
            builtChildren.append(child);
        }
        built.insert(QStringLiteral("children"), builtChildren);
    }
    return built;
}

QByteArray zlibDecompressPdfStream(const QByteArray &compressed)
{
    if (compressed.size() < 2) {
        return {};
    }
    for (quint32 expected : {quint32(compressed.size() * 6), quint32(compressed.size() * 24), quint32(1024 * 1024)}) {
        QByteArray withHeader(4, '\0');
        withHeader[0] = char((expected >> 24) & 0xFF);
        withHeader[1] = char((expected >> 16) & 0xFF);
        withHeader[2] = char((expected >> 8) & 0xFF);
        withHeader[3] = char(expected & 0xFF);
        withHeader.append(compressed);
        const QByteArray out = qUncompress(withHeader);
        if (!out.isEmpty()) {
            return out;
        }
    }
    return {};
}

QString decodePdfHexToken(const QByteArray &hexBytes)
{
    const QByteArray raw = QByteArray::fromHex(hexBytes);
    if (raw.size() >= 2 && quint8(raw[0]) == 0xFE && quint8(raw[1]) == 0xFF) {
        QString utf16;
        for (qsizetype i = 2; i + 1 < raw.size(); i += 2) {
            utf16.append(QChar((quint8(raw[i]) << 8) | quint8(raw[i + 1])));
        }
        return utf16;
    }
    return QString::fromUtf8(raw);
}

QString extractTextFromPdfBytes(const QByteArray &pdfBytes, const QString &filePath)
{
    // 1. Try pdftotext if available on system PATH
    if (!filePath.isEmpty()) {
        QProcess proc;
        proc.start(QStringLiteral("pdftotext"), {QStringLiteral("-layout"), filePath, QStringLiteral("-")});
        if (proc.waitForFinished(4000) && proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
            const QString text = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
            if (!text.isEmpty()) {
                return text.left(60000);
            }
        }
    }

    // 2. Built-in lightweight PDF stream text extractor (FlateDecode + Tj / TJ tokens)
    QStringList chunks;
    qsizetype pos = 0;
    while (pos < pdfBytes.size()) {
        const qsizetype streamStart = pdfBytes.indexOf("stream", pos);
        if (streamStart < 0) {
            break;
        }
        qsizetype dataStart = streamStart + 6;
        if (dataStart < pdfBytes.size() && pdfBytes[dataStart] == '\r') {
            ++dataStart;
        }
        if (dataStart < pdfBytes.size() && pdfBytes[dataStart] == '\n') {
            ++dataStart;
        }
        const qsizetype streamEnd = pdfBytes.indexOf("endstream", dataStart);
        if (streamEnd < 0) {
            break;
        }
        pos = streamEnd + 9;
        QByteArray rawStream = pdfBytes.mid(dataStart, streamEnd - dataStart);
        while (!rawStream.isEmpty() && (rawStream.endsWith('\r') || rawStream.endsWith('\n'))) {
            rawStream.chop(1);
        }
        QByteArray content = zlibDecompressPdfStream(rawStream);
        if (content.isEmpty()) {
            content = rawStream;
        }
        if (content.indexOf("BT") < 0 && content.indexOf("Tj") < 0 && content.indexOf("TJ") < 0) {
            continue;
        }
        const QString streamStr = QString::fromLatin1(content);
        static const QRegularExpression literalToken(QStringLiteral("\\(([^()\\\\]*(?:\\\\.[^()\\\\]*)*)\\)"));
        auto litIt = literalToken.globalMatch(streamStr);
        QString line;
        while (litIt.hasNext()) {
            QString token = litIt.next().captured(1);
            token.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
            token.replace(QStringLiteral("\\r"), QStringLiteral(" "));
            token.replace(QStringLiteral("\\t"), QStringLiteral(" "));
            token.replace(QStringLiteral("\\("), QStringLiteral("("));
            token.replace(QStringLiteral("\\)"), QStringLiteral(")"));
            token.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
            if (!token.trimmed().isEmpty()) {
                line += token;
            }
        }
        static const QRegularExpression hexToken(QStringLiteral("<([0-9A-Fa-f]{4,})>"));
        auto hexIt = hexToken.globalMatch(streamStr);
        while (hexIt.hasNext()) {
            const QString decoded = decodePdfHexToken(hexIt.next().captured(1).toLatin1());
            if (!decoded.trimmed().isEmpty()) {
                line += decoded;
            }
        }
        if (!line.trimmed().isEmpty()) {
            chunks.append(line.trimmed());
        }
    }
    return chunks.join(QLatin1Char('\n')).left(60000);
}

QByteArray encodeImageForAi(const QImage &image, QString *mimeOut)
{
    if (image.isNull()) {
        return {};
    }
    QImage scaled = image;
    constexpr int MaxDimension = 1536;
    if (scaled.width() > MaxDimension || scaled.height() > MaxDimension) {
        scaled = scaled.scaled(MaxDimension, MaxDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (scaled.hasAlphaChannel()) {
        scaled.save(&buffer, "PNG");
        if (mimeOut) {
            *mimeOut = QStringLiteral("image/png");
        }
    } else {
        scaled.save(&buffer, "JPEG", 88);
        if (mimeOut) {
            *mimeOut = QStringLiteral("image/jpeg");
        }
    }
    return bytes;
}

} // namespace

// ---------------------------------------------------------------------------
// AiAttachment
// ---------------------------------------------------------------------------

bool AiAttachment::fromFile(const QString &filePath, AiAttachment *out, QString *error)
{
    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        if (error) {
            *error = QCoreApplication::translate("fhmi", "文件不存在：%1").arg(filePath);
        }
        return false;
    }
    if (info.size() > 20 * 1024 * 1024) {
        if (error) {
            *error = QCoreApplication::translate("fhmi", "文件不能超过 20 MB：%1").arg(info.fileName());
        }
        return false;
    }

    const QString suffix = info.suffix().toLower();
    static const QSet<QString> imageExts{
        QStringLiteral("png"),  QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("webp"), QStringLiteral("bmp"), QStringLiteral("gif"),
    };
    if (imageExts.contains(suffix)) {
        const QImage img(filePath);
        if (img.isNull()) {
            if (error) {
                *error = QCoreApplication::translate("fhmi", "无法读取图片：%1").arg(info.fileName());
            }
            return false;
        }
        AiAttachment att;
        att.kind = Kind::Image;
        att.fileName = info.fileName();
        att.rawBytes = encodeImageForAi(img, &att.mimeType);
        if (out) {
            *out = std::move(att);
        }
        return true;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QCoreApplication::translate("fhmi", "无法打开文件：%1").arg(info.fileName());
        }
        return false;
    }
    const QByteArray raw = file.readAll();

    if (suffix == QLatin1String("pdf")) {
        AiAttachment att;
        att.kind = Kind::Pdf;
        att.fileName = info.fileName();
        att.mimeType = QStringLiteral("application/pdf");
        att.rawBytes = raw;
        att.extractedText = extractTextFromPdfBytes(raw, filePath);
        if (out) {
            *out = std::move(att);
        }
        return true;
    }

    AiAttachment att;
    att.kind = Kind::TextFile;
    att.fileName = info.fileName();
    att.mimeType = QStringLiteral("text/plain");
    att.extractedText = QString::fromUtf8(raw).left(60000);
    if (out) {
        *out = std::move(att);
    }
    return true;
}

AiAttachment AiAttachment::fromClipboardImage(const QImage &image, int index)
{
    AiAttachment att;
    att.kind = Kind::Image;
    att.fileName = QCoreApplication::translate("fhmi", "剪贴板图片_%1.png").arg(index);
    att.rawBytes = encodeImageForAi(image, &att.mimeType);
    return att;
}

// ---------------------------------------------------------------------------
// AiConfig
// ---------------------------------------------------------------------------

QStringList AiConfig::providerIds()
{
    return {
        QStringLiteral("deepseek"),
        QStringLiteral("qwen"),
        QStringLiteral("siliconflow"),
        QStringLiteral("zhipu"),
        QStringLiteral("moonshot"),
        QStringLiteral("doubao"),
        QStringLiteral("openai"),
        QStringLiteral("gemini"),
        QStringLiteral("ollama"),
        QStringLiteral("lmstudio"),
        QStringLiteral("custom"),
    };
}

QString AiConfig::providerDisplayName(const QString &provider)
{
    if (provider == QLatin1String("deepseek")) {
        return QStringLiteral("DeepSeek");
    }
    if (provider == QLatin1String("qwen")) {
        return QCoreApplication::translate("fhmi", "通义千问 (DashScope)");
    }
    if (provider == QLatin1String("siliconflow")) {
        return QCoreApplication::translate("fhmi", "硅基流动 (SiliconFlow)");
    }
    if (provider == QLatin1String("zhipu")) {
        return QCoreApplication::translate("fhmi", "智谱清言 (GLM)");
    }
    if (provider == QLatin1String("moonshot")) {
        return QCoreApplication::translate("fhmi", "月之暗面 (Kimi)");
    }
    if (provider == QLatin1String("doubao")) {
        return QCoreApplication::translate("fhmi", "火山方舟 (豆包)");
    }
    if (provider == QLatin1String("openai")) {
        return QStringLiteral("OpenAI");
    }
    if (provider == QLatin1String("gemini")) {
        return QStringLiteral("Google Gemini");
    }
    if (provider == QLatin1String("ollama")) {
        return QCoreApplication::translate("fhmi", "Ollama (本地离线)");
    }
    if (provider == QLatin1String("lmstudio")) {
        return QCoreApplication::translate("fhmi", "LM Studio (本地离线)");
    }
    return QCoreApplication::translate("fhmi", "自定义 OpenAI 兼容");
}

QString AiConfig::presetBaseUrl(const QString &provider)
{
    if (provider == QLatin1String("deepseek")) {
        return QStringLiteral("https://api.deepseek.com/v1");
    }
    if (provider == QLatin1String("qwen")) {
        return QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1");
    }
    if (provider == QLatin1String("siliconflow")) {
        return QStringLiteral("https://api.siliconflow.cn/v1");
    }
    if (provider == QLatin1String("zhipu")) {
        return QStringLiteral("https://open.bigmodel.cn/api/paas/v4");
    }
    if (provider == QLatin1String("moonshot")) {
        return QStringLiteral("https://api.moonshot.cn/v1");
    }
    if (provider == QLatin1String("doubao")) {
        return QStringLiteral("https://ark.cn-beijing.volces.com/api/v3");
    }
    if (provider == QLatin1String("openai")) {
        return QStringLiteral("https://api.openai.com/v1");
    }
    if (provider == QLatin1String("gemini")) {
        return QStringLiteral("https://generativelanguage.googleapis.com/v1beta/openai");
    }
    if (provider == QLatin1String("ollama")) {
        return QStringLiteral("http://127.0.0.1:11434/v1");
    }
    if (provider == QLatin1String("lmstudio")) {
        return QStringLiteral("http://127.0.0.1:1234/v1");
    }
    return QStringLiteral("https://api.deepseek.com/v1");
}

QStringList AiConfig::presetModels(const QString &provider)
{
    if (provider == QLatin1String("deepseek")) {
        return {
            QStringLiteral("deepseek-v4-pro"),
            QStringLiteral("deepseek-v4-flash"),
            QStringLiteral("deepseek-flash"),
            QStringLiteral("deepseek-chat"),
            QStringLiteral("deepseek-reasoner"),
        };
    }
    if (provider == QLatin1String("qwen")) {
        return {
            QStringLiteral("qwen4-max"),
            QStringLiteral("qwen3.8-max"),
            QStringLiteral("qwen3.8-flash"),
            QStringLiteral("qwen3.7-plus"),
            QStringLiteral("qwen3-coder-plus"),
            QStringLiteral("qwen-plus"),
            QStringLiteral("qwen-max"),
        };
    }
    if (provider == QLatin1String("siliconflow")) {
        return {
            QStringLiteral("deepseek-ai/DeepSeek-V4-Pro"),
            QStringLiteral("deepseek-ai/DeepSeek-V4-Flash"),
            QStringLiteral("Qwen/Qwen4-Max"),
            QStringLiteral("Qwen/Qwen3.8-Max"),
            QStringLiteral("zai-org/GLM-5.4"),
            QStringLiteral("zai-org/GLM-5.3-Flash"),
        };
    }
    if (provider == QLatin1String("zhipu")) {
        return {
            QStringLiteral("glm-5.4"),
            QStringLiteral("glm-5.3"),
            QStringLiteral("glm-5.3-flash"),
            QStringLiteral("glm-5-plus"),
            QStringLiteral("glm-4.5"),
        };
    }
    if (provider == QLatin1String("moonshot")) {
        return {
            QStringLiteral("kimi-k2.5"),
            QStringLiteral("kimi-k2-thinking"),
            QStringLiteral("kimi-k2-0711-preview"),
            QStringLiteral("kimi-latest"),
        };
    }
    if (provider == QLatin1String("doubao")) {
        return {
            QStringLiteral("doubao-2.0-pro"),
            QStringLiteral("doubao-2.0-flash"),
            QStringLiteral("doubao-seed-1.8"),
            QStringLiteral("deepseek-v4-pro"),
        };
    }
    if (provider == QLatin1String("openai")) {
        return {
            QStringLiteral("gpt-6.2-astra"),
            QStringLiteral("gpt-6.2-sol"),
            QStringLiteral("gpt-6.2"),
            QStringLiteral("gpt-6.1-terra"),
            QStringLiteral("gpt-6-luna"),
            QStringLiteral("gpt-6-cyber"),
            QStringLiteral("gpt-5.2"),
        };
    }
    if (provider == QLatin1String("gemini")) {
        return {
            QStringLiteral("gemini-4-argon"),
            QStringLiteral("gemini-4-carbon"),
            QStringLiteral("gemini-4-neon"),
            QStringLiteral("gemini-4-hydrogen"),
            QStringLiteral("gemini-3.8-flash"),
            QStringLiteral("gemini-3.5-pro"),
        };
    }
    if (provider == QLatin1String("ollama")) {
        return {
            QStringLiteral("qwen4:14b"),
            QStringLiteral("qwen4:32b"),
            QStringLiteral("qwen3.8-coder:14b"),
            QStringLiteral("qwen3.8-coder:32b"),
            QStringLiteral("deepseek-v4:14b"),
            QStringLiteral("deepseek-v4:32b"),
            QStringLiteral("glm-5.3-flash:9b"),
            QStringLiteral("llama4:8b"),
        };
    }
    if (provider == QLatin1String("lmstudio")) {
        return {
            QStringLiteral("qwen4-14b-instruct"),
            QStringLiteral("qwen3.8-coder-14b-instruct"),
            QStringLiteral("deepseek-v4-flash"),
            QStringLiteral("glm-5.3-flash"),
        };
    }
    return {
        QStringLiteral("gpt-6.2-astra"),
        QStringLiteral("gpt-6.2-sol"),
        QStringLiteral("gemini-4-argon"),
        QStringLiteral("gemini-4-carbon"),
        QStringLiteral("claude-opus-5"),
        QStringLiteral("claude-sonnet-5"),
        QStringLiteral("deepseek-v4-pro"),
        QStringLiteral("qwen4-max"),
        QStringLiteral("glm-5.4"),
    };
}

QString AiConfig::presetModel(const QString &provider)
{
    const QStringList models = presetModels(provider);
    return models.isEmpty() ? QStringLiteral("deepseek-chat") : models.first();
}

bool AiConfig::requiresApiKey(const QString &provider)
{
    return provider != QLatin1String("ollama") && provider != QLatin1String("lmstudio");
}

QUrl AiConfig::chatCompletionsUrl(const QString &baseUrl)
{
    QString trimmed = baseUrl.trimmed();
    while (trimmed.endsWith(QLatin1Char('/'))) {
        trimmed.chop(1);
    }
    if (trimmed.isEmpty()) {
        trimmed = presetBaseUrl(QStringLiteral("deepseek"));
    }
    if (trimmed.endsWith(QLatin1String("/chat/completions"), Qt::CaseInsensitive)) {
        return QUrl(trimmed);
    }
    if (trimmed.endsWith(QLatin1String("/v1"), Qt::CaseInsensitive) ||
        trimmed.endsWith(QLatin1String("/v3"), Qt::CaseInsensitive) ||
        trimmed.endsWith(QLatin1String("/v4"), Qt::CaseInsensitive) ||
        trimmed.endsWith(QLatin1String("/openai"), Qt::CaseInsensitive)) {
        return QUrl(trimmed + QStringLiteral("/chat/completions"));
    }
    return QUrl(trimmed + QStringLiteral("/v1/chat/completions"));
}

QUrl AiConfig::modelsUrl(const QString &baseUrl)
{
    QString trimmed = baseUrl.trimmed();
    while (trimmed.endsWith(QLatin1Char('/'))) {
        trimmed.chop(1);
    }
    if (trimmed.isEmpty()) {
        trimmed = presetBaseUrl(QStringLiteral("deepseek"));
    }
    if (trimmed.endsWith(QLatin1String("/chat/completions"), Qt::CaseInsensitive)) {
        trimmed.chop(int(sizeof("/chat/completions") - 1));
        return QUrl(trimmed + QStringLiteral("/models"));
    }
    if (trimmed.endsWith(QLatin1String("/models"), Qt::CaseInsensitive)) {
        return QUrl(trimmed);
    }
    if (trimmed.endsWith(QLatin1String("/v1"), Qt::CaseInsensitive) ||
        trimmed.endsWith(QLatin1String("/v3"), Qt::CaseInsensitive) ||
        trimmed.endsWith(QLatin1String("/v4"), Qt::CaseInsensitive) ||
        trimmed.endsWith(QLatin1String("/openai"), Qt::CaseInsensitive)) {
        return QUrl(trimmed + QStringLiteral("/models"));
    }
    return QUrl(trimmed + QStringLiteral("/v1/models"));
}

QString AiConfig::loadProviderApiKey(const QString &provider)
{
    QSettings settings;
    const QString key = settings.value(providerKeyPath(provider, QStringLiteral("apiKey"))).toString();
    if (!key.isEmpty()) {
        return key;
    }
    return settings.value(LegacyApiKey).toString();
}

void AiConfig::saveProviderApiKey(const QString &provider, const QString &apiKey)
{
    QSettings settings;
    settings.setValue(providerKeyPath(provider, QStringLiteral("apiKey")), apiKey);
}

AiConfig AiConfig::load()
{
    QSettings settings;
    AiConfig config;
    config.provider = settings.value(ProviderKey, QStringLiteral("deepseek")).toString();
    if (!providerIds().contains(config.provider)) {
        config.provider = QStringLiteral("deepseek");
    }
    config.baseUrl = settings
                         .value(providerKeyPath(config.provider, QStringLiteral("baseUrl")),
                                settings.value(BaseUrlKey, presetBaseUrl(config.provider)))
                         .toString()
                         .trimmed();
    if (config.baseUrl.isEmpty()) {
        config.baseUrl = presetBaseUrl(config.provider);
    }
    config.model = settings
                       .value(providerKeyPath(config.provider, QStringLiteral("model")),
                              settings.value(ModelKey, presetModel(config.provider)))
                       .toString()
                       .trimmed();
    if (config.model.isEmpty()) {
        config.model = presetModel(config.provider);
    }
    config.apiKey = loadProviderApiKey(config.provider);
    return config;
}

void AiConfig::save(const AiConfig &config)
{
    QSettings settings;
    settings.setValue(ProviderKey, config.provider);
    settings.setValue(BaseUrlKey, config.baseUrl.trimmed());
    settings.setValue(ModelKey, config.model.trimmed());
    settings.setValue(providerKeyPath(config.provider, QStringLiteral("baseUrl")), config.baseUrl.trimmed());
    settings.setValue(providerKeyPath(config.provider, QStringLiteral("model")), config.model.trimmed());
    saveProviderApiKey(config.provider, config.apiKey);
}

// ---------------------------------------------------------------------------
// AiService
// ---------------------------------------------------------------------------

AiService::AiService(QObject *parent) : QObject(parent), m_network(new QNetworkAccessManager(this)) {}

AiService::~AiService()
{
    abort();
}

void AiService::abort()
{
    if (QNetworkReply *reply = m_reply.data()) {
        m_reply.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_streamBuffer.clear();
    m_rawResponse.clear();
    m_accumulatedText.clear();
    m_sawSseData = false;
}

QString AiService::formatReplyError(QNetworkReply *reply, const QByteArray &body) const
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString apiDetail;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const QJsonObject root = doc.object();
        if (root.value(QStringLiteral("error")).isObject()) {
            apiDetail = root.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
        } else if (root.value(QStringLiteral("error")).isString()) {
            apiDetail = root.value(QStringLiteral("error")).toString();
        } else if (root.value(QStringLiteral("message")).isString()) {
            apiDetail = root.value(QStringLiteral("message")).toString();
        }
    }
    if (status == 401 || status == 403) {
        return apiDetail.isEmpty() ? tr("API 密钥无效或无权限 (HTTP %1)").arg(status)
                                   : tr("认证失败 (HTTP %1): %2").arg(status).arg(apiDetail);
    }
    if (status == 404) {
        return apiDetail.isEmpty() ? tr("接口地址或模型不存在 (HTTP 404)")
                                   : tr("接口或模型不存在 (HTTP 404): %1").arg(apiDetail);
    }
    if (status == 429) {
        return apiDetail.isEmpty() ? tr("请求过于频繁或额度不足 (HTTP 429)")
                                   : tr("请求受限 (HTTP 429): %1").arg(apiDetail);
    }
    if (status >= 400) {
        return apiDetail.isEmpty() ? tr("服务器返回 HTTP %1").arg(status)
                                   : tr("HTTP %1: %2").arg(status).arg(apiDetail);
    }
    if (reply->error() == QNetworkReply::OperationCanceledError || reply->error() == QNetworkReply::TimeoutError) {
        return tr("AI 请求超时");
    }
    return tr("网络错误: %1").arg(reply->errorString());
}

void AiService::sendChat(const AiConfig &config, const QList<AiChatMessage> &messages)
{
    abort();
    const QUrl url = AiConfig::chatCompletionsUrl(config.baseUrl);
    if (!url.isValid() || url.scheme().isEmpty()) {
        emit failed(tr("AI 接口地址无效"));
        return;
    }
    if (AiConfig::requiresApiKey(config.provider) && config.apiKey.trimmed().isEmpty()) {
        emit failed(tr("请先在“设置 → AI 助手”中配置 API 密钥"));
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "text/event-stream, application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("FluentHMI-Studio/" FHMI_VERSION));
    if (!config.apiKey.trimmed().isEmpty()) {
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(config.apiKey.trimmed()).toUtf8());
    }
    request.setTransferTimeout(ChatTimeoutMs);

    QJsonArray jsonMessages;
    const bool supportsNativePdfBlock = config.provider == QLatin1String("openai") ||
                                        config.provider == QLatin1String("gemini") ||
                                        config.provider == QLatin1String("custom");
    for (const AiChatMessage &msg : messages) {
        if (msg.attachments.isEmpty()) {
            jsonMessages.append(QJsonObject{
                {QStringLiteral("role"), msg.role},
                {QStringLiteral("content"), msg.content},
            });
            continue;
        }
        QJsonArray contentParts;
        QString combinedText = msg.content;
        for (const AiAttachment &att : msg.attachments) {
            if (att.kind == AiAttachment::Kind::TextFile && !att.extractedText.isEmpty()) {
                combinedText += QStringLiteral("\n\n[附件 %1 内容]:\n%2").arg(att.fileName, att.extractedText); // i18n: ignore
            } else if (att.kind == AiAttachment::Kind::Pdf) {
                if (!att.extractedText.isEmpty()) {
                    combinedText += QStringLiteral("\n\n[PDF 附件 %1 提取文本]:\n%2").arg(att.fileName, att.extractedText); // i18n: ignore
                }
                if (supportsNativePdfBlock && !att.rawBytes.isEmpty()) {
                    const QString dataUrl = QStringLiteral("data:application/pdf;base64,%1")
                                                .arg(QString::fromLatin1(att.rawBytes.toBase64()));
                    contentParts.append(QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("file")},
                        {QStringLiteral("file"),
                         QJsonObject{
                             {QStringLiteral("filename"), att.fileName},
                             {QStringLiteral("file_data"), dataUrl},
                         }},
                    });
                }
            }
        }
        if (!combinedText.isEmpty()) {
            contentParts.prepend(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), combinedText},
            });
        }
        for (const AiAttachment &att : msg.attachments) {
            if (att.kind == AiAttachment::Kind::Image && !att.rawBytes.isEmpty()) {
                const QString dataUrl = QStringLiteral("data:%1;base64,%2")
                                            .arg(att.mimeType, QString::fromLatin1(att.rawBytes.toBase64()));
                contentParts.append(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("image_url")},
                    {QStringLiteral("image_url"), QJsonObject{{QStringLiteral("url"), dataUrl}}},
                });
            }
        }
        jsonMessages.append(QJsonObject{
            {QStringLiteral("role"), msg.role},
            {QStringLiteral("content"), contentParts},
        });
    }
    const QJsonObject payload{
        {QStringLiteral("model"), config.model.trimmed().isEmpty() ? AiConfig::presetModel(config.provider) : config.model.trimmed()},
        {QStringLiteral("messages"), jsonMessages},
        {QStringLiteral("stream"), true},
    };

    QNetworkReply *reply = m_network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_reply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty()) {
            return;
        }
        m_rawResponse.append(chunk);
        m_streamBuffer.append(chunk);
        processStreamBuffer(false);
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_reply.clear();
        const QByteArray rest = reply->readAll();
        if (!rest.isEmpty()) {
            m_rawResponse.append(rest);
            m_streamBuffer.append(rest);
        }
        processStreamBuffer(true);
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit failed(formatReplyError(reply, m_rawResponse));
            return;
        }

        if (!m_sawSseData && m_accumulatedText.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(m_rawResponse);
            if (doc.isObject()) {
                const QJsonArray choices = doc.object().value(QStringLiteral("choices")).toArray();
                if (!choices.isEmpty()) {
                    const QJsonObject choice = choices.first().toObject();
                    QString content = choice.value(QStringLiteral("message"))
                                          .toObject()
                                          .value(QStringLiteral("content"))
                                          .toString();
                    if (content.isEmpty()) {
                        content = choice.value(QStringLiteral("text")).toString();
                    }
                    if (!content.isEmpty()) {
                        m_accumulatedText = content;
                        emit chunkReceived(content);
                    }
                }
            }
        }
        emit finished(m_accumulatedText);
    });
}

void AiService::processStreamBuffer(bool finalChunk)
{
    while (true) {
        const qsizetype newlinePos = m_streamBuffer.indexOf('\n');
        if (newlinePos < 0 && !finalChunk) {
            break;
        }
        QByteArray line;
        if (newlinePos >= 0) {
            line = m_streamBuffer.left(newlinePos).trimmed();
            m_streamBuffer.remove(0, newlinePos + 1);
        } else {
            line = m_streamBuffer.trimmed();
            m_streamBuffer.clear();
        }
        if (line.isEmpty() || !line.startsWith("data:")) {
            if (newlinePos < 0) {
                break;
            }
            continue;
        }
        const QByteArray data = line.mid(5).trimmed();
        if (data == "[DONE]") {
            m_sawSseData = true;
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        m_sawSseData = true;
        const QJsonArray choices = doc.object().value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty()) {
            continue;
        }
        const QJsonObject choice = choices.first().toObject();
        QString delta = choice.value(QStringLiteral("delta")).toObject().value(QStringLiteral("content")).toString();
        if (delta.isEmpty() && choice.value(QStringLiteral("message")).isObject()) {
            delta = choice.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString();
        }
        if (!delta.isEmpty()) {
            m_accumulatedText.append(delta);
            emit chunkReceived(delta);
        }
        if (newlinePos < 0) {
            break;
        }
    }
}

void AiService::testConnection(const AiConfig &config)
{
    abort();
    const QUrl url = AiConfig::chatCompletionsUrl(config.baseUrl);
    if (!url.isValid() || url.scheme().isEmpty()) {
        emit testFinished(false, tr("AI 接口地址无效"));
        return;
    }
    if (AiConfig::requiresApiKey(config.provider) && config.apiKey.trimmed().isEmpty()) {
        emit testFinished(false, tr("请先填写 API 密钥"));
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("FluentHMI-Studio/" FHMI_VERSION));
    if (!config.apiKey.trimmed().isEmpty()) {
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(config.apiKey.trimmed()).toUtf8());
    }
    request.setTransferTimeout(TestTimeoutMs);

    const QString model = config.model.trimmed().isEmpty() ? AiConfig::presetModel(config.provider) : config.model.trimmed();
    const QJsonObject payload{
        {QStringLiteral("model"), model},
        {QStringLiteral("messages"), QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                                            {QStringLiteral("content"), QStringLiteral("Reply with OK")}}}},
        {QStringLiteral("stream"), false},
    };

    QNetworkReply *reply = m_network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, model]() {
        m_reply.clear();
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit testFinished(false, formatReplyError(reply, body));
            return;
        }
        emit testFinished(true, tr("连接成功（模型：%1）").arg(model));
    });
}

void AiService::fetchModels(const AiConfig &config)
{
    abort();
    const QUrl url = AiConfig::modelsUrl(config.baseUrl);
    if (!url.isValid() || url.scheme().isEmpty()) {
        emit modelsFetched(false, {}, tr("AI 接口地址无效"));
        return;
    }
    if (AiConfig::requiresApiKey(config.provider) && config.apiKey.trimmed().isEmpty()) {
        emit modelsFetched(false, {}, tr("请先填写 API 密钥"));
        return;
    }

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("FluentHMI-Studio/" FHMI_VERSION));
    if (!config.apiKey.trimmed().isEmpty()) {
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(config.apiKey.trimmed()).toUtf8());
    }
    request.setTransferTimeout(TestTimeoutMs);

    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_reply.clear();
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit modelsFetched(false, {}, formatReplyError(reply, body));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        QStringList models;
        if (doc.isObject()) {
            const QJsonArray data = doc.object().value(QStringLiteral("data")).toArray();
            for (const QJsonValue &item : data) {
                QString id = item.toObject().value(QStringLiteral("id")).toString().trimmed();
                if (id.startsWith(QLatin1String("models/"))) {
                    id.remove(0, 7);
                }
                if (!id.isEmpty() && !models.contains(id)) {
                    models.append(id);
                }
            }
            const QJsonArray ollamaModels = doc.object().value(QStringLiteral("models")).toArray();
            for (const QJsonValue &item : ollamaModels) {
                QString id = item.toObject().value(QStringLiteral("name")).toString().trimmed();
                if (id.isEmpty()) {
                    id = item.toObject().value(QStringLiteral("model")).toString().trimmed();
                }
                if (!id.isEmpty() && !models.contains(id)) {
                    models.append(id);
                }
            }
        }
        if (models.isEmpty()) {
            emit modelsFetched(false, {}, tr("服务端未返回可用模型列表"));
            return;
        }
        emit modelsFetched(true, models, tr("已获取 %1 个模型").arg(models.size()));
    });
}

// ---------------------------------------------------------------------------
// Prompt builder & Patch parser
// ---------------------------------------------------------------------------

QString AiService::buildSystemPrompt(const Project &project, const QStringList &selectedWidgetIds)
{
    QStringList widgetSummaries;
    for (const WidgetMeta &meta : WidgetRegistry::instance().all()) {
        QStringList propSpecs;
        for (const PropDef &prop : meta.properties) {
            if (prop.name.startsWith(QLatin1String("_curve_")) && !prop.name.startsWith(QLatin1String("_curve_0_"))) {
                continue;
            }
            QString spec = prop.name;
            if (!prop.options.isEmpty()) {
                spec += QStringLiteral("=[%1]").arg(prop.options.join(QLatin1Char('|')));
            }
            propSpecs.append(spec);
        }
        widgetSummaries.append(QStringLiteral("- %1 (%2, default %3x%4, bind:%5): %6")
                                   .arg(meta.typeName, meta.displayName)
                                   .arg(meta.defaultWidth)
                                   .arg(meta.defaultHeight)
                                   .arg(meta.bindProperty().isEmpty() ? QStringLiteral("none") : meta.bindProperty(),
                                        propSpecs.join(QStringLiteral(", "))));
    }

    const QString projectJson = QString::fromUtf8(QJsonDocument(project.toJson()).toJson(QJsonDocument::Compact));
    const QString selectedText = selectedWidgetIds.isEmpty() ? QStringLiteral("none") : selectedWidgetIds.join(QStringLiteral(", "));

    // i18n: ignore
    return QStringLiteral(
               "你是 FluentHMI Studio 内置的 AI 智慧助手，精通工业上位机 HMI 界面设计、通信协议解析（串口/TCP/UDP/HEX帧/JSON/键值、Modbus RTU/TCP、西门子 S7、CAN/CAN FD 与 DBC、MQTT）、数据绑定与故障排查。\n" // i18n: ignore
               "\n" // i18n: ignore
               "## 回复准则\n" // i18n: ignore
               "1. 回答简洁、准确、专业。对用户关于当前控件、量程、协议、绑定的疑问，直接结合下方【当前工程 JSON】与【当前选中控件】作答。\n" // i18n: ignore
               "2. 当用户要求创建、修改、删除控件，配置数据绑定，生成接收帧模板（frameTemplates），或新增工程数据点（points）与报警规则（alarms）时，除简要说明外，必须在回复末尾附上且仅附上一个 ```fhmi-patch 代码块。\n" // i18n: ignore
               "3. ```fhmi-patch 代码块内必须是合法 JSON 对象，支持以下增量字段（无需重复未修改的控件）：\n" // i18n: ignore
               "```fhmi-patch\n" // i18n: ignore
               "{\n" // i18n: ignore
               "  \"title\": \"变更说明标题\",\n" // i18n: ignore
               "  \"canvas\": {\"width\": 1200, \"height\": 800, \"gridSize\": 10},\n" // i18n: ignore
               "  \"upsertWidgets\": [\n" // i18n: ignore
               "    {\n" // i18n: ignore
               "      \"id\": \"gaugeMeter1\",\n" // i18n: ignore
               "      \"type\": \"GaugeMeter\",\n" // i18n: ignore
               "      \"geometry\": {\"x\": 40, \"y\": 40, \"w\": 180, \"h\": 180},\n" // i18n: ignore
               "      \"properties\": {\"title\": \"电流\", \"unit\": \"A\", \"minimum\": 0, \"maximum\": 2000, \"value\": 60},\n" // i18n: ignore
               "      \"binding\": {\"parseMode\": \"键值匹配\", \"sourceCardId\": \"\", \"key\": \"current\", \"expression\": \"value * 1\"}\n" // i18n: ignore
               "    }\n" // i18n: ignore
               "  ],\n" // i18n: ignore
               "  \"removeWidgetIds\": [],\n" // i18n: ignore
               "  \"frameTemplates\": {},\n" // i18n: ignore
               "  \"points\": [\n" // i18n: ignore
               "    {\"name\": \"总电流\", \"binding\": {\"parseMode\": \"键值匹配\", \"key\": \"current\"}}\n" // i18n: ignore
               "  ],\n" // i18n: ignore
               "  \"alarms\": [\n" // i18n: ignore
               "    {\"id\": \"过流报警\", \"condition\": \"T[\\\"总电流\\\"] > 1500\", \"level\": 3, \"ackRequired\": true, \"text\": \"总电流超过 1500A\"}\n" // i18n: ignore
               "  ]\n" // i18n: ignore
               "}\n" // i18n: ignore
               "```\n" // i18n: ignore
               "4. 修改已有控件时，在 `upsertWidgets` 中指定该控件的 `id`，只填写需要修改的 `properties`、`geometry` 或 `binding` 字段即可（系统会自动与现有属性合并）。新增控件时必须指定 `type`（来自下方控件目录）与合理的 `geometry`（避免与已有控件重叠，坐标对齐 10px 网格）。将控件放入容器（GroupBox/CardContainer/TabWidget）可附加 `\"parentId\": \"容器id\"`。\n" // i18n: ignore
               "5. 绑定字段 `binding.parseMode` 严格使用以下枚举值：`\"键值匹配\"`（配 `key`）、`\"JSON路径\"`（配 `jsonPath`）、`\"HEX字节提取\"`（配 `byteOffset`、`dataType`、`byteOrder`、`frameHeader`、`frameTail`）、`\"Modbus\"`（配 `mbArea`、`regIndex`、`mbDataType`、`mbRegOrder`、`mbBit`、`mbPeriod`）、`\"S7\"`（配 `s7` 地址如 `DB1.DBD0`、`s7DataType` 如 `REAL`）、`\"工程数据点\"`（配 `key` 指向工程数据点 `name`）、`\"CAN信号\"`（配 `canId`、`canStartBit`、`canBitLength`、`canByteOrder` 取 `Intel`/`Motorola MSB`/`Motorola LSB`、`canSigned`、`canFactor`、`canOffset`）、`\"CAN 信号名称\"`（配 `key` 如 `Msg.Sig`）。单点绑定换算公式 `binding.expression` 中用 `value` 表示原始值（例如 `\"value * 0.1\"`）。\n" // i18n: ignore
               "6. `RealtimeChart` 多条曲线在 `properties` 中配置：`\"curveCount\": 2`、`\"_curve_0_name\": \"电压\"`、`\"_curve_0_parseMode\": \"键值匹配\"`、`\"_curve_0_key\": \"voltage\"`、`\"_curve_1_name\": \"电流\"`、`\"_curve_1_parseMode\": \"键值匹配\"`、`\"_curve_1_key\": \"current\"`。\n" // i18n: ignore
               "7. 工程数据点 `points` 每项格式：设备采集点为 `{\"name\": \"点名\", \"binding\": {\"parseMode\": \"键值匹配\", \"key\": \"字段名\"}}`；计算点为 `{\"name\": \"点名\", \"expression\": \"T[\\\"其他点名\\\"] * 2\"}`；工程参数为 `{\"name\": \"参数名\", \"initialValue\": 100}`。报警 `alarms` 每项格式：`{\"id\": \"报警名称\", \"condition\": \"T[\\\"点名\\\"] > 1500\", \"level\": 3, \"ackRequired\": true, \"text\": \"报警文本\"}`（`level` 取整数 1、2 或 3，3 为最高紧急级别；条件表达式必须用 `T[\"点名\"]` 引用工程数据点）。\n" // i18n: ignore
               "\n" // i18n: ignore
               "## 可用控件目录 (type, 名称, 默认尺寸, 绑定属性: 属性列表)\n" // i18n: ignore
               "%1\n" // i18n: ignore
               "\n" // i18n: ignore
               "## 当前选中控件 ID\n" // i18n: ignore
               "%2\n" // i18n: ignore
               "\n" // i18n: ignore
               "## 当前工程 JSON\n" // i18n: ignore
               "%3\n") // i18n: ignore
        .arg(widgetSummaries.join(QLatin1Char('\n')), selectedText, projectJson);
}

bool AiService::applyPatchObject(const Project &currentProject, const QJsonObject &patch, Project *outProject,
                                 QStringList *summaryLines, QStringList *touchedWidgetIds, QString *error)
{
    QJsonObject doc = currentProject.toJson();
    QStringList summaries;
    QStringList touched;

    // If the patch itself is a full UIDP document with "widgets" array and no "upsertWidgets"
    if (patch.contains(QStringLiteral("widgets")) && patch.value(QStringLiteral("widgets")).isArray() &&
        !patch.contains(QStringLiteral("upsertWidgets"))) {
        doc.insert(QStringLiteral("widgets"), patch.value(QStringLiteral("widgets")).toArray());
        summaries.append(tr("更新全部画布控件 (%1 个)").arg(patch.value(QStringLiteral("widgets")).toArray().size()));
    }

    if (patch.value(QStringLiteral("canvas")).isObject()) {
        const QJsonObject canvasPatch = patch.value(QStringLiteral("canvas")).toObject();
        QJsonObject canvas = doc.value(QStringLiteral("canvas")).toObject();
        canvas = mergeJsonObjects(canvas, canvasPatch);
        doc.insert(QStringLiteral("canvas"), canvas);
        summaries.append(tr("调整画布尺寸为 %1 × %2")
                             .arg(canvas.value(QStringLiteral("width")).toInt(currentProject.canvasWidth))
                             .arg(canvas.value(QStringLiteral("height")).toInt(currentProject.canvasHeight)));
    }

    QJsonArray widgets = doc.value(QStringLiteral("widgets")).toArray();

    if (patch.value(QStringLiteral("removeWidgetIds")).isArray()) {
        for (const QJsonValue &idVal : patch.value(QStringLiteral("removeWidgetIds")).toArray()) {
            const QString id = idVal.toString().trimmed();
            if (!id.isEmpty() && removeWidgetFromTree(widgets, id)) {
                summaries.append(tr("删除控件：%1").arg(id));
            }
        }
    }

    QSet<QString> usedIds;
    collectWidgetIds(widgets, &usedIds);

    if (patch.value(QStringLiteral("upsertWidgets")).isArray()) {
        const QJsonArray upserts = patch.value(QStringLiteral("upsertWidgets")).toArray();
        int addedIndex = int(widgets.size());
        for (const QJsonValue &val : upserts) {
            if (!val.isObject()) {
                continue;
            }
            const QJsonObject item = val.toObject();
            const QString id = item.value(QStringLiteral("id")).toString().trimmed();
            QString matchedType;
            if (!id.isEmpty() && updateExistingWidgetInTree(widgets, id, item, &matchedType)) {
                const WidgetMeta *meta = WidgetRegistry::instance().find(matchedType);
                const QString display = meta ? translated(meta->displayName) : matchedType;
                summaries.append(tr("修改控件：%1 (%2)").arg(display, id));
                touched.append(id);
                continue;
            }
            QString itemError;
            const QJsonObject created = normalizeNewWidgetTree(item, &usedIds, addedIndex++, &itemError);
            if (created.isEmpty()) {
                if (error) {
                    *error = itemError;
                }
                return false;
            }
            const QString createdId = created.value(QStringLiteral("id")).toString();
            const QString createdType = created.value(QStringLiteral("type")).toString();
            const WidgetMeta *meta = WidgetRegistry::instance().find(createdType);
            const QString display = meta ? translated(meta->displayName) : createdType;
            const QString parentId = item.value(QStringLiteral("parentId")).toString().trimmed();
            if (!parentId.isEmpty() && appendChildToContainer(widgets, parentId, created)) {
                summaries.append(tr("新增子控件：%1 (%2 → %3)").arg(display, createdId, parentId));
            } else {
                widgets.append(created);
                summaries.append(tr("新增控件：%1 (%2)").arg(display, createdId));
            }
            touched.append(createdId);
        }
    }
    doc.insert(QStringLiteral("widgets"), widgets);

    if (patch.value(QStringLiteral("frameTemplates")).isObject()) {
        QJsonObject templates = doc.value(QStringLiteral("frameTemplates")).toObject();
        const QJsonObject patchTemplates = patch.value(QStringLiteral("frameTemplates")).toObject();
        for (auto it = patchTemplates.begin(); it != patchTemplates.end(); ++it) {
            if (it.value().isNull()) {
                templates.remove(it.key());
                summaries.append(tr("删除帧模板：%1").arg(it.key()));
            } else if (it.value().isObject()) {
                templates.insert(it.key(), it.value());
                const QString name = it.value().toObject().value(QStringLiteral("name")).toString(it.key());
                summaries.append(tr("配置帧模板：%1 (%2)").arg(name, it.key()));
            }
        }
        if (templates.isEmpty()) {
            doc.remove(QStringLiteral("frameTemplates"));
        } else {
            doc.insert(QStringLiteral("frameTemplates"), templates);
        }
    }

    QJsonObject logic = doc.value(QStringLiteral("logic")).toObject();
    const QJsonArray patchPoints = patch.value(QStringLiteral("points")).isArray()
                                       ? patch.value(QStringLiteral("points")).toArray()
                                       : patch.value(QStringLiteral("logic")).toObject().value(QStringLiteral("points")).toArray();
    if (!patchPoints.isEmpty()) {
        QJsonArray points = logic.value(QStringLiteral("points")).toArray();
        for (const QJsonValue &ptVal : patchPoints) {
            if (!ptVal.isObject()) {
                continue;
            }
            const QJsonObject pt = normalizePointObject(ptVal.toObject());
            const QString name = pt.value(QStringLiteral("name")).toString().trimmed();
            if (name.isEmpty()) {
                continue;
            }
            bool updated = false;
            for (qsizetype i = 0; i < points.size(); ++i) {
                if (points.at(i).toObject().value(QStringLiteral("name")).toString() == name) {
                    points[i] = normalizePointObject(mergeJsonObjects(points.at(i).toObject(), pt));
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                points.append(pt);
            }
            summaries.append(updated ? tr("更新数据点：%1").arg(name) : tr("新增数据点：%1").arg(name));
        }
        logic.insert(QStringLiteral("points"), points);
    }

    QSet<QString> knownPoints;
    for (const QJsonValue &ptVal : logic.value(QStringLiteral("points")).toArray()) {
        knownPoints.insert(ptVal.toObject().value(QStringLiteral("name")).toString().trimmed());
    }

    const QJsonArray patchAlarms = patch.value(QStringLiteral("alarms")).isArray()
                                       ? patch.value(QStringLiteral("alarms")).toArray()
                                       : patch.value(QStringLiteral("logic")).toObject().value(QStringLiteral("alarms")).toArray();
    if (!patchAlarms.isEmpty()) {
        QJsonArray alarms = logic.value(QStringLiteral("alarms")).toArray();
        for (const QJsonValue &alVal : patchAlarms) {
            if (!alVal.isObject()) {
                continue;
            }
            const QJsonObject al = normalizeAlarmObject(alVal.toObject(), knownPoints);
            const QString id = al.value(QStringLiteral("id")).toString().trimmed();
            if (id.isEmpty()) {
                continue;
            }
            bool updated = false;
            for (qsizetype i = 0; i < alarms.size(); ++i) {
                if (alarms.at(i).toObject().value(QStringLiteral("id")).toString() == id) {
                    alarms[i] = normalizeAlarmObject(mergeJsonObjects(alarms.at(i).toObject(), al), knownPoints);
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                alarms.append(al);
            }
            summaries.append(updated ? tr("更新报警规则：%1").arg(id) : tr("新增报警规则：%1").arg(id));
        }
        logic.insert(QStringLiteral("alarms"), alarms);
    }

    if (!logic.isEmpty()) {
        doc.insert(QStringLiteral("logic"), logic);
    }

    doc.insert(QStringLiteral("version"),
               Project::formatVersion(doc.value(QStringLiteral("widgets")).toArray(),
                                      doc.value(QStringLiteral("logic")).toObject()));

    Project validated;
    QString validationError;
    if (!Project::fromJson(doc, &validated, &validationError)) {
        if (error) {
            *error = validationError;
        }
        return false;
    }
    validated.filePath = currentProject.filePath;
    validated.recentEndpoints = currentProject.recentEndpoints;
    validated.recentTextSamples = currentProject.recentTextSamples;

    if (outProject) {
        *outProject = validated;
    }
    if (summaryLines) {
        *summaryLines = summaries;
    }
    if (touchedWidgetIds) {
        *touchedWidgetIds = touched;
    }
    return true;
}

AiPatchProposal AiService::extractPatch(const QString &responseText, const Project &currentProject)
{
    AiPatchProposal result;
    result.cleanText = responseText.trimmed();

    static const QRegularExpression patchBlockPattern(
        QStringLiteral("```(?:fhmi-patch|uidp-patch)\\s*\\n([\\s\\S]*?)```"),
        QRegularExpression::CaseInsensitiveOption);

    QRegularExpressionMatch match = patchBlockPattern.match(responseText);
    if (!match.hasMatch()) {
        static const QRegularExpression jsonBlockPattern(
            QStringLiteral("```json\\s*\\n([\\s\\S]*?)```"),
            QRegularExpression::CaseInsensitiveOption);
        auto it = jsonBlockPattern.globalMatch(responseText);
        while (it.hasNext()) {
            const QRegularExpressionMatch candidate = it.next();
            const QByteArray raw = candidate.captured(1).trimmed().toUtf8();
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (doc.isObject()) {
                const QJsonObject obj = doc.object();
                if (obj.contains(QStringLiteral("upsertWidgets")) || obj.contains(QStringLiteral("removeWidgetIds")) ||
                    obj.contains(QStringLiteral("frameTemplates")) || obj.contains(QStringLiteral("points")) ||
                    obj.contains(QStringLiteral("alarms"))) {
                    match = candidate;
                }
            }
        }
    }

    if (!match.hasMatch()) {
        return result;
    }

    result.hasPatch = true;
    const QString rawBlock = match.captured(0);
    const QByteArray jsonBytes = match.captured(1).trimmed().toUtf8();
    result.cleanText = responseText.left(match.capturedStart()) + responseText.mid(match.capturedEnd());
    result.cleanText = result.cleanText.trimmed();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.valid = false;
        result.error = tr("AI 生成的变更 JSON 格式有误: %1").arg(parseError.errorString());
        return result;
    }

    const QJsonObject patchObj = doc.object();
    result.title = patchObj.value(QStringLiteral("title")).toString().trimmed();
    if (result.title.isEmpty()) {
        result.title = tr("AI 工程变更建议");
    }

    result.valid = applyPatchObject(currentProject, patchObj, &result.patchedProject, &result.summaryLines,
                                    &result.touchedWidgetIds, &result.error);
    if (result.valid && result.summaryLines.isEmpty()) {
        result.hasPatch = false;
    }
    return result;
}

} // namespace fhmi
