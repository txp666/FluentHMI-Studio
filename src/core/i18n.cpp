#include "core/i18n.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>

namespace fhmi {

namespace {

QString g_language = kLanguageChinese;

} // namespace

TableTranslator::TableTranslator(QObject *parent) : QTranslator(parent) {}

bool TableTranslator::loadTable(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return false;
    }
    m_table.clear();
    const QJsonObject table = document.object();
    for (auto it = table.begin(); it != table.end(); ++it) {
        const QString english = it.value().toString();
        if (!english.isEmpty()) {
            m_table.insert(it.key(), english);
        }
    }
    return true;
}

QString TableTranslator::translate(const char *, const char *sourceText, const char *, int) const
{
    if (!sourceText || m_table.isEmpty()) {
        return QString();
    }
    return m_table.value(QString::fromUtf8(sourceText));
}

bool TableTranslator::isEmpty() const { return m_table.isEmpty(); }

QString resolveLanguage(const QString &setting)
{
    if (setting == kLanguageChinese || setting == kLanguageEnglish) {
        return setting;
    }
    // The first preferred UI language, not the format locale: inside a macOS bundle the latter only
    // reports the languages the bundle declares.
    const QStringList preferred = QLocale::system().uiLanguages();
    const bool chinese = preferred.isEmpty() ? QLocale::system().language() == QLocale::Chinese
                                             : QLocale(preferred.first()).language() == QLocale::Chinese;
    return chinese ? kLanguageChinese : kLanguageEnglish;
}

QString applyLanguage(QCoreApplication *app, const QString &setting)
{
    g_language = resolveLanguage(setting);
    if (g_language == kLanguageEnglish && app) {
        auto *translator = new TableTranslator(app);
        if (translator->loadTable(QString::fromLatin1(kEnglishTable))) {
            app->installTranslator(translator);
        }
    }
    return g_language;
}

QString language() { return g_language; }

QString translated(const char *source) { return QCoreApplication::translate("fhmi", source); }

QString translated(const QString &source)
{
    return source.isEmpty() ? source : QCoreApplication::translate("fhmi", source.toUtf8().constData());
}

} // namespace fhmi
