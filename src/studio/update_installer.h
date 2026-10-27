#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

class QProcess;

namespace fhmi {

// Replaces this installation with a downloaded package.
//
// A running program cannot replace its own files on Windows, so the swap is left to a small
// helper (a shell script on macOS, PowerShell on Windows) that waits for the studio to exit,
// moves the old installation aside, moves the new one into its place, optionally starts it
// and removes the old one. If the second move fails the old installation is moved back.
// Everything happens in a staging folder beside the installation, so each move is a rename
// on one volume.
class UpdateInstaller : public QObject
{
    Q_OBJECT

  public:
    explicit UpdateInstaller(QObject *parent = nullptr);
    ~UpdateInstaller() override;

    // What an update replaces: FluentHMI Studio.app on macOS, the folder holding
    // FluentHMIStudio.exe on Windows. Empty when this is not a packaged installation
    // (a build tree, or a platform without packages).
    static QString installRoot();
    // Beside installRoot(); downloads and the unpacked package go here. It exists only
    // while an update is under way or waiting to be installed.
    static QString stagingDir();
    // Whether this installation can replace itself; `reason` says why not. Leaves nothing
    // behind.
    static bool canInstall(QString *reason);
    // What the helper left after an update that did not complete, or empty. Removes the
    // staging folder it reads it from.
    static QString takeFailureReport();

    // Unpacks the package; answers with prepared() or failed().
    void prepare(const QString &package);
    bool isPrepared() const { return !m_newRoot.isEmpty(); }
    void cancel();
    // cancel(), and removes the staging folder with whatever was downloaded or unpacked.
    void discard();
    // Starts the helper. The caller must then exit; the swap waits for this process to end.
    bool launch(bool relaunch, QString *error);

  signals:
    void prepared();
    void failed(const QString &message);

  private:
    QString findNewRoot(const QString &dir) const;

    QProcess *m_unpack = nullptr;
    QString m_newRoot;
};

} // namespace fhmi
