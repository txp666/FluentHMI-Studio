#include "core/chart_file.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtTest/QtTest>

using namespace fhmi;

Q_DECLARE_METATYPE(fhmi::PyJsonValue)

namespace {

PyJsonValue member(const PyJsonValue &object, const char *name)
{
    for (size_t i = 0; i < object.keys.size(); ++i) {
        if (object.keys[i] == QLatin1String(name)) {
            return object.items[i];
        }
    }
    return {};
}

QStringList texts(const PyJsonValue &array)
{
    QStringList out;
    for (const PyJsonValue &item : array.items) {
        out.append(item.text);
    }
    return out;
}

QList<QList<double>> numbers(const PyJsonValue &array)
{
    QList<QList<double>> out;
    for (const PyJsonValue &curve : array.items) {
        QList<double> samples;
        for (const PyJsonValue &item : curve.items) {
            samples.append(item.number);
        }
        out.append(samples);
    }
    return out;
}

} // namespace

class TestChartFile : public QObject
{
    Q_OBJECT

  private slots:
    // tests/data/chart_file_vectors.json, made by PyQt UI Designer's chart: what it saves for given curves,
    // byte for byte, and what it reads from given files (or refuses). Read with parsePyJson, since
    // QJsonDocument turns the -0.0 a save case writes into 0.
    void goldenVectors_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<PyJsonValue>("vector");
        QFile file(QStringLiteral(FHMI_TEST_DATA_DIR "/chart_file_vectors.json"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        PyJsonValue document;
        QString error;
        QVERIFY2(parsePyJson(QString::fromUtf8(file.readAll()), &document, &error), qPrintable(error));
        for (const char *kind : {"save", "load"}) {
            const PyJsonValue cases = member(document, kind);
            QVERIFY(!cases.items.empty());
            for (const PyJsonValue &vector : cases.items) {
                const QString title = QStringLiteral("%1: %2").arg(QLatin1String(kind), member(vector, "title").text);
                QTest::newRow(title.toUtf8().constData()) << QString::fromLatin1(kind) << vector;
            }
        }
    }

    void goldenVectors()
    {
        QFETCH(QString, kind);
        QFETCH(PyJsonValue, vector);
        if (kind == QLatin1String("save")) {
            const QStringList names = texts(member(vector, "names"));
            const QList<QList<double>> values = numbers(member(vector, "values"));
            for (const auto &[format, key] :
                 {std::pair(ChartFileFormat::Csv, "csv"), std::pair(ChartFileFormat::Json, "json")}) {
                QList<ChartSamples> samples;
                for (const QList<double> &curve : values) {
                    samples.append([curve, next = qsizetype(0)](double *value) mutable {
                        if (next >= curve.size()) {
                            return false;
                        }
                        *value = curve.at(next++);
                        return true;
                    });
                }
                QBuffer buffer;
                buffer.open(QIODevice::WriteOnly);
                QString error;
                QVERIFY2(writeChartFile(&buffer, format, names, samples, &error), qPrintable(error));
                QCOMPARE(QString::fromUtf8(buffer.data()), member(vector, key).text);
            }
            return;
        }
        const ChartFileFormat format =
            member(vector, "format").text == QLatin1String("json") ? ChartFileFormat::Json : ChartFileFormat::Csv;
        QStringList names;
        QList<QList<double>> values;
        QString error;
        const bool loaded = readChartFile(member(vector, "text").text.toUtf8(), format, &names, &values, &error);
        if (member(vector, "error").flag) {
            QVERIFY2(!loaded, qPrintable(names.join(QLatin1Char(','))));
            QVERIFY(!error.isEmpty());
            return;
        }
        QVERIFY2(loaded, qPrintable(error));
        QCOMPARE(names, texts(member(vector, "names")));
        QCOMPARE(values, numbers(member(vector, "values")));
    }
};

QTEST_APPLESS_MAIN(TestChartFile)
#include "tst_chart_file.moc"
