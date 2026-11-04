#pragma once

#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QTranslator>

class QCoreApplication;

namespace fhmi {

// The source texts are Chinese. English comes from one flat table {"中文原文": "English"}, the same file as
// PyQt UI Designer's, compiled into the studio and the player.
inline constexpr char kEnglishTable[] = ":/fluenthmi/i18n/en.json";

inline const QString kLanguageAuto = QStringLiteral("Auto");
inline const QString kLanguageChinese = QStringLiteral("zh_CN");
inline const QString kLanguageEnglish = QStringLiteral("en_US");

// Translates through the table, whatever the context: the key is the Chinese text alone. A text the table
// lacks stays Chinese.
class TableTranslator : public QTranslator
{
    Q_OBJECT

  public:
    explicit TableTranslator(QObject *parent = nullptr);

    // Reads a JSON object of Chinese keys and English values; false when the file cannot be read or is not one.
    bool loadTable(const QString &path);
    void setTable(const QHash<QString, QString> &table) { m_table = table; }

    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr,
                      int n = -1) const override;
    bool isEmpty() const override;

  private:
    QHash<QString, QString> m_table;
};

// The language a setting ("Auto", "zh_CN", "en_US") stands for: Auto is Simplified Chinese on a Chinese
// system and English anywhere else.
QString resolveLanguage(const QString &setting);

// Resolves the setting, installs the English table on `app` when it is English and remembers the result for
// language(). Returns the resolved language. The studio and the player call installLanguage() (runtime/
// appearance.h), which also installs FluentQtWidgets' own Chinese texts.
QString applyLanguage(QCoreApplication *app, const QString &setting);

// A source text held in a variable, translated: a table entry marked QT_TR_NOOP, a .uidp value shown to the user.
QString translated(const char *source);
QString translated(const QString &source);

// The effective language: Simplified Chinese until applyLanguage() says otherwise.
QString language();

} // namespace fhmi
