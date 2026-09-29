// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QObject>
#include <QStringList>

class QLocalServer;
class QLocalSocket;

/// One Leht per user: a second launch hands its files to the one running,
/// which opens them as tabs, and exits.
///
/// The running Leht listens on a local socket in $XDG_RUNTIME_DIR (the
/// user's own, mode 0700), made readable by the user alone. A launch sends
/// one line per file, `open <path>` (absolute, percent-encoded), then
/// `token <XDG_ACTIVATION_TOKEN>` if it was given one, then `end`; the
/// running Leht answers `ok` once the files are on their way.
class SingleInstance : public QObject {
    Q_OBJECT

public:
    /// `name` is the socket: a path, or a name QLocalServer resolves.
    explicit SingleInstance(QString name = defaultName(), QObject* parent = nullptr);
    ~SingleInstance() override;

    /// $XDG_RUNTIME_DIR/<app id>-<uid>.sock.
    [[nodiscard]] static QString defaultName();

    /// Hands `paths` (absolute) and a Wayland activation token to the Leht
    /// listening on `name`. True once it has taken them; false if none
    /// listens, or it did not answer within `timeoutMs`. Blocks, so the
    /// listener must be in another thread (or process).
    static bool sendToRunning(const QString& name, const QStringList& paths, const QByteArray& token,
                              int timeoutMs = 3000);

    /// Starts listening. False if another Leht listens already; a socket
    /// left behind by one that crashed is removed and listened on. Checking
    /// and listening hold `<name>.lock`, so two launches at once cannot both
    /// listen.
    bool listen();

signals:
    /// Another launch's files, and its activation token (may be empty).
    void filesReceived(const QStringList& paths, const QByteArray& activationToken);

private:
    void onConnection();
    void onReadable(QLocalSocket* socket);

    QString name_;
    QLocalServer* server_ = nullptr;
};
