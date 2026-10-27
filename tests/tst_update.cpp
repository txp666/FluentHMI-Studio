#include "studio/update_release.h"
#include "studio/update_service.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QtTest>

using namespace fhmi;

namespace {

const QString suffix = QStringLiteral("-windows-x64.zip");

QByteArray releaseJson(const QString &tag, const QString &base = QStringLiteral("https://example.com/"))
{
    return QStringLiteral(R"({
        "tag_name": "%1",
        "html_url": "https://github.com/txp666/FluentHMI-Studio/releases/tag/%1",
        "body": "- 安装包只带一份 Qt",
        "assets": [
            {"name": "FluentHMI-Studio-%1-macos-universal.zip", "size": 300,
             "browser_download_url": "%2FluentHMI-Studio-%1-macos-universal.zip"},
            {"name": "FluentHMI-Studio-%1-windows-x64.zip.sha256", "size": 106,
             "browser_download_url": "%2FluentHMI-Studio-%1-windows-x64.zip.sha256"},
            {"name": "FluentHMI-Studio-%1-windows-x64.zip", "size": 200,
             "browser_download_url": "%2FluentHMI-Studio-%1-windows-x64.zip"}
        ]})")
        .arg(tag, base)
        .toUtf8();
}

// Serves fixed bodies by path over plain HTTP/1.1, one request per connection.
class FakeServer : public QObject
{
  public:
    FakeServer()
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                const QByteArray request = socket->readAll();
                const QByteArray path = request.split(' ').value(1);
                requests.append(QString::fromLatin1(path));
                const bool known = bodies.contains(path);
                const QByteArray body = known ? bodies.value(path) : QByteArray("not found");
                socket->write((known ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n") +
                              QByteArray("Content-Length: ") + QByteArray::number(body.size()) +
                              "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()); }

    QTcpServer server;
    QHash<QByteArray, QByteArray> bodies;
    QStringList requests;
};

} // namespace

class TestUpdate : public QObject
{
    Q_OBJECT

  private slots:
    void parsesTheLatestRelease()
    {
        ReleaseInfo release;
        QString error;
        QVERIFY2(UpdateRelease::parse(releaseJson(QStringLiteral("v0.1.2")), suffix, &release, &error),
                 qPrintable(error));
        QCOMPARE(release.tag, QStringLiteral("v0.1.2"));
        QCOMPARE(release.version, QVersionNumber(0, 1, 2));
        QCOMPARE(release.notes, QStringLiteral("- 安装包只带一份 Qt"));
        QCOMPARE(release.page.toString(),
                 QStringLiteral("https://github.com/txp666/FluentHMI-Studio/releases/tag/v0.1.2"));
        QCOMPARE(release.package.name, QStringLiteral("FluentHMI-Studio-v0.1.2-windows-x64.zip"));
        QCOMPARE(release.package.size, 200);
        QCOMPARE(release.checksum.name, QStringLiteral("FluentHMI-Studio-v0.1.2-windows-x64.zip.sha256"));
        QVERIFY(release.hasPackage());
    }

    void releaseWithoutThisPlatformsPackage()
    {
        ReleaseInfo release;
        QVERIFY(UpdateRelease::parse(releaseJson(QStringLiteral("v0.1.2")), QStringLiteral("-linux-x64.zip"), &release,
                                     nullptr));
        QVERIFY(!release.hasPackage());
        QVERIFY(UpdateRelease::parse(releaseJson(QStringLiteral("v0.1.2")), QString(), &release, nullptr));
        QVERIFY(!release.hasPackage());
    }

    void packageWithoutChecksumIsNotInstallable()
    {
        // The macOS package in the sample has no .sha256 beside it.
        ReleaseInfo release;
        QVERIFY(UpdateRelease::parse(releaseJson(QStringLiteral("v0.1.2")), QStringLiteral("-macos-universal.zip"),
                                     &release, nullptr));
        QCOMPARE(release.package.name, QStringLiteral("FluentHMI-Studio-v0.1.2-macos-universal.zip"));
        QVERIFY(!release.hasPackage());
    }

