// Regenerates the parts of the .uidp guide that come from the widget registry: the widget reference in
// docs/uidp-widgets.md and the widget list that ends docs/uidp-format.md. Run it after changing a widget's
// properties; tst_docs fails until both are current.
//
//   uidp_reference docs   rewrite both documents in place
//   uidp_reference        print the reference
#include "core/widget_reference.h"

#include <QtCore/QFile>
#include <QtCore/QTextStream>

namespace {

// Replaces what follows the marker line in `path` with `generated`; false when that fails.
bool update(const QString &path, const QString &generated)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QTextStream(stderr) << "cannot read " << path << ": " << file.errorString() << '\n';
        return false;
    }
    const QString document = QString::fromUtf8(file.readAll()).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    file.close();
    const QString updated = fhmi::withGenerated(document, generated);
    if (updated.isEmpty()) {
        QTextStream(stderr) << path << " has no line\n" << fhmi::GeneratedMarker << '\n';
        return false;
    }
    if (updated == document) {
        QTextStream(stdout) << path << " is current\n";
        return true;
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream(stderr) << "cannot write " << path << ": " << file.errorString() << '\n';
        return false;
    }
    file.write(updated.toUtf8());
    QTextStream(stdout) << "updated " << path << '\n';
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2) {
        QTextStream(stdout) << fhmi::widgetReference();
        return 0;
    }
    const QString docs = QString::fromLocal8Bit(argv[1]);
    bool ok = update(docs + QStringLiteral("/uidp-format.md"), fhmi::widgetIndex());
    ok = update(docs + QStringLiteral("/uidp-widgets.md"), fhmi::widgetReference()) && ok;
    return ok ? 0 : 1;
}
