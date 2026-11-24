#include "studio/update_installer.h"
#include "studio/update_release.h"
#include "studio/update_service.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QProcess>
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

    void windowsSwapScriptPreservesUserFilesAndDrivers()
    {
        const QByteArray script = UpdateInstaller::windowsSwapScript();
        QVERIFY(script.contains("package-files.txt"));
        QVERIFY(script.contains("runtime-files.txt"));
        QVERIFY(script.contains("ReadAllLines"));
        QVERIFY(script.contains("-LiteralPath"));
        QVERIFY(script.contains("FluentHMI-Studio-backup-"));
        QVERIFY(script.contains("restore-notice.txt"));

#if defined(Q_OS_WIN)
        const auto writeFile = [](const QString &path, const QByteArray &data) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(path));
            QCOMPARE(file.write(data), data.size());
        };
        const auto readFile = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const auto runSwap = [&](const QString &root, const QString &next, const QString &staging) {
            const QString ps1 = QDir(staging).filePath(QStringLiteral("swap.ps1"));
            writeFile(ps1, script);
            QProcess ps;
            ps.setProgram(QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
                              .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe")));
            ps.setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                             QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"),
                             QDir::toNativeSeparators(ps1), QStringLiteral("-ProcessId"), QStringLiteral("0"),
                             QStringLiteral("-Root"), QDir::toNativeSeparators(root), QStringLiteral("-New"),
                             QDir::toNativeSeparators(next), QStringLiteral("-Backup"),
                             QDir::toNativeSeparators(QDir(staging).filePath(QStringLiteral("old"))),
                             QStringLiteral("-Staging"), QDir::toNativeSeparators(staging),
                             QStringLiteral("-Relaunch"), QStringLiteral("0")});
            ps.start();
            QVERIFY2(ps.waitForFinished(30000), qPrintable(ps.errorString()));
            QCOMPARE(ps.exitStatus(), QProcess::NormalExit);
            QCOMPARE(ps.exitCode(), 0);
        };

        // 1. Standard upgrade with package-files.txt: updates app files, removes obsolete packaged files,
        //    and preserves user projects (including Chinese names and brackets), CAN drivers, and folders.
        {
            QTemporaryDir work;
            const QString root = work.filePath(QStringLiteral("FluentHMI-Studio"));
            const QString staging = work.filePath(QStringLiteral(".fluenthmi-update"));
            const QString next = QDir(staging).filePath(QStringLiteral("new/FluentHMI-Studio"));

            writeFile(root + QStringLiteral("/FluentHMIStudio.exe"), "old-studio");
            writeFile(root + QStringLiteral("/old-only.dll"), "obsolete");
            writeFile(root + QStringLiteral("/tutorials/01-温湿度采集.uidp"), "old-tutorial");
            writeFile(root + QStringLiteral("/package-files.txt"),
                      QStringLiteral("FluentHMIStudio.exe\nold-only.dll\ntutorials/01-温湿度采集.uidp\npackage-files.txt\n")
                          .toUtf8());
            // User files and CAN drivers placed in the folder:
            writeFile(root + QStringLiteral("/my_project.uidp"), "user-project");
            writeFile(root + QStringLiteral("/中文工程[1].uidp"), "user-cn");
            writeFile(root + QStringLiteral("/zlgcan.dll"), "zlg-dll");
            writeFile(root + QStringLiteral("/kerneldlls/dll_cfg.ini"), "zlg-cfg");
            writeFile(root + QStringLiteral("/kerneldlls/USBCANFD.dll"), "zlg-kernel");
            writeFile(root + QStringLiteral("/zlgcan/zlgcan.dll"), "zlg-sub");
            writeFile(root + QStringLiteral("/tutorials/user_demo.uidp"), "user-in-tutorials");
            QVERIFY(QDir().mkpath(root + QStringLiteral("/recordings/empty")));

            writeFile(next + QStringLiteral("/FluentHMIStudio.exe"), "new-studio");
            writeFile(next + QStringLiteral("/new-only.dll"), "fresh");
            writeFile(next + QStringLiteral("/tutorials/01-温湿度采集.uidp"), "new-tutorial");
            writeFile(next + QStringLiteral("/package-files.txt"),
                      QStringLiteral("FluentHMIStudio.exe\nnew-only.dll\ntutorials/01-温湿度采集.uidp\npackage-files.txt\n")
                          .toUtf8());

            runSwap(root, next, staging);

            QCOMPARE(readFile(root + QStringLiteral("/FluentHMIStudio.exe")), QByteArray("new-studio"));
            QCOMPARE(readFile(root + QStringLiteral("/new-only.dll")), QByteArray("fresh"));
            QCOMPARE(readFile(root + QStringLiteral("/tutorials/01-温湿度采集.uidp")), QByteArray("new-tutorial"));
            QVERIFY(!QFileInfo::exists(root + QStringLiteral("/old-only.dll")));
            QCOMPARE(readFile(root + QStringLiteral("/my_project.uidp")), QByteArray("user-project"));
            QCOMPARE(readFile(root + QStringLiteral("/中文工程[1].uidp")), QByteArray("user-cn"));
            QCOMPARE(readFile(root + QStringLiteral("/zlgcan.dll")), QByteArray("zlg-dll"));
            QCOMPARE(readFile(root + QStringLiteral("/kerneldlls/dll_cfg.ini")), QByteArray("zlg-cfg"));
            QCOMPARE(readFile(root + QStringLiteral("/kerneldlls/USBCANFD.dll")), QByteArray("zlg-kernel"));
            QCOMPARE(readFile(root + QStringLiteral("/zlgcan/zlgcan.dll")), QByteArray("zlg-sub"));
            QCOMPARE(readFile(root + QStringLiteral("/tutorials/user_demo.uidp")), QByteArray("user-in-tutorials"));
            QVERIFY(QDir(root + QStringLiteral("/recordings/empty")).exists());
            QVERIFY(!QDir(staging).exists());
        }

        // 2. Fallback upgrade from a legacy install without package-files.txt (only runtime-files.txt).
        {
            QTemporaryDir work;
            const QString root = work.filePath(QStringLiteral("FluentHMI-Studio"));
            const QString staging = work.filePath(QStringLiteral(".fluenthmi-update"));
            const QString next = QDir(staging).filePath(QStringLiteral("new/FluentHMI-Studio"));

            writeFile(root + QStringLiteral("/FluentHMIStudio.exe"), "old-studio");
            writeFile(root + QStringLiteral("/Qt6Core.dll"), "old-qt");
            writeFile(root + QStringLiteral("/runtime-files.txt"), "Qt6Core.dll\n");
            writeFile(root + QStringLiteral("/zlgcan.dll"), "zlg-dll");
            writeFile(root + QStringLiteral("/projects/pump.uidp"), "pump-project");

            writeFile(next + QStringLiteral("/FluentHMIStudio.exe"), "new-studio");
            writeFile(next + QStringLiteral("/Qt6Core.dll"), "new-qt");
            writeFile(next + QStringLiteral("/runtime-files.txt"), "Qt6Core.dll\n");
            writeFile(next + QStringLiteral("/package-files.txt"),
                      "FluentHMIStudio.exe\nQt6Core.dll\nruntime-files.txt\npackage-files.txt\n");

            runSwap(root, next, staging);

            QCOMPARE(readFile(root + QStringLiteral("/FluentHMIStudio.exe")), QByteArray("new-studio"));
            QCOMPARE(readFile(root + QStringLiteral("/Qt6Core.dll")), QByteArray("new-qt"));
            QCOMPARE(readFile(root + QStringLiteral("/zlgcan.dll")), QByteArray("zlg-dll"));
            QCOMPARE(readFile(root + QStringLiteral("/projects/pump.uidp")), QByteArray("pump-project"));
            QVERIFY(!QDir(staging).exists());
        }

        // 3. Collision with a new packaged file: keep new app file intact and save user's colliding file in a
        //    visible backup folder beside Root rather than deleting it.
        {
            QTemporaryDir work;
            const QString root = work.filePath(QStringLiteral("FluentHMI-Studio"));
            const QString staging = work.filePath(QStringLiteral(".fluenthmi-update"));
            const QString next = QDir(staging).filePath(QStringLiteral("new/FluentHMI-Studio"));

            writeFile(root + QStringLiteral("/FluentHMIStudio.exe"), "old-studio");
            writeFile(root + QStringLiteral("/package-files.txt"), "FluentHMIStudio.exe\npackage-files.txt\n");
            writeFile(root + QStringLiteral("/extra.txt"), "user-extra");

            writeFile(next + QStringLiteral("/FluentHMIStudio.exe"), "new-studio");
            writeFile(next + QStringLiteral("/extra.txt"), "packaged-extra");
            writeFile(next + QStringLiteral("/package-files.txt"),
                      "FluentHMIStudio.exe\nextra.txt\npackage-files.txt\n");

            runSwap(root, next, staging);

            QCOMPARE(readFile(root + QStringLiteral("/extra.txt")), QByteArray("packaged-extra"));
            QVERIFY(QFileInfo::exists(QDir(staging).filePath(QStringLiteral("restore-notice.txt"))));
            QVERIFY(!QFileInfo::exists(QDir(staging).filePath(QStringLiteral("update-failed.txt"))));
            const QStringList backups =
                QDir(work.path()).entryList({QStringLiteral("FluentHMI-Studio-backup-*")}, QDir::Dirs);
            QCOMPARE(backups.size(), 1);
            QCOMPARE(readFile(work.filePath(backups.first() + QStringLiteral("/extra.txt"))), QByteArray("user-extra"));
        }
#endif
    }
};

QTEST_GUILESS_MAIN(TestUpdate)
#include "tst_update.moc"
