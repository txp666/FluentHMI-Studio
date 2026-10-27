// Regenerates the widget reference at the end of docs/uidp-format.md from the widget registry. Run it
// after changing a widget's properties; tst_docs fails until the document is current.
//
//   uidp_reference docs/uidp-format.md   rewrite the document in place
//   uidp_reference                       print the reference
#include "core/widget_reference.h"

#include <QtCore/QFile>
#include <QtCore/QTextStream>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        QTextStream(stdout) << fhmi::widgetReference();
        return 0;
    }
    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::ReadOnly)) {
        QTextStream(stderr) << "cannot read " << file.fileName() << ": " << file.errorString() << '\n';
        return 1;
    }
    const QString document = QString::fromUtf8(file.readAll()).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    file.close();
    const QString updated = fhmi::withWidgetReference(document);
    if (updated.isEmpty()) {
        QTextStream(stderr) << file.fileName() << " has no line\n" << fhmi::WidgetReferenceMarker << '\n';
        return 1;
    }
    if (updated == document) {
        QTextStream(stdout) << file.fileName() << " is current\n";
        return 0;
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream(stderr) << "cannot write " << file.fileName() << ": " << file.errorString() << '\n';
        return 1;
    }
    file.write(updated.toUtf8());
    QTextStream(stdout) << "updated " << file.fileName() << '\n';
    return 0;
}
