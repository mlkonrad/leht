// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <optional>

namespace leht::ipc {
struct Frame;
class WorkerProcess;
}  // namespace leht::ipc

/// Combine Files, Reduce File Size and Split Document: whole-file jobs that
/// write new files and never change the one on screen.
///
/// Each job runs on this object's own thread, in a sandboxed leht-worker of
/// its own -- never the one showing the document. Compression rewrites the
/// document it works on, a merge reads files the user has not opened, and
/// either way a hostile input should cost that job, not the open document.
/// Jobs work on files as they are on disk: unsaved edits are not included.
///
/// Every output is written into a temporary file beside its target and
/// renamed into place only when the job succeeds, so a failed or cancelled
/// job leaves nothing half-written. Each job ends in exactly one of
/// finished(), failed() or passwordRequired().
class FileTools : public QObject {
    Q_OBJECT

public:
    FileTools();
    ~FileTools() override;

    /// Stops the running job after its current step. Any thread.
    void cancel() { cancel_ = true; }

public slots:
    /// Merges `inputs` (PDFs and images, in order) into `output`.
    void combine(QStringList inputs, QString output, bool linearize);

    /// Compresses `input` into `output` with preset `preset` (an
    /// ops::CompressPreset) and JPEG `quality` (0: the preset's). Nothing is
    /// written if the result would not be smaller than the input.
    void compress(QString input, QString password, QString output, int preset, int quality,
                  bool linearize);

    /// What each Reduce File Size preset would make of `input`, without
    /// writing anything: the presets are run in turn into memory, the one the
    /// dialog starts on first, and each result is reported by estimated().
    /// Quiet: it never emits finished(), failed() or passwordRequired(). A
    /// password-protected file (without `password`) gets no estimate.
    void estimate(QString input, QString password);

    /// Writes the pages `ranges[i]` of `input` to `outputs[i]`, one file each.
    void split(QString input, QString password, QStringList ranges, QStringList outputs);

    /// Writes `input` to `output` with a password (`lock`) or without one.
    /// `method` is an ops::Encryption, `permissions` ipc::Protect's bits.
    /// `output` may be `input`: the new file replaces it only when complete.
    void protect(QString input, QString password, QString output, bool lock, QString userPassword,
                 QString ownerPassword, int method, int permissions);

signals:
    /// `done` of `total` steps; `what` says which, for display.
    void progress(int done, int total, QString what);
    /// `summary` is for display; `written` lists the files made.
    void finished(QString summary, QStringList written);
    void failed(QString message);
    /// `input` is encrypted: ask for its password and run the job again with
    /// it. `wrong` when the one given did not unlock it.
    void passwordRequired(bool wrong);
    /// From estimate(): preset `preset` would write `bytes` (-1: no estimate).
    void estimated(int preset, qint64 bytes);

private:
    /// A fresh, handshaken worker; nullptr after emitting failed().
    std::unique_ptr<leht::ipc::WorkerProcess> startWorker();

    /// Opens `input` in `w`, unlocking it with `password`. False after
    /// emitting failed() or passwordRequired().
    bool openInput(leht::ipc::WorkerProcess& w, const QString& input, const QString& password);

    /// Sends `msg` (with `fd`, if any) and returns the reply. On a Failed reply,
    /// a dead worker or garbage, sets `error` and returns nullopt.
    template <typename Msg>
    std::optional<leht::ipc::Frame> request(leht::ipc::WorkerProcess& w, const Msg& msg, int fd,
                                            QString& error);

    /// Emits failed() for a job stopped by cancel(), and clears the flag.
    bool cancelled();

    std::atomic<bool> cancel_{false};
    std::uint64_t nextId_ = 1;
};
