// SPDX-License-Identifier: AGPL-3.0-or-later
#include "single_instance.hpp"

#include <QDir>
#include <QGuiApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QUrl>

#include <unistd.h>

namespace {

/// A message is a few lines per file; anything longer is not a launch.
constexpr qint64 kMaxMessage = 1 << 20;

}  // namespace

SingleInstance::SingleInstance(QString name, QObject* parent)
    : QObject(parent), name_(std::move(name)), server_(new QLocalServer(this)) {
    connect(server_, &QLocalServer::newConnection, this, &SingleInstance::onConnection);
}

SingleInstance::~SingleInstance() = default;

QString SingleInstance::defaultName() {
    QString app = QGuiApplication::desktopFileName();
    if (app.isEmpty()) {
        app = QStringLiteral("leht");
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return QDir(dir.isEmpty() ? QDir::tempPath() : dir)
        .filePath(QStringLiteral("%1-%2.sock").arg(app).arg(::getuid()));
}

bool SingleInstance::sendToRunning(const QString& name, const QStringList& paths, const QByteArray& token,
                                   int timeoutMs) {
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(timeoutMs)) {
        return false;
    }
    QByteArray message;
    for (const QString& path : paths) {
        message += "open " + QUrl::toPercentEncoding(path, "/") + '\n';
    }
    if (!token.isEmpty()) {
        message += "token " + QUrl::toPercentEncoding(QString::fromUtf8(token)) + '\n';
    }
    message += "end\n";
    socket.write(message);
    if (!socket.waitForBytesWritten(timeoutMs)) {
        return false;
    }
    // Taken only once it says so: a Leht hung in a dialog must not swallow
    // the files.
    while (!socket.canReadLine()) {
        if (!socket.waitForReadyRead(timeoutMs)) {
            return false;
        }
    }
    const bool ok = socket.readLine().trimmed() == "ok";
    socket.disconnectFromServer();
    return ok;
}

bool SingleInstance::listen() {
    // Two launches at once must not both listen: one at a time checks for a
    // running Leht and takes the socket, and the other then finds it.
    QLockFile lock(name_ + QStringLiteral(".lock"));
    if (!lock.tryLock(2000)) {
        return false;
    }
    // Asked before listening: listen() with an access option makes the socket
    // elsewhere and renames it into place, over a live one too.
    QLocalSocket probe;
    probe.connectToServer(name_);
    if (probe.waitForConnected(500)) {
        return false;
    }
    // None answers: whatever is there was left by a Leht that crashed.
    QLocalServer::removeServer(name_);
    // Only this user may connect: the socket is 0700, in a 0700 directory.
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    return server_->listen(name_);
}

void SingleInstance::onConnection() {
    while (QLocalSocket* socket = server_->nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { onReadable(socket); });
        onReadable(socket);  // it may have arrived with the connection
    }
}

void SingleInstance::onReadable(QLocalSocket* socket) {
    if (socket->bytesAvailable() > kMaxMessage) {
        socket->abort();
        return;
    }
    // Wait for the whole message: it ends with "end".
    const QByteArray peek = socket->peek(socket->bytesAvailable());
    if (!peek.endsWith("end\n")) {
        return;
    }
    QStringList paths;
    QByteArray token;
    const QList<QByteArray> lines = socket->readAll().split('\n');
    for (const QByteArray& line : lines) {
        if (line.startsWith("open ")) {
            const QString path = QUrl::fromPercentEncoding(line.mid(5));
            if (QDir::isAbsolutePath(path)) {
                paths << path;
            }
        } else if (line.startsWith("token ")) {
            token = QUrl::fromPercentEncoding(line.mid(6)).toUtf8();
        }
    }
    socket->write("ok\n");
    socket->flush();
    emit filesReceived(paths, token);
}