    void rejectsWhatIsNotARelease()
    {
        ReleaseInfo release;
        QString error;
        QVERIFY(!UpdateRelease::parse("not json", suffix, &release, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!UpdateRelease::parse(R"({"message": "Not Found"})", suffix, &release, &error));
        QVERIFY(!UpdateRelease::parse(releaseJson(QStringLiteral("nightly")), suffix, &release, &error));
        QVERIFY(!UpdateRelease::parse(releaseJson(QStringLiteral("v0.1.2-beta")), suffix, &release, &error));
    }

    void comparesVersionsNumerically_data()
    {
        QTest::addColumn<QString>("tag");
        QTest::addColumn<QString>("current");
        QTest::addColumn<bool>("newer");
        QTest::newRow("patch") << "v0.1.2" << "0.1.1" << true;
        QTest::newRow("same") << "v0.1.1" << "0.1.1" << false;
        QTest::newRow("older") << "v0.1.0" << "0.1.1" << false;
        QTest::newRow("two digits") << "v0.1.10" << "0.1.9" << true;
        QTest::newRow("minor") << "v0.2" << "0.1.9" << true;
        QTest::newRow("no v") << "1.0.0" << "0.9.9" << true;
    }

    void comparesVersionsNumerically()
    {
        QFETCH(QString, tag);
        QFETCH(QString, current);
        QFETCH(bool, newer);
        ReleaseInfo release;
        QVERIFY(UpdateRelease::parse(releaseJson(tag), suffix, &release, nullptr));
        QCOMPARE(UpdateRelease::isNewer(release, current), newer);
    }

    void readsChecksumFiles()
    {
        const QByteArray hex = QByteArray(64, 'a');
        const QString name = QStringLiteral("FluentHMI-Studio-v0.1.2-windows-x64.zip");
        QCOMPARE(UpdateRelease::checksumFor(hex + "  " + name.toUtf8() + "\n", name), hex);
        // CRLF, the binary marker and upper-case digits are all accepted.
        QCOMPARE(UpdateRelease::checksumFor(hex.toUpper() + " *" + name.toUtf8() + "\r\n", name), hex);
        QVERIFY(UpdateRelease::checksumFor(hex + "  other.zip\n", name).isEmpty());
        QVERIFY(UpdateRelease::checksumFor("abc  " + name.toUtf8(), name).isEmpty());
    }

    void checksAndDownloadsAVerifiedPackage()
    {
        FakeServer server;
        const QByteArray package = QByteArray("PK fake package ").repeated(1000);
        const QByteArray digest = QCryptographicHash::hash(package, QCryptographicHash::Sha256).toHex();
        const QByteArray name = "FluentHMI-Studio-v9.0.0" + suffix.toUtf8();
        QByteArray json = releaseJson(QStringLiteral("v9.0.0"), server.base());
        json.replace("\"size\": 200", "\"size\": " + QByteArray::number(package.size()));
        server.bodies.insert("/latest", json);
        server.bodies.insert("/" + name, package);
        server.bodies.insert("/" + name + ".sha256", digest + "  " + name + "\n");

        UpdateService service;
        service.setReleaseUrl(QUrl(server.base() + QStringLiteral("latest")));
        service.setPackageSuffix(suffix);
        QSignalSpy checked(&service, &UpdateService::checked);
        QSignalSpy failed(&service, &UpdateService::failed);
        service.check();
        QVERIFY(checked.wait(5000));
        QCOMPARE(failed.count(), 0);
        const ReleaseInfo release = checked.first().first().value<ReleaseInfo>();
        QCOMPARE(release.tag, QStringLiteral("v9.0.0"));
        QVERIFY(release.hasPackage());

        QTemporaryDir dir;
        QSignalSpy downloaded(&service, &UpdateService::downloaded);
        QSignalSpy progress(&service, &UpdateService::progress);
        service.download(release, dir.path());
        QVERIFY(downloaded.wait(5000));
        QCOMPARE(failed.count(), 0);
        QVERIFY(progress.count() > 0);
        QFile file(downloaded.first().first().toString());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), package);

        // A package that does not match its checksum is refused and not left behind.
        server.bodies.insert("/" + name, QByteArray(package).replace("fake", "evil"));
        file.remove();
        service.download(release, dir.path());
        QVERIFY(failed.wait(5000));
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("校验")));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList());
    }

    void reportsAMissingRelease()
    {
        FakeServer server;
        UpdateService service;
        service.setReleaseUrl(QUrl(server.base() + QStringLiteral("latest")));
        QSignalSpy failed(&service, &UpdateService::failed);
        service.check();
        QVERIFY(failed.wait(5000));
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("没有找到")));
    }
};

QTEST_GUILESS_MAIN(TestUpdate)
#include "tst_update.moc"
