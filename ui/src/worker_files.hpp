// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// What every part of the viewer that drives a leht-worker needs: where the
// worker is, how long it may take, and how a file it writes into becomes the
// user's file. The worker cannot open paths; this side opens them and passes
// descriptors, and makes the result durable and atomic.

#include <QByteArray>
#include <QFileInfo>
#include <QString>

#include <sys/types.h>

#include <chrono>

/// Where leht-worker lives, in order of preference: an explicit override, next
/// to the running binary (an app bundle), libexec relative to the binary (a
/// relocated or DESTDIR install), the configured install location, and the
/// build tree (tests and running from the build directory).
QString workerPath();

/// How long the worker may take to produce each reply before it is killed and
/// the file blamed. Generous: a legitimate open sizes every page, and 30 s
/// covers documents far beyond any real one. LEHT_WORKER_TIMEOUT_MS overrides
/// it (the tests use a short one).
std::chrono::milliseconds requestTimeout();

/// The process umask, read without changing it (umask(2) can only be read by
/// setting it, which would race with other threads creating files).
mode_t currentUmask();

/// A new file beside `target`, for the worker to write into; commit() makes it
/// the target, atomically and durably. Discarded unless committed.
class Beside {
public:
    explicit Beside(const QString& target);
    Beside(const Beside&) = delete;
    Beside& operator=(const Beside&) = delete;
    ~Beside();

    [[nodiscard]] int fd() const { return fd_; }
    [[nodiscard]] const QString& error() const { return error_; }
    [[nodiscard]] QString path() const { return info_.absoluteFilePath(); }

    /// Returns empty on success, why not otherwise.
    QString commit();

private:
    QFileInfo info_;
    QByteArray temp_;
    int fd_ = -1;
    QString error_;
};
