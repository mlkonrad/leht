// SPDX-License-Identifier: AGPL-3.0-or-later
#include "worker_files.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

QString workerPath() {
    const QString env = qEnvironmentVariable("LEHT_WORKER_PATH");
    if (!env.isEmpty()) {
        return env;
    }
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString& candidate :
         {appDir + QStringLiteral("/leht-worker"),
          QDir::cleanPath(appDir + QStringLiteral("/" LEHT_WORKER_RELATIVE_PATH)),
          QStringLiteral(LEHT_WORKER_INSTALLED_PATH),
          QStringLiteral(LEHT_WORKER_BUILD_PATH)}) {
        if (QFileInfo(candidate).isExecutable()) {
            return candidate;
        }
    }
    return QStringLiteral(LEHT_WORKER_BUILD_PATH);
}

std::chrono::milliseconds requestTimeout() {
    bool ok = false;
    const int ms = qEnvironmentVariableIntValue("LEHT_WORKER_TIMEOUT_MS", &ok);
    return std::chrono::milliseconds(ok && ms > 0 ? ms : 30'000);
}

mode_t currentUmask() {
    QFile status(QStringLiteral("/proc/self/status"));
    if (status.open(QIODevice::ReadOnly)) {
        for (const QByteArray& line : status.readAll().split('\n')) {
            if (line.startsWith("Umask:")) {
                bool ok = false;
                const uint mask = line.mid(6).trimmed().toUInt(&ok, 8);
                if (ok) {
                    return static_cast<mode_t>(mask);
                }
            }
        }
    }
    return 022;
}

Beside::Beside(const QString& target) : info_(target) {
    temp_ = QFile::encodeName(info_.absolutePath() + QStringLiteral("/.") + info_.fileName() +
                              QStringLiteral(".leht-XXXXXX"));
    fd_ = ::mkostemp(temp_.data(), O_CLOEXEC);
    if (fd_ < 0) {
        error_ = QString::fromUtf8(std::strerror(errno));
    }
}

Beside::~Beside() {
    if (fd_ >= 0) {
        ::close(fd_);
        ::unlink(temp_.constData());
    }
}

QString Beside::commit() {
    struct stat st {};
    const QByteArray target = QFile::encodeName(info_.absoluteFilePath());
    const mode_t mode = ::stat(target.constData(), &st) == 0 ? (st.st_mode & 07777)
                                                             : (0666 & ~currentUmask());
    if (::fchmod(fd_, mode) != 0 || ::fsync(fd_) != 0) {
        return QString::fromUtf8(std::strerror(errno));
    }
    ::close(fd_);
    fd_ = -1;
    if (::rename(temp_.constData(), target.constData()) != 0) {
        const int err = errno;
        ::unlink(temp_.constData());
        return QString::fromUtf8(std::strerror(err));
    }
    const int dir = ::open(QFile::encodeName(info_.absolutePath()).constData(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) {
        (void)::fsync(dir);
        ::close(dir);
    }
    return {};
}
